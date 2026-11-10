/*
 * Text forms of STR , STR#, MENU, DLOG, DITL and ALRT, as described
 * in docs/protocols/res.md. Strings are MacRoman, converted to UTF-8
 * and escaped onto one line; the dialog and menu forms are lines of
 * key=value tokens that tokenize(2) splits.
 */
#include "rescompat.h"
#include "res.h"

typedef struct Rd Rd;
struct Rd {
	uchar	*p;
	u32int	n;
	u32int	off;
	int	err;	/* ran past the end */
};

static int
seterr(char *err, int nerr, char *msg)
{
	if(err != nil && nerr > 0){
		strncpy(err, msg, nerr-1);
		err[nerr-1] = 0;
	}
	return -1;
}

static int
need(Rd *r, u32int n)
{
	if(r->err || r->off > r->n || n > r->n - r->off){
		r->err = 1;
		return 0;
	}
	return 1;
}

static int
rd8(Rd *r)
{
	if(!need(r, 1))
		return 0;
	return r->p[r->off++];
}

static int
rd16(Rd *r)
{
	int v;

	if(!need(r, 2))
		return 0;
	v = (r->p[r->off]<<8) | r->p[r->off+1];
	r->off += 2;
	return v;
}

static int
rds16(Rd *r)
{
	return (short)rd16(r);
}

static u32int
rd32(Rd *r)
{
	u32int v;

	v = (u32int)rd16(r) << 16;
	return v | rd16(r);
}

static void
align2(Rd *r)
{
	if(r->off & 1)
		r->off++;
}

/*
 * MacRoman text with the system font's control glyphs:
 * 0x11 command, 0x12 check, 0x13 diamond, 0x14 apple.
 */
static int
textrune(int c)
{
	switch(c){
	case 0x11:
		return 0x2318;
	case 0x12:
		return 0x2713;
	case 0x13:
		return 0x25C6;
	case 0x14:
		return 0xF8FF;
	case 0:
		return 0xFFFD;
	}
	return macromanrune(c);
}

static char*
textstr(uchar *s, u32int n)
{
	Rbuf b;
	u32int i;

	rbufinit(&b);
	for(i = 0; i < n; i++)
		rbufrune(&b, textrune(s[i]));
	rbufc(&b, 0);
	if(b.err){
		rbuffree(&b);
		return nil;
	}
	return (char*)b.p;
}

/* a Pascal string; nil on truncation or no memory (r->err says which) */
static char*
rdpstr(Rd *r)
{
	int len;
	char *s;

	len = rd8(r);
	if(r->err || !need(r, len))
		return nil;
	s = textstr(r->p + r->off, len);
	r->off += len;
	return s;
}

static void
kvrect(Rbuf *b, Rd *r)
{
	int t, l, bo, ri;

	t = rds16(r);
	l = rds16(r);
	bo = rds16(r);
	ri = rds16(r);
	rbufs(b, " rect=");
	rbufint(b, l);
	rbufc(b, ',');
	rbufint(b, t);
	rbufc(b, ',');
	rbufint(b, ri);
	rbufc(b, ',');
	rbufint(b, bo);
}

static void
kvhex(Rbuf *b, char *key, u32int v, int ndig)
{
	rbufc(b, ' ');
	rbufs(b, key);
	rbufs(b, "=0x");
	rbufhex(b, v, ndig);
}

static void
kvchar(Rbuf *b, char *key, int c)
{
	char s[8];
	Rbuf t;

	rbufinit(&t);
	rbufrune(&t, textrune(c));
	if(t.err || t.n >= sizeof s){
		b->err = 1;
		rbuffree(&t);
		return;
	}
	memmove(s, t.p, t.n);
	s[t.n] = 0;
	rbuffree(&t);
	rbufkv(b, key, s);
}

static int
strdec(Rd *r, Rbuf *b)
{
	char *s;

	s = rdpstr(r);
	if(s == nil)
		return -1;
	rbufesc(b, s);
	rbufc(b, '\n');
	free(s);
	return 0;
}

static int
decstr(Rd *r, Rbuf *b)
{
	return strdec(r, b);
}

static int
decstrlist(Rd *r, Rbuf *b)
{
	int i, n;

	n = rd16(r);
	for(i = 0; i < n; i++)
		if(strdec(r, b) < 0)
			return -1;
	return r->err ? -1 : 0;
}

static int
decmenu(Rd *r, Rbuf *b)
{
	static char *styles[] = {
		"bold", "italic", "underline", "outline",
		"shadow", "condense", "extend",
	};
	int id, proc, i, item, icon, key, mark, style, first;
	u32int enable;
	char *s;

	id = rds16(r);
	rd16(r);	/* width, filled in at run time */
	rd16(r);	/* height */
	proc = rds16(r);
	rd16(r);	/* filler */
	enable = rd32(r);
	s = rdpstr(r);
	if(s == nil)
		return -1;
	rbufs(b, "menu");
	rbufkvint(b, "id", id);
	rbufkv(b, "title", s);
	free(s);
	rbufkvint(b, "proc", proc);
	rbufkvint(b, "enabled", enable & 1);
	rbufc(b, '\n');
	for(item = 1; r->off < r->n && r->p[r->off] != 0; item++){
		s = rdpstr(r);
		if(s == nil)
			return -1;
		icon = rd8(r);
		key = rd8(r);
		mark = rd8(r);
		style = rd8(r);
		if(r->err){
			free(s);
			return -1;
		}
		rbufs(b, "item");
		rbufkv(b, "title", s);
		free(s);
		if(key == 0x1B)
			rbufkvint(b, "submenu", mark);
		else{
			if(key == 0x1C)
				rbufkvint(b, "script", icon);
			else if(key != 0)
				kvchar(b, "key", key);
			if(mark != 0)
				kvchar(b, "mark", mark);
		}
		if(icon != 0 && key != 0x1C)
			rbufkvint(b, "icon", icon);
		if(style != 0){
			rbufs(b, " style=");
			first = 1;
			for(i = 0; i < 8; i++){
				if((style & (1<<i)) == 0)
					continue;
				if(!first)
					rbufc(b, ',');
				first = 0;
				if(i < 7)
					rbufs(b, styles[i]);
				else
					rbufs(b, "128");
			}
		}
		rbufkvint(b, "enabled", item > 31 || (enable & (1UL<<item)) != 0);
		rbufc(b, '\n');
	}
	return r->err ? -1 : 0;
}

static int
decdlog(Rd *r, Rbuf *b)
{
	int proc, vis, goaway, ditl;
	u32int refcon;
	char *s;

	rbufs(b, "dlog");
	kvrect(b, r);
	proc = rds16(r);
	vis = rd8(r);
	rd8(r);
	goaway = rd8(r);
	rd8(r);
	refcon = rd32(r);
	ditl = rds16(r);
	s = rdpstr(r);
	if(s == nil)
		return -1;
	rbufkvint(b, "proc", proc);
	rbufkvint(b, "visible", vis != 0);
	rbufkvint(b, "goaway", goaway != 0);
	kvhex(b, "refcon", refcon, 8);
	rbufkvint(b, "ditl", ditl);
	rbufkv(b, "title", s);
	free(s);
	align2(r);
	if(r->off <= r->n && r->n - r->off >= 2)
		kvhex(b, "position", rd16(r), 4);
	rbufc(b, '\n');
	return r->err ? -1 : 0;
}

static int
decalrt(Rd *r, Rbuf *b)
{
	int ditl, stages;

	rbufs(b, "alrt");
	kvrect(b, r);
	ditl = rds16(r);
	stages = rd16(r);
	if(r->err)
		return -1;
	rbufkvint(b, "ditl", ditl);
	kvhex(b, "stages", stages, 4);
	if(r->n - r->off >= 2)
		kvhex(b, "position", rd16(r), 4);
	rbufc(b, '\n');
	return r->err ? -1 : 0;
}

static char*
itemtype(int t)
{
	switch(t){
	case 0:
		return "user";
	case 1:
		return "help";
	case 4:
		return "button";
	case 5:
		return "checkbox";
	case 6:
		return "radio";
	case 7:
		return "control";
	case 8:
		return "text";
	case 16:
		return "edit";
	case 32:
		return "icon";
	case 64:
		return "picture";
	}
	return nil;
}

static int
decditl(Rd *r, Rbuf *b)
{
	int i, n, type, t, len;
	u32int j;
	uchar *d;
	char *s, *name;

	n = (rd16(r) + 1) & 0xFFFF;
	for(i = 0; i < n; i++){
		rd32(r);	/* handle placeholder */
		rbufs(b, "item");
		rbufkvint(b, "n", i+1);
		/* rect comes before type in the data; emit type first */
		if(!need(r, 10))
			return -1;
		type = r->p[r->off+8];
		t = type & 0x7F;
		name = itemtype(t);
		if(name != nil)
			rbufkv(b, "type", name);
		else
			rbufkvint(b, "type", t);
		kvrect(b, r);
		rd8(r);
		len = rd8(r);
		if(!need(r, len))
			return -1;
		d = r->p + r->off;
		rbufkvint(b, "enabled", (type & 0x80) == 0);
		switch(t){
		case 4:
		case 5:
		case 6:
		case 8:
		case 16:
			s = textstr(d, len);
			if(s == nil){
				b->err = 1;
				return -1;
			}
			rbufkv(b, "text", s);
			free(s);
			break;
		case 7:
		case 32:
		case 64:
			if(len >= 2){
				rbufkvint(b, "id", (short)((d[0]<<8) | d[1]));
				break;
			}
			/* fall through */
		default:
			if(len > 0){
				rbufs(b, " data=");
				for(j = 0; j < (u32int)len; j++)
					rbufhex(b, d[j], 2);
			}
		}
		rbufc(b, '\n');
		r->off += len;
		align2(r);
	}
	return r->err ? -1 : 0;
}

typedef struct Textdec Textdec;
struct Textdec {
	char	*type;
	int	(*dec)(Rd*, Rbuf*);
};

static Textdec textdecs[] = {
	{"STR ", decstr},
	{"STR#", decstrlist},
	{"MENU", decmenu},
	{"DLOG", decdlog},
	{"DITL", decditl},
	{"ALRT", decalrt},
};

static Textdec*
textdec(Res *r)
{
	int i;

	for(i = 0; i < (int)(sizeof textdecs / sizeof textdecs[0]); i++)
		if(restypeeq(r, textdecs[i].type))
			return &textdecs[i];
	return nil;
}

int
istexttype(Res *r)
{
	return textdec(r) != nil;
}

int
restext(Res *res, Rbuf *out, char *err, int nerr)
{
	Textdec *t;
	Rd r;

	t = textdec(res);
	if(t == nil)
		return seterr(err, nerr, "no text form for this type");
	r.p = res->data;
	r.n = res->len;
	r.off = 0;
	r.err = 0;
	if(t->dec(&r, out) < 0 || r.err){
		if(out->err)
			return seterr(err, nerr, "out of memory");
		return seterr(err, nerr, "resource data truncated");
	}
	if(out->err)
		return seterr(err, nerr, "out of memory");
	return 0;
}
