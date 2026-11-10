/*
 * Drawing controls with libdraw, in the 9/UX look (assets/README.md,
 * Controls and Windows). Colours are by role from assets/gen/uxstyle.h.
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

static ulong palette[] = {
	UXink, UXpaper, UXcream, UXbutter, UXolive, UXmist, UXsea, UXteal,
	UXdeep, UXnavy, UXred, UXgrey1, UXgrey2, UXgrey3, UXgrey4, UXgrey5,
};

/* a solid colour on w's display, cached per window */
Image*
_color(Uxwin *w, ulong rgba)
{
	int i;

	for(i = 0; i < nelem(palette); i++)
		if(palette[i] == rgba)
			break;
	if(i == nelem(palette))
		return allocimage(w->d, Rect(0,0,1,1), w->img->chan, 1, rgba);	/* not cached */
	if(w->col[i] == nil)
		w->col[i] = allocimage(w->d, Rect(0,0,1,1), w->img->chan, 1, rgba);
	return w->col[i];
}

/* the system and body fonts (assets/fonts.ndb), falling back to the window's font */
void
_winfonts(Uxwin *w)
{
	Font *f;

	f = openfont(w->d, "/lib/font/bit/lucidasans/unicode.7.font");
	if(f != nil)
		w->font = f;
	w->sysfont = openfont(w->d, "/lib/font/bit/lucidasans/boldunicode.7.font");
	if(w->sysfont == nil)
		w->sysfont = w->font;
}

static int
active(Uxwin *w)
{
	return ux.front == nil || ux.front == w;
}

/* a glyph from assets/gen/uxglyphs.c as a mask, cached per window */
static Image*
glyphmask(Uxwin *w, char *name, Point *sz)
{
	UXglyph *g;
	Image *m;
	int i;

	for(i = 0; uxglyphs[i].name != nil; i++)
		if(strcmp(uxglyphs[i].name, name) == 0)
			break;
	g = &uxglyphs[i];
	if(g->name == nil || i >= nelem(w->glyph))
		return nil;
	*sz = Pt(g->w, g->h);
	if(w->glyph[i] == nil){
		m = allocimage(w->d, Rect(0, 0, g->w, g->h), GREY1, 0, DBlack);
		if(m == nil)
			return nil;
		loadimage(m, m->r, g->black, ((g->w+7)/8)*g->h);
		w->glyph[i] = m;
	}
	return w->glyph[i];
}

/* draw glyph name with its top left at p */
static void
glyph(Uxwin *w, char *name, Point p, Image *col)
{
	Image *m;
	Point sz;

	m = glyphmask(w, name, &sz);
	if(m != nil)
		draw(w->img, Rpt(p, addpt(p, sz)), col, m, ZP);
}

/* glyph name centred in r */
static void
glyphin(Uxwin *w, char *name, Rectangle r, Image *col)
{
	Point sz;

	if(glyphmask(w, name, &sz) == nil)
		return;
	glyph(w, name, Pt(r.min.x + (Dx(r)-sz.x)/2, r.min.y + (Dy(r)-sz.y)/2), col);
}

/* c's rectangle in screen coordinates, scrolled by its parents' scroll views */
Rectangle
_abs(Uxctl *c)
{
	Rectangle r;
	Uxctl *p;

	r = c->r;
	for(p = c->parent; p != nil; p = p->parent)
		if(strcmp(p->t->name, "scroll-view") == 0)
			r = rectsubpt(r, p->origin);
	return rectaddpt(r, c->w->img->r.min);
}

static void
fill(Uxwin *w, Rectangle r, ulong col)
{
	draw(w->img, r, _color(w, col), nil, ZP);
}

static void
frame(Uxwin *w, Rectangle r, int n, ulong col)
{
	border(w->img, r, n, _color(w, col), ZP);
}

static Point
centred(Font *f, Rectangle r, char *s)
{
	Point sz;

	sz = stringsize(f, s);
	return Pt(r.min.x + (Dx(r)-sz.x)/2, r.min.y + (Dy(r)-sz.y)/2);
}

/* a rounded rectangle: fill (if any) inside a thick outline of radius rad */
static void
roundrect(Uxwin *w, Rectangle r, int rad, int thick, ulong fillc, ulong line)
{
	Image *f, *l;
	int i, d;

	l = _color(w, line);
	f = fillc ? _color(w, fillc) : nil;
	for(i = 0; i < thick; i++){
		/* each pass is a 1 px outline, inset by one */
		d = rad - i < 1 ? 1 : rad - i;
		/* straight edges */
		draw(w->img, Rect(r.min.x+d, r.min.y, r.max.x-d, r.min.y+1), l, nil, ZP);
		draw(w->img, Rect(r.min.x+d, r.max.y-1, r.max.x-d, r.max.y), l, nil, ZP);
		draw(w->img, Rect(r.min.x, r.min.y+d, r.min.x+1, r.max.y-d), l, nil, ZP);
		draw(w->img, Rect(r.max.x-1, r.min.y+d, r.max.x, r.max.y-d), l, nil, ZP);
		/* corners: quarter arcs */
		arc(w->img, Pt(r.min.x+d, r.min.y+d), d, d, 0, l, ZP, 90, 90);
		arc(w->img, Pt(r.max.x-1-d, r.min.y+d), d, d, 0, l, ZP, 0, 90);
		arc(w->img, Pt(r.min.x+d, r.max.y-1-d), d, d, 0, l, ZP, 180, 90);
		arc(w->img, Pt(r.max.x-1-d, r.max.y-1-d), d, d, 0, l, ZP, 270, 90);
		r = insetrect(r, 1);
	}
	if(f != nil){
		d = rad - thick < 0 ? 0 : rad - thick;
		draw(w->img, Rect(r.min.x+d, r.min.y, r.max.x-d, r.max.y), f, nil, ZP);
		draw(w->img, Rect(r.min.x, r.min.y+d, r.max.x, r.max.y-d), f, nil, ZP);
		if(d > 0){
			fillellipse(w->img, Pt(r.min.x+d, r.min.y+d), d, d, f, ZP);
			fillellipse(w->img, Pt(r.max.x-1-d, r.min.y+d), d, d, f, ZP);
			fillellipse(w->img, Pt(r.min.x+d, r.max.y-1-d), d, d, f, ZP);
			fillellipse(w->img, Pt(r.max.x-1-d, r.max.y-1-d), d, d, f, ZP);
		}
	}
}

static void
drawbutton(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	ulong fg, bg, line;
	int en;

	w = c->w;
	en = (c->state & Cenabled) != 0;
	if((c->state & Cdefault) && en)
		/* a 2 px ring with a 1 px gap outside the button */
		roundrect(w, insetrect(r, -UXbuttondefaultring), UXbuttonradius+UXbuttondefaultring, 2, 0, UXink);
	line = en ? UXink : UXcoldisabled;
	bg = c->pressed == 1 ? UXcolselect : UXpaper;
	fg = !en ? UXcoldisabled : c->pressed == 1 ? UXcolselecttext : UXink;
	roundrect(w, r, UXbuttonradius, 1, bg, line);
	if(w->focus == c && c->pressed == 0 && !(c->state & Cdefault))
		roundrect(w, insetrect(r, 2), UXbuttonradius-1, 1, 0, UXcolfocus);
	string(w->img, centred(w->sysfont, r, c->label), _color(w, fg), ZP, w->sysfont, c->label);
}

static void
bullets(Uxwin *w, Point p, int n, int ht, Image *col)
{
	int i;

	for(i = 0; i < n; i++)
		fillellipse(w->img, Pt(p.x + i*7 + 3, p.y + ht/2), 2, 2, col, ZP);
}

static void
drawtext(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Image *fg, *bg, *selbg;
	Font *f;
	Point p;
	int q, x, ht, focused, cw;
	Rectangle in;

	w = c->w;
	f = w->font;
	ht = f->height;
	focused = w->focus == c && active(w);
	fg = _color(w, (c->state & Cenabled) ? UXink : UXcoldisabled);
	bg = _color(w, UXcolfield);
	selbg = _color(w, UXcoltextselect);
	if(focused)
		frame(w, insetrect(r, -UXfieldfocusring), UXfieldfocusring, UXcolfocus);
	draw(w->img, r, bg, nil, ZP);
	frame(w, r, 1, (c->state & UXreadonly) ? UXcoldisabled : UXink);
	in = insetrect(r, 1);
	in.min.x += UXfieldpad-1;
	in.max.x -= UXfieldpad-1;
	in.min.y += (c->state & UXmultiline) ? 2 : (Dy(in)-ht)/2;
	replclipr(w->img, 0, insetrect(r, 1));
	if(c->state & UXsecret){
		bullets(w, in.min, c->ntext, ht, fg);
		if(focused)
			draw(w->img, Rect(in.min.x + c->q0*7, in.min.y, in.min.x + c->q0*7 + 1, in.min.y+ht), _color(w, UXink), nil, ZP);
		replclipr(w->img, 0, w->img->r);
		return;
	}
	/* one-line: keep the caret in view */
	if(!(c->state & UXmultiline)){
		if(c->q1 < c->org)
			c->org = c->q1;
		while(c->org < c->q1 && runestringnwidth(f, c->text+c->org, c->q1-c->org) > Dx(in)-2)
			c->org++;
	}else
		c->org = 0;
	p = in.min;
	x = p.x;
	for(q = c->org; q <= c->ntext; q++){
		if(focused && q == c->q0 && c->q0 == c->q1)
			draw(w->img, Rect(x, p.y, x+1, p.y+ht), _color(w, UXink), nil, ZP);
		if(q == c->ntext)
			break;
		if(c->text[q] == '\n'){
			p.y += ht;
			x = p.x;
			if(p.y+ht > in.max.y)
				break;
			continue;
		}
		cw = runestringnwidth(f, c->text+q, 1);
		if(x+cw > in.max.x && !(c->state & UXmultiline))
			break;
		x = runestringnbg(w->img, Pt(x, p.y), fg, ZP, f, c->text+q, 1,
			q >= c->q0 && q < c->q1 && focused ? selbg : bg, ZP).x;
	}
	replclipr(w->img, 0, w->img->r);
}

static void
drawcheck(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle b;
	ulong ink;

	w = c->w;
	ink = (c->state & Cenabled) ? UXink : UXcoldisabled;
	b = Rect(0, 0, UXchecksize, UXchecksize);
	b = rectaddpt(b, Pt(r.min.x, r.min.y + (Dy(r)-UXchecksize)/2));
	if(c->pressed){
		fill(w, b, UXcoltrack);
		frame(w, b, 1, ink);
		frame(w, insetrect(b, 1), 1, ink);
	}else{
		fill(w, b, UXpaper);
		frame(w, b, 1, ink);
	}
	if(c->check == 1)
		glyphin(w, "check", insetrect(b, 1), _color(w, ink));
	else if(c->check == 2)
		glyphin(w, "mixed", b, _color(w, ink));
	if(w->focus == c && active(w))
		frame(w, insetrect(b, -2), 1, UXcolfocus);
	string(w->img, Pt(b.max.x+UXcheckgap, r.min.y + (Dy(r)-w->sysfont->height)/2),
		_color(w, ink), ZP, w->sysfont, c->label);
}

static void
drawlabel(Uxctl *c, Rectangle r)
{
	char *s;

	s = _runestr(c->text, c->ntext);
	string(c->w->img, Pt(r.min.x, r.min.y + (Dy(r)-c->w->sysfont->height)/2),
		_color(c->w, (c->state & Cenabled) ? UXink : UXcoldisabled), ZP, c->w->sysfont, s);
	free(s);
}

static ulong
winbg(Uxwin *w)
{
	if(strcmp(w->kind, "dialog") == 0 || strcmp(w->kind, "alert") == 0)
		return UXcolface;
	return UXpaper;
}

static void
drawgroup(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Point p;
	Font *f;

	w = c->w;
	f = w->sysfont;
	r.min.y += f->height/2;
	frame(w, r, 1, UXcoldisabled);
	if(c->label[0]){
		p = Pt(r.min.x+UXcontrolgap, r.min.y - f->height/2);
		stringbg(w->img, p, _color(w, UXink), ZP, f, c->label, _color(w, winbg(w)), Pt(-4, 0));
	}
}

static void
drawlist(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle lr, in;
	int i, y0, sel;
	Font *f;

	w = c->w;
	f = w->font;
	fill(w, r, UXpaper);
	frame(w, r, 1, UXink);
	in = insetrect(r, 1);
	y0 = in.min.y;
	if(c->t == ctltypes[8] && c->dir){
		/* the chooser's directory, as a header */
		lr = Rect(in.min.x, y0, in.max.x, y0+UXlistrowheight);
		fill(w, lr, UXcolface);
		string(w->img, Pt(lr.min.x+UXlistpad, lr.min.y + (UXlistrowheight-f->height)/2),
			_color(w, UXink), ZP, f, c->dir);
		y0 += UXlistrowheight;
		fill(w, Rect(in.min.x, y0, in.max.x, y0+1), UXcoldisabled);
		y0++;
	}
	/* keep the first selected item in view */
	for(i = 0; i < c->nitems; i++)
		if(c->sel[i]){
			if(i < c->top)
				c->top = i;
			while(y0 + (i-c->top+1)*UXlistrowheight > in.max.y && c->top < i)
				c->top++;
			break;
		}
	for(i = c->top; i < c->nitems; i++){
		lr = Rect(in.min.x, y0 + (i-c->top)*UXlistrowheight, in.max.x, y0 + (i-c->top+1)*UXlistrowheight);
		if(lr.max.y > in.max.y)
			break;
		sel = c->sel[i];
		if(sel)
			fill(w, lr, active(w) ? UXcolselect : UXcoltrack);
		string(w->img, Pt(lr.min.x+UXlistpad, lr.min.y + (UXlistrowheight-f->height)/2),
			_color(w, sel && active(w) ? UXcolselecttext : UXink), ZP, f, c->items[i]);
	}
}

/* darkens a selected icon (assets/README.md, Finder: Selected), through the icon's mask */
static Image*
shade(Uxwin *w)
{
	if(w->col[nelem(palette)] == nil)
		w->col[nelem(palette)] = allocimage(w->d, Rect(0,0,1,1), RGBA32, 1, setalpha(0x000033FF, 0x80));
	return w->col[nelem(palette)];
}

/*
 * The desktop: the desktop colour, tiled with the pattern image named
 * by $uxpattern (an assets/gen/patterns tile) when there is one.
 */
static void
deskbg(Uxwin *w, Rectangle r)
{
	static Display *pd;
	static Image *pat;
	static int tried;
	char *f;
	int fd;

	fill(w, r, UXcoldesktop);
	if(pd != w->d){
		pd = w->d;
		pat = nil;
		tried = 0;
	}
	if(!tried){
		tried = 1;
		f = getenv("uxpattern");
		if(f != nil && (fd = open(f, OREAD)) >= 0){
			pat = readimage(w->d, fd, 0);
			if(pat != nil)
				replclipr(pat, 1, Rect(-0x3FFFFFF, -0x3FFFFFF, 0x3FFFFFF, 0x3FFFFFF));
			close(fd);
		}
		free(f);
	}
	if(pat != nil)
		draw(w->img, r, pat, nil, ZP);
}

/* the label, elided in the middle to fit max pixels; malloc'd */
static char*
elide(Font *f, char *s, int max)
{
	Rune *r, *e;
	int n, a, b;
	char *t;

	if(stringwidth(f, s) <= max)
		return estrdup(s);
	r = runesmprint("%s", s);
	n = runestrlen(r);
	e = emalloc((n+2)*sizeof(Rune));
	for(a = n/2, b = n - n/2; a > 0; ){
		memmove(e, r, a*sizeof(Rune));
		e[a] = L'…';
		memmove(e+a+1, r+n-b, b*sizeof(Rune));
		e[a+1+b] = 0;
		if(runestringwidth(f, e) <= max)
			break;
		if(a >= b)
			a--;
		else
			b--;
	}
	if(a == 0){
		e[0] = L'…';
		e[1] = 0;
	}
	t = smprint("%S", e);
	free(r);
	free(e);
	return t;
}

static void
drawicons(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Icon *ic;
	Point p, lp;
	Rectangle ir, lr;
	Font *f;
	Image *img;
	int i, lw, desk;
	char *l;

	w = c->w;
	f = w->font;
	desk = strcmp(w->kind, "desktop") == 0;
	if(desk)
		deskbg(w, r);
	else
		fill(w, r, UXpaper);
	replclipr(w->img, 0, r);
	for(i = 0; i < c->nicons; i++){
		ic = &c->icons[i];
		/* pos is the icon's centre */
		p = subpt(addpt(r.min, ic->pt), c->origin);
		ir = Rect(p.x-UXiconlarge/2, p.y-UXiconlarge/2, p.x+UXiconlarge/2, p.y+UXiconlarge/2);
		if((img = _resicon(w, ic->icon, 0)) != nil){
			draw(w->img, ir, img, nil, img->r.min);
			if(ic->sel)
				draw(w->img, ir, shade(w), img, img->r.min);
		}else{
			/* no icon resource */
			fill(w, ir, ic->sel ? UXgrey4 : UXgrey1);
			frame(w, ir, 1, UXink);
		}
		if(ic->label[0] == 0)
			continue;
		l = elide(f, ic->label, UXiconviewlabelmax);
		lw = stringwidth(f, l);
		lp = Pt(p.x - lw/2, ir.max.y + UXiconviewlabelgap);
		lr = Rect(lp.x-UXiconviewlabelpad, lp.y, lp.x+lw+UXiconviewlabelpad, lp.y+f->height);
		if(ic->sel){
			fill(w, lr, active(w) ? UXcolselect : UXcoltrack);
			string(w->img, lp, _color(w, active(w) ? UXcolselecttext : UXink), ZP, f, l);
		}else{
			/* on the desktop a label sits on a paper plate, to read over any pattern */
			if(desk)
				fill(w, lr, UXpaper);
			string(w->img, lp, _color(w, UXink), ZP, f, l);
		}
		free(l);
	}
	if(c->drag == Dband && Dx(c->band) > 0 && Dy(c->band) > 0)
		frame(w, c->band, 1, desk ? UXpaper : UXgrey4);
	replclipr(w->img, 0, w->img->r);
}

static void
drawscroll(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle bar, up, down, track, th;
	int h, ch, canscroll;
	ulong arrow;

	w = c->w;
	frame(w, r, 1, UXink);
	bar = Rect(r.max.x-UXscrollwidth, r.min.y, r.max.x, r.max.y);
	fill(w, insetrect(bar, 1), UXpaper);
	frame(w, bar, 1, UXink);
	if(!active(w))
		return;	/* inactive windows show a blank bar */
	ch = Dy(c->extent);
	canscroll = ch > Dy(r);
	up = Rect(bar.min.x, bar.min.y, bar.max.x, bar.min.y+UXscrollwidth);
	down = Rect(bar.min.x, bar.max.y-UXscrollwidth, bar.max.x, bar.max.y);
	track = Rect(bar.min.x+1, up.max.y, bar.max.x-1, down.min.y);
	arrow = canscroll ? UXink : UXcoldisabled;
	fill(w, insetrect(up, 1), UXcolface);
	fill(w, insetrect(down, 1), UXcolface);
	frame(w, up, 1, UXink);
	frame(w, down, 1, UXink);
	glyphin(w, "arrow-up", up, _color(w, arrow));
	glyphin(w, "arrow-down", down, _color(w, arrow));
	if(!canscroll)
		return;
	fill(w, track, UXcoltrack);
	h = Dy(track);
	th = track;
	th.min.y = track.min.y + (vlong)h*(c->origin.y - c->extent.min.y)/ch;
	th.max.y = th.min.y + (vlong)h*Dy(r)/ch;
	if(th.max.y - th.min.y < UXscrollthumbmin)
		th.max.y = th.min.y + UXscrollthumbmin;
	if(th.max.y > track.max.y){
		th.min.y -= th.max.y - track.max.y;
		th.max.y = track.max.y;
	}
	if(th.min.y < track.min.y)
		th.min.y = track.min.y;
	th.min.x--;
	th.max.x++;
	fill(w, th, UXpaper);
	fill(w, Rect(th.min.x, th.min.y-1, th.max.x, th.min.y), UXink);
	fill(w, Rect(th.min.x, th.max.y, th.max.x, th.max.y+1), UXink);
	glyphin(w, "thumbgrip", th, _color(w, UXink));
}

void
_drawctl(Uxctl *c)
{
	Rectangle r;
	char *n;

	if(!(c->state & Cvisible) || c->w->img == nil)
		return;
	r = _abs(c);
	n = c->t->name;
	if(strcmp(n, "button") == 0)
		drawbutton(c, r);
	else if(strcmp(n, "text-field") == 0)
		drawtext(c, r);
	else if(strcmp(n, "checkbox") == 0)
		drawcheck(c, r);
	else if(strcmp(n, "label") == 0)
		drawlabel(c, r);
	else if(strcmp(n, "group") == 0)
		drawgroup(c, r);
	else if(strcmp(n, "list") == 0 || strcmp(n, "file-chooser") == 0)
		drawlist(c, r);
	else if(strcmp(n, "icon-view") == 0)
		drawicons(c, r);
	else if(strcmp(n, "scroll-view") == 0)
		drawscroll(c, r);
}

void
_redraw(Uxwin *w)
{
	Rectangle r;
	int i;

	w->dirty = 0;
	if(w->img == nil)
		return;
	r = w->img->r;
	fill(w, r, winbg(w));
	if(strcmp(w->kind, "dialog") == 0 || strcmp(w->kind, "alert") == 0){
		/* rio's border is the teal band; libux draws the ink frame inside it */
		frame(w, r, 1, UXink);
	}
	for(i = 0; i < w->nctl; i++)
		_drawctl(w->ctl[i]);
	_resdraw(w);
	flushimage(w->d, 1);
}

void
_drawdirty(void)
{
	int i;

	for(i = 0; i < ux.nwin; i++)
		if(ux.win[i]->dirty)
			_redraw(ux.win[i]);
}
