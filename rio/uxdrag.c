/*
 * 9/UX: drag and drop, /dev/ux/drag (docs/protocols/devux.md, "Drag
 * and drop"). rio is the broker: while a drag is in flight it has the
 * pointer (mousethread calls uxdragmouse), tells targets what is over
 * them, and carries the payload from source to target through
 * drag/data, so no path crosses a namespace.
 *
 * Like the rest of ux*.c this runs among rio's threads in one proc:
 * no locks, and a blocked data read waits on its own channel.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include "dat.h"
#include "fns.h"
#include "ux.h"

enum
{
	Dover,
	Ddropped,

	Maxdrag	= 16*1024*1024,	/* payload bytes held per drag */
	Maxlist	= 32,		/* types or ops in a list */

	/* drag/data fid modes */
	Mnone = 0,
	Mput,
	Mreply,
	Mfetch,
};

static	Drag		*drags;	/* newest first */
static	int		dragseq;
static	int		reqseq;
static	Uxdata	*waiters;
static	int		tellsrc;	/* the source window hasn't seen its button go up */

static	Cursor	dragok;
static	Cursor	dragno;

/*
 * Cursors: an arrow with a box (dropping here works), and an arrow
 * with a bar (it doesn't).
 */
static char *okart[16] = {
	"XX..............",
	"X#X.............",
	"X##X............",
	"X###X...........",
	"X####X..........",
	"X#####X.........",
	"X######X........",
	"X###XXXXX.......",
	"X##XX.XXXXXXXXX.",
	"X#X...X#######X.",
	"XX....X###X###X.",
	"......X##XXX##X.",
	"......X###X###X.",
	"......X#######X.",
	"......XXXXXXXXX.",
	"................",
};
static char *noart[16] = {
	"XX..............",
	"X#X.............",
	"X##X............",
	"X###X...........",
	"X####X..........",
	"X#####X.........",
	"X######X........",
	"X###XXXXX.......",
	"X##XX...........",
	"X#X....XXXXXXXX.",
	"XX.....X######X.",
	"......XX######X.",
	"......X#######X.",
	"......XXXXXXXXX.",
	"................",
	"................",
};

/* X is black (set), # is white (clr), . is clear */
static void
mkcursor(Cursor *c, char **art)
{
	int x, y, i;

	memset(c, 0, sizeof *c);
	c->offset = Pt(0, 0);
	for(y=0; y<16; y++)
		for(x=0; x<16; x++){
			i = y*2 + x/8;
			switch(art[y][x]){
			case 'X':
				c->set[i] |= 0x80>>(x%8);
				break;
			case '#':
				c->clr[i] |= 0x80>>(x%8);
				break;
			}
		}
}

void
uxdraginit(void)
{
	mkcursor(&dragok, okart);
	mkcursor(&dragno, noart);
}

static Drag*
finddrag(int id)
{
	Drag *d;

	for(d=drags; d; d=d->next)
		if(d->id == id)
			return d;
	return nil;
}

static Drag*
flying(void)
{
	Drag *d;

	for(d=drags; d; d=d->next)
		if(d->state == Dover)
			return d;
	return nil;
}

int
uxdragging(void)
{
	return flying() != nil;
}

/* is word in the comma-separated list? */
static int
inlist(char *list, char *word)
{
	char *p, *e;
	int n;

	n = strlen(word);
	for(p=list; p!=nil && *p; p=e){
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

static char*
firstof(char *list)
{
	char *s, *p;

	s = estrdup(list);
	if((p = strchr(s, ',')) != nil)
		*p = 0;
	return s;
}

static int
isid(char *s)
{
	char *p;

	if(s == nil || *s == 0 || strlen(s) > 64)
		return 0;
	if(!(*s >= 'a' && *s <= 'z') && !(*s >= '0' && *s <= '9'))
		return 0;
	for(p=s; *p; p++)
		if(!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9')
		&& *p != '.' && *p != '_' && *p != '-')
			return 0;
	return 1;
}

/* a list of type IDs, or of operations */
static int
goodlist(char *list, int ops)
{
	char *s, *f[Maxlist];
	int i, n;

	if(list == nil || *list == 0)
		return 0;
	s = estrdup(list);
	n = getfields(s, f, nelem(f), 0, ",");
	for(i=0; i<n; i++){
		if(ops){
			if(strcmp(f[i], "copy") != 0 && strcmp(f[i], "move") != 0 && strcmp(f[i], "link") != 0)
				break;
		}else if(!isid(f[i]))
			break;
	}
	free(s);
	return n > 0 && i == n;
}

/* attr=value in a tokenized command; nil if absent */
static char*
attr(char **f, int nf, char *name)
{
	int i, n;

	n = strlen(name);
	for(i=1; i<nf; i++)
		if(strncmp(f[i], name, n) == 0 && f[i][n] == '=')
			return f[i]+n+1;
	return nil;
}

/* arguments before the first attribute, verb included */
static int
npos(char **f, int nf)
{
	int i;

	for(i=0; i<nf; i++)
		if(strchr(f[i], '=') != nil)
			break;
	return i;
}

static void
wakeall(void)
{
	Uxdata *o;

	for(o=waiters; o; o=o->nextwait)
		nbsendul(o->wake, 1);
}

static void
blobput(Blob *b)
{
	if(--b->ref > 0)
		return;
	free(b->type);
	free(b->buf);
	free(b->err);
	free(b);
}

static void
postid(char *ev, Drag *d, int win)
{
	if(win >= 0)
		uxpost(Sdrag, "%s id=%d win=%d", ev, d->id, win);
	else
		uxpost(Sdrag, "%s id=%d", ev, d->id);
}

static void
unaccept(Drag *d)
{
	free(d->atype);
	free(d->aop);
	d->atype = d->aop = nil;
}

static void
dragend(Drag *d)
{
	Drag **l;
	Blob *b, *nb;

	for(l=&drags; *l; l=&(*l)->next)
		if(*l == d){
			*l = d->next;
			break;
		}
	for(b=d->blobs; b; b=nb){
		nb = b->next;
		b->d = nil;
		if(!b->done){
			b->done = 1;
			if(b->err == nil)
				b->err = estrdup("gone");
		}
		blobput(b);
	}
	postid("end", d, -1);
	free(d->app);
	free(d->types);
	free(d->ops);
	free(d->label);
	unaccept(d);
	free(d);
	wakeall();
}

static void
dragcancel(Drag *d)
{
	if(d->state == Dover)
		riosetcursor(nil);
	postid("cancel", d, -1);
	dragend(d);
}

static void
showcursor(Drag *d)
{
	riosetcursor(d->target != 0 && d->atype != nil ? &dragok : &dragno);
}

/* the window under the pointer that can take a drop, and pt local to it */
static int
dragtarget(Point xy, Point *pt)
{
	Window *w;

	*pt = ZP;
	if(uxbarhit(xy))
		return 0;
	w = wpointto(xy);
	if(w == nil || w->deleted || w->i == nil || Dx(w->screenr) <= 0)
		return 0;
	*pt = subpt(xy, addpt(w->screenr.min, Pt(Borderwidth, Borderwidth)));
	return w->id;
}

/*
 * The pointer moved, or a button changed, while a drag is in flight.
 * Called by mousethread.
 */
void
uxdragmouse(Point xy, int buttons)
{
	Drag *d;
	Point pt;
	int win;
	Fmt f;
	char *s;

	d = flying();
	if(d == nil)
		return;
	win = dragtarget(xy, &pt);
	if(win != d->target){
		if(d->target != 0)
			postid("leave", d, d->target);
		unaccept(d);
		d->target = win;
		d->pt = pt;
		if(win != 0){
			fmtstrinit(&f);
			fmtprint(&f, "enter id=%d win=%d pt=%d,%d", d->id, win, pt.x, pt.y);
			fmtattr(&f, "types", d->types);
			fmtattr(&f, "ops", d->ops);
			s = fmtstrflush(&f);
			uxpost(Sdrag, "%s", s);
			free(s);
		}
	}else if(win != 0 && !eqpt(pt, d->pt)){
		d->pt = pt;
		uxpost(Sdrag, "over id=%d win=%d pt=%d,%d", d->id, win, pt.x, pt.y);
	}
	if((buttons & 7) != 0){
		showcursor(d);
		return;
	}
	riosetcursor(nil);
	if(d->target == 0 || d->atype == nil || uxwin(d->target) == nil){
		dragcancel(d);
		return;
	}
	d->state = Ddropped;
	uxpost(Sdrag, "drop id=%d win=%d pt=%d,%d type=%s op=%s", d->id, d->target,
		d->pt.x, d->pt.y, d->atype, d->aop);
}

/*
 * Has the source window still to be told its button went up? Called
 * once per mouse event by mousethread while a drag is in flight.
 */
int
uxdragtellsrc(void)
{
	int t;

	t = tellsrc;
	tellsrc = 0;
	return t;
}

static char*
dragstart(Uxfid *u, Window *own, char **f, int nf)
{
	Drag *d;
	Window *w;
	char *v, *ops;
	int win;
	Fmt fm;
	char *s;

	if(npos(f, nf) != 1)
		return "bad syntax";
	v = attr(f, nf, "win");
	if(v != nil){
		win = atoi(v);
		w = uxwin(win);
	}else{
		if(own == nil)
			return "bad syntax: no window";
		w = own->deleted || own->i == nil ? nil : own;
	}
	if(w == nil)
		return "not found";
	if(!goodlist(attr(f, nf, "types"), 0))
		return "bad value: types";
	ops = attr(f, nf, "ops");
	if(ops != nil && !goodlist(ops, 1))
		return "bad value: ops";
	if((mouse->buttons & 7) == 0)
		return "bad value: no button down";
	if(flying() != nil)
		return "busy";
	d = emalloc(sizeof *d);
	d->id = ++dragseq;
	d->src = w->id;
	d->app = uxappid(w->id) != nil ? estrdup(uxappid(w->id)) : nil;
	d->types = estrdup(attr(f, nf, "types"));
	d->ops = estrdup(ops != nil ? ops : "copy");
	v = attr(f, nf, "label");
	d->label = v != nil ? estrdup(v) : nil;
	d->sess = u;
	d->state = Dover;
	d->next = drags;
	drags = d;
	tellsrc = 1;

	fmtstrinit(&fm);
	fmtprint(&fm, "start id=%d win=%d", d->id, d->src);
	fmtattr(&fm, "app", d->app);
	fmtattr(&fm, "types", d->types);
	fmtattr(&fm, "ops", d->ops);
	fmtattr(&fm, "label", d->label);
	s = fmtstrflush(&fm);
	uxpost(Sdrag, "%s", s);
	free(s);
	uxdragmouse(mouse->xy, mouse->buttons);
	return nil;
}

/* drag/ctl */
char*
uxdragctl(Uxfid *u, Window *own, char *line)
{
	char *f[16], *type, *op, *v, *s;
	int nf, np, win;
	Drag *d;
	Fmt fm;

	nf = tokenize(line, f, nelem(f));
	if(nf == 0)
		return nil;
	np = npos(f, nf);
	d = np > 1 ? finddrag(atoi(f[1])) : nil;
	v = attr(f, nf, "win");
	win = v != nil ? atoi(v) : (own != nil ? own->id : 0);

	if(strcmp(f[0], "start") == 0)
		return dragstart(u, own, f, nf);
	if(strcmp(f[0], "cancel") == 0){
		if(np == 1)
			d = flying();
		if(np > 2)
			return "bad syntax";
		if(d == nil || d->state != Dover)
			return "not found";
		dragcancel(d);
		return nil;
	}
	if(strcmp(f[0], "accept") == 0 || strcmp(f[0], "refuse") == 0){
		if(np != 2)
			return "bad syntax";
		if(d == nil)
			return "not found";
		if(win == 0)
			return "bad syntax: no window";
		if(f[0][0] == 'r'){
			if(d->state == Dover && d->target == win && d->atype != nil){
				unaccept(d);
				postid("refuse", d, win);
				showcursor(d);
			}
			return nil;
		}
		type = attr(f, nf, "type");
		op = attr(f, nf, "op");
		if(type != nil && !inlist(d->types, type))
			return "bad value: type not offered";
		if(op != nil && !inlist(d->ops, op))
			return "bad value: op not allowed";
		if(d->state != Dover || d->target != win)
			return nil;	/* the pointer has moved on */
		unaccept(d);
		d->atype = type != nil ? estrdup(type) : firstof(d->types);
		d->aop = op != nil ? estrdup(op) : firstof(d->ops);
		uxpost(Sdrag, "accept id=%d win=%d type=%s op=%s", d->id, win, d->atype, d->aop);
		showcursor(d);
		return nil;
	}
	if(strcmp(f[0], "done") == 0){
		op = attr(f, nf, "op");
		if(np != 2)
			return "bad syntax";
		if(d == nil || d->state != Ddropped)
			return "not found";
		if(op != nil && !inlist(d->ops, op))
			return "bad value: op not allowed";
		fmtstrinit(&fm);
		fmtprint(&fm, "done id=%d win=%d op=%s", d->id, d->target, op != nil ? op : d->aop);
		fmtattr(&fm, "err", attr(f, nf, "err"));
		s = fmtstrflush(&fm);
		uxpost(Sdrag, "%s", s);
		free(s);
		dragend(d);
		return nil;
	}
	return "unknown command";
}

/* the fid that started drags was closed: they end */
void
uxdragclose(Uxfid *u)
{
	Drag *d, *nd;

	for(d=drags; d; d=nd){
		nd = d->next;
		if(d->sess != u)
			continue;
		if(d->state == Dover)
			dragcancel(d);
		else
			dragend(d);
	}
}

char*
uxdragstate(int *np)
{
	Drag *d, **v;
	int i, n;
	Fmt f;
	char *s;

	n = 0;
	for(d=drags; d; d=d->next)
		n++;
	v = emalloc((n+1)*sizeof v[0]);
	i = n;
	for(d=drags; d; d=d->next)
		v[--i] = d;
	fmtstrinit(&f);
	for(i=0; i<n; i++){
		d = v[i];
		fmtprint(&f, "drag id=%d win=%d", d->id, d->src);
		fmtattr(&f, "app", d->app);
		fmtattr(&f, "types", d->types);
		fmtattr(&f, "ops", d->ops);
		fmtattr(&f, "label", d->label);
		fmtprint(&f, " state=%s target=%d", d->state == Dover ? "over" : "dropped", d->target);
		if(d->atype != nil)
			fmtprint(&f, " type=%s op=%s", d->atype, d->aop);
		fmtprint(&f, "\n");
	}
	free(v);
	s = fmtstrflush(&f);
	*np = strlen(s);
	return s;
}

/*
 * drag/data
 */
Uxdata*
uxdataopen(void)
{
	Uxdata *o;

	o = emalloc(sizeof *o);
	o->wake = chancreate(sizeof(int), 1);
	return o;
}

static Blob*
newblob(Drag *d, char *type, int item, int req)
{
	Blob *b;

	b = emalloc(sizeof *b);
	b->type = estrdup(type);
	b->item = item;
	b->req = req;
	b->ref = 2;	/* the drag's list, and the fid */
	b->d = d;
	b->next = d->blobs;
	d->blobs = b;
	return b;
}

static Blob*
findreq(int req)
{
	Drag *d;
	Blob *b;

	for(d=drags; d; d=d->next)
		for(b=d->blobs; b; b=b->next)
			if(b->req == req && !b->done)
				return b;
	return nil;
}

static char*
datafirst(Uxdata *o, char *line)
{
	char *f[8], *v;
	int nf, np, item;
	Drag *d;
	Blob *b;

	nf = tokenize(line, f, nelem(f));
	if(nf == 0)
		return "bad syntax";
	np = npos(f, nf);
	v = attr(f, nf, "item");
	item = v != nil ? atoi(v) : -1;
	if(strcmp(f[0], "put") == 0 || strcmp(f[0], "fetch") == 0){
		if(np != 3)
			return "bad syntax";
		d = finddrag(atoi(f[1]));
		if(d == nil)
			return "not found";
		if(!inlist(d->types, f[2]))
			return "bad value: type not offered";
		if(v != nil && item < 0)
			return "bad value: item";
		if(f[0][0] == 'p'){
			o->mode = Mput;
			o->b = newblob(d, f[2], item, 0);
			return nil;
		}
		if(d->state != Ddropped)
			return "bad value: not dropped";
		o->mode = Mfetch;
		for(b=d->blobs; b; b=b->next)
			if(b->req == 0 && b->item == item && strcmp(b->type, f[2]) == 0)
				break;
		if(b != nil){
			b->ref++;
			o->b = b;
			return nil;
		}
		o->b = newblob(d, f[2], item, ++reqseq);
		if(item >= 0)
			uxpost(Sdrag, "fetch id=%d type=%s item=%d req=%d", d->id, f[2], item, o->b->req);
		else
			uxpost(Sdrag, "fetch id=%d type=%s req=%d", d->id, f[2], o->b->req);
		return nil;
	}
	if(strcmp(f[0], "reply") == 0 || strcmp(f[0], "fail") == 0){
		if(np < 2 || (f[0][0] == 'r' && np != 2))
			return "bad syntax";
		b = findreq(atoi(f[1]));
		if(b == nil)
			return "not found";
		if(f[0][0] == 'r'){
			o->mode = Mreply;
			b->ref++;
			o->b = b;
			return nil;
		}
		b->err = estrdup(np > 2 ? f[2] : "failed");
		b->done = 1;
		wakeall();
		return nil;
	}
	return "unknown command";
}

/* a write to drag/data; data is not NUL-terminated */
char*
uxdatawrite(Uxdata *o, char *data, long n)
{
	char *line, *err, *p;
	Blob *b;
	long k;

	if(o->mode == Mnone){
		/* the first line is the command; the rest of the write is payload */
		p = memchr(data, '\n', n);
		k = p != nil ? p-data : n;
		line = emalloc(k+1);
		memmove(line, data, k);
		line[k] = 0;
		err = datafirst(o, line);
		free(line);
		if(err != nil || p == nil || k+1 == n)
			return err;
		if(o->mode != Mput && o->mode != Mreply)
			return "bad syntax: data after the command";
		data += k+1;
		n -= k+1;
	}
	if(o->mode == Mfetch)
		return "bad syntax: fetching";
	b = o->b;
	if(b->d == nil)
		return "gone";
	if(b->d->total+n > Maxdrag)
		return "too large";
	b->buf = erealloc(b->buf, b->n+n);
	memmove(b->buf+b->n, data, n);
	b->n += n;
	b->d->total += n;
	wakeall();
	return nil;
}

static void
unwait(Uxdata *o)
{
	Uxdata **l;

	for(l=&waiters; *l; l=&(*l)->nextwait)
		if(*l == o){
			*l = o->nextwait;
			break;
		}
	o->waiting = 0;
}

/* a read of drag/data; it responds */
void
uxdataread(Xfid *x, Uxdata *o)
{
	Fcall t;
	Blob *b;
	long n;
	enum { Awake, Aflush, Aend };
	Alt alts[Aend+1];

	if(o->mode != Mfetch){
		filsysrespond(x->fs, x, &t, "bad syntax: not fetching");
		return;
	}
	if(o->waiting){
		filsysrespond(x->fs, x, &t, "busy");
		return;
	}
	o->waiting = 1;
	o->nextwait = waiters;
	waiters = o;
	b = o->b;
	while(o->off >= b->n && !b->done){
		alts[Awake].c = o->wake;
		alts[Awake].v = nil;
		alts[Awake].op = CHANRCV;
		alts[Aflush].c = x->flushc;
		alts[Aflush].v = nil;
		alts[Aflush].op = CHANRCV;
		alts[Aend].op = CHANEND;
		if(alt(alts) == Aflush){
			unwait(o);
			filsyscancel(x);
			return;
		}
	}
	unwait(o);
	if(o->off < b->n){
		n = b->n - o->off;
		if(n > x->count)
			n = x->count;
		t.data = b->buf + o->off;
		t.count = n;
		o->off += n;
		filsysrespond(x->fs, x, &t, nil);
		return;
	}
	if(b->err != nil){
		filsysrespond(x->fs, x, &t, b->err);
		return;
	}
	t.data = nil;
	t.count = 0;
	filsysrespond(x->fs, x, &t, nil);
}

void
uxdataclose(Uxdata *o)
{
	if(o->waiting)
		unwait(o);
	if(o->b != nil){
		if(o->mode == Mput || o->mode == Mreply){
			o->b->done = 1;
			wakeall();
		}
		blobput(o->b);
	}
	chanfree(o->wake);
	free(o);
}
