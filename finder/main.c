/*
 * Finder: start up, menus, and Finder's proc, which handles every
 * event from libux, the rescan tick, and uxmeta's change stream.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>
#include "fcore.h"
#include "finder.h"

Fcws	*ws;
Fcmeta	*meta;
View	*views;
View	*desk;
View	*front;
int	nodesk;

static Channel	*evc;	/* Fev* from libux handlers */
static Channel	*tickc;
static Channel	*metac;	/* char* lines from /mnt/meta/changes */
static int	tickms = 1000;
static char	*metaroot = "/mnt/meta";
static int	deskfd = -1;

static void
usage(void)
{
	fprint(2, "usage: finder [-D] [-M] [-m metaroot] [-w workspace] [-i ms] [-v name=path]...\n");
	threadexitsall("usage");
}

static char*
sdup(char *s)
{
	return s != nil ? festrdup(s) : nil;
}

static void
handoff(Fev *f, Uxev *e)
{
	f->type = e->type;
	f->via = e->via;
	f->win = e->win;
	f->ctl = e->ctl;
	f->action = sdup(e->action);
	f->item = sdup(e->item);
	f->text = sdup(e->text);
	f->n = e->n;
	f->pt = e->pt;
	sendp(evc, f);
}

/* handlers: on libux's UI thread; copy the event and hand it over */
void
viewhandler(Uxev *e, void *arg)
{
	Fev *f;

	f = femalloc(sizeof *f);
	f->v = arg;
	if(e->type == UXdrop)
		droplookup(arg, e, f);
	handoff(f, e);
}

void
alerthandler(Uxev *e, void *arg)
{
	Fev *f;

	f = femalloc(sizeof *f);
	f->a = arg;
	handoff(f, e);
}

static void
apphandler(Uxev *e, void*)
{
	handoff(femalloc(sizeof(Fev)), e);
}

static void
fevfree(Fev *f)
{
	int i;

	for(i = 0; i < f->npaths; i++)
		free(f->paths[i]);
	free(f->paths);
	free(f->dest);
	free(f->action);
	free(f->item);
	free(f->text);
	free(f);
}

/* menus: IDs are what scripts invoke, so they're stable */
static void
menus(void)
{
	Uxmenu *m;

	m = uxmenu("file", "File");
	uxitem(m, "newfolder", "New Folder", "n");
	uxitem(m, "open", "Open", "o");
	uxitem(m, "close", "Close Window", "w");
	uxsep(m, "file1");
	uxitem(m, "info", "Get Info", "i");
	uxitem(m, "duplicate", "Duplicate", "d");
	uxitem(m, "rename", "Rename", "r");
	uxsep(m, "file2");
	uxitem(m, "trash", "Move to Trash", "bs");
	uxitem(m, "putback", "Put Back", nil);
	uxsep(m, "file3");
	uxitem(m, "eject", "Eject", "e");
	m = uxmenu("edit", "Edit");
	uxitem(m, "selectall", "Select All", "a");
	m = uxmenu("view", "View");
	uxitem(m, "byicon", "by Icon", nil);
	uxitem(m, "bylist", "by List", nil);
	uxsep(m, "view1");
	uxitem(m, "byname", "by Name", nil);
	uxitem(m, "bykind", "by Kind", nil);
	uxitem(m, "bydate", "by Date", nil);
	uxitem(m, "bysize", "by Size", nil);
	uxsep(m, "view2");
	uxitem(m, "cleanup", "Clean Up", nil);
	m = uxmenu("special", "Special");
	uxitem(m, "emptytrash", "Empty Trash…", nil);
}

void
setfront(View *v)
{
	if(v != nil && v->kind == Kinfo)
		return;
	front = v;
	updatemenus();
}

void
updatemenus(void)
{
	View *v;
	Item **sel;
	int n, i, vols, files;

	v = front != nil ? front : desk;
	n = v != nil ? viewsel(v, &sel) : 0;
	vols = files = 0;
	for(i = 0; i < n; i++)
		if(sel[i]->vol != nil)
			vols++;
		else if(!sel[i]->trash)
			files++;
	if(n > 0)
		free(sel);
	uxitemenable("open", n > 0);
	uxitemenable("info", n > 0 || (v != nil && v->kind == Kfolder));
	uxitemenable("duplicate", files > 0 && vols == 0 && v->kind == Kfolder);
	uxitemenable("rename", n == 1 && files == 1);
	uxitemenable("eject", vols > 0);
	uxitemenable("trash", files > 0 && vols == 0 && v->kind == Kfolder);
	uxitemenable("putback", n > 0 && v->kind == Ktrash);
	uxitemenable("newfolder", v != nil && v->kind == Kfolder);
	uxitemenable("close", v != nil && v->kind != Kdesk);
	uxitemenable("cleanup", v != nil && (v->kind == Kdesk || v->mode == Micons));
	i = v != nil && v->kind == Kfolder;
	uxitemenable("byicon", i);
	uxitemenable("bylist", i);
	uxitemcheck("byicon", i && v->mode == Micons);
	uxitemcheck("bylist", i && v->mode == Mlist);
	uxitemcheck("byname", i && v->sort == Sname);
	uxitemcheck("bykind", i && v->sort == Skind);
	uxitemcheck("bydate", i && v->sort == Sdate);
	uxitemcheck("bysize", i && v->sort == Ssize);
}

/* the view an action is for: the window it names, or the front one */
static View*
target(Fev *e)
{
	View *v;

	if(e->win != nil && (v = viewbywin(e->win)) != nil)
		return v;
	if(front != nil)
		return front;
	return desk;
}

static void
eachsel(View *v, void (*fn)(View*, Item*))
{
	Item **sel;
	char **paths;
	int i, n;

	n = viewsel(v, &sel);
	if(n == 0)
		return;
	/* fn may change the view; work from copies of the paths */
	paths = femalloc(n*sizeof(char*));
	for(i = 0; i < n; i++)
		paths[i] = festrdup(sel[i]->path);
	free(sel);
	for(i = 0; i < n; i++){
		Item *it;
		int j;

		it = nil;
		for(j = 0; j < v->nitem; j++)
			if(strcmp(v->item[j]->path, paths[i]) == 0)
				it = v->item[j];
		if(it != nil)
			fn(v, it);
		free(paths[i]);
	}
	free(paths);
}

static void
infoitem(View*, Item *it)
{
	if(!it->trash)
		infoopen(it->path);
}

static void
dupitem(View *v, Item *it)
{
	char *p;

	if(it->vol != nil)
		return;
	p = fcduplicate(ws, it->path);
	if(p == nil){
		alerterr(it->name);
		return;
	}
	viewrescan(v);
	viewselectname(v, fcbasename(p));
	free(p);
}

static void
ejectitem(View*, Item *it)
{
	View *w, *nx;
	int n;

	if(it->vol == nil)
		return;
	n = strlen(it->vol->path);
	for(w = views; w != nil; w = nx){
		nx = w->next;
		if(w->kind != Kdesk && w->path != nil && strncmp(w->path, it->vol->path, n) == 0
		&& (w->path[n] == 0 || w->path[n] == '/' || n == 1))
			viewclose(w, 0);
	}
	if(it->vol->attached && fcvoldetach(it->vol) < 0)
		alerterr(it->name);
	deskcheck();
}

static void
newfolder(View *v)
{
	char *p;

	if(v->kind != Kfolder)
		return;
	p = fcmkdir(ws, v->path, nil);
	if(p == nil){
		alerterr(v->title);
		return;
	}
	viewrescan(v);
	viewselectname(v, fcbasename(p));
	free(p);
	if(v->mode == Micons)
		renamestart(v);
}

static void
action(Fev *e)
{
	View *v;
	char *a;

	v = target(e);
	if(v == nil)
		return;
	a = e->action;
	if(strcmp(a, "open") == 0)
		eachsel(v, openit);
	else if(strcmp(a, "close") == 0){
		if(v->kind != Kdesk)
			viewclose(v, v->kind == Kfolder);
	}else if(strcmp(a, "info") == 0){
		Item **sel;
		int n;

		n = viewsel(v, &sel);
		if(n > 0){
			free(sel);
			eachsel(v, infoitem);
		}else if(v->kind == Kfolder)
			infoopen(v->path);
	}else if(strcmp(a, "duplicate") == 0)
		eachsel(v, dupitem);
	else if(strcmp(a, "rename") == 0)
		renamestart(v);
	else if(strcmp(a, "eject") == 0)
		eachsel(v, ejectitem);
	else if(strcmp(a, "trash") == 0)
		eachsel(v, totrash);
	else if(strcmp(a, "putback") == 0)
		eachsel(v, putback);
	else if(strcmp(a, "emptytrash") == 0)
		emptytrash();
	else if(strcmp(a, "newfolder") == 0)
		newfolder(v);
	else if(strcmp(a, "selectall") == 0)
		viewselectall(v);
	else if(strcmp(a, "byicon") == 0)
		viewsetmode(v, Micons);
	else if(strcmp(a, "bylist") == 0)
		viewsetmode(v, Mlist);
	else if(strcmp(a, "byname") == 0)
		viewsetsort(v, Sname);
	else if(strcmp(a, "bykind") == 0)
		viewsetsort(v, Skind);
	else if(strcmp(a, "bydate") == 0)
		viewsetsort(v, Sdate);
	else if(strcmp(a, "bysize") == 0)
		viewsetsort(v, Ssize);
	else if(strcmp(a, "cleanup") == 0)
		viewcleanup(v);
	updatemenus();
}

static void
handle(Fev *e)
{
	View *v;

	if(e->a != nil){
		alertevent(e->a, e);
		return;
	}
	if(e->v != nil){
		/* the view may have closed since the event was queued */
		for(v = views; v != nil; v = v->next)
			if(v == e->v)
				break;
		if(v == nil)
			return;
		if(e->type == UXfront)
			setfront(v);
		if(v->kind == Kinfo)
			infoevent(v, e);
		else
			viewevent(v, e);
		return;
	}
	switch(e->type){
	case UXaction:
		action(e);
		break;
	case UXquit:
		for(v = views; v != nil; v = v->next)
			if(v->kind == Kfolder)
				savewin(v);
		uxquit();
		break;
	}
}

static void
tick(void)
{
	View *v;

	deskcheck();
	trashcheck();
	for(v = views; v != nil; v = v->next)
		if(v->kind == Kfolder)
			viewrescan(v);
		else if(v->kind == Kinfo)
			inforefresh(v);
}

static void
ticker(void*)
{
	threadsetname("finder tick");
	for(;;){
		sleep(tickms);
		sendul(tickc, 1);
	}
}

/* follow uxmeta's change stream; reopen it if uxmeta restarts */
static void
metareader(void*)
{
	char *p, buf[8192], *s, *e;
	int fd, n, off;

	threadsetname("finder meta");
	p = smprint("%s/changes", metaroot);
	for(;;){
		fd = open(p, OREAD);
		if(fd < 0){
			sleep(1000);
			continue;
		}
		off = 0;
		while((n = read(fd, buf+off, sizeof buf-1-off)) > 0){
			n += off;
			buf[n] = 0;
			for(s = buf; (e = strchr(s, '\n')) != nil; s = e+1){
				*e = 0;
				sendp(metac, festrdup(s));
			}
			off = n - (s-buf);
			memmove(buf, s, off);
		}
		close(fd);
		sleep(1000);
	}
}

/* reopen the windows that were open when Finder last ran */
static void
reopen(void)
{
	Fcrec *r, *recs;
	char *s;

	if(meta == nil)
		return;
	recs = fcmetaquery(meta, "folder open=yes");
	for(r = recs; r != nil; r = r->next){
		s = fcreckey(r);
		if(s != nil && access(s, AEXIST) == 0)
			folderopen(s, 0);
	}
	fcrecfree(recs);
}

static void
finderproc(void*)
{
	Alt a[4];
	Fev *e;
	char *line;
	ulong t;

	threadsetname("finder");
	deskload();
	reopen();
	setfront(desk);
	memset(a, 0, sizeof a);
	a[0].c = evc;
	a[0].v = &e;
	a[0].op = CHANRCV;
	a[1].c = tickc;
	a[1].v = &t;
	a[1].op = CHANRCV;
	a[2].c = metac;
	a[2].v = &line;
	a[2].op = meta != nil ? CHANRCV : CHANNOP;
	a[3].op = CHANEND;
	for(;;){
		switch(alt(a)){
		case 0:
			handle(e);
			fevfree(e);
			break;
		case 1:
			tick();
			break;
		case 2:
			metachange(line);
			free(line);
			break;
		}
	}
}

/* make our own window the desktop, if rio has /dev/ux */
static void
claimdesk(void)
{
	int fd;

	deskfd = open("/dev/ux/desktop/ctl", ORDWR);
	if(deskfd < 0)
		return;
	if(fprint(deskfd, "claim") < 0){
		fprint(2, "finder: can't claim the desktop: %r\n");
		close(deskfd);
		deskfd = -1;
		return;
	}
	/* libux asks for classic chrome on its windows; not on the desktop */
	if((fd = open("/dev/ux/ctl", OWRITE)) >= 0){
		fprint(fd, "chrome %d off\n", uxwinid(desk->w));
		close(fd);
	}
}

void
threadmain(int argc, char **argv)
{
	char *wsname, *s, *e;
	int nometa;
	char *vols[32];
	int nvols, i;

	wsname = nil;
	nometa = 0;
	nvols = 0;
	ARGBEGIN{
	case 'D':
		nodesk = 1;
		break;
	case 'M':
		nometa = 1;
		break;
	case 'm':
		metaroot = EARGF(usage());
		break;
	case 'w':
		wsname = EARGF(usage());
		break;
	case 'i':
		tickms = atoi(EARGF(usage()));
		if(tickms < 10)
			tickms = 10;
		break;
	case 'v':
		if(nvols == nelem(vols))
			sysfatal("too many -v");
		vols[nvols++] = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	quotefmtinstall();
	if(!nometa){
		meta = fcmetaopen(metaroot);
		if(meta == nil)
			fprint(2, "finder: %r; running without uxmeta\n");
	}
	ws = fcwsopen(wsname, meta);
	if(ws == nil)
		sysfatal("workspace: %r");
	for(i = 0; i < nvols; i++){
		s = festrdup(vols[i]);
		e = strchr(s, '=');
		if(e == nil)
			sysfatal("-v name=path");
		*e++ = 0;
		fcwsaddvol(ws, s, e)->desktop = 1;
		free(s);
	}
	putenv("workspace", ws->name);

	evc = chancreate(sizeof(Fev*), 256);
	tickc = chancreate(sizeof(ulong), 0);
	metac = chancreate(sizeof(char*), 256);

	uxinit("finder", "Finder");
	/* Finder has no resources of its own; it uses the system's icons */
	s = getenv("uxsysres");
	if(s == nil || *s == 0){
		free(s);
		s = festrdup("/lib/ux/system.rsrc");
	}
	if(access(s, AREAD) == 0 && uxres(s) < 0)
		fprint(2, "finder: system icons: %r\n");
	free(s);
	uxapphandler(apphandler, nil);
	menus();
	desk = deskopen();
	if(!nodesk)
		claimdesk();
	proccreate(finderproc, nil, 64*1024);
	proccreate(ticker, nil, 8*1024);
	if(meta != nil)
		proccreate(metareader, nil, 16*1024);
	uxmain();
	threadexitsall(nil);
}
