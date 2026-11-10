/*
 * 9/UX core: mode, event streams, window-state events, the desktop.
 * See docs/protocols/devux.md.
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

int	uxmode;
Rune	uxcmdkey = Kmod4;

static	Uxsub	*subs[Nstream];
static	ulong	seq;
static	Channel	*syncc;	/* chan(int)[1] */

typedef struct Shadow Shadow;
struct Shadow
{
	int		id;
	Rectangle	r;
	char		*label;
	int		hidden;
	char		*app;
};

static	Shadow	*shadow;
static	int		nshadow;
static	int		lastinput;
static	char		*lastinapp;
static	int		lasttop;
static	Rectangle	lastscreen;
static	int		lastfgwin = -1;
static	char		*lastfgapp;

static	int		deskwin;
static	Uxfid	*deskowner;
static	Rectangle	lastdeskr;

static void	uxsyncthread(void*);

/* %q quotes spaces, quotes and control characters; nothing else */
static int
noquote(int)
{
	return 0;
}

void
uxinit(int on)
{
	quotefmtinstall();
	doquote = noquote;	/* quote only what tokenize needs (devux conventions) */
	uxmode = on;
	syncc = chancreate(sizeof(int), 1);
	threadcreate(uxsyncthread, nil, STACK);
	lastscreen = screen->r;
	lastdeskr = uxdeskrect();
	uxdraginit();
	if(uxmode)
		uxbarinit();
}

/*
 * Something about the windows may have changed; the sync thread
 * compares with what it last reported once the caller yields.
 */
void
uxchanged(void)
{
	if(syncc != nil)
		nbsendul(syncc, 1);
}

/*
 * Event streams
 */

Uxsub*
uxsubscribe(int kind)
{
	Uxsub *s;

	s = emalloc(sizeof(Uxsub));
	s->kind = kind;
	s->wake = chancreate(sizeof(int), 1);
	s->ref = 1;
	s->next = subs[kind];
	subs[kind] = s;
	s->q[s->nq++] = smprint("%lud resync\n", ++seq);
	return s;
}

void
uxsubfree(Uxsub *s)
{
	int i;

	if(--s->ref > 0)
		return;
	for(i=0; i<s->nq; i++)
		free(s->q[i]);
	chanfree(s->wake);
	free(s);
}

void
uxunsubscribe(Uxsub *s)
{
	Uxsub **l;

	for(l=&subs[s->kind]; *l; l=&(*l)->next)
		if(*l == s){
			*l = s->next;
			break;
		}
	s->dead = 1;
	nbsendul(s->wake, 1);
	uxsubfree(s);
}

static void
enqueue(Uxsub *s, ulong sq, char *msg)
{
	int i, n;

	if(s->nq == Maxqueue){
		n = s->nq+1;
		for(i=0; i<s->nq; i++)
			free(s->q[i]);
		s->nq = 0;
		s->q[s->nq++] = smprint("%lud lost n=%d\n", sq, n);
	}else
		s->q[s->nq++] = smprint("%lud %s\n", sq, msg);
	nbsendul(s->wake, 1);
}

void
uxpost(int kind, char *fmt, ...)
{
	va_list arg;
	char *msg;
	Uxsub *s;

	va_start(arg, fmt);
	msg = vsmprint(fmt, arg);
	va_end(arg);
	if(msg == nil)
		return;
	seq++;
	for(s=subs[kind]; s; s=s->next)
		enqueue(s, seq, msg);
	free(msg);
}

/* " name=value", quoted as needed; nothing for a nil value */
void
fmtattr(Fmt *f, char *name, char *val)
{
	char *t;

	if(val == nil)
		return;
	t = smprint("%s=%s", name, val);
	fmtprint(f, " %q", t);
	free(t);
}

/* a window label as one line of text */
char*
uxlabel(Window *w)
{
	char *s, *p;

	s = estrdup(w->label != nil ? w->label : "");
	for(p=s; *p; p++)
		if(*p == '\n' || *p == '\r' || *p == '\t')
			*p = ' ';
	return s;
}

Window*
uxwin(int id)
{
	Window *w;

	w = wlookid(id);
	if(w == nil || w->deleted || w->i == nil)
		return nil;
	return w;
}

static int
streq(char *a, char *b)
{
	if(a == nil || b == nil)
		return a == b;
	return strcmp(a, b) == 0;
}

static char*
dupnil(char *s)
{
	return s != nil ? estrdup(s) : nil;
}

static Shadow*
findshadow(Shadow *s, int n, int id)
{
	int i;

	for(i=0; i<n; i++)
		if(s[i].id == id)
			return &s[i];
	return nil;
}

static void
postwin(char *ev, int id, Rectangle *r, char *label, char *app, int withapp)
{
	Fmt f;
	char *s;

	fmtstrinit(&f);
	fmtprint(&f, "%s win=%d", ev, id);
	if(r != nil)
		fmtprint(&f, " rect=%d,%d,%d,%d", r->min.x, r->min.y, r->max.x, r->max.y);
	if(label != nil)
		fmtattr(&f, "label", label);
	if(withapp)
		fmtattr(&f, "app", app);
	s = fmtstrflush(&f);
	uxpost(Sux, "%s", s);
	free(s);
}

static void	deskrelease(int);

/*
 * Compare the windows with what was last reported, and report the
 * differences on /dev/ux/events (and the foreground app on the menu
 * bar's events).
 */
static void
uxsync(void)
{
	int i, n, top, inid;
	Window *w, *tw;
	Shadow *ns, *s, *o;
	Uxapp *fg;
	char *app;
	Rectangle r;

	if(!eqrect(screen->r, lastscreen)){
		lastscreen = screen->r;
		uxpost(Sux, "screen rect=%d,%d,%d,%d", lastscreen.min.x, lastscreen.min.y,
			lastscreen.max.x, lastscreen.max.y);
	}
	if(deskwin != 0 && uxwin(deskwin) == nil)
		deskrelease(0);
	r = uxdeskrect();
	if(!eqrect(r, lastdeskr)){
		lastdeskr = r;
		uxpost(Sdesk, "resize rect=%d,%d,%d,%d", r.min.x, r.min.y, r.max.x, r.max.y);
	}

	ns = emalloc((nwindow+1)*sizeof(Shadow));
	n = 0;
	for(i=0; i<nwindow; i++){
		w = window[i];
		if(w->deleted || w->i == nil)
			continue;
		s = &ns[n++];
		s->id = w->id;
		s->r = w->i->r;
		s->label = uxlabel(w);
		s->hidden = Dx(w->screenr) <= 0;
		s->app = dupnil(uxappid(w->id));
	}

	/* windows that went away */
	for(i=0; i<nshadow; i++){
		o = &shadow[i];
		if(findshadow(ns, n, o->id) != nil)
			continue;
		if(o->app != nil)
			postwin("app", o->id, nil, nil, nil, 0);
		postwin("delete", o->id, nil, nil, nil, 0);
		uxappdelwin(o->id);
	}

	/* new and changed windows */
	for(i=0; i<n; i++){
		s = &ns[i];
		o = findshadow(shadow, nshadow, s->id);
		if(o == nil){
			postwin("new", s->id, &s->r, s->label, nil, 0);
			if(s->hidden)
				postwin("hide", s->id, nil, nil, nil, 0);
			if(s->app != nil)
				postwin("app", s->id, nil, nil, s->app, 1);
			continue;
		}
		if(!streq(o->label, s->label))
			postwin("label", s->id, nil, s->label, nil, 0);
		if(!eqrect(o->r, s->r))
			postwin("rect", s->id, &s->r, nil, nil, 0);
		if(o->hidden != s->hidden)
			postwin(s->hidden ? "hide" : "unhide", s->id, nil, nil, nil, 0);
		if(!streq(o->app, s->app))
			postwin("app", s->id, nil, nil, s->app, 1);
	}

	/* the topmost ordinary window */
	top = 0;
	tw = nil;
	for(i=0; i<nwindow; i++){
		w = window[i];
		if(w->deleted || w->i == nil || Dx(w->screenr) <= 0 || uxisdesktop(w))
			continue;
		if(tw == nil || w->topped > tw->topped)
			tw = w;
	}
	if(tw != nil)
		top = tw->id;
	if(top != 0 && top != lasttop)
		postwin("top", top, nil, nil, nil, 0);
	lasttop = top;

	/* keyboard focus */
	inid = 0;
	if(input != nil && !input->deleted && input->i != nil)
		inid = input->id;
	app = uxappid(inid);
	if(inid != lastinput || !streq(app, lastinapp)){
		postwin("focus", inid, nil, nil, app, 1);
		lastinput = inid;
		free(lastinapp);
		lastinapp = dupnil(app);
	}

	/* the foreground app, for the menu bar */
	inid = uxforeground(&fg);
	app = fg != nil ? fg->id : nil;
	if(inid != lastfgwin || !streq(app, lastfgapp)){
		lastfgwin = inid;
		free(lastfgapp);
		lastfgapp = dupnil(app);
		if(app != nil)
			uxpost(Smenu, "focus app=%s win=%d", app, inid);
		else
			uxpost(Smenu, "focus win=%d", inid);
		uxbardraw();
	}

	for(i=0; i<nshadow; i++){
		free(shadow[i].label);
		free(shadow[i].app);
	}
	free(shadow);
	shadow = ns;
	nshadow = n;
}

static void
uxsyncthread(void*)
{
	threadsetname("uxsync");
	for(;;){
		recvul(syncc);
		uxsync();
	}
}

/*
 * The windows file: every window, bottom to top.
 */
static int
toppedcmp(void *a, void *b)
{
	Window *x, *y;

	x = *(Window**)a;
	y = *(Window**)b;
	if(uxisdesktop(x))
		return -1;
	if(uxisdesktop(y))
		return 1;
	return x->topped - y->topped;
}

char*
uxwindows(int *np)
{
	Window **ws;
	Window *w;
	int i, n;
	Fmt f;
	char *s, *st;

	ws = emalloc((nwindow+1)*sizeof(Window*));
	n = 0;
	for(i=0; i<nwindow; i++)
		if(!window[i]->deleted && window[i]->i != nil)
			ws[n++] = window[i];
	qsort(ws, n, sizeof ws[0], toppedcmp);
	fmtstrinit(&f);
	for(i=0; i<n; i++){
		w = ws[i];
		fmtprint(&f, "window win=%d rect=%d,%d,%d,%d", w->id,
			w->i->r.min.x, w->i->r.min.y, w->i->r.max.x, w->i->r.max.y);
		s = uxlabel(w);
		fmtattr(&f, "label", s);
		free(s);
		fmtattr(&f, "app", uxappid(w->id));
		st = smprint("%s%s%s%s%s",
			w == input ? "current" : "",
			w == input && (Dx(w->screenr) <= 0 || uxisdesktop(w)) ? "," : "",
			Dx(w->screenr) <= 0 ? "hidden" : "",
			Dx(w->screenr) <= 0 && uxisdesktop(w) ? "," : "",
			uxisdesktop(w) ? "desktop" : "");
		fmtprint(&f, " state=%s\n", st);
		free(st);
	}
	free(ws);
	s = fmtstrflush(&f);
	*np = strlen(s);
	return s;
}

/*
 * Mode and settings: /dev/ux/ctl
 */
static char*
keyname(Rune r)
{
	switch(r){
	case Kmod4:	return "mod4";
	case Kalt:	return "alt";
	case Kctl:	return "ctl";
	}
	return "none";
}

char*
uxctlread(int *np)
{
	char *s;

	s = smprint("mode %s\ncmdkey %s\n", uxmode ? "on" : "off", keyname(uxcmdkey));
	*np = strlen(s);
	return s;
}

void	uxdeskfit(void);

void
uxsetmode(int on)
{
	if(on == uxmode)
		return;
	uxmode = on;
	if(uxmode)
		uxbarinit();
	else
		uxbarfree();
	uxdeskfit();
	uxpost(Sux, "mode ux=%s", on ? "on" : "off");
	uxchanged();
}

char*
uxctlwrite(char *line)
{
	char *f[4];
	int nf;

	nf = tokenize(line, f, nelem(f));
	if(nf == 0)
		return nil;
	if(strcmp(f[0], "mode") == 0){
		if(nf != 2)
			return "bad syntax";
		if(strcmp(f[1], "on") == 0)
			uxsetmode(1);
		else if(strcmp(f[1], "off") == 0)
			uxsetmode(0);
		else
			return "bad value";
		return nil;
	}
	if(strcmp(f[0], "cmdkey") == 0){
		if(nf != 2)
			return "bad syntax";
		if(strcmp(f[1], "mod4") == 0)
			uxcmdkey = Kmod4;
		else if(strcmp(f[1], "alt") == 0)
			uxcmdkey = Kalt;
		else if(strcmp(f[1], "ctl") == 0)
			uxcmdkey = Kctl;
		else if(strcmp(f[1], "none") == 0)
			uxcmdkey = 0;
		else
			return "bad value";
		return nil;
	}
	return "unknown command";
}

/*
 * The desktop
 */
int
uxisdesktop(Window *w)
{
	return w != nil && deskwin != 0 && w->id == deskwin;
}

int
uxdesktopid(void)
{
	return deskwin;
}

Rectangle
uxdeskrect(void)
{
	Rectangle r;

	r = screen->r;
	if(uxmode)
		r.min.y += uxbarh;
	return r;
}

/*
 * Put the desktop window where the mode says: covering the desktop
 * rectangle with its border outside it, or hidden.
 */
void
uxdeskfit(void)
{
	Window *w;
	Image *i;
	Rectangle r;

	w = uxwin(deskwin);
	if(w == nil)
		return;
	incref(w);
	if(uxmode){
		r = insetrect(uxdeskrect(), -Borderwidth);
		if(eqrect(r, w->screenr)){
			wdeskbottom();
		}else{
			i = allocwindow(wscreen, r, Refbackup, DNofill);
			if(i != nil)
				wsendctlmesg(w, Reshaped, r, i);
		}
	}else if(Dx(w->screenr) > 0){
		wuncurrent(w);
		i = allocimage(display, w->screenr, w->i->chan, 0, DNofill);
		if(i != nil)
			wsendctlmesg(w, Reshaped, ZR, i);
	}
	wclose(w);
}

static void
deskrelease(int restore)
{
	Window *w;
	Image *i;
	int id;

	id = deskwin;
	if(id == 0)
		return;
	deskwin = 0;
	if(deskowner != nil)
		deskowner->claim = 0;
	deskowner = nil;
	w = uxwin(id);
	if(restore && w != nil && Dx(w->screenr) <= 0){
		/* hidden while the mode was off: show it again */
		incref(w);
		i = allocwindow(wscreen, w->i->r, Refbackup, DNofill);
		if(i != nil)
			wsendctlmesg(w, Reshaped, i->r, i);
		wclose(w);
	}
	uxpost(Sdesk, "release win=%d", id);
	uxpost(Sux, "desktop win=0");
	uxchanged();
}

char*
uxdeskctl(Uxfid *u, Window *own, char *line)
{
	char *f[4];
	int nf, id;
	Window *w;

	nf = tokenize(line, f, nelem(f));
	if(nf == 0)
		return nil;
	if(strcmp(f[0], "claim") == 0){
		if(nf > 2)
			return "bad syntax";
		if(nf == 2){
			id = atoi(f[1]);
			w = uxwin(id);
		}else{
			if(own == nil)
				return "bad syntax: no window";
			w = own->deleted || own->i == nil ? nil : own;
		}
		if(w == nil)
			return "not found";
		if(deskwin != 0){
			if(deskowner == u && deskwin == w->id)
				return nil;
			return "in use";
		}
		deskwin = w->id;
		deskowner = u;
		u->claim = 1;
		uxdeskfit();
		uxpost(Sdesk, "claim win=%d", deskwin);
		uxpost(Sux, "desktop win=%d", deskwin);
		uxchanged();
		return nil;
	}
	if(strcmp(f[0], "release") == 0){
		if(nf != 1)
			return "bad syntax";
		if(deskowner != u)
			return "not found";
		deskrelease(1);
		return nil;
	}
	return "unknown command";
}

void
uxdeskclose(Uxfid *u)
{
	if(u->claim && deskowner == u)
		deskrelease(1);
}

char*
uxdeskstate(int *np)
{
	Rectangle r;
	char *s;

	r = uxdeskrect();
	s = smprint("win=%d rect=%d,%d,%d,%d\n", deskwin, r.min.x, r.min.y, r.max.x, r.max.y);
	*np = strlen(s);
	return s;
}

/*
 * wctl and the user can't move, resize, hide or delete the desktop
 * window; its own client can still delete it.
 */
int
uxrefuse(Window *w, Window *own, int del)
{
	if(!uxisdesktop(w))
		return 0;
	if(del && own == w)
		return 0;
	return 1;
}

/* the screen changed size: refit the bar and the desktop */
void
uxresized(void)
{
	if(uxmode){
		uxbarfree();
		uxbarinit();
	}
	uxdeskfit();
	uxchanged();
}
