/*
 * Windows: the app's own rio window, dialogs in new rio windows, and
 * headless windows when there is no display. uxlk held throughout.
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

static int
readint(char *file)
{
	char buf[32];
	int fd, n;

	fd = open(file, OREAD);
	if(fd < 0)
		return -1;
	n = read(fd, buf, sizeof buf-1);
	close(fd);
	if(n <= 0)
		return -1;
	buf[n] = 0;
	return atoi(buf);
}

static void
setlabel(char *file, char *s)
{
	int fd;

	fd = open(file, OWRITE|OTRUNC);
	if(fd >= 0){
		write(fd, s, strlen(s));
		close(fd);
	}
}

static Font*
winfont(Display *d)
{
	char *name, buf[128];
	Subfont *df;
	Font *f;

	f = nil;
	name = getenv("font");
	if(name != nil){
		f = openfont(d, name);
		free(name);
	}
	if(f == nil){
		df = getdefont(d);
		if(df == nil)
			return nil;
		d->defaultsubfont = df;
		snprint(buf, sizeof buf, "%d %d\n0 %d\t%s\n", df->height, df->ascent, df->n-1, "*default*");
		f = buildfont(d, buf, "*default*");
	}
	d->defaultfont = f;
	return f;
}

/* the app's own rio window; 0 if there is no display */
static int
mainwin(Uxwin *w)
{
	if(initdraw(nil, nil, ux.name) < 0)
		return 0;
	w->d = display;
	w->img = screen;
	w->font = font;
	_winfonts(w);
	w->mc = initmouse(nil, screen);
	w->kc = initkeyboard(nil);
	if(w->mc == nil || w->kc == nil)
		sysfatal("libux: mouse or keyboard: %r");
	w->winid = readint("/dev/winid");
	if(w->winid <= 0)
		w->winid = ++ux.nextwinid;
	w->r = insetrect(screen->r, -Borderwidth);
	w->main = 1;
	w->wctlfd = open("/dev/wctl", OWRITE);
	setlabel("/dev/label", w->title);
	return 1;
}

typedef struct Dlg Dlg;
struct Dlg {
	Uxwin	*w;
	Rectangle	r;
	Channel	*c;
	char	*err;
};

/* in its own namespace: mount a new rio window, open everything, report back */
static void
dlgproc(void *v)
{
	Dlg *g;
	Uxwin *w;
	char *wsys, buf[128];
	int fd, n;

	g = v;
	w = g->w;
	rfork(RFNAMEG);
	wsys = getenv("wsys");
	if(wsys == nil){
		g->err = "no $wsys";
		goto Out;
	}
	fd = open(wsys, ORDWR);
	free(wsys);
	if(fd < 0){
		g->err = "can't open $wsys";
		goto Out;
	}
	snprint(buf, sizeof buf, "new -r %d %d %d %d", g->r.min.x, g->r.min.y, g->r.max.x, g->r.max.y);
	if(mount(fd, -1, "/mnt/wsys", MREPL, buf) < 0){
		g->err = "can't make a window";
		goto Out;
	}
	w->d = initdisplay(nil, "/mnt/wsys", nil);
	if(w->d == nil){
		g->err = "initdisplay failed";
		goto Out;
	}
	w->font = winfont(w->d);
	if(w->font != nil)
		_winfonts(w);
	w->winnamefd = open("/mnt/wsys/winname", OREAD);
	w->wctlfd = open("/mnt/wsys/wctl", ORDWR);
	n = pread(w->winnamefd, buf, sizeof buf-1, 0);
	if(n <= 0 || w->font == nil){
		g->err = "no window";
		goto Out;
	}
	buf[n] = 0;
	w->img = namedimage(w->d, buf);
	if(w->img == nil){
		g->err = "can't get window image";
		goto Out;
	}
	w->mc = initmouse("/mnt/wsys/mouse", w->img);
	w->kc = initkeyboard("/mnt/wsys/cons");
	w->winid = readint("/mnt/wsys/winid");
	setlabel("/mnt/wsys/label", w->title);
	if(w->mc == nil || w->kc == nil || w->winid <= 0)
		g->err = "no mouse or keyboard";
Out:
	sendp(g->c, g->err);
}

/* a new rio window for w; 0 on failure */
int
_dialogwin(Uxwin *w, Rectangle r)
{
	Dlg g;

	memset(&g, 0, sizeof g);
	g.w = w;
	g.r = r;
	g.c = chancreate(sizeof(char*), 0);
	w->winnamefd = -1;
	w->wctlfd = -1;
	proccreate(dlgproc, &g, 32*1024);
	recvp(g.c);
	chanfree(g.c);
	if(g.err != nil){
		fprint(2, "libux: dialog window: %s\n", g.err);
		return 0;
	}
	if(Dx(w->img->r) > 0)
		w->r = insetrect(w->img->r, -Borderwidth);
	return 1;
}

Uxwin*
_newwin(char *kind, char *title, Rectangle r)
{
	Uxwin *w;

	w = emalloc(sizeof *w);
	w->kind = estrdup(kind);
	w->title = estrdup(title);
	w->wctlfd = -1;
	w->winnamefd = -1;
	if(!ux.headless){
		if(ux.nwin == 0 && !ux.hadmain){
			ux.hadmain = 1;
			if(!mainwin(w))
				ux.headless = 1;
		}else if(!_dialogwin(w, Dx(r) > 0 ? r : Rect(100, 100, 500, 300)))
			ux.headless = 1;
	}
	if(w->img == nil){
		w->main = ux.nwin == 0 && ux.nextwinid == 0;	/* the app's first window, as with a display */
		w->winid = ++ux.nextwinid;
		w->r = Dx(r) > 0 ? r : Rect(0, 0, 400, 300);
	}
	_addobj(w, Owin);
	ux.win = erealloc(ux.win, (ux.nwin+1)*sizeof(Uxwin*));
	ux.win[ux.nwin++] = w;
	w->dirty = 1;
	_event(w, "new", "kind", kind, nil);
	_regwin(w->winid, 1);
	_menuwin(w, 1);
	_chrome(w);
	return w;
}

void
_delwin(Uxwin *w)
{
	int i;
	Uxctl *c;

	for(i = 0; i < w->nctl; i++){
		c = w->ctl[i];
		_event(c, "delete", nil);
		_gone(c);
		_delobj(c);
	}
	_event(w, "delete", nil);
	_gone(w);
	_delobj(w);
	for(i = 0; i < ux.nwin; i++)
		if(ux.win[i] == w){
			memmove(ux.win+i, ux.win+i+1, (ux.nwin-i-1)*sizeof(Uxwin*));
			ux.nwin--;
			break;
		}
	if(ux.front == w)
		ux.front = nil;
	_regwin(w->winid, 0);
	_menuwin(w, 0);
	_resfreewin(w);
	if(w->img != nil && !w->main){
		if(w->wctlfd >= 0)
			write(w->wctlfd, "delete", 6);
		closemouse(w->mc);
		closekeyboard(w->kc);
		freefont(w->font);
		closedisplay(w->d);
		close(w->winnamefd);
		close(w->wctlfd);
	}
	w->img = nil;
	w->mc = nil;
	w->kc = nil;
}

/* the close box, or ctl close */
void
_winclose(Uxwin *w, int via)
{
	Uxev e;

	if(w->fn != nil){
		memset(&e, 0, sizeof e);
		e.type = UXclose;
		e.via = via;
		e.win = w;
		_notify(w->fn, w->arg, &e);
		return;
	}
	if(w->main)
		ux.quit = 1;
	else
		_delwin(w);
}

void
_winstate(Uxwin *w, int state)
{
	char *s;

	if(state == w->state)
		return;
	if(w->wctlfd >= 0 && (state&Whidden) != (w->state&Whidden))
		fprint(w->wctlfd, (state&Whidden) ? "hide" : "unhide");
	w->state = state;
	s = _flags(state, 1);
	_event(w, "state", "state", s, nil);
	free(s);
}

static void
setcurrent(Uxwin *w)
{
	int i;

	if(ux.front == w)
		return;
	for(i = 0; i < ux.nwin; i++)
		if(ux.win[i] != w && (ux.win[i]->state & Wcurrent))
			_winstate(ux.win[i], ux.win[i]->state & ~Wcurrent);
	for(i = 0; i < ux.nwin; i++)
		ux.win[i]->dirty = 1;	/* active and inactive windows draw differently */
	ux.front = w;
	_winstate(w, w->state | Wcurrent);
	if(w->fn != nil){
		Uxev e;

		memset(&e, 0, sizeof e);
		e.type = UXfront;
		e.via = UXui;
		e.win = w;
		_notify(w->fn, w->arg, &e);
	}
}

void
_front(Uxwin *w)
{
	if(w->wctlfd >= 0){
		if(w->state & Whidden)
			fprint(w->wctlfd, "unhide");
		fprint(w->wctlfd, "top");
		fprint(w->wctlfd, "current");
	}
	_winstate(w, w->state & ~Whidden);
	setcurrent(w);
}

static Uxwin*
modal(void)
{
	int i;

	for(i = ux.nwin-1; i >= 0; i--)
		if(ux.win[i]->state & Wmodal)
			return ux.win[i];
	return nil;
}

static Uxctl*
ctlat(Uxwin *w, Point p)
{
	int i;
	Uxctl *c;

	for(i = w->nctl-1; i >= 0; i--){
		c = w->ctl[i];
		if((c->state & Cvisible) && c->t->mouse != nil && ptinrect(p, _abs(c)))
			return c;
	}
	return nil;
}

void
_wininput(Uxwin *w, Mouse *m)
{
	Uxctl *c;
	Uxwin *mw;
	int down;

	down = (m->buttons & ~w->lastm.buttons) != 0;
	w->lastm = *m;
	if(w->grab != nil){
		if(w->grab->dead || !w->grab->t->mouse(w->grab, m))
			w->grab = nil;
		return;
	}
	if(!down)
		return;
	mw = modal();
	if(mw != nil && mw != w)
		return;
	setcurrent(w);
	c = ctlat(w, m->xy);
	if(c == nil || !(c->state & Cenabled))
		return;
	if((m->buttons & 7) && c->t->focusable && strcmp(c->t->name, "button") != 0)
		_setfocus(w, c, UXui);
	if(c->t->mouse(c, m))
		w->grab = c;
}

static int
canfocus(Uxctl *c)
{
	return c->t->focusable && (c->state & (Cenabled|Cvisible)) == (Cenabled|Cvisible);
}

void
_winkey(Uxwin *w, Rune r)
{
	Uxctl *c;
	Uxev e;
	int i, j;

	if(modal() != nil && modal() != w)
		return;
	setcurrent(w);
	c = w->focus;
	if(r == '\t' && (c == nil || !(c->state & UXmultiline))){
		i = -1;
		for(j = 0; j < w->nctl; j++)
			if(w->ctl[j] == c)
				i = j;
		for(j = 1; j <= w->nctl; j++)
			if(canfocus(w->ctl[(i+j+w->nctl) % w->nctl])){
				_setfocus(w, w->ctl[(i+j+w->nctl) % w->nctl], UXui);
				break;
			}
		return;
	}
	if(c != nil && c->t->key != nil && (c->state & Cenabled)){
		if(r == '\n' && w->def != nil && !(c->state & UXmultiline) && strcmp(c->t->name, "text-field") != 0)
			goto Default;
		if(c->t->key(c, r))
			return;
	}
Default:
	if(r == '\n' && w->def != nil && (w->def->state & Cenabled)){
		_activate(w->def, UXui);
		return;
	}
	if(r == Kesc && (strcmp(w->kind, "dialog") == 0 || strcmp(w->kind, "alert") == 0)){
		memset(&e, 0, sizeof e);
		e.type = UXcancel;
		e.via = UXui;
		e.win = w;
		_notify(w->fn, w->arg, &e);
	}
}

void
_winresize(Uxwin *w)
{
	char buf[128];
	int n;
	Rectangle old;

	if(w->main){
		if(getwindow(display, Refnone) < 0)
			sysfatal("libux: can't reattach to window: %r");
		w->img = screen;
	}else{
		n = pread(w->winnamefd, buf, sizeof buf-1, 0);
		if(n <= 0)
			return;
		buf[n] = 0;
		freeimage(w->img);
		w->img = namedimage(w->d, buf);
		if(w->img == nil){
			_delwin(w);
			return;
		}
	}
	old = w->r;
	w->r = insetrect(w->img->r, -Borderwidth);
	if(!eqrect(old, w->r)){
		snprint(buf, sizeof buf, "%d,%d,%d,%d", w->r.min.x, w->r.min.y, w->r.max.x, w->r.max.y);
		_event(w, "rect", "rect", buf, nil);
		if(w->fn != nil){
			Uxev e;

			memset(&e, 0, sizeof e);
			e.type = UXresize;
			e.via = UXui;
			e.win = w;
			_notify(w->fn, w->arg, &e);
		}
	}
	w->dirty = 1;
}
