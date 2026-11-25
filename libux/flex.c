/*
 * Spike: uitree's portable style words and a flexbox layout.
 *
 * A window with a root style (uxwinstyle) is laid out by flexbox on
 * every redraw: its top-level controls are the root's children, and a
 * div or group control's children are those whose parent it is. Every
 * word in uitree core.md's portable list is accepted; a few are
 * accepted and ignored (flex-wrap, overflow-*, rounded-*, state
 * prefixes such as hover:), as the spike notes say.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <mouse.h>
#include <keyboard.h>
#include <fcall.h>
#include <9p.h>
#include "ux.h"
#include "impl.h"

enum {
	Rem	= 16,	/* pixels in 1rem */
	Big	= 1<<28,
	Maxkids	= 512,
};

/* theme tokens, mapped onto the 9/UX palette */
static struct {
	char	*name;
	ulong	rgba;
} tokens[] = {
	"background",	UXpaper,
	"foreground",	UXink,
	"muted",	UXgrey1,
	"muted-foreground",	UXgrey4,
	"primary",	UXdeep,
	"primary-foreground",	UXpaper,
	"accent",	UXbutter,
	"accent-foreground",	UXink,
	"border",	UXgrey3,
	"ring",	UXteal,
	"danger",	UXred,
	"success",	UXolive,
	"warning",	UXbutter,
	/* host extras: the 9/UX palette by name */
	"ink",	UXink,
	"paper",	UXpaper,
	"cream",	UXcream,
	"butter",	UXbutter,
	"olive",	UXolive,
	"mist",	UXmist,
	"sea",	UXsea,
	"teal",	UXteal,
	"deep",	UXdeep,
	"navy",	UXnavy,
	"red",	UXred,
	"grey1",	UXgrey1,
	"grey2",	UXgrey2,
	"grey3",	UXgrey3,
	"grey4",	UXgrey4,
	"grey5",	UXgrey5,
};

int
_parsecolor(char *s, ulong *rgba)
{
	int i, n;
	char *e;
	ulong v;

	if(s[0] == '#'){
		n = strlen(s+1);
		if(n != 6 && n != 8)
			return -1;
		v = strtoul(s+1, &e, 16);
		if(*e != 0)
			return -1;
		if(n == 6)
			v = v<<8 | 0xFF;
		*rgba = v;
		return 0;
	}
	for(i = 0; i < nelem(tokens); i++)
		if(strcmp(tokens[i].name, s) == 0){
			*rgba = tokens[i].rgba;
			return 0;
		}
	return -1;
}

/* n, n%, nrem, or full */
static int
parselen(char *s, Uxlen *l)
{
	char *e;
	double v;

	if(strcmp(s, "full") == 0){
		l->kind = Lpct;
		l->v = 100;
		return 0;
	}
	v = strtod(s, &e);
	if(e == s)
		return -1;
	if(*e == 0)
		l->kind = Lpx;
	else if(strcmp(e, "%") == 0)
		l->kind = Lpct;
	else if(strcmp(e, "rem") == 0){
		l->kind = Lpx;
		v *= Rem;
	}else
		return -1;
	l->v = v;
	return 0;
}

static int
pxval(char *s, int *v)
{
	Uxlen l;

	if(parselen(s, &l) < 0 || l.kind != Lpx)
		return -1;
	*v = l.v;
	return 0;
}

void
_styledefault(Uxstyle *s)
{
	memset(s, 0, sizeof *s);
	s->dir = Dcol;
	s->shrink = 1;
	s->align = Astretch;
	s->justify = Jstart;
	s->textalign = -1;
	s->textsize = -1;
	s->bold = -1;
}

/* words that are accepted and change nothing in this spike */
static char *ignored[] = {
	"flex-wrap", "overflow-hidden", "overflow-y-scroll", "overflow-x-scroll",
	"relative", "rounded-sm", "rounded-md", "rounded-lg",
	nil,
};

/*
 * Parse style words into s (on top of what it holds).
 * Returns nil, or the error, as uitree spells it.
 */
char*
_parsestyle(Uxstyle *s, char *words)
{
	char *buf, *f[64], *w, *v, *q;
	int i, n, x, k;
	static char err[128];

	buf = estrdup(words);
	n = tokenize(buf, f, nelem(f));
	for(i = 0; i < n; i++){
		w = f[i];
		/* state forms: accepted, not drawn */
		if((q = strchr(w, ':')) != nil && q[1] != 0
		&& (strncmp(w, "hover:", 6) == 0 || strncmp(w, "active:", 7) == 0
		|| strncmp(w, "focus:", 6) == 0 || strncmp(w, "disabled:", 9) == 0))
			continue;
		v = strchr(w, '=');
		if(v != nil)
			*v++ = 0;
		if(v == nil){
			for(k = 0; ignored[k]; k++)
				if(strcmp(ignored[k], w) == 0)
					break;
			if(ignored[k])
				continue;
			if(strcmp(w, "flex") == 0 || strcmp(w, "flex-row") == 0)
				s->dir = Drow;
			else if(strcmp(w, "flex-col") == 0)
				s->dir = Dcol;
			else if(strcmp(w, "h-flex") == 0){
				s->dir = Drow;
				s->align = Acenter;
			}else if(strcmp(w, "v-flex") == 0)
				s->dir = Dcol;
			else if(strcmp(w, "flex-1") == 0){
				s->grow = 1;
				s->shrink = 1;
				s->basis0 = 1;
			}else if(strcmp(w, "flex-none") == 0){
				s->grow = 0;
				s->shrink = 0;
			}else if(strcmp(w, "items-start") == 0 || strcmp(w, "items-baseline") == 0)
				s->align = Astart;
			else if(strcmp(w, "items-center") == 0)
				s->align = Acenter;
			else if(strcmp(w, "items-end") == 0)
				s->align = Aend;
			else if(strcmp(w, "items-stretch") == 0)
				s->align = Astretch;
			else if(strcmp(w, "justify-start") == 0)
				s->justify = Jstart;
			else if(strcmp(w, "justify-center") == 0)
				s->justify = Jcenter;
			else if(strcmp(w, "justify-end") == 0)
				s->justify = Jend;
			else if(strcmp(w, "justify-between") == 0)
				s->justify = Jbetween;
			else if(strcmp(w, "size-full") == 0){
				parselen("full", &s->w);
				parselen("full", &s->h);
			}else if(strcmp(w, "w-full") == 0)
				parselen("full", &s->w);
			else if(strcmp(w, "h-full") == 0)
				parselen("full", &s->h);
			else if(strcmp(w, "absolute") == 0)
				s->abs = 1;
			else if(strcmp(w, "border-1") == 0)
				s->border = 1;
			else if(strcmp(w, "text-xs") == 0)
				s->textsize = 0;
			else if(strcmp(w, "text-sm") == 0)
				s->textsize = 1;
			else if(strcmp(w, "text-base") == 0)
				s->textsize = 2;
			else if(strcmp(w, "text-lg") == 0)
				s->textsize = 3;
			else if(strcmp(w, "text-xl") == 0)
				s->textsize = 4;
			else if(strcmp(w, "text-2xl") == 0 || strcmp(w, "text-3xl") == 0)
				s->textsize = 5;
			else if(strcmp(w, "font-semibold") == 0 || strcmp(w, "font-bold") == 0)
				s->bold = 1;
			else if(strcmp(w, "font-normal") == 0)
				s->bold = 0;
			else if(strcmp(w, "text-left") == 0)
				s->textalign = 0;
			else if(strcmp(w, "text-center") == 0)
				s->textalign = 1;
			else if(strcmp(w, "text-right") == 0)
				s->textalign = 2;
			else if(strcmp(w, "truncate") == 0)
				s->truncate = 1;
			else
				goto bad;
			continue;
		}
		if(strcmp(w, "gap") == 0){
			if(pxval(v, &s->gap) < 0)
				goto bad;
		}else if(strcmp(w, "p") == 0 || strcmp(w, "px") == 0 || strcmp(w, "py") == 0
		|| strcmp(w, "pt") == 0 || strcmp(w, "pb") == 0 || strcmp(w, "pl") == 0 || strcmp(w, "pr") == 0
		|| strcmp(w, "m") == 0 || strcmp(w, "mx") == 0 || strcmp(w, "my") == 0){
			int *b;	/* top right bottom left */

			if(pxval(v, &x) < 0)
				goto bad;
			b = w[0] == 'p' ? s->pad : s->mar;
			switch(w[1]){
			case 0:	b[0] = b[1] = b[2] = b[3] = x; break;
			case 'x':	b[1] = b[3] = x; break;
			case 'y':	b[0] = b[2] = x; break;
			case 't':	b[0] = x; break;
			case 'r':	b[1] = x; break;
			case 'b':	b[2] = x; break;
			case 'l':	b[3] = x; break;
			}
		}else if(strcmp(w, "w") == 0){
			if(parselen(v, &s->w) < 0)
				goto bad;
		}else if(strcmp(w, "h") == 0){
			if(parselen(v, &s->h) < 0)
				goto bad;
		}else if(strcmp(w, "min-w") == 0){
			if(parselen(v, &s->minw) < 0)
				goto bad;
		}else if(strcmp(w, "min-h") == 0){
			if(parselen(v, &s->minh) < 0)
				goto bad;
		}else if(strcmp(w, "max-w") == 0){
			if(parselen(v, &s->maxw) < 0)
				goto bad;
		}else if(strcmp(w, "max-h") == 0){
			if(parselen(v, &s->maxh) < 0)
				goto bad;
		}else if(strcmp(w, "top") == 0 || strcmp(w, "left") == 0
		|| strcmp(w, "right") == 0 || strcmp(w, "bottom") == 0){
			k = w[0] == 't' ? 0 : w[0] == 'r' ? 1 : w[0] == 'b' ? 2 : 3;
			if(pxval(v, &s->inset[k]) < 0)
				goto bad;
			s->hasinset |= 1<<k;
		}else if(strcmp(w, "bg") == 0){
			if(_parsecolor(v, &s->bg) < 0)
				goto bad;
			s->hasbg = 1;
		}else if(strcmp(w, "text-color") == 0){
			if(_parsecolor(v, &s->fg) < 0)
				goto bad;
			s->hasfg = 1;
		}else if(strcmp(w, "border-color") == 0){
			if(_parsecolor(v, &s->bordercol) < 0)
				goto bad;
			s->hasbordercol = 1;
		}else{
			v[-1] = '=';
			goto bad;
		}
	}
	free(buf);
	s->set = 1;
	return nil;
bad:
	snprint(err, sizeof err, "unknown style word: %s%s%s", w, v ? "=" : "", v ? v : "");
	free(buf);
	return err;
}

/*
 * Layout.
 */

static Uxstyle defstyle;

static Uxstyle*
sty(Uxctl *c)
{
	if(c->st != nil)
		return c->st;
	if(defstyle.shrink == 0)
		_styledefault(&defstyle);
	return &defstyle;
}

static int
iscontainer(Uxctl *c)
{
	return c->t == &_divtype || strcmp(c->t->name, "group") == 0;
}

/* resolve a length against the containing size; -1 when not set */
static int
len(Uxlen *l, int of)
{
	switch(l->kind){
	case Lpx:
		return l->v;
	case Lpct:
		if(of >= Big || of < 0)
			return -1;
		return of * l->v / 100;
	}
	return -1;
}

static int
clamp(int v, Uxlen *mn, Uxlen *mx, int of)
{
	int m;

	m = len(mx, of);
	if(m >= 0 && v > m)
		v = m;
	m = len(mn, of);
	if(m >= 0 && v < m)
		v = m;
	return v;
}

/* a group's frame and label take room at its edges */
static void
padding(Uxctl *c, int *p)
{
	Uxstyle *s;
	int i;

	s = sty(c);
	for(i = 0; i < 4; i++)
		p[i] = s->pad[i] + s->border;
	if(c != nil && strcmp(c->t->name, "group") == 0){
		p[0] += c->w->sysfont->height + 4;
		p[1] += 8;
		p[2] += 8;
		p[3] += 8;
	}
}

/* the children of box (nil: the window's root) that take part in layout */
static int
kids(Uxwin *w, Uxctl *box, Uxctl **out, int max)
{
	int i, n;
	Uxctl *c;

	n = 0;
	for(i = 0; i < w->nctl && n < max; i++){
		c = w->ctl[i];
		if(c->dead || c->parent != box || !(c->state & Cvisible))
			continue;
		out[n++] = c;
	}
	return n;
}

static Point natural(Uxctl*, int);

/* a font for a style's text words, or nil for the control's own */
Font*
_stylefont(Uxwin *w, Uxstyle *s, int bold)
{
	if(s != nil && s->bold >= 0)
		bold = s->bold;
	return _font(w, s != nil && s->textsize >= 0 ? s->textsize : 2, bold, 0);
}

/* the size a leaf control wants, its width limited to availw (-1: none) */
static Point
measure(Uxctl *c, int availw)
{
	Uxwin *w;
	Font *f;
	char *s;
	Point p;
	int x;

	w = c->w;
	p = Pt(100, 100);
	if(c->t == &_svgtype)
		return _svgsize(c);
	if(c->t == &_stexttype)
		return _stextsize(c, availw);
	if(strcmp(c->t->name, "button") == 0){
		x = stringwidth(w->sysfont, c->label) + 2*UXbuttonpad;
		if(x < UXbuttonminwidth)
			x = UXbuttonminwidth;
		return Pt(x, UXbuttonheight);
	}
	if(strcmp(c->t->name, "checkbox") == 0)
		return Pt(UXchecksize + UXcheckgap + stringwidth(w->sysfont, c->label), UXbuttonheight);
	if(strcmp(c->t->name, "text-field") == 0)
		return Pt(160, UXfieldheight + 2);
	if(strcmp(c->t->name, "label") == 0){
		s = _runestr(c->text, c->ntext);
		f = c->st != nil && c->st->set ? _stylefont(w, c->st, 1)
			: (c->state & UXsmall) ? w->smallfont : w->sysfont;
		p = Pt(stringwidth(f, s), f->height);
		free(s);
		return p;
	}
	USED(availw);
	return p;
}

/* the size c wants, border box, without margins */
static Point
natural(Uxctl *c, int availw)
{
	Uxstyle *s, *cs;
	Uxctl **k;
	int n, i, pad[4], ew, eh, inner;
	Point p, q;

	s = sty(c);
	ew = len(&s->w, availw);
	eh = len(&s->h, -1);
	if(!iscontainer(c)){
		p = measure(c, ew >= 0 ? ew : availw);
		if(ew >= 0)
			p.x = ew;
		if(eh >= 0)
			p.y = eh;
		return p;
	}
	padding(c, pad);
	inner = ew >= 0 ? ew : availw;
	if(inner >= 0)
		inner -= pad[1] + pad[3];
	k = emalloc(Maxkids*sizeof(Uxctl*));
	n = kids(c->w, c, k, Maxkids);
	p = ZP;
	for(i = 0; i < n; i++){
		cs = sty(k[i]);
		if(cs->abs)
			continue;
		q = natural(k[i], s->dir == Dcol && inner >= 0 ? inner - cs->mar[1] - cs->mar[3] : -1);
		q.x += cs->mar[1] + cs->mar[3];
		q.y += cs->mar[0] + cs->mar[2];
		if(s->dir == Drow){
			p.x += q.x + (i > 0 ? s->gap : 0);
			if(q.y > p.y)
				p.y = q.y;
		}else{
			p.y += q.y + (i > 0 ? s->gap : 0);
			if(q.x > p.x)
				p.x = q.x;
		}
	}
	free(k);
	p.x += pad[1] + pad[3];
	p.y += pad[0] + pad[2];
	if(ew >= 0)
		p.x = ew;
	if(eh >= 0)
		p.y = eh;
	return p;
}

#define MAIN(p)	(row ? (p).x : (p).y)
#define CROSS(p)	(row ? (p).y : (p).x)

static void layout(Uxwin*, Uxctl*, Uxstyle*, Rectangle);

static void
place(Uxwin *w, Uxctl *c, Rectangle r)
{
	c->r = r;
	if(iscontainer(c))
		layout(w, c, sty(c), r);
}

/* absolutely placed children, against the padding box pb */
static void
placeabs(Uxwin *w, Uxctl *c, Rectangle pb)
{
	Uxstyle *s;
	Point sz;
	Rectangle r;

	s = sty(c);
	sz = natural(c, Dx(pb));
	if(len(&s->w, Dx(pb)) >= 0)
		sz.x = len(&s->w, Dx(pb));
	if(len(&s->h, Dy(pb)) >= 0)
		sz.y = len(&s->h, Dy(pb));
	r.min = pb.min;
	if(s->hasinset & 1<<3)
		r.min.x = pb.min.x + s->inset[3];
	else if(s->hasinset & 1<<1)
		r.min.x = pb.max.x - s->inset[1] - sz.x;
	if(s->hasinset & 1<<0)
		r.min.y = pb.min.y + s->inset[0];
	else if(s->hasinset & 1<<2)
		r.min.y = pb.max.y - s->inset[2] - sz.y;
	if((s->hasinset & (1<<3|1<<1)) == (1<<3|1<<1))
		sz.x = pb.max.x - s->inset[1] - r.min.x;
	if((s->hasinset & (1<<0|1<<2)) == (1<<0|1<<2))
		sz.y = pb.max.y - s->inset[2] - r.min.y;
	r.max = addpt(r.min, sz);
	place(w, c, r);
}

/* lay out box's children (box nil: the window root) inside r, box's border box */
static void
layout(Uxwin *w, Uxctl *box, Uxstyle *s, Rectangle r)
{
	Uxctl **k;
	Uxstyle *cs;
	int n, i, row, pad[4], cmain, ccross, room, sumgrow, sumshrink, nflow;
	int *base, *cross, mn, mx, pos, extra, between, avail;
	Rectangle content, cr;
	Point q;

	if(box != nil)
		padding(box, pad);
	else
		for(i = 0; i < 4; i++)
			pad[i] = s->pad[i] + s->border;
	content = Rect(r.min.x+pad[3], r.min.y+pad[0], r.max.x-pad[1], r.max.y-pad[2]);
	/* on the heap: the UI thread's stack is small, and this recurses */
	k = emalloc(Maxkids*sizeof(Uxctl*));
	base = emalloc(Maxkids*sizeof(int));
	cross = emalloc(Maxkids*sizeof(int));
	n = kids(w, box, k, Maxkids);
	row = s->dir == Drow;
	cmain = row ? Dx(content) : Dy(content);
	ccross = row ? Dy(content) : Dx(content);

	sumgrow = sumshrink = nflow = 0;
	room = cmain;
	for(i = 0; i < n; i++){
		cs = sty(k[i]);
		if(cs->abs)
			continue;
		avail = ccross - (row ? cs->mar[0]+cs->mar[2] : cs->mar[1]+cs->mar[3]);
		/* cross size */
		cross[i] = len(row ? &cs->h : &cs->w, ccross);
		if(cross[i] < 0){
			if(s->align == Astretch)
				cross[i] = avail;
			else{
				q = natural(k[i], row ? -1 : avail);
				cross[i] = CROSS(q);
			}
		}
		cross[i] = row ? clamp(cross[i], &cs->minh, &cs->maxh, ccross)
			: clamp(cross[i], &cs->minw, &cs->maxw, ccross);
		/* main size */
		base[i] = len(row ? &cs->w : &cs->h, cmain);
		if(base[i] < 0){
			if(cs->basis0)
				base[i] = 0;
			else{
				q = natural(k[i], row ? -1 : cross[i]);
				base[i] = MAIN(q);
			}
		}
		room -= base[i] + (row ? cs->mar[1]+cs->mar[3] : cs->mar[0]+cs->mar[2]);
		if(nflow++ > 0)
			room -= s->gap;
		sumgrow += cs->grow;
		sumshrink += cs->shrink * base[i];
	}

	/* grow or shrink */
	for(i = 0; i < n; i++){
		cs = sty(k[i]);
		if(cs->abs)
			continue;
		if(room > 0 && sumgrow > 0)
			base[i] += room * cs->grow / sumgrow;
		else if(room < 0 && sumshrink > 0)
			base[i] += (vlong)room * cs->shrink * base[i] / sumshrink;
		/* a leaf doesn't shrink past what it needs, as CSS's min-width: auto */
		mn = len(row ? &cs->minw : &cs->minh, cmain);
		if(mn < 0 && !iscontainer(k[i]) && cs->shrink){
			q = natural(k[i], row ? -1 : cross[i]);
			mn = MAIN(q);
			if(base[i] < mn && !cs->basis0)
				base[i] = mn;
		}
		mx = len(row ? &cs->maxw : &cs->maxh, cmain);
		if(mx >= 0 && base[i] > mx)
			base[i] = mx;
		if(mn >= 0 && base[i] < mn && len(row ? &cs->minw : &cs->minh, cmain) >= 0)
			base[i] = mn;
		if(base[i] < 0)
			base[i] = 0;
	}

	/* what's left after growing goes to justify */
	extra = cmain;
	for(i = 0; i < n; i++){
		cs = sty(k[i]);
		if(cs->abs)
			continue;
		extra -= base[i] + (row ? cs->mar[1]+cs->mar[3] : cs->mar[0]+cs->mar[2]);
	}
	extra -= s->gap * (nflow > 0 ? nflow-1 : 0);
	pos = 0;
	between = 0;
	if(extra > 0)
		switch(s->justify){
		case Jcenter:	pos = extra/2; break;
		case Jend:	pos = extra; break;
		case Jbetween:	if(nflow > 1) between = extra/(nflow-1); break;
		}

	for(i = 0; i < n; i++){
		int m0, m1, c0, c1, cp;

		cs = sty(k[i]);
		if(cs->abs)
			continue;
		m0 = row ? cs->mar[3] : cs->mar[0];
		m1 = row ? cs->mar[1] : cs->mar[2];
		c0 = row ? cs->mar[0] : cs->mar[3];
		c1 = row ? cs->mar[2] : cs->mar[1];
		pos += m0;
		switch(s->align){
		case Acenter:	cp = c0 + (ccross - c0 - c1 - cross[i])/2; break;
		case Aend:	cp = ccross - c1 - cross[i]; break;
		default:	cp = c0; break;
		}
		if(row)
			cr = Rect(content.min.x+pos, content.min.y+cp, content.min.x+pos+base[i], content.min.y+cp+cross[i]);
		else
			cr = Rect(content.min.x+cp, content.min.y+pos, content.min.x+cp+cross[i], content.min.y+pos+base[i]);
		place(w, k[i], cr);
		pos += base[i] + m1 + s->gap + between;
	}
	cr = Rect(r.min.x+s->border, r.min.y+s->border, r.max.x-s->border, r.max.y-s->border);
	for(i = 0; i < n; i++)
		if(sty(k[i])->abs)
			placeabs(w, k[i], cr);
	free(k);
	free(base);
	free(cross);
}

/* lay out a window that has a root style: called before every redraw */
void
_flexlayout(Uxwin *w)
{
	Point sz;

	if(w->st == nil || w->img == nil)
		return;
	sz = subpt(w->img->r.max, w->img->r.min);
	layout(w, nil, w->st, Rect(0, 0, sz.x, sz.y));
}

/* the window root's natural size, for sizing a window to its content */
Point
_flexnatural(Uxwin *w, int availw)
{
	Uxctl **k;
	Uxstyle *s, *cs;
	Point p, q;
	int i, n, inner;

	s = w->st;
	k = emalloc(Maxkids*sizeof(Uxctl*));
	n = kids(w, nil, k, Maxkids);
	inner = availw - s->pad[1] - s->pad[3];
	p = ZP;
	for(i = 0; i < n; i++){
		cs = sty(k[i]);
		if(cs->abs)
			continue;
		q = natural(k[i], s->dir == Dcol ? inner : -1);
		q.x += cs->mar[1] + cs->mar[3];
		q.y += cs->mar[0] + cs->mar[2];
		if(s->dir == Drow){
			p.x += q.x + (i ? s->gap : 0);
			p.y = q.y > p.y ? q.y : p.y;
		}else{
			p.y += q.y + (i ? s->gap : 0);
			p.x = q.x > p.x ? q.x : p.x;
		}
	}
	free(k);
	return addpt(p, Pt(s->pad[1]+s->pad[3], s->pad[0]+s->pad[2]));
}

/*
 * The div type: a box that draws only its style's bg and border.
 */
static char *divfiles[] = { nil };
static Verb divverbs[] = { nil };
/* a press on a div, svg or styled-text node: activate, for a uitree host's on=click */
int
_spikemouse(Uxctl *c, Mouse *m)
{
	if((m->buttons & 7) && c->fn != nil)
		_activate(c, UXui);
	return 0;
}

Ctltype _divtype = { "div", divfiles, divverbs, nil, nil, nil, _spikemouse, nil, 0 };

void
_drawdiv(Uxctl *c, Rectangle r)
{
	Uxstyle *s;
	Uxwin *w;

	s = c->st;
	w = c->w;
	if(s == nil)
		return;
	if(s->hasbg)
		draw(w->img, r, _anycolor(w, s->bg), nil, ZP);
	if(s->border)
		border(w->img, r, s->border, _anycolor(w, s->hasbordercol ? s->bordercol : UXgrey3), ZP);
}

/* a colour of any value, cached per window */
Image*
_anycolor(Uxwin *w, ulong rgba)
{
	static struct {
		Uxwin	*w;
		ulong	rgba;
		Image	*i;
	} cache[64];
	static int ncache;
	int i;

	for(i = 0; i < ncache; i++)
		if(cache[i].w == w && cache[i].rgba == rgba)
			return cache[i].i;
	if(ncache == nelem(cache))
		return _color(w, rgba);
	cache[ncache].w = w;
	cache[ncache].rgba = rgba;
	cache[ncache].i = allocimage(w->d, Rect(0,0,1,1), w->img->chan, 1, rgba);
	return cache[ncache++].i;
}

/*
 * Fonts by size step (0 xs .. 5 3xl), weight and slant.
 * 9front ships lucidasans in these sizes, bold, and an italic.
 */
Font*
_font(Uxwin *w, int size, int bold, int italic)
{
	static struct {
		Display	*d;
		char	*name;
		Font	*f;
	} cache[32];
	static int ncache;
	static int sizes[] = { 6, 6, 7, 8, 10, 13 };
	char name[128];
	Font *f;
	int i;

	if(size < 0)
		size = 2;
	if(size > 5)
		size = 5;
	if(italic)
		snprint(name, sizeof name, "/lib/font/bit/lucidasans/italicunicode.%d.font", sizes[size]);
	else
		snprint(name, sizeof name, "/lib/font/bit/lucidasans/%sunicode.%d.font",
			bold ? "bold" : "", sizes[size]);
	for(i = 0; i < ncache; i++)
		if(cache[i].d == w->d && strcmp(cache[i].name, name) == 0)
			return cache[i].f;
	f = openfont(w->d, name);
	if(f == nil && italic)	/* no italic at this size: the regular face */
		return _font(w, size, bold, 0);
	if(f == nil)
		f = bold ? w->sysfont : w->font;
	if(ncache < nelem(cache)){
		cache[ncache].d = w->d;
		cache[ncache].name = estrdup(name);
		cache[ncache++].f = f;
	}
	return f;
}

/*
 * API
 */
Uxctl*
uxdiv(Uxwin *w, char *id)
{
	Uxctl *c;

	qlock(&uxlk);
	c = _newctl(w, id, &_divtype, "", ZR);
	qunlock(&uxlk);
	return c;
}

int
uxstyle(Uxctl *c, char *words)
{
	char *err;
	Uxstyle s;

	qlock(&uxlk);
	if(c->st != nil)
		s = *c->st;
	else
		_styledefault(&s);
	err = _parsestyle(&s, words);
	if(err == nil){
		if(c->st == nil)
			c->st = emalloc(sizeof *c->st);
		*c->st = s;
		c->w->dirty = 1;
	}else
		werrstr("%s", err);
	qunlock(&uxlk);
	return err == nil ? 0 : -1;
}

int
uxwinstyle(Uxwin *w, char *words)
{
	char *err;
	Uxstyle s;

	qlock(&uxlk);
	if(w->st != nil)
		s = *w->st;
	else
		_styledefault(&s);
	err = _parsestyle(&s, words);
	if(err == nil){
		if(w->st == nil)
			w->st = emalloc(sizeof *w->st);
		*w->st = s;
		w->dirty = 1;
	}else
		werrstr("%s", err);
	qunlock(&uxlk);
	return err == nil ? 0 : -1;
}

/* replace a control's or a window's style, rather than add to it */
int
uxsetstyle(Uxctl *c, char *words)
{
	char *err;
	Uxstyle s;

	qlock(&uxlk);
	_styledefault(&s);
	err = _parsestyle(&s, words);
	if(err == nil){
		if(c->st == nil)
			c->st = emalloc(sizeof *c->st);
		*c->st = s;
		c->w->dirty = 1;
	}else
		werrstr("%s", err);
	qunlock(&uxlk);
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
	return err == nil ? 0 : -1;
}

int
uxwinsetstyle(Uxwin *w, char *words)
{
	char *err;
	Uxstyle s;

	qlock(&uxlk);
	_styledefault(&s);
	err = _parsestyle(&s, words);
	if(err == nil){
		if(w->st == nil)
			w->st = emalloc(sizeof *w->st);
		*w->st = s;
		w->dirty = 1;
	}else
		werrstr("%s", err);
	qunlock(&uxlk);
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
	return err == nil ? 0 : -1;
}

/* where the last layout put a control, window-local */
Rectangle
uxctlrect(Uxctl *c)
{
	Rectangle r;

	qlock(&uxlk);
	r = c->dead ? ZR : _local(c);
	qunlock(&uxlk);
	return r;
}

/* run fn on the UI thread, soon */
void
uxcall(Uxfn *fn, void *arg)
{
	Uxev e;

	memset(&e, 0, sizeof e);
	qlock(&uxlk);
	_notify(fn, arg, &e);
	qunlock(&uxlk);
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
}
