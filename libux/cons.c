/*
 * The console control (ux.md, console): a shell window's text. What
 * the app writes is output, kept before the output point (qh); what a
 * person types goes after it, and is sent, a line at a time, when it
 * ends in a newline. Text is drawn in the body font, word-wrapped to
 * the control's width. Tabs stop every $tabstop (4 if unset) widths
 * of a 0, as in rio and mc(1).
 *
 * Inside a scroll view the console fits the view's width, grows with
 * its text, sets the view's extent, and keeps the end in view while
 * it's shown, so the view's scroll bar is the console's.
 *
 * Everything here runs with uxlk held, except the ux* functions, which
 * take it.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <mouse.h>
#include <keyboard.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "dx.h"
#include "ux.h"
#include "impl.h"

enum {
	Pad	= 4,	/* text inset, left and right */
	Padtop	= 3,
	Maxcons	= 256*1024,	/* runes kept; the oldest go first */
	Trim	= 32*1024,	/* how many go at a time */
};

static void
kick(void)
{
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
}

static int
inscroll(Uxctl *c)
{
	return c->parent != nil && strcmp(c->parent->t->name, "scroll-view") == 0;
}

static int
active(Uxwin *w)
{
	return ux.front == nil || ux.front == w;
}

static Font*
consfont(Uxctl *c)
{
	return c->cfont != nil ? c->cfont : c->w->font;
}

static void
dirty(Uxctl *c, int from)
{
	if(from < c->lfrom)
		c->lfrom = from;
	c->w->dirty = 1;
}

static void
note(Uxctl *c, int type, int via, char *text)
{
	Uxev e;

	memset(&e, 0, sizeof e);
	e.type = type;
	e.via = via;
	e.win = c->w;
	e.ctl = c;
	e.text = text;
	_notify(c->fn, c->arg, &e);
}


/* replace runes q0..q1 with n runes r; offsets after q1 move with the text */
static void
replace(Uxctl *c, int q0, int q1, Rune *r, int n)
{
	int d;

	d = n - (q1-q0);
	if(d > 0)
		c->text = erealloc(c->text, (c->ntext + d + 1)*sizeof(Rune));
	memmove(c->text+q0+n, c->text+q1, (c->ntext-q1)*sizeof(Rune));
	memmove(c->text+q0, r, n*sizeof(Rune));
	c->ntext += d;
#define Move(q) if(q >= q1) q += d; else if(q > q0) q = q0
	Move(c->qh);
	Move(c->q0);
	Move(c->q1);
	Move(c->qa);
#undef Move
	dirty(c, q0);
}

/* drop the oldest output, a whole line at a time, past Maxcons */
static void
trim(Uxctl *c)
{
	int n;

	if(c->ntext <= Maxcons)
		return;
	n = c->ntext - Maxcons + Trim;
	if(n > c->qh)
		n = c->qh;
	while(n < c->qh && c->text[n-1] != '\n')
		n++;
	if(n <= 0)
		return;
	replace(c, 0, n, nil, 0);
	c->lfrom = 0;
	c->nline = 0;
}

/* output: s goes in before the output point; a backspace takes back what's before it */
static void
output(Uxctl *c, char *s, long n)
{
	Rune *r;
	int nr, k;
	char *e;

	r = emalloc((n+1)*sizeof(Rune));
	nr = 0;
	for(e = s+n; s < e; ){
		if(!fullrune(s, e-s)){
			/* a rune cut short by the writer: show what's there */
			r[nr++] = Runeerror;
			break;
		}
		s += chartorune(&r[nr], s);
		switch(r[nr]){
		case 0:
		case '\r':
			continue;
		case '\b':
			if(nr > 0)
				nr--;
			else if(c->qh > 0 && c->text[c->qh-1] != '\n')
				replace(c, c->qh-1, c->qh, nil, 0);
			continue;
		}
		nr++;
	}
	if(nr > 0){
		/* a caret at the output point (or in what's typed) moves on with it */
		k = c->qh;
		replace(c, k, k, r, nr);
		c->qh = k + nr;
	}
	free(r);
	trim(c);
}

/* send what's typed, up to and including its last newline (all of it if eof) */
static void
sendinput(Uxctl *c, int eof, int via)
{
	char *s, num[16];
	int q;

	q = c->ntext;
	if(!eof){
		while(q > c->qh && c->text[q-1] != '\n')
			q--;
		if(q == c->qh)
			return;
	}
	s = _runestr(c->text+c->qh, q-c->qh);
	c->qh = q;
	snprint(num, sizeof num, "%ld", strlen(s));
	note(c, UXsubmit, via, s);
	free(s);
}

/* typed or pasted text: replaces the selection if it's in the input, else goes at the end */
static void
type(Uxctl *c, Rune *r, int n, int via)
{
	int qh;

	if(c->q0 < c->qh)
		c->q0 = c->q1 = c->ntext;
	qh = c->qh;
	replace(c, c->q0, c->q1, r, n);
	c->qh = qh;	/* typing at the output point is input, not output */
	c->q0 = c->q1;
	c->follow = 1;
	sendinput(c, 0, via);
}

static void
interrupt(Uxctl *c, int via)
{
	note(c, UXinterrupt, via, nil);
}

/* layout */

static int
tabwidth(Font *f)
{
	static int stop;
	char *s;
	int w;

	if(stop == 0){
		s = getenv("tabstop");
		stop = s != nil ? atoi(s) : 0;
		free(s);
		if(stop <= 0)
			stop = 4;
	}
	w = stop*stringwidth(f, "0");
	return w > 0 ? w : 8;
}

static int
runew(Font *f, Rune r, int x, int tabw)
{
	if(r == '\t')
		return tabw - x%tabw;
	return runestringnwidth(f, &r, 1);
}

/* the text's width inside the control */
static int
textwidth(Uxctl *c)
{
	int w;

	w = Dx(c->r) - 2*Pad;
	return w > 1 ? w : 1;
}

static void
addline(Uxctl *c, int q)
{
	if(c->nline % 64 == 0)
		c->line = erealloc(c->line, (c->nline+64)*sizeof(int));
	c->line[c->nline++] = q;
}

/* the line rune q is on */
static int
lineof(Uxctl *c, int q)
{
	int lo, hi, m;

	lo = 0;
	hi = c->nline-1;
	while(lo < hi){
		m = (lo+hi+1)/2;
		if(c->line[m] <= q)
			lo = m;
		else
			hi = m-1;
	}
	return lo;
}

/* where lines start, from the line before the one holding rune from */
static void
wrap(Uxctl *c, int from)
{
	Font *f;
	int q, x, w, wd, tabw, sp, i, start, nl;
	Rune r;

	f = consfont(c);
	wd = textwidth(c);
	tabw = tabwidth(f);
	i = 0;
	if(c->nline > 0 && from > 0){
		i = lineof(c, from);
		if(i > 0)
			i--;
	}
	c->nline = i;
	q = i == 0 ? 0 : c->line[i];
	for(;;){
		start = q;
		addline(c, start);
		x = 0;
		sp = -1;
		nl = 0;
		while(q < c->ntext){
			r = c->text[q];
			if(r == '\n'){
				q++;
				nl = 1;
				break;
			}
			w = runew(f, r, x, tabw);
			if(x+w > wd && q > start){
				if(sp >= 0)
					q = sp+1;
				break;
			}
			if(r == ' ' || r == '\t')
				sp = q;
			x += w;
			q++;
		}
		if(q >= c->ntext){
			if(nl)
				addline(c, q);
			break;
		}
	}
	c->lfrom = c->ntext+1;
	c->lw = wd;
}

static int
height(Uxctl *c)
{
	return 2*Padtop + c->nline*consfont(c)->height;
}

/*
 * Before drawing: wrap what changed, and in a scroll view, fit its
 * width, grow with the text and set the view's extent, keeping the end
 * in view if it was (or if something was typed).
 */
void
_conslayout(Uxctl *c)
{
	Uxctl *v;
	Rectangle port, vb, hb, gb, r, ext;
	int atend, h;
	char buf[64];

	if(c->w->img == nil || consfont(c) == nil)
		return;
	v = inscroll(c) ? c->parent : nil;
	atend = 0;
	if(v != nil){
		_scrollparts(v, v->r, &port, &vb, &hb, &gb);
		atend = c->follow || v->origin.y + Dy(port) >= v->extent.max.y;
		r = Rect(port.min.x, port.min.y, port.max.x, port.min.y + Dy(c->r));
		if(Dx(r) != Dx(c->r))
			c->nline = 0;
		c->r = r;
	}
	if(c->nline == 0 || textwidth(c) != c->lw)
		wrap(c, 0);
	else if(c->lfrom <= c->ntext)
		wrap(c, c->lfrom);
	c->follow = 0;
	if(v == nil)
		return;
	h = height(c);
	if(h < Dy(port))
		h = Dy(port);
	if(Dy(c->r) != h){
		c->r.max.y = c->r.min.y + h;
		snprint(buf, sizeof buf, "%d,%d,%d,%d", c->r.min.x, c->r.min.y, c->r.max.x, c->r.max.y);
	}
	ext = Rect(0, 0, Dx(port), h);
	if(!eqrect(ext, v->extent)){
		v->extent = ext;
		snprint(buf, sizeof buf, "%d,%d,%d,%d", ext.min.x, ext.min.y, ext.max.x, ext.max.y);
	}
	if(atend)
		_setorigin(v, Pt(v->origin.x, ext.max.y - Dy(port)), UXui);
	else
		_clamporigin(v, UXui);
}

/* drawing */

static void
drawrun(Uxwin *w, Font *f, Point p, Rune *r, int n, Image *fg, Image *bg)
{
	if(n > 0)
		runestringnbg(w->img, p, fg, ZP, f, r, n, bg, ZP);
}

void
_drawcons(Uxctl *c, Rectangle r)
{
	Uxwin *w;
	Font *f;
	Rectangle vis;
	Image *fg, *bg, *selbg, *b;
	Point p;
	int i, q, e, x, x0, ht, tabw, focused, run, rw, sel;

	w = c->w;
	f = consfont(c);
	vis = _vis(c);
	draw(w->img, vis, _color(w, UXcolwindow), nil, ZP);
	if(f == nil || c->nline == 0)
		return;
	ht = f->height;
	tabw = tabwidth(f);
	focused = w->focus == c && active(w);
	fg = _color(w, (c->state & Cenabled) ? UXcoltext : UXcoldisabled);
	bg = _color(w, UXcolwindow);
	selbg = _color(w, focused ? UXcoltextselect : UXcolface);
	x0 = r.min.x + Pad;
	i = (vis.min.y - (r.min.y + Padtop)) / ht;
	if(i < 0)
		i = 0;
	for(; i < c->nline; i++){
		p = Pt(x0, r.min.y + Padtop + i*ht);
		if(p.y >= vis.max.y)
			break;
		q = c->line[i];
		e = i+1 < c->nline ? c->line[i+1] : c->ntext;
		x = 0;
		/* runs of runes alike in selection, ending at tabs and newlines */
		while(q < e){
			sel = q >= c->q0 && q < c->q1;
			b = sel ? selbg : bg;
			if(c->text[q] == '\t' || c->text[q] == '\n'){
				rw = c->text[q] == '\t' ? runew(f, '\t', x, tabw) : (sel ? stringwidth(f, " ") : 0);
				if(sel && rw > 0)
					draw(w->img, Rect(x0+x, p.y, x0+x+rw, p.y+ht), b, nil, ZP);
				x += rw;
				q++;
				continue;
			}
			for(run = 1; q+run < e; run++){
				if(c->text[q+run] == '\t' || c->text[q+run] == '\n')
					break;
				if((q+run >= c->q0 && q+run < c->q1) != sel)
					break;
			}
			drawrun(w, f, Pt(x0+x, p.y), c->text+q, run, fg, b);
			x += runestringnwidth(f, c->text+q, run);
			q += run;
		}
	}
	/* the caret: a line at the insertion point */
	if(focused && c->q0 == c->q1){
		i = lineof(c, c->q0);
		/* at the end of a wrapped line, the caret goes at the next line's start */
		q = c->line[i];
		x = 0;
		for(; q < c->q0; q++)
			x += c->text[q] == '\n' ? 0 : runew(f, c->text[q], x, tabw);
		p = Pt(x0+x, r.min.y + Padtop + i*ht);
		draw(w->img, Rect(p.x, p.y, p.x+1, p.y+ht), _color(w, UXcoltext), nil, ZP);
	}
}

/* the rune nearest point p (screen coordinates) */
static int
runeat(Uxctl *c, Point p)
{
	Font *f;
	Rectangle r;
	int i, q, e, x, w, tabw;

	f = consfont(c);
	if(f == nil || c->nline == 0)
		return c->ntext;
	r = _abs(c);
	i = (p.y - (r.min.y + Padtop)) / f->height;
	if(p.y < r.min.y + Padtop)
		i = 0;
	if(i >= c->nline)
		return c->ntext;
	tabw = tabwidth(f);
	q = c->line[i];
	e = i+1 < c->nline ? c->line[i+1] : c->ntext;
	x = r.min.x + Pad;
	for(; q < e; q++){
		if(c->text[q] == '\n')
			break;
		w = runew(f, c->text[q], x - (r.min.x+Pad), tabw);
		if(p.x < x + w/2)
			break;
		x += w;
	}
	/* a wrapped line's last rune is on it; past it is the next line's first */
	if(q == e && i+1 < c->nline && q > c->line[i])
		q--;
	return q;
}

/* input */

static int
isword(Rune r)
{
	return r == '_' || isalpharune(r) || (r >= '0' && r <= '9') || r >= 0x80;
}

static int
consmouse(Uxctl *c, Mouse *m)
{
	int q, a, b;

	if(m->buttons & (8|16)){
		if(inscroll(c))
			return c->parent->t->mouse(c->parent, m);
		return 0;
	}
	q = runeat(c, m->xy);
	if(!c->pressed){
		if(!(m->buttons & 1))
			return 0;
		c->pressed = 1;
		if(c->lastclick != 0 && m->msec - c->lastclick < 500 && c->lastitem == q){
			/* double click: the word, or the line */
			a = b = q;
			if(q < c->ntext && isword(c->text[q])){
				while(a > 0 && isword(c->text[a-1]))
					a--;
				while(b < c->ntext && isword(c->text[b]))
					b++;
			}else{
				while(a > 0 && c->text[a-1] != '\n')
					a--;
				while(b < c->ntext && c->text[b] != '\n')
					b++;
			}
			c->q0 = a;
			c->q1 = b;
			c->qa = a;
			c->lastclick = 0;
		}else{
			c->q0 = c->q1 = c->qa = q;
			c->lastclick = m->msec;
			c->lastitem = q;
		}
		c->w->dirty = 1;
		return 1;
	}
	if(m->buttons & 1){
		a = c->qa < q ? c->qa : q;
		b = c->qa < q ? q : c->qa;
		if(a != c->q0 || b != c->q1){
			c->q0 = a;
			c->q1 = b;
			c->w->dirty = 1;
		}
		return 1;
	}
	c->pressed = 0;
	return 0;
}

/* scroll the view by d pixels, or to the top (-1) or end (1) */
static void
scrollby(Uxctl *c, int d, int to)
{
	Uxctl *v;

	if(!inscroll(c))
		return;
	v = c->parent;
	if(to < 0)
		_setorigin(v, Pt(v->origin.x, v->extent.min.y), UXui);
	else if(to > 0)
		_setorigin(v, Pt(v->origin.x, v->extent.max.y), UXui);
	else
		_setorigin(v, Pt(v->origin.x, v->origin.y + d), UXui);
}

static int
conskey(Uxctl *c, Rune r)
{
	Font *f;
	Rectangle port, vb, hb, gb;
	char buf[UTFmax+1];
	int n, ht, page;

	if(r == Kdel){
		interrupt(c, UXui);
		return 1;
	}
	if(c->raw && !(c->state & UXreadonly)){
		n = runetochar(buf, &r);
		buf[n] = 0;
		note(c, UXsubmit, UXui, buf);
		return 1;
	}
	f = consfont(c);
	ht = f != nil ? f->height : 13;
	page = Dy(c->r);
	if(inscroll(c)){
		_scrollparts(c->parent, c->parent->r, &port, &vb, &hb, &gb);
		page = Dy(port) - ht;
	}
	switch(r){
	case Kup:
		scrollby(c, -ht, 0);
		return 1;
	case Kdown:
		scrollby(c, ht, 0);
		return 1;
	case Kpgup:
		scrollby(c, -page, 0);
		return 1;
	case Kpgdown:
		scrollby(c, page, 0);
		return 1;
	case Khome:
		scrollby(c, 0, -1);
		return 1;
	case Kend:
		scrollby(c, 0, 1);
		return 1;
	case Kleft:
		if(c->q0 > 0)
			c->q0--;
		c->q1 = c->q0;
		c->w->dirty = 1;
		return 1;
	case Kright:
		if(c->q1 < c->ntext)
			c->q1++;
		c->q0 = c->q1;
		c->w->dirty = 1;
		return 1;
	case Kesc:
		return 1;
	}
	if(c->state & UXreadonly)
		return 0;
	if(r == 0x04){	/* ^D: send what's typed with no newline; nothing typed is end of file */
		c->q0 = c->q1 = c->ntext;
		sendinput(c, 1, UXui);
		c->follow = 1;
		c->w->dirty = 1;
		return 1;
	}
	if(r == Kbs || r == 0x15 || r == 0x17){	/* backspace, ^U, ^W: only what's typed */
		if(c->q0 < c->qh){
			c->q0 = c->q1 = c->ntext;
		}
		if(c->q0 == c->q1){
			if(r == Kbs){
				if(c->q0 > c->qh)
					c->q0--;
			}else if(r == 0x15)
				c->q0 = c->qh;
			else{
				while(c->q0 > c->qh && isspacerune(c->text[c->q0-1]))
					c->q0--;
				while(c->q0 > c->qh && !isspacerune(c->text[c->q0-1]))
					c->q0--;
			}
		}
		replace(c, c->q0, c->q1, nil, 0);
		c->follow = 1;
		return 1;
	}
	if(r == '\n'){
		/* Enter sends the whole line, wherever the caret is in it */
		c->q0 = c->q1 = c->ntext;
		type(c, &r, 1, UXui);
		return 1;
	}
	if(r == '\t' || (r >= ' ' && !(r >= KF && r < KF+0x1000))){
		type(c, &r, 1, UXui);
		return 1;
	}
	return 0;
}

/* Edit menu commands (uxedit) on a focused console */
char*
_consedit(Uxctl *c, char *cmd)
{
	char *s;
	Rune *r;
	int n;

	if(strcmp(cmd, "selectall") == 0){
		c->q0 = 0;
		c->q1 = c->ntext;
		c->w->dirty = 1;
		return nil;
	}
	if(strcmp(cmd, "copy") == 0 || strcmp(cmd, "cut") == 0){
		if(c->q1 > c->q0){
			s = _runestr(c->text+c->q0, c->q1-c->q0);
			n = _clipput(s);
			free(s);
			if(n < 0)
				return "failed: can't write the clipboard";
		}
		/* only what's typed can be cut; output is copied */
		if(cmd[1] == 'u' && c->q1 > c->q0 && c->q0 >= c->qh && !(c->state & UXreadonly)){
			replace(c, c->q0, c->q1, nil, 0);
		}
		return nil;
	}
	if(strcmp(cmd, "paste") == 0){
		if(c->state & UXreadonly)
			return "bad value: readonly";
		s = _clipget();
		if(s == nil)
			return "failed: can't read the clipboard";
		r = runesmprint("%s", s);
		free(s);
		type(c, r, runestrlen(r), UXui);
		free(r);
		return nil;
	}
	return "unknown command";
}

/* files and verbs */

static char*
consread(Uxctl *c, char *file)
{
	if(strcmp(file, "value") == 0)
		return _runestr(c->text, c->ntext);
	if(strcmp(file, "selection") == 0)
		return smprint("%d %d\n", c->q0, c->q1);
	if(strcmp(file, "input") == 0)
		return _runestr(c->text+c->qh, c->ntext-c->qh);
	return nil;
}

static char*
vinsert(Uxctl *c, int, char **argv)
{
	Rune *r;

	if(c->state & UXreadonly)
		return "bad value: readonly";
	r = runesmprint("%s", argv[1]);
	type(c, r, runestrlen(r), UXctl);
	free(r);
	return nil;
}

static char*
vinterrupt(Uxctl *c, int, char**)
{
	interrupt(c, UXctl);
	return nil;
}

static char*
vselect(Uxctl *c, int, char **argv)
{
	char *p;
	int q0, q1;

	q0 = strtol(argv[1], &p, 10);
	if(*argv[1] == 0 || *p != 0)
		return "bad syntax";
	q1 = strtol(argv[2], &p, 10);
	if(*argv[2] == 0 || *p != 0)
		return "bad syntax";
	if(q0 < 0 || q1 < q0 || q1 > c->ntext)
		return "bad value: selection out of range";
	c->q0 = q0;
	c->q1 = q1;
	c->w->dirty = 1;
	return nil;
}

static char*
vfocus(Uxctl *c, int, char**)
{
	_setfocus(c->w, c, UXctl);
	return nil;
}

/* drop the output but its last line, which is likely a prompt */
static char*
vclear(Uxctl *c, int, char**)
{
	int q;

	for(q = c->qh; q > 0 && c->text[q-1] != '\n'; q--)
		;
	replace(c, 0, q, nil, 0);
	c->nline = 0;
	c->follow = 1;
	return nil;
}

static char *consfiles[] = { "value", "selection", "input", nil };

static Verb consverbs[] = {
	"insert", Redit, 1, 1, vinsert,
	"interrupt", Ractivate, 0, 0, vinterrupt,
	"select", Ractivate, 2, 2, vselect,
	"focus", Ractivate, 0, 0, vfocus,
	"clear", Redit, 0, 0, vclear,
	nil,
};

Ctltype _constype = { "console", consfiles, consverbs, consread, nil, nil, consmouse, conskey, 1 };

/* the API */

Uxctl*
uxconsole(Uxwin *w, char *id, Rectangle r)
{
	Uxctl *c;

	qlock(&uxlk);
	c = _newctl(w, id, &_constype, "", r);
	c->lfrom = 0;
	c->follow = 1;
	qunlock(&uxlk);
	kick();
	return c;
}

void
uxconswrite(Uxctl *c, char *s, long n)
{
	if(n <= 0)
		return;
	qlock(&uxlk);
	if(!c->dead)
		output(c, s, n);
	qunlock(&uxlk);
	kick();
}

void
uxconsraw(Uxctl *c, int on)
{
	qlock(&uxlk);
	c->raw = on != 0;
	qunlock(&uxlk);
}

void
uxconsclear(Uxctl *c)
{
	qlock(&uxlk);
	if(!c->dead)
		vclear(c, 0, nil);
	qunlock(&uxlk);
	kick();
}

int
uxconsfont(Uxctl *c, char *name)
{
	Font *f;

	qlock(&uxlk);
	if(c->w->d == nil){
		qunlock(&uxlk);
		return 0;
	}
	f = openfont(c->w->d, name);
	if(f == nil){
		qunlock(&uxlk);
		return -1;
	}
	if(c->cfont != nil)
		freefont(c->cfont);
	c->cfont = f;
	c->nline = 0;
	c->follow = 1;
	c->w->dirty = 1;
	qunlock(&uxlk);
	kick();
	return 0;
}
