/*
 * The libux API on the uitree tree. An app's windows and controls are
 * nodes in the tree (tmodel.c), which is what 9P serves and uxd sees;
 * each node is drawn by a control of the drawing layer (dx.h), made at
 * the same time and returned as the handle.
 *
 * The tree is the source of truth both ways. The API's setters change
 * the node, then the control. A 9P write changes the node, and
 * capisync, run by every uisync, brings the control up to date. An
 * app's handlers are fed from the tree's own events (evhook): what a
 * person does to a control is emitted as a uitree event, and that
 * event, like one a script caused, reaches the handler. Events the API
 * itself causes are sent to 9P subscribers but not to handlers.
 *
 * Locks: the model lock (lockmodel) is taken first, uxlk (inside the
 * drawing layer) second. The drawing layer is never called with the
 * model held, except from capisync, which runs on the UI thread.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "dx.h"
#include "ux.h"
#include "tree.h"
#include "undx.h"

typedef struct Cc Cc;
typedef struct Cw Cw;
typedef struct Call Call;
typedef struct Ic Ic;

enum {
	Kbutton,
	Ktext,
	Kcheck,
	Klabel,
	Kgroup,
	Klist,
	Kicons,
	Kscroll,
	Kchooser,
	Kcons,
};

/* an icon view's icon, as its items file shows it */
struct Ic {
	char	*id;
	char	*label;
	char	*icon;
	Point	pt;
};

/* a control made through the API, and its node */
struct Cc {
	int	node;
	char	*win;	/* its window's name */
	Uxctl	*c;
	int	kind;
	Uxfn	*fn;
	void	*arg;
	char	*label;	/* what the control shows now, to compare with the node */
	char	*value;
	int	off;
	Ic	*ic;	/* Kicons */
	int	nic;
	Cc	*next;
};

/* a window made through the API */
struct Cw {
	char	*name;
	Uxwin	*w;
	Uxfn	*fn;
	void	*arg;
	char	*title;
	Cw	*next;
};

/* a handler call waiting for the UI thread */
struct Call {
	Uxfn	*fn;
	void	*arg;
	Uxev	ev;
	char	*s[3];
	Call	*next;
};

static char *ntype[] = {
[Kbutton]	"button",
[Ktext]	"input",
[Kcheck]	"checkbox",
[Klabel]	"text",
[Kgroup]	"group-box",
[Klist]	"list",
[Kicons]	"icon-view",
[Kscroll]	"scroll",
[Kchooser]	"file-chooser",
[Kcons]	"console",
};

/* the prop that holds what a kind shows as its label */
static char *labelprop[] = {
[Kbutton]	"label",
[Ktext]	"placeholder",
[Kcheck]	"label",
[Klabel]	nil,
[Kgroup]	"title",
[Klist]	nil,
[Kicons]	nil,
[Kscroll]	nil,
[Kchooser]	nil,
[Kcons]	nil,
};

static Cc	*ccs;
static Cw	*cws;
static Call	*calls, **calltail = &calls;
static int	inapi;	/* the API is writing: its events don't reach handlers */
static Uxev	*orig;	/* the drawing layer's event being emitted */
static int	usedmain;

typedef struct Kn Kn;
struct Kn {
	char	*kind;
	int	n;
	Kn	*next;
};
static Kn	*kinds;

static void	capisync1(void);
static void	hook(char*, char*, char*, char*);

static Cc*
ccbyctl(Uxctl *c)
{
	Cc *x;

	for(x = ccs; x != nil; x = x->next)
		if(x->c == c)
			return x;
	return nil;
}

static Cc*
ccbynode(int id)
{
	Cc *x;

	for(x = ccs; x != nil; x = x->next)
		if(x->node == id)
			return x;
	return nil;
}

static Cw*
cwbywin(Uxwin *w)
{
	Cw *x;

	for(x = cws; x != nil; x = x->next)
		if(x->w == w)
			return x;
	return nil;
}

static Cw*
cwbyname(char *name)
{
	Cw *x;

	if(name == nil)
		return nil;
	for(x = cws; x != nil; x = x->next)
		if(strcmp(x->name, name) == 0)
			return x;
	return nil;
}

static void
freeic(Cc *c)
{
	int i;

	for(i = 0; i < c->nic; i++){
		free(c->ic[i].id);
		free(c->ic[i].label);
		free(c->ic[i].icon);
	}
	free(c->ic);
	c->ic = nil;
	c->nic = 0;
}

/* forget the controls whose nodes are gone */
static void
purge(void)
{
	Cc **l, *c;

	for(l = &ccs; (c = *l) != nil; ){
		if(nodebyid(c->node) == nil){
			*l = c->next;
			free(c->win);
			free(c->label);
			free(c->value);
			freeic(c);
			free(c);
			continue;
		}
		l = &c->next;
	}
}

static char*
strornil(char *s)
{
	return s != nil ? estrdup(s) : nil;
}

static void
setstr(char **p, char *s)
{
	free(*p);
	*p = strornil(s);
}

static int
streq(char *a, char *b)
{
	if(a == nil || b == nil)
		return a == b;
	return strcmp(a, b) == 0;
}

/*
 * Handlers
 */

static void
runcalls(Uxev*, void*)
{
	Call *k, *n;
	int i;

	lockmodel();
	capisync1();	/* a handler reads the control: bring it up to date first */
	k = calls;
	calls = nil;
	calltail = &calls;
	unlockmodel();
	for(; k != nil; k = n){
		n = k->next;
		k->fn(&k->ev, k->arg);
		for(i = 0; i < nelem(k->s); i++)
			free(k->s[i]);
		free(k);
	}
}

/* a word attr=V of attrs, malloc'd, or nil */
static char*
attrof(char *attrs, char *name)
{
	char *s, *f[32];
	int i, n, l;

	if(attrs == nil)
		return nil;
	s = estrdup(attrs);
	n = tokenize(s, f, nelem(f));
	l = strlen(name);
	for(i = 0; i < n; i++)
		if(strncmp(f[i], name, l) == 0 && f[i][l] == '='){
			name = estrdup(f[i]+l+1);
			free(s);
			return name;
		}
	free(s);
	return nil;
}

static int
viaof(char *attrs)
{
	char *v;
	int r;

	v = attrof(attrs, "via");
	r = UXctl;
	if(v != nil){
		if(strcmp(v, "user") == 0)
			r = UXui;
		else if(strcmp(v, "key") == 0)
			r = UXkey;
		else if(strcmp(v, "menu") == 0)
			r = UXmenubar;
		free(v);
	}
	return r;
}

/* a uitree event as the Uxev a handler gets; 0 if it has none */
static int
synth(Call *k, char *name, char *attrs, Cw *w, Cc *c)
{
	static struct {
		char	*name;
		int	type;
	} map[] = {
		"click",	UXactivate,
		"change",	UXvalue,
		"select",	UXselection,
		"submit",	UXsubmit,
		"open",	UXopen,
		"rename",	UXrename,
		"move",	UXmove,
		"scroll",	UXscroll,
		"close",	UXclose,
		"confirm",	UXdefault,
		"cancel",	UXcancel,
		"interrupt",	UXinterrupt,
	};
	int i;
	char *v;

	for(i = 0; i < nelem(map); i++)
		if(strcmp(map[i].name, name) == 0)
			break;
	if(i == nelem(map))
		return 0;
	memset(&k->ev, 0, sizeof k->ev);
	k->ev.type = map[i].type;
	k->ev.via = viaof(attrs);
	k->ev.win = w->w;
	k->ev.ctl = c != nil ? c->c : nil;
	if((v = attrof(attrs, "item")) != nil){
		k->s[0] = v;
		k->ev.item = v;
	}
	if((v = attrof(attrs, "text")) != nil){
		k->s[1] = v;
		k->ev.text = v;
	}
	if((v = attrof(attrs, "n")) != nil){
		k->ev.n = atoi(v);
		free(v);
	}
	return 1;
}

/* every event in the tree passes here (tev.c's deliver), model held */
static void
hook(char *name, char *path, char *attrs, char *win)
{
	Cw *w;
	Cc *c;
	Win *mw;
	Node *n;
	Uxfn *fn;
	void *arg;
	Call *k;
	char *e;

	if(inapi || path == nil || strcmp(path, "-") == 0 || (w = cwbyname(win)) == nil)
		return;
	c = nil;
	if(strcmp(path, ".") == 0){
		fn = w->fn;
		arg = w->arg;
	}else{
		if((mw = winbyname(win)) == nil)
			return;
		if((e = resolve(mw->root, path, &n)) != nil){
			free(e);
			return;
		}
		if((c = ccbynode(n->id)) == nil)
			return;
		fn = c->fn;
		arg = c->arg;
	}
	if(fn == nil)
		return;
	k = emalloc(sizeof *k);
	memset(k, 0, sizeof *k);
	k->fn = fn;
	k->arg = arg;
	if(orig != nil){
		k->ev = *orig;
		k->ev.action = k->s[0] = strornil(orig->action);
		k->ev.item = k->s[1] = strornil(orig->item);
		k->ev.text = k->s[2] = strornil(orig->text);
	}else if(!synth(k, name, attrs, w, c)){
		free(k);
		return;
	}
	*calltail = k;
	calltail = &k->next;
	uxcall(runcalls, nil);
}

/*
 * The drawing layer's events, as uitree events
 */

static Win*
winofnode(Node *n)
{
	while(n->parent != nil)
		n = n->parent;
	return n->win;
}

static char*
viaword(int via)
{
	switch(via){
	case UXkey:
		return "key";
	case UXmenubar:
		return "menu";
	case UXctl:
		return "ctl";
	}
	return "user";
}

static void	iconitems(Cc*, Node*);

static void
capictlev(Uxev *e, void *arg)
{
	Cc *c;
	Node *n;
	char *name, *attrs, *v, *q, **ids;
	int state, i, ni;
	Fmt f;

	lockmodel();
	n = nodebyid((int)(uintptr)arg);
	c = ccbynode((int)(uintptr)arg);
	if(n == nil || c == nil || quitting){
		unlockmodel();
		return;
	}
	state = 0;
	fmtstrinit(&f);
	switch(e->type){
	case UXactivate:
		name = "click";
		break;
	case UXvalue:
		name = "change";
		state = 1;
		v = uxgetvalue(c->c);
		setstr(&c->value, v);
		setprop(n, c->kind == Klabel ? "text" : "value", v, 0);
		if(c->kind == Kcheck)
			fmtprint(&f, "value=%s ", v);
		free(v);
		break;
	case UXselection:
		name = "select";
		state = 1;
		if(c->kind == Kicons){
			ni = uxicongetsel(c->c, &ids);
			fmtprint(&f, "value=");
			for(i = 0; i < ni; i++){
				fmtprint(&f, "%s%s", i > 0 ? "," : "", ids[i]);
				free(ids[i]);
			}
			fmtprint(&f, " ");
			free(ids);
		}else if(e->n > 0)
			fmtprint(&f, "value=%d ", e->n);
		break;
	case UXsubmit:
		name = "submit";
		break;
	case UXopen:
		name = "open";
		if(e->item != nil)
			fmtprint(&f, "item=%s ", e->item);
		break;
	case UXrename:
		name = "rename";
		if(e->item != nil){
			fmtprint(&f, "item=%s ", e->item);
			for(i = 0; i < c->nic; i++)
				if(strcmp(c->ic[i].id, e->item) == 0)
					setstr(&c->ic[i].label, e->text);
			iconitems(c, n);
		}
		break;
	case UXmove:
		name = "move";
		state = 1;
		if(e->item != nil){
			fmtprint(&f, "item=%s pos=%d,%d ", e->item, e->pt.x, e->pt.y);
			for(i = 0; i < c->nic; i++)
				if(strcmp(c->ic[i].id, e->item) == 0)
					c->ic[i].pt = e->pt;
			iconitems(c, n);
		}
		break;
	case UXscroll:
		name = "scroll";
		state = 1;
		break;
	case UXinterrupt:
		name = "interrupt";
		break;
	case UXchoose:
		name = "choose";
		if(e->text != nil){
			setprop(n, "value", e->text, 0);
			if((q = quoteval(e->text)) != nil && strchr(q, ' ') == nil)
				fmtprint(&f, "value=%s ", q);
			free(q);
		}
		break;
	case UXcancel:
		name = "cancel";
		break;
	case UXdrop:
		name = "drop";
		if(e->item != nil)
			fmtprint(&f, "item=%s ", e->item);
		fmtprint(&f, "drag=%d ", e->drag);
		break;
	case UXdragend:
		name = "drag-end";
		break;
	default:
		name = "event";
		break;
	}
	fmtprint(&f, "via=%s", viaword(e->via));
	attrs = fmtstrflush(&f);
	orig = e;
	emitnode(name, winofnode(n), n, attrs, state);
	orig = nil;
	free(attrs);
	sweep();
	unlockmodel();
}

static void
capiwinev(Uxev *e, void *arg)
{
	Win *w;
	char *name, *attrs;
	int state;
	Point p;

	lockmodel();
	w = winbyname(arg);
	if(w == nil || quitting){
		unlockmodel();
		return;
	}
	state = 0;
	attrs = nil;
	switch(e->type){
	case UXclose:
		name = "close";
		attrs = smprint("via=%s", e->via == UXui ? "titlebar" : viaword(e->via));
		break;
	case UXresize:
		name = "resize";
		state = 1;
		p = uxwinsize(e->win);
		w->w = p.x;
		w->h = p.y;
		attrs = smprint("w=%d h=%d via=%s", p.x, p.y, viaword(e->via));
		break;
	case UXfront:
		name = "focus";
		state = 1;
		break;
	case UXdefault:
		name = "confirm";
		break;
	case UXcancel:
		name = "cancel";
		break;
	case UXdrop:
		name = "drop";
		attrs = smprint("drag=%d via=%s", e->drag, viaword(e->via));
		break;
	case UXaction:
		name = "action";
		break;
	default:
		name = "event";
		break;
	}
	if(attrs == nil)
		attrs = smprint("via=%s", viaword(e->via));
	orig = e;
	emitnode(name, w, w->root, attrs, state);
	orig = nil;
	free(attrs);
	sweep();
	unlockmodel();
}

/*
 * The API
 */

void
uxinit(char *name, char *title)
{
	dxinit(name, title);
	initapp();
	evhook = hook;
	drawing = 1;
	treeserve(nil, nil);
	uxpublish(treepost);
}

static char*
winname(char *kind)
{
	Kn *k;

	if(!usedmain){
		usedmain = 1;
		return estrdup("main");
	}
	for(k = kinds; k != nil; k = k->next)
		if(strcmp(k->kind, kind) == 0)
			break;
	if(k == nil){
		k = emalloc(sizeof *k);
		k->kind = estrdup(kind);
		k->n = 0;
		k->next = kinds;
		kinds = k;
	}
	return smprint("%s-%d", kind, ++k->n);
}

static Uxwin*
adoptwin(Uxwin *uw, char *name)
{
	Cw *cw;

	lockmodel();
	cw = emalloc(sizeof *cw);
	memset(cw, 0, sizeof *cw);
	cw->name = name;
	cw->w = uw;
	cw->next = cws;
	cws = cw;
	unlockmodel();
	dxwinhandler(uw, capiwinev, cw->name);
	return uw;
}

static char*
mkwin(char *kind, char *title, Rectangle r, int dialog)
{
	char *name;
	Win *w;

	lockmodel();
	name = winname(dialog ? "dialog" : kind != nil ? kind : "window");
	if((w = winbyname(name)) == nil)
		w = newwin(name);
	w->capi = 1;
	w->shown = 1;
	free(w->title);
	w->title = estrdup(title != nil ? title : "");
	if(Dx(r) > 0){
		w->w = Dx(r);
		w->h = Dy(r);
	}
	if(dialog){
		free(w->kind);
		w->kind = estrdup("dialog");
	}
	unlockmodel();
	return name;
}

Uxwin*
uxwindow(char *kind, char *title, Rectangle r)
{
	char *name;

	name = mkwin(kind, title, r, 0);
	return adoptwin(dxwindow(kind, title, r), name);
}

Uxwin*
uxdialog(char *title, Rectangle r)
{
	char *name;

	name = mkwin("dialog", title, r, 1);
	return adoptwin(dxdialog(title, r), name);
}

void
uxwinhandler(Uxwin *w, Uxfn *fn, void *arg)
{
	Cw *cw;

	lockmodel();
	cw = cwbywin(w);
	if(cw != nil){
		cw->fn = fn;
		cw->arg = arg;
	}
	unlockmodel();
	if(cw == nil)
		dxwinhandler(w, fn, arg);
}

void
uxwinsettitle(Uxwin *w, char *t)
{
	Cw *cw;
	Win *mw;

	lockmodel();
	if((cw = cwbywin(w)) != nil && (mw = winbyname(cw->name)) != nil){
		free(mw->title);
		mw->title = estrdup(t);
		setstr(&cw->title, t);
	}
	unlockmodel();
	dxwinsettitle(w, t);
}

void
uxwinclose(Uxwin *w)
{
	Cw *cw, **l;
	Win *mw;

	lockmodel();
	for(l = &cws; (cw = *l) != nil; l = &cw->next)
		if(cw->w == w)
			break;
	if(cw != nil){
		*l = cw->next;
		if((mw = winbyname(cw->name)) != nil)
			delwin(mw);
		purge();
		sweep();
		/* cw->name is the drawing handler's arg: it stays */
		free(cw->title);
		free(cw);
	}
	unlockmodel();
	dxwinclose(w);
}

/* the style that places a node where the API's rectangle says */
static void
setgeom(Node *n, Rectangle r)
{
	free(n->style);
	n->style = smprint("absolute left=%d top=%d w=%d h=%d", r.min.x, r.min.y, Dx(r), Dy(r));
}

/* c, just made, gets a node in w's root */
static Uxctl*
adopt(Uxwin *w, Uxctl *c, int kind, char *type, Rectangle r, char *lbl)
{
	Cw *cw;
	Cc *cc;
	Win *mw;
	Node *n, *o;
	char *id;

	if(c == nil)
		return nil;
	id = uxctlid(c);
	lockmodel();
	if((cw = cwbywin(w)) == nil || (mw = winbyname(cw->name)) == nil){
		unlockmodel();
		return c;
	}
	if((o = kidbyname(mw->root, id)) != nil){
		delnode(o);
		purge();
	}
	n = newnode(mw->root, id, type != nil ? type : ntype[kind]);
	setgeom(n, r);
	if(lbl != nil && labelprop[kind] != nil && lbl[0] != 0)
		setprop(n, labelprop[kind], lbl, 0);
	cc = emalloc(sizeof *cc);
	memset(cc, 0, sizeof *cc);
	cc->node = n->id;
	cc->win = estrdup(cw->name);
	cc->c = c;
	cc->kind = kind;
	cc->label = strornil(lbl);
	cc->next = ccs;
	ccs = cc;
	unlockmodel();
	dxhandler(c, capictlev, (void*)(uintptr)cc->node);
	return c;
}

/* the node of an adopted control, model held */
static Node*
nodeof(Uxctl *c, Cc **ccp)
{
	Cc *cc;

	if((cc = ccbyctl(c)) == nil)
		return nil;
	if(ccp != nil)
		*ccp = cc;
	return nodebyid(cc->node);
}

Uxctl*
uxbutton(Uxwin *w, char *id, char *label, Rectangle r)
{
	return adopt(w, dxbutton(w, id, label, r), Kbutton, nil, r, label);
}

Uxctl*
uxtextfield(Uxwin *w, char *id, char *label, Rectangle r, int flags)
{
	Uxctl *c;
	Node *n;

	c = adopt(w, dxtextfield(w, id, label, r, flags), Ktext,
		flags & UXmultiline ? "textarea" : "input", r, label);
	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		setprop(n, "value", "", 0);
		if(flags & UXsecret)
			setprop(n, "secret", "on", 1);
		if(flags & (UXsensitive|UXsecret))
			setprop(n, "sensitive", "on", 1);
		if(flags & UXreadonly)
			setprop(n, "readonly", "on", 1);
	}
	unlockmodel();
	return c;
}

Uxctl*
uxcheckbox(Uxwin *w, char *id, char *label, Rectangle r)
{
	Uxctl *c;
	Node *n;
	Cc *cc;

	c = adopt(w, dxcheckbox(w, id, label, r), Kcheck, nil, r, label);
	lockmodel();
	if((n = nodeof(c, &cc)) != nil){
		setprop(n, "value", "off", 0);
		setstr(&cc->value, "off");
	}
	unlockmodel();
	return c;
}

Uxctl*
uxlabel(Uxwin *w, char *id, char *text, Rectangle r)
{
	Uxctl *c;
	Node *n;
	Cc *cc;

	c = adopt(w, dxlabel(w, id, text, r), Klabel, nil, r, nil);
	lockmodel();
	if((n = nodeof(c, &cc)) != nil){
		setprop(n, "text", text, 0);
		setstr(&cc->value, text);
	}
	unlockmodel();
	return c;
}

Uxctl*
uxgroup(Uxwin *w, char *id, char *label, Rectangle r)
{
	return adopt(w, dxgroup(w, id, label, r), Kgroup, nil, r, label);
}

Uxctl*
uxlist(Uxwin *w, char *id, Rectangle r)
{
	return adopt(w, dxlist(w, id, r), Klist, nil, r, nil);
}

Uxctl*
uxiconview(Uxwin *w, char *id, Rectangle r)
{
	return adopt(w, dxiconview(w, id, r), Kicons, nil, r, nil);
}

Uxctl*
uxscrollview(Uxwin *w, char *id, Rectangle r)
{
	return adopt(w, dxscrollview(w, id, r), Kscroll, nil, r, nil);
}

Uxctl*
uxfilechooser(Uxwin *w, char *id, char *dir, Rectangle r)
{
	Uxctl *c;
	Node *n;

	c = adopt(w, dxfilechooser(w, id, dir, r), Kchooser, nil, r, nil);
	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		setprop(n, "dir", dir != nil ? dir : ".", 0);
		setprop(n, "value", "", 0);
	}
	unlockmodel();
	return c;
}

Uxctl*
uxconsole(Uxwin *w, char *id, Rectangle r)
{
	Uxctl *c;
	Node *n;

	c = adopt(w, dxconsole(w, id, r), Kcons, nil, r, nil);
	lockmodel();
	if((n = nodeof(c, nil)) != nil)
		setprop(n, "value", "", 0);
	unlockmodel();
	return c;
}

/* move n under p, keeping its name */
static void
reparent(Node *n, Node *p)
{
	Node *o;
	int i, j;

	o = n->parent;
	if(o == p || p == nil)
		return;
	if(o != nil){
		for(i = j = 0; i < o->nkid; i++)
			if(o->kid[i] != n)
				o->kid[j++] = o->kid[i];
		o->nkid = j;
		for(i = j = 0; i < o->nord; i++)
			if(o->ord[i] != n->id)
				o->ord[j++] = o->ord[i];
		o->nord = j;
	}
	n->parent = p;
	p->kid = erealloc(p->kid, (p->nkid+1)*sizeof(Node*));
	p->kid[p->nkid++] = n;
	p->ord = erealloc(p->ord, (p->nord+1)*sizeof(int));
	p->ord[p->nord++] = n->id;
}

void
uxdelete(Uxctl *c)
{
	Node *n, *root;
	Win *w;

	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		/* as the drawing layer does: the children stay, in the window */
		w = winofnode(n);
		root = w != nil ? w->root : nil;
		while(n->nkid > 0 && root != nil && n->kid[0] != nil)
			reparent(n->kid[0], root);
		delnode(n);
		purge();
		sweep();
	}
	unlockmodel();
	dxdelete(c);
}

void
uxhandler(Uxctl *c, Uxfn *fn, void *arg)
{
	Cc *cc;

	lockmodel();
	if((cc = ccbyctl(c)) != nil){
		cc->fn = fn;
		cc->arg = arg;
	}
	unlockmodel();
	if(cc == nil)
		dxhandler(c, fn, arg);
}

void
uxsetparent(Uxctl *c, Uxctl *p)
{
	Node *n, *pn;
	Win *w;

	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		pn = p != nil ? nodeof(p, nil) : nil;
		if(pn == nil && (w = winofnode(n)) != nil)
			pn = w->root;
		reparent(n, pn);
		sweep();
	}
	unlockmodel();
	dxsetparent(c, p);
}

void
uxsetrect(Uxctl *c, Rectangle r)
{
	Node *n;

	lockmodel();
	if((n = nodeof(c, nil)) != nil)
		setgeom(n, r);
	unlockmodel();
	dxsetrect(c, r);
}

/* an echo of the API's own change, for 9P subscribers only */
static void
echo(Node *n, char *name, char *attrs, int state)
{
	inapi = 1;
	emitnode(name, winofnode(n), n, attrs, state);
	inapi = 0;
}

void
uxsetvalue(Uxctl *c, char *v)
{
	Node *n;
	Cc *cc;
	char *a;

	lockmodel();
	if((n = nodeof(c, &cc)) != nil){
		setprop(n, cc->kind == Klabel ? "text" : "value", v, 0);
		setstr(&cc->value, v);
		if(cc->kind != Klabel){
			a = cc->kind == Kcheck ? smprint("value=%s via=ctl", v) : estrdup("via=ctl");
			echo(n, "change", a, 1);
			free(a);
		}
		sweep();
	}
	unlockmodel();
	dxsetvalue(c, v);
}

void
uxsetlabel(Uxctl *c, char *l)
{
	Node *n;
	Cc *cc;

	lockmodel();
	if((n = nodeof(c, &cc)) != nil && labelprop[cc->kind] != nil){
		setprop(n, labelprop[cc->kind], l, 0);
		setstr(&cc->label, l);
	}
	unlockmodel();
	dxsetlabel(c, l);
}

void
uxenable(Uxctl *c, int on)
{
	Node *n;
	Cc *cc;

	lockmodel();
	if((n = nodeof(c, &cc)) != nil){
		if(on)
			rmprop(n, "disabled");
		else
			setprop(n, "disabled", "on", 1);
		cc->off = !on;
	}
	unlockmodel();
	dxenable(c, on);
}

/* a hidden node is left out of its parent's order (core.md, Node files) */
void
uxshow(Uxctl *c, int on)
{
	Node *n, *p;
	int i, in;

	lockmodel();
	if((n = nodeof(c, nil)) != nil && (p = n->parent) != nil){
		in = 0;
		for(i = 0; i < p->nord; i++)
			if(p->ord[i] == n->id)
				in = 1;
		if(on && !in){
			p->ord = erealloc(p->ord, (p->nord+1)*sizeof(int));
			p->ord[p->nord++] = n->id;
		}else if(!on && in){
			for(i = 0; i < p->nord; i++)
				if(p->ord[i] == n->id){
					memmove(p->ord+i, p->ord+i+1, (p->nord-i-1)*sizeof(int));
					p->nord--;
					break;
				}
		}
	}
	unlockmodel();
	dxshow(c, on);
}

/* list items: "N text", N from 1; a tab between columns stays */
void
uxsetitems(Uxctl *c, char **items, int ni)
{
	Node *n;
	Fmt f;
	char *s;
	int i;

	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		fmtstrinit(&f);
		for(i = 0; i < ni; i++)
			fmtprint(&f, "%s%d %s", i > 0 ? "\n" : "", i+1, items[i]);
		s = fmtstrflush(&f);
		setprop(n, "items", s, 0);
		free(s);
	}
	unlockmodel();
	dxsetitems(c, items, ni);
}

void
uxsetcolumns(Uxctl *c, char **titles, int *widths, int nc)
{
	Node *n;
	Fmt f;
	char *q;
	int i;

	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		fmtstrinit(&f);
		for(i = 0; i < nc; i++){
			q = quoteval(titles[i]);
			fmtprint(&f, "%s%d %s width=%d", i > 0 ? "\n" : "", i+1, q, widths != nil ? widths[i] : 0);
			free(q);
		}
		q = fmtstrflush(&f);
		setprop(n, "columns", q, 0);
		free(q);
	}
	unlockmodel();
	dxsetcolumns(c, titles, widths, nc);
}

/*
 * Icon views: the items file is icon-items (components.md):
 * id label=text icon=name pos=x,y [selected]
 */
static void
iconitems(Cc *c, Node *n)
{
	Fmt f;
	char *l, *ic, *t, **ids;
	int i, j, ns, sel;

	ns = uxicongetsel(c->c, &ids);
	fmtstrinit(&f);
	for(i = 0; i < c->nic; i++){
		t = smprint("label=%s", c->ic[i].label != nil ? c->ic[i].label : "");
		l = quoteval(t);
		free(t);
		t = smprint("icon=%s", c->ic[i].icon != nil ? c->ic[i].icon : "");
		ic = quoteval(t);
		free(t);
		sel = 0;
		for(j = 0; j < ns; j++)
			if(strcmp(ids[j], c->ic[i].id) == 0)
				sel = 1;
		fmtprint(&f, "%s%s %s %s pos=%d,%d%s", i > 0 ? "\n" : "", c->ic[i].id, l, ic,
			c->ic[i].pt.x, c->ic[i].pt.y, sel ? " selected" : "");
		free(l);
		free(ic);
	}
	for(j = 0; j < ns; j++)
		free(ids[j]);
	free(ids);
	t = fmtstrflush(&f);
	setprop(n, "items", t, 0);
	free(t);
}

static Ic*
icof(Cc *c, char *id)
{
	int i;

	for(i = 0; i < c->nic; i++)
		if(strcmp(c->ic[i].id, id) == 0)
			return &c->ic[i];
	return nil;
}

void
uxiconadd(Uxctl *c, char *id, char *label, char *icon, Point pt)
{
	Node *n;
	Cc *cc;
	Ic *ic;

	dxiconadd(c, id, label, icon, pt);
	lockmodel();
	if((n = nodeof(c, &cc)) != nil){
		if((ic = icof(cc, id)) == nil){
			cc->ic = erealloc(cc->ic, (cc->nic+1)*sizeof(Ic));
			ic = &cc->ic[cc->nic++];
			memset(ic, 0, sizeof *ic);
			ic->id = estrdup(id);
		}
		setstr(&ic->label, label);
		setstr(&ic->icon, icon);
		ic->pt = pt;
		iconitems(cc, n);
	}
	unlockmodel();
}

void
uxicondel(Uxctl *c, char *id)
{
	Node *n;
	Cc *cc;
	int i;

	dxicondel(c, id);
	lockmodel();
	if((n = nodeof(c, &cc)) != nil){
		for(i = 0; i < cc->nic; i++)
			if(strcmp(cc->ic[i].id, id) == 0){
				free(cc->ic[i].id);
				free(cc->ic[i].label);
				free(cc->ic[i].icon);
				memmove(cc->ic+i, cc->ic+i+1, (cc->nic-i-1)*sizeof(Ic));
				cc->nic--;
				break;
			}
		iconitems(cc, n);
	}
	unlockmodel();
}

void
uxiconclear(Uxctl *c)
{
	Node *n;
	Cc *cc;

	dxiconclear(c);
	lockmodel();
	if((n = nodeof(c, &cc)) != nil){
		freeic(cc);
		iconitems(cc, n);
	}
	unlockmodel();
}

void
uxiconset(Uxctl *c, char *id, char *label, char *icon, Point pt)
{
	Node *n;
	Cc *cc;
	Ic *ic;

	dxiconset(c, id, label, icon, pt);
	lockmodel();
	if((n = nodeof(c, &cc)) != nil && (ic = icof(cc, id)) != nil){
		if(label != nil)
			setstr(&ic->label, label);
		if(icon != nil)
			setstr(&ic->icon, icon);
		ic->pt = pt;
		iconitems(cc, n);
	}
	unlockmodel();
}

void
uxiconselect(Uxctl *c, char **ids, int ni)
{
	Node *n;
	Cc *cc;

	dxiconselect(c, ids, ni);
	lockmodel();
	if((n = nodeof(c, &cc)) != nil)
		iconitems(cc, n);
	unlockmodel();
}

/* consoles: value is the output, as the console keeps it */
enum { Maxcons = 64*1024 };

void
uxconswrite(Uxctl *c, char *s, long ns)
{
	Node *n;
	char *v, *t;
	long l;

	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		v = getprop(n, "value");
		l = v != nil ? strlen(v) : 0;
		t = emalloc(l+ns+1);
		if(l > 0)
			memmove(t, v, l);
		memmove(t+l, s, ns);
		t[l+ns] = 0;
		l += ns;
		if(l > Maxcons)
			memmove(t, t+l-Maxcons, Maxcons+1);
		setprop(n, "value", t, 0);
		free(t);
	}
	unlockmodel();
	dxconswrite(c, s, ns);
}

void
uxconsclear(Uxctl *c)
{
	Node *n;
	char *v, *p;

	lockmodel();
	if((n = nodeof(c, nil)) != nil && (v = getprop(n, "value")) != nil){
		v = estrdup(v);
		p = strrchr(v, '\n');
		setprop(n, "value", p != nil ? p+1 : v, 0);
		free(v);
	}
	unlockmodel();
	dxconsclear(c);
}

void
uxconsraw(Uxctl *c, int on)
{
	Node *n;

	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		if(on)
			setprop(n, "raw", "on", 1);
		else
			rmprop(n, "raw");
	}
	unlockmodel();
	dxconsraw(c, on);
}

/*
 * 9P's changes, onto the controls: run by trender's sync, on the UI
 * thread with the model held.
 */
static void
capisync1(void)
{
	Cc *c;
	Cw *w;
	Win *mw;
	Node *n;
	char *v;
	int off;

	for(w = cws; w != nil; w = w->next){
		if((mw = winbyname(w->name)) == nil)
			continue;
		if(w->title == nil || strcmp(w->title, mw->title) != 0){
			setstr(&w->title, mw->title);
			dxwinsettitle(w->w, mw->title);
		}
	}
	for(c = ccs; c != nil; c = c->next){
		if((n = nodebyid(c->node)) == nil)
			continue;
		if(labelprop[c->kind] != nil){
			v = getprop(n, labelprop[c->kind]);
			if(v == nil)
				v = "";
			if(!streq(v, c->label != nil ? c->label : "")){
				setstr(&c->label, v);
				dxsetlabel(c->c, v);
			}
		}
		if(c->kind == Kcheck || c->kind == Ktext || c->kind == Klabel){
			v = getprop(n, c->kind == Klabel ? "text" : "value");
			if(v == nil)
				v = "";
			if(!streq(v, c->value != nil ? c->value : "")){
				setstr(&c->value, v);
				dxsetvalue(c->c, v);
			}
		}
		off = getprop(n, "disabled") != nil;
		if(off != c->off){
			c->off = off;
			dxenable(c->c, !off);
		}
	}
}

void
capisync(void)
{
	capisync1();
}

/* a node's bounds, for the bounds file: nil if it isn't the API's */
char*
capibounds(Node *n)
{
	Cc *c;
	Cw *w;
	Win *mw;
	Rectangle r;
	Point p;

	if(n->parent == nil){
		mw = n->win;
		if(mw == nil || !mw->capi || (w = cwbyname(mw->name)) == nil)
			return nil;
		p = uxwinsize(w->w);
		return smprint("0 0 %d %d", p.x, p.y);
	}
	if((c = ccbynode(n->id)) == nil)
		return nil;
	r = uxctlrect(c->c);
	return smprint("%d %d %d %d", r.min.x, r.min.y, Dx(r), Dy(r));
}
