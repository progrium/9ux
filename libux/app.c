/*
 * The app and its UI loop, and the public API.
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

static void
kick(void)
{
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
}

void
uxinit(char *name, char *title)
{
	char *s;

	if(!_validid(name) || strchr(name, '.') || strchr(name, '_'))
		sysfatal("libux: bad app name %q", name);
	quotefmtinstall();
	ux.name = estrdup(name);
	ux.title = estrdup(title ? title : name);
	ux.user = estrdup(getuser());
	ux.menufd = -1;
	ux.regfd = -1;
	s = getenv("uxheadless");
	ux.headless = s != nil && strcmp(s, "1") == 0;
	free(s);
	ux.uikick = chancreate(sizeof(ulong), 1);
	ux.menuc = chancreate(sizeof(char*), 32);
	ux.cmdtail = &ux.cmdq;
	ux.notetail = &ux.notes;
	_startresponder();
}

enum { Akick, Amenu, Nfixed };

static void
setalt(Alt *a, Channel *c, void *v, int op)
{
	memset(a, 0, sizeof *a);
	a->c = c;
	a->v = v;
	a->op = op;
}


void
uxmain(void)
{
	Alt *a;
	Mouse m;
	Rune r;
	char *line;
	ulong k;
	int i, n, rz, which, nw;
	Uxwin **ws;

	qlock(&uxlk);
	_register();
	_menuregister();
	qunlock(&uxlk);
	a = nil;
	ws = nil;
	for(;;){
		qlock(&uxlk);
		_docmds();
		if(ux.rebar){
			ux.rebar = 0;
			_menuregister();
		}
		qunlock(&uxlk);
		_runnotes();
		qlock(&uxlk);
		_docmds();
		_drawdirty();
		if(ux.quit){
			qunlock(&uxlk);
			break;
		}
		nw = ux.nwin;
		a = erealloc(a, (Nfixed + 3*nw + 1)*sizeof(Alt));
		ws = erealloc(ws, (nw+1)*sizeof(Uxwin*));
		memset(a, 0, (Nfixed + 3*nw + 1)*sizeof(Alt));
		setalt(&a[Akick], ux.uikick, &k, CHANRCV);
		setalt(&a[Amenu], ux.menuc, &line, CHANRCV);
		n = Nfixed;
		for(i = 0; i < nw; i++){
			ws[i] = ux.win[i];
			if(ws[i]->mc == nil){
				setalt(&a[n++], nil, nil, CHANNOP);
				setalt(&a[n++], nil, nil, CHANNOP);
				setalt(&a[n++], nil, nil, CHANNOP);
				continue;
			}
			setalt(&a[n++], ws[i]->mc->c, &m, CHANRCV);
			setalt(&a[n++], ws[i]->mc->resizec, &rz, CHANRCV);
			setalt(&a[n++], ws[i]->kc->c, &r, CHANRCV);
		}
		a[n].op = CHANEND;
		qunlock(&uxlk);

		which = alt(a);

		qlock(&uxlk);
		switch(which){
		case Akick:
			break;
		case Amenu:
			_barevent(line);
			free(line);
			break;
		default:
			i = (which - Nfixed) / 3;
			if(i >= nw || ws[i]->dead)
				break;
			switch((which - Nfixed) % 3){
			case 0:
				_wininput(ws[i], &m);
				break;
			case 1:
				_winresize(ws[i]);
				break;
			case 2:
				_winkey(ws[i], r);
				break;
			}
		}
		qunlock(&uxlk);
	}
	free(a);
	free(ws);
	/* answer what's done before the app exits */
	qlock(&uxlk);
	_docmds();
	qunlock(&uxlk);
	_flushdone();
}

void
uxquit(void)
{
	qlock(&uxlk);
	ux.quit = 1;
	qunlock(&uxlk);
	kick();
}

void
uxapphandler(Uxfn *fn, void *arg)
{
	qlock(&uxlk);
	ux.fn = fn;
	ux.arg = arg;
	qunlock(&uxlk);
}

char*
uxappid(void)
{
	char *s;

	qlock(&uxlk);
	s = ux.id ? estrdup(ux.id) : smprint("%s-%d", ux.name, getpid());
	qunlock(&uxlk);
	return s;
}

void
uxsettitle(char *t)
{
	qlock(&uxlk);
	free(ux.title);
	ux.title = estrdup(t);
	_event(nil, "title", "title", t, nil);
	if(ux.menufd >= 0)
		{
		char *q = _quote(t);
		fprint(ux.menufd, "title %s\n", q);
		free(q);
	}
	qunlock(&uxlk);
}

/* windows */

Uxwin*
uxwindow(char *kind, char *title, Rectangle r)
{
	Uxwin *w;

	qlock(&uxlk);
	w = _newwin(kind ? kind : "window", title, r);
	qunlock(&uxlk);
	kick();
	return w;
}

Uxwin*
uxdialog(char *title, Rectangle r)
{
	Uxwin *w;

	qlock(&uxlk);
	w = _newwin("dialog", title, r);
	_winstate(w, w->state | Wmodal);
	_front(w);
	qunlock(&uxlk);
	kick();
	return w;
}

void
uxwinhandler(Uxwin *w, Uxfn *fn, void *arg)
{
	qlock(&uxlk);
	w->fn = fn;
	w->arg = arg;
	qunlock(&uxlk);
}

void
uxwinsettitle(Uxwin *w, char *t)
{
	int fd;

	qlock(&uxlk);
	free(w->title);
	w->title = estrdup(t);
	if(!w->dead){
		_event(w, "title", "title", t, nil);
		if(w->main && w->img){
			fd = open("/dev/label", OWRITE|OTRUNC);
			if(fd >= 0){
				fprint(fd, "%s", t);
				close(fd);
			}
		}
	}
	qunlock(&uxlk);
}

void
uxwinclose(Uxwin *w)
{
	qlock(&uxlk);
	if(!w->dead)
		_delwin(w);
	qunlock(&uxlk);
	kick();
}

int
uxwinid(Uxwin *w)
{
	return w->winid;
}

void
uxdefault(Uxwin *w, Uxctl *c)
{
	qlock(&uxlk);
	if(w->def)
		w->def->state &= ~Cdefault;
	w->def = c;
	if(c)
		c->state |= Cdefault;
	w->dirty = 1;
	qunlock(&uxlk);
	kick();
}

/* controls */

static Uxctl*
mk(Uxwin *w, char *id, int type, char *label, Rectangle r)
{
	Uxctl *c;

	qlock(&uxlk);
	c = _newctl(w, id, ctltypes[type], label, r);
	qunlock(&uxlk);
	kick();
	return c;
}

Uxctl*
uxbutton(Uxwin *w, char *id, char *label, Rectangle r)
{
	return mk(w, id, 0, label, r);
}

Uxctl*
uxtextfield(Uxwin *w, char *id, char *label, Rectangle r, int flags)
{
	Uxctl *c;

	c = mk(w, id, 1, label, r);
	qlock(&uxlk);
	c->state |= flags & (UXmultiline|UXsecret|UXsensitive|UXreadonly);
	if(c->state & UXsecret)
		c->state |= UXsensitive;
	qunlock(&uxlk);
	return c;
}

Uxctl*
uxcheckbox(Uxwin *w, char *id, char *label, Rectangle r)
{
	return mk(w, id, 2, label, r);
}

Uxctl*
uxlabel(Uxwin *w, char *id, char *text, Rectangle r)
{
	Uxctl *c;

	c = mk(w, id, 3, "", r);
	qlock(&uxlk);
	c->text = runesmprint("%s", text);
	c->ntext = runestrlen(c->text);
	qunlock(&uxlk);
	return c;
}

Uxctl*
uxgroup(Uxwin *w, char *id, char *label, Rectangle r)
{
	return mk(w, id, 4, label, r);
}

Uxctl*
uxlist(Uxwin *w, char *id, Rectangle r)
{
	Uxctl *c;

	c = mk(w, id, 5, "", r);
	qlock(&uxlk);
	c->sel = emalloc(1);
	qunlock(&uxlk);
	return c;
}

Uxctl*
uxiconview(Uxwin *w, char *id, Rectangle r)
{
	return mk(w, id, 6, "", r);
}

Uxctl*
uxscrollview(Uxwin *w, char *id, Rectangle r)
{
	Uxctl *c;

	c = mk(w, id, 7, "", r);
	qlock(&uxlk);
	c->extent = Rect(0, 0, Dx(r), Dy(r));
	qunlock(&uxlk);
	return c;
}

Uxctl*
uxfilechooser(Uxwin *w, char *id, char *dir, Rectangle r)
{
	Uxctl *c;

	c = mk(w, id, 8, "", r);
	qlock(&uxlk);
	c->dir = estrdup(dir ? dir : ".");
	cleanname(c->dir);
	c->name = estrdup("");
	_chooserload(c);
	qunlock(&uxlk);
	return c;
}

void
uxdelete(Uxctl *c)
{
	Uxwin *w;
	int i;

	qlock(&uxlk);
	if(c->dead){
		qunlock(&uxlk);
		return;
	}
	w = c->w;
	for(i = 0; i < w->nctl; i++)
		if(w->ctl[i] == c){
			memmove(w->ctl+i, w->ctl+i+1, (w->nctl-i-1)*sizeof(Uxctl*));
			w->nctl--;
			break;
		}
	for(i = 0; i < w->nctl; i++)
		if(w->ctl[i]->parent == c)
			w->ctl[i]->parent = nil;
	if(w->focus == c)
		w->focus = nil;
	if(w->def == c)
		w->def = nil;
	if(w->grab == c)
		w->grab = nil;
	_event(c, "delete", nil);
	_gone(c);
	_delobj(c);
	w->dirty = 1;
	qunlock(&uxlk);
	kick();
}

void
uxhandler(Uxctl *c, Uxfn *fn, void *arg)
{
	qlock(&uxlk);
	c->fn = fn;
	c->arg = arg;
	qunlock(&uxlk);
}

void
uxsetparent(Uxctl *c, Uxctl *p)
{
	qlock(&uxlk);
	c->parent = p;
	c->w->dirty = 1;
	qunlock(&uxlk);
	kick();
}

char*
uxctlid(Uxctl *c)
{
	return c->id;
}

char*
uxgetvalue(Uxctl *c)
{
	char *s;

	qlock(&uxlk);
	if(c->t == ctltypes[2])
		s = estrdup(c->check == 2 ? "mixed" : c->check ? "on" : "off");
	else if(c->t == ctltypes[8])
		s = estrdup(c->name);
	else
		s = _runestr(c->text, c->ntext);
	qunlock(&uxlk);
	return s;
}

void
uxsetvalue(Uxctl *c, char *v)
{
	qlock(&uxlk);
	if(c->t == ctltypes[2]){
		c->check = strcmp(v, "on") == 0 ? 1 : strcmp(v, "mixed") == 0 ? 2 : 0;
		_event(c, "value", "value", c->check == 2 ? "mixed" : c->check ? "on" : "off", nil);
		c->w->dirty = 1;
	}else if(c->t == ctltypes[1] || c->t == ctltypes[3]){
		free(c->text);
		c->text = runesmprint("%s", v);
		c->ntext = runestrlen(c->text);
		if(c->q0 > c->ntext)
			c->q0 = c->ntext;
		if(c->q1 > c->ntext)
			c->q1 = c->ntext;
		c->org = 0;
		c->w->dirty = 1;
		if(c->state & UXsecret)
			_event(c, "value", "redacted", "on", nil);
		else if(strlen(v) <= Maxevvalue && strchr(v, '\n') == nil)
			_event(c, "value", "value", v, nil);
		else
			_event(c, "value", nil);
	}
	qunlock(&uxlk);
	kick();
}

void
uxsetlabel(Uxctl *c, char *l)
{
	qlock(&uxlk);
	free(c->label);
	c->label = estrdup(l);
	c->w->dirty = 1;
	_event(c, "label", "label", l, nil);
	qunlock(&uxlk);
	kick();
}

static void
stateevent(Uxctl *c)
{
	char *s;

	s = _ctlread(c, "state");
	s[strlen(s)-1] = 0;
	_event(c, "state", "state", s, nil);
	free(s);
}

void
uxenable(Uxctl *c, int on)
{
	qlock(&uxlk);
	if(on)
		c->state |= Cenabled;
	else
		c->state &= ~Cenabled;
	c->w->dirty = 1;
	stateevent(c);
	qunlock(&uxlk);
	kick();
}

void
uxshow(Uxctl *c, int on)
{
	qlock(&uxlk);
	if(on)
		c->state |= Cvisible;
	else
		c->state &= ~Cvisible;
	c->w->dirty = 1;
	stateevent(c);
	qunlock(&uxlk);
	kick();
}

void
uxfocus(Uxctl *c)
{
	qlock(&uxlk);
	_setfocus(c->w, c, UXui);
	qunlock(&uxlk);
	kick();
}

void
uxsetitems(Uxctl *c, char **items, int n)
{
	int i;

	qlock(&uxlk);
	for(i = 0; i < c->nitems; i++)
		free(c->items[i]);
	free(c->items);
	free(c->sel);
	c->items = emalloc((n+1)*sizeof(char*));
	for(i = 0; i < n; i++)
		c->items[i] = estrdup(items[i]);
	c->nitems = n;
	c->sel = emalloc(n+1);
	c->top = 0;
	c->w->dirty = 1;
	_event(c, "items", nil);
	qunlock(&uxlk);
	kick();
}

int
uxgetsel(Uxctl *c, int **sel)
{
	int i, n;

	qlock(&uxlk);
	*sel = emalloc((c->nitems+1)*sizeof(int));
	n = 0;
	for(i = 0; i < c->nitems; i++)
		if(c->sel[i])
			(*sel)[n++] = i;
	qunlock(&uxlk);
	return n;
}

void
uxiconadd(Uxctl *c, char *id, char *label, char *icon, Point pt)
{
	Icon *ic;

	qlock(&uxlk);
	if(!_validid(id))
		sysfatal("libux: bad icon item id %q", id);
	c->icons = erealloc(c->icons, (c->nicons+1)*sizeof(Icon));
	ic = &c->icons[c->nicons++];
	memset(ic, 0, sizeof *ic);
	ic->id = estrdup(id);
	ic->label = estrdup(label);
	ic->icon = estrdup(icon);
	ic->pt = pt;
	c->w->dirty = 1;
	_event(c, "items", nil);
	qunlock(&uxlk);
	kick();
}

static void
freeicon(Icon *ic)
{
	free(ic->id);
	free(ic->label);
	free(ic->icon);
}

void
uxicondel(Uxctl *c, char *id)
{
	int i;

	qlock(&uxlk);
	for(i = 0; i < c->nicons; i++)
		if(strcmp(c->icons[i].id, id) == 0){
			freeicon(&c->icons[i]);
			memmove(c->icons+i, c->icons+i+1, (c->nicons-i-1)*sizeof(Icon));
			c->nicons--;
			c->w->dirty = 1;
			_event(c, "items", nil);
			break;
		}
	qunlock(&uxlk);
	kick();
}

void
uxiconclear(Uxctl *c)
{
	int i;

	qlock(&uxlk);
	for(i = 0; i < c->nicons; i++)
		freeicon(&c->icons[i]);
	c->nicons = 0;
	c->w->dirty = 1;
	_event(c, "items", nil);
	qunlock(&uxlk);
	kick();
}

void
uxsetextent(Uxctl *c, Rectangle r)
{
	char buf[64];

	qlock(&uxlk);
	c->extent = r;
	snprint(buf, sizeof buf, "%d,%d,%d,%d", r.min.x, r.min.y, r.max.x, r.max.y);
	_event(c, "extent", "rect", buf, nil);
	c->w->dirty = 1;
	qunlock(&uxlk);
	kick();
}

void
uxsetorigin(Uxctl *c, Point p)
{
	char buf[32];

	qlock(&uxlk);
	c->origin = p;
	snprint(buf, sizeof buf, "%d,%d", p.x, p.y);
	_event(c, "origin", "pt", buf, nil);
	c->w->dirty = 1;
	qunlock(&uxlk);
	kick();
}

Point
uxgetorigin(Uxctl *c)
{
	Point p;

	qlock(&uxlk);
	p = c->origin;
	qunlock(&uxlk);
	return p;
}

char*
uxchooserdir(Uxctl *c)
{
	char *s;

	qlock(&uxlk);
	s = estrdup(c->dir);
	qunlock(&uxlk);
	return s;
}
