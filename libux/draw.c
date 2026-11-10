/*
 * Drawing controls with libdraw. Placeholder look until the visual
 * design (assets/) lands: black on white, classic proportions.
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

static ulong colors[Ncolor] = {
	[Cwhite]	DWhite,
	[Cblack]	DBlack,
	[Cgray]	0x888888FF,
	[Cdark]	0x444444FF,
	[Clight]	0xDDDDDDFF,
	[Csel]	DBlack,
	[Cshade]	0x00000080,	/* half black, premultiplied: darkens a selected icon */
};

Image*
_color(Uxwin *w, int i)
{
	if(w->col[i] == nil)
		w->col[i] = allocimage(w->d, Rect(0,0,1,1), i == Cshade ? RGBA32 : w->img->chan, 1, colors[i]);
	return w->col[i];
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
	return rectaddpt(r, insetrect(c->w->img->r, 0).min);
}

static void
text(Uxwin *w, Point p, char *s, Image *col)
{
	string(w->img, p, col, ZP, w->font, s);
}

static Point
centered(Uxwin *w, Rectangle r, char *s)
{
	Point sz;

	sz = stringsize(w->font, s);
	return Pt(r.min.x + (Dx(r)-sz.x)/2, r.min.y + (Dy(r)-sz.y)/2);
}

static void
drawbutton(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Image *fg, *bg;

	w = c->w;
	if(c->state & Cdefault){
		border(w->img, insetrect(r, -4), 3, _color(w, Cblack), ZP);
	}
	bg = _color(w, c->pressed == 1 ? Cblack : Cwhite);
	fg = _color(w, (c->state & Cenabled) ? (c->pressed == 1 ? Cwhite : Cblack) : Cgray);
	draw(w->img, insetrect(r, 1), bg, nil, ZP);
	border(w->img, r, 1, _color(w, Cblack), ZP);
	/* round the corners */
	draw(w->img, Rect(r.min.x, r.min.y, r.min.x+1, r.min.y+1), _color(w, Cwhite), nil, ZP);
	draw(w->img, Rect(r.max.x-1, r.min.y, r.max.x, r.min.y+1), _color(w, Cwhite), nil, ZP);
	draw(w->img, Rect(r.min.x, r.max.y-1, r.min.x+1, r.max.y), _color(w, Cwhite), nil, ZP);
	draw(w->img, Rect(r.max.x-1, r.max.y-1, r.max.x, r.max.y), _color(w, Cwhite), nil, ZP);
	if(w->focus == c && c->pressed == 0)
		border(w->img, insetrect(r, 2), 1, _color(w, Clight), ZP);
	text(w, centered(w, r, c->label), c->label, fg);
}

static void
drawtext(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Image *fg;
	Point p;
	Rune bullet, *s;
	int i, q, x, ht, focused;
	Rectangle in;

	w = c->w;
	ht = w->font->height;
	fg = _color(w, (c->state & Cenabled) ? Cblack : Cgray);
	draw(w->img, r, _color(w, Cwhite), nil, ZP);
	border(w->img, r, 1, _color(w, (c->state & UXreadonly) ? Cgray : Cblack), ZP);
	in = insetrect(r, 3);
	focused = w->focus == c;
	bullet = L'*';
	s = c->text;
	if(c->state & UXsecret){
		s = emalloc((c->ntext+1)*sizeof(Rune));
		for(i = 0; i < c->ntext; i++)
			s[i] = bullet;
	}
	/* one-line: keep the caret in view */
	if(!(c->state & UXmultiline)){
		if(c->q1 < c->org)
			c->org = c->q1;
		while(c->org < c->q1 && runestringnwidth(w->font, s+c->org, c->q1-c->org) > Dx(in)-2)
			c->org++;
	}else
		c->org = 0;
	p = in.min;
	x = p.x;
	for(q = c->org; q <= c->ntext; q++){
		if(focused && q >= c->q0 && q < c->q1)
			draw(w->img, Rect(x, p.y, x+runestringnwidth(w->font, s+q, 1), p.y+ht), _color(w, Clight), nil, ZP);
		if(focused && q == c->q0 && c->q0 == c->q1)
			draw(w->img, Rect(x, p.y, x+1, p.y+ht), _color(w, Cblack), nil, ZP);
		if(q == c->ntext)
			break;
		if(s[q] == '\n'){
			p.y += ht;
			x = p.x;
			if(p.y+ht > in.max.y)
				break;
			continue;
		}
		if(x+runestringnwidth(w->font, s+q, 1) > in.max.x)
			continue;
		x = runestringnbg(w->img, Pt(x, p.y), fg, ZP, w->font, s+q, 1,
			focused && q >= c->q0 && q < c->q1 ? _color(w, Clight) : _color(w, Cwhite), ZP).x;
	}
	if(s != c->text)
		free(s);
}

static void
drawcheck(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle b;
	Image *fg;

	w = c->w;
	fg = _color(w, (c->state & Cenabled) ? Cblack : Cgray);
	b = Rect(r.min.x, r.min.y + (Dy(r)-12)/2, r.min.x+12, r.min.y + (Dy(r)-12)/2 + 12);
	draw(w->img, b, _color(w, Cwhite), nil, ZP);
	border(w->img, b, 1, fg, ZP);
	if(c->check == 1){
		line(w->img, addpt(b.min, Pt(2,2)), subpt(b.max, Pt(3,3)), 0, 0, 0, fg, ZP);
		line(w->img, Pt(b.max.x-3, b.min.y+2), Pt(b.min.x+2, b.max.y-3), 0, 0, 0, fg, ZP);
	}else if(c->check == 2)
		draw(w->img, Rect(b.min.x+3, b.min.y+5, b.max.x-3, b.min.y+7), fg, nil, ZP);
	if(w->focus == c)
		border(w->img, insetrect(b, -2), 1, _color(w, Clight), ZP);
	text(w, Pt(b.max.x+6, r.min.y + (Dy(r)-w->font->height)/2), c->label, fg);
}

static void
drawlabel(Uxctl *c, Rectangle r)
{
	char *s;

	s = _runestr(c->text, c->ntext);
	text(c->w, r.min, s, _color(c->w, (c->state & Cenabled) ? Cblack : Cgray));
	free(s);
}

static void
drawgroup(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Point p;

	w = c->w;
	r.min.y += w->font->height/2;
	border(w->img, r, 1, _color(w, Cgray), ZP);
	if(c->label[0]){
		p = Pt(r.min.x+8, r.min.y - w->font->height/2);
		stringbg(w->img, p, _color(w, Cblack), ZP, w->font, c->label, _color(w, Cwhite), ZP);
	}
}

static void
drawlist(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle lr, in;
	int i, lh, y0;

	w = c->w;
	lh = w->font->height + 2;
	draw(w->img, r, _color(w, Cwhite), nil, ZP);
	border(w->img, r, 1, _color(w, Cblack), ZP);
	in = insetrect(r, 2);
	y0 = in.min.y;
	if(c->t == ctltypes[8] && c->dir){
		text(w, Pt(in.min.x+2, y0), c->dir, _color(w, Cdark));
		y0 += lh;
		draw(w->img, Rect(in.min.x, y0-1, in.max.x, y0), _color(w, Cgray), nil, ZP);
	}
	/* keep the first selected item in view */
	for(i = 0; i < c->nitems; i++)
		if(c->sel[i]){
			if(i < c->top)
				c->top = i;
			while(y0 + (i-c->top+1)*lh > in.max.y && c->top < i)
				c->top++;
			break;
		}
	for(i = c->top; i < c->nitems; i++){
		lr = Rect(in.min.x, y0 + (i-c->top)*lh, in.max.x, y0 + (i-c->top+1)*lh);
		if(lr.max.y > in.max.y)
			break;
		if(c->sel[i])
			draw(w->img, lr, _color(w, Csel), nil, ZP);
		text(w, Pt(lr.min.x+2, lr.min.y+1), c->items[i], _color(w, c->sel[i] ? Cwhite : Cblack));
	}
}

static void
drawicons(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Icon *ic;
	Point p;
	Rectangle ir;
	Image *img;
	int i;

	w = c->w;
	draw(w->img, r, _color(w, Cwhite), nil, ZP);
	replclipr(w->img, 0, r);
	for(i = 0; i < c->nicons; i++){
		ic = &c->icons[i];
		p = addpt(subpt(addpt(r.min, ic->pt), c->origin), Pt(0, 0));
		ir = Rect(p.x-16, p.y-16, p.x+16, p.y+16);
		if((img = _resicon(w, ic->icon, 0)) != nil){
			draw(w->img, ir, img, nil, img->r.min);
			if(ic->sel)
				draw(w->img, ir, _color(w, Cshade), img, img->r.min);
		}else{
			draw(w->img, ir, _color(w, ic->sel ? Cdark : Clight), nil, ZP);
			border(w->img, ir, 1, _color(w, Cblack), ZP);
		}
		p = Pt(p.x - stringwidth(w->font, ic->label)/2, ir.max.y+2);
		if(ic->sel)
			stringbg(w->img, p, _color(w, Cwhite), ZP, w->font, ic->label, _color(w, Csel), ZP);
		else
			text(w, p, ic->label, _color(w, Cblack));
	}
	replclipr(w->img, 0, w->img->r);
}

static void
drawscroll(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle bar, th;
	int h, ch;

	w = c->w;
	border(w->img, r, 1, _color(w, Cblack), ZP);
	bar = Rect(r.max.x-12, r.min.y+1, r.max.x-1, r.max.y-1);
	draw(w->img, bar, _color(w, Clight), nil, ZP);
	ch = Dy(c->extent);
	h = Dy(bar);
	if(ch > 0 && ch > Dy(r)){
		th = bar;
		th.min.y = bar.min.y + (vlong)h*(c->origin.y - c->extent.min.y)/ch;
		th.max.y = th.min.y + (vlong)h*Dy(r)/ch;
		if(th.max.y > bar.max.y)
			th.max.y = bar.max.y;
		draw(w->img, th, _color(w, Cgray), nil, ZP);
	}
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
	int i;

	w->dirty = 0;
	if(w->img == nil)
		return;
	draw(w->img, w->img->r, _color(w, Cwhite), nil, ZP);
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
