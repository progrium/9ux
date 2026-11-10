/*
 * 9/UX menu bar: drawing, mouse tracking and Command shortcuts.
 * See docs/protocols/devux.md#menu-bar.
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

int	uxbarh;

static	Image	*bar;
static	Rectangle	barr;

enum
{
	Tpad		= UXmenubartitlepad,	/* space either side of a title */
	Maxlevel	= 8,
};

#define	F	uxsysfont

/* a title in the bar: the rio menu, the app title, or an app menu */
typedef struct Title Title;
struct Title
{
	Rectangle	r;
	char		*text;
	Uxmenu	*m;	/* nil for the app title */
	int		rio;
};

/* what the bar shows now */
static	Title	titles[64];
static	int		ntitles;
static	Uxmenu	*shown;		/* copies of the shown app menus */
static	Uxapp	*shownapp;
static	char		*shownappid;
static	char		*shownlabel;	/* a legacy window's label, as shown */
static	int		shownwin;
static	Uxmenu	*riomenu;

/* an open drop-down */
typedef struct Drop Drop;
struct Drop
{
	Image	*i;
	Rectangle	r;
	Uxmenu	*m;
	int		sel;	/* highlighted item index, -1 for none */
	int		n;
	Rectangle	*ir;	/* item rectangles */
	Uxitem	**it;
};

static	Drop		drops[Maxlevel];
static	int		ndrops;

/* the clock at the right of the bar: on, off, or frozen at some text */
static	int		clockmode = 1;
static	char		*clockfixed;
static	Channel	*clocktick;
static	Rectangle	clockr;

Uxmenu*	uxbarmenus(Uxapp*, int);
Uxmenu*	uxmenufor(Uxapp*, char*, int);
Uxmenu*	uxdupmenu(Uxmenu*);
Uxitem*	uxdupitem(Uxitem*);
void		uxfreeitem(Uxitem*);
void		uxfreemenus(Uxmenu*);
int		uxmatchkey(Rune, int);

static int
itemh(void)
{
	return UXmenuitemheight;
}

/* once a minute, just after the minute turns */
static void
clockproc(void*)
{
	threadsetname("uxclock");
	for(;;){
		sleep((60 - time(0)%60)*1000 + 50);
		sendul(clocktick, 1);
	}
}

static void
clockthread(void*)
{
	threadsetname("uxclockdraw");
	for(;;){
		recvul(clocktick);
		uxbardraw();
	}
}

static char*
clocktext(char *buf, int n)
{
	static char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	Tm *tm;

	if(clockmode == 0)
		return nil;
	if(clockfixed != nil)
		return clockfixed;
	tm = localtime(time(0));
	snprint(buf, n, "%s %02d:%02d", days[tm->wday%7], tm->hour, tm->min);
	return buf;
}

/* the clock, right-aligned in the system font (assets/README.md, The menu bar) */
static void
drawclock(void)
{
	char buf[32], *s;
	Point p;
	int w;

	clockr = ZR;
	s = clocktext(buf, sizeof buf);
	if(s == nil || s[0] == 0)
		return;
	w = stringwidth(F, s);
	p = Pt(barr.max.x-UXmenubarclockpad-w, barr.min.y+(Dy(barr)-1-F->height)/2);
	clockr = Rect(p.x, barr.min.y, barr.max.x, barr.max.y-1);
	string(bar, p, uxcolor(UXink), ZP, F, s);
}

/* clock on, clock off, or clock at text... (frozen, for screenshots and tests) */
char*
uxclockctl(char **f, int nf)
{
	char *s;
	int i;

	if(nf == 2 && strcmp(f[1], "on") == 0){
		clockmode = 1;
		free(clockfixed);
		clockfixed = nil;
	}else if(nf == 2 && strcmp(f[1], "off") == 0)
		clockmode = 0;
	else if(nf >= 3 && strcmp(f[1], "at") == 0){
		clockmode = 1;
		free(clockfixed);
		clockfixed = estrdup(f[2]);
		for(i = 3; i < nf; i++){
			s = clockfixed;
			clockfixed = smprint("%s %s", s, f[i]);
			free(s);
		}
	}else
		return "bad syntax";
	uxbardraw();
	return nil;
}

char*
uxclockstate(void)
{
	if(clockmode == 0)
		return "off";
	if(clockfixed != nil)
		return clockfixed;
	return "on";
}

void
uxbarinit(void)
{
	uxstyleinit();
	if(clocktick == nil){
		clocktick = chancreate(sizeof(ulong), 0);
		proccreate(clockproc, nil, STACK);
		threadcreate(clockthread, nil, STACK);
	}
	uxbarh = UXmenubarheight;
	barr = screen->r;
	barr.max.y = barr.min.y+uxbarh;
	bar = allocwindow(wscreen, barr, Refbackup, DNofill);
	uxbardraw();
}

void
uxbarfree(void)
{
	if(bar != nil){
		freeimage(bar);
		bar = nil;
		flushimage(display, 1);
	}
}

void
uxbarontop(void)
{
	if(bar != nil)
		topwindow(bar);
}

int
uxbarhit(Point p)
{
	return uxmode && bar != nil && ptinrect(p, barr);
}

/* the rio menu: rio's own commands and the windows */
static Uxitem*
mkitem(char *action, char *label, int sep)
{
	Uxitem *i;

	i = emalloc(sizeof(Uxitem));
	i->action = estrdup(action);
	i->label = label != nil ? estrdup(label) : nil;
	i->enabled = 1;
	i->sep = sep;
	return i;
}

static Uxmenu*
mkriomenu(void)
{
	static char *cmds[] = { "New", "Resize", "Move", "Delete", "Hide" };
	Uxmenu *m;
	Uxitem **l;
	Window *w;
	char buf[32], *lab;
	int i, nhid;

	m = emalloc(sizeof(Uxmenu));
	m->id = estrdup("rio");
	m->title = estrdup("rio");
	m->top = 1;
	l = &m->items;
	for(i=0; i<nelem(cmds); i++){
		snprint(buf, sizeof buf, "rio.%d", i);
		*l = mkitem(buf, cmds[i], 0);
		l = &(*l)->next;
	}
	*l = mkitem("rio.s1", nil, 1);
	l = &(*l)->next;
	nhid = 0;
	for(i=0; i<nwindow; i++){
		w = window[i];
		if(w->deleted || w->i == nil || uxisdesktop(w))
			continue;
		if(Dx(w->screenr) <= 0){
			nhid++;
			continue;
		}
		snprint(buf, sizeof buf, "rio.w%d", w->id);
		lab = w->label != nil ? w->label : "";
		*l = mkitem(buf, lab, 0);
		if(w == input)
			(*l)->check = Chkon;
		l = &(*l)->next;
	}
	if(nhid > 0){
		*l = mkitem("rio.s2", nil, 1);
		l = &(*l)->next;
		for(i=0; i<nwindow; i++){
			w = window[i];
			if(w->deleted || w->i == nil || uxisdesktop(w) || Dx(w->screenr) > 0)
				continue;
			snprint(buf, sizeof buf, "rio.w%d", w->id);
			lab = smprint("Unhide %s", w->label != nil ? w->label : "");
			*l = mkitem(buf, lab, 0);
			free(lab);
			l = &(*l)->next;
		}
	}
	*l = mkitem("rio.s3", nil, 1);
	l = &(*l)->next;
	*l = mkitem("rio.exit", "Exit", 0);
	return m;
}

static void
addtitle(int *x, char *text, Uxmenu *m, int rio)
{
	Title *t;

	if(ntitles == nelem(titles))
		return;
	t = &titles[ntitles++];
	t->text = text;
	t->m = m;
	t->rio = rio;
	t->r = Rect(*x, barr.min.y, *x+Tpad+stringwidth(F, text)+Tpad, barr.max.y-1);
	*x = t->r.max.x;
}

/*
 * The mark (the rio menu), then the foreground app's name and its
 * menus. A window with no app shows its label, which also opens the
 * rio menu.
 */
static void
layout(void)
{
	Uxapp *a;
	Uxmenu *m;
	Window *w;
	int x, win;

	uxfreemenus(shown);
	shown = nil;
	free(shownappid);
	shownappid = nil;
	free(shownlabel);
	shownlabel = nil;
	ntitles = 0;
	win = uxforeground(&a);
	shownapp = a;
	shownwin = win;

	titles[0].text = "";
	titles[0].m = nil;
	titles[0].rio = 1;
	titles[0].r = Rect(barr.min.x+UXmenubarmarkx-Tpad/2, barr.min.y,
		barr.min.x+UXmenubarmarkx+16+Tpad/2, barr.max.y-1);
	ntitles = 1;

	x = barr.min.x+UXmenubarfirsttitle-Tpad;
	if(a != nil){
		shownlabel = estrdup(a->title != nil ? a->title : "");
		shownappid = estrdup(a->id);
		addtitle(&x, shownlabel, nil, 0);
	}else if((w = uxwin(win)) != nil && !uxisdesktop(w)){
		shownlabel = estrdup(w->label != nil ? w->label : "");
		addtitle(&x, shownlabel, nil, 1);
	}
	if(a == nil)
		return;
	shown = uxbarmenus(a, win);
	for(m=shown; m; m=m->next)
		addtitle(&x, m->title, m, 0);
}

static void
drawtitle(int k, int hi)
{
	Title *t;
	Image *fg, *bg;
	Point p;

	t = &titles[k];
	bg = uxcolor(hi ? UXcolselect : UXpaper);
	fg = uxcolor(hi ? UXcolselecttext : UXink);
	draw(bar, t->r, bg, nil, ZP);
	if(k == 0){
		/* the mark */
		p = Pt(barr.min.x+UXmenubarmarkx, barr.min.y+(Dy(barr)-1-16)/2);
		if(uxmark != nil)
			draw(bar, Rpt(p, addpt(p, Pt(16, 16))), uxmark, nil, uxmark->r.min);
		else
			string(bar, p, fg, ZP, F, "9");
		return;
	}
	p = Pt(t->r.min.x+Tpad, t->r.min.y+(Dy(t->r)-F->height)/2);
	string(bar, p, fg, ZP, F, t->text);
}

static int	openedtitle = -1;

void
uxbardraw(void)
{
	int k;
	Uxapp *a;

	if(bar == nil)
		return;
	/* an app in a batch changes nothing on screen until commit */
	uxforeground(&a);
	if(a != nil && a->batch)
		return;
	layout();
	draw(bar, barr, uxcolor(UXpaper), nil, ZP);
	draw(bar, Rect(barr.min.x, barr.max.y-1, barr.max.x, barr.max.y), uxcolor(UXink), nil, ZP);
	drawclock();
	for(k=0; k<ntitles; k++)
		drawtitle(k, k == openedtitle);
	flushimage(display, 1);
}

/*
 * Drop-down menus
 */
/* a named key, written out; nil for a printable one */
static char*
keyname(Rune r)
{
	static char k[8];

	switch(r){
	case Kup:	return "Up";
	case Kdown:	return "Down";
	case Kleft:	return "Left";
	case Kright:	return "Right";
	case Khome:	return "Home";
	case Kend:	return "End";
	case Kpgup:	return "PgUp";
	case Kpgdown:	return "PgDn";
	case Kins:	return "Ins";
	case Kdel:	return "Del";
	case '\b':	return "Bs";
	case '\t':	return "Tab";
	case Kesc:	return "Esc";
	case '\n':	return "Enter";
	case ' ':	return "Space";
	}
	if(r > KF && r <= (KF|12)){
		snprint(k, sizeof k, "F%d", r-KF);
		return k;
	}
	if(r >= 'a' && r <= 'z')
		r += 'A'-'a';
	snprint(k, sizeof k, "%C", r);
	return k;
}

/*
 * A shortcut, right-aligned at x (or measured, with dst nil): the
 * modifier glyphs ctl, alt, shift, then the Command glyph, then the key.
 */
static int
drawkey(Image *dst, int x, int y, Uxitem *i, Image *col)
{
	static char *mods[] = { "ctl", "alt", "shift", "mod" };
	int bits[] = { Mctl, Malt, Mshift, -1 };
	int k, w, h, gap;
	char *kn;

	if(i->key == nil)
		return 0;
	gap = 2;
	kn = keyname(i->krune);
	w = stringwidth(F, kn);
	for(k=0; k<nelem(mods); k++)
		if(bits[k] < 0 || (i->kmods & bits[k]))
			w += uxglyphwidth(mods[k])+gap;
	if(dst == nil)
		return w;
	x -= w;
	for(k=0; k<nelem(mods); k++){
		if(bits[k] >= 0 && !(i->kmods & bits[k]))
			continue;
		h = uxglyphheight(mods[k]);
		uxglyph(dst, Pt(x, y+(itemh()-h)/2), mods[k], col);
		x += uxglyphwidth(mods[k])+gap;
	}
	string(dst, Pt(x, y+(itemh()-F->height)/2), col, ZP, F, kn);
	return w;
}

static void
drawitem(Drop *d, int k, int hi)
{
	Uxitem *i;
	Rectangle r;
	Image *fg, *bg, *dot;
	int x, y;

	i = d->it[k];
	r = d->ir[k];
	draw(d->i, r, uxcolor(UXpaper), nil, ZP);
	if(i->sep){
		/* a dotted line */
		y = (r.min.y+r.max.y)/2;
		dot = uxcolor(UXcoldisabled);
		for(x=r.min.x+1; x<r.max.x-1; x+=2)
			draw(d->i, Rect(x, y, x+1, y+1), dot, nil, ZP);
		return;
	}
	hi = hi && i->enabled;
	bg = uxcolor(hi ? UXcolselect : UXpaper);
	fg = uxcolor(hi ? UXcolselecttext : (i->enabled ? UXink : UXcoldisabled));
	draw(d->i, r, bg, nil, ZP);
	string(d->i, Pt(r.min.x+UXmenupadleft, r.min.y+(Dy(r)-F->height)/2), fg, ZP, F,
		i->label != nil ? i->label : i->action);
	if(i->check == Chkon)
		uxglyph(d->i, Pt(r.min.x+(UXmenupadleft-uxglyphwidth("check"))/2,
			r.min.y+(Dy(r)-uxglyphheight("check"))/2), "check", fg);
	else if(i->check == Chkmixed)
		uxglyph(d->i, Pt(r.min.x+(UXmenupadleft-uxglyphwidth("mixed"))/2,
			r.min.y+(Dy(r)-uxglyphheight("mixed"))/2), "mixed", fg);
	if(i->sub != nil)
		uxglyph(d->i, Pt(r.max.x-UXmenupadright/2-uxglyphwidth("submenu"),
			r.min.y+(Dy(r)-uxglyphheight("submenu"))/2), "submenu", fg);
	else
		drawkey(d->i, r.max.x-UXmenupadright, r.min.y, i, fg);
}

static void
closedrops(int level)
{
	while(ndrops > level){
		ndrops--;
		freeimage(drops[ndrops].i);
		free(drops[ndrops].ir);
		free(drops[ndrops].it);
		memset(&drops[ndrops], 0, sizeof(Drop));
	}
	flushimage(display, 1);
}

static Uxmenu*
findsub(char *id)
{
	Uxmenu *m;

	if(id == nil || shownappid == nil)
		return nil;
	shownapp = uxfindapp(shownappid);
	if(shownapp == nil)
		return nil;
	m = uxmenufor(shownapp, id, shownwin);
	return m != nil ? uxdupmenu(m) : nil;
}

/* open menu m below or beside r */
static void
opendrop(Uxmenu *m, Rectangle at, int beside)
{
	Drop *d;
	Uxitem *i;
	int n, k, wid, kw, h, y;
	Rectangle r, fr;
	Image *under;

	if(ndrops >= Maxlevel){
		uxfreemenus(m);
		return;
	}
	n = 0;
	wid = 0;
	kw = 0;
	h = 0;
	for(i=m->items; i; i=i->next){
		n++;
		if(i->sep){
			h += UXmenusepheight;
			continue;
		}
		h += itemh();
		k = stringwidth(F, i->label != nil ? i->label : i->action);
		if(k > wid)
			wid = k;
		if(i->sub != nil)
			k = uxglyphwidth("submenu");
		else
			k = drawkey(nil, 0, 0, i, nil);
		if(k > kw)
			kw = k;
	}
	if(n == 0){
		uxfreemenus(m);
		return;
	}
	/* frame, padding, and the shadow right and below */
	wid += UXmenupadleft+UXmenupadright+(kw > 0 ? UXmenukeygap+kw : 0)+2;
	if(beside)
		r = Rect(at.max.x, at.min.y-1, at.max.x+wid, at.min.y-1+h+2);
	else
		r = Rect(at.min.x, at.max.y, at.min.x+wid, at.max.y+h+2);
	r.max = addpt(r.max, Pt(UXmenushadow, UXmenushadow));
	/* keep it on screen */
	if(r.max.x > screen->r.max.x)
		r = rectaddpt(r, Pt((beside ? at.min.x-Dx(r)-r.min.x : screen->r.max.x-r.max.x), 0));
	if(r.max.y > screen->r.max.y)
		r = rectaddpt(r, Pt(0, screen->r.max.y-r.max.y));
	if(r.min.x < screen->r.min.x)
		r = rectaddpt(r, Pt(screen->r.min.x-r.min.x, 0));
	d = &drops[ndrops];
	/* what's under the shadow's corners, copied before the menu covers it */
	under = allocimage(display, r, screen->chan, 0, DNofill);
	if(under != nil)
		draw(under, r, screen, nil, r.min);
	d->i = allocwindow(wscreen, r, Refbackup, DNofill);
	if(d->i == nil){
		freeimage(under);
		uxfreemenus(m);
		return;
	}
	ndrops++;
	d->r = r;
	d->m = m;
	d->n = n;
	d->sel = -1;
	d->ir = emalloc(n*sizeof(Rectangle));
	d->it = emalloc(n*sizeof(Uxitem*));
	/* the menu: a paper box in an ink frame, its shadow, and what's behind
	 * the shadow's corners (the screen there) */
	fr = r;
	fr.max = subpt(fr.max, Pt(UXmenushadow, UXmenushadow));
	if(under != nil){
		draw(d->i, r, under, nil, r.min);
		freeimage(under);
	}
	draw(d->i, rectaddpt(fr, Pt(UXmenushadow, UXmenushadow)), uxcolor(UXink), nil, ZP);
	draw(d->i, fr, uxcolor(UXpaper), nil, ZP);
	border(d->i, fr, 1, uxcolor(UXink), ZP);
	r = fr;
	y = r.min.y+1;
	k = 0;
	for(i=m->items; i; i=i->next){
		h = i->sep ? UXmenusepheight : itemh();
		d->ir[k] = Rect(r.min.x+1, y, r.max.x-1, y+h);
		d->it[k] = i;
		drawitem(d, k, 0);
		y += h;
		k++;
	}
	flushimage(display, 1);
}

static void
freedropmenus(int level)
{
	int k;

	for(k=level; k<ndrops; k++){
		uxfreemenus(drops[k].m);
		drops[k].m = nil;
	}
}

static void
closeall(void)
{
	freedropmenus(0);
	closedrops(0);
	if(openedtitle >= 0){
		openedtitle = -1;
		uxbardraw();
	}
}

static int
titleat(Point p)
{
	int k;

	if(!ptinrect(p, barr))
		return -1;
	for(k=0; k<ntitles; k++)
		if((titles[k].m != nil || titles[k].rio) && ptinrect(p, titles[k].r))
			return k;
	return -1;
}

static void
opentitle(int k)
{
	Uxmenu *m;

	freedropmenus(0);
	closedrops(0);
	if(openedtitle >= 0 && openedtitle < ntitles)
		drawtitle(openedtitle, 0);
	openedtitle = k;
	if(k < 0)
		return;
	drawtitle(k, 1);
	if(titles[k].rio){
		uxfreemenus(riomenu);
		riomenu = mkriomenu();
		m = uxdupmenu(riomenu);
	}else
		m = uxdupmenu(titles[k].m);
	opendrop(m, titles[k].r, 0);
}

/* which drop-down and item the point is in */
static int
dropat(Point p, int *item)
{
	int l, k;

	for(l=ndrops-1; l>=0; l--){
		if(!ptinrect(p, drops[l].r))
			continue;
		*item = -1;
		for(k=0; k<drops[l].n; k++)
			if(ptinrect(p, drops[l].ir[k])){
				*item = k;
				break;
			}
		return l;
	}
	return -1;
}

static void
highlight(int l, int k)
{
	Drop *d;
	Uxitem *i;
	Uxmenu *sub;

	d = &drops[l];
	if(d->sel == k)
		return;
	freedropmenus(l+1);
	closedrops(l+1);
	if(d->sel >= 0)
		drawitem(d, d->sel, 0);
	d->sel = k;
	if(k >= 0){
		drawitem(d, k, 1);
		i = d->it[k];
		if(i->sub != nil && i->enabled && (sub = findsub(i->sub)) != nil)
			opendrop(sub, d->ir[k], 1);
	}
	flushimage(display, 1);
}

/* run the chosen item: an action event, or one of rio's commands */
static void
choose(Uxitem *i, int fromrio)
{
	extern void resize(void), move(void), delete(void), hide(void);
	extern void confirmexit(void);
	extern Image *sweep(void);
	Window *w;
	int id;

	if(!fromrio){
		if(shownappid != nil && i->enabled && i->sub == nil)
			uxpost(Smenu, "action app=%s win=%d id=%s via=click", shownappid, shownwin, i->action);
		return;
	}
	if(strncmp(i->action, "rio.w", 5) == 0){
		id = atoi(i->action+5);
		w = uxwin(id);
		if(w == nil)
			return;
		if(Dx(w->screenr) <= 0){
			wunhide(w);
			return;
		}
		incref(w);
		wcurrent(w);
		wtopme(w);
		wsendctlmesg(w, Topped, ZR, nil);
		wclose(w);
		return;
	}
	if(strcmp(i->action, "rio.exit") == 0){
		confirmexit();
		return;
	}
	switch(atoi(i->action+4)){
	case 0:
		new(sweep(), FALSE, scrolling, 0, nil, "/bin/rc", nil);
		break;
	case 1:
		resize();
		break;
	case 2:
		move();
		break;
	case 3:
		delete();
		break;
	case 4:
		hide();
		break;
	}
}

/*
 * Button 1 went down in the bar.  Track until an item is chosen or the
 * menus are dismissed.  Pressing and releasing on a title without
 * moving into its menu leaves it open (click to open, click to choose).
 */
void
uxbarmouse(void)
{
	int k, l, item, sticky, fromrio;
	Uxitem *chosen;
	Point p;

	if(!(mouse->buttons & 1)){
		/* only button 1 opens menus; swallow others */
		while(mouse->buttons)
			readmouse(mousectl);
		return;
	}
	sweeping = TRUE;
	riosetcursor(nil);
	layout();
	k = titleat(mouse->xy);
	if(k < 0){
		while(mouse->buttons)
			readmouse(mousectl);
		sweeping = FALSE;
		return;
	}
	opentitle(k);
	sticky = 1;
	chosen = nil;
	fromrio = titles[k].rio;
	for(;;){
		readmouse(mousectl);
		p = mouse->xy;
		if((k = titleat(p)) >= 0 && k != openedtitle){
			opentitle(k);
			fromrio = titles[k].rio;
		}
		if((l = dropat(p, &item)) >= 0){
			sticky = 0;
			highlight(l, item);
		}else if(ndrops > 0 && drops[ndrops-1].sel >= 0 && drops[ndrops-1].it[drops[ndrops-1].sel]->sub == nil)
			highlight(ndrops-1, -1);
		if(mouse->buttons == 0){
			if(sticky && k >= 0){
				/* click on a title: keep it open until the next press */
				while(mouse->buttons == 0){
					readmouse(mousectl);
					p = mouse->xy;
					if((k = titleat(p)) >= 0 && k != openedtitle){
						opentitle(k);
						fromrio = titles[k].rio;
					}
					if((l = dropat(p, &item)) >= 0)
						highlight(l, item);
					else if(ndrops > 0 && drops[ndrops-1].sel >= 0 && drops[ndrops-1].it[drops[ndrops-1].sel]->sub == nil)
						highlight(ndrops-1, -1);
				}
				sticky = 0;
				if(titleat(mouse->xy) >= 0 || dropat(mouse->xy, &item) >= 0)
					continue;
				/* pressed outside: dismiss, and let the press go nowhere */
				while(mouse->buttons)
					readmouse(mousectl);
				break;
			}
			if((l = dropat(p, &item)) >= 0 && item >= 0){
				chosen = uxdupitem(drops[l].it[item]);
				if(!chosen->enabled || chosen->sep || chosen->sub != nil){
					uxfreeitem(chosen);
					chosen = nil;
				}
			}
			break;
		}
	}
	closeall();
	sweeping = FALSE;
	if(chosen != nil){
		choose(chosen, fromrio);
		uxfreeitem(chosen);
	}
}

/*
 * Keyboard: Command shortcuts.  Called by keyboardtap for every
 * message from the keyboard, before it goes to the focused window.
 * Returns 1 if the message is swallowed.
 */
static	Rune	held[32];
static	int		nheld;
static	Rune	swallowed[8];
static	int		nswallowed;
static	char		lastk[128];

static int
inset(Rune r, Rune *s, int n)
{
	int i;

	for(i=0; i<n; i++)
		if(s[i] == r)
			return 1;
	return 0;
}

static int
ismod(Rune r)
{
	return r == Kshift || r == Kalt || r == Kctl || r == Kmod4 || r == Kaltgr;
}

static Rune
normkey(Rune r)
{
	if(r >= 'A' && r <= 'Z')
		return r+'a'-'A';
	if(r >= 1 && r <= 26 && r != '\b' && r != '\t' && r != '\n')
		return r+'a'-1;
	return r;
}

int
uxkbd(char *s)
{
	Rune set[32], r, out[32];
	int n, nout, i, mods, drop, released;
	char *p, *q;

	if(*s == 'c')
		return nswallowed > 0;
	if(*s != 'k' && *s != 'K')
		return 0;
	n = 0;
	for(p=s+1; *p && n<nelem(set); ){
		p += chartorune(&r, p);
		set[n++] = r;
	}
	drop = 0;
	/* forget swallowed keys that were released */
	released = 0;
	for(i=0; i<nswallowed; ){
		if(!inset(swallowed[i], set, n)){
			swallowed[i] = swallowed[--nswallowed];
			released = 1;
		}else
			i++;
	}
	if(*s == 'k' && uxmode && uxcmdkey != 0 && inset(uxcmdkey, set, n)){
		mods = 0;
		if(uxcmdkey != Kshift && inset(Kshift, set, n))
			mods |= Mshift;
		if(uxcmdkey != Kalt && inset(Kalt, set, n))
			mods |= Malt;
		if(uxcmdkey != Kctl && inset(Kctl, set, n))
			mods |= Mctl;
		for(i=0; i<n; i++){
			r = set[i];
			if(ismod(r) || inset(r, held, nheld) || inset(r, swallowed, nswallowed))
				continue;
			if(uxmatchkey(normkey(r), mods) && nswallowed < nelem(swallowed))
				swallowed[nswallowed++] = r;
		}
	}
	memmove(held, set, n*sizeof(Rune));
	nheld = n;
	if(nswallowed == 0 && !released){
		snprint(lastk, sizeof lastk, "%s", s);
		return 0;
	}
	/* hide swallowed keys from the window */
	nout = 0;
	for(i=0; i<n; i++)
		if(!inset(set[i], swallowed, nswallowed))
			out[nout++] = set[i];
	q = s+1;
	for(i=0; i<nout; i++)
		q += runetochar(q, &out[i]);
	*q = 0;
	if(strcmp(s+1, lastk+1) == 0 && (*s == lastk[0] || released))
		drop = 1;
	if(!drop)
		snprint(lastk, sizeof lastk, "%s", s);
	return drop;
}
