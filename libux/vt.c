/*
 * vt: an xterm-compatible terminal for the console control (vt.h).
 *
 * What's written is decoded as UTF-8 and run through a parser after
 * the DEC VT500 state diagram (ESC, CSI, OSC and DCS, with C0 controls
 * acted on wherever they come). Each action goes to the screen showing:
 * the primary screen is the owner's text (Vbuf), edited in place; the
 * alternate screen is a grid of cells.
 */
#ifndef VTHOST
#include <u.h>
#include <libc.h>
#include <keyboard.h>
#endif
#include "vt.h"

enum {
	Ground,
	Esc,
	Escint,
	Csi,
	Csiign,
	Str,
	Stresc,

	Maxstr	= 4096,
	Maxarg	= 65535,
	Tabw	= 8,
};

static void*
vmalloc(long n)
{
	void *p;

	p = malloc(n > 0 ? n : 1);
	if(p == nil){
#ifdef VTHOST
		abort();
#else
		sysfatal("vt: out of memory");
#endif
	}
	memset(p, 0, n > 0 ? n : 1);
	return p;
}

static void*
vrealloc(void *p, long n)
{
	p = realloc(p, n > 0 ? n : 1);
	if(p == nil){
#ifdef VTHOST
		abort();
#else
		sysfatal("vt: out of memory");
#endif
	}
	return p;
}

static void
reply(Vt *vt, char *s)
{
	if(vt->reply != nil)
		vt->reply(vt, s, strlen(s));
}

/* colours */

static u32int ansi[16] = {
	/* after Terminal.app's Basic: readable on a light or a dark window */
	0x000000, 0x990000, 0x00A600, 0x999900, 0x0000B2, 0xB200B2, 0x00A6B2, 0xBFBFBF,
	0x666666, 0xE50000, 0x00D900, 0xE5E500, 0x0000FF, 0xE500E5, 0x00E5E5, 0xE5E5E5,
};

/* a colour's 0xRRGGBB; def for the default */
u32int
vtrgb(u32int c, u32int def)
{
	int i, r, g, b;
	static int lv[6] = { 0, 95, 135, 175, 215, 255 };

	if(c == 0)
		return def;
	if((c & 0xFF000000) == VTrgb)
		return c & 0xFFFFFF;
	i = c & 0xFF;
	if(i < 16)
		return ansi[i];
	if(i < 232){
		i -= 16;
		r = lv[i/36];
		g = lv[(i/6)%6];
		b = lv[i%6];
		return r<<16 | g<<8 | b;
	}
	r = 8 + 10*(i-232);
	return r<<16 | r<<8 | r;
}

/* character widths, after wcwidth: 0 for combining marks, 2 for East Asian wide */
int
vtwcwidth(Rune r)
{
	static Rune zero[][2] = {
		{0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x0610, 0x061A},
		{0x064B, 0x065F}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
		{0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064}, {0x20D0, 0x20FF},
		{0xFE00, 0xFE0F}, {0xFE20, 0xFE2F}, {0xFEFF, 0xFEFF},
	};
	static Rune wide[][2] = {
		{0x1100, 0x115F}, {0x231A, 0x231B}, {0x2329, 0x232A}, {0x23E9, 0x23EC},
		{0x2614, 0x2615}, {0x2E80, 0x303E}, {0x3041, 0x33FF}, {0x3400, 0x4DBF},
		{0x4E00, 0x9FFF}, {0xA000, 0xA4CF}, {0xA960, 0xA97F}, {0xAC00, 0xD7A3},
		{0xF900, 0xFAFF}, {0xFE10, 0xFE19}, {0xFE30, 0xFE6F}, {0xFF00, 0xFF60},
		{0xFFE0, 0xFFE6}, {0x1F300, 0x1F64F}, {0x1F680, 0x1F6FF}, {0x1F900, 0x1F9FF},
		{0x20000, 0x2FFFD}, {0x30000, 0x3FFFD},
	};
	int i;

	if(r < 0x300)
		return 1;
	for(i = 0; i < nelem(zero); i++)
		if(r >= zero[i][0] && r <= zero[i][1])
			return 0;
	for(i = 0; i < nelem(wide); i++)
		if(r >= wide[i][0] && r <= wide[i][1])
			return 2;
	return 1;
}

/* DEC special graphics, 0x5F to 0x7E */
static Rune decgfx[32] = {
	0x00A0, 0x25C6, 0x2592, 0x2409, 0x240C, 0x240D, 0x240A, 0x00B0,
	0x00B1, 0x2424, 0x240B, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C,
	0x23BA, 0x23BB, 0x2500, 0x23BC, 0x23BD, 0x251C, 0x2524, 0x2534,
	0x252C, 0x2502, 0x2264, 0x2265, 0x03C0, 0x2260, 0x00A3, 0x00B7,
};

/*
 * The primary screen: the owner's text. Its rows are lines (ending
 * in newlines), and the screen is the last vt->rows of them.
 */

static void putr(Vt*, Rune);

static void
brepl(Vt *vt, int q0, int q1, Rune *r, Vattr *a, int n)
{
	vt->buf->replace(vt->buf, q0, q1, r, a, n);
}

static int
lstart(Vbuf *b, int q)
{
	while(q > 0 && b->text[q-1] != '\n')
		q--;
	return q;
}

static int
lend(Vbuf *b, int q)
{
	while(q < b->qh && b->text[q] != '\n')
		q++;
	return q;
}

static int
lcol(Vt *vt)
{
	return vt->buf->qc - lstart(vt->buf, vt->buf->qc);
}

static Rune*
runes(Rune r, int n)
{
	Rune *s;
	int i;

	s = vmalloc((n+1)*sizeof(Rune));
	for(i = 0; i < n; i++)
		s[i] = r;
	return s;
}

static void
lins(Vt *vt, int q, Rune r, int n)
{
	Rune *s;

	if(n <= 0)
		return;
	s = runes(r, n);
	brepl(vt, q, q, s, nil, n);
	free(s);
}

/* write what's waiting at the cursor, over what's there to the line's end, then after it */
static void
prflush(Vt *vt)
{
	Vbuf *b;
	int q, k, n;

	n = vt->npr;
	if(n == 0)
		return;
	vt->npr = 0;
	b = vt->buf;
	q = b->qc;
	k = 0;
	if(!vt->insert){
		k = lend(b, q) - q;
		if(k > n)
			k = n;
	}
	brepl(vt, q, q+k, vt->pr, vt->pa, n);
	b->qc = q+n;
}

/* the cursor to column col of its line, out past the line's end with spaces */
static void
lsetcol(Vt *vt, int col)
{
	Vbuf *b;
	int ls, le;

	b = vt->buf;
	if(col < 0)
		col = 0;
	ls = lstart(b, b->qc);
	le = lend(b, ls);
	if(ls+col > le)
		lins(vt, le, ' ', ls+col-le);
	b->qc = ls+col;
}

/* the first rune of the screen's top row */
static int
ltop(Vt *vt)
{
	Vbuf *b;
	int q, i;

	b = vt->buf;
	q = lstart(b, b->qh);
	for(i = 1; i < vt->rows && q > 0; i++)
		q = lstart(b, q-1);
	return q;
}

static int
lrow(Vt *vt)
{
	Vbuf *b;
	int q, n;

	b = vt->buf;
	n = 0;
	for(q = ltop(vt); q < b->qc; q++)
		if(b->text[q] == '\n')
			n++;
	return n;
}

/* down a line: the next one, or a new one at the end */
static void
lnewline(Vt *vt, int keepcol)
{
	Vbuf *b;
	Rune nl;
	int le, col;

	b = vt->buf;
	col = lcol(vt);
	le = lend(b, b->qc);
	if(le >= b->qh){
		nl = '\n';
		brepl(vt, b->qh, b->qh, &nl, nil, 1);
		b->qc = b->qh;
		/* a new line starts at the left: 9front's programs end lines with \n alone */
		return;
	}
	b->qc = le+1;
	if(keepcol)
		lsetcol(vt, col);
}

static void
lgoto(Vt *vt, int row, int col)
{
	Vbuf *b;
	Rune nl;
	int q, e, i;

	b = vt->buf;
	if(row >= vt->rows)
		row = vt->rows-1;
	q = ltop(vt);
	for(i = 0; i < row; i++){
		e = lend(b, q);
		if(e >= b->qh){
			nl = '\n';
			brepl(vt, b->qh, b->qh, &nl, nil, 1);
			q = b->qh;
		}else
			q = e+1;
	}
	b->qc = q;
	lsetcol(vt, col);
}

static void
lup(Vt *vt, int n)
{
	Vbuf *b;
	int q, top, col;

	b = vt->buf;
	col = lcol(vt);
	top = ltop(vt);
	q = lstart(b, b->qc);
	while(n-- > 0 && q > top)
		q = lstart(b, q-1);
	b->qc = q;
	lsetcol(vt, col);
}

static void
ldown(Vt *vt, int n)
{
	Vbuf *b;
	int q, e, col;

	b = vt->buf;
	col = lcol(vt);
	q = lstart(b, b->qc);
	while(n-- > 0){
		e = lend(b, q);
		if(e >= b->qh)
			break;
		q = e+1;
	}
	b->qc = q;
	lsetcol(vt, col);
}

/* runes q0..q1 to spaces, newlines kept */
static void
lblank(Vt *vt, int q0, int q1)
{
	Vbuf *b;
	Rune *s;
	int i, qc;

	b = vt->buf;
	if(q1 <= q0)
		return;
	qc = b->qc;
	s = vmalloc((q1-q0)*sizeof(Rune));
	for(i = q0; i < q1; i++)
		s[i-q0] = b->text[i] == '\n' ? '\n' : ' ';
	brepl(vt, q0, q1, s, nil, q1-q0);
	free(s);
	b->qc = qc;
}

static void
lel(Vt *vt, int mode)
{
	Vbuf *b;
	int ls, le, col, e;

	b = vt->buf;
	ls = lstart(b, b->qc);
	le = lend(b, ls);
	switch(mode){
	case 0:
		brepl(vt, b->qc, le, nil, nil, 0);
		break;
	case 1:
		e = b->qc+1 < le ? b->qc+1 : le;
		lblank(vt, ls, e);
		break;
	case 2:
		col = b->qc - ls;
		brepl(vt, ls, le, nil, nil, 0);
		b->qc = ls;
		lsetcol(vt, col);
		break;
	}
}

static void
led(Vt *vt, int mode)
{
	Vbuf *b;
	Rune nl;
	int top, row, col, e;

	b = vt->buf;
	switch(mode){
	case 0:
		brepl(vt, b->qc, b->qh, nil, nil, 0);
		break;
	case 1:
		top = ltop(vt);
		e = b->qc < b->qh ? b->qc+1 : b->qc;
		if(e > lend(b, b->qc))
			e = lend(b, b->qc);
		lblank(vt, top, e);
		break;
	case 2:
		/* the screen goes; the cursor stays where it was on it */
		top = ltop(vt);
		row = lrow(vt);
		col = lcol(vt);
		brepl(vt, top, b->qh, nil, nil, 0);
		vt->cleared = row;
		nl = '\n';
		while(row-- > 0)
			brepl(vt, b->qh, b->qh, &nl, nil, 1);
		b->qc = b->qh;
		lsetcol(vt, col);
		return;
	case 3:
		/* what's scrolled back: before the screen, or before what was it if just erased */
		top = ltop(vt);
		if(vt->cleared >= 0){
			top = lstart(b, b->qc);
			for(row = vt->cleared; row > 0 && top > 0; row--)
				top = lstart(b, top-1);
		}
		if(top > 0)
			brepl(vt, 0, top, nil, nil, 0);
		break;
	}
}

/*
 * The alternate screen: a grid.
 */

static Vcell*
cell(Vt *vt, int x, int y)
{
	return &vt->grid[y*vt->cols + x];
}

/* an erased cell: in the current background (xterm's bce) */
static Vcell
blank(Vt *vt)
{
	Vcell c;

	c.r = ' ';
	c.a.fg = 0;
	c.a.bg = vt->a.bg;
	c.a.fl = 0;
	return c;
}

/* cells i..j (row-major, j exclusive) erased */
static void
gerase(Vt *vt, int i, int j)
{
	Vcell b;

	b = blank(vt);
	if(i < 0)
		i = 0;
	if(j > vt->rows*vt->cols)
		j = vt->rows*vt->cols;
	for(; i < j; i++)
		vt->grid[i] = b;
	vt->gdirty = 1;
}

static void
gscrollup(Vt *vt, int top, int bot, int n)
{
	int h;

	if(top > bot)
		return;
	h = bot-top+1;
	if(n > h)
		n = h;
	if(n <= 0)
		return;
	memmove(cell(vt, 0, top), cell(vt, 0, top+n), (h-n)*vt->cols*sizeof(Vcell));
	gerase(vt, (bot-n+1)*vt->cols, (bot+1)*vt->cols);
}

static void
gscrolldown(Vt *vt, int top, int bot, int n)
{
	int h;

	if(top > bot)
		return;
	h = bot-top+1;
	if(n > h)
		n = h;
	if(n <= 0)
		return;
	memmove(cell(vt, 0, top+n), cell(vt, 0, top), (h-n)*vt->cols*sizeof(Vcell));
	gerase(vt, top*vt->cols, (top+n)*vt->cols);
}

static void
glf(Vt *vt)
{
	if(vt->cy == vt->bot)
		gscrollup(vt, vt->top, vt->bot, 1);
	else if(vt->cy < vt->rows-1)
		vt->cy++;
}

static void
gri(Vt *vt)
{
	if(vt->cy == vt->top)
		gscrolldown(vt, vt->top, vt->bot, 1);
	else if(vt->cy > 0)
		vt->cy--;
}

/* a wide character half overwritten loses its other half */
static void
gsplit(Vt *vt, int x, int y)
{
	Vcell *c, b;

	c = cell(vt, x, y);
	b = blank(vt);
	if((c->a.fl & Vcont) && x > 0)
		*cell(vt, x-1, y) = b;
	if((c->a.fl & Vwide) && x+1 < vt->cols)
		*cell(vt, x+1, y) = b;
}

static void
gput(Vt *vt, Rune r)
{
	Vcell *c;
	int w, n;

	w = vtwcwidth(r);
	if(w == 0)
		return;
	if(w > vt->cols)
		w = 1;
	if(vt->wrapnext && vt->autowrap){
		vt->cx = 0;
		glf(vt);
	}
	vt->wrapnext = 0;
	if(w == 2 && vt->cx+1 >= vt->cols){
		if(vt->autowrap){
			gsplit(vt, vt->cx, vt->cy);
			*cell(vt, vt->cx, vt->cy) = blank(vt);
			vt->cx = 0;
			glf(vt);
		}else
			w = 1;
	}
	if(vt->insert){
		n = vt->cols - vt->cx - w;
		if(n > 0)
			memmove(cell(vt, vt->cx+w, vt->cy), cell(vt, vt->cx, vt->cy), n*sizeof(Vcell));
	}
	gsplit(vt, vt->cx, vt->cy);
	c = cell(vt, vt->cx, vt->cy);
	c->r = r;
	c->a = vt->a;
	c->a.fl &= ~(Vwide|Vcont);
	if(w == 2){
		gsplit(vt, vt->cx+1, vt->cy);
		c->a.fl |= Vwide;
		c[1].r = ' ';
		c[1].a = c->a;
		c[1].a.fl = (c->a.fl & ~Vwide) | Vcont;
	}
	vt->cx += w;
	if(vt->cx >= vt->cols){
		vt->cx = vt->cols-1;
		if(vt->autowrap)
			vt->wrapnext = 1;
	}
	vt->gdirty = 1;
}

static void
gclampcur(Vt *vt)
{
	if(vt->cx >= vt->cols)
		vt->cx = vt->cols-1;
	if(vt->cx < 0)
		vt->cx = 0;
	if(vt->cy >= vt->rows)
		vt->cy = vt->rows-1;
	if(vt->cy < 0)
		vt->cy = 0;
}

static void
ggoto(Vt *vt, int row, int col)
{
	int lo, hi;

	lo = 0;
	hi = vt->rows-1;
	if(vt->origin){
		lo = vt->top;
		hi = vt->bot;
		row += vt->top;
	}
	if(row < lo)
		row = lo;
	if(row > hi)
		row = hi;
	vt->cy = row;
	vt->cx = col;
	vt->wrapnext = 0;
	gclampcur(vt);
	vt->gdirty = 1;
}

static void
gnexttab(Vt *vt, int n)
{
	while(n-- > 0){
		while(vt->cx < vt->cols-1){
			vt->cx++;
			if(vt->tabs[vt->cx])
				break;
		}
	}
}

static void
gprevtab(Vt *vt, int n)
{
	while(n-- > 0){
		while(vt->cx > 0){
			vt->cx--;
			if(vt->tabs[vt->cx])
				break;
		}
	}
}

static void
defaulttabs(Vt *vt)
{
	int i;

	for(i = 0; i < vt->cols; i++)
		vt->tabs[i] = i > 0 && i%Tabw == 0;
}

static void
setalt(Vt *vt, int on)
{
	prflush(vt);
	if(on && !vt->alt){
		vt->alt = 1;
		vt->top = 0;
		vt->bot = vt->rows-1;
		vt->cx = vt->cy = 0;
		vt->wrapnext = 0;
		gerase(vt, 0, vt->rows*vt->cols);
	}else if(!on && vt->alt){
		vt->alt = 0;
		vt->gdirty = 1;
	}
}

/*
 * Actions, on whichever screen is showing.
 */

static void
savecur(Vt *vt)
{
	int i;

	i = vt->alt;
	prflush(vt);
	if(i){
		vt->sv[i].x = vt->cx;
		vt->sv[i].y = vt->cy;
	}else
		vt->buf->qs = vt->buf->qc;
	vt->sv[i].a = vt->a;
	vt->sv[i].origin = vt->origin;
	memmove(vt->sv[i].cs, vt->cs, sizeof vt->cs);
	vt->sv[i].gl = vt->gl;
	vt->sv[i].set = 1;
}

static void
restorecur(Vt *vt)
{
	int i;

	i = vt->alt;
	prflush(vt);
	if(!vt->sv[i].set)
		return;
	if(i){
		vt->cx = vt->sv[i].x;
		vt->cy = vt->sv[i].y;
		vt->wrapnext = 0;
		gclampcur(vt);
	}else if(vt->buf->qs >= 0 && vt->buf->qs <= vt->buf->qh)
		vt->buf->qc = vt->buf->qs;
	vt->a = vt->sv[i].a;
	vt->origin = vt->sv[i].origin;
	memmove(vt->cs, vt->sv[i].cs, sizeof vt->cs);
	vt->gl = vt->sv[i].gl;
}

static void
cr(Vt *vt)
{
	if(vt->alt){
		vt->cx = 0;
		vt->wrapnext = 0;
	}else{
		prflush(vt);
		vt->buf->qc = lstart(vt->buf, vt->buf->qc);
	}
}

static void
lf(Vt *vt)
{
	if(vt->alt){
		vt->wrapnext = 0;
		glf(vt);
	}else{
		prflush(vt);
		lnewline(vt, 1);
	}
}

static void
cup(Vt *vt, int row, int col)
{
	if(vt->alt)
		ggoto(vt, row, col);
	else{
		prflush(vt);
		lgoto(vt, row, col);
	}
}

static void
cuu(Vt *vt, int n)
{
	int lo;

	if(vt->alt){
		lo = vt->cy >= vt->top ? vt->top : 0;
		vt->cy -= n;
		if(vt->cy < lo)
			vt->cy = lo;
		vt->wrapnext = 0;
	}else{
		prflush(vt);
		lup(vt, n);
	}
}

static void
cud(Vt *vt, int n)
{
	int hi;

	if(vt->alt){
		hi = vt->cy <= vt->bot ? vt->bot : vt->rows-1;
		vt->cy += n;
		if(vt->cy > hi)
			vt->cy = hi;
		vt->wrapnext = 0;
	}else{
		prflush(vt);
		ldown(vt, n);
	}
}

static void
setcol(Vt *vt, int col)
{
	if(vt->alt){
		vt->cx = col;
		vt->wrapnext = 0;
		gclampcur(vt);
	}else{
		prflush(vt);
		lsetcol(vt, col);
	}
}

static int
curcol(Vt *vt)
{
	if(vt->alt)
		return vt->cx;
	prflush(vt);
	return lcol(vt);
}

static int
currow(Vt *vt)
{
	if(vt->alt)
		return vt->origin ? vt->cy - vt->top : vt->cy;
	prflush(vt);
	return lrow(vt);
}

static void
tab(Vt *vt, int n)
{
	Vbuf *b;
	int col;

	if(vt->alt){
		gnexttab(vt, n);
		vt->wrapnext = 0;
		return;
	}
	prflush(vt);
	b = vt->buf;
	if(b->qc == lend(b, b->qc)){
		/* at a line's end, a tab is kept as one: the console sets tab stops */
		lins(vt, b->qc, '\t', n);	/* the cursor moves on with it */
		return;
	}
	col = lcol(vt);
	lsetcol(vt, (col/Tabw + n)*Tabw);
}

static void
ed(Vt *vt, int mode)
{
	int i;

	if(!vt->alt){
		prflush(vt);
		led(vt, mode);
		return;
	}
	i = vt->cy*vt->cols + vt->cx;
	switch(mode){
	case 0:
		gsplit(vt, vt->cx, vt->cy);
		gerase(vt, i, vt->rows*vt->cols);
		break;
	case 1:
		gsplit(vt, vt->cx, vt->cy);
		gerase(vt, 0, i+1);
		break;
	case 2:
	case 3:
		gerase(vt, 0, vt->rows*vt->cols);
		break;
	}
}

static void
el(Vt *vt, int mode)
{
	int y;

	if(!vt->alt){
		prflush(vt);
		lel(vt, mode);
		return;
	}
	y = vt->cy*vt->cols;
	gsplit(vt, vt->cx, vt->cy);
	switch(mode){
	case 0:
		gerase(vt, y+vt->cx, y+vt->cols);
		break;
	case 1:
		gerase(vt, y, y+vt->cx+1);
		break;
	case 2:
		gerase(vt, y, y+vt->cols);
		break;
	}
}

static void
ich(Vt *vt, int n)
{
	Vbuf *b;
	int qc, k;

	if(!vt->alt){
		prflush(vt);
		b = vt->buf;
		qc = b->qc;
		lins(vt, qc, ' ', n);
		b->qc = qc;
		return;
	}
	k = vt->cols - vt->cx;
	if(n > k)
		n = k;
	gsplit(vt, vt->cx, vt->cy);
	memmove(cell(vt, vt->cx+n, vt->cy), cell(vt, vt->cx, vt->cy), (k-n)*sizeof(Vcell));
	gerase(vt, vt->cy*vt->cols+vt->cx, vt->cy*vt->cols+vt->cx+n);
}

static void
dch(Vt *vt, int n)
{
	Vbuf *b;
	int k, le;

	if(!vt->alt){
		prflush(vt);
		b = vt->buf;
		le = lend(b, b->qc);
		if(n > le - b->qc)
			n = le - b->qc;
		brepl(vt, b->qc, b->qc+n, nil, nil, 0);
		return;
	}
	k = vt->cols - vt->cx;
	if(n > k)
		n = k;
	gsplit(vt, vt->cx, vt->cy);
	memmove(cell(vt, vt->cx, vt->cy), cell(vt, vt->cx+n, vt->cy), (k-n)*sizeof(Vcell));
	gerase(vt, (vt->cy+1)*vt->cols-n, (vt->cy+1)*vt->cols);
}

static void
ech(Vt *vt, int n)
{
	Vbuf *b;
	int le;

	if(!vt->alt){
		prflush(vt);
		b = vt->buf;
		le = lend(b, b->qc);
		if(n > le - b->qc)
			n = le - b->qc;
		lblank(vt, b->qc, b->qc+n);
		return;
	}
	if(n > vt->cols - vt->cx)
		n = vt->cols - vt->cx;
	gsplit(vt, vt->cx, vt->cy);
	gerase(vt, vt->cy*vt->cols+vt->cx, vt->cy*vt->cols+vt->cx+n);
}

static void
il(Vt *vt, int n)
{
	Vbuf *b;
	int ls;

	if(!vt->alt){
		prflush(vt);
		b = vt->buf;
		ls = lstart(b, b->qc);
		lins(vt, ls, '\n', n);
		b->qc = ls;
		return;
	}
	if(vt->cy < vt->top || vt->cy > vt->bot)
		return;
	gscrolldown(vt, vt->cy, vt->bot, n);
	vt->cx = 0;
	vt->wrapnext = 0;
}

static void
dl(Vt *vt, int n)
{
	Vbuf *b;
	int ls, q, e;

	if(!vt->alt){
		prflush(vt);
		b = vt->buf;
		ls = lstart(b, b->qc);
		q = ls;
		while(n-- > 0){
			e = lend(b, q);
			if(e >= b->qh){
				q = e;
				break;
			}
			q = e+1;
		}
		brepl(vt, ls, q, nil, nil, 0);
		b->qc = ls;
		return;
	}
	if(vt->cy < vt->top || vt->cy > vt->bot)
		return;
	gscrollup(vt, vt->cy, vt->bot, n);
	vt->cx = 0;
	vt->wrapnext = 0;
}

static void
reverseindex(Vt *vt)
{
	if(vt->alt)
		gri(vt);
	else{
		prflush(vt);
		lup(vt, 1);
	}
}

static void
softreset(Vt *vt)
{
	memset(&vt->a, 0, sizeof vt->a);
	vt->insert = 0;
	vt->autowrap = 1;
	vt->origin = 0;
	vt->cursor = 1;
	vt->appcursor = 0;
	vt->appkeypad = 0;
	vt->top = 0;
	vt->bot = vt->rows-1;
	vt->cs[0] = vt->cs[1] = vt->cs[2] = vt->cs[3] = 'B';
	vt->gl = 0;
	vt->sv[0].set = vt->sv[1].set = 0;
}

void
vtreset(Vt *vt)
{
	setalt(vt, 0);
	softreset(vt);
	vt->mouse = 0;
	vt->sgrmouse = 0;
	vt->bpaste = 0;
	vt->focus = 0;
	vt->wrapnext = 0;
	vt->state = Ground;
	vt->cleared = -1;
	defaulttabs(vt);
	vt->gdirty = 1;
}

/* SGR */

/* an extended colour at arg i (38, 48 or 58): the last arg it took, its colour in *c */
static int
extcolor(Vt *vt, int i, u32int *c)
{
	int m, k, j;

	if(i+1 >= vt->narg)
		return i;
	m = vt->arg[i+1];
	if(vt->sep[i+1] == ':'){
		/* 38:5:n or 38:2:[colourspace]:r:g:b */
		for(k = i+2; k < vt->narg && vt->sep[k] == ':'; k++)
			;
		j = i+2;
		if(m == 5 && j < k)
			*c = VTpal | (vt->arg[j] & 0xFF);
		else if(m == 2){
			if(k - j >= 4)
				j++;
			if(j+2 < k)
				*c = VTrgb | (vt->arg[j]&0xFF)<<16 | (vt->arg[j+1]&0xFF)<<8 | (vt->arg[j+2]&0xFF);
		}
		return k-1;
	}
	if(m == 5){
		if(i+2 < vt->narg)
			*c = VTpal | (vt->arg[i+2] & 0xFF);
		return i+2;
	}
	if(m == 2){
		if(i+4 < vt->narg)
			*c = VTrgb | (vt->arg[i+2]&0xFF)<<16 | (vt->arg[i+3]&0xFF)<<8 | (vt->arg[i+4]&0xFF);
		return i+4;
	}
	return i+1;
}

static void
sgr(Vt *vt)
{
	Vattr *a;
	u32int dummy;
	int i, p;

	a = &vt->a;
	if(vt->narg == 0){
		memset(a, 0, sizeof *a);
		return;
	}
	for(i = 0; i < vt->narg; i++){
		p = vt->arg[i];
		switch(p){
		case 0:
			memset(a, 0, sizeof *a);
			break;
		case 1:	a->fl |= Vbold; break;
		case 2:	a->fl |= Vfaint; break;
		case 3:	a->fl |= Vitalic; break;
		case 4:
			/* 4:0 is no underline; 4:1 to 4:5 are kinds of it */
			if(i+1 < vt->narg && vt->sep[i+1] == ':' && vt->arg[i+1] == 0)
				a->fl &= ~Vuline;
			else
				a->fl |= Vuline;
			break;
		case 5:
		case 6:	a->fl |= Vblink; break;
		case 7:	a->fl |= Vinverse; break;
		case 8:	a->fl |= Vhidden; break;
		case 9:	a->fl |= Vstrike; break;
		case 21:	a->fl |= Vuline; break;
		case 22:	a->fl &= ~(Vbold|Vfaint); break;
		case 23:	a->fl &= ~Vitalic; break;
		case 24:	a->fl &= ~Vuline; break;
		case 25:	a->fl &= ~Vblink; break;
		case 27:	a->fl &= ~Vinverse; break;
		case 28:	a->fl &= ~Vhidden; break;
		case 29:	a->fl &= ~Vstrike; break;
		case 38:	i = extcolor(vt, i, &a->fg); continue;
		case 39:	a->fg = 0; break;
		case 48:	i = extcolor(vt, i, &a->bg); continue;
		case 49:	a->bg = 0; break;
		case 58:	i = extcolor(vt, i, &dummy); continue;
		default:
			if(p >= 30 && p <= 37)
				a->fg = VTpal | (p-30);
			else if(p >= 40 && p <= 47)
				a->bg = VTpal | (p-40);
			else if(p >= 90 && p <= 97)
				a->fg = VTpal | (p-90+8);
			else if(p >= 100 && p <= 107)
				a->bg = VTpal | (p-100+8);
		}
		/* sub-parameters we don't use */
		while(i+1 < vt->narg && vt->sep[i+1] == ':')
			i++;
	}
}

/* modes */

static int*
decmode(Vt *vt, int n)
{
	switch(n){
	case 1:	return &vt->appcursor;
	case 6:	return &vt->origin;
	case 7:	return &vt->autowrap;
	case 25:	return &vt->cursor;
	case 66:	return &vt->appkeypad;
	case 1004:	return &vt->focus;
	case 1006:	return &vt->sgrmouse;
	case 2004:	return &vt->bpaste;
	}
	return nil;
}

static void
decset(Vt *vt, int n, int on)
{
	int *m;

	switch(n){
	case 6:
		vt->origin = on;
		cup(vt, 0, 0);
		return;
	case 47:
	case 1047:
		setalt(vt, on);
		return;
	case 1048:
		if(on)
			savecur(vt);
		else
			restorecur(vt);
		return;
	case 1049:
		if(on){
			savecur(vt);
			setalt(vt, 1);
		}else{
			setalt(vt, 0);
			restorecur(vt);
		}
		return;
	case 9:
	case 1000:
	case 1002:
	case 1003:
		vt->mouse = on ? n : 0;
		return;
	}
	if((m = decmode(vt, n)) != nil)
		*m = on;
}

static int
decstate(Vt *vt, int n)
{
	int *m;

	switch(n){
	case 47:
	case 1047:
	case 1049:
		return vt->alt ? 1 : 2;
	case 9:
	case 1000:
	case 1002:
	case 1003:
		return vt->mouse == n ? 1 : 2;
	}
	if((m = decmode(vt, n)) != nil)
		return *m ? 1 : 2;
	return 0;
}

/* CSI */

static int
arg(Vt *vt, int i, int def)
{
	return i < vt->narg && vt->arg[i] != 0 ? vt->arg[i] : def;
}

static int
arg0(Vt *vt, int i, int def)
{
	return i < vt->narg ? vt->arg[i] : def;
}

static void
csi(Vt *vt, int f)
{
	char b[64];
	int i, n, x;

	if(f != 'J')
		vt->cleared = -1;

	if(vt->priv == '?'){
		switch(f){
		case 'h':
		case 'l':
			prflush(vt);
			for(i = 0; i < vt->narg; i++)
				decset(vt, vt->arg[i], f == 'h');
			break;
		case 'J':
			ed(vt, arg0(vt, 0, 0));
			break;
		case 'K':
			el(vt, arg0(vt, 0, 0));
			break;
		case 'n':
			if(arg0(vt, 0, 0) == 6){
				snprint(b, sizeof b, "\033[?%d;%dR", currow(vt)+1, curcol(vt)+1);
				reply(vt, b);
			}
			break;
		case 'p':
			if(vt->inter == '$'){
				n = arg0(vt, 0, 0);
				snprint(b, sizeof b, "\033[?%d;%d$y", n, decstate(vt, n));
				reply(vt, b);
			}
			break;
		}
		return;
	}
	if(vt->priv == '>'){
		if(f == 'c')
			reply(vt, "\033[>0;95;0c");	/* a VT100-class xterm, patch 95 */
		else if(f == 'q')
			reply(vt, "\033P>|9ux\033\\");
		return;
	}
	if(vt->priv != 0)
		return;
	switch(vt->inter){
	case 0:
		break;
	case '!':
		if(f == 'p'){
			prflush(vt);
			softreset(vt);
		}
		return;
	case '$':
		if(f == 'p'){
			n = arg0(vt, 0, 0);
			x = n == 4 ? (vt->insert ? 1 : 2) : 0;
			snprint(b, sizeof b, "\033[%d;%d$y", n, x);
			reply(vt, b);
		}
		return;
	default:
		/* DECSCUSR (cursor shape) and the like */
		return;
	}
	n = arg(vt, 0, 1);
	switch(f){
	case '@':	ich(vt, n); break;
	case 'A':	cuu(vt, n); break;
	case 'B':
	case 'e':	cud(vt, n); break;
	case 'C':
	case 'a':	setcol(vt, curcol(vt)+n); break;
	case 'D':	setcol(vt, curcol(vt)-n); break;
	case 'E':	cud(vt, n); cr(vt); break;
	case 'F':	cuu(vt, n); cr(vt); break;
	case 'G':
	case '`':	setcol(vt, n-1); break;
	case 'H':
	case 'f':	cup(vt, arg(vt, 0, 1)-1, arg(vt, 1, 1)-1); break;
	case 'I':	tab(vt, n); break;
	case 'J':	ed(vt, arg0(vt, 0, 0)); break;
	case 'K':	el(vt, arg0(vt, 0, 0)); break;
	case 'L':	il(vt, n); break;
	case 'M':	dl(vt, n); break;
	case 'P':	dch(vt, n); break;
	case 'S':
		if(vt->alt)
			gscrollup(vt, vt->top, vt->bot, n);
		break;
	case 'T':
		if(vt->alt && vt->narg <= 1)
			gscrolldown(vt, vt->top, vt->bot, n);
		break;
	case 'X':	ech(vt, n); break;
	case 'Z':
		if(vt->alt)
			gprevtab(vt, n);
		break;
	case 'b':
		if(vt->last != 0){
			if(n > 65535)
				n = 65535;
			while(n-- > 0)
				putr(vt, vt->last);
		}
		break;
	case 'c':
		if(arg0(vt, 0, 0) == 0)
			reply(vt, "\033[?62;22c");	/* VT220, with ANSI colour */
		break;
	case 'd':	cup(vt, n-1, curcol(vt)); break;
	case 'g':
		if(!vt->alt)
			break;
		x = arg0(vt, 0, 0);
		if(x == 0)
			vt->tabs[vt->cx] = 0;
		else if(x == 3)
			memset(vt->tabs, 0, vt->cols);
		break;
	case 'h':
	case 'l':
		for(i = 0; i < vt->narg; i++)
			if(vt->arg[i] == 4)
				vt->insert = f == 'h';
		break;
	case 'm':
		sgr(vt);
		break;
	case 'n':
		x = arg0(vt, 0, 0);
		if(x == 5)
			reply(vt, "\033[0n");
		else if(x == 6){
			snprint(b, sizeof b, "\033[%d;%dR", currow(vt)+1, curcol(vt)+1);
			reply(vt, b);
		}
		break;
	case 'r':
		if(!vt->alt)
			break;
		i = arg(vt, 0, 1)-1;
		x = arg(vt, 1, vt->rows)-1;
		if(x >= vt->rows)
			x = vt->rows-1;
		if(i < x){
			vt->top = i;
			vt->bot = x;
			ggoto(vt, 0, 0);
		}
		break;
	case 's':
		if(vt->narg == 0)
			savecur(vt);
		break;
	case 'u':
		restorecur(vt);
		break;
	case 't':
		if(arg0(vt, 0, 0) == 18){
			snprint(b, sizeof b, "\033[8;%d;%dt", vt->rows, vt->cols);
			reply(vt, b);
		}
		break;
	}
}

/* OSC and the other strings */

static void
colorreply(Vt *vt, char *what, u32int c)
{
	char b[64];
	int r, g, bl;

	r = c>>16 & 0xFF;
	g = c>>8 & 0xFF;
	bl = c & 0xFF;
	snprint(b, sizeof b, "\033]%s;rgb:%02x%02x/%02x%02x/%02x%02x\033\\", what, r, r, g, g, bl, bl);
	reply(vt, b);
}

static void
strend(Vt *vt)
{
	char *s, *p, what[16];
	int n;

	vt->state = Ground;
	if(vt->strkind != ']' || vt->str == nil)
		return;
	s = vt->str;
	s[vt->nstr] = 0;
	n = 0;
	for(p = s; *p >= '0' && *p <= '9'; p++)
		n = n*10 + *p-'0';
	if(p == s)
		return;
	if(*p == ';')
		p++;
	switch(n){
	case 0:
	case 2:
		if(vt->settitle != nil)
			vt->settitle(vt, p);
		break;
	case 4:
		/* 4;i;? asks for a palette entry */
		n = atoi(p);
		if((p = strchr(p, ';')) != nil && strcmp(p+1, "?") == 0 && n >= 0 && n < 256){
			snprint(what, sizeof what, "4;%d", n);
			colorreply(vt, what, vtrgb(VTpal|n, 0));
		}
		break;
	case 10:
		if(strcmp(p, "?") == 0)
			colorreply(vt, "10", vt->fgrgb);
		break;
	case 11:
		if(strcmp(p, "?") == 0)
			colorreply(vt, "11", vt->bgrgb);
		break;
	}
}

static void
straddc(Vt *vt, Rune r)
{
	char b[UTFmax];
	int n;

	if(vt->str == nil)
		vt->str = vmalloc(Maxstr+1);
	n = runetochar(b, &r);
	if(vt->nstr+n <= Maxstr){
		memmove(vt->str+vt->nstr, b, n);
		vt->nstr += n;
	}
}

/* ESC, and what follows it */

static void
esc(Vt *vt, Rune r)
{
	if(r != '[')
		vt->cleared = -1;
	vt->state = Ground;
	switch(r){
	case '[':
		vt->state = Csi;
		vt->narg = 0;
		vt->priv = 0;
		vt->inter = 0;
		vt->arg[0] = 0;
		vt->sep[0] = ';';
		break;
	case ']':
	case 'P':
	case '_':
	case '^':
	case 'X':
		vt->state = Str;
		vt->strkind = r;
		vt->nstr = 0;
		break;
	case '(':
	case ')':
	case '*':
	case '+':
	case '#':
	case '%':
	case ' ':
		vt->state = Escint;
		vt->inter = r;
		break;
	case '7':
		savecur(vt);
		break;
	case '8':
		restorecur(vt);
		break;
	case 'D':
		lf(vt);
		break;
	case 'E':
		cr(vt);
		lf(vt);
		break;
	case 'H':
		if(vt->alt)
			vt->tabs[vt->cx] = 1;
		break;
	case 'M':
		reverseindex(vt);
		break;
	case 'c':
		vtreset(vt);
		break;
	case '=':
		vt->appkeypad = 1;
		break;
	case '>':
		vt->appkeypad = 0;
		break;
	}
}

static void
escint(Vt *vt, Rune r)
{
	int i, j;

	if(r >= 0x20 && r <= 0x2F){
		vt->inter = r;
		return;
	}
	vt->state = Ground;
	switch(vt->inter){
	case '(':
	case ')':
	case '*':
	case '+':
		i = vt->inter == '(' ? 0 : vt->inter == ')' ? 1 : vt->inter == '*' ? 2 : 3;
		vt->cs[i] = r == '0' ? '0' : 'B';
		break;
	case '#':
		if(r == '8' && vt->alt){
			/* DECALN: the screen full of E's */
			for(i = 0; i < vt->rows; i++)
				for(j = 0; j < vt->cols; j++){
					cell(vt, j, i)->r = 'E';
					memset(&cell(vt, j, i)->a, 0, sizeof(Vattr));
				}
			vt->gdirty = 1;
		}
		break;
	}
}

static void
control(Vt *vt, Rune r)
{
	vt->cleared = -1;
	switch(r){
	case 0x07:
		vt->bell = 1;
		break;
	case 0x08:
		if(vt->alt){
			if(vt->wrapnext)
				vt->wrapnext = 0;
			else if(vt->cx > 0)
				vt->cx--;
		}else
			setcol(vt, curcol(vt)-1);
		break;
	case 0x09:
		tab(vt, 1);
		break;
	case 0x0A:
	case 0x0B:
	case 0x0C:
		lf(vt);
		break;
	case 0x0D:
		cr(vt);
		break;
	case 0x0E:
		vt->gl = 1;
		break;
	case 0x0F:
		vt->gl = 0;
		break;
	}
}

static void
putr(Vt *vt, Rune r)
{
	vt->cleared = -1;
	if(vt->cs[vt->gl] == '0' && r >= 0x5F && r <= 0x7E)
		r = decgfx[r-0x5F];
	vt->last = r;
	if(vt->alt){
		gput(vt, r);
		return;
	}
	if(vt->npr == vt->maxpr){
		prflush(vt);
		if(vt->maxpr == 0){
			vt->maxpr = 1024;
			vt->pr = vmalloc(vt->maxpr*sizeof(Rune));
			vt->pa = vmalloc(vt->maxpr*sizeof(Vattr));
		}
	}
	vt->pr[vt->npr] = r;
	vt->pa[vt->npr] = vt->a;
	vt->pa[vt->npr].fl &= ~(Vwide|Vcont);
	vt->npr++;
}

static void
rune(Vt *vt, Rune r)
{
	if(r == 0x18 || r == 0x1A){	/* CAN, SUB: whatever was going on stops */
		vt->state = Ground;
		return;
	}
	if(r == 0x1B){
		if(vt->state == Str)
			vt->state = Stresc;
		else{
			vt->state = Esc;
			vt->inter = 0;
		}
		return;
	}
	switch(vt->state){
	case Ground:
		if(r < 0x20 || r == 0x7F)
			control(vt, r);
		else if(r < 0x80 || r >= 0xA0)
			putr(vt, r);
		break;
	case Esc:
		if(r < 0x20)
			control(vt, r);
		else
			esc(vt, r);
		break;
	case Escint:
		if(r < 0x20)
			control(vt, r);
		else
			escint(vt, r);
		break;
	case Csi:
		if(r < 0x20){
			control(vt, r);
			break;
		}
		if(r >= '0' && r <= '9'){
			if(vt->narg == 0)
				vt->narg = 1;
			vt->arg[vt->narg-1] = vt->arg[vt->narg-1]*10 + r-'0';
			if(vt->arg[vt->narg-1] > Maxarg)
				vt->arg[vt->narg-1] = Maxarg;
		}else if(r == ';' || r == ':'){
			if(vt->narg == 0)
				vt->narg = 1;
			if(vt->narg < nelem(vt->arg)){
				vt->arg[vt->narg] = 0;
				vt->sep[vt->narg] = r;
				vt->narg++;
			}
		}else if(r >= 0x3C && r <= 0x3F){
			if(vt->narg == 0 && vt->priv == 0 && vt->inter == 0)
				vt->priv = r;
			else
				vt->state = Csiign;
		}else if(r >= 0x20 && r <= 0x2F)
			vt->inter = r;
		else if(r >= 0x40 && r <= 0x7E){
			vt->state = Ground;
			csi(vt, r);
		}else
			vt->state = Csiign;
		break;
	case Csiign:
		if(r < 0x20)
			control(vt, r);
		else if(r >= 0x40 && r <= 0x7E)
			vt->state = Ground;
		break;
	case Str:
		if(r == 0x07)
			strend(vt);
		else if(r >= 0x20)
			straddc(vt, r);
		break;
	case Stresc:
		strend(vt);
		if(r != '\\'){
			vt->state = Esc;
			esc(vt, r);
		}
		break;
	}
}

void
vtwrite(Vt *vt, char *s, long n)
{
	char *e;
	Rune r;
	int k;

	for(e = s+n; s < e; ){
		if(vt->nu == 0 && (uchar)*s < 0x80){
			rune(vt, (uchar)*s++);
			continue;
		}
		while(s < e && vt->nu < UTFmax && !fullrune(vt->u, vt->nu))
			vt->u[vt->nu++] = *s++;
		if(!fullrune(vt->u, vt->nu))
			break;	/* the rest comes with the next write */
		k = chartorune(&r, vt->u);
		vt->nu -= k;
		memmove(vt->u, vt->u+k, vt->nu);
		rune(vt, r);
	}
	prflush(vt);
}

/* setting up */

Vt*
vtnew(int rows, int cols, Vbuf *b)
{
	Vt *vt;

	vt = vmalloc(sizeof *vt);
	vt->buf = b;
	vt->rows = rows > 0 ? rows : 1;
	vt->cols = cols > 0 ? cols : 1;
	vt->grid = vmalloc(vt->rows*vt->cols*sizeof(Vcell));
	vt->tabs = vmalloc(vt->cols);
	b->qs = -1;
	vt->bgrgb = 0xFFFFFF;
	vtreset(vt);
	gerase(vt, 0, vt->rows*vt->cols);
	return vt;
}

void
vtfree(Vt *vt)
{
	if(vt == nil)
		return;
	free(vt->grid);
	free(vt->tabs);
	free(vt->str);
	free(vt->pr);
	free(vt->pa);
	free(vt);
}

void
vtresize(Vt *vt, int rows, int cols)
{
	Vcell *g, b;
	int x, y, i, oc;

	if(rows < 1)
		rows = 1;
	if(cols < 1)
		cols = 1;
	if(rows == vt->rows && cols == vt->cols)
		return;
	prflush(vt);
	b = blank(vt);
	b.a.bg = 0;
	g = vmalloc(rows*cols*sizeof(Vcell));
	for(i = 0; i < rows*cols; i++)
		g[i] = b;
	/* keep the bottom rows, as xterm does when the cursor would go off */
	y = vt->cy - (rows-1);
	if(y < 0)
		y = 0;
	for(i = 0; i < rows && y+i < vt->rows; i++)
		for(x = 0; x < cols && x < vt->cols; x++)
			g[i*cols+x] = vt->grid[(y+i)*vt->cols+x];
	free(vt->grid);
	vt->grid = g;
	vt->cy -= y;
	oc = vt->cols;
	vt->rows = rows;
	vt->cols = cols;
	vt->tabs = vrealloc(vt->tabs, cols);
	for(x = oc; x < cols; x++)
		vt->tabs[x] = x%Tabw == 0;
	vt->top = 0;
	vt->bot = rows-1;
	vt->wrapnext = 0;
	gclampcur(vt);
	vt->gdirty = 1;
}

/* the alternate screen as text, a line per row, without trailing blanks */
char*
vtscreen(Vt *vt)
{
	char *s, *p, *e;
	Vcell *c;
	int x, y;

	s = vmalloc(vt->rows*(vt->cols*UTFmax+1)+1);
	p = s;
	for(y = 0; y < vt->rows; y++){
		e = p;
		for(x = 0; x < vt->cols; x++){
			c = cell(vt, x, y);
			if(c->a.fl & Vcont)
				continue;
			p += runetochar(p, &c->r);
			if(c->r != ' ')
				e = p;
		}
		p = e;
		*p++ = '\n';
	}
	*p = 0;
	return s;
}

/* keys and the mouse, as xterm sends them */

int
vtkey(Vt *vt, Rune r, char *b, int nb)
{
	static char *fn[] = {
		"\033OP", "\033OQ", "\033OR", "\033OS", "\033[15~", "\033[17~",
		"\033[18~", "\033[19~", "\033[20~", "\033[21~", "\033[23~", "\033[24~",
	};
	char *s, c;
	int n;

	s = nil;
	c = 0;
	switch(r){
	case Kup:	c = 'A'; break;
	case Kdown:	c = 'B'; break;
	case Kright:	c = 'C'; break;
	case Kleft:	c = 'D'; break;
	case Khome:	c = 'H'; break;
	case Kend:	c = 'F'; break;
	case Kpgup:	s = "\033[5~"; break;
	case Kpgdown:	s = "\033[6~"; break;
	case Kins:	s = "\033[2~"; break;
	default:
		if(r >= KF+1 && r <= KF+12)
			s = fn[r-KF-1];
		else if(r >= KF && r < KF+0x1000)
			return 0;	/* no xterm key for it */
	}
	if(c != 0){
		if(nb < 4)
			return 0;
		b[0] = '\033';
		b[1] = vt->appcursor ? 'O' : '[';
		b[2] = c;
		b[3] = 0;
		return 3;
	}
	if(s != nil){
		n = strlen(s);
		if(n >= nb)
			return 0;
		memmove(b, s, n+1);
		return n;
	}
	if(nb <= UTFmax)
		return 0;
	n = runetochar(b, &r);
	b[n] = 0;
	return n;
}

/*
 * A mouse event at cell (col, row), if the program asked for it: but
 * is 0, 1 or 2 for the left, middle or right button (or 3 for none,
 * moving), or 64 or 65 for the wheel. 0 if it isn't reported.
 */
int
vtmouse(Vt *vt, int but, int kind, int col, int row, char *b, int nb)
{
	int cb;

	if(vt->mouse == 0)
		return 0;
	if(kind == VMmotion && (vt->mouse == 9 || vt->mouse == 1000 || (vt->mouse == 1002 && but == 3)))
		return 0;
	if(kind == VMrelease && (vt->mouse == 9 || but >= 64))
		return 0;
	cb = but;
	if(kind == VMmotion)
		cb += 32;
	if(vt->sgrmouse)
		return snprint(b, nb, "\033[<%d;%d;%d%c", cb, col+1, row+1, kind == VMrelease ? 'm' : 'M');
	if(kind == VMrelease)
		cb = 3;
	if(col > 222)
		col = 222;
	if(row > 222)
		row = 222;
	return snprint(b, nb, "\033[M%c%c%c", 32+cb, 32+col+1, 32+row+1);
}
