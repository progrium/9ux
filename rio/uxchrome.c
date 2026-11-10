/*
 * 9/UX classic window chrome (devux `chrome`): a title bar above the
 * window, a 1 px ink frame and a shadow, drawn by rio in a frame image
 * kept just below the window's own. The window's image and rectangle
 * stay what libdraw clients expect; rio paints its 4 px border to match
 * (the top rows as the bottom of the title bar, the rest as paper).
 * See assets/README.md (Windows) for the look.
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

typedef struct Chrome Chrome;
struct Chrome
{
	int		win;
	Image	*frame;
	Rectangle	fr;		/* the frame image's rectangle */
	int		zoomed;
	Rectangle	unzoomed;	/* the window's rectangle before zoom */
	Chrome	*next;
};

static	Chrome	*chromes;

enum
{
	T	= UXwindowtitleheight,	/* title bar, with its bottom rule */
	Box	= UXwindowboxsize,
	Boxin	= UXwindowboxinset,
};

static Chrome*
findchrome(int id)
{
	Chrome *c;

	for(c=chromes; c; c=c->next)
		if(c->win == id)
			return c;
	return nil;
}

static int
active(Window *w)
{
	return uxmode && w != nil && !w->deleted && w->i != nil && Dx(w->screenr) > 0
		&& findchrome(w->id) != nil && !uxisdesktop(w);
}

int
uxchromed(Window *w)
{
	return active(w);
}

/* the outer box: frame ink around the title bar and the window's image */
static Rectangle
outer(Window *w)
{
	Rectangle r;

	r = w->screenr;
	r.min.x--;
	r.max.x++;
	r.max.y++;
	r.min.y = r.min.y+Borderwidth-T-1;
	return r;
}

/* the title bar, inside the frame, with its bottom rule */
static Rectangle
titlerect(Window *w)
{
	Rectangle o;

	o = insetrect(outer(w), 1);
	o.max.y = o.min.y+T;
	return o;
}

static Rectangle
closebox(Window *w)
{
	Rectangle t;
	Point p;

	t = titlerect(w);
	p = Pt(t.min.x+Boxin, t.min.y+(T-1-Box)/2);
	return Rpt(p, addpt(p, Pt(Box, Box)));
}

static Rectangle
zoombox(Window *w)
{
	Rectangle t;
	Point p;

	t = titlerect(w);
	p = Pt(t.max.x-Boxin-Box, t.min.y+(T-1-Box)/2);
	return Rpt(p, addpt(p, Pt(Box, Box)));
}

/* draw the title bar into dst (clipped to dst) */
static void
drawtitle(Image *dst, Window *w, int pressed)
{
	Rectangle t, b, tr;
	Image *ink;
	char *lab;
	int on, wid, x, y, x0, x1;
	Point p;

	ink = uxcolor(UXink);
	t = titlerect(w);
	on = w == input;
	draw(dst, t, uxcolor(on ? UXcoltitlebar : UXcoltitleoff), nil, ZP);
	draw(dst, Rect(t.min.x, t.max.y-1, t.max.x, t.max.y), ink, nil, ZP);
	if(on)
		draw(dst, Rect(t.min.x, t.min.y, t.max.x, t.min.y+1), uxcolor(UXcoltitlelight), nil, ZP);
	lab = uxlabel(w);
	wid = stringwidth(uxsysfont, lab);
	if(wid > Dx(t)-2*(Boxin+Box+UXwindowtitlepad))
		wid = Dx(t)-2*(Boxin+Box+UXwindowtitlepad);
	if(wid < 0)
		wid = 0;
	p = Pt(t.min.x+(Dx(t)-wid)/2, t.min.y+(T-1-uxsysfont->height)/2);
	tr = Rect(p.x, t.min.y, p.x+wid, t.max.y-1);
	if(on){
		/* olive rules every 3 px between the boxes, clear around the title */
		x0 = closebox(w).max.x+UXwindowtitlepad/2;
		x1 = zoombox(w).min.x-UXwindowtitlepad/2;
		for(y=t.min.y+4; y<t.max.y-4; y+=3){
			if(tr.min.x-UXwindowtitlepad > x0)
				draw(dst, Rect(x0, y, tr.min.x-UXwindowtitlepad, y+1), uxcolor(UXcoltitlerule), nil, ZP);
			if(tr.max.x+UXwindowtitlepad < x1)
				draw(dst, Rect(tr.max.x+UXwindowtitlepad, y, x1, y+1), uxcolor(UXcoltitlerule), nil, ZP);
		}
		/* the boxes: paper squares in ink, the zoom box with a small square */
		b = closebox(w);
		draw(dst, b, uxcolor(UXpaper), nil, ZP);
		border(dst, b, 1, ink, ZP);
		if(pressed == 1)
			uxglyph(dst, Pt(b.min.x+(Box-uxglyphwidth("close"))/2, b.min.y+(Box-uxglyphheight("close"))/2), "close", ink);
		b = zoombox(w);
		draw(dst, b, uxcolor(UXpaper), nil, ZP);
		border(dst, b, 1, ink, ZP);
		x = b.min.x;
		y = b.min.y;
		border(dst, Rect(x, y, x+6, y+6), 1, ink, ZP);
		if(pressed == 2)
			draw(dst, insetrect(b, 2), ink, nil, ZP);
	}
	if(wid > 0){
		string(dst, p, uxcolor(on ? UXink : UXcoltitletextoff), ZP, uxsysfont, lab);
		/* too long: cover what runs past the room there is */
		if(stringwidth(uxsysfont, lab) > wid)
			draw(dst, Rect(tr.max.x, t.min.y+1, t.max.x-(Boxin+Box), t.max.y-1),
				uxcolor(on ? UXcoltitlebar : UXcoltitleoff), nil, ZP);
	}
	free(lab);
}

/* the frame image: ink box, title bar, shadow */
static void
drawframe(Chrome *c, Window *w, int pressed)
{
	Rectangle o, f;
	Image *ink;

	if(c->frame == nil)
		return;
	ink = uxcolor(UXink);
	f = c->fr;
	o = outer(w);
	draw(c->frame, Rect(o.min.x+1, o.max.y, f.max.x, f.max.y), ink, nil, ZP);
	draw(c->frame, Rect(o.max.x, o.min.y+1, f.max.x, f.max.y), ink, nil, ZP);
	border(c->frame, o, 1, ink, ZP);
	drawtitle(c->frame, w, pressed);
}

/*
 * rio drew (or would draw) the window's border: paint chrome instead.
 * Called from wborder.
 */
int
uxchromeborder(Window *w)
{
	Chrome *c;
	Rectangle r, in;
	Image *paper;

	if(!active(w))
		return 0;
	c = findchrome(w->id);
	r = w->i->r;
	in = insetrect(r, Borderwidth);
	paper = uxcolor(UXpaper);
	/* left, right and bottom: ink at the very edge is the frame's */
	draw(w->i, Rect(r.min.x, in.min.y, in.min.x, r.max.y), paper, nil, ZP);
	draw(w->i, Rect(in.max.x, in.min.y, r.max.x, r.max.y), paper, nil, ZP);
	draw(w->i, Rect(in.min.x, in.max.y, in.max.x, r.max.y), paper, nil, ZP);
	/* the top rows are the bottom of the title bar */
	drawtitle(w->i, w, 0);
	drawframe(c, w, 0);
	return 1;
}

static void
freeframe(Chrome *c)
{
	if(c->frame != nil){
		freeimage(c->frame);
		c->frame = nil;
	}
}

/*
 * Make the frame image fit the window, just below it. Called when the
 * window's image changes (wresize) and when it's raised (wtopme, before
 * topwindow of the window's own image).
 */
void
uxchromefit(Window *w)
{
	Chrome *c;
	Rectangle f;

	if(w == nil || (c = findchrome(w->id)) == nil)
		return;
	if(!active(w)){
		freeframe(c);
		return;
	}
	f = outer(w);
	f.max = addpt(f.max, Pt(UXwindowshadow, UXwindowshadow));
	if(c->frame == nil || !eqrect(f, c->fr)){
		freeframe(c);
		c->frame = allocwindow(wscreen, f, Refbackup, DNofill);
		c->fr = f;
		if(c->frame == nil)
			return;
		/* what's behind the shadow's corners */
		draw(c->frame, f, uxcolor(UXcoldesktop), nil, ZP);
	}
	topwindow(c->frame);
	topwindow(w->i);
	uxchromeborder(w);
}

/* the window was bottomed: its frame goes below it */
void
uxchromebottom(Window *w)
{
	Chrome *c;

	if(w == nil || (c = findchrome(w->id)) == nil || c->frame == nil)
		return;
	bottomwindow(c->frame);
}

/* the window went away */
void
uxchromedel(int id)
{
	Chrome *c, **l;

	for(l=&chromes; (c = *l) != nil; l=&c->next)
		if(c->win == id){
			*l = c->next;
			freeframe(c);
			free(c);
			return;
		}
}

static void
repaint(Window *w)
{
	if(w->i == nil)
		return;
	if(!uxchromeborder(w)){
		/* back to rio's own border */
		border(w->i, w->i->r, Selborder, w == input ? titlecol : lighttitlecol, ZP);
	}
	flushimage(display, 1);
}

/* mode or chrome changed: refit every chromed window */
void
uxchromeall(void)
{
	Chrome *c;
	Window *w;

	for(c=chromes; c; c=c->next)
		if((w = uxwin(c->win)) != nil){
			uxchromefit(w);
			repaint(w);
		}
}

/* chrome [winid] on|off */
char*
uxchromectl(Window *own, char **f, int nf)
{
	Window *w;
	Chrome *c;
	int on;

	if(nf == 2)
		w = own;
	else if(nf == 3)
		w = uxwin(atoi(f[1]));
	else
		return "bad syntax";
	if(nf == 2 && own == nil)
		return "bad syntax: no window";
	if(w == nil || w->deleted || w->i == nil)
		return "not found";
	if(strcmp(f[nf-1], "on") == 0)
		on = 1;
	else if(strcmp(f[nf-1], "off") == 0)
		on = 0;
	else
		return "bad value";
	if(on && uxisdesktop(w))
		return "bad value: desktop window";
	c = findchrome(w->id);
	if(on && c == nil){
		c = emalloc(sizeof(Chrome));
		c->win = w->id;
		c->next = chromes;
		chromes = c;
		uxchromefit(w);
		repaint(w);
		uxpost(Sux, "chrome win=%d chrome=on", w->id);
	}else if(!on && c != nil){
		uxchromedel(w->id);
		repaint(w);
		uxpost(Sux, "chrome win=%d chrome=off", w->id);
	}
	uxchanged();
	return nil;
}

/*
 * Pointer routing: is p on the chrome (title bar or frame) of a window,
 * with nothing above it there? Returns that window.
 */
Window*
uxchromeat(Point p)
{
	Chrome *c;
	Window *w, *best, *v;
	Rectangle o, t;
	int i;

	best = nil;
	for(c=chromes; c; c=c->next){
		w = uxwin(c->win);
		if(!active(w))
			continue;
		o = outer(w);
		t = titlerect(w);
		t.max.y = w->screenr.min.y+Borderwidth;
		/* the title bar (with the window's own top border rows) and the frame */
		if(!ptinrect(p, o))
			continue;
		if(ptinrect(p, w->screenr) && p.y >= t.max.y)
			continue;
		if(best == nil || w->topped > best->topped)
			best = w;
	}
	if(best == nil)
		return nil;
	/* covered by a window above it? */
	for(i=0; i<nwindow; i++){
		v = window[i];
		if(v == best || v->deleted || v->i == nil || v->topped <= best->topped)
			continue;
		if(ptinrect(p, v->screenr))
			return nil;
		if(active(v) && ptinrect(p, outer(v)))
			return nil;
	}
	return best;
}

static void
zoom(Window *w, Chrome *c)
{
	Rectangle r, d;
	Image *i;

	if(c->zoomed){
		r = c->unzoomed;
		c->zoomed = 0;
	}else{
		c->unzoomed = w->screenr;
		/* the outer box fills the desktop, with room for the shadow */
		d = uxdeskrect();
		d.max = subpt(d.max, Pt(UXwindowshadow, UXwindowshadow));
		r = Rect(d.min.x+1, d.min.y+T+1-Borderwidth, d.max.x-1, d.max.y-1);
		c->zoomed = 1;
	}
	i = allocwindow(wscreen, r, Refbackup, DNofill);
	if(i == nil)
		return;
	incref(w);
	wsendctlmesg(w, Reshaped, r, i);
	wclose(w);
}

/* track a press in a box; returns 1 if released inside it */
static int
trackbox(Window *w, Chrome *c, Rectangle b, int which)
{
	int in, was;

	was = -1;
	while(mouse->buttons & 1){
		in = ptinrect(mouse->xy, b);
		if(in != was){
			drawframe(c, w, in ? which : 0);
			drawtitle(w->i, w, in ? which : 0);
			flushimage(display, 1);
			was = in;
		}
		readmouse(mousectl);
	}
	in = ptinrect(mouse->xy, b);
	drawframe(c, w, 0);
	drawtitle(w->i, w, 0);
	flushimage(display, 1);
	return in;
}

/* drag the window by its title bar, showing an outline */
static void
movewin(Window *w)
{
	Point p0, d;
	Rectangle r, o;
	Image *i;
	extern void drawborder(Rectangle, Image*);

	p0 = mouse->xy;
	o = outer(w);
	r = o;
	while(mouse->buttons & 1){
		d = subpt(mouse->xy, p0);
		r = rectaddpt(o, d);
		/* keep the title bar below the menu bar and on screen */
		if(r.min.y < uxdeskrect().min.y)
			r = rectaddpt(r, Pt(0, uxdeskrect().min.y-r.min.y));
		if(!eqrect(r, o))
			drawborder(r, sizecol);
		readmouse(mousectl);
	}
	drawborder(r, nil);
	d = subpt(r.min, o.min);
	if(eqpt(d, ZP))
		return;
	r = rectaddpt(w->screenr, d);
	i = allocwindow(wscreen, r, Refbackup, DNofill);
	if(i == nil)
		return;
	incref(w);
	wsendctlmesg(w, Reshaped, r, i);
	wclose(w);
}

/* button down on w's chrome: focus, raise, then boxes or a move */
void
uxchromemouse(Window *w)
{
	Chrome *c;
	Point p;

	c = findchrome(w->id);
	if(c == nil)
		return;
	if(!(mouse->buttons & 1)){
		while(mouse->buttons)
			readmouse(mousectl);
		return;
	}
	incref(w);
	if(w != input){
		wcurrent(w);
		wtopme(w);
		wsendctlmesg(w, Topped, ZR, nil);
		/* a click on an inactive title only activates it, unless it drags */
	}else
		wtopme(w);
	p = mouse->xy;
	sweeping = TRUE;
	if(ptinrect(p, closebox(w))){
		if(trackbox(w, c, closebox(w), 1)){
			uxpost(Sux, "close win=%d", w->id);
			/* no app to act on it: delete the window, as rio's Delete does */
			if(uxappof(w->id) == nil)
				wsendctlmesg(w, Deleted, ZR, nil);
		}
	}else if(ptinrect(p, zoombox(w))){
		if(trackbox(w, c, zoombox(w), 2))
			zoom(w, c);
	}else
		movewin(w);
	sweeping = FALSE;
	wclose(w);
}

/* the windows file's state flag */
int
uxhaschrome(int id)
{
	return findchrome(id) != nil;
}
