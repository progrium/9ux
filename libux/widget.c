/*
 * spike: controls a uitree host needs that the Toolbox didn't have: a
 * pop-up menu button (select), radio buttons, a slider, a switch and a
 * tab bar, plus a pop-up menu over a window (select's menu and context
 * menus). Each is drawn in the classic look and tells its handler
 * UXvalue when a person changes it; uxchoice reads the new value.
 */
#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <draw.h>
#include <mouse.h>
#include <keyboard.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "ux.h"
#include "impl.h"

static void
_kickui(void)
{
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
}

enum {
	Radiosize = 12,
	Switchw = 28,
	Switchh = 16,
	Slidew = 160,
	Thumbw = 10,
	Thumbh = 16,
	Tabh = 22,
	Tabpad = 12,
	Popupad = 8,
	Arroww = 16,
};

static char *nofiles[] = { nil };
static Verb noverbs[] = { nil };

static int	popupmouse(Uxctl*, Mouse*);
static int	radiomouse(Uxctl*, Mouse*);
static int	slidermouse(Uxctl*, Mouse*);
static int	switchmouse(Uxctl*, Mouse*);
static int	tabmouse(Uxctl*, Mouse*);

Ctltype _popuptype = { "popup", nofiles, noverbs, nil, nil, nil, popupmouse, nil, 0 };
Ctltype _radiotype = { "radios", nofiles, noverbs, nil, nil, nil, radiomouse, nil, 0 };
Ctltype _slidertype = { "slider", nofiles, noverbs, nil, nil, nil, slidermouse, nil, 0 };
Ctltype _switchtype = { "switch", nofiles, noverbs, nil, nil, nil, switchmouse, nil, 0 };
Ctltype _tabbartype = { "tabbar", nofiles, noverbs, nil, nil, nil, tabmouse, nil, 0 };

extern Ctltype _canvastype;

int
_iswidget(Uxctl *c)
{
	return c->t == &_popuptype || c->t == &_radiotype || c->t == &_slidertype
		|| c->t == &_switchtype || c->t == &_tabbartype;
}

static void
note(Uxctl *c, int type)
{
	Uxev e;

	memset(&e, 0, sizeof e);
	e.type = type;
	e.via = UXui;
	e.win = c->w;
	e.ctl = c;
	_notify(c->fn, c->arg, &e);
}

static Image*
col(Uxwin *w, ulong rgba)
{
	return _color(w, rgba);
}

static ulong
ink(Uxctl *c)
{
	return (c->state & Cenabled) ? UXink : UXcoldisabled;
}

static char*
itemlabel(Uxctl *c, int i)
{
	if(i < 0 || i >= c->nitems)
		return nil;
	return c->items[i];
}

/* the size a widget wants; x < 0 when c isn't one */
Point
_widgetsize(Uxctl *c, int availw)
{
	Font *f;
	int i, x, m;

	USED(availw);
	f = c->w->sysfont;
	if(c->t == &_popuptype){
		m = stringwidth(f, c->label);
		for(i = 0; i < c->nitems; i++)
			if(strcmp(c->items[i], "-") != 0 && (x = stringwidth(f, c->items[i])) > m)
				m = x;
		return Pt(m + 2*Popupad + Arroww, UXbuttonheight);
	}
	if(c->t == &_radiotype){
		if(c->row){
			x = 0;
			for(i = 0; i < c->nitems; i++)
				x += Radiosize + UXcheckgap + stringwidth(f, c->items[i]) + 16;
			return Pt(x, UXbuttonheight);
		}
		m = 0;
		for(i = 0; i < c->nitems; i++)
			if((x = stringwidth(f, c->items[i])) > m)
				m = x;
		return Pt(Radiosize + UXcheckgap + m, c->nitems * UXbuttonheight);
	}
	if(c->t == &_slidertype)
		return Pt(Slidew, Thumbh + 4);
	if(c->t == &_switchtype)
		return Pt(Switchw + (c->label[0] ? UXcheckgap + stringwidth(f, c->label) : 0), UXbuttonheight);
	if(c->t == &_tabbartype){
		x = 0;
		for(i = 0; i < c->nitems; i++)
			x += stringwidth(f, c->items[i]) + 2*Tabpad - 1;
		return Pt(x + 8, Tabh);
	}
	return Pt(-1, -1);
}

/* popup: a pop-up menu button */

static void
drawpopup(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle b;
	char *s;
	Point p;
	int y;
	Point tri[3];

	w = c->w;
	b = Rect(r.min.x, r.min.y, r.max.x-1, r.max.y-1);
	draw(w->img, b, col(w, c->pressed ? UXcolselect : UXpaper), nil, ZP);
	border(w->img, b, 1, col(w, ink(c)), ZP);
	/* the drop shadow, right and below */
	draw(w->img, Rect(b.min.x+2, b.max.y, b.max.x+1, b.max.y+1), col(w, ink(c)), nil, ZP);
	draw(w->img, Rect(b.max.x, b.min.y+2, b.max.x+1, b.max.y+1), col(w, ink(c)), nil, ZP);
	s = itemlabel(c, c->check);
	if(s == nil)
		s = c->label;
	p = Pt(b.min.x+Popupad, b.min.y + (Dy(b)-w->sysfont->height)/2);
	string(w->img, p, col(w, c->pressed ? UXcolselecttext : (s == c->label ? UXcoldisabled : ink(c))), ZP, w->sysfont, s);
	y = b.min.y + Dy(b)/2 - 2;
	tri[0] = Pt(b.max.x-Arroww+2, y);
	tri[1] = Pt(b.max.x-6, y);
	tri[2] = Pt(b.max.x-Arroww/2-2, y+5);
	fillpoly(w->img, tri, 3, 0, col(w, c->pressed ? UXcolselecttext : ink(c)), ZP);
}

static void
popupchose(Uxwin *w, int i, void *arg)
{
	Uxctl *c;

	c = arg;
	c->pressed = 0;
	w->dirty = 1;
	if(i >= 0 && i != c->check){
		c->check = i;
		note(c, UXvalue);
	}
}

static int
popupmouse(Uxctl *c, Mouse *m)
{
	Rectangle r;

	if(!(m->buttons & 1))
		return 0;
	c->pressed = 1;
	r = _abs(c);
	_popopen(c->w, Pt(r.min.x, r.max.y), c->items, c->nitems, c->check, popupchose, c);
	return 0;
}

/* radios: one of a list */

static Rectangle
radiorect(Uxctl *c, Rectangle r, int i)
{
	int j, x;

	if(!c->row)
		return Rect(r.min.x, r.min.y + i*UXbuttonheight, r.max.x, r.min.y + (i+1)*UXbuttonheight);
	x = r.min.x;
	for(j = 0; j < i; j++)
		x += Radiosize + UXcheckgap + stringwidth(c->w->sysfont, c->items[j]) + 16;
	return Rect(x, r.min.y, x + Radiosize + UXcheckgap + stringwidth(c->w->sysfont, c->items[i]) + 16, r.max.y);
}

static void
drawradios(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle ir;
	Point o;
	int i;

	w = c->w;
	for(i = 0; i < c->nitems; i++){
		ir = radiorect(c, r, i);
		o = Pt(ir.min.x + Radiosize/2, ir.min.y + Dy(ir)/2);
		fillellipse(w->img, o, Radiosize/2, Radiosize/2, col(w, c->pressed && c->lastitem == i ? UXcoltrack : UXpaper), ZP);
		ellipse(w->img, o, Radiosize/2, Radiosize/2, 0, col(w, ink(c)), ZP);
		if(i == c->check)
			fillellipse(w->img, o, 3, 3, col(w, ink(c)), ZP);
		string(w->img, Pt(ir.min.x + Radiosize + UXcheckgap, ir.min.y + (Dy(ir)-w->sysfont->height)/2),
			col(w, ink(c)), ZP, w->sysfont, c->items[i]);
	}
}

static int
radiomouse(Uxctl *c, Mouse *m)
{
	int i;
	Rectangle r;

	r = _abs(c);
	for(i = 0; i < c->nitems; i++)
		if(ptinrect(m->xy, radiorect(c, r, i)))
			break;
	if(m->buttons & 1){
		c->pressed = 1;
		c->lastitem = i < c->nitems ? i : -1;
		c->w->dirty = 1;
		return 1;
	}
	if(c->pressed){
		c->pressed = 0;
		c->w->dirty = 1;
		if(i < c->nitems && i == c->lastitem && i != c->check){
			c->check = i;
			note(c, UXvalue);
		}
	}
	return 0;
}

/* slider: val in vmin..vmax, by vstep */

static int
sliderx(Uxctl *c, Rectangle r)
{
	int span;

	span = c->vmax - c->vmin;
	if(span <= 0)
		return r.min.x + Thumbw/2;
	return r.min.x + Thumbw/2 + (vlong)(Dx(r) - Thumbw) * (c->val - c->vmin) / span;
}

static void
drawslider(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle t, th;
	int x, y;

	w = c->w;
	y = r.min.y + Dy(r)/2;
	t = Rect(r.min.x + Thumbw/2, y-2, r.max.x - Thumbw/2, y+2);
	x = sliderx(c, r);
	draw(w->img, t, col(w, UXcoltrack), nil, ZP);
	draw(w->img, Rect(t.min.x, t.min.y, x, t.max.y), col(w, (c->state & Cenabled) ? UXcolselect : UXcoldisabled), nil, ZP);
	border(w->img, t, 1, col(w, ink(c)), ZP);
	th = Rect(x - Thumbw/2, y - Thumbh/2, x + Thumbw/2, y + Thumbh/2);
	draw(w->img, th, col(w, c->pressed ? UXcoltrack : UXpaper), nil, ZP);
	border(w->img, th, 1, col(w, ink(c)), ZP);
	draw(w->img, Rect(th.min.x+1, th.max.y, th.max.x+1, th.max.y+1), col(w, ink(c)), nil, ZP);
	draw(w->img, Rect(th.max.x, th.min.y+1, th.max.x+1, th.max.y+1), col(w, ink(c)), nil, ZP);
}

static int
slideval(Uxctl *c, Rectangle r, int x)
{
	int span, v, st;

	span = Dx(r) - Thumbw;
	if(span <= 0)
		return c->vmin;
	v = c->vmin + (vlong)(x - r.min.x - Thumbw/2) * (c->vmax - c->vmin) / span;
	st = c->vstep > 0 ? c->vstep : 1;
	v = c->vmin + (v - c->vmin + st/2) / st * st;
	if(v < c->vmin)
		v = c->vmin;
	if(v > c->vmax)
		v = c->vmax;
	return v;
}

static int
slidermouse(Uxctl *c, Mouse *m)
{
	int v;

	if(m->buttons & 1){
		c->pressed = 1;
		v = slideval(c, _abs(c), m->xy.x);
		if(v != c->val){
			c->val = v;
			note(c, UXvalue);
		}
		c->w->dirty = 1;
		return 1;
	}
	if(c->pressed){
		c->pressed = 0;
		c->w->dirty = 1;
		note(c, UXsubmit);	/* the drag ended: commit */
	}
	return 0;
}

/* switch: a sliding on/off */

static void
drawswitch(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle t;
	int y, rad, kx;

	w = c->w;
	y = r.min.y + (Dy(r)-Switchh)/2;
	t = Rect(r.min.x, y, r.min.x+Switchw, y+Switchh);
	rad = Switchh/2;
	fillellipse(w->img, Pt(t.min.x+rad, t.min.y+rad), rad, rad, col(w, c->check ? UXcolselect : UXcoltrack), ZP);
	fillellipse(w->img, Pt(t.max.x-rad-1, t.min.y+rad), rad, rad, col(w, c->check ? UXcolselect : UXcoltrack), ZP);
	draw(w->img, Rect(t.min.x+rad, t.min.y, t.max.x-rad-1, t.max.y+1), col(w, c->check ? UXcolselect : UXcoltrack), nil, ZP);
	ellipse(w->img, Pt(t.min.x+rad, t.min.y+rad), rad, rad, 0, col(w, ink(c)), ZP);
	ellipse(w->img, Pt(t.max.x-rad-1, t.min.y+rad), rad, rad, 0, col(w, ink(c)), ZP);
	draw(w->img, Rect(t.min.x+rad, t.min.y, t.max.x-rad-1, t.min.y+1), col(w, ink(c)), nil, ZP);
	draw(w->img, Rect(t.min.x+rad, t.min.y+2*rad, t.max.x-rad-1, t.min.y+2*rad+1), col(w, ink(c)), nil, ZP);
	draw(w->img, Rect(t.min.x+rad, t.min.y+1, t.max.x-rad-1, t.min.y+2*rad), col(w, c->check ? UXcolselect : UXcoltrack), nil, ZP);
	kx = c->check ? t.max.x-rad-1 : t.min.x+rad;
	fillellipse(w->img, Pt(kx, t.min.y+rad), rad-2, rad-2, col(w, UXpaper), ZP);
	ellipse(w->img, Pt(kx, t.min.y+rad), rad-2, rad-2, 0, col(w, ink(c)), ZP);
	if(c->label[0])
		string(w->img, Pt(t.max.x+UXcheckgap, r.min.y + (Dy(r)-w->sysfont->height)/2),
			col(w, ink(c)), ZP, w->sysfont, c->label);
}

static int
switchmouse(Uxctl *c, Mouse *m)
{
	if(m->buttons & 1){
		c->pressed = 1;
		return 1;
	}
	if(c->pressed){
		c->pressed = 0;
		if(ptinrect(m->xy, _abs(c))){
			c->check = !c->check;
			c->w->dirty = 1;
			note(c, UXvalue);
		}
	}
	return 0;
}

/* tabbar: tabs across the top of a panel */

static Rectangle
tabrect(Uxctl *c, Rectangle r, int i)
{
	int j, x;

	x = r.min.x + 4;
	for(j = 0; j < i; j++)
		x += stringwidth(c->w->sysfont, c->items[j]) + 2*Tabpad - 1;
	return Rect(x, r.min.y, x + stringwidth(c->w->sysfont, c->items[i]) + 2*Tabpad, r.max.y);
}

static void
drawtabbar(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Rectangle t;
	int i, sel;

	w = c->w;
	sel = -1;
	draw(w->img, Rect(r.min.x, r.max.y-1, r.max.x, r.max.y), col(w, UXink), nil, ZP);
	for(i = 0; i < c->nitems; i++){
		t = tabrect(c, r, i);
		if(i == c->check){
			sel = i;
			continue;
		}
		t.min.y += 3;
		draw(w->img, t, col(w, UXcolface), nil, ZP);
		border(w->img, t, 1, col(w, UXink), ZP);
		string(w->img, Pt(t.min.x+Tabpad, t.min.y + (Dy(t)-w->sysfont->height)/2),
			col(w, ink(c)), ZP, w->sysfont, c->items[i]);
	}
	if(sel >= 0){
		/* the chosen tab sits in front, open into the panel below */
		t = tabrect(c, r, sel);
		draw(w->img, t, col(w, UXpaper), nil, ZP);
		draw(w->img, Rect(t.min.x, t.min.y, t.max.x, t.min.y+1), col(w, UXink), nil, ZP);
		draw(w->img, Rect(t.min.x, t.min.y, t.min.x+1, t.max.y), col(w, UXink), nil, ZP);
		draw(w->img, Rect(t.max.x-1, t.min.y, t.max.x, t.max.y), col(w, UXink), nil, ZP);
		string(w->img, Pt(t.min.x+Tabpad, t.min.y + (Dy(t)-w->sysfont->height)/2),
			col(w, ink(c)), ZP, w->sysfont, c->items[sel]);
	}
}

static int
tabmouse(Uxctl *c, Mouse *m)
{
	int i;
	Rectangle r;

	if(!(m->buttons & 1))
		return 0;
	r = _abs(c);
	for(i = 0; i < c->nitems; i++)
		if(ptinrect(m->xy, tabrect(c, r, i)))
			break;
	if(i < c->nitems && i != c->check){
		c->check = i;
		c->w->dirty = 1;
		note(c, UXvalue);
	}
	return 0;
}

void
_drawwidget(Uxctl *c, Rectangle r)
{
	if(c->t == &_popuptype)
		drawpopup(c, r);
	else if(c->t == &_radiotype)
		drawradios(c, r);
	else if(c->t == &_slidertype)
		drawslider(c, r);
	else if(c->t == &_switchtype)
		drawswitch(c, r);
	else if(c->t == &_tabbartype)
		drawtabbar(c, r);
}

/*
 * A pop-up menu over a window: it takes the mouse until it's done. A
 * release over an item chooses it; the release of the press that opened
 * it leaves it open, so it works as press-drag-release and as two clicks.
 */
struct Uxpop {
	char	**items;
	int	n;
	Rectangle	r;
	int	hover;
	int	first;	/* the opening press is still down */
	void	(*fn)(Uxwin*, int, void*);
	void	*arg;
};

static int
isseparator(char *s)
{
	return strcmp(s, "-") == 0;
}

void
_popopen(Uxwin *w, Point at, char **items, int n, int cur, void (*fn)(Uxwin*, int, void*), void *arg)
{
	Uxpop *p;
	int i, wd, x;
	Rectangle wr;

	_popclose(w, -1);
	p = emalloc(sizeof *p);
	memset(p, 0, sizeof *p);
	p->items = emalloc((n+1)*sizeof(char*));
	wd = 0;
	for(i = 0; i < n; i++){
		p->items[i] = estrdup(items[i]);
		if((x = stringwidth(w->sysfont, items[i])) > wd)
			wd = x;
	}
	p->n = n;
	p->hover = cur;
	p->first = w->lastm.buttons != 0;
	p->fn = fn;
	p->arg = arg;
	wd += UXmenupadleft + UXmenupadright;
	p->r = Rect(at.x, at.y, at.x+wd, at.y+n*UXmenuitemheight+2);
	/* keep it inside the window */
	if(w->img != nil){
		wr = insetrect(w->img->r, 2);
		if(p->r.max.y > wr.max.y)
			p->r = rectaddpt(p->r, Pt(0, wr.max.y - p->r.max.y));
		if(p->r.max.x > wr.max.x)
			p->r = rectaddpt(p->r, Pt(wr.max.x - p->r.max.x, 0));
	}
	w->pop = p;
	w->dirty = 1;
}

/* close the window's pop-up, telling its owner the item chosen (-1: none) */
void
_popclose(Uxwin *w, int i)
{
	Uxpop *p;
	int j;

	p = w->pop;
	if(p == nil)
		return;
	w->pop = nil;
	w->dirty = 1;
	if(p->fn != nil)
		p->fn(w, i, p->arg);
	for(j = 0; j < p->n; j++)
		free(p->items[j]);
	free(p->items);
	free(p);
}

static int
popitem(Uxpop *p, Point pt)
{
	int i;

	if(!ptinrect(pt, insetrect(p->r, 1)))
		return -1;
	i = (pt.y - p->r.min.y - 1) / UXmenuitemheight;
	if(i < 0 || i >= p->n || isseparator(p->items[i]))
		return -1;
	return i;
}

void
_popmouse(Uxwin *w, Mouse *m, int down)
{
	Uxpop *p;
	int i;

	p = w->pop;
	i = popitem(p, m->xy);
	if(i != p->hover){
		p->hover = i;
		w->dirty = 1;
	}
	if(m->buttons & 7){
		if(down && i < 0 && !p->first)
			_popclose(w, -1);	/* a press outside: no choice */
		return;
	}
	/* a release */
	if(i >= 0)
		_popclose(w, i);
	else if(p->first)
		p->first = 0;
	else
		_popclose(w, -1);
}

void
_drawpop(Uxwin *w)
{
	Uxpop *p;
	Rectangle r, ir;
	int i, y;

	p = w->pop;
	if(p == nil || w->img == nil)
		return;
	r = p->r;
	draw(w->img, rectaddpt(r, Pt(UXmenushadow, UXmenushadow)), col(w, UXink), nil, ZP);
	draw(w->img, r, col(w, UXpaper), nil, ZP);
	border(w->img, r, 1, col(w, UXink), ZP);
	for(i = 0; i < p->n; i++){
		ir = Rect(r.min.x+1, r.min.y+1+i*UXmenuitemheight, r.max.x-1, r.min.y+1+(i+1)*UXmenuitemheight);
		if(isseparator(p->items[i])){
			y = ir.min.y + Dy(ir)/2;
			draw(w->img, Rect(ir.min.x, y, ir.max.x, y+1), col(w, UXcoldisabled), nil, ZP);
			continue;
		}
		if(i == p->hover)
			draw(w->img, ir, col(w, UXcolselect), nil, ZP);
		string(w->img, Pt(ir.min.x+UXmenupadleft, ir.min.y + (Dy(ir)-w->sysfont->height)/2),
			col(w, i == p->hover ? UXcolselecttext : UXink), ZP, w->sysfont, p->items[i]);
	}
}

/*
 * The API.
 */
static Uxctl*
mkwidget(Uxwin *w, char *id, Ctltype *t, char *label)
{
	Uxctl *c;

	qlock(&uxlk);
	c = _newctl(w, id, t, label != nil ? label : "", ZR);
	c->check = -1;
	qunlock(&uxlk);
	return c;
}

Uxctl*
uxpopup(Uxwin *w, char *id, char *placeholder)
{
	return mkwidget(w, id, &_popuptype, placeholder);
}

Uxctl*
uxradios(Uxwin *w, char *id, int row)
{
	Uxctl *c;

	c = mkwidget(w, id, &_radiotype, "");
	c->row = row;
	return c;
}

Uxctl*
uxslider(Uxwin *w, char *id, int min, int max, int step)
{
	Uxctl *c;

	c = mkwidget(w, id, &_slidertype, "");
	uxsetrange(c, min, max, step);
	return c;
}

Uxctl*
uxswitch(Uxwin *w, char *id, char *label)
{
	Uxctl *c;

	c = mkwidget(w, id, &_switchtype, label);
	c->check = 0;
	return c;
}

Uxctl*
uxtabbar(Uxwin *w, char *id)
{
	return mkwidget(w, id, &_tabbartype, "");
}

void
uxsetrange(Uxctl *c, int min, int max, int step)
{
	qlock(&uxlk);
	c->vmin = min;
	c->vmax = max > min ? max : min;
	c->vstep = step > 0 ? step : 1;
	if(c->val < c->vmin)
		c->val = c->vmin;
	if(c->val > c->vmax)
		c->val = c->vmax;
	c->w->dirty = 1;
	qunlock(&uxlk);
	_kickui();
}

/* the chosen item (-1: none), a slider's value, or a switch's 0 or 1 */
int
uxchoice(Uxctl *c)
{
	int v;

	qlock(&uxlk);
	v = c->t == &_slidertype ? c->val : c->check;
	qunlock(&uxlk);
	return v;
}

void
uxsetchoice(Uxctl *c, int v)
{
	qlock(&uxlk);
	if(c->t == &_slidertype){
		if(v < c->vmin)
			v = c->vmin;
		if(v > c->vmax)
			v = c->vmax;
		c->val = v;
	}else
		c->check = v;
	c->w->dirty = 1;
	qunlock(&uxlk);
	_kickui();
}

typedef struct Menupick Menupick;
struct Menupick {
	Uxfn	*fn;
	void	*arg;
};

static void
menuchose(Uxwin *w, int i, void *arg)
{
	Menupick *mp;
	Uxev e;

	mp = arg;
	if(i >= 0){
		memset(&e, 0, sizeof e);
		e.type = UXaction;
		e.via = UXui;
		e.win = w;
		e.n = i;
		_notify(mp->fn, mp->arg, &e);
	}
	free(mp);
}

/* a context menu at pt (window-local); fn gets UXaction with Uxev.n the item */
void
uxpopmenu(Uxwin *w, Point pt, char **items, int n, Uxfn *fn, void *arg)
{
	Menupick *mp;

	mp = emalloc(sizeof *mp);
	mp->fn = fn;
	mp->arg = arg;
	qlock(&uxlk);
	if(w->img != nil)
		pt = addpt(pt, w->img->r.min);
	_popopen(w, pt, items, n, -1, menuchose, mp);
	qunlock(&uxlk);
	_kickui();
}

/*
 * canvas: a retained display list, uitree's ops (kind id geometry
 * attrs), painted in node space. Pointer events name the item under the
 * pointer: UXpointer with Uxev.n 0 down, 1 up, 2 drag, 3 move.
 */
static int	canvasmouse(Uxctl*, Mouse*);
Ctltype _canvastype = { "canvas", nofiles, noverbs, nil, nil, nil, canvasmouse, nil, 0 };

typedef struct Op Op;
struct Op {
	char	*f[64];
	int	n;
	char	*buf;
};

static int
opsplit(char *ops, Op **op)
{
	char *p, *nl, *s;
	int n;
	Op *o;

	n = 0;
	*op = nil;
	s = estrdup(ops != nil ? ops : "");
	for(p = s; p != nil && *p; p = nl){
		nl = strchr(p, '\n');
		if(nl != nil)
			*nl++ = 0;
		*op = erealloc(*op, (n+1)*sizeof(Op));
		o = &(*op)[n];
		o->buf = estrdup(p);
		o->n = tokenize(o->buf, o->f, nelem(o->f));
		if(o->n < 2){
			free(o->buf);
			continue;
		}
		n++;
	}
	free(s);
	return n;
}

static void
opfree(Op *op, int n)
{
	int i;

	for(i = 0; i < n; i++)
		free(op[i].buf);
	free(op);
}

static char*
opattr(Op *o, char *name)
{
	int i, l;

	l = strlen(name);
	for(i = 2; i < o->n; i++)
		if(strncmp(o->f[i], name, l) == 0 && o->f[i][l] == '=')
			return o->f[i]+l+1;
	return nil;
}

/* the op's geometry: its numbers after kind and id, x,y pairs split */
static int
opnums(Op *o, double *v, int max)
{
	int i, n;
	char *p, *e;

	n = 0;
	for(i = 2; i < o->n && n < max; i++){
		if(strchr(o->f[i], '=') != nil)
			continue;
		p = o->f[i];
		if(*p == 'M' || *p == 'L')
			p++;
		while(*p && n < max){
			v[n] = strtod(p, &e);
			if(e == p)
				break;
			n++;
			p = e;
			if(*p == ',')
				p++;
		}
	}
	return n;
}

static Image*
opcolor(Uxwin *w, char *s, ulong dflt, int *none)
{
	ulong rgba;

	*none = 0;
	if(s == nil){
		if(dflt == 0){
			*none = 1;
			return nil;
		}
		return col(w, dflt);
	}
	if(strcmp(s, "none") == 0 || _parsecolor(s, &rgba) < 0){
		*none = 1;
		return nil;
	}
	return col(w, rgba);
}

static int
rnd(double d)
{
	return d < 0 ? (int)(d - 0.5) : (int)(d + 0.5);
}

/* an item's bounds in node space, for hits */
static Rectangle
opbounds(Op *o)
{
	double v[64];
	int n, i;
	Rectangle r;
	char *k;

	n = opnums(o, v, nelem(v));
	k = o->f[0];
	if(strcmp(k, "rect") == 0 || strcmp(k, "image") == 0){
		if(n >= 4)
			return Rect(rnd(v[0]), rnd(v[1]), rnd(v[0]+v[2]), rnd(v[1]+v[3]));
	}else if(strcmp(k, "circle") == 0){
		if(n >= 3)
			return Rect(rnd(v[0]-v[2]), rnd(v[1]-v[2]), rnd(v[0]+v[2])+1, rnd(v[1]+v[2])+1);
	}else if(strcmp(k, "ellipse") == 0){
		if(n >= 4)
			return Rect(rnd(v[0]-v[2]), rnd(v[1]-v[3]), rnd(v[0]+v[2])+1, rnd(v[1]+v[3])+1);
	}else if(strcmp(k, "text") == 0){
		if(n >= 2)
			return Rect(rnd(v[0]), rnd(v[1])-12, rnd(v[0])+120, rnd(v[1])+4);
	}else if(n >= 2){
		r = Rect(rnd(v[0]), rnd(v[1]), rnd(v[0])+1, rnd(v[1])+1);
		for(i = 2; i+1 < n; i += 2)
			combinerect(&r, Rect(rnd(v[i]), rnd(v[i+1]), rnd(v[i])+1, rnd(v[i+1])+1));
		return insetrect(r, -3);
	}
	return ZR;
}

static void
paintop(Uxwin *w, Op *o, Point org)
{
	double v[64];
	Point pts[32];
	Image *fill, *stroke;
	int n, i, np, fnone, snone, wd, rad;
	char *k, *s;
	Rectangle r;
	Font *f;

	n = opnums(o, v, nelem(v));
	k = o->f[0];
	fill = opcolor(w, opattr(o, "fill"), 0, &fnone);
	stroke = opcolor(w, opattr(o, "stroke") != nil ? opattr(o, "stroke") : opattr(o, "border"), 0, &snone);
	wd = opattr(o, "width") != nil ? rnd(atof(opattr(o, "width"))) : 1;
	if(wd < 1)
		wd = 1;
	if(strcmp(k, "rect") == 0 && n >= 4){
		r = rectaddpt(Rect(rnd(v[0]), rnd(v[1]), rnd(v[0]+v[2]), rnd(v[1]+v[3])), org);
		rad = opattr(o, "radius") != nil ? atoi(opattr(o, "radius")) : 0;
		if(!fnone){
			if(rad > 0){
				draw(w->img, Rect(r.min.x+rad, r.min.y, r.max.x-rad, r.max.y), fill, nil, ZP);
				draw(w->img, Rect(r.min.x, r.min.y+rad, r.max.x, r.max.y-rad), fill, nil, ZP);
				fillellipse(w->img, Pt(r.min.x+rad, r.min.y+rad), rad, rad, fill, ZP);
				fillellipse(w->img, Pt(r.max.x-1-rad, r.min.y+rad), rad, rad, fill, ZP);
				fillellipse(w->img, Pt(r.min.x+rad, r.max.y-1-rad), rad, rad, fill, ZP);
				fillellipse(w->img, Pt(r.max.x-1-rad, r.max.y-1-rad), rad, rad, fill, ZP);
			}else
				draw(w->img, r, fill, nil, ZP);
		}
		if(!snone)
			border(w->img, r, wd, stroke, ZP);
	}else if((strcmp(k, "circle") == 0 && n >= 3) || (strcmp(k, "ellipse") == 0 && n >= 4)){
		Point c;
		int rx, ry;

		c = addpt(Pt(rnd(v[0]), rnd(v[1])), org);
		rx = rnd(v[2]);
		ry = strcmp(k, "circle") == 0 ? rx : rnd(v[3]);
		if(!fnone)
			fillellipse(w->img, c, rx, ry, fill, ZP);
		if(!snone)
			ellipse(w->img, c, rx, ry, wd-1, stroke, ZP);
	}else if(strcmp(k, "text") == 0 && n >= 2){
		s = nil;
		for(i = 2; i < o->n; i++)
			if(strchr(o->f[i], '=') == nil && strtod(o->f[i], nil) == 0 && !isdigit(o->f[i][0]) && o->f[i][0] != '-' && o->f[i][0] != '.'){
				s = o->f[i];
				break;
			}
		if(s != nil){
			f = opattr(o, "size") != nil && atoi(opattr(o, "size")) >= 14 ? w->sysfont : w->font;
			stroke = opcolor(w, opattr(o, "color"), UXink, &snone);
			if(!snone)
				string(w->img, addpt(Pt(rnd(v[0]), rnd(v[1]) - f->ascent), org), stroke, ZP, f, s);
		}
	}else if(strcmp(k, "image") == 0 && n >= 4){
		r = rectaddpt(Rect(rnd(v[0]), rnd(v[1]), rnd(v[0]+v[2]), rnd(v[1]+v[3])), org);
		draw(w->img, r, col(w, UXcoltrack), nil, ZP);
		border(w->img, r, 1, col(w, UXcoldisabled), ZP);
	}else if(n >= 4){
		/* line, path, polyline, polygon */
		np = 0;
		for(i = 0; i+1 < n && np < nelem(pts); i += 2)
			pts[np++] = addpt(Pt(rnd(v[i]), rnd(v[i+1])), org);
		if(strcmp(k, "polygon") == 0 && !fnone)
			fillpoly(w->img, pts, np, 0, fill, ZP);
		if(snone && strcmp(k, "polygon") != 0)
			stroke = col(w, UXink), snone = 0;
		if(!snone)
			poly(w->img, pts, np, Endsquare, Endsquare, wd > 1 ? wd/2 : 0, stroke, ZP);
		if(strcmp(k, "polygon") == 0 && !snone && np > 2)
			line(w->img, pts[np-1], pts[0], Endsquare, Endsquare, wd > 1 ? wd/2 : 0, stroke, ZP);
	}
}

static void
drawcanvas(Uxctl *c, Rectangle r)
{
	Op *op;
	int i, n;

	n = opsplit(c->ops, &op);
	replclipr(c->w->img, 0, rectclip(&r, c->w->clip) ? r : c->w->clip);
	for(i = 0; i < n; i++)
		paintop(c->w, &op[i], r.min);
	replclipr(c->w->img, 0, c->w->clip);
	opfree(op, n);
}

/* the topmost item at p, node space: malloc'd id, or nil */
static char*
canvashit(Uxctl *c, Point p)
{
	Op *op;
	int i, n;
	char *id;

	id = nil;
	n = opsplit(c->ops, &op);
	for(i = n-1; i >= 0; i--)
		if(ptinrect(p, opbounds(&op[i]))){
			id = estrdup(op[i].f[1]);
			break;
		}
	opfree(op, n);
	return id;
}

static void
pointer(Uxctl *c, int kind, Mouse *m)
{
	Uxev e;
	Point p;

	p = subpt(m->xy, _abs(c).min);
	memset(&e, 0, sizeof e);
	e.type = UXpointer;
	e.via = UXui;
	e.win = c->w;
	e.ctl = c;
	e.n = kind;
	e.pt = p;
	e.item = canvashit(c, p);
	e.text = (m->buttons & 4) || c->pressed == 4 ? "right" : (m->buttons & 2) || c->pressed == 2 ? "middle" : "left";
	_notify(c->fn, c->arg, &e);
	free(e.item);
}

static int
canvasmouse(Uxctl *c, Mouse *m)
{
	if(m->buttons & 7){
		if(!c->pressed){
			c->pressed = m->buttons & 7;
			pointer(c, 0, m);
		}else
			pointer(c, 2, m);
		return 1;
	}
	if(c->pressed){
		pointer(c, 1, m);
		c->pressed = 0;
		return 0;
	}
	pointer(c, 3, m);
	return 0;
}

int
_iscanvas(Uxctl *c)
{
	return c->t == &_canvastype;
}

void
_drawcanvas(Uxctl *c, Rectangle r)
{
	drawcanvas(c, r);
}

/* a move with no button over a canvas */
void
_canvasmove(Uxctl *c, Mouse *m)
{
	pointer(c, 3, m);
}

Uxctl*
uxcanvas(Uxwin *w, char *id)
{
	return mkwidget(w, id, &_canvastype, "");
}

void
uxsetops(Uxctl *c, char *ops)
{
	qlock(&uxlk);
	free(c->ops);
	c->ops = estrdup(ops != nil ? ops : "");
	c->w->dirty = 1;
	qunlock(&uxlk);
	_kickui();
}
