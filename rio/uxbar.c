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
static	Image	*gray;
static	Rectangle	barr;

enum
{
	Hpad		= 8,	/* space around a title */
	Vpad		= 2,
	Itemh	= 0,	/* set from the font */
	Sepht	= 7,
	Checkw	= 16,
	Keygap	= 24,
	Maxlevel	= 8,
};

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
	return font->height+2*Vpad;
}

void
uxbarinit(void)
{
	uxbarh = font->height+2*Vpad+1;
	if(gray == nil)
		gray = allocimage(display, Rect(0, 0, 1, 1), screen->chan, 1, 0x888888FF);
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
layout(void)
{
	Uxapp *a;
	Uxmenu *m;
	Window *w;
	int x, win;
	char *t;

	uxfreemenus(shown);
	shown = nil;
	free(shownappid);
	shownappid = nil;
	ntitles = 0;
	win = uxforeground(&a);
	shownapp = a;
	shownwin = win;
	x = barr.min.x+Hpad;

	titles[ntitles].text = "rio";
	titles[ntitles].m = nil;
	titles[ntitles].rio = 1;
	titles[ntitles].r = Rect(x-Hpad/2, barr.min.y, x+stringwidth(font, "rio")+Hpad/2, barr.max.y-1);
	x += stringwidth(font, "rio")+2*Hpad;
	ntitles++;

	t = nil;
	if(a != nil){
		t = a->title;
		shownappid = estrdup(a->id);
	}else if((w = uxwin(win)) != nil)
		t = w->label;
	if(t != nil){
		titles[ntitles].text = t;
		titles[ntitles].m = nil;
		titles[ntitles].rio = 0;
		titles[ntitles].r = Rect(x-Hpad/2, barr.min.y, x+stringwidth(font, t)+Hpad/2, barr.max.y-1);
		x += stringwidth(font, t)+2*Hpad;
		ntitles++;
	}
	if(a == nil)
		return;
	shown = uxbarmenus(a, win);
	for(m=shown; m && ntitles<nelem(titles); m=m->next){
		titles[ntitles].text = m->title;
		titles[ntitles].m = m;
		titles[ntitles].rio = 0;
		titles[ntitles].r = Rect(x-Hpad/2, barr.min.y, x+stringwidth(font, m->title)+Hpad/2, barr.max.y-1);
		x += stringwidth(font, m->title)+2*Hpad;
		ntitles++;
	}
}

static void
drawtitle(int k, int hi)
{
	Title *t;
	Image *fg, *bg;

	t = &titles[k];
	bg = hi ? display->black : display->white;
	fg = hi ? display->white : display->black;
	draw(bar, t->r, bg, nil, ZP);
	string(bar, Pt(t->r.min.x+Hpad/2, t->r.min.y+Vpad), fg, ZP, font, t->text);
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
	draw(bar, barr, display->white, nil, ZP);
	draw(bar, Rect(barr.min.x, barr.max.y-1, barr.max.x, barr.max.y), display->black, nil, ZP);
	for(k=0; k<ntitles; k++)
		drawtitle(k, k == openedtitle);
	/* the app title in bold, by overstrike */
	if(ntitles > 1 && titles[1].m == nil && !titles[1].rio)
		string(bar, Pt(titles[1].r.min.x+Hpad/2+1, titles[1].r.min.y+Vpad), display->black, ZP, font, titles[1].text);
	flushimage(display, 1);
}

/*
 * Drop-down menus
 */
static char*
keytext(Uxitem *i)
{
	static char buf[64];
	char *p, *e, k[16];
	Rune r;

	if(i->key == nil)
		return nil;
	p = buf;
	e = buf+sizeof buf;
	if(i->kmods & Mctl)
		p = seprint(p, e, "Ctl-");
	if(i->kmods & Malt)
		p = seprint(p, e, "Alt-");
	if(i->kmods & Mshift)
		p = seprint(p, e, "Shift-");
	r = i->krune;
	if(r >= 'a' && r <= 'z')
		r += 'A'-'a';
	switch(i->krune){
	case Kup:	strcpy(k, "Up"); break;
	case Kdown:	strcpy(k, "Down"); break;
	case Kleft:	strcpy(k, "Left"); break;
	case Kright:	strcpy(k, "Right"); break;
	case Khome:	strcpy(k, "Home"); break;
	case Kend:	strcpy(k, "End"); break;
	case Kpgup:	strcpy(k, "PgUp"); break;
	case Kpgdown:	strcpy(k, "PgDn"); break;
	case Kins:	strcpy(k, "Ins"); break;
	case Kdel:	strcpy(k, "Del"); break;
	case '\b':	strcpy(k, "Bs"); break;
	case '\t':	strcpy(k, "Tab"); break;
	case Kesc:	strcpy(k, "Esc"); break;
	case '\n':	strcpy(k, "Enter"); break;
	case ' ':	strcpy(k, "Space"); break;
	default:
		if(i->krune > KF && i->krune <= (KF|12))
			snprint(k, sizeof k, "F%d", i->krune-KF);
		else
			snprint(k, sizeof k, "%C", r);
	}
	seprint(p, e, "Cmd-%s", k);
	return buf;
}

static void
drawitem(Drop *d, int k, int hi)
{
	Uxitem *i;
	Rectangle r;
	Image *fg, *bg;
	Point p;
	char *kt;
	int y;

	i = d->it[k];
	r = d->ir[k];
	if(i->sep){
		draw(d->i, r, display->white, nil, ZP);
		y = (r.min.y+r.max.y)/2;
		draw(d->i, Rect(r.min.x, y, r.max.x, y+1), gray, nil, ZP);
		return;
	}
	hi = hi && i->enabled;
	bg = hi ? display->black : display->white;
	fg = hi ? display->white : (i->enabled ? display->black : gray);
	draw(d->i, r, bg, nil, ZP);
	p = Pt(r.min.x+Checkw, r.min.y+Vpad);
	string(d->i, p, fg, ZP, font, i->label != nil ? i->label : i->action);
	if(i->check == Chkon){
		y = r.min.y+Dy(r)/2;
		line(d->i, Pt(r.min.x+4, y), Pt(r.min.x+7, y+3), Endsquare, Endsquare, 1, fg, ZP);
		line(d->i, Pt(r.min.x+7, y+3), Pt(r.min.x+12, y-4), Endsquare, Endsquare, 1, fg, ZP);
	}else if(i->check == Chkmixed){
		y = r.min.y+Dy(r)/2;
		draw(d->i, Rect(r.min.x+4, y-1, r.min.x+12, y+1), fg, nil, ZP);
	}
	if(i->sub != nil){
		/* a small triangle */
		y = r.min.y+Dy(r)/2;
		for(k=0; k<4; k++)
			draw(d->i, Rect(r.max.x-10+k, y-3+k, r.max.x-9+k, y+4-k), fg, nil, ZP);
	}else if((kt = keytext(i)) != nil)
		string(d->i, Pt(r.max.x-Hpad-stringwidth(font, kt), r.min.y+Vpad), fg, ZP, font, kt);
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
	char *kt;
	Rectangle r;

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
			h += Sepht;
			continue;
		}
		h += itemh();
		k = stringwidth(font, i->label != nil ? i->label : i->action);
		if(k > wid)
			wid = k;
		if(i->sub != nil)
			k = 12;
		else if((kt = keytext(i)) != nil)
			k = stringwidth(font, kt);
		else
			k = 0;
		if(k > kw)
			kw = k;
	}
	if(n == 0){
		uxfreemenus(m);
		return;
	}
	wid += Checkw+Hpad+(kw > 0 ? Keygap+kw : 0)+2;
	if(beside)
		r = Rect(at.max.x, at.min.y-1, at.max.x+wid, at.min.y-1+h+2);
	else
		r = Rect(at.min.x, at.max.y, at.min.x+wid, at.max.y+h+2);
	/* keep it on screen */
	if(r.max.x > screen->r.max.x)
		r = rectaddpt(r, Pt((beside ? at.min.x-wid-r.min.x : screen->r.max.x-r.max.x), 0));
	if(r.max.y > screen->r.max.y)
		r = rectaddpt(r, Pt(0, screen->r.max.y-r.max.y));
	if(r.min.x < screen->r.min.x)
		r = rectaddpt(r, Pt(screen->r.min.x-r.min.x, 0));
	d = &drops[ndrops];
	d->i = allocwindow(wscreen, r, Refbackup, DNofill);
	if(d->i == nil){
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
	draw(d->i, r, display->white, nil, ZP);
	border(d->i, r, 1, display->black, ZP);
	y = r.min.y+1;
	k = 0;
	for(i=m->items; i; i=i->next){
		h = i->sep ? Sepht : itemh();
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
