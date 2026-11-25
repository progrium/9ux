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
	int	origin;	/* Kscroll: the y origin the node had */
	Rectangle	r;	/* the API's rectangle, window-local */
	Cc	*next;
};

/* a window made through the API */
struct Cw {
	char	*name;
	Uxwin	*w;
	Uxfn	*fn;
	void	*arg;
	char	*title;
	int	focus;	/* the node the control has focus for */
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
static Uxfn	*directfn;	/* the handler for the drawing layer's event (orig) */
static void	*directarg;
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
static void	hook(char*, char*, char*, char*, char*);
static void	actioncall(char*, char*, char*);
static void	menusync(void);
static void	defclose(Uxev*, void*);
void	uxwinclose(Uxwin*);
static char*	verb(Node*, Tok*, int);

/* menu.c */
extern Uxfn	*_actionhook;
char*	_uitreebar(void);
char*	_uitreemenu(int, char**);
void	_uitreeaction(char*, Uxwin*, int);
void	_uitreequit(int);

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
	if((v = attrof(attrs, "item")) != nil || (v = attrof(attrs, "items")) != nil){
		k->s[0] = v;
		k->ev.item = v;
		if(c != nil && c->kind == Klist){
			k->ev.n = atoi(v)-1;
			k->ev.item = nil;
		}
	}
	if((v = attrof(attrs, "text")) != nil){
		k->s[1] = v;
		k->ev.text = v;
	}
	if((v = attrof(attrs, "n")) != nil){
		k->ev.n = atoi(v);
		free(v);
	}
	if((v = attrof(attrs, "pos")) != nil){
		k->ev.pt.x = atoi(v);
		if(strchr(v, ',') != nil)
			k->ev.pt.y = atoi(strchr(v, ',')+1);
		free(v);
	}
	return 1;
}

/* every event in the tree passes here (tev.c's deliver), model held */
static void
hook(char *name, char *key, char *path, char *attrs, char *win)
{
	Cw *w;
	Cc *c;
	Win *mw;
	Node *n;
	Uxfn *fn;
	void *arg;
	Call *k;
	char *e;

	if(inapi)
		return;
	if(strcmp(name, "action") == 0 && key != nil && strcmp(key, "-") != 0){
		actioncall(key, attrs, win);
		return;
	}
	if(path == nil || strcmp(path, "-") == 0 || (w = cwbyname(win)) == nil)
		return;
	c = nil;
	if(strcmp(path, ".") == 0){
		fn = w->fn;
		arg = w->arg;
		if(fn == nil && strcmp(name, "close") == 0){
			fn = defclose;
			arg = w->w;
		}
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
	/* the drawing layer's own event: its handler runs now, with it */
	if(orig != nil){
		directfn = fn;
		directarg = arg;
		return;
	}
	k = emalloc(sizeof *k);
	memset(k, 0, sizeof *k);
	k->fn = fn;
	k->arg = arg;
	if(!synth(k, name, attrs, w, c)){
		free(k);
		return;
	}
	*calltail = k;
	calltail = &k->next;
	uxcall(runcalls, nil);
}

/* quit, from 9P: the app's handler decides, as with uxquit */
static void
quitasked(char *via)
{
	char *a;

	a = smprint("via=%s", via != nil ? via : "ctl");
	_uitreequit(viaof(a));
	free(a);
}

/* a window with no handler closes, as in the drawing layer; main quits */
static void
defclose(Uxev*, void *arg)
{
	Cw *w;
	int ismain;

	lockmodel();
	w = cwbywin(arg);
	ismain = w != nil && strcmp(w->name, "main") == 0;
	unlockmodel();
	if(w == nil)
		return;
	if(ismain)
		uxquit();
	else
		uxwinclose(arg);
}

/* close, from 9P: UXclose to the window's handler, which closes it or not */
static void
closeasked(Win *mw, char *via)
{
	Cw *w;
	Call *k;
	char *a;

	if((w = cwbyname(mw->name)) == nil)
		return;
	k = emalloc(sizeof *k);
	memset(k, 0, sizeof *k);
	k->fn = w->fn != nil ? w->fn : defclose;
	k->arg = w->fn != nil ? w->arg : w->w;
	k->ev.type = UXclose;
	a = smprint("via=%s", via != nil ? via : "ctl");
	k->ev.via = viaof(a);
	free(a);
	k->ev.win = w->w;
	*calltail = k;
	calltail = &k->next;
	uxcall(runcalls, nil);
}

/* an action event from the tree: a script's, or a key binding's */
static void
actioncall(char *action, char *attrs, char *win)
{
	Cw *w;

	w = cwbyname(win);
	_uitreeaction(action, w != nil ? w->w : nil, viaof(attrs));
}

/* an action chosen in the drawing layer (the bar, a shortcut): tell the tree */
static void
actionev(Uxev *e, void*)
{
	Cw *w;
	char *a;

	lockmodel();
	w = e->win != nil ? cwbywin(e->win) : nil;
	a = smprint("via=%s", e->text != nil ? e->text : "menu");
	inapi++;
	emit("action", e->action, "-", a, w != nil ? w->name : nil, nil, 0, 0);
	inapi--;
	free(a);
	unlockmodel();
}

/* the app's menus, into the tree's menu/ */
static void
menusync(void)
{
	char *bar, *id, *text;
	int i, j;

	bar = _uitreebar();
	lockmodel();
	free(app->bar);
	app->bar = bar;
	for(i = 0; (text = _uitreemenu(i, &id)) != nil; i++){
		for(j = 0; j < app->nmenu; j++)
			if(strcmp(app->menu[j].name, id) == 0)
				break;
		if(j == app->nmenu){
			app->menu = erealloc(app->menu, (app->nmenu+1)*sizeof(Menu));
			app->menu[j].name = id;
			app->menu[j].text = text;
			app->nmenu++;
		}else{
			free(id);
			free(app->menu[j].text);
			app->menu[j].text = text;
		}
	}
	unlockmodel();
}

Uxmenu*
uxmenu(char *id, char *title)
{
	Uxmenu *m;

	m = dxmenu(id, title);
	menusync();
	return m;
}

void
uxitem(Uxmenu *m, char *action, char *label, char *key)
{
	dxitem(m, action, label, key);
	menusync();
}

void
uxsep(Uxmenu *m, char *id)
{
	dxsep(m, id);
	menusync();
}

void
uxitemenable(char *action, int on)
{
	dxitemenable(action, on);
	menusync();
}

void
uxitemcheck(char *action, int on)
{
	dxitemcheck(action, on);
	menusync();
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
void	_dxlistsel(Uxctl*, int*, int);

/* a list's selection as its value: item keys, from 1, space-separated */
static char*
listvalue(Cc *c)
{
	Fmt f;
	int *sel, i, n;

	n = uxgetsel(c->c, &sel);
	fmtstrinit(&f);
	for(i = 0; i < n; i++)
		fmtprint(&f, "%s%d", i > 0 ? " " : "", sel[i]+1);
	free(sel);
	return fmtstrflush(&f);
}

static void
capictlev(Uxev *e, void *arg)
{
	Uxfn *fn;
	void *darg;
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
			if(ni > 0){
				fmtprint(&f, "sel=");
				for(i = 0; i < ni; i++)
					fmtprint(&f, "%s%s", i > 0 ? "," : "", ids[i]);
				fmtprint(&f, " ");
			}
			for(i = 0; i < ni; i++)
				free(ids[i]);
			free(ids);
			iconitems(c, n);
		}else if(c->kind == Klist){
			v = listvalue(c);
			setstr(&c->value, v);
			setprop(n, "value", v, 0);
			if(v[0] != 0){
				q = quoteval(v);
				fmtprint(&f, "value=%s ", q);
				free(q);
			}
			free(v);
		}
		break;
	case UXsubmit:
		name = "submit";
		break;
	case UXopen:
		name = "open";
		if(c->kind == Kicons && e->item != nil)
			fmtprint(&f, "items=%s ", e->item);
		else if(e->item != nil)
			fmtprint(&f, "item=%s ", e->item);
		else if(c->kind == Klist)
			fmtprint(&f, "item=%d ", e->n+1);
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
		c->origin = uxgetorigin(c->c).y;
		q = smprint("%d", c->origin);
		setprop(n, "origin", q, 0);
		free(q);
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
	directfn = nil;
	emitnode(name, winofnode(n), n, attrs, state);
	orig = nil;
	free(attrs);
	sweep();
	fn = directfn;
	darg = directarg;
	directfn = nil;
	unlockmodel();
	if(fn != nil)
		fn(e, darg);
}

static void
capiwinev(Uxev *e, void *arg)
{
	Uxfn *fn;
	void *darg;
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
	directfn = nil;
	emitnode(name, w, w->root, attrs, state);
	orig = nil;
	free(attrs);
	sweep();
	fn = directfn;
	darg = directarg;
	directfn = nil;
	unlockmodel();
	if(fn != nil)
		fn(e, darg);
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
	quithook = quitasked;
	closehook = closeasked;
	verbhook = verb;
	_actionhook = actionev;
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

/*
 * The style that places a node where the API's rectangle says: the
 * API's is window-local, a node's place is its parent's.
 */
static void
setgeom(Node *n, Rectangle r)
{
	Cc *pc;

	if(n->parent != nil && (pc = ccbynode(n->parent->id)) != nil)
		r = rectsubpt(r, pc->r.min);
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
	cc->r = r;
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
	Node *n, *k, *root;
	Win *w;
	Cc *cc;

	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		/* as the drawing layer does: the children stay, in the window */
		w = winofnode(n);
		root = w != nil ? w->root : nil;
		while(n->nkid > 0 && root != nil && (k = n->kid[0]) != nil){
			reparent(k, root);
			if((cc = ccbynode(k->id)) != nil)
				setgeom(k, cc->r);
		}
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
	Cc *cc;

	lockmodel();
	if((n = nodeof(c, nil)) != nil){
		pn = p != nil ? nodeof(p, nil) : nil;
		if(pn == nil && (w = winofnode(n)) != nil)
			pn = w->root;
		reparent(n, pn);
		if((cc = ccbyctl(c)) != nil)
			setgeom(n, cc->r);
		sweep();
	}
	unlockmodel();
	dxsetparent(c, p);
}

void
uxsetrect(Uxctl *c, Rectangle r)
{
	Node *n;
	Cc *cc;

	lockmodel();
	if((n = nodeof(c, &cc)) != nil){
		cc->r = r;
		setgeom(n, r);
	}
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
	t = fmtstrflush(&f);
	setprop(n, "items", t, 0);
	free(t);
	fmtstrinit(&f);
	for(j = 0; j < ns; j++){
		fmtprint(&f, "%s%s", j > 0 ? " " : "", ids[j]);
		free(ids[j]);
	}
	free(ids);
	t = fmtstrflush(&f);
	setprop(n, "selection", t, 0);
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

/* a list's value, onto the list */
static void
listsel(Cc *c, char *v)
{
	char *s, *f[256];
	int idx[256], i, n;

	setstr(&c->value, v);
	s = estrdup(v);
	n = getfields(s, f, nelem(f), 1, " ,");
	for(i = 0; i < n; i++)
		idx[i] = atoi(f[i])-1;
	free(s);
	_dxlistsel(c->c, idx, n);
}

void
uxfocus(Uxctl *c)
{
	Cc *cc;
	Cw *w;
	Win *mw;

	dxfocus(c);
	lockmodel();
	if((cc = ccbyctl(c)) != nil && (w = cwbyname(cc->win)) != nil && (mw = winbyname(w->name)) != nil){
		mw->focus = cc->node;
		w->focus = cc->node;
	}
	unlockmodel();
}

void
uxsetorigin(Uxctl *c, Point p)
{
	Node *n;
	Cc *cc;
	char *s;

	dxsetorigin(c, p);
	lockmodel();
	if((n = nodeof(c, &cc)) != nil){
		cc->origin = uxgetorigin(c).y;
		s = smprint("%d", cc->origin);
		setprop(n, "origin", s, 0);
		free(s);
	}
	unlockmodel();
}

/*
 * The verbs of the desktop types, from 9P: icon views and consoles.
 * (char*)-1 leaves a verb to tcmd.c. The model is held.
 */
static char*
iconverb(Cc *c, Node *n, Tok *t, int nt)
{
	Fmt f;
	Ic *ic;
	char **ids, *a, *q;
	int i;
	Point pt;

	if(strcmp(t[0].s, "select") == 0 || strcmp(t[0].s, "open") == 0){
		for(i = 1; i < nt; i++)
			if(icof(c, t[i].s) == nil)
				return smprint("not found: %s", t[i].s);
		if(strcmp(t[0].s, "open") == 0 && nt < 2)
			return smprint("bad syntax: open ID...");
		fmtstrinit(&f);
		for(i = 1; i < nt; i++)
			fmtprint(&f, "%s%s", i > 1 ? "," : "", t[i].s);
		a = fmtstrflush(&f);
		if(strcmp(t[0].s, "select") == 0){
			ids = emalloc(nt*sizeof(char*));
			for(i = 1; i < nt; i++)
				ids[i-1] = t[i].s;
			dxiconselect(c->c, ids, nt-1);
			free(ids);
			iconitems(c, n);
			q = a[0] != 0 ? smprint("sel=%s via=ctl", a) : estrdup("via=ctl");
			emitnode("select", winofnode(n), n, q, 1);
		}else{
			q = smprint("items=%s via=ctl", a);
			emitnode("open", winofnode(n), n, q, 0);
		}
		free(q);
		free(a);
		return nil;
	}
	if(strcmp(t[0].s, "move") == 0){
		if(nt != 3 || (pt.x = strtol(t[2].s, &a, 10), *a != ',') || (pt.y = strtol(a+1, &q, 10), *q != 0))
			return smprint("bad syntax: move ID x,y");
		if((ic = icof(c, t[1].s)) == nil)
			return smprint("not found: %s", t[1].s);
		ic->pt = pt;
		dxiconset(c->c, ic->id, nil, nil, pt);
		iconitems(c, n);
		q = smprint("item=%s pos=%d,%d via=ctl", ic->id, pt.x, pt.y);
		emitnode("move", winofnode(n), n, q, 1);
		free(q);
		return nil;
	}
	if(strcmp(t[0].s, "rename") == 0){
		if(nt != 3)
			return smprint("bad syntax: rename ID TEXT");
		if((ic = icof(c, t[1].s)) == nil)
			return smprint("not found: %s", t[1].s);
		setstr(&ic->label, t[2].s);
		dxiconset(c->c, ic->id, ic->label, nil, ic->pt);
		iconitems(c, n);
		a = quoteval(t[2].s);
		q = smprint("item=%s text=%s via=ctl", ic->id, a);
		emitnode("rename", winofnode(n), n, q, 0);
		free(q);
		free(a);
		return nil;
	}
	return (char*)-1;
}

static char*
consverb(Cc *c, Tok *t, int nt)
{
	if(strcmp(t[0].s, "append") == 0){
		if(nt != 2)
			return smprint("bad syntax: append TEXT");
		uxconswrite(c->c, t[1].s, strlen(t[1].s));
		return nil;
	}
	if(strcmp(t[0].s, "clear") == 0){
		uxconsclear(c->c);
		return nil;
	}
	if(strcmp(t[0].s, "insert") == 0)
		return smprint("unsupported: insert");
	return (char*)-1;
}

static char*
verb(Node *n, Tok *t, int nt)
{
	Cc *c;

	if((c = ccbynode(n->id)) == nil)
		return (char*)-1;
	if(c->kind == Kicons)
		return iconverb(c, n, t, nt);
	if(c->kind == Kcons)
		return consverb(c, t, nt);
	return (char*)-1;
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
		if(mw->focus != w->focus){
			w->focus = mw->focus;
			if((c = ccbynode(w->focus)) != nil)
				dxfocus(c->c);
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
		if(c->kind == Klist){
			v = getprop(n, "value");
			if(v == nil)
				v = "";
			if(!streq(v, c->value != nil ? c->value : ""))
				listsel(c, v);
		}
		if(c->kind == Kscroll){
			v = getprop(n, "origin");
			if(v != nil && atoi(v) != c->origin){
				c->origin = atoi(v);
				dxsetorigin(c->c, Pt(0, c->origin));
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
