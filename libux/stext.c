/*
 * Spike: uitree's styled-text element on libdraw.
 *
 * text is the whole text; runs is one line per styled range,
 * "start len word...", byte offsets into text, as the GPUI host takes
 * them. Words: text-color= bg= font-bold font-semibold italic
 * underline line-through, and the size words text-xs .. text-3xl.
 * The node's own style gives the base size, weight, colour and
 * text-left/center/right. Text wraps at blanks to the node's width;
 * a newline breaks the line. Runs may change the font mid-line, and
 * the line's baseline is shared.
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

typedef struct Look Look;
typedef struct Piece Piece;

struct Look {
	Font	*f;
	ulong	fg;
	int	hasbg;
	ulong	bg;
	int	under;
	int	strike;
};

/* a stretch of text in one look that doesn't contain a break */
struct Piece {
	int	s, e;	/* byte offsets */
	int	look;
	int	space;	/* all blanks: a break opportunity */
	int	nl;	/* a newline */
	int	w;
};

static Look
baselook(Uxctl *c)
{
	Look l;
	Uxstyle *s;

	memset(&l, 0, sizeof l);
	s = c->st;
	l.f = _font(c->w, s != nil && s->textsize >= 0 ? s->textsize : 2, s != nil && s->bold > 0, 0);
	l.fg = s != nil && s->hasfg ? s->fg : UXink;
	return l;
}

/* the looks: 0 is the base, then one per run line; at[i] is byte i's look */
static Look*
looks(Uxctl *c, int *at, int ntext, int *nlook)
{
	Look *lk, l, base;
	char *buf, *line, *nl, *f[32];
	int n, i, k, st, ln, size, bold, ital;

	base = baselook(c);
	lk = emalloc(sizeof(Look));
	lk[0] = base;
	*nlook = 1;
	for(i = 0; i < ntext; i++)
		at[i] = 0;
	if(c->runs == nil)
		return lk;
	buf = estrdup(c->runs);
	for(line = buf; line != nil && *line; line = nl){
		nl = strchr(line, '\n');
		if(nl != nil)
			*nl++ = 0;
		n = tokenize(line, f, nelem(f));
		if(n < 2)
			continue;
		st = atoi(f[0]);
		ln = atoi(f[1]);
		if(st < 0 || ln <= 0 || st >= ntext)
			continue;
		if(st + ln > ntext)
			ln = ntext - st;
		l = base;
		size = c->st != nil && c->st->textsize >= 0 ? c->st->textsize : 2;
		bold = c->st != nil && c->st->bold > 0;
		ital = 0;
		for(k = 2; k < n; k++){
			char *v;

			v = strchr(f[k], '=');
			if(v != nil)
				*v++ = 0;
			if(strcmp(f[k], "text-color") == 0 || strcmp(f[k], "color") == 0){
				if(v != nil)
					_parsecolor(v, &l.fg);
			}else if(strcmp(f[k], "bg") == 0 || strcmp(f[k], "text-bg") == 0){
				if(v != nil && _parsecolor(v, &l.bg) == 0)
					l.hasbg = 1;
			}else if(strcmp(f[k], "font-bold") == 0 || strcmp(f[k], "font-semibold") == 0)
				bold = 1;
			else if(strcmp(f[k], "italic") == 0)
				ital = 1;
			else if(strcmp(f[k], "underline") == 0)
				l.under = 1;
			else if(strcmp(f[k], "line-through") == 0)
				l.strike = 1;
			else if(strcmp(f[k], "text-xs") == 0)
				size = 0;
			else if(strcmp(f[k], "text-sm") == 0)
				size = 1;
			else if(strcmp(f[k], "text-base") == 0)
				size = 2;
			else if(strcmp(f[k], "text-lg") == 0)
				size = 3;
			else if(strcmp(f[k], "text-xl") == 0)
				size = 4;
			else if(strcmp(f[k], "text-2xl") == 0 || strcmp(f[k], "text-3xl") == 0)
				size = 5;
		}
		l.f = _font(c->w, size, bold, ital);
		lk = erealloc(lk, (*nlook+1)*sizeof(Look));
		lk[*nlook] = l;
		for(i = st; i < st+ln; i++)
			at[i] = *nlook;
		(*nlook)++;
	}
	free(buf);
	return lk;
}

static int
piecew(char *t, Piece *p, Look *lk)
{
	char *s;
	int w;

	s = emalloc(p->e - p->s + 1);
	memmove(s, t + p->s, p->e - p->s);
	w = stringwidth(lk[p->look].f, s);
	free(s);
	return w;
}

static void
mkpiece(Piece *p, int s, int e, int look, int space, int nl)
{
	p->s = s;
	p->e = e;
	p->look = look;
	p->space = space;
	p->nl = nl;
	p->w = 0;
}

static Piece*
pieces(char *t, int n, int *at, Look *lk, int *np)
{
	Piece *p;
	int i, j, sp;

	p = nil;
	*np = 0;
	for(i = 0; i < n; i = j){
		if(*np % 64 == 0)
			p = erealloc(p, (*np+64)*sizeof(Piece));
		if(t[i] == '\n'){
			mkpiece(&p[(*np)++], i, i+1, at[i], 0, 1);
			j = i+1;
			continue;
		}
		sp = t[i] == ' ' || t[i] == '\t';
		for(j = i+1; j < n && at[j] == at[i] && t[j] != '\n'
			&& ((t[j] == ' ' || t[j] == '\t') == sp); j++)
			;
		/* don't split a UTF-8 sequence */
		while(j < n && (t[j] & 0xC0) == 0x80)
			j++;
		mkpiece(&p[*np], i, j, at[i], sp, 0);
		p[*np].w = piecew(t, &p[*np], lk);
		(*np)++;
	}
	return p;
}

/*
 * Lay the text out in width (-1: no limit). With r non-nil, draw it
 * there. Returns the size it takes.
 */
static Point
flow(Uxctl *c, int width, Rectangle *r)
{
	char *t, *s;
	int n, *at, nlook, np, i, j, k, ls, le, lw, x, y, asc, desc, maxw, ww, align;
	Look *lk, *l;
	Piece *p;
	Uxwin *w;
	Point pt;

	w = c->w;
	t = c->stext != nil ? c->stext : "";
	n = strlen(t);
	at = emalloc((n+1)*sizeof(int));
	lk = looks(c, at, n, &nlook);
	p = pieces(t, n, at, lk, &np);
	align = c->st != nil && c->st->textalign > 0 ? c->st->textalign : 0;
	y = 0;
	maxw = 0;
	for(i = 0; i < np || (i == 0 && np == 0); ){
		/* gather a line: pieces ls..le-1 */
		ls = i;
		lw = 0;
		for(j = i; j < np && !p[j].nl; j = k){
			if(p[j].space){
				ww = p[j].w;
				k = j+1;
			}else{
				/* a word: the pieces up to the next blank */
				ww = 0;
				for(k = j; k < np && !p[k].nl && !p[k].space; k++)
					ww += p[k].w;
				if(width >= 0 && lw > 0 && lw + ww > width)
					break;
			}
			lw += ww;
		}
		le = j;
		/* trailing blanks don't count */
		while(le > ls && p[le-1].space)
			lw -= p[--le].w;
		asc = desc = 0;
		for(k = ls; k < le; k++){
			l = &lk[p[k].look];
			if(l->f->ascent > asc)
				asc = l->f->ascent;
			if(l->f->height - l->f->ascent > desc)
				desc = l->f->height - l->f->ascent;
		}
		if(le == ls){
			asc = lk[0].f->ascent;
			desc = lk[0].f->height - asc;
		}
		if(r != nil){
			x = r->min.x;
			if(align == 1 && width >= 0)
				x += (width - lw)/2;
			else if(align == 2 && width >= 0)
				x += width - lw;
			for(k = ls; k < le; k++){
				l = &lk[p[k].look];
				s = emalloc(p[k].e - p[k].s + 1);
				memmove(s, t + p[k].s, p[k].e - p[k].s);
				pt = Pt(x, r->min.y + y + asc - l->f->ascent);
				if(l->hasbg)
					draw(w->img, Rect(x, r->min.y+y, x+p[k].w, r->min.y+y+asc+desc), _anycolor(w, l->bg), nil, ZP);
				string(w->img, pt, _anycolor(w, l->fg), ZP, l->f, s);
				if(l->under)
					draw(w->img, Rect(x, r->min.y+y+asc+1, x+p[k].w, r->min.y+y+asc+2), _anycolor(w, l->fg), nil, ZP);
				if(l->strike)
					draw(w->img, Rect(x, r->min.y+y+asc-l->f->ascent/3-1, x+p[k].w, r->min.y+y+asc-l->f->ascent/3), _anycolor(w, l->fg), nil, ZP);
				free(s);
				x += p[k].w;
			}
		}
		if(lw > maxw)
			maxw = lw;
		y += asc + desc;
		/* skip the break: blanks or one newline */
		i = j;
		while(i < np && p[i].space)
			i++;
		if(i < np && p[i].nl)
			i++;
		else if(i >= np)
			break;
		if(np == 0)
			break;
	}
	free(at);
	free(lk);
	free(p);
	return Pt(maxw, y);
}

static char *stextfiles[] = { "text", "runs", nil };
static Verb stextverbs[] = { nil };
Ctltype _stexttype = { "styled-text", stextfiles, stextverbs, nil, nil, nil, _spikemouse, nil, 0 };

Point
_stextsize(Uxctl *c, int availw)
{
	return flow(c, availw, nil);
}

void
_drawstext(Uxctl *c, Rectangle r)
{
	flow(c, Dx(r), &r);
}

Uxctl*
uxstyledtext(Uxwin *w, char *id, char *text, char *runs)
{
	Uxctl *c;

	qlock(&uxlk);
	c = _newctl(w, id, &_stexttype, "", ZR);
	c->stext = estrdup(text);
	c->runs = runs != nil ? estrdup(runs) : nil;
	qunlock(&uxlk);
	return c;
}

void
uxsetstext(Uxctl *c, char *text, char *runs)
{
	qlock(&uxlk);
	free(c->stext);
	free(c->runs);
	c->stext = estrdup(text);
	c->runs = runs != nil ? estrdup(runs) : nil;
	c->w->dirty = 1;
	qunlock(&uxlk);
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
}
