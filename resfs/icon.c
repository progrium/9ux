/*
 * Icons and cursors, decoded to 9front image(6) files with channel
 * a8r8g8b8: uncompressed, 4 bytes a pixel stored B G R A, alpha
 * premultiplied (masks are 1-bit, so a pixel is opaque or all zero).
 */
#include "rescompat.h"
#include "res.h"

/* The classic 16-colour palette (clut 4). */
u32int macpal4[16] = {
	0xFFFFFF, 0xFCF305, 0xFF6402, 0xDD0806, 0xF20884, 0x4600A5, 0x0000D4, 0x02ABEA,
	0x1FB714, 0x006411, 0x562C05, 0x90713A, 0xC0C0C0, 0x808080, 0x404040, 0x000000,
};

/* The classic 256-colour system palette (clut 8). */
u32int macpal8[256] = {
	0xFFFFFF, 0xFFFFCC, 0xFFFF99, 0xFFFF66, 0xFFFF33, 0xFFFF00, 0xFFCCFF, 0xFFCCCC,
	0xFFCC99, 0xFFCC66, 0xFFCC33, 0xFFCC00, 0xFF99FF, 0xFF99CC, 0xFF9999, 0xFF9966,
	0xFF9933, 0xFF9900, 0xFF66FF, 0xFF66CC, 0xFF6699, 0xFF6666, 0xFF6633, 0xFF6600,
	0xFF33FF, 0xFF33CC, 0xFF3399, 0xFF3366, 0xFF3333, 0xFF3300, 0xFF00FF, 0xFF00CC,
	0xFF0099, 0xFF0066, 0xFF0033, 0xFF0000, 0xCCFFFF, 0xCCFFCC, 0xCCFF99, 0xCCFF66,
	0xCCFF33, 0xCCFF00, 0xCCCCFF, 0xCCCCCC, 0xCCCC99, 0xCCCC66, 0xCCCC33, 0xCCCC00,
	0xCC99FF, 0xCC99CC, 0xCC9999, 0xCC9966, 0xCC9933, 0xCC9900, 0xCC66FF, 0xCC66CC,
	0xCC6699, 0xCC6666, 0xCC6633, 0xCC6600, 0xCC33FF, 0xCC33CC, 0xCC3399, 0xCC3366,
	0xCC3333, 0xCC3300, 0xCC00FF, 0xCC00CC, 0xCC0099, 0xCC0066, 0xCC0033, 0xCC0000,
	0x99FFFF, 0x99FFCC, 0x99FF99, 0x99FF66, 0x99FF33, 0x99FF00, 0x99CCFF, 0x99CCCC,
	0x99CC99, 0x99CC66, 0x99CC33, 0x99CC00, 0x9999FF, 0x9999CC, 0x999999, 0x999966,
	0x999933, 0x999900, 0x9966FF, 0x9966CC, 0x996699, 0x996666, 0x996633, 0x996600,
	0x9933FF, 0x9933CC, 0x993399, 0x993366, 0x993333, 0x993300, 0x9900FF, 0x9900CC,
	0x990099, 0x990066, 0x990033, 0x990000, 0x66FFFF, 0x66FFCC, 0x66FF99, 0x66FF66,
	0x66FF33, 0x66FF00, 0x66CCFF, 0x66CCCC, 0x66CC99, 0x66CC66, 0x66CC33, 0x66CC00,
	0x6699FF, 0x6699CC, 0x669999, 0x669966, 0x669933, 0x669900, 0x6666FF, 0x6666CC,
	0x666699, 0x666666, 0x666633, 0x666600, 0x6633FF, 0x6633CC, 0x663399, 0x663366,
	0x663333, 0x663300, 0x6600FF, 0x6600CC, 0x660099, 0x660066, 0x660033, 0x660000,
	0x33FFFF, 0x33FFCC, 0x33FF99, 0x33FF66, 0x33FF33, 0x33FF00, 0x33CCFF, 0x33CCCC,
	0x33CC99, 0x33CC66, 0x33CC33, 0x33CC00, 0x3399FF, 0x3399CC, 0x339999, 0x339966,
	0x339933, 0x339900, 0x3366FF, 0x3366CC, 0x336699, 0x336666, 0x336633, 0x336600,
	0x3333FF, 0x3333CC, 0x333399, 0x333366, 0x333333, 0x333300, 0x3300FF, 0x3300CC,
	0x330099, 0x330066, 0x330033, 0x330000, 0x00FFFF, 0x00FFCC, 0x00FF99, 0x00FF66,
	0x00FF33, 0x00FF00, 0x00CCFF, 0x00CCCC, 0x00CC99, 0x00CC66, 0x00CC33, 0x00CC00,
	0x0099FF, 0x0099CC, 0x009999, 0x009966, 0x009933, 0x009900, 0x0066FF, 0x0066CC,
	0x006699, 0x006666, 0x006633, 0x006600, 0x0033FF, 0x0033CC, 0x003399, 0x003366,
	0x003333, 0x003300, 0x0000FF, 0x0000CC, 0x000099, 0x000066, 0x000033, 0xEE0000,
	0xDD0000, 0xBB0000, 0xAA0000, 0x880000, 0x770000, 0x550000, 0x440000, 0x220000,
	0x110000, 0x00EE00, 0x00DD00, 0x00BB00, 0x00AA00, 0x008800, 0x007700, 0x005500,
	0x004400, 0x002200, 0x001100, 0x0000EE, 0x0000DD, 0x0000BB, 0x0000AA, 0x000088,
	0x000077, 0x000055, 0x000044, 0x000022, 0x000011, 0xEEEEEE, 0xDDDDDD, 0xBBBBBB,
	0xAAAAAA, 0x888888, 0x777777, 0x555555, 0x444444, 0x222222, 0x111111, 0x000000,
};

enum {
	Opaque = 0xFF000000,
	Black = Opaque|0x000000,
	White = Opaque|0xFFFFFF,
};

typedef struct Icontype Icontype;
struct Icontype {
	char	*type;
	int	size;	/* width and height */
	int	depth;	/* 1, 4 or 8 */
	char	*mask;	/* type holding the mask */
};

static Icontype icontypes[] = {
	{"ICN#", 32, 1, "ICN#"},
	{"icl4", 32, 4, "ICN#"},
	{"icl8", 32, 8, "ICN#"},
	{"ics#", 16, 1, "ics#"},
	{"ics4", 16, 4, "ics#"},
	{"ics8", 16, 8, "ics#"},
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

static Icontype*
icontype(Res *r)
{
	int i;

	for(i = 0; i < (int)(sizeof icontypes / sizeof icontypes[0]); i++)
		if(restypeeq(r, icontypes[i].type))
			return &icontypes[i];
	return nil;
}

int
isicontype(Res *r)
{
	return icontype(r) != nil || restypeeq(r, "CURS");
}

static void
field(Rbuf *b, char *s)
{
	int n;

	for(n = strlen(s); n < 11; n++)
		rbufc(b, ' ');
	rbufs(b, s);
	rbufc(b, ' ');
}

static void
fieldint(Rbuf *b, int v)
{
	char tmp[16];

	snprint(tmp, sizeof tmp, "%d", v);
	field(b, tmp);
}

/* px holds w*h pixels as 0xAARRGGBB, not premultiplied */
static void
writeimage(Rbuf *b, int w, int h, u32int *px)
{
	uchar q[4];
	u32int c, a;
	int i;

	field(b, "a8r8g8b8");
	fieldint(b, 0);
	fieldint(b, 0);
	fieldint(b, w);
	fieldint(b, h);
	for(i = 0; i < w*h; i++){
		c = px[i];
		a = c >> 24;
		if(a == 0)
			c = 0;
		q[0] = c;
		q[1] = c >> 8;
		q[2] = c >> 16;
		q[3] = a;
		rbufput(b, q, 4);
	}
}

static int
bit(uchar *p, int w, int x, int y)
{
	return (p[y*(w/8) + x/8] >> (7 - x%8)) & 1;
}

/* mask bits for a size x size icon: from the mask type's second half */
static uchar*
findmask(Resfile *rf, char *type, int id, int size)
{
	Res *m;
	u32int half;

	half = size*size/8;
	m = reslookup(rf, type, id);
	if(m == nil || m->len < 2*half)
		return nil;
	return m->data + half;
}

static int
decodeicon(Resfile *rf, Res *r, Icontype *t, u32int *px, char *err, int nerr)
{
	u32int need, c;
	uchar *mask, *d;
	int x, y, v, s;

	s = t->size;
	need = s*s*t->depth/8;
	if(r->len < need)
		return seterr(err, nerr, "icon data too short");
	d = r->data;
	if(t->depth == 1)
		mask = r->len >= 2*need ? d + need : nil;
	else
		mask = findmask(rf, t->mask, r->id, s);
	for(y = 0; y < s; y++)
		for(x = 0; x < s; x++){
			switch(t->depth){
			case 1:
				c = bit(d, s, x, y) ? Black : White;
				break;
			case 4:
				v = d[(y*s + x)/2];
				v = (x & 1) ? v & 0xF : v >> 4;
				c = Opaque | macpal4[v];
				break;
			default:
				c = Opaque | macpal8[d[y*s + x]];
				break;
			}
			if(mask != nil && !bit(mask, s, x, y))
				c = 0;
			px[y*s + x] = c;
		}
	return 0;
}

int
rescursorimage(Res *r, Rbuf *out, char *err, int nerr)
{
	u32int px[16*16];
	int x, y, d, m;

	if(r->len < 68)
		return seterr(err, nerr, "CURS data too short");
	for(y = 0; y < 16; y++)
		for(x = 0; x < 16; x++){
			d = bit(r->data, 16, x, y);
			m = bit(r->data+32, 16, x, y);
			/* data without mask inverts the screen; drawn black */
			px[y*16 + x] = d ? Black : m ? White : 0;
		}
	writeimage(out, 16, 16, px);
	return out->err ? seterr(err, nerr, "out of memory") : 0;
}

/*
 * The /dev/cursor form: offset.x[4] offset.y[4] (little-endian),
 * clr[32], set[32]. set is the CURS data; clr is mask and not data;
 * the offset is minus the hot spot.
 */
int
rescursor(Res *r, Rbuf *out, char *err, int nerr)
{
	uchar q[8], clr[32];
	int i, hx, hy, ox, oy;

	if(r->len < 68)
		return seterr(err, nerr, "CURS data too short");
	hy = (short)((r->data[64]<<8) | r->data[65]);
	hx = (short)((r->data[66]<<8) | r->data[67]);
	ox = -hx;
	oy = -hy;
	for(i = 0; i < 4; i++){
		q[i] = ox >> (8*i);
		q[4+i] = oy >> (8*i);
	}
	for(i = 0; i < 32; i++)
		clr[i] = r->data[32+i] & ~r->data[i];
	rbufput(out, q, 8);
	rbufput(out, clr, 32);
	rbufput(out, r->data, 32);
	return out->err ? seterr(err, nerr, "out of memory") : 0;
}

int
resimage(Resfile *rf, Res *r, Rbuf *out, char *err, int nerr)
{
	u32int px[32*32];
	Icontype *t;

	if(restypeeq(r, "CURS"))
		return rescursorimage(r, out, err, nerr);
	t = icontype(r);
	if(t == nil)
		return seterr(err, nerr, "not an icon type");
	if(decodeicon(rf, r, t, px, err, nerr) < 0)
		return -1;
	writeimage(out, t->size, t->size, px);
	return out->err ? seterr(err, nerr, "out of memory") : 0;
}

/* the deepest icon of the family that decodes */
int
resicon(Resfile *rf, int id, int size, Rbuf *out, char *err, int nerr)
{
	static char *large[] = {"icl8", "icl4", "ICN#"};
	static char *small[] = {"ics8", "ics4", "ics#"};
	char **order;
	Res *r;
	int i;

	order = size == Iconsmall ? small : large;
	for(i = 0; i < 3; i++){
		r = reslookup(rf, order[i], id);
		if(r != nil && resimage(rf, r, out, nil, 0) == 0)
			return 0;
		rbuffree(out);
	}
	return seterr(err, nerr, "no icon");
}
