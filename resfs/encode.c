/*
 * The text forms of docs/protocols/res.md back to resource bytes:
 * the inverse of text.c, used by rescomp. Errors name the body line
 * (counting from 1) in *errline, or 0 for the block as a whole.
 */
#include "rescompat.h"
#include "res.h"

enum {
	Maxtok = 64,
};

typedef struct Enc Enc;
struct Enc {
	Rbuf	*out;
	char	*err;
	int	nerr;
	int	*errline;
	int	line;
	char	*tok[Maxtok];
	int	ntok;
};

static int
fail(Enc *e, char *msg, char *detail)
{
	if(e->err != nil && e->nerr > 0){
		if(detail != nil)
			snprint(e->err, e->nerr, "%s: %s", msg, detail);
		else
			snprint(e->err, e->nerr, "%s", msg);
	}
	if(e->errline != nil)
		*e->errline = e->line;
	return -1;
}

static int
splitline(Enc *e, char *s)
{
	e->ntok = restokenize(s, e->tok, Maxtok);
	if(e->ntok < 0)
		return fail(e, "bad syntax", "unterminated quote or too many tokens");
	return 0;
}

static void
put16(Rbuf *b, int v)
{
	rbufc(b, v>>8);
	rbufc(b, v);
}

static void
put32(Rbuf *b, u32int v)
{
	put16(b, v>>16);
	put16(b, v);
}

/* a decimal integer in [lo, hi] */
static int
parseint(char *s, vlong lo, vlong hi, vlong *v)
{
	vlong x;
	int neg;

	neg = 0;
	if(*s == '-'){
		neg = 1;
		s++;
	}
	if(*s < '0' || *s > '9')
		return -1;
	x = 0;
	for(; *s >= '0' && *s <= '9'; s++){
		x = x*10 + (*s - '0');
		if(x > 0xFFFFFFFFLL)
			return -1;
	}
	if(*s != 0)
		return -1;
	if(neg)
		x = -x;
	if(x < lo || x > hi)
		return -1;
	*v = x;
	return 0;
}

static int
intattr(Enc *e, char *key, vlong lo, vlong hi, vlong def, int need, vlong *v)
{
	char *s;

	s = resattr(e->tok, e->ntok, key);
	if(s == nil){
		if(need)
			return fail(e, "bad syntax: missing attribute", key);
		*v = def;
		return 0;
	}
	if(parseint(s, lo, hi, v) < 0)
		return fail(e, "bad value", key);
	return 0;
}

static int
boolattr(Enc *e, char *key, int def, int *v)
{
	char *s;

	s = resattr(e->tok, e->ntok, key);
	if(s == nil)
		*v = def;
	else if(strcmp(s, "on") == 0)
		*v = 1;
	else if(strcmp(s, "off") == 0)
		*v = 0;
	else
		return fail(e, "bad value", key);
	return 0;
}

/* the attribute's text as MacRoman bytes in b (not NUL-terminated) */
static int
textattr(Enc *e, char *key, int need, Rbuf *b)
{
	char *s;

	s = resattr(e->tok, e->ntok, key);
	if(s == nil){
		if(need)
			return fail(e, "bad syntax: missing attribute", key);
		return 0;
	}
	if(resunesc(s) < 0)
		return fail(e, "bad value: bad escape in", key);
	if(utf2macroman(b, s, 1) < 0)
		return fail(e, "bad value: not representable in MacRoman", key);
	return 0;
}

static int
pstr(Enc *e, Rbuf *t)
{
	if(t->err)
		return fail(e, "out of memory", nil);
	if(t->n > 255)
		return fail(e, "too large: string over 255 bytes", nil);
	rbufc(e->out, t->n);
	rbufput(e->out, t->p, t->n);
	return 0;
}

static int
pstrattr(Enc *e, char *key, int need)
{
	Rbuf t;
	int r;

	rbufinit(&t);
	r = textattr(e, key, need, &t);
	if(r == 0)
		r = pstr(e, &t);
	rbuffree(&t);
	return r;
}

/* one character attribute as a MacRoman byte, 0 when absent */
static int
charattr(Enc *e, char *key, int *c)
{
	Rbuf t;

	*c = 0;
	rbufinit(&t);
	if(textattr(e, key, 0, &t) < 0){
		rbuffree(&t);
		return -1;
	}
	if(t.n > 1){
		rbuffree(&t);
		return fail(e, "bad value: more than one character", key);
	}
	if(t.n == 1)
		*c = t.p[0];
	rbuffree(&t);
	return 0;
}

/* rect=l,t,r,b, written as top, left, bottom, right */
static int
rectattr(Enc *e)
{
	char *s, *p;
	vlong v[4];
	int i;

	s = resattr(e->tok, e->ntok, "rect");
	if(s == nil)
		return fail(e, "bad syntax: missing attribute", "rect");
	for(i = 0; i < 4; i++){
		p = strchr(s, ',');
		if((p == nil) != (i == 3))
			return fail(e, "bad value", "rect");
		if(p != nil)
			*p = 0;
		if(parseint(s, -32768, 32767, &v[i]) < 0)
			return fail(e, "bad value", "rect");
		if(p != nil){
			*p = ',';
			s = p+1;
		}
	}
	put16(e->out, v[1]);
	put16(e->out, v[0]);
	put16(e->out, v[3]);
	put16(e->out, v[2]);
	return 0;
}

static int
verb(Enc *e, char *want)
{
	if(e->ntok == 0 || strcmp(e->tok[0], want) != 0)
		return fail(e, "bad syntax: expected", want);
	return 0;
}

static int
encstr(Enc *e, char **lines, int n)
{
	Rbuf t;
	int r;

	if(n != 1)
		return fail(e, "bad syntax: STR takes exactly one line", nil);
	e->line = 1;
	if(resunesc(lines[0]) < 0)
		return fail(e, "bad value: bad escape", nil);
	rbufinit(&t);
	if(utf2macroman(&t, lines[0], 1) < 0){
		rbuffree(&t);
		return fail(e, "bad value: not representable in MacRoman", nil);
	}
	r = pstr(e, &t);
	rbuffree(&t);
	return r;
}

static int
encstrlist(Enc *e, char **lines, int n)
{
	int i;

	if(n > 65535)
		return fail(e, "too large: more than 65535 strings", nil);
	put16(e->out, n);
	for(i = 0; i < n; i++){
		e->line = i+1;
		if(encstr(e, &lines[i], 1) < 0){
			e->line = i+1;
			if(e->errline != nil)
				*e->errline = i+1;
			return -1;
		}
	}
	return 0;
}

static int
stylebits(Enc *e, int *bits)
{
	static char *names[] = {
		"bold", "italic", "underline", "outline",
		"shadow", "condense", "extend",
	};
	char *s, *p;
	int i;

	*bits = 0;
	s = resattr(e->tok, e->ntok, "style");
	if(s == nil || *s == 0)
		return 0;
	for(;;){
		p = strchr(s, ',');
		if(p != nil)
			*p = 0;
		for(i = 0; i < 7; i++)
			if(strcmp(s, names[i]) == 0)
				break;
		if(i < 7)
			*bits |= 1<<i;
		else if(strcmp(s, "128") == 0)
			*bits |= 0x80;
		else
			return fail(e, "bad value", "style");
		if(p == nil)
			break;
		*p = ',';
		s = p+1;
	}
	return 0;
}

static int
encmenu(Enc *e, char **lines, int n)
{
	vlong id, proc, v;
	int i, on, icon, key, mark, style, enoff, off;
	u32int enable;
	Rbuf *b;

	b = e->out;
	if(n < 1)
		return fail(e, "bad syntax: MENU needs a menu line", nil);
	e->line = 1;
	if(splitline(e, lines[0]) < 0 || verb(e, "menu") < 0)
		return -1;
	if(intattr(e, "id", -32768, 32767, 0, 1, &id) < 0
	|| intattr(e, "proc", -32768, 32767, 0, 0, &proc) < 0
	|| boolattr(e, "enabled", 1, &on) < 0)
		return -1;
	put16(b, id);
	put16(b, 0);
	put16(b, 0);
	put16(b, proc);
	put16(b, 0);
	enoff = b->n;
	put32(b, 0);
	if(pstrattr(e, "title", 1) < 0)
		return -1;
	/* items past the list are left enabled, as Rez does */
	enable = 0xFFFFFFFEUL | (on ? 1 : 0);
	for(i = 1; i < n; i++){
		e->line = i+1;
		if(splitline(e, lines[i]) < 0 || verb(e, "item") < 0)
			return -1;
		off = b->n;
		if(pstrattr(e, "title", 1) < 0)
			return -1;
		if(!b->err && b->p[off] == 0)
			return fail(e, "bad value: an item title can't be empty", nil);
		key = mark = 0;
		if(intattr(e, "icon", 0, 255, 0, 0, &v) < 0)
			return -1;
		icon = v;
		if(resattr(e->tok, e->ntok, "submenu") != nil){
			if(intattr(e, "submenu", 0, 255, 0, 1, &v) < 0)
				return -1;
			key = 0x1B;
			mark = v;
		}else{
			if(resattr(e->tok, e->ntok, "script") != nil){
				if(intattr(e, "script", 0, 255, 0, 1, &v) < 0)
					return -1;
				key = 0x1C;
				icon = v;
			}else if(charattr(e, "key", &key) < 0)
				return -1;
			if(charattr(e, "mark", &mark) < 0)
				return -1;
		}
		if(stylebits(e, &style) < 0 || boolattr(e, "enabled", 1, &on) < 0)
			return -1;
		if(!on && i < 32)
			enable &= ~(1UL << i);
		rbufc(b, icon);
		rbufc(b, key);
		rbufc(b, mark);
		rbufc(b, style);
	}
	rbufc(b, 0);
	if(b->err)
		return fail(e, "out of memory", nil);
	b->p[enoff] = enable>>24;
	b->p[enoff+1] = enable>>16;
	b->p[enoff+2] = enable>>8;
	b->p[enoff+3] = enable;
	return 0;
}

static int
oneline(Enc *e, char **lines, int n, char *v)
{
	if(n != 1)
		return fail(e, "bad syntax: expected one line", nil);
	e->line = 1;
	if(splitline(e, lines[0]) < 0 || verb(e, v) < 0)
		return -1;
	return 0;
}

static int
encdlog(Enc *e, char **lines, int n)
{
	vlong proc, refcon, ditl, pos;
	int vis, goaway;
	Rbuf *b;

	b = e->out;
	if(oneline(e, lines, n, "dlog") < 0 || rectattr(e) < 0)
		return -1;
	if(intattr(e, "proc", -32768, 32767, 0, 0, &proc) < 0
	|| boolattr(e, "visible", 1, &vis) < 0
	|| boolattr(e, "goaway", 0, &goaway) < 0
	|| intattr(e, "refcon", 0, 0xFFFFFFFFLL, 0, 0, &refcon) < 0
	|| intattr(e, "ditl", -32768, 32767, 0, 1, &ditl) < 0
	|| intattr(e, "position", 0, 65535, 0, 0, &pos) < 0)
		return -1;
	put16(b, proc);
	rbufc(b, vis);
	rbufc(b, 0);
	rbufc(b, goaway);
	rbufc(b, 0);
	put32(b, refcon);
	put16(b, ditl);
	if(pstrattr(e, "title", 0) < 0)
		return -1;
	if(resattr(e->tok, e->ntok, "title") == nil)
		rbufc(b, 0);
	if(resattr(e->tok, e->ntok, "position") != nil){
		if(b->n & 1)
			rbufc(b, 0);
		put16(b, pos);
	}
	return 0;
}

static int
encalrt(Enc *e, char **lines, int n)
{
	vlong ditl, stages, pos;

	if(oneline(e, lines, n, "alrt") < 0 || rectattr(e) < 0)
		return -1;
	if(intattr(e, "ditl", -32768, 32767, 0, 1, &ditl) < 0
	|| intattr(e, "stages", 0, 65535, 0, 0, &stages) < 0
	|| intattr(e, "position", 0, 65535, 0, 0, &pos) < 0)
		return -1;
	put16(e->out, ditl);
	put16(e->out, stages);
	if(resattr(e->tok, e->ntok, "position") != nil)
		put16(e->out, pos);
	return 0;
}

static int
itemtypenum(char *s)
{
	static struct { char *name; int t; } types[] = {
		{"user", 0}, {"help", 1}, {"button", 4}, {"checkbox", 5},
		{"radio", 6}, {"control", 7}, {"text", 8}, {"edit", 16},
		{"icon", 32}, {"picture", 64},
	};
	vlong v;
	int i;

	for(i = 0; i < (int)(sizeof types / sizeof types[0]); i++)
		if(strcmp(s, types[i].name) == 0)
			return types[i].t;
	if(parseint(s, 0, 127, &v) == 0)
		return v;
	return -1;
}

static int
encditl(Enc *e, char **lines, int n)
{
	int i, t, on, h, l, lenoff;
	vlong id;
	char *s;
	Rbuf *b, d;

	b = e->out;
	if(n < 1 || n > 65536)
		return fail(e, "bad syntax: DITL needs 1 to 65536 items", nil);
	put16(b, n-1);
	for(i = 0; i < n; i++){
		e->line = i+1;
		if(splitline(e, lines[i]) < 0 || verb(e, "item") < 0)
			return -1;
		s = resattr(e->tok, e->ntok, "type");
		if(s == nil || (t = itemtypenum(s)) < 0)
			return fail(e, "bad value", "type");
		if(boolattr(e, "enabled", 1, &on) < 0)
			return -1;
		put32(b, 0);
		if(rectattr(e) < 0)
			return -1;
		rbufc(b, t | (on ? 0 : 0x80));
		lenoff = b->n;
		rbufc(b, 0);
		rbufinit(&d);
		switch(t){
		case 4:
		case 5:
		case 6:
		case 8:
		case 16:
			if(textattr(e, "text", 0, &d) < 0){
				rbuffree(&d);
				return -1;
			}
			break;
		case 7:
		case 32:
		case 64:
			if(resattr(e->tok, e->ntok, "id") != nil){
				if(intattr(e, "id", -32768, 32767, 0, 1, &id) < 0){
					rbuffree(&d);
					return -1;
				}
				put16(&d, id);
				break;
			}
			/* fall through */
		default:
			s = resattr(e->tok, e->ntok, "data");
			for(; s != nil && *s; s += 2){
				if((h = reshexval(s[0])) < 0 || (l = reshexval(s[1])) < 0){
					rbuffree(&d);
					return fail(e, "bad value", "data");
				}
				rbufc(&d, h<<4 | l);
			}
		}
		if(d.err || d.n > 255){
			rbuffree(&d);
			return fail(e, "too large: item data over 255 bytes", nil);
		}
		if(!b->err)
			b->p[lenoff] = d.n;
		rbufput(b, d.p, d.n);
		if(d.n & 1)
			rbufc(b, 0);
		rbuffree(&d);
	}
	return 0;
}

typedef struct Textenc Textenc;
struct Textenc {
	char	*type;
	int	(*enc)(Enc*, char**, int);
};

static Textenc textencs[] = {
	{"STR ", encstr},
	{"STR#", encstrlist},
	{"MENU", encmenu},
	{"DLOG", encdlog},
	{"DITL", encditl},
	{"ALRT", encalrt},
};

int
hasencoder(uchar type[4])
{
	int i;

	for(i = 0; i < (int)(sizeof textencs / sizeof textencs[0]); i++)
		if(memcmp(type, textencs[i].type, 4) == 0)
			return 1;
	return 0;
}

/*
 * Encode the text form in lines[0..n) as a resource of the given type.
 * The lines are modified.
 */
int
restextenc(uchar type[4], char **lines, int n, Rbuf *out, char *err, int nerr, int *errline)
{
	Enc e;
	int i;

	memset(&e, 0, sizeof e);
	e.out = out;
	e.err = err;
	e.nerr = nerr;
	e.errline = errline;
	if(errline != nil)
		*errline = 0;
	for(i = 0; i < (int)(sizeof textencs / sizeof textencs[0]); i++)
		if(memcmp(type, textencs[i].type, 4) == 0){
			if(textencs[i].enc(&e, lines, n) < 0)
				return -1;
			if(out->err)
				return fail(&e, "out of memory", nil);
			return 0;
		}
	return fail(&e, "no text form for this type", nil);
}
