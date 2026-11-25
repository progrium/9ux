/*
 * The file tree over 9P, and main: serve $ui, run the client, exit
 * with its status.
 *
 *	uxkit [-D] [-m mtpt] cmd [arg...]
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "dat.h"

enum {
	/* app files */
	Qroot, Qversion, Qctl, Qevents, Qtrace, Qkeys, Qmenudir, Qmenu,
	Qthemedir, Qtokens, Qtname, Qpref, Qmode, Qassetdir, Qtypesdir,
	Qtypedir, Qspec, Qtypever, Qstyledir, Qwords, Qwindir, Qclip,
	Qdebugdir, Qinject,
	/* node files */
	Qnode, Qtype, Qstyle, Qorder, Qtag, Qon, Qcontext, Qnctl, Qnevents,
	Qtree, Qbounds, Qprop, Qtitle, Qinner, Qstate, Qfocus, Qoverdir,
	Qmax,
};

typedef struct Faux Faux;
struct Faux {
	int	k;
	int	node;
	char	*name;	/* type, menu or prop name */
	/* once open */
	int	omode;
	int	trunc;
	int	wrote;
	char	*upath;	/* the path from $ui, for errors */
	char	*wname;	/* its window, if any */
	char	*buf;
	long	nbuf;
	char	*rbuf;
	long	nrbuf;
	Sub	*sub;
};

typedef struct Tab Tab;
struct Tab {
	char	*name;
	int	k;
	int	parent;
	int	dir;
};

static Tab tab[] = {
	"version",	Qversion,	Qroot,	0,
	"ctl",	Qctl,	Qroot,	0,
	"events",	Qevents,	Qroot,	0,
	"trace",	Qtrace,	Qroot,	0,
	"keys",	Qkeys,	Qroot,	0,
	"menu",	Qmenudir,	Qroot,	1,
	"theme",	Qthemedir,	Qroot,	1,
	"asset",	Qassetdir,	Qroot,	1,
	"types",	Qtypesdir,	Qroot,	1,
	"style",	Qstyledir,	Qroot,	1,
	"win",	Qwindir,	Qroot,	1,
	"clip",	Qclip,	Qroot,	0,
	"debug",	Qdebugdir,	Qroot,	1,
	"tokens",	Qtokens,	Qthemedir,	0,
	"name",	Qtname,	Qthemedir,	0,
	"pref",	Qpref,	Qthemedir,	0,
	"mode",	Qmode,	Qthemedir,	0,
	"words",	Qwords,	Qstyledir,	0,
	"inject",	Qinject,	Qdebugdir,	0,
	"spec",	Qspec,	Qtypedir,	0,
	"version",	Qtypever,	Qtypedir,	0,
	"type",	Qtype,	Qnode,	0,
	"style",	Qstyle,	Qnode,	0,
	"order",	Qorder,	Qnode,	0,
	"tag",	Qtag,	Qnode,	0,
	"on",	Qon,	Qnode,	0,
	"context",	Qcontext,	Qnode,	0,
	"ctl",	Qnctl,	Qnode,	0,
	"events",	Qnevents,	Qnode,	0,
	"tree",	Qtree,	Qnode,	0,
	"bounds",	Qbounds,	Qnode,	0,
	"title",	Qtitle,	-1,	0,
	"inner",	Qinner,	-1,	0,
	"state",	Qstate,	-1,	0,
	"focus",	Qfocus,	-1,	0,
};

static char *mtpt = "/mnt/ui";
extern char *profiles[];
static char *srvname;

static Faux*
newaux(int k, int node, char *name)
{
	Faux *a;

	a = emalloc(sizeof *a);
	memset(a, 0, sizeof *a);
	a->k = k;
	a->node = node;
	a->name = name != nil ? estrdup(name) : nil;
	a->omode = -1;
	return a;
}

static void
freeaux(Faux *a)
{
	if(a == nil)
		return;
	free(a->name);
	free(a->upath);
	free(a->wname);
	free(a->buf);
	free(a->rbuf);
	free(a);
}

static int
isdir(int k)
{
	switch(k){
	case Qroot: case Qmenudir: case Qthemedir: case Qassetdir: case Qtypesdir:
	case Qtypedir: case Qstyledir: case Qwindir: case Qdebugdir: case Qnode:
	case Qoverdir:
		return 1;
	}
	return 0;
}

static Qid
qidof(Faux *a)
{
	Qid q;
	ulong h;
	char *p;

	h = 0;
	if(a->name != nil)
		for(p = a->name; *p; p++)
			h = h*31 + *p;
	q.path = ((uvlong)a->node<<32) | ((uvlong)a->k<<24) | (h & 0xFFFFFF);
	q.vers = 0;
	q.type = isdir(a->k) ? QTDIR : QTFILE;
	return q;
}

static char*
tabname(int k)
{
	int i;

	for(i = 0; i < nelem(tab); i++)
		if(tab[i].k == k)
			return tab[i].name;
	return "?";
}

static int
menuidx(char *name)
{
	int i;

	for(i = 0; i < app->nmenu; i++)
		if(strcmp(app->menu[i].name, name) == 0)
			return i;
	return -1;
}

/*
 * Walking.
 */
static char*
fswalk1(Fid *fid, char *name, Qid *qid)
{
	Faux *a;
	Node *n, *c;
	Win *w;
	Pdesc *p;
	int i;

	a = fid->aux;
	if(strcmp(name, "..") == 0){
		switch(a->k){
		case Qroot: case Qmenudir: case Qthemedir: case Qassetdir: case Qtypesdir:
		case Qstyledir: case Qwindir: case Qdebugdir:
			a->k = Qroot;
			break;
		case Qtypedir:
			a->k = Qtypesdir;
			break;
		case Qnode:
			n = nodebyid(a->node);
			if(n == nil)
				return "not found";
			if(n->parent == nil){
				a->k = Qwindir;
				a->node = 0;
			}else
				a->node = n->parent->id;
			break;
		case Qoverdir:
			a->k = Qnode;
			break;
		}
		free(a->name);
		a->name = nil;
		*qid = qidof(a);
		fid->qid = *qid;
		return nil;
	}
	switch(a->k){
	case Qroot: case Qthemedir: case Qstyledir: case Qdebugdir: case Qtypedir:
		for(i = 0; i < nelem(tab); i++)
			if(tab[i].parent == a->k && strcmp(tab[i].name, name) == 0)
				break;
		if(i == nelem(tab))
			return "not found";
		a->k = tab[i].k;
		break;
	case Qmenudir:
		if(strcmp(name, "bar") != 0 && menuidx(name) < 0)
			return "not found";
		a->k = Qmenu;
		a->name = estrdup(name);
		break;
	case Qtypesdir:
		if(typebyname(name) == nil)
			return "not found";
		a->k = Qtypedir;
		a->name = estrdup(name);
		break;
	case Qwindir:
		if((w = winbyname(name)) == nil)
			return "not found";
		a->k = Qnode;
		a->node = w->root->id;
		break;
	case Qnode:
		if((n = nodebyid(a->node)) == nil)
			return "not found";
		/* a child named like a window file (title, focus...) hides it */
		for(i = 0; i < nelem(tab); i++)
			if((tab[i].parent == Qnode || (tab[i].parent == -1 && n->parent == nil && kidbyname(n, name) == nil))
			&& strcmp(tab[i].name, name) == 0){
				a->k = tab[i].k;
				*qid = qidof(a);
				fid->qid = *qid;
				return nil;
			}
		p = pdesc(typebyname(n->type), name);
		if(p != nil && isfileprop(p)){
			a->k = Qprop;
			a->name = estrdup(name);
			break;
		}
		if((c = walkkid(n, name)) == nil)
			return "not found";
		a->node = c->id;
		break;
	default:
		return "not found";
	}
	*qid = qidof(a);
	fid->qid = *qid;
	return nil;
}

static char*
fsclone(Fid *old, Fid *new)
{
	Faux *a;

	a = old->aux;
	new->aux = newaux(a->k, a->node, a->name);
	return nil;
}

static void
fsattach(Req *r)
{
	r->fid->aux = newaux(Qroot, 0, nil);
	r->fid->qid = qidof(r->fid->aux);
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

/*
 * Stat and directories.
 */
static char*
nameof(Faux *a)
{
	Node *n;

	switch(a->k){
	case Qroot:
		return "/";
	case Qmenu: case Qtypedir: case Qprop:
		return a->name;
	case Qnode:
		n = nodebyid(a->node);
		if(n == nil)
			return "gone";
		if(n->parent == nil)
			return n->win->name;
		return n->name;
	}
	return tabname(a->k);
}

static void
fillstat(Dir *d, Faux *a)
{
	memset(d, 0, sizeof *d);
	d->name = estrdup(nameof(a));
	d->uid = estrdup("ui");
	d->gid = estrdup("ui");
	d->muid = estrdup("");
	d->qid = qidof(a);
	d->mode = isdir(a->k) ? DMDIR|0777 : 0666;
	if(a->k == Qversion || a->k == Qspec || a->k == Qtypever || a->k == Qwords || a->k == Qinner)
		d->mode = 0444;
	d->atime = d->mtime = time(0);
}

static void
fsstat(Req *r)
{
	Faux *a;

	a = r->fid->aux;
	if(a->k == Qnode && nodebyid(a->node) == nil){
		respond(r, "gone");
		return;
	}
	fillstat(&r->d, a);
	respond(r, nil);
}

typedef struct Dirl Dirl;
struct Dirl {
	Faux	**e;
	int	n;
};

static void
dadd(Dirl *l, int k, int node, char *name)
{
	l->e = erealloc(l->e, (l->n+1)*sizeof(Faux*));
	l->e[l->n++] = newaux(k, node, name);
}

static Dirl*
dirlist(Faux *a)
{
	Dirl *l;
	Node *n;
	Pdesc *p;
	int i;

	l = emalloc(sizeof *l);
	memset(l, 0, sizeof *l);
	switch(a->k){
	case Qroot: case Qthemedir: case Qstyledir: case Qdebugdir: case Qtypedir:
		for(i = 0; i < nelem(tab); i++)
			if(tab[i].parent == a->k)
				dadd(l, tab[i].k, 0, a->k == Qtypedir ? a->name : nil);
		break;
	case Qmenudir:
		dadd(l, Qmenu, 0, "bar");
		for(i = 0; i < app->nmenu; i++)
			dadd(l, Qmenu, 0, app->menu[i].name);
		break;
	case Qtypesdir:
		for(i = 0; i < ntypes; i++)
			dadd(l, Qtypedir, 0, types[i].name);
		break;
	case Qwindir:
		for(i = 0; i < app->nwin; i++)
			dadd(l, Qnode, app->win[i]->root->id, nil);
		break;
	case Qnode:
		n = nodebyid(a->node);
		if(n == nil)
			break;
		/* over/ lists only its overlays */
		for(i = 0; strcmp(n->type, "@over") != 0 && i < nelem(tab); i++)
			if(tab[i].parent == Qnode || (tab[i].parent == -1 && n->parent == nil))
				dadd(l, tab[i].k, n->id, nil);
		if(n->parent == nil && n->win->over != nil)
			dadd(l, Qnode, n->win->over->id, nil);
		p = nil;
		for(i = 0; typebyname(n->type) != nil && i < typebyname(n->type)->np; i++){
			p = &typebyname(n->type)->p[i];
			if(isfileprop(p))
				dadd(l, Qprop, n->id, p->name);
		}
		USED(p);
		for(i = 0; i < n->nkid; i++)
			dadd(l, Qnode, n->kid[i]->id, nil);
		break;
	}
	/* a typedir's files carry the type's name, for stat; fix their names */
	if(a->k == Qtypedir)
		for(i = 0; i < l->n; i++){
			free(l->e[i]->name);
			l->e[i]->name = nil;
		}
	return l;
}

static int
dirgen(int i, Dir *d, void *v)
{
	Dirl *l;

	l = v;
	if(i >= l->n)
		return -1;
	fillstat(d, l->e[i]);
	return 0;
}

static void
freedirl(Dirl *l)
{
	int i;

	for(i = 0; i < l->n; i++)
		freeaux(l->e[i]);
	free(l->e);
	free(l);
}

/*
 * Reading.
 */
static char*
oneline(char *s)
{
	if(s == nil || s[0] == 0)
		return estrdup("");
	return smprint("%s\n", s);
}

static char*
version(void)
{
	Fmt f;
	int i;

	fmtstrinit(&f);
	fmtprint(&f, "uitree 1.0\nhost 9ux-uxkit 0.1\nlayer core 1.0\nlayer components 1.0\n");
	for(i = 0; profiles[i] != nil; i++)
		fmtprint(&f, "profile %s\n", profiles[i]);
	fmtprint(&f, "cmdkey super\n");
	return fmtstrflush(&f);
}

/*
 * ctl help: "verb class", where the class is the right it needs in the
 * public view (policy.md, Rights); author verbs no grant can give.
 */
static char*
helptext(Node *n)
{
	Fmt f;
	int i;

	fmtstrinit(&f);
	if(n == nil){
		fmtprint(&f, "window author\nquit activate\nask author\nkeep author\nbegin author\ncommit author\nabort author\ntheme author\n");
		fmtprint(&f, "set edit\nnew author\ndelete author\ntree author\n");
	}else{
		fmtprint(&f, "set edit\nnew author\ndelete author\ntree author\n");
		if(typebyname(n->type) != nil)
			for(i = 0; i < typebyname(n->type)->nverb; i++)
				fmtprint(&f, "%s %s\n", typebyname(n->type)->verb[i], typebyname(n->type)->vclass[i]);
		if(n->parent == nil)
			fmtprint(&f, "show activate\nactivate activate\nstate edit\nnotify activate\nclose activate\ndestroy author\nask author\nkeep author\n");
	}
	return fmtstrflush(&f);
}

/* a file's contents, or nil and *err */
static char*
content(Faux *a, long *len, char **err)
{
	Node *n, *x;
	Win *w;
	Fmt f;
	char *s, *t;
	int i, j, mi;
	Pdesc *pd;

	*err = nil;
	*len = -1;
	n = nil;
	w = nil;
	if(a->k >= Qnode){
		n = nodebyid(a->node);
		if(n == nil){
			*err = "gone";
			return nil;
		}
		x = n;
		while(x->parent != nil)
			x = x->parent;
		w = x->win;
	}
	switch(a->k){
	case Qversion:
		return version();
	case Qctl:
		return helptext(nil);
	case Qtrace:
		return tracetext();
	case Qinject:
		return estrdup("");
	case Qkeys:
		return estrdup(app->keys);
	case Qmenu:
		if(strcmp(a->name, "bar") == 0)
			return oneline(app->bar);
		mi = menuidx(a->name);
		return estrdup(mi >= 0 ? app->menu[mi].text : "");
	case Qtokens:
		fmtstrinit(&f);
		for(i = 0; i < Ntok; i++)
			fmtprint(&f, "%s %s\n", tokname[i], app->tok[i]);
		return fmtstrflush(&f);
	case Qtname:
		return oneline(app->themename);
	case Qpref:
		return oneline(app->pref);
	case Qmode:
		return oneline(effmode());
	case Qspec:
		return estrdup(typebyname(a->name)->spec);
	case Qtypever:
		return smprint("%s 1.0\n", a->name);
	case Qwords:
		fmtstrinit(&f);
		for(i = 0; stylewords[i] != nil; i++){
			s = stylewords[i];
			if(s[strlen(s)-1] == '=')
				fmtprint(&f, "%.*s value\n", (int)strlen(s)-1, s);
			else
				fmtprint(&f, "%s\n", s);
		}
		return fmtstrflush(&f);
	case Qclip:
		s = emalloc(nclip+1);
		memmove(s, clip, nclip);
		s[nclip] = 0;
		*len = nclip;
		return s;
	case Qtype:
		return oneline(n->type);
	case Qstyle:
		return oneline(n->style);
	case Qorder:
		fmtstrinit(&f);
		for(i = 0; i < n->nord; i++)
			for(j = 0; j < n->nkid; j++)
				if(n->kid[j]->id == n->ord[i])
					fmtprint(&f, "%s%s", i > 0 ? " " : "", n->kid[j]->name);
		s = fmtstrflush(&f);
		t = oneline(s);
		free(s);
		return t;
	case Qtag:
		return oneline(getprop(n, "tag"));
	case Qon:
		return oneline(getprop(n, "on"));
	case Qcontext:
		return oneline(getprop(n, "context"));
	case Qnctl:
		return helptext(n);
	case Qtree:
		return printtree(n);
	case Qbounds:
		if((s = uibounds(n)) != nil){
			t = oneline(s);
			free(s);
			return t;
		}
		return oneline(n->bounds);
	case Qprop:
		s = getprop(n, a->name);
		if(s == nil && (pd = pdesc(typebyname(n->type), a->name)) != nil)
			s = pd->dflt;
		return oneline(s);
	case Qtitle:
		return oneline(w->title);
	case Qinner:
		return smprint("%d %d\n", w->w, w->h);
	case Qstate:
		return oneline(w->shown ? w->state : "hidden");
	case Qfocus:
		x = nodebyid(w->focus);
		if(x == nil)
			return estrdup("");
		s = relpath(x);
		t = oneline(s);
		free(s);
		return t;
	}
	*err = "permission denied";
	return nil;
}

static void
fsread(Req *r)
{
	Faux *a;
	Dirl *l;
	char *e;
	long off, n;

	a = r->fid->aux;
	if(a->sub != nil){
		subread(a->sub, r);
		return;
	}
	if(quitting){
		respond(r, "app gone");
		return;
	}
	if(isdir(a->k)){
		l = dirlist(a);
		dirread9p(r, dirgen, l);
		freedirl(l);
		respond(r, nil);
		return;
	}
	off = r->ifcall.offset;
	if(off == 0 || a->rbuf == nil){
		free(a->rbuf);
		a->rbuf = content(a, &a->nrbuf, &e);
		if(a->rbuf == nil){
			respond(r, e);
			return;
		}
		if(a->nrbuf < 0)
			a->nrbuf = strlen(a->rbuf);
	}
	n = r->ifcall.count;
	if(off >= a->nrbuf)
		n = 0;
	else if(off + n > a->nrbuf)
		n = a->nrbuf - off;
	memmove(r->ofcall.data, a->rbuf + off, n);
	r->ofcall.count = n;
	respond(r, nil);
}

/*
 * Opening.
 */
static int
buffered(Faux *a)
{
	Node *n;
	Pdesc *p;

	switch(a->k){
	case Qprop:
		/* text, bytes and data apply at close; the rest as written */
		if((n = nodebyid(a->node)) == nil || (p = pdesc(typebyname(n->type), a->name)) == nil)
			return 1;
		/* except a value that names children: accordion's open items */
		if(strcmp(a->name, "value") == 0 && strcmp(p->form, "text") == 0
		&& typebyname(n->type) != nil && typebyname(n->type)->child == 2)
			return 0;
		return strcmp(p->form, "text") == 0 || strcmp(p->form, "bytes") == 0 || strcmp(p->form, "data") == 0;
	case Qtree: case Qkeys: case Qtokens: case Qclip:
		return 1;
	case Qmenu:
		return strcmp(a->name, "bar") != 0;
	}
	return 0;
}

static int
writable(int k)
{
	switch(k){
	case Qctl: case Qkeys: case Qmenu: case Qtokens: case Qtname: case Qpref:
	case Qmode: case Qclip: case Qinject: case Qtype: case Qstyle: case Qorder:
	case Qtag: case Qon: case Qcontext: case Qnctl: case Qtree: case Qprop:
	case Qtitle: case Qstate: case Qfocus:
		return 1;
	}
	return 0;
}

static char*
apppath(Faux *a)
{
	switch(a->k){
	case Qmenu:
		return smprint("menu/%s", a->name);
	case Qtokens: case Qtname: case Qpref: case Qmode:
		return smprint("theme/%s", tabname(a->k));
	case Qinject:
		return estrdup("debug/inject");
	case Qwords:
		return estrdup("style/words");
	}
	return estrdup(tabname(a->k));
}

static void
setpaths(Faux *a)
{
	Node *n, *x;
	char *p;

	free(a->upath);
	free(a->wname);
	a->wname = nil;
	if(a->k < Qnode){
		a->upath = apppath(a);
		return;
	}
	n = nodebyid(a->node);
	if(n == nil){
		a->upath = estrdup("?");
		return;
	}
	for(x = n; x->parent != nil; x = x->parent)
		;
	a->wname = estrdup(x->win->name);
	p = uipath(n);
	if(a->k == Qnode)
		a->upath = p;
	else{
		a->upath = smprint("%s/%s", p, a->k == Qprop ? a->name : tabname(a->k));
		free(p);
	}
}

static void
fsopen(Req *r)
{
	Faux *a;
	Node *n;
	int m;

	a = r->fid->aux;
	if(quitting){
		respond(r, "app gone");
		return;
	}
	n = nil;
	if(a->k >= Qnode && (n = nodebyid(a->node)) == nil){
		respond(r, "gone");
		return;
	}
	m = r->ifcall.mode & 3;
	if((m == OWRITE || m == ORDWR || (r->ifcall.mode & OTRUNC)) && !writable(a->k)){
		respond(r, "permission denied");
		return;
	}
	a->omode = r->ifcall.mode;
	a->trunc = (r->ifcall.mode & OTRUNC) != 0;
	setpaths(a);
	if(a->k == Qevents)
		a->sub = newsub(Sapp, nil, 0);
	else if(a->k == Qnevents){
		if(n->parent == nil)
			a->sub = newsub(Swin, a->wname, 0);
		else
			a->sub = newsub(Snode, a->wname, n->id);
	}
	respond(r, nil);
}

static void
fscreate(Req *r)
{
	Faux *a;
	Node *n, *c;
	char *e, *name;

	a = r->fid->aux;
	name = r->ifcall.name;
	if(quitting){
		respond(r, "app gone");
		return;
	}
	if(a->k == Qnode && (r->ifcall.perm & DMDIR)){
		if((n = nodebyid(a->node)) == nil){
			respond(r, "gone");
			return;
		}
		if(kidbyname(n, name) != nil){
			respond(r, "in use");
			return;
		}
		if(isnodefile(name, 0) || name[0] == '@' || name[0] == '.'){
			respond(r, "in use: a node file");
			return;
		}
		if((e = canhold(n, "div", name)) != nil){
			respond(r, e);
			return;
		}
		c = newnode(n, name, "div");
		e = uipath(c);
		trace("mkdir path=%s", e);
		free(e);
		a->node = c->id;
		a->k = Qnode;
		a->omode = r->ifcall.mode;
		setpaths(a);
		r->fid->qid = qidof(a);
		r->ofcall.qid = r->fid->qid;
		respond(r, nil);
		sweep();
		return;
	}
	if(a->k == Qmenudir && !(r->ifcall.perm & DMDIR)){
		if(strcmp(name, "bar") != 0 && menuidx(name) < 0){
			app->menu = erealloc(app->menu, (app->nmenu+1)*sizeof(Menu));
			app->menu[app->nmenu].name = estrdup(name);
			app->menu[app->nmenu].text = estrdup("");
			app->nmenu++;
		}
		a->k = Qmenu;
		a->name = estrdup(name);
		a->omode = r->ifcall.mode;
		a->trunc = 1;
		setpaths(a);
		r->fid->qid = qidof(a);
		r->ofcall.qid = r->fid->qid;
		respond(r, nil);
		return;
	}
	respond(r, "permission denied");
}

static void
fsremove(Req *r)
{
	Faux *a;
	Node *n;
	char *e;
	int i;

	a = r->fid->aux;
	if(quitting){
		respond(r, "app gone");
		return;
	}
	if(a->k == Qnode){
		n = nodebyid(a->node);
		if(n == nil){
			respond(r, "gone");
			return;
		}
		if(n->parent == nil){
			respond(r, "permission denied");
			return;
		}
		e = uipath(n);
		trace("remove path=%s", e);
		free(e);
		delnode(n);
		respond(r, nil);
		sweep();
		return;
	}
	if(a->k == Qmenu && (i = menuidx(a->name)) >= 0){
		free(app->menu[i].name);
		free(app->menu[i].text);
		app->menu[i] = app->menu[--app->nmenu];
		respond(r, nil);
		return;
	}
	respond(r, "permission denied");
}

/*
 * Writing.
 */
static void
tracewrite(Faux *a, char *v, long n)
{
	Node *x;
	char *q;

	if(n < 0)
		n = strlen(v);
	if(n > 0 && v[n-1] == '\n' && a->k != Qclip)
		n--;
	x = a->k == Qprop ? nodebyid(a->node) : nil;
	if(x != nil && strcmp(a->name, "value") == 0 && issecret(x))
		trace("write path=%s value=<secret>", a->upath);
	else if(n > 64 || memchr(v, '\n', n) != nil || memchr(v, 0, n) != nil)
		trace("write path=%s len=%ld", a->upath, n);
	else{
		q = emalloc(n+1);
		memmove(q, v, n);
		q[n] = 0;
		v = quoteval(q);
		trace("write path=%s value=%s", a->upath, v);
		free(v);
		free(q);
	}
}

static void
failed(Faux *a, char *err)
{
	Node *n;
	char *q;

	q = quoteval(err);
	trace("error path=%s err=%s", a->upath, q);
	free(q);
	n = a->k >= Qnode ? nodebyid(a->node) : nil;
	emiterr(a->upath, err, n, nil, a->wname);
}

/* a write to a file that applies at once */
static char*
applynow(Faux *a, char *v)
{
	Node *n;
	Win *w;
	char *e, *buf, *f[256], *old;
	int i, nf, *ord;
	Node *c;

	n = nil;
	w = nil;
	if(a->k >= Qnode){
		if((n = nodebyid(a->node)) == nil)
			return "gone";
		for(c = n; c->parent != nil; c = c->parent)
			;
		w = c->win;
	}
	switch(a->k){
	case Qtype:
		return settype(n, v);
	case Qstyle:
		if((e = checkstyle(v)) != nil)
			return e;
		free(n->style);
		n->style = estrdup(v);
		return nil;
	case Qorder:
		buf = estrdup(v);
		nf = getfields(buf, f, nelem(f), 1, " \t\n");
		ord = emalloc((nf+1)*sizeof(int));
		for(i = 0; i < nf; i++){
			if(f[i][0] == 0){
				nf = i;
				break;
			}
			if((c = kidbyname(n, f[i])) == nil){
				e = smprint("not found: %s", f[i]);
				free(buf);
				free(ord);
				return e;
			}
			ord[i] = c->id;
		}
		free(buf);
		free(n->ord);
		n->ord = ord;
		n->nord = nf;
		return nil;
	case Qtag: case Qcontext:
		setprop(n, tabname(a->k), v, 0);
		return nil;
	case Qon:
		if((e = checkon(v)) != nil)
			return e;
		setprop(n, "on", v, 0);
		return nil;
	case Qtitle:
		return setwinprop(w, "title", v);
	case Qfocus:
		return setfocus(w, v, 1);
	case Qstate:
		if(strcmp(v, "normal") != 0 && strcmp(v, "min") != 0 && strcmp(v, "max") != 0
		&& strcmp(v, "full") != 0 && strcmp(v, "hidden") != 0)
			return smprint("bad value: state %s", v);
		free(w->state);
		w->state = estrdup(v);
		w->shown = strcmp(v, "hidden") != 0;
		buf = smprint("value=%s via=ctl", v);
		emitnode("state", w, w->root, buf, 1);
		free(buf);
		return nil;
	case Qpref: case Qmode:
		if(strcmp(v, "system") != 0 && strcmp(v, "light") != 0 && strcmp(v, "dark") != 0)
			return smprint("bad value: %s", v);
		old = estrdup(effmode());
		free(app->pref);
		app->pref = estrdup(v);
		if(strcmp(old, effmode()) != 0){
			buf = smprint("mode=%s via=ctl", effmode());
			emit("theme", "-", "-", buf, nil, nil, 0, 1);
			free(buf);
		}
		free(old);
		return nil;
	case Qtname:
		if(strcmp(v, "default") != 0)
			return smprint("not found: theme %s", v);
		return nil;
	case Qprop:
		return setnodeprop(n, a->name, v);
	case Qmenu:	/* bar */
		free(app->bar);
		app->bar = estrdup(v);
		return nil;
	}
	return "permission denied";
}

/* a write to a file that applies at close */
static char*
applyclose(Faux *a)
{
	Node *n;
	char *v, *e;

	switch(a->k){
	case Qprop:
		if((n = nodebyid(a->node)) == nil)
			return nil;
		v = trim1(a->buf != nil ? a->buf : "", a->nbuf);
		e = setnodeprop(n, a->name, v);
		free(v);
		return e;
	case Qtree:
		if((n = nodebyid(a->node)) == nil)
			return nil;
		v = trim1(a->buf != nil ? a->buf : "", 0);
		free(v);
		v = emalloc(a->nbuf+1);
		memmove(v, a->buf, a->nbuf);
		v[a->nbuf] = 0;
		e = settree(n, v);
		free(v);
		return e;
	case Qkeys: case Qtokens: case Qmenu:
		v = emalloc(a->nbuf+1);
		memmove(v, a->buf, a->nbuf);
		v[a->nbuf] = 0;
		if(a->k == Qkeys)
			e = setkeys(v);
		else if(a->k == Qtokens)
			e = settokens(v);
		else
			e = setmenu(a->name, v);
		free(v);
		return e;
	case Qclip:
		free(clip);
		clip = emalloc(a->nbuf+1);
		memmove(clip, a->buf, a->nbuf);
		nclip = a->nbuf;
		return nil;
	}
	return nil;
}

static void
addbuf(Faux *a, char *data, long n, vlong off)
{
	USED(off);
	a->buf = erealloc(a->buf, a->nbuf+n+1);
	memmove(a->buf+a->nbuf, data, n);
	a->nbuf += n;
	a->buf[a->nbuf] = 0;
}

/* run the complete lines written to /debug/inject */
static char*
runinject(Faux *a)
{
	char *p, *nl, *e, *line;
	long done;

	e = nil;
	p = a->buf;
	while(e == nil && (nl = memchr(p, '\n', a->nbuf - (p - a->buf))) != nil){
		line = emalloc(nl-p+1);
		memmove(line, p, nl-p);
		line[nl-p] = 0;
		e = injectline(line);
		free(line);
		p = nl+1;
		sweep();
	}
	done = p - a->buf;
	memmove(a->buf, p, a->nbuf - done);
	a->nbuf -= done;
	if(e != nil)
		a->nbuf = 0;
	return e;
}

static void
fswrite(Req *r)
{
	Faux *a;
	Node *n;
	char *e, *v, *text;
	long cnt;

	a = r->fid->aux;
	cnt = r->ifcall.count;
	n = nil;
	if(quitting){
		respond(r, "app gone");
		return;
	}
	if(a->k >= Qnode)
		n = nodebyid(a->node);
	if(a->k >= Qnode && n == nil){
		failed(a, "gone");
		respond(r, "gone");
		return;
	}
	a->wrote = 1;
	r->ofcall.count = cnt;
	e = nil;
	if(buffered(a)){
		addbuf(a, r->ifcall.data, cnt, r->ifcall.offset);
		respond(r, nil);
		return;
	}
	if(a->k == Qinject){
		addbuf(a, r->ifcall.data, cnt, r->ifcall.offset);
		e = runinject(a);
		respond(r, e);
		sweep();
		return;
	}
	if(a->k == Qctl || a->k == Qnctl){
		addbuf(a, r->ifcall.data, cnt, r->ifcall.offset);
		if(a->nbuf > 0 && a->buf[a->nbuf-1] == '\n' && cnt < r->srv->msize - IOHDRSZ){
			text = a->buf;
			a->buf = nil;
			a->nbuf = 0;
			e = ctlrun(a->k == Qctl ? nil : nodebyid(a->node), text, a->upath);
			free(text);
			if(e != nil)
				failed(a, e);
		}
		respond(r, e);
		sweep();
		return;
	}
	v = trim1(r->ifcall.data, cnt);
	e = applynow(a, v);
	if(e != nil)
		failed(a, e);
	else
		tracewrite(a, v, -1);
	free(v);
	respond(r, e);
	sweep();
}

static void
fsdestroyfid(Fid *fid)
{
	Faux *a;
	char *e, *text;

	a = fid->aux;
	if(a == nil)
		return;
	if(a->sub != nil){
		freesub(a->sub);
		a->sub = nil;
	}
	if(a->omode != -1 && !quitting && ((a->omode&3) == OWRITE || (a->omode&3) == ORDWR || a->trunc)){
		e = nil;
		if(buffered(a)){
			if(a->wrote || a->trunc)
				if((e = applyclose(a)) == nil)
					tracewrite(a, a->buf != nil ? a->buf : "", a->nbuf);
		}else if(a->k == Qctl || a->k == Qnctl){
			if(a->nbuf > 0){
				text = emalloc(a->nbuf+1);
				memmove(text, a->buf, a->nbuf);
				text[a->nbuf] = 0;
				if(a->k == Qctl || nodebyid(a->node) != nil)
					e = ctlrun(a->k == Qctl ? nil : nodebyid(a->node), text, a->upath);
				free(text);
			}
		}else if(a->k == Qinject){
			if(a->nbuf > 0){
				addbuf(a, "\n", 1, 0);
				runinject(a);
			}
		}else if(a->trunc && !a->wrote && (a->k < Qnode || nodebyid(a->node) != nil))
			e = applynow(a, "");
		if(e != nil)
			failed(a, e);
		sweep();
	}
	freeaux(a);
	fid->aux = nil;
}

static void
fsflush(Req *r)
{
	subflush(r);
}

static void
fswstat(Req *r)
{
	respond(r, nil);
}

/*
 * The model lock. uxkit serves two kinds of connection: the author's,
 * mounted at $ui, and one for each registration with uxd. lib9p locks
 * each connection apart (Srv.slock), so the model has a lock of its
 * own, taken around every request of either, and by the UI thread. It
 * nests within a thread: a reply inside a request can destroy a fid,
 * and that takes it again.
 */
static QLock	modellk;
static int	mowner = -1;
static int	mdepth;

void
lockmodel(void)
{
	if(mowner == threadid()){
		mdepth++;
		return;
	}
	qlock(&modellk);
	mowner = threadid();
	mdepth = 1;
}

void
unlockmodel(void)
{
	if(--mdepth > 0)
		return;
	mowner = -1;
	qunlock(&modellk);
}

static void
lfsattach(Req *r)
{
	lockmodel();
	fsattach(r);
	unlockmodel();
}

static void
lfsopen(Req *r)
{
	lockmodel();
	fsopen(r);
	unlockmodel();
}

static void
lfscreate(Req *r)
{
	lockmodel();
	fscreate(r);
	unlockmodel();
}

static void
lfsread(Req *r)
{
	lockmodel();
	fsread(r);
	unlockmodel();
}

static void
lfswrite(Req *r)
{
	lockmodel();
	fswrite(r);
	unlockmodel();
}

static void
lfsremove(Req *r)
{
	lockmodel();
	fsremove(r);
	unlockmodel();
}

static void
lfsstat(Req *r)
{
	lockmodel();
	fsstat(r);
	unlockmodel();
}

static void
lfswstat(Req *r)
{
	lockmodel();
	fswstat(r);
	unlockmodel();
}

static void
lfsflush(Req *r)
{
	lockmodel();
	fsflush(r);
	unlockmodel();
}


static char*
lfswalk1(Fid *fid, char *name, Qid *qid)
{
	char *e;

	lockmodel();
	e = fswalk1(fid, name, qid);
	unlockmodel();
	return e;
}

static char*
lfsclone(Fid *old, Fid *new)
{
	char *e;

	lockmodel();
	e = fsclone(old, new);
	unlockmodel();
	return e;
}

static void
lfsdestroyfid(Fid *fid)
{
	lockmodel();
	fsdestroyfid(fid);
	unlockmodel();
}

static void	poster(void*);

static void
fsstart(Srv*)
{
	startresponder();
	proccreate(poster, nil, 32*1024);
}

static Srv fs = {
	.start = fsstart,
	.attach = lfsattach,
	.walk1 = lfswalk1,
	.clone = lfsclone,
	.open = lfsopen,
	.create = lfscreate,
	.read = lfsread,
	.write = lfswrite,
	.remove = lfsremove,
	.stat = lfsstat,
	.wstat = lfswstat,
	.flush = lfsflush,
	.destroyfid = lfsdestroyfid,
};

/*
 * The connection uxd takes at registration (policy.md, Registration):
 * the same tree, served on a pipe posted as /srv/NAME. uxd applies the
 * rights. The pipe is made by the poster proc, which shares the author
 * connection's fd table, so the responder can answer either.
 */
static Channel	*postc, *postr;
static Srv	pubtmpl;	/* fs before it runs: no pools, buffers or locks of its own yet */

static void
pubfree(Srv *s)
{
	close(s->infd);
	free(s);
}

static void
pubproc(void *v)
{
	threadsetname("uxkit public");
	srv(v);
}

static void forker(void (*)(void*), void*, int);

static void
poster(void*)
{
	char *name, path[160];
	int fd[2], cfd;
	Srv *s;

	threadsetname("uxkit poster");
	for(;;){
		name = recvp(postc);
		snprint(path, sizeof path, "/srv/%s", name);
		if(pipe(fd) < 0){
			sendul(postr, -1);
			continue;
		}
		if((cfd = create(path, OWRITE, 0600)) < 0 || fprint(cfd, "%d", fd[0]) < 0){
			if(cfd >= 0){
				close(cfd);
				remove(path);
			}
			close(fd[0]);
			close(fd[1]);
			sendul(postr, -1);
			continue;
		}
		close(cfd);
		close(fd[0]);
		s = emalloc(sizeof *s);
		*s = pubtmpl;
		s->infd = s->outfd = fd[1];
		s->forker = forker;
		s->free = pubfree;
		proccreate(pubproc, s, 32*1024);
		sendul(postr, 0);
	}
}

/* libux's registration posts the tree with this (uxpublish) */
static int
pubpost(char *name)
{
	sendp(postc, name);
	return recvul(postr) == 0 ? 0 : -1;
}

static void
forker(void (*fn)(void*), void *arg, int flag)
{
	procrfork(fn, arg, 256*1024, flag);
}

static void
usage(void)
{
	fprint(2, "usage: uxkit [-dD] [-m mtpt] [-n name] [-s srvname] cmd [arg...]\n");
	threadexitsall("usage");
}

/* run the client, and exit with its status */
static void
client(void *v)
{
	char **argv, *cmd;
	int pid;
	Waitmsg *w;

	argv = v;
	/*
	 * Everything the child needs is made before the fork. The child gets
	 * a copy of the heap taken while the server procs may be halfway
	 * through changing it (their malloc lock held, a free list being
	 * relinked), so it mustn't malloc: it would hang or find the heap
	 * corrupt. That was uxkit's rare hang and heap crash at start.
	 */
	cmd = argv[0];
	if(cmd[0] != '/' && strncmp(cmd, "./", 2) != 0 && strncmp(cmd, "../", 3) != 0)
		cmd = smprint("/bin/%s", cmd);
	switch(pid = fork()){
	case -1:
		sysfatal("fork: %r");
	case 0:
		exec(cmd, argv);
		fprint(2, "uxkit: exec %s: %r\n", cmd);
		_exits("exec failed");
	}
	while((w = wait()) != nil){
		if(w->pid == pid)
			threadexitsall(w->msg[0] != 0 ? w->msg : nil);
		free(w);
	}
	threadexitsall("no client");
}

int mainstacksize = 64*1024;

static char *appname;

/*
 * The app's name at uxd, [a-z][a-z0-9-]*: the client's, or its
 * script's when the client is a shell (rc tools.rc is tools).
 */
static char*
nameof1(char **argv)
{
	char *s, *p, *q;

	s = argv[0];
	if((p = strrchr(s, '/')) != nil)
		s = p+1;
	if((strcmp(s, "rc") == 0 || strcmp(s, "sh") == 0) && argv[1] != nil){
		s = argv[1];
		if((p = strrchr(s, '/')) != nil)
			s = p+1;
	}
	s = estrdup(s);
	if((p = strchr(s, '.')) != nil)
		*p = 0;
	for(p = q = s; *p; p++){
		if(*p >= 'A' && *p <= 'Z')
			*p += 'a'-'A';
		if(*p >= 'a' && *p <= 'z' || *p >= '0' && *p <= '9' && q > s || *p == '-' && q > s)
			*q++ = *p;
		else if(q > s)
			*q++ = '-';
	}
	*q = 0;
	if(*s == 0){
		free(s);
		s = estrdup("uitree");
	}
	return s;
}

/*
 * uxkitdebug=1 keeps a proc that fails an assertion (the heap's among
 * them) alive and asleep, so acid can read its stack.
 */
extern void (*__assert)(char*);

static void
stayassert(char *s)
{
	fprint(2, "uxkit: assertion failed: %s, pid %d\n", s, getpid());
	for(;;)
		sleep(1000);
}

void
threadmain(int argc, char **argv)
{
	char *pe;

	if((pe = getenv("uxkitdebug")) != nil){
		__assert = stayassert;
		free(pe);
	}
	ARGBEGIN{
	case 'd':
		drawing = 1;
		break;
	case 'D':
		chatty9p++;
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 'n':
		appname = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc < 1)
		usage();
	if(appname == nil)
		appname = nameof1(argv);
	initapp();
	postc = chancreate(sizeof(char*), 0);
	postr = chancreate(sizeof(ulong), 0);
	if(drawing)
		uistart(appname, pubpost);
	if(access(mtpt, AEXIST) < 0)
		close(create(mtpt, OREAD, DMDIR|0777));
	fs.forker = forker;
	pubtmpl = fs;
	pubtmpl.start = nil;
	threadpostmountsrv(&fs, srvname, mtpt, MREPL|MCREATE);
	putenv("ui", mtpt);
	if(!drawing)
		client(argv);
	proccreate(client, argv, 64*1024);
	uirun();
	threadexitsall(nil);
}

