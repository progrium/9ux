/*
 * Spike: uitree's svg element on libdraw.
 *
 * A small subset of SVG, enough for icon sets and simple logos:
 * <svg> with viewBox, width and height; <g>, <path> (every command,
 * arcs included), <rect>, <circle>, <ellipse>, <polygon>, <polyline>
 * and <line>; fill, stroke and stroke-width as attributes, inherited
 * through <g> and <svg>; fill-rule; currentColor (the node's
 * text-color, else ink). Not handled: transforms, gradients, text,
 * <use>, CSS, dashes, clip paths, opacity. Shapes are flattened to
 * polygons and drawn with a 4x4 supersampled coverage mask, so edges
 * are antialiased; strokes are drawn as round-jointed segments.
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

typedef struct Pt2 Pt2;
typedef struct Edge Edge;
typedef struct Shape Shape;
typedef struct Svg Svg;
typedef struct Paint Paint;

struct Pt2 {
	double	x, y;
};

struct Edge {
	double	x0, y0, x1, y1;
};

struct Paint {
	int	fill;	/* 0 none, 1 colour, 2 currentColor */
	ulong	fillc;
	int	stroke;
	ulong	strokec;
	double	sw;
	int	evenodd;
};

/* one filled or stroked shape: closed polygons (subpaths) */
struct Shape {
	Pt2	**sub;
	int	*nsub;
	int	*closed;
	int	n;
	Paint	p;
	Shape	*next;
};

struct Svg {
	double	vx, vy, vw, vh;
	double	w, h;
	Shape	*shapes, **tail;
	/* building a subpath */
	Pt2	*cur;
	int	ncur;
	int	curclosed;
	Shape	*s;
};

enum {
	SS	= 4,	/* supersampling per axis */
	Curveseg	= 16,
};

/*
 * Building shapes
 */
static void
addpt2(Svg *g, double x, double y)
{
	if(g->ncur % 32 == 0)
		g->cur = erealloc(g->cur, (g->ncur+32)*sizeof(Pt2));
	g->cur[g->ncur].x = x;
	g->cur[g->ncur].y = y;
	g->ncur++;
}

static void
endsub(Svg *g)
{
	Shape *s;

	s = g->s;
	if(g->ncur >= 2){
		s->sub = erealloc(s->sub, (s->n+1)*sizeof(Pt2*));
		s->nsub = erealloc(s->nsub, (s->n+1)*sizeof(int));
		s->closed = erealloc(s->closed, (s->n+1)*sizeof(int));
		s->sub[s->n] = g->cur;
		s->nsub[s->n] = g->ncur;
		s->closed[s->n] = g->curclosed;
		s->n++;
	}else
		free(g->cur);
	g->cur = nil;
	g->ncur = 0;
	g->curclosed = 0;
}

static void
beginshape(Svg *g, Paint *p)
{
	Shape *s;

	s = emalloc(sizeof *s);
	s->p = *p;
	g->s = s;
	*g->tail = s;
	g->tail = &s->next;
}

static void
endshape(Svg *g)
{
	endsub(g);
	g->s = nil;
}

/*
 * Attributes
 */
static char*
attr(char *tag, char *name)
{
	static char buf[8192];
	char *p, *q, quote;
	int n;

	n = strlen(name);
	for(p = tag; (p = strstr(p, name)) != nil; p += n){
		if(p > tag && p[-1] != ' ' && p[-1] != '\t' && p[-1] != '\n' && p[-1] != '\r')
			continue;
		q = p + n;
		while(*q == ' ')
			q++;
		if(*q != '=')
			continue;
		q++;
		while(*q == ' ')
			q++;
		quote = *q;
		if(quote != '"' && quote != '\'')
			continue;
		q++;
		for(n = 0; *q && *q != quote && n < sizeof buf-1; n++)
			buf[n] = *q++;
		buf[n] = 0;
		return buf;
	}
	return nil;
}

static double
num(char *tag, char *name, double def)
{
	char *s;

	s = attr(tag, name);
	if(s == nil)
		return def;
	return strtod(s, nil);
}

static void
paintattrs(char *tag, Paint *p)
{
	char *s;

	if((s = attr(tag, "fill")) != nil){
		if(strcmp(s, "none") == 0)
			p->fill = 0;
		else if(strcmp(s, "currentColor") == 0)
			p->fill = 2;
		else if(_parsecolor(s, &p->fillc) == 0)
			p->fill = 1;
		else if(strcmp(s, "black") == 0){
			p->fill = 1;
			p->fillc = UXink;
		}else if(strcmp(s, "white") == 0){
			p->fill = 1;
			p->fillc = UXpaper;
		}else
			p->fill = 2;
	}
	if((s = attr(tag, "stroke")) != nil){
		if(strcmp(s, "none") == 0)
			p->stroke = 0;
		else if(strcmp(s, "currentColor") == 0)
			p->stroke = 2;
		else if(_parsecolor(s, &p->strokec) == 0)
			p->stroke = 1;
		else if(strcmp(s, "black") == 0){
			p->stroke = 1;
			p->strokec = UXink;
		}else
			p->stroke = 2;
	}
	if((s = attr(tag, "stroke-width")) != nil)
		p->sw = strtod(s, nil);
	if((s = attr(tag, "fill-rule")) != nil)
		p->evenodd = strcmp(s, "evenodd") == 0;
}

/*
 * Path data
 */
static int
pnum(char **sp, double *v)
{
	char *s, *e;

	s = *sp;
	while(*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	if(*s == 0 || (strchr("+-.0123456789", *s) == nil))
		return 0;
	*v = strtod(s, &e);
	if(e == s)
		return 0;
	*sp = e;
	return 1;
}

/* an arc flag: one digit, which may run into the next number */
static int
pflag(char **sp, double *v)
{
	char *s;

	s = *sp;
	while(*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r')
		s++;
	if(*s != '0' && *s != '1')
		return 0;
	*v = *s - '0';
	*sp = s+1;
	return 1;
}

static void
cubic(Svg *g, Pt2 p0, Pt2 p1, Pt2 p2, Pt2 p3)
{
	int i;
	double t, u;

	for(i = 1; i <= Curveseg; i++){
		t = (double)i/Curveseg;
		u = 1-t;
		addpt2(g, u*u*u*p0.x + 3*u*u*t*p1.x + 3*u*t*t*p2.x + t*t*t*p3.x,
			u*u*u*p0.y + 3*u*u*t*p1.y + 3*u*t*t*p2.y + t*t*t*p3.y);
	}
}

static void
quad(Svg *g, Pt2 p0, Pt2 p1, Pt2 p2)
{
	int i;
	double t, u;

	for(i = 1; i <= Curveseg; i++){
		t = (double)i/Curveseg;
		u = 1-t;
		addpt2(g, u*u*p0.x + 2*u*t*p1.x + t*t*p2.x, u*u*p0.y + 2*u*t*p1.y + t*t*p2.y);
	}
}

static double
vang(double ux, double uy, double vx, double vy)
{
	double a;

	a = atan2(ux*vy - uy*vx, ux*vx + uy*vy);
	return a;
}

/* SVG's endpoint arc, as in the spec's implementation notes (F.6.5) */
static void
svgarc(Svg *g, Pt2 p0, double rx, double ry, double phi, int large, int sweep, Pt2 p1)
{
	double cp, sp, x1, y1, l, sq, num, den, cx1, cy1, cx, cy, t1, dt, t;
	int i, n;

	if(rx == 0 || ry == 0){
		addpt2(g, p1.x, p1.y);
		return;
	}
	rx = fabs(rx);
	ry = fabs(ry);
	phi = phi * PI / 180;
	cp = cos(phi);
	sp = sin(phi);
	x1 = cp*(p0.x-p1.x)/2 + sp*(p0.y-p1.y)/2;
	y1 = -sp*(p0.x-p1.x)/2 + cp*(p0.y-p1.y)/2;
	l = x1*x1/(rx*rx) + y1*y1/(ry*ry);
	if(l > 1){
		rx *= sqrt(l);
		ry *= sqrt(l);
	}
	num = rx*rx*ry*ry - rx*rx*y1*y1 - ry*ry*x1*x1;
	den = rx*rx*y1*y1 + ry*ry*x1*x1;
	sq = den == 0 || num < 0 ? 0 : sqrt(num/den);
	if(large == sweep)
		sq = -sq;
	cx1 = sq*rx*y1/ry;
	cy1 = -sq*ry*x1/rx;
	cx = cp*cx1 - sp*cy1 + (p0.x+p1.x)/2;
	cy = sp*cx1 + cp*cy1 + (p0.y+p1.y)/2;
	t1 = vang(1, 0, (x1-cx1)/rx, (y1-cy1)/ry);
	dt = vang((x1-cx1)/rx, (y1-cy1)/ry, (-x1-cx1)/rx, (-y1-cy1)/ry);
	if(!sweep && dt > 0)
		dt -= 2*PI;
	else if(sweep && dt < 0)
		dt += 2*PI;
	n = fabs(dt) / (PI/16) + 1;
	for(i = 1; i <= n; i++){
		t = t1 + dt*i/n;
		addpt2(g, cx + rx*cos(t)*cp - ry*sin(t)*sp, cy + rx*cos(t)*sp + ry*sin(t)*cp);
	}
}

static void
pathdata(Svg *g, char *d)
{
	char cmd, *s;
	double a[7];
	Pt2 cur, start, ctl, p1, p2, p3;
	int rel, lastc, i, n;

	cur.x = cur.y = 0;
	start = cur;
	ctl = cur;
	lastc = 0;
	cmd = 0;
	s = d;
	for(;;){
		while(*s == ' ' || *s == ',' || *s == '\t' || *s == '\n' || *s == '\r')
			s++;
		if(*s == 0)
			break;
		if(strchr("MmLlHhVvCcSsQqTtAaZz", *s) != nil)
			cmd = *s++;
		else if(cmd == 0 || strchr("+-.0123456789", *s) == nil)
			break;
		rel = cmd >= 'a';
		switch(cmd){
		case 'Z': case 'z':
			g->curclosed = 1;
			endsub(g);
			cur = start;
			lastc = cmd;
			continue;
		case 'M': case 'm':
			if(!pnum(&s, &a[0]) || !pnum(&s, &a[1]))
				return;
			endsub(g);
			cur.x = rel ? cur.x + a[0] : a[0];
			cur.y = rel ? cur.y + a[1] : a[1];
			start = cur;
			addpt2(g, cur.x, cur.y);
			cmd = rel ? 'l' : 'L';	/* further pairs are lines */
			lastc = 'M';
			continue;
		}
		if(g->ncur == 0)
			addpt2(g, cur.x, cur.y);
		n = 0;
		switch(cmd){
		case 'L': case 'l': case 'T': case 't': n = 2; break;
		case 'H': case 'h': case 'V': case 'v': n = 1; break;
		case 'C': case 'c': n = 6; break;
		case 'S': case 's': case 'Q': case 'q': n = 4; break;
		case 'A': case 'a': n = 7; break;
		}
		for(i = 0; i < n; i++)
			if(((cmd == 'A' || cmd == 'a') && (i == 3 || i == 4)) ? !pflag(&s, &a[i]) : !pnum(&s, &a[i]))
				return;
		switch(cmd){
		case 'L': case 'l':
			cur.x = rel ? cur.x + a[0] : a[0];
			cur.y = rel ? cur.y + a[1] : a[1];
			addpt2(g, cur.x, cur.y);
			break;
		case 'H': case 'h':
			cur.x = rel ? cur.x + a[0] : a[0];
			addpt2(g, cur.x, cur.y);
			break;
		case 'V': case 'v':
			cur.y = rel ? cur.y + a[0] : a[0];
			addpt2(g, cur.x, cur.y);
			break;
		case 'C': case 'c':
		case 'S': case 's':
			if(cmd == 'C' || cmd == 'c'){
				p1.x = rel ? cur.x + a[0] : a[0];
				p1.y = rel ? cur.y + a[1] : a[1];
				i = 2;
			}else{
				if(strchr("CcSs", lastc) != nil){
					p1.x = 2*cur.x - ctl.x;
					p1.y = 2*cur.y - ctl.y;
				}else
					p1 = cur;
				i = 0;
			}
			p2.x = rel ? cur.x + a[i] : a[i];
			p2.y = rel ? cur.y + a[i+1] : a[i+1];
			p3.x = rel ? cur.x + a[i+2] : a[i+2];
			p3.y = rel ? cur.y + a[i+3] : a[i+3];
			cubic(g, cur, p1, p2, p3);
			ctl = p2;
			cur = p3;
			break;
		case 'Q': case 'q':
			p1.x = rel ? cur.x + a[0] : a[0];
			p1.y = rel ? cur.y + a[1] : a[1];
			p2.x = rel ? cur.x + a[2] : a[2];
			p2.y = rel ? cur.y + a[3] : a[3];
			quad(g, cur, p1, p2);
			ctl = p1;
			cur = p2;
			break;
		case 'T': case 't':
			if(strchr("QqTt", lastc) != nil){
				p1.x = 2*cur.x - ctl.x;
				p1.y = 2*cur.y - ctl.y;
			}else
				p1 = cur;
			p2.x = rel ? cur.x + a[0] : a[0];
			p2.y = rel ? cur.y + a[1] : a[1];
			quad(g, cur, p1, p2);
			ctl = p1;
			cur = p2;
			break;
		case 'A': case 'a':
			p1.x = rel ? cur.x + a[5] : a[5];
			p1.y = rel ? cur.y + a[6] : a[6];
			svgarc(g, cur, a[0], a[1], a[2], a[3] != 0, a[4] != 0, p1);
			cur = p1;
			break;
		}
		lastc = cmd;
	}
}

static void
pointsattr(Svg *g, char *pts, int closed)
{
	double x, y;

	while(pnum(&pts, &x) && pnum(&pts, &y))
		addpt2(g, x, y);
	g->curclosed = closed;
}

static void
svgellipse(Svg *g, double cx, double cy, double rx, double ry)
{
	int i, n;

	n = 48;
	for(i = 0; i < n; i++)
		addpt2(g, cx + rx*cos(2*PI*i/n), cy + ry*sin(2*PI*i/n));
	g->curclosed = 1;
}

static void
roundrect(Svg *g, double x, double y, double w, double h, double rx, double ry)
{
	int i, n;
	double cx[4], cy[4], a;

	if(rx <= 0 && ry <= 0){
		addpt2(g, x, y);
		addpt2(g, x+w, y);
		addpt2(g, x+w, y+h);
		addpt2(g, x, y+h);
		g->curclosed = 1;
		return;
	}
	if(rx <= 0) rx = ry;
	if(ry <= 0) ry = rx;
	if(rx > w/2) rx = w/2;
	if(ry > h/2) ry = h/2;
	cx[0] = x+w-rx; cy[0] = y+ry;	/* top right, then clockwise */
	cx[1] = x+w-rx; cy[1] = y+h-ry;
	cx[2] = x+rx; cy[2] = y+h-ry;
	cx[3] = x+rx; cy[3] = y+ry;
	n = 8;
	for(i = 0; i < 4; i++){
		int j;

		for(j = 0; j <= n; j++){
			a = -PI/2 + (i*PI/2) + (PI/2)*j/n;
			addpt2(g, cx[i] + rx*cos(a), cy[i] + ry*sin(a));
		}
	}
	g->curclosed = 1;
}

/* parse doc into shapes */
static Svg*
svgparse(char *doc)
{
	Svg *g;
	Paint stack[32], p;
	int depth, closing, self;
	char *s, *e, *tag, name[32];
	int n;

	g = emalloc(sizeof *g);
	g->tail = &g->shapes;
	g->vw = g->vh = 24;
	memset(&p, 0, sizeof p);
	p.fill = 2;	/* SVG's default fill is black: here, currentColor */
	p.sw = 1;
	depth = 0;
	stack[0] = p;
	for(s = doc; (s = strchr(s, '<')) != nil; s = e+1){
		e = strchr(s, '>');
		if(e == nil)
			break;
		if(s[1] == '!' || s[1] == '?')
			continue;
		closing = s[1] == '/';
		self = e[-1] == '/';
		tag = emalloc(e-s+1);
		memmove(tag, s+1, e-s-1);
		tag[e-s-1] = 0;
		for(n = 0; tag[n+closing] && !strchr(" \t\r\n/", tag[n+closing]) && n < sizeof name-1; n++)
			name[n] = tag[n+closing];
		name[n] = 0;
		if(closing){
			if((strcmp(name, "g") == 0 || strcmp(name, "svg") == 0) && depth > 0)
				p = stack[depth--];
			free(tag);
			continue;
		}
		if(strcmp(name, "svg") == 0 || strcmp(name, "g") == 0){
			if(strcmp(name, "svg") == 0){
				char *vb;

				g->w = num(tag, "width", 0);
				g->h = num(tag, "height", 0);
				if((vb = attr(tag, "viewBox")) != nil){
					char *q;

					q = vb;
					pnum(&q, &g->vx);
					pnum(&q, &g->vy);
					pnum(&q, &g->vw);
					pnum(&q, &g->vh);
				}else if(g->w > 0 && g->h > 0){
					g->vw = g->w;
					g->vh = g->h;
				}
			}
			if(!self){
				if(depth < nelem(stack)-1)
					stack[++depth] = p;
				paintattrs(tag, &p);
			}
			free(tag);
			continue;
		}
		{
			Paint q;

			q = p;
			paintattrs(tag, &q);
			if(q.fill == 0 && q.stroke == 0){
				free(tag);
				continue;
			}
			if(strcmp(name, "path") == 0){
				char *d;

				d = attr(tag, "d");
				if(d != nil){
					d = estrdup(d);
					beginshape(g, &q);
					pathdata(g, d);
					endshape(g);
					free(d);
				}
			}else if(strcmp(name, "rect") == 0){
				beginshape(g, &q);
				roundrect(g, num(tag, "x", 0), num(tag, "y", 0), num(tag, "width", 0), num(tag, "height", 0),
					num(tag, "rx", 0), num(tag, "ry", 0));
				endshape(g);
			}else if(strcmp(name, "circle") == 0){
				beginshape(g, &q);
				svgellipse(g, num(tag, "cx", 0), num(tag, "cy", 0), num(tag, "r", 0), num(tag, "r", 0));
				endshape(g);
			}else if(strcmp(name, "ellipse") == 0){
				beginshape(g, &q);
				svgellipse(g, num(tag, "cx", 0), num(tag, "cy", 0), num(tag, "rx", 0), num(tag, "ry", 0));
				endshape(g);
			}else if(strcmp(name, "polygon") == 0 || strcmp(name, "polyline") == 0){
				char *pts;

				pts = attr(tag, "points");
				if(pts != nil){
					pts = estrdup(pts);
					beginshape(g, &q);
					pointsattr(g, pts, name[4] == 'g');
					endshape(g);
					free(pts);
				}
			}else if(strcmp(name, "line") == 0){
				q.fill = 0;
				if(q.stroke){
					beginshape(g, &q);
					addpt2(g, num(tag, "x1", 0), num(tag, "y1", 0));
					addpt2(g, num(tag, "x2", 0), num(tag, "y2", 0));
					endshape(g);
				}
			}
		}
		free(tag);
	}
	return g;
}

static void
svgfree(Svg *g)
{
	Shape *s, *n;
	int i;

	for(s = g->shapes; s != nil; s = n){
		n = s->next;
		for(i = 0; i < s->n; i++)
			free(s->sub[i]);
		free(s->sub);
		free(s->nsub);
		free(s->closed);
		free(s);
	}
	free(g->cur);
	free(g);
}

/*
 * Rasterizing: edges in pixel space into an 8-bit coverage mask.
 */
typedef struct Edges Edges;
struct Edges {
	Edge	*e;
	int	n;
};

static void
edge(Edges *es, double x0, double y0, double x1, double y1)
{
	if(y0 == y1)
		return;
	if(es->n % 64 == 0)
		es->e = erealloc(es->e, (es->n+64)*sizeof(Edge));
	es->e[es->n].x0 = x0;
	es->e[es->n].y0 = y0;
	es->e[es->n].x1 = x1;
	es->e[es->n].y1 = y1;
	es->n++;
}

/* a closed polygon, oriented so its signed area is positive when pos is set */
static void
polyedges(Edges *es, Pt2 *p, int n, int orient)
{
	int i;
	double a;

	a = 0;
	for(i = 0; i < n; i++)
		a += p[i].x*p[(i+1)%n].y - p[(i+1)%n].x*p[i].y;
	if(orient && a < 0){
		for(i = 0; i < n; i++)
			edge(es, p[(i+1)%n].x, p[(i+1)%n].y, p[i].x, p[i].y);
		return;
	}
	for(i = 0; i < n; i++)
		edge(es, p[i].x, p[i].y, p[(i+1)%n].x, p[(i+1)%n].y);
}

/* a stroke: each segment as a quad, each joint and end as a disc, all one way round */
static void
strokepoly(Edges *es, Pt2 *p, int n, int closed, double hw)
{
	int i, j, m;
	Pt2 q[4], c[16];
	double dx, dy, l;

	m = closed ? n : n-1;
	for(i = 0; i < m; i++){
		Pt2 a, b;

		a = p[i];
		b = p[(i+1)%n];
		dx = b.x - a.x;
		dy = b.y - a.y;
		l = sqrt(dx*dx + dy*dy);
		if(l == 0)
			continue;
		dx = dx/l*hw;
		dy = dy/l*hw;
		q[0].x = a.x - dy; q[0].y = a.y + dx;
		q[1].x = b.x - dy; q[1].y = b.y + dx;
		q[2].x = b.x + dy; q[2].y = b.y - dx;
		q[3].x = a.x + dy; q[3].y = a.y - dx;
		polyedges(es, q, 4, 1);
	}
	if(hw < 0.75)	/* hairlines: no discs */
		return;
	for(i = 0; i < n; i++){
		for(j = 0; j < nelem(c); j++){
			c[j].x = p[i].x + hw*cos(2*PI*j/nelem(c));
			c[j].y = p[i].y + hw*sin(2*PI*j/nelem(c));
		}
		polyedges(es, c, nelem(c), 1);
	}
}

static int
dcmp(void *a, void *b)
{
	double x, y;

	x = ((double*)a)[0];
	y = ((double*)b)[0];
	return x < y ? -1 : x > y;
}

/* coverage of es in a w by h mask */
static uchar*
raster(Edges *es, int w, int h, int evenodd)
{
	float *acc;
	double *x, y, t;
	uchar *m;
	int i, j, row, nx, wind, k, x0i, x1i, px;
	double xa, xb, from;
	Edge *e;

	acc = emalloc(w*h*sizeof(float));
	x = emalloc(2*(es->n+1)*sizeof(double));
	for(row = 0; row < h*SS; row++){
		y = (row + 0.5) / SS;
		nx = 0;
		for(i = 0; i < es->n; i++){
			e = &es->e[i];
			if((y >= e->y0 && y < e->y1) || (y >= e->y1 && y < e->y0)){
				t = (y - e->y0) / (e->y1 - e->y0);
				x[2*nx] = e->x0 + t*(e->x1 - e->x0);
				x[2*nx+1] = e->y1 > e->y0 ? 1 : -1;
				nx++;
			}
		}
		if(nx < 2)
			continue;
		qsort(x, nx, 2*sizeof(double), dcmp);
		wind = 0;
		from = 0;
		for(k = 0; k < nx; k++){
			int was;

			was = evenodd ? (wind & 1) : wind != 0;
			if(!was)
				from = x[2*k];
			wind += (int)x[2*k+1];
			if(was && !(evenodd ? (wind & 1) : wind != 0)){
				/* span from..x[2k] is inside: add its horizontal coverage */
				xa = from < 0 ? 0 : from;
				xb = x[2*k] > w ? w : x[2*k];
				if(xb <= xa)
					continue;
				x0i = xa;
				x1i = xb;
				j = (row/SS)*w;
				if(x0i == x1i){
					acc[j+x0i] += (xb - xa) / SS;
					continue;
				}
				acc[j+x0i] += (x0i + 1 - xa) / SS;
				for(px = x0i+1; px < x1i && px < w; px++)
					acc[j+px] += 1.0 / SS;
				if(x1i < w)
					acc[j+x1i] += (xb - x1i) / SS;
			}
		}
	}
	m = emalloc(w*h);
	for(i = 0; i < w*h; i++){
		t = acc[i];
		m[i] = t >= 1 ? 255 : t <= 0 ? 0 : t*255 + 0.5;
	}
	free(acc);
	free(x);
	return m;
}

/* draw a parsed svg into r of dst, currentColor being fg */
static void
svgdraw(Uxwin *w, Image *dst, Svg *g, Rectangle r, ulong fg)
{
	Shape *s;
	Edges es;
	double sc, ox, oy;
	int i, j, pass, W, H;
	Pt2 *p;
	uchar *m;
	Image *mask;
	ulong col;

	W = Dx(r);
	H = Dy(r);
	if(W <= 0 || H <= 0 || g->vw <= 0 || g->vh <= 0)
		return;
	sc = (double)W / g->vw;
	if((double)H / g->vh < sc)
		sc = (double)H / g->vh;
	ox = (W - g->vw*sc)/2 - g->vx*sc;
	oy = (H - g->vh*sc)/2 - g->vy*sc;
	mask = allocimage(w->d, Rect(0, 0, W, H), GREY8, 0, DBlack);
	if(mask == nil)
		return;
	for(s = g->shapes; s != nil; s = s->next)
		for(pass = 0; pass < 2; pass++){
			if(pass == 0 && !s->p.fill || pass == 1 && !s->p.stroke)
				continue;
			memset(&es, 0, sizeof es);
			for(i = 0; i < s->n; i++){
				p = emalloc(s->nsub[i]*sizeof(Pt2));
				for(j = 0; j < s->nsub[i]; j++){
					p[j].x = s->sub[i][j].x*sc + ox;
					p[j].y = s->sub[i][j].y*sc + oy;
				}
				if(pass == 0)
					polyedges(&es, p, s->nsub[i], 0);
				else
					strokepoly(&es, p, s->nsub[i], s->closed[i], s->p.sw*sc/2);
				free(p);
			}
			if(es.n == 0)
				continue;
			m = raster(&es, W, H, pass == 0 && s->p.evenodd);
			loadimage(mask, mask->r, m, W*H);
			free(m);
			free(es.e);
			if(pass == 0)
				col = s->p.fill == 2 ? fg : s->p.fillc;
			else
				col = s->p.stroke == 2 ? fg : s->p.strokec;
			draw(dst, r, _anycolor(w, col), mask, ZP);
		}
	freeimage(mask);
}

/*
 * The svg type
 */
static char *svgfiles[] = { "src", nil };
static Verb svgverbs[] = { nil };
Ctltype _svgtype = { "svg", svgfiles, svgverbs, nil, nil, nil, _spikemouse, nil, 0 };

static Svg*
svgof(Uxctl *c)
{
	if(c->svg == nil && c->svgsrc != nil)
		c->svg = svgparse(c->svgsrc);
	return c->svg;
}

Point
_svgsize(Uxctl *c)
{
	Svg *g;

	g = svgof(c);
	if(g == nil)
		return Pt(24, 24);
	if(g->w > 0 && g->h > 0)
		return Pt(g->w, g->h);
	return Pt(g->vw, g->vh);
}

void
_drawsvg(Uxctl *c, Rectangle r)
{
	Svg *g;
	ulong fg;

	g = svgof(c);
	if(g == nil)
		return;
	fg = c->st != nil && c->st->hasfg ? c->st->fg : UXcoltext;
	if(!(c->state & Cenabled))
		fg = UXcoldisabled;
	/* rendered once per size and colour, then composited */
	if(c->svgimg == nil || Dx(c->svgimg->r) != Dx(r) || Dy(c->svgimg->r) != Dy(r) || c->svgfg != fg){
		if(c->svgimg != nil)
			freeimage(c->svgimg);
		c->svgimg = allocimage(c->w->d, Rect(0, 0, Dx(r), Dy(r)), ARGB32, 0, DTransparent);
		if(c->svgimg == nil)
			return;
		c->svgfg = fg;
		svgdraw(c->w, c->svgimg, g, c->svgimg->r, fg);
	}
	draw(c->w->img, r, c->svgimg, nil, ZP);
}

void
_svgfree(Uxctl *c)
{
	if(c->svg != nil)
		svgfree(c->svg);
	c->svg = nil;
	if(c->svgimg != nil)
		freeimage(c->svgimg);
	c->svgimg = nil;
}

static char*
readall(char *file)
{
	int fd;
	long n, tot;
	char *buf;

	fd = open(file, OREAD);
	if(fd < 0)
		return nil;
	buf = nil;
	tot = 0;
	for(;;){
		buf = erealloc(buf, tot+8192+1);
		n = read(fd, buf+tot, 8192);
		if(n <= 0)
			break;
		tot += n;
	}
	close(fd);
	buf[tot] = 0;
	return buf;
}

/* src is SVG text if it starts with <, else a file name */
Uxctl*
uxsvg(Uxwin *w, char *id, char *src)
{
	Uxctl *c;
	char *doc;

	doc = src[0] == '<' ? estrdup(src) : readall(src);
	qlock(&uxlk);
	c = _newctl(w, id, &_svgtype, "", ZR);
	c->svgsrc = doc;
	qunlock(&uxlk);
	return c;
}

void
uxsetsvg(Uxctl *c, char *src)
{
	char *doc;

	doc = src[0] == '<' ? estrdup(src) : readall(src);
	qlock(&uxlk);
	free(c->svgsrc);
	c->svgsrc = doc;
	_svgfree(c);
	c->w->dirty = 1;
	qunlock(&uxlk);
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
}
