/*
 * Resources: icons, strings, menus and dialogs from the app's resource
 * file, read through resfs (docs/protocols/res.md).
 *
 * uxres starts a resfs for the app's file (and the system resource
 * file) in a proc of its own, with its own namespace, so the mount
 * goes away with the app and nobody else sees it. Every resource file
 * is opened by that proc and handed back as a file descriptor, which
 * the app's procs share. A resfs tree that is already mounted (a
 * directory) is used as it is.
 *
 * Lookups try the app's resources first, then the system's, as the
 * Resource Manager's chain did.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <mouse.h>
#include <keyboard.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include <ctype.h>
#include "ux.h"
#include "impl.h"

typedef struct Rroot Rroot;
typedef struct Rreq Rreq;
typedef struct Rdlg Rdlg;
typedef struct Rdecor Rdecor;
typedef struct Rimg Rimg;
typedef struct Rmenu Rmenu;

enum {
	Maxroot	= 2,	/* the app's, the system's */
	Maxres	= 1<<20,	/* largest text form read */
	Nparam	= 4,
};

struct Rroot {
	char	*dir;	/* in the resource proc's namespace */
	char	*index;	/* index, read on first use */
	int	noindex;
};

/* a request to the resource proc */
struct Rreq {
	char	*path;
	int	fd;
	char	err[ERRMAX];
};

/* a dialog or alert made from a DITL */
struct Rdecor {
	Rectangle	r;	/* window-local */
	char	*icon;
};

struct Rdlg {
	Uxwin	*w;
	Uxctl	**item;	/* by DITL item number - 1; nil where no control */
	int	nitem;
	Rdecor	*dec;
	int	ndec;
	Rdlg	*next;
};

struct Rimg {
	Uxwin	*w;
	char	*name;
	int	small;
	Image	*img;	/* nil: looked for and not found */
	Rimg	*next;
};

struct Rmenu {
	int	id;
	Uxmenu	*m;
	Rmenu	*next;
};

static struct {
	QLock;		/* one request to the proc at a time */
	int	started;
	char	*err;	/* why uxres failed */
	Rroot	root[Maxroot];
	int	nroot;
	Channel	*req;	/* Rreq* */
	Channel	*rep;	/* Rreq* */
	QLock	idxlk;	/* Rroot.index */
	char	*param[Nparam];	/* guarded by uxlk */
	Rdlg	*dlgs;	/* uxlk */
	Rimg	*imgs;	/* uxlk */
	Rmenu	*menus;	/* uxlk */
} res;

/* a file to serve and the tree name it gets */
typedef struct Rfile Rfile;
struct Rfile {
	char	*name;
	char	*path;
};

typedef struct Rstart Rstart;
struct Rstart {
	Rfile	f[Maxroot];
	int	nf;
	char	*dir[Maxroot];	/* roots given as directories */
	Channel	*c;	/* char*: nil, or the error */
};

static int
isdir(char *path)
{
	Dir *d;
	int r;

	d = dirstat(path);
	if(d == nil)
		return 0;
	r = (d->qid.type & QTDIR) != 0;
	free(d);
	return r;
}

static int
isfile(char *path)
{
	Dir *d;
	int r;

	d = dirstat(path);
	if(d == nil)
		return 0;
	r = (d->qid.type & QTDIR) == 0;
	free(d);
	return r;
}

/* start resfs on s->f, posting it in /srv; nil or the error */
static char*
startresfs(Rstart *s, char *srvname)
{
	char *argv[6+Maxroot+1], *bin, buf[ERRMAX];
	int i, n, pid;
	Waitmsg *wm;

	bin = getenv("resfs");
	if(bin == nil || *bin == 0){
		free(bin);
		bin = estrdup("/bin/resfs");
	}
	n = 0;
	argv[n++] = "resfs";
	argv[n++] = "-m";
	argv[n++] = "";
	argv[n++] = "-s";
	argv[n++] = srvname;
	for(i = 0; i < s->nf; i++)
		argv[n++] = smprint("%s=%s", s->f[i].name, s->f[i].path);
	argv[n] = nil;
	switch(pid = rfork(RFPROC|RFFDG)){
	case -1:
		snprint(buf, sizeof buf, "fork: %r");
		return estrdup(buf);
	case 0:
		exec(bin, argv);
		fprint(2, "libux: exec %s: %r\n", bin);
		_exits("exec");
	}
	for(i = 5; i < n; i++)
		free(argv[i]);
	while((wm = wait()) != nil){
		if(wm->pid == pid)
			break;
		free(wm);
	}
	if(wm == nil){
		snprint(buf, sizeof buf, "%s: lost", bin);
		free(bin);
		return estrdup(buf);
	}
	if(wm->msg[0]){
		snprint(buf, sizeof buf, "%s: %s", bin, wm->msg);
		free(wm);
		free(bin);
		return estrdup(buf);
	}
	free(wm);
	free(bin);
	return nil;
}

/* the resource proc: its own namespace with resfs in it; opens files */
static void
resproc(void *v)
{
	Rstart *s;
	Rreq *o;
	char *err, *srvname, buf[ERRMAX];
	int fd, i;

	threadsetname("uxres");
	s = v;
	rfork(RFNAMEG);
	err = nil;
	if(s->nf > 0){
		srvname = smprint("uxres.%s.%s.%d", getuser(), ux.name, getpid());
		err = startresfs(s, srvname);
		if(err == nil){
			snprint(buf, sizeof buf, "/srv/%s", srvname);
			fd = open(buf, ORDWR);
			if(fd < 0){
				snprint(buf, sizeof buf, "open /srv/%s: %r", srvname);
				err = estrdup(buf);
			}else{
				remove(buf);
				if(mount(fd, -1, "/mnt", MBEFORE, "") < 0){
					snprint(buf, sizeof buf, "mount resfs: %r");
					err = estrdup(buf);
				}
				close(fd);
			}
		}
		free(srvname);
	}
	if(err == nil){
		for(i = 0; i < s->nf; i++){
			res.root[res.nroot].dir = smprint("/mnt/%s", s->f[i].name);
			res.nroot++;
		}
	}
	sendp(s->c, err);
	if(err != nil)
		return;
	while((o = recvp(res.req)) != nil){
		o->fd = open(o->path, OREAD);
		if(o->fd < 0)
			rerrstr(o->err, sizeof o->err);
		sendp(res.rep, o);
	}
}

/* open a file in the resource proc's namespace */
static int
resopen(char *path)
{
	Rreq o;

	memset(&o, 0, sizeof o);
	o.path = path;
	qlock(&res);
	if(res.req == nil){
		qunlock(&res);
		werrstr("no resources");
		return -1;
	}
	sendp(res.req, &o);
	recvp(res.rep);
	qunlock(&res);
	if(o.fd < 0)
		werrstr("%s", o.err);
	return o.fd;
}

static char*
readfd(int fd, long *np)
{
	char *buf;
	long n, m;

	buf = nil;
	n = 0;
	for(;;){
		buf = erealloc(buf, n+8192+1);
		m = read(fd, buf+n, 8192);
		if(m <= 0)
			break;
		n += m;
		if(n > Maxres)
			break;
	}
	buf[n] = 0;
	if(np)
		*np = n;
	return buf;
}

/* a type code as resfs names it: STR  is STR%20 */
static char*
esctype(char *type)
{
	static char hex[] = "0123456789ABCDEF";
	char buf[16], *p;
	int i, c;

	p = buf;
	for(i = 0; i < 4 && type[i]; i++){
		c = (uchar)type[i];
		if(c < 0x21 || c > 0x7E || c == '/' || c == '%'){
			*p++ = '%';
			*p++ = hex[c>>4];
			*p++ = hex[c&15];
		}else
			*p++ = c;
	}
	*p = 0;
	return estrdup(buf);
}

/* a resource's file, from the first root that has it: malloc'd, NUL-terminated */
static char*
resread(char *type, int id, char *file)
{
	char *t, *path, *buf;
	int i, fd;

	t = esctype(type);
	buf = nil;
	for(i = 0; i < res.nroot && buf == nil; i++){
		path = smprint("%s/%s/%d/%s", res.root[i].dir, t, id, file);
		fd = resopen(path);
		free(path);
		if(fd < 0)
			continue;
		buf = readfd(fd, nil);
		close(fd);
	}
	free(t);
	if(buf == nil)
		werrstr("no %s %d", type, id);
	return buf;
}

/* undo the text forms' escapes, in place */
static char*
unesc(char *s)
{
	char *r, *w;
	int c;

	if(s == nil)
		return nil;
	for(r = w = s; *r; r++){
		if(*r != '\\' || r[1] == 0){
			*w++ = *r;
			continue;
		}
		switch(*++r){
		case 'n':
			*w++ = '\n';
			break;
		case 't':
			*w++ = '\t';
			break;
		case 'x':
			if(isxdigit(r[1]) && isxdigit(r[2])){
				c = r[1] <= '9' ? r[1]-'0' : (r[1]|0x20)-'a'+10;
				c = c*16 + (r[2] <= '9' ? r[2]-'0' : (r[2]|0x20)-'a'+10);
				*w++ = c;
				r += 2;
			}else
				*w++ = 'x';
			break;
		default:
			*w++ = *r;
		}
	}
	*w = 0;
	return s;
}

/* an attribute's value in a tokenized line, escapes undone; nil if absent */
static char*
attr(char **f, int nf, char *name)
{
	int i, n;

	n = strlen(name);
	for(i = 1; i < nf; i++)
		if(strncmp(f[i], name, n) == 0 && f[i][n] == '=')
			return unesc(f[i]+n+1);
	return nil;
}

static int
on(char *v, int def)
{
	if(v == nil)
		return def;
	return strcmp(v, "on") == 0;
}

static int
parserect(char *s, Rectangle *r)
{
	char *f[5];
	char buf[64];

	if(s == nil || strlen(s) >= sizeof buf)
		return 0;
	strcpy(buf, s);
	if(getfields(buf, f, nelem(f), 0, ",") != 4)
		return 0;
	*r = Rect(atoi(f[0]), atoi(f[1]), atoi(f[2]), atoi(f[3]));
	return 1;
}

/* lines of a text form; buf is changed */
static int
lines(char *buf, char ***lp)
{
	char **l, *p, *e;
	int n;

	l = nil;
	n = 0;
	for(p = buf; *p; p = e+1){
		e = strchr(p, '\n');
		if(e == nil)
			e = p+strlen(p);
		l = erealloc(l, (n+1)*sizeof(char*));
		l[n++] = p;
		if(*e == 0)
			break;
		*e = 0;
	}
	*lp = l;
	return n;
}

/*
 * An ID made from a title, as scripts see it: lowercase letters and
 * digits, other runs as one -, at most 32 bytes. "Save As…" is save-as.
 */
static char*
slug(char *s)
{
	char buf[33], *p;
	int dash;

	p = buf;
	dash = 0;
	for(; *s && p < buf+sizeof buf-1; s++){
		if(*s >= 'A' && *s <= 'Z' || *s >= 'a' && *s <= 'z' || *s >= '0' && *s <= '9'){
			if(dash && p > buf && p < buf+sizeof buf-2)
				*p++ = '-';
			dash = 0;
			*p++ = tolower(*s);
		}else
			dash = 1;
	}
	*p = 0;
	return estrdup(buf);
}

static int
inlist(char **l, int n, char *s)
{
	int i;

	for(i = 0; i < n; i++)
		if(l[i] != nil && strcmp(l[i], s) == 0)
			return 1;
	return 0;
}

static char*
defaultpath(void)
{
	char *s, *dir, *p;

	s = getenv("uxres");
	if(s != nil && *s)
		return s;
	free(s);
	s = smprint("/mnt/res/%s", ux.name);
	if(isdir(s))
		return s;
	free(s);
	if(argv0 == nil)
		return nil;
	dir = estrdup(argv0);
	p = strrchr(dir, '/');
	if(p != nil)
		*p = 0;
	else{
		free(dir);
		dir = estrdup(".");
	}
	s = smprint("%s/%s.rsrc", dir, ux.name);
	if(!isfile(s)){
		free(s);
		s = smprint("%s/resources.rsrc", dir);
		if(!isfile(s)){
			free(s);
			s = nil;
		}
	}
	free(dir);
	return s;
}

/* public */

int
uxres(char *path)
{
	Rstart s;
	char *sys, *err, *p;

	qlock(&res);
	if(res.started){
		qunlock(&res);
		werrstr("resources already set");
		return -1;
	}
	memset(&s, 0, sizeof s);
	p = path ? estrdup(path) : defaultpath();
	if(p == nil){
		qunlock(&res);
		werrstr("no resource file for %s", ux.name);
		return -1;
	}
	if(isdir(p)){
		res.root[res.nroot++].dir = p;
	}else if(isfile(p)){
		s.f[s.nf].name = "uxres.app";
		s.f[s.nf++].path = p;
	}else{
		qunlock(&res);
		werrstr("%s: no resources there", p);
		free(p);
		return -1;
	}
	sys = getenv("uxsysres");
	if(sys == nil || *sys == 0){
		free(sys);
		sys = estrdup("/lib/ux/system.rsrc");
	}
	if(isfile(sys)){
		s.f[s.nf].name = "uxres.sys";
		s.f[s.nf++].path = sys;
	}
	s.c = chancreate(sizeof(char*), 0);
	res.req = chancreate(sizeof(Rreq*), 0);
	res.rep = chancreate(sizeof(Rreq*), 0);
	proccreate(resproc, &s, 16*1024);
	err = recvp(s.c);
	chanfree(s.c);
	if(err != nil){
		chanfree(res.req);
		chanfree(res.rep);
		res.req = res.rep = nil;
		res.nroot = 0;
		qunlock(&res);
		werrstr("%s", err);
		free(err);
		return -1;
	}
	res.started = 1;
	qunlock(&res);
	return 0;
}

char*
uxresstr(int id, int n)
{
	char *buf, **l, *s;
	int nl;

	if(n == 0){
		buf = resread("STR ", id, "text");
		if(buf == nil)
			return nil;
		nl = lines(buf, &l);
		s = nl > 0 ? estrdup(unesc(l[0])) : estrdup("");
		free(l);
		free(buf);
		return s;
	}
	buf = resread("STR#", id, "text");
	if(buf == nil)
		return nil;
	nl = lines(buf, &l);
	/* the text ends with a newline: the last "line" is empty */
	if(nl > 0 && l[nl-1][0] == 0)
		nl--;
	s = nil;
	if(n > 0 && n <= nl)
		s = estrdup(unesc(l[n-1]));
	else
		werrstr("no string %d in STR# %d", n, id);
	free(l);
	free(buf);
	return s;
}

static int
menuused(char *id)
{
	int r;

	qlock(&uxlk);
	r = _findmenu(id) != nil;
	qunlock(&uxlk);
	return r;
}

/* is action used by an item of any menu already? */
static int
actionused(char *id)
{
	int i, j, r;

	r = 0;
	qlock(&uxlk);
	for(i = 0; i < ux.nmenu && !r; i++)
		for(j = 0; j < ux.menu[i]->nitem; j++)
			if(strcmp(ux.menu[i]->item[j].action, id) == 0){
				r = 1;
				break;
			}
	qunlock(&uxlk);
	return r;
}

Uxmenu*
uxresmenu(int id)
{
	char *buf, **l, *f[32], *t, *mid, *aid, *v, **ids;
	int nl, nf, i, n, menuon;
	Uxmenu *m;
	Rmenu *rm;

	qlock(&uxlk);
	for(rm = res.menus; rm != nil; rm = rm->next)
		if(rm->id == id){
			qunlock(&uxlk);
			return rm->m;
		}
	qunlock(&uxlk);
	buf = resread("MENU", id, "text");
	if(buf == nil)
		return nil;
	nl = lines(buf, &l);
	if(nl < 1 || (nf = tokenize(l[0], f, nelem(f))) < 1 || strcmp(f[0], "menu") != 0){
		free(l);
		free(buf);
		werrstr("MENU %d: no menu line", id);
		return nil;
	}
	t = attr(f, nf, "title");
	if(t == nil)
		t = "";
	menuon = on(attr(f, nf, "enabled"), 1);
	mid = slug(t);
	if(!_validid(mid) || menuused(mid)){
		free(mid);
		mid = smprint("menu%d", id);
	}
	m = uxmenu(mid, t);
	ids = emalloc(nl*sizeof(char*));
	for(i = 1, n = 1; i < nl; i++){
		nf = tokenize(l[i], f, nelem(f));
		if(nf < 1 || strcmp(f[0], "item") != 0)
			continue;
		t = attr(f, nf, "title");
		if(t == nil)
			t = "";
		if(strcmp(t, "-") == 0){
			aid = smprint("%s-%d", mid, n);
			uxsep(m, aid);
		}else{
			aid = slug(t);
			if(!_validid(aid) || inlist(ids, n, aid) || actionused(aid)){
				free(aid);
				aid = smprint("%s-%d", mid, n);
			}
			v = attr(f, nf, "key");
			if(v != nil && v[0] >= 'A' && v[0] <= 'Z' && v[1] == 0)
				v[0] = tolower(v[0]);
			uxitem(m, aid, t, v);
			if(!menuon || !on(attr(f, nf, "enabled"), 1))
				uxitemenable(aid, 0);
			if((v = attr(f, nf, "mark")) != nil && *v)
				uxitemcheck(aid, 1);
		}
		ids[n-1] = aid;
		n++;
	}
	for(i = 0; i < n-1; i++)
		free(ids[i]);
	free(ids);
	free(mid);
	free(l);
	free(buf);
	qlock(&uxlk);
	rm = emalloc(sizeof *rm);
	rm->id = id;
	rm->m = m;
	rm->next = res.menus;
	res.menus = rm;
	qunlock(&uxlk);
	return m;
}

void
uxresparam(int n, char *s)
{
	if(n < 0 || n >= Nparam)
		return;
	qlock(&uxlk);
	free(res.param[n]);
	res.param[n] = s ? estrdup(s) : nil;
	qunlock(&uxlk);
}

/* text with ^0 to ^3 replaced by the params */
static char*
params(char *s)
{
	Fmt f;
	int n;

	fmtstrinit(&f);
	qlock(&uxlk);
	for(; *s; s++){
		if(s[0] == '^' && s[1] >= '0' && s[1] < '0'+Nparam){
			n = s[1]-'0';
			if(res.param[n] != nil)
				fmtprint(&f, "%s", res.param[n]);
			s++;
		}else
			fmtrune(&f, (uchar)*s);
	}
	qunlock(&uxlk);
	return fmtstrflush(&f);
}

/* build a window's controls from DITL id; def is the default item */
static int
ditl(Uxwin *w, int id, int def, Uxfn *fn, void *arg)
{
	char *buf, **l, *f[16], *type, *text, *cid, **ids, *s, *prev;
	int nl, nf, i, n;
	Rectangle r;
	Rdlg *d;
	Uxctl *c;

	buf = resread("DITL", id, "text");
	if(buf == nil)
		return -1;
	nl = lines(buf, &l);
	d = emalloc(sizeof *d);
	d->w = w;
	d->item = emalloc((nl+1)*sizeof(Uxctl*));
	ids = emalloc((nl+1)*sizeof(char*));
	prev = nil;
	for(i = 0, n = 0; i < nl; i++){
		nf = tokenize(l[i], f, nelem(f));
		if(nf < 1 || strcmp(f[0], "item") != 0)
			continue;
		n++;
		type = attr(f, nf, "type");
		text = attr(f, nf, "text");
		if(type == nil || !parserect(attr(f, nf, "rect"), &r))
			continue;
		cid = nil;
		if(text != nil && (strcmp(type, "button") == 0 || strcmp(type, "checkbox") == 0
		|| strcmp(type, "radio") == 0)){
			cid = slug(text);
			if(!_validid(cid) || inlist(ids, n, cid)){
				free(cid);
				cid = nil;
			}
		}
		if(cid == nil)
			cid = smprint("item%d", n);
		ids[n-1] = cid;
		c = nil;
		if(text == nil)
			text = "";
		if(strcmp(type, "button") == 0)
			c = uxbutton(w, cid, text, r);
		else if(strcmp(type, "checkbox") == 0 || strcmp(type, "radio") == 0)
			c = uxcheckbox(w, cid, text, r);
		else if(strcmp(type, "text") == 0){
			s = params(text);
			c = uxlabel(w, cid, s, r);
			free(prev);
			prev = s;
		}else if(strcmp(type, "edit") == 0){
			s = prev ? estrdup(prev) : estrdup(cid);
			if(*s && s[strlen(s)-1] == ':')
				s[strlen(s)-1] = 0;
			c = uxtextfield(w, cid, s, r, 0);
			free(s);
			if(*text){
				s = params(text);
				uxsetvalue(c, s);
				free(s);
			}
		}else if(strcmp(type, "icon") == 0 && (s = attr(f, nf, "id")) != nil){
			d->dec = erealloc(d->dec, (d->ndec+1)*sizeof(Rdecor));
			d->dec[d->ndec].r = r;
			d->dec[d->ndec].icon = estrdup(s);
			d->ndec++;
		}
		if(c != nil){
			d->item[n-1] = c;
			if(fn != nil)
				uxhandler(c, fn, arg);
			if(n == def && strcmp(type, "button") == 0)
				uxdefault(w, c);
		}
	}
	d->nitem = n;
	for(i = 0; i < n; i++)
		free(ids[i]);
	free(ids);
	free(prev);
	free(l);
	free(buf);
	qlock(&uxlk);
	d->next = res.dlgs;
	res.dlgs = d;
	w->dirty = 1;
	qunlock(&uxlk);
	return 0;
}

static Uxwin*
mkdialog(char *title, Rectangle r, int ditlid, int def, Uxfn *fn, void *arg)
{
	Uxwin *w;

	w = uxdialog(title, r);
	if(fn != nil)
		uxwinhandler(w, fn, arg);
	if(ditl(w, ditlid, def, fn, arg) < 0){
		uxwinclose(w);
		return nil;
	}
	return w;
}

Uxwin*
uxresdialog(int id, Uxfn *fn, void *arg)
{
	char *buf, **l, *f[16], *t, *s;
	int nl, nf;
	Rectangle r;
	Uxwin *w;

	buf = resread("DLOG", id, "text");
	if(buf == nil)
		return nil;
	nl = lines(buf, &l);
	w = nil;
	if(nl > 0 && (nf = tokenize(l[0], f, nelem(f))) > 0 && strcmp(f[0], "dlog") == 0
	&& parserect(attr(f, nf, "rect"), &r) && (s = attr(f, nf, "ditl")) != nil){
		t = attr(f, nf, "title");
		w = mkdialog(t ? t : "", r, atoi(s), 1, fn, arg);
	}else
		werrstr("DLOG %d: bad dlog line", id);
	free(l);
	free(buf);
	return w;
}

Uxwin*
uxresalert(int id, Uxfn *fn, void *arg)
{
	char *buf, **l, *f[16], *s, *title;
	int nl, nf, def;
	Rectangle r;
	Uxwin *w;

	buf = resread("ALRT", id, "text");
	if(buf == nil)
		return nil;
	nl = lines(buf, &l);
	w = nil;
	if(nl > 0 && (nf = tokenize(l[0], f, nelem(f))) > 0 && strcmp(f[0], "alrt") == 0
	&& parserect(attr(f, nf, "rect"), &r) && (s = attr(f, nf, "ditl")) != nil){
		/* the first stage's bold-item bit picks item 2 as the default */
		def = 1;
		if(attr(f, nf, "stages") != nil && (strtoul(attr(f, nf, "stages"), nil, 0) & 8))
			def = 2;
		qlock(&uxlk);
		title = estrdup(ux.title);
		qunlock(&uxlk);
		w = mkdialog(title, r, atoi(s), def, fn, arg);
		free(title);
	}else
		werrstr("ALRT %d: bad alrt line", id);
	free(l);
	free(buf);
	return w;
}

static Rdlg*
finddlg(Uxwin *w)
{
	Rdlg *d;

	for(d = res.dlgs; d != nil; d = d->next)
		if(d->w == w)
			return d;
	return nil;
}

int
uxresitem(Uxctl *c)
{
	Rdlg *d;
	int i;

	qlock(&uxlk);
	d = finddlg(c->w);
	if(d != nil)
		for(i = 0; i < d->nitem; i++)
			if(d->item[i] == c){
				qunlock(&uxlk);
				return i+1;
			}
	qunlock(&uxlk);
	return 0;
}

Uxctl*
uxresctl(Uxwin *w, int n)
{
	Rdlg *d;
	Uxctl *c;

	c = nil;
	qlock(&uxlk);
	d = finddlg(w);
	if(d != nil && n >= 1 && n <= d->nitem && !w->dead)
		c = d->item[n-1];
	qunlock(&uxlk);
	return c;
}

/* internal; uxlk held */

/* the ID of the icon family named name in a root's index, or -1 */
static int
indexicon(Rroot *rt, char *name, int small)
{
	char *buf, **l, *f[8], *n, *t;
	int nl, nf, i, id, pass;
	long len;
	int fd;

	qlock(&res.idxlk);
	if(rt->index == nil && !rt->noindex){
		t = smprint("%s/index", rt->dir);
		fd = resopen(t);
		free(t);
		if(fd < 0)
			rt->noindex = 1;
		else{
			rt->index = readfd(fd, &len);
			close(fd);
		}
	}
	buf = rt->index ? estrdup(rt->index) : nil;
	qunlock(&res.idxlk);
	if(buf == nil)
		return -1;
	nl = lines(buf, &l);
	id = -1;
	t = nil;
	/* names are on the 1-bit member, as rescomp's icon writes them; then any member */
	for(pass = 0; pass < 2 && id < 0; pass++)
		for(i = 0; i < nl && id < 0; i++){
			/* tokenize a copy: the second pass reads the lines again */
			free(t);
			t = estrdup(l[i]);
			nf = tokenize(t, f, nelem(f));
			if(nf < 2)
				continue;
			if(pass == 0 && strcmp(f[0], small ? "ics#" : "ICN#") != 0)
				continue;
			if(pass == 1 && strcmp(f[0], "ICN#") != 0 && strcmp(f[0], "icl4") != 0
			&& strcmp(f[0], "icl8") != 0 && strcmp(f[0], "ics#") != 0
			&& strcmp(f[0], "ics4") != 0 && strcmp(f[0], "ics8") != 0)
				continue;
			/* index names are not escaped */
			for(nf--; nf >= 2; nf--)
				if(strncmp(f[nf], "name=", 5) == 0)
					break;
			if(nf < 2)
				continue;
			n = f[nf]+5;
			if(strcmp(n, name) == 0)
				id = atoi(f[1]);
		}
	free(t);
	free(l);
	free(buf);
	return id;
}

static int
isnum(char *s)
{
	if(*s == '-')
		s++;
	if(*s == 0)
		return 0;
	for(; *s; s++)
		if(*s < '0' || *s > '9')
			return 0;
	return 1;
}

static Image*
loadicon(Uxwin *w, char *name, int small)
{
	char *path;
	int i, id, fd;
	Image *img;

	for(i = 0; i < res.nroot; i++){
		id = isnum(name) ? atoi(name) : indexicon(&res.root[i], name, small);
		if(id < -32768 || id > 32767)
			continue;
		if(id < 0 && !isnum(name))
			continue;
		path = smprint("%s/icon/%d/%s", res.root[i].dir, id, small ? "small" : "image");
		fd = resopen(path);
		free(path);
		if(fd < 0)
			continue;
		img = readimage(w->d, fd, 0);
		close(fd);
		if(img != nil)
			return img;
	}
	return nil;
}

/*
 * The image for an icon name: a decimal resource ID, or the name of an
 * icon family (ICN#, or ics# for small). Cached per window and owned
 * by the cache; nil when there are no resources or no such icon.
 */
Image*
_resicon(Uxwin *w, char *name, int small)
{
	Rimg *ri;

	if(w->img == nil || res.nroot == 0 || name == nil || *name == 0)
		return nil;
	for(ri = res.imgs; ri != nil; ri = ri->next)
		if(ri->w == w && ri->small == small && strcmp(ri->name, name) == 0)
			return ri->img;
	ri = emalloc(sizeof *ri);
	ri->w = w;
	ri->name = estrdup(name);
	ri->small = small;
	ri->img = loadicon(w, name, small);
	ri->next = res.imgs;
	res.imgs = ri;
	return ri->img;
}

/* draw a window's DITL icons; called by _redraw */
void
_resdraw(Uxwin *w)
{
	Rdlg *d;
	Image *img;
	Rectangle r;
	int i;

	if(w->img == nil || (d = finddlg(w)) == nil)
		return;
	for(i = 0; i < d->ndec; i++){
		img = _resicon(w, d->dec[i].icon, Dx(d->dec[i].r) < 32);
		if(img == nil)
			continue;
		r = rectaddpt(d->dec[i].r, w->img->r.min);
		r.max.x = r.min.x + Dx(img->r);
		r.max.y = r.min.y + Dy(img->r);
		draw(w->img, r, img, nil, img->r.min);
	}
}

/* forget a window going away: its icons and its DITL */
void
_resfreewin(Uxwin *w)
{
	Rimg **rp, *ri;
	Rdlg **dp, *d;
	int i;

	for(rp = &res.imgs; (ri = *rp) != nil; ){
		if(ri->w != w){
			rp = &ri->next;
			continue;
		}
		*rp = ri->next;
		if(ri->img != nil)
			freeimage(ri->img);
		free(ri->name);
		free(ri);
	}
	for(dp = &res.dlgs; (d = *dp) != nil; ){
		if(d->w != w){
			dp = &d->next;
			continue;
		}
		*dp = d->next;
		for(i = 0; i < d->ndec; i++)
			free(d->dec[i].icon);
		free(d->dec);
		free(d->item);
		free(d);
	}
}
