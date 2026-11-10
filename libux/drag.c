/*
 * Drag and drop through rio's /dev/ux/drag (docs/protocols/devux.md,
 * "Drag and drop").
 *
 * One ctl fid (dragfd) is this app's source session and also carries
 * its accepts and dones. A reader proc follows drag/events: it accepts
 * or refuses for the drop-target controls under the pointer, queues
 * UXdrop and UXdragend for the UI thread, and answers fetches for our
 * own drags in procs of their own, so a target in this same app,
 * fetching from its handler on the UI thread, never waits on the UI
 * thread. Files offered with uxdragfile are read here, in the source's
 * namespace, and their bytes go through rio: no path leaves the app.
 *
 * Locks: uxlk for the model and the drop targets, draglk (taken after
 * uxlk when both are needed) for our drags and the drags seen.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <mouse.h>
#include <keyboard.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "ux.h"
#include "impl.h"

typedef struct Target Target;
typedef struct Seen Seen;
typedef struct Fetch Fetch;
typedef struct Dropctx Dropctx;

struct Uxdrag {
	char	*ops;
	char	*label;
	char	**type;	/* types with their bytes, put at start */
	char	**data;
	long	*ndata;
	int	ntype;
	char	**path;	/* the "file" offer, served on demand */
	int	npath;
	int	id;
	int	winid;
	ulong	srcoid;
	Uxdrag	*next;
};

typedef struct Icondrag Icondrag;
struct Icondrag {
	ulong	oid;
	Uxdragfn	*fn;
	void	*arg;
	Icondrag	*next;
};

struct Target {
	ulong	oid;
	char	*types;
	char	*ops;	/* nil: any */
	Target	*next;
};

/* a drag seen on drag/events */
struct Seen {
	int	id;
	char	*app;
	char	*types;
	char	*ops;
	Seen	*next;
};

struct Fetch {
	int	id;
	char	*type;
	int	item;
	int	req;
};

struct Dropctx {
	Uxfn	*fn;
	void	*arg;
	int	id;
	char	*op;
	char	*resop;
	char	*reserr;
};

static QLock	draglk;
static Uxdrag	*srcs;
static Seen	*seen;
static Target	*targets;	/* uxlk */
static Icondrag	*icondrags;	/* uxlk */
static int	dragfd = -1;
static Dropctx	*curdrop;	/* the drop whose handler is running (UI thread) */

/* the reader's last answer, so it writes only changes */
static int	lastid, lastwin, lastok;
static ulong	lastoid;

/*
 * Lists: comma-separated words.
 */
static int
inlist(char *list, char *word)
{
	char *p, *e;
	int n;

	n = strlen(word);
	for(p = list; p != nil && *p; p = e){
		e = strchr(p, ',');
		if(e == nil)
			e = p+strlen(p);
		if(e-p == n && strncmp(p, word, n) == 0)
			return 1;
		if(*e == ',')
			e++;
	}
	return 0;
}

/* the first word of src that's also in want (nil want: any), malloc'd */
static char*
firstcommon(char *src, char *want)
{
	char *s, *f[32];
	int i, n;

	s = estrdup(src);
	n = getfields(s, f, nelem(f), 0, ",");
	for(i = 0; i < n; i++)
		if(want == nil || inlist(want, f[i])){
			f[i] = estrdup(f[i]);
			free(s);
			return f[i];
		}
	free(s);
	return nil;
}

static char*
attr(char **f, int nf, char *name)
{
	int i, n;

	n = strlen(name);
	for(i = 0; i < nf; i++)
		if(strncmp(f[i], name, n) == 0 && f[i][n] == '=')
			return f[i]+n+1;
	return nil;
}

static int
ctlwrite(char *fmt, ...)
{
	va_list arg;
	char *s;
	int n;

	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	n = write(dragfd, s, strlen(s));
	free(s);
	return n < 0 ? -1 : 0;
}

static void	reader(void*);

/* open the session and start following events; uxlk held or not */
static int
ensure(void)
{
	static QLock lk;
	int efd, *p;

	qlock(&lk);
	if(dragfd >= 0){
		qunlock(&lk);
		return 0;
	}
	efd = open("/dev/ux/drag/events", OREAD);
	if(efd < 0){
		qunlock(&lk);
		werrstr("no drag and drop: %r");
		return -1;
	}
	dragfd = open("/dev/ux/drag/ctl", ORDWR);
	if(dragfd < 0){
		close(efd);
		qunlock(&lk);
		werrstr("no drag and drop: %r");
		return -1;
	}
	p = emalloc(sizeof *p);
	*p = efd;
	proccreate(reader, p, 32*1024);
	qunlock(&lk);
	return 0;
}

/*
 * Sources
 */
Uxdrag*
uxdragnew(char *ops, char *label)
{
	Uxdrag *d;

	d = emalloc(sizeof *d);
	d->ops = estrdup(ops != nil ? ops : "copy");
	d->label = label != nil ? estrdup(label) : nil;
	return d;
}

void
uxdragops(Uxdrag *d, char *ops)
{
	free(d->ops);
	d->ops = estrdup(ops != nil ? ops : "copy");
}

void
uxdragdata(Uxdrag *d, char *type, void *data, long n)
{
	d->type = erealloc(d->type, (d->ntype+1)*sizeof d->type[0]);
	d->data = erealloc(d->data, (d->ntype+1)*sizeof d->data[0]);
	d->ndata = erealloc(d->ndata, (d->ntype+1)*sizeof d->ndata[0]);
	d->type[d->ntype] = estrdup(type);
	d->data[d->ntype] = emalloc(n+1);
	memmove(d->data[d->ntype], data, n);
	d->ndata[d->ntype] = n;
	d->ntype++;
}

void
uxdragfile(Uxdrag *d, char *path)
{
	d->path = erealloc(d->path, (d->npath+1)*sizeof d->path[0]);
	d->path[d->npath++] = estrdup(path);
}

static void
dragfree(Uxdrag *d)
{
	int i;

	for(i = 0; i < d->ntype; i++){
		free(d->type[i]);
		free(d->data[i]);
	}
	for(i = 0; i < d->npath; i++)
		free(d->path[i]);
	free(d->type);
	free(d->data);
	free(d->ndata);
	free(d->path);
	free(d->ops);
	free(d->label);
	free(d);
}

/* the id of the drag in flight from window winid, from drag/state */
static int
flyingfrom(int winid)
{
	char *buf, *line, *e, *f[32], *v;
	int fd, n, nf, id, off;
	enum { Nbuf = 16*1024 };

	/* on the UI thread, whose stack is small: buffers come from the heap */
	fd = open("/dev/ux/drag/state", OREAD);
	if(fd < 0)
		return -1;
	buf = emalloc(Nbuf);
	off = 0;
	while(off < Nbuf-1 && (n = read(fd, buf+off, Nbuf-1-off)) > 0)
		off += n;
	close(fd);
	buf[off] = 0;
	id = -1;
	for(line = buf; (e = strchr(line, '\n')) != nil; line = e+1){
		*e = 0;
		nf = tokenize(line, f, nelem(f));
		v = attr(f, nf, "win");
		if(nf > 0 && strcmp(f[0], "drag") == 0 && v != nil && atoi(v) == winid
		&& (v = attr(f, nf, "state")) != nil && strcmp(v, "over") == 0)
			id = atoi(attr(f, nf, "id"));
	}
	free(buf);
	return id;
}

static int
put(int id, char *type, char *data, long n)
{
	char *s;
	int fd, ok;

	fd = open("/dev/ux/drag/data", OWRITE);
	if(fd < 0)
		return -1;
	s = smprint("put %d %s\n", id, type);
	ok = write(fd, s, strlen(s)) == strlen(s);
	free(s);
	while(ok && n > 0){
		s = data;
		ok = write(fd, s, n > 8192 ? 8192 : n) > 0;
		data += n > 8192 ? 8192 : n;
		n -= n > 8192 ? 8192 : n;
	}
	close(fd);
	return ok ? 0 : -1;
}

/* start the drag; uxlk held. d belongs to libux from here on */
int
_dragstart(Uxdrag *d, Uxctl *src)
{
	Fmt f;
	char *s, *l;
	int i, n, r;

	if(ensure() < 0){
		dragfree(d);
		return -1;
	}
	if(d->ntype == 0 && d->npath == 0){
		dragfree(d);
		werrstr("bad value: nothing to drag");
		return -1;
	}
	/* the file offer first, then the others in the order given */
	fmtstrinit(&f);
	n = 0;
	if(d->npath > 0){
		fmtprint(&f, "file");
		n++;
	}
	for(i = 0; i < d->ntype; i++)
		if(strcmp(d->type[i], "file") != 0)
			fmtprint(&f, "%s%s", n++ ? "," : "", d->type[i]);
	s = fmtstrflush(&f);
	l = d->label != nil ? _attr("label", d->label) : estrdup("");
	d->winid = src->w->winid;
	r = ctlwrite("start win=%d types=%s ops=%s %s\n", d->winid, s, d->ops, l);
	free(s);
	free(l);
	if(r < 0){
		dragfree(d);
		return -1;
	}
	d->id = flyingfrom(d->winid);
	if(d->id < 0){
		dragfree(d);
		werrstr("gone: the drag ended at once");
		return -1;
	}
	d->srcoid = src->oid;
	qlock(&draglk);
	d->next = srcs;
	srcs = d;
	qunlock(&draglk);
	for(i = 0; i < d->ntype; i++)
		put(d->id, d->type[i], d->data[i], d->ndata[i]);
	return 0;
}

int
uxdragstart(Uxdrag *d, Uxctl *src)
{
	int r;

	qlock(&uxlk);
	r = _dragstart(d, src);
	qunlock(&uxlk);
	return r;
}

static Uxdrag*
oursrc(int id)
{
	Uxdrag *d;

	for(d = srcs; d != nil; d = d->next)
		if(d->id == id)
			return d;
	return nil;
}

/*
 * Answering fetches for our drags
 */
static void
fail(int req, char *why)
{
	int fd;
	char *s, *q;

	fd = open("/dev/ux/drag/data", OWRITE);
	if(fd < 0)
		return;
	q = _quote(why);
	s = smprint("fail %d %s\n", req, q);
	write(fd, s, strlen(s));
	free(s);
	free(q);
	close(fd);
}

static int
replyopen(int req)
{
	int fd;
	char buf[32];

	fd = open("/dev/ux/drag/data", OWRITE);
	if(fd < 0)
		return -1;
	snprint(buf, sizeof buf, "reply %d\n", req);
	if(write(fd, buf, strlen(buf)) != strlen(buf)){
		close(fd);
		return -1;
	}
	return fd;
}

static char*
filelist(char **path, int n)
{
	Fmt f;
	Dir *d;
	char *a, *base;
	int i;

	fmtstrinit(&f);
	for(i = 0; i < n; i++){
		base = strrchr(path[i], '/');
		base = base != nil && base[1] != 0 ? base+1 : path[i];
		a = _attr("name", base);
		fmtprint(&f, "file %s", a);
		free(a);
		if((d = dirstat(path[i])) != nil){
			if(d->qid.type & QTDIR)
				fmtprint(&f, " dir=on");
			else
				fmtprint(&f, " size=%lld", d->length);
			fmtprint(&f, " mode=%luo", d->mode & 0777);
			free(d);
		}
		fmtprint(&f, "\n");
	}
	return fmtstrflush(&f);
}

static void
serve(void *v)
{
	Fetch *ft;
	Uxdrag *d;
	char **paths, *data, buf[8192];
	int i, n, fd, out, npath;
	long ndata;
	Dir *dir;

	ft = v;
	threadsetname("uxdragfetch");
	data = nil;
	ndata = 0;
	paths = nil;
	npath = 0;
	qlock(&draglk);
	d = oursrc(ft->id);
	if(d != nil){
		for(i = 0; i < d->ntype; i++)
			if(strcmp(d->type[i], ft->type) == 0 && ft->item < 0){
				data = emalloc(d->ndata[i]+1);
				memmove(data, d->data[i], d->ndata[i]);
				ndata = d->ndata[i];
			}
		if(strcmp(ft->type, "file") == 0){
			npath = d->npath;
			paths = emalloc((npath+1)*sizeof paths[0]);
			for(i = 0; i < npath; i++)
				paths[i] = estrdup(d->path[i]);
		}
	}
	qunlock(&draglk);

	if(data == nil && strcmp(ft->type, "file") == 0 && ft->item < 0 && paths != nil){
		data = filelist(paths, npath);
		ndata = strlen(data);
	}
	if(data != nil){
		if((out = replyopen(ft->req)) >= 0){
			for(i = 0; i < ndata; i += n){
				n = ndata-i > 8192 ? 8192 : ndata-i;
				if(write(out, data+i, n) != n)
					break;
			}
			close(out);
		}
	}else if(paths == nil || ft->item < 0)
		fail(ft->req, "not found: no such type");
	else if(ft->item >= npath)
		fail(ft->req, "bad value: no such item");
	else{
		dir = dirstat(paths[ft->item]);
		if(dir == nil){
			snprint(buf, sizeof buf, "%r");
			fail(ft->req, buf);
		}else if(dir->qid.type & QTDIR)
			fail(ft->req, "bad value: a folder has no contents");
		else if((fd = open(paths[ft->item], OREAD)) < 0){
			snprint(buf, sizeof buf, "%r");
			fail(ft->req, buf);
		}else{
			if((out = replyopen(ft->req)) >= 0){
				while((n = read(fd, buf, sizeof buf)) > 0)
					if(write(out, buf, n) != n)
						break;
				close(out);
			}
			close(fd);
		}
		free(dir);
	}
	free(data);
	for(i = 0; i < npath; i++)
		free(paths[i]);
	free(paths);
	free(ft->type);
	free(ft);
}

/*
 * Icon views: when selected icons are dragged out of the view, libux
 * (ctl.c's iconmouse, uxlk held) asks us to take the drag over.
 */
static Rectangle	localrect(Uxctl*);

static int
icondragout(Uxctl *c, Mouse*)
{
	Icondrag *g;
	Uxdrag *d;
	Fmt f;
	char **items, *s, *a;
	int i, n;

	for(g = icondrags; g != nil; g = g->next)
		if(g->oid == c->oid)
			break;
	items = emalloc((c->nicons+1)*sizeof items[0]);
	n = 0;
	fmtstrinit(&f);
	for(i = 0; i < c->nicons; i++)
		if(c->icons[i].sel){
			items[n++] = c->icons[i].id;
			a = _attr("label", c->icons[i].label != nil ? c->icons[i].label : "");
			fmtprint(&f, "icon id=%s %s\n", c->icons[i].id, a);
			free(a);
		}
	s = fmtstrflush(&f);
	if(n == 0){
		free(s);
		free(items);
		return 0;
	}
	d = uxdragnew("copy,move", n == 1 ? items[0] : nil);
	uxdragdata(d, "ux.icon", s, strlen(s));
	free(s);
	if(g != nil && g->fn != nil)
		g->fn(d, items, n, g->arg);
	free(items);
	return _dragstart(d, c) == 0;
}

void
uxicondrag(Uxctl *c, Uxdragfn *fn, void *arg)
{
	Icondrag *g;

	qlock(&uxlk);
	for(g = icondrags; g != nil; g = g->next)
		if(g->oid == c->oid)
			break;
	if(g == nil){
		g = emalloc(sizeof *g);
		g->oid = c->oid;
		g->next = icondrags;
		icondrags = g;
	}
	g->fn = fn;
	g->arg = arg;
	_icondragout = icondragout;
	qunlock(&uxlk);
}

/* the icon of an icon view at window-local pt, or nil; uxlk held */
static char*
iconat(Uxctl *c, Point pt)
{
	Rectangle r, ir;
	int i;

	if(strcmp(c->t->name, "icon-view") != 0)
		return nil;
	r = localrect(c);
	ir = Rect(-UXiconlarge/2, -UXiconlarge/2, UXiconlarge/2, UXiconviewcellh-UXiconlarge/2);
	for(i = c->nicons-1; i >= 0; i--)
		if(ptinrect(pt, rectaddpt(ir, subpt(addpt(r.min, c->icons[i].pt), c->origin))))
			return c->icons[i].id;
	return nil;
}

/*
 * Targets
 */
void
uxdroptypes(Uxctl *c, char *types, char *ops)
{
	Target *t, **l;

	qlock(&uxlk);
	for(l = &targets; (t = *l) != nil; l = &t->next)
		if(t->oid == c->oid){
			*l = t->next;
			free(t->types);
			free(t->ops);
			free(t);
			break;
		}
	if(types != nil && *types != 0){
		t = emalloc(sizeof *t);
		t->oid = c->oid;
		t->types = estrdup(types);
		t->ops = ops != nil ? estrdup(ops) : nil;
		t->next = targets;
		targets = t;
	}
	qunlock(&uxlk);
	ensure();
}

static Target*
targetof(Uxctl *c)
{
	Target *t;

	for(t = targets; t != nil; t = t->next)
		if(t->oid == c->oid)
			return t;
	return nil;
}

static Rectangle
localrect(Uxctl *c)
{
	Rectangle r;
	Uxctl *p;

	r = c->r;
	for(p = c->parent; p != nil; p = p->parent)
		if(strcmp(p->t->name, "scroll-view") == 0)
			r = rectsubpt(r, p->origin);
	return r;
}

/* the topmost drop target in w at pt; uxlk held */
static Uxctl*
dropat(Uxwin *w, Point pt, Target **tp)
{
	Uxctl *c;
	Target *t;
	int i;

	for(i = w->nctl-1; i >= 0; i--){
		c = w->ctl[i];
		if(c->dead || (c->state & (Cenabled|Cvisible)) != (Cenabled|Cvisible))
			continue;
		if((t = targetof(c)) == nil)
			continue;
		if(ptinrect(pt, localrect(c))){
			*tp = t;
			return c;
		}
	}
	return nil;
}

static Seen*
findseen(int id)
{
	Seen *s;

	for(s = seen; s != nil; s = s->next)
		if(s->id == id)
			return s;
	return nil;
}

static Point
parsept(char *s)
{
	Point p;
	char *c;

	p = ZP;
	if(s == nil)
		return p;
	p.x = atoi(s);
	if((c = strchr(s, ',')) != nil)
		p.y = atoi(c+1);
	return p;
}

/* enter or over one of our windows: accept or refuse for what's under pt */
static void
hover(int id, int win, Point pt, char *types, char *ops)
{
	Uxwin *w;
	Uxctl *c;
	Target *t;
	char *type, *op;
	ulong oid;

	type = op = nil;
	oid = 0;
	qlock(&uxlk);
	w = _findwin(win);
	if(w == nil){
		qunlock(&uxlk);
		return;
	}
	c = dropat(w, pt, &t);
	if(c != nil){
		type = firstcommon(types, t->types);
		op = firstcommon(ops, t->ops);
		if(type == nil || op == nil){
			free(type);
			free(op);
			type = op = nil;
		}else
			oid = c->oid;
	}
	qunlock(&uxlk);
	if(type != nil){
		if(!(lastid == id && lastwin == win && lastok && lastoid == oid))
			ctlwrite("accept %d win=%d type=%s op=%s\n", id, win, type, op);
		lastok = 1;
	}else{
		if(lastid == id && lastwin == win && lastok)
			ctlwrite("refuse %d win=%d\n", id, win);
		lastok = 0;
	}
	lastid = id;
	lastwin = win;
	lastoid = oid;
	free(type);
	free(op);
}

static void
dropnote(Uxev *e, void *v)
{
	Dropctx *x;
	char *err;

	x = v;
	curdrop = x;
	if(x->fn != nil)
		x->fn(e, x->arg);
	curdrop = nil;
	err = x->reserr != nil ? _attr("err", x->reserr) : estrdup("");
	ctlwrite("done %d op=%s %s\n", x->id, x->resop != nil ? x->resop : x->op, err);
	free(err);
	free(x->op);
	free(x->resop);
	free(x->reserr);
	free(x);
}

static void
kickui(void)
{
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
}

/* dropped on one of our windows */
static void
dropped(int id, int win, Point pt, char *type, char *op)
{
	Uxwin *w;
	Uxctl *c;
	Target *t;
	Dropctx *x;
	Uxev e;

	qlock(&uxlk);
	w = _findwin(win);
	c = w != nil ? dropat(w, pt, &t) : nil;
	if(c == nil){
		qunlock(&uxlk);
		ctlwrite("done %d op=%s err='nothing to drop on'\n", id, op);
		return;
	}
	x = emalloc(sizeof *x);
	x->fn = c->fn;
	x->arg = c->arg;
	x->id = id;
	x->op = estrdup(op);
	memset(&e, 0, sizeof e);
	e.type = UXdrop;
	e.via = UXui;
	e.win = w;
	e.ctl = c;
	e.text = type;
	e.action = op;
	e.pt = pt;
	e.drag = id;
	e.item = iconat(c, pt);
	_notify(dropnote, x, &e);
	qunlock(&uxlk);
	kickui();
}

/* one of our drags finished (op) or was cancelled (nil) */
static void
srcended(int id, char *op)
{
	Uxdrag *d;
	Obj *o;
	Uxctl *c;
	Uxev e;
	ulong oid;

	qlock(&uxlk);
	qlock(&draglk);
	d = oursrc(id);
	oid = d != nil ? d->srcoid : 0;
	qunlock(&draglk);
	o = oid != 0 ? _lookobj(oid) : nil;
	if(o != nil && o->kind == Octl && !o->dead){
		c = (Uxctl*)o;
		memset(&e, 0, sizeof e);
		e.type = UXdragend;
		e.via = UXui;
		e.win = c->w;
		e.ctl = c;
		e.text = op;
		e.drag = id;
		_notify(c->fn, c->arg, &e);
		kickui();
	}
	qunlock(&uxlk);
}

static void
handle(char *line)
{
	char *f[32], *ev, *v;
	int nf, id, win;
	Seen *s;
	Uxdrag *d, **l;
	Seen **sl;
	Fetch *ft;

	nf = tokenize(line, f, nelem(f));
	if(nf < 2)
		return;
	ev = f[1];
	v = attr(f, nf, "id");
	if(v == nil)
		return;
	id = atoi(v);
	v = attr(f, nf, "win");
	win = v != nil ? atoi(v) : 0;

	if(strcmp(ev, "start") == 0){
		s = emalloc(sizeof *s);
		s->id = id;
		s->app = attr(f, nf, "app") != nil ? estrdup(attr(f, nf, "app")) : nil;
		s->types = estrdup(attr(f, nf, "types") != nil ? attr(f, nf, "types") : "");
		s->ops = estrdup(attr(f, nf, "ops") != nil ? attr(f, nf, "ops") : "copy");
		qlock(&draglk);
		s->next = seen;
		seen = s;
		qunlock(&draglk);
	}else if(strcmp(ev, "enter") == 0 || strcmp(ev, "over") == 0){
		char *types, *ops;

		qlock(&draglk);
		s = findseen(id);
		types = estrdup(attr(f, nf, "types") != nil ? attr(f, nf, "types") : s != nil ? s->types : "");
		ops = estrdup(attr(f, nf, "ops") != nil ? attr(f, nf, "ops") : s != nil ? s->ops : "copy");
		qunlock(&draglk);
		if(ev[0] == 'e')
			lastok = 0;
		hover(id, win, parsept(attr(f, nf, "pt")), types, ops);
		free(types);
		free(ops);
	}else if(strcmp(ev, "leave") == 0){
		if(lastid == id && lastwin == win)
			lastwin = 0;
	}else if(strcmp(ev, "drop") == 0){
		qlock(&uxlk);
		v = _findwin(win) != nil ? "ours" : nil;
		qunlock(&uxlk);
		if(v != nil && attr(f, nf, "type") != nil && attr(f, nf, "op") != nil)
			dropped(id, win, parsept(attr(f, nf, "pt")), attr(f, nf, "type"), attr(f, nf, "op"));
	}else if(strcmp(ev, "fetch") == 0){
		qlock(&draglk);
		d = oursrc(id);
		qunlock(&draglk);
		if(d == nil || attr(f, nf, "req") == nil || attr(f, nf, "type") == nil)
			return;
		ft = emalloc(sizeof *ft);
		ft->id = id;
		ft->type = estrdup(attr(f, nf, "type"));
		ft->item = attr(f, nf, "item") != nil ? atoi(attr(f, nf, "item")) : -1;
		ft->req = atoi(attr(f, nf, "req"));
		proccreate(serve, ft, 32*1024);
	}else if(strcmp(ev, "done") == 0){
		srcended(id, attr(f, nf, "err") == nil ? attr(f, nf, "op") : nil);
	}else if(strcmp(ev, "cancel") == 0){
		srcended(id, nil);
	}else if(strcmp(ev, "end") == 0){
		qlock(&draglk);
		for(l = &srcs; (d = *l) != nil; l = &d->next)
			if(d->id == id){
				*l = d->next;
				dragfree(d);
				break;
			}
		for(sl = &seen; (s = *sl) != nil; sl = &s->next)
			if(s->id == id){
				*sl = s->next;
				free(s->app);
				free(s->types);
				free(s->ops);
				free(s);
				break;
			}
		qunlock(&draglk);
		if(lastid == id)
			lastid = lastwin = lastok = 0;
	}
}

static void
reader(void *v)
{
	char *buf, *p, *e;
	int fd, n, off;
	enum { Nbuf = 8192+1 };

	buf = emalloc(Nbuf);
	fd = *(int*)v;
	free(v);
	threadsetname("uxdrag");
	off = 0;
	while((n = read(fd, buf+off, Nbuf-1-off)) > 0){
		n += off;
		buf[n] = 0;
		for(p = buf; (e = strchr(p, '\n')) != nil; p = e+1){
			*e = 0;
			handle(p);
		}
		off = n - (p-buf);
		memmove(buf, p, off);
	}
	close(fd);
	free(buf);
}

/*
 * In a UXdrop handler: getting the payload through rio
 */
static int
fetchopen(Uxev *e, char *type, int item)
{
	char *s;
	int fd, r;

	if(e == nil || e->drag <= 0){
		werrstr("bad value: not a drop");
		return -1;
	}
	fd = open("/dev/ux/drag/data", ORDWR);
	if(fd < 0)
		return -1;
	if(item >= 0)
		s = smprint("fetch %d %s item=%d\n", e->drag, type, item);
	else
		s = smprint("fetch %d %s\n", e->drag, type);
	r = write(fd, s, strlen(s));
	free(s);
	if(r < 0){
		close(fd);
		return -1;
	}
	return fd;
}

char*
uxdropdata(Uxev *e, char *type, int item, long *np)
{
	char *buf;
	long n, m;
	int fd;

	if(np != nil)
		*np = 0;
	fd = fetchopen(e, type, item);
	if(fd < 0)
		return nil;
	n = 0;
	buf = emalloc(8192+1);
	while((m = read(fd, buf+n, 8192)) > 0){
		n += m;
		buf = erealloc(buf, n+8192+1);
	}
	close(fd);
	if(m < 0){
		free(buf);
		return nil;
	}
	buf[n] = 0;
	if(np != nil)
		*np = n;
	return buf;
}

int
uxdropfiles(Uxev *e, Uxdropfile **fp)
{
	char *list, *line, *nl, *f[16], *v;
	Uxdropfile *fs;
	int n, nf;

	*fp = nil;
	list = uxdropdata(e, "file", -1, nil);
	if(list == nil)
		return -1;
	fs = nil;
	n = 0;
	for(line = list; *line; line = nl){
		nl = strchr(line, '\n');
		if(nl != nil)
			*nl++ = 0;
		else
			nl = line+strlen(line);
		nf = tokenize(line, f, nelem(f));
		if(nf < 1 || strcmp(f[0], "file") != 0 || (v = attr(f, nf, "name")) == nil)
			continue;
		fs = erealloc(fs, (n+1)*sizeof fs[0]);
		memset(&fs[n], 0, sizeof fs[n]);
		fs[n].name = estrdup(v);
		fs[n].size = (v = attr(f, nf, "size")) != nil ? strtoll(v, nil, 10) : -1;
		fs[n].dir = (v = attr(f, nf, "dir")) != nil && strcmp(v, "on") == 0;
		fs[n].mode = (v = attr(f, nf, "mode")) != nil ? strtoul(v, nil, 8) : 0644;
		n++;
	}
	free(list);
	*fp = fs;
	return n;
}

void
uxdropfilesfree(Uxdropfile *fs, int n)
{
	int i;

	for(i = 0; i < n; i++)
		free(fs[i].name);
	free(fs);
}

int
uxdropsave(Uxev *e, int item, char *path, ulong mode)
{
	char *buf, err[ERRMAX];
	int fd, out, n;

	fd = fetchopen(e, "file", item);
	if(fd < 0)
		return -1;
	out = create(path, OWRITE|OEXCL, mode & 0777);
	if(out < 0){
		close(fd);
		return -1;
	}
	buf = emalloc(8192);
	while((n = read(fd, buf, 8192)) > 0)
		if(write(out, buf, n) != n){
			n = -1;
			break;
		}
	free(buf);
	close(fd);
	close(out);
	if(n < 0){
		rerrstr(err, sizeof err);
		remove(path);
		werrstr("%s", err);
		return -1;
	}
	return 0;
}

void
uxdropresult(Uxev*, char *op, char *err)
{
	if(curdrop == nil)
		return;
	free(curdrop->resop);
	free(curdrop->reserr);
	curdrop->resop = op != nil ? estrdup(op) : nil;
	curdrop->reserr = err != nil ? estrdup(err) : nil;
}

int
uxdragfromself(Uxev *e)
{
	int r;

	qlock(&draglk);
	r = e != nil && oursrc(e->drag) != nil;
	qunlock(&draglk);
	return r;
}

char**
uxdragpaths(Uxev *e, int *np)
{
	Uxdrag *d;
	char **p;
	int i;

	*np = 0;
	p = nil;
	qlock(&draglk);
	d = e != nil ? oursrc(e->drag) : nil;
	if(d != nil){
		p = emalloc((d->npath+1)*sizeof p[0]);
		for(i = 0; i < d->npath; i++)
			p[i] = estrdup(d->path[i]);
		*np = d->npath;
	}
	qunlock(&draglk);
	return p;
}
