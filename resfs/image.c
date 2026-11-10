/*
 * Reading image(6) files and encoding icons and cursors from them,
 * for rescomp. Only uncompressed images, with channels from r, g,
 * b, k, a and x (no colour-mapped m8).
 */
#include "rescompat.h"
#include "res.h"

enum {
	Hdr = 5*12,
	Maxdim = 1024,
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

/* one 12-byte header field, trimmed, into f */
static int
field(uchar *p, char *f)
{
	int i, j;

	if(p[11] != ' ')
		return -1;
	for(i = 0; i < 11 && p[i] == ' '; i++)
		;
	for(j = 0; i < 11; i++, j++){
		if(p[i] == ' ' || p[i] == 0)
			return -1;
		f[j] = p[i];
	}
	f[j] = 0;
	return j > 0 ? 0 : -1;
}

static int
fieldint(uchar *p, long *v)
{
	char f[12], *s;
	long x;
	int neg;

	if(field(p, f) < 0)
		return -1;
	s = f;
	neg = *s == '-';
	if(neg)
		s++;
	if(*s == 0)
		return -1;
	for(x = 0; *s; s++){
		if(*s < '0' || *s > '9' || x > 100000000)
			return -1;
		x = x*10 + *s - '0';
	}
	*v = neg ? -x : x;
	return 0;
}

typedef struct Chan Chan;
struct Chan {
	int	type;	/* r g b k a x */
	int	bits;
};

static int
parsechan(char *s, Chan *c, int *nc, int *depth)
{
	int n, d, b;

	/* old images give an ldepth: 0 to 3 are 1 to 8-bit grey */
	if(s[0] >= '0' && s[0] <= '3' && s[1] == 0){
		c[0].type = 'k';
		c[0].bits = 1 << (s[0] - '0');
		*nc = 1;
		*depth = c[0].bits;
		return 0;
	}
	n = 0;
	d = 0;
	while(*s){
		if(n >= 4 || strchr("rgbkax", *s) == nil)
			return -1;
		c[n].type = *s++;
		if(*s < '1' || *s > '8')
			return -1;
		b = *s++ - '0';
		c[n].bits = b;
		d += b;
		n++;
	}
	if(n == 0 || (d != 1 && d != 2 && d != 4 && d != 8 && d != 16 && d != 24 && d != 32))
		return -1;
	if(d < 8 && n != 1)
		return -1;
	*nc = n;
	*depth = d;
	return 0;
}

/*
 * Decode an uncompressed image(6) into *px: w*h pixels 0xAARRGGBB,
 * not premultiplied. The caller frees *px.
 */
int
resreadimage(uchar *buf, u32int n, int *wp, int *hp, u32int **pxp, char *err, int nerr)
{
	char f[12];
	long minx, miny, maxx, maxy;
	int nc, depth, w, h, x, y, i, sh, bpl, a, r, g, b, v, max;
	Chan c[4];
	u32int pix, *px;
	uchar *row;

	if(n >= 11 && memcmp(buf, "compressed\n", 11) == 0)
		return seterr(err, nerr, "compressed images are not supported");
	if(n < Hdr)
		return seterr(err, nerr, "short image header");
	if(field(buf, f) < 0 || parsechan(f, c, &nc, &depth) < 0)
		return seterr(err, nerr, "unsupported image channels");
	if(fieldint(buf+12, &minx) < 0 || fieldint(buf+24, &miny) < 0
	|| fieldint(buf+36, &maxx) < 0 || fieldint(buf+48, &maxy) < 0)
		return seterr(err, nerr, "bad image header");
	if(maxx <= minx || maxy <= miny || maxx - minx > Maxdim || maxy - miny > Maxdim)
		return seterr(err, nerr, "bad image size");
	w = maxx - minx;
	h = maxy - miny;
	bpl = (w*depth + 7) / 8;
	if((u32int)bpl * h > n - Hdr)
		return seterr(err, nerr, "image data too short");
	px = malloc(w*h*sizeof *px);
	if(px == nil)
		return seterr(err, nerr, "out of memory");
	for(y = 0; y < h; y++){
		row = buf + Hdr + y*bpl;
		for(x = 0; x < w; x++){
			if(depth < 8)
				pix = (row[x*depth/8] >> (8 - depth - (x*depth)%8)) & ((1<<depth) - 1);
			else{
				pix = 0;
				for(i = depth/8 - 1; i >= 0; i--)
					pix = pix<<8 | row[x*(depth/8) + i];
			}
			a = 255;
			r = g = b = 0;
			sh = 0;
			for(i = nc-1; i >= 0; i--){
				max = (1 << c[i].bits) - 1;
				v = ((pix >> sh) & max) * 255 / max;
				sh += c[i].bits;
				switch(c[i].type){
				case 'r':
					r = v;
					break;
				case 'g':
					g = v;
					break;
				case 'b':
					b = v;
					break;
				case 'k':
					r = g = b = v;
					break;
				case 'a':
					a = v;
					break;
				}
			}
			if(a == 0)
				r = g = b = 0;
			else if(a < 255){
				r = r*255/a;
				g = g*255/a;
				b = b*255/a;
				if(r > 255)
					r = 255;
				if(g > 255)
					g = 255;
				if(b > 255)
					b = 255;
			}
			px[y*w + x] = (u32int)a<<24 | r<<16 | g<<8 | b;
		}
	}
	*wp = w;
	*hp = h;
	*pxp = px;
	return 0;
}

static int
opaque(u32int c)
{
	return (c >> 24) >= 128;
}

static int
dark(u32int c)
{
	int r, g, b;

	r = (c >> 16) & 0xFF;
	g = (c >> 8) & 0xFF;
	b = c & 0xFF;
	return r*299 + g*587 + b*114 < 128*1000;
}

static int
nearest(u32int c, u32int *pal, int n)
{
	int i, best, dr, dg, db;
	long d, bestd;

	best = 0;
	bestd = -1;
	for(i = 0; i < n; i++){
		dr = (int)((c >> 16) & 0xFF) - (int)((pal[i] >> 16) & 0xFF);
		dg = (int)((c >> 8) & 0xFF) - (int)((pal[i] >> 8) & 0xFF);
		db = (int)(c & 0xFF) - (int)(pal[i] & 0xFF);
		d = (long)dr*dr + (long)dg*dg + (long)db*db;
		if(bestd < 0 || d < bestd){
			best = i;
			bestd = d;
			if(d == 0)
				break;
		}
	}
	return best;
}

static void
bits(Rbuf *out, int s, u32int *px, int mask)
{
	int x, y, v;
	u32int c;

	for(y = 0; y < s; y++)
		for(x = 0; x < s; x += 8){
			int k;
			v = 0;
			for(k = 0; k < 8; k++){
				c = px[y*s + x + k];
				v <<= 1;
				if(mask ? opaque(c) : opaque(c) && dark(c))
					v |= 1;
			}
			rbufc(out, v);
		}
}

/*
 * An icon resource of the given type from a size x size image.
 * For ICN# and ics#, bw (if not nil) gives the 1-bit form; its pixels
 * are black where dark and opaque. The mask always comes from px.
 */
int
resiconenc(uchar type[4], int w, int h, u32int *px, u32int *bw, Rbuf *out, char *err, int nerr)
{
	int s, depth, i, v;
	u32int c;

	if(memcmp(type, "ICN#", 4) == 0 || memcmp(type, "icl4", 4) == 0 || memcmp(type, "icl8", 4) == 0)
		s = 32;
	else if(memcmp(type, "ics#", 4) == 0 || memcmp(type, "ics4", 4) == 0 || memcmp(type, "ics8", 4) == 0)
		s = 16;
	else
		return seterr(err, nerr, "not an icon type");
	if(w != s || h != s)
		return seterr(err, nerr, s == 32 ? "icon image must be 32x32" : "icon image must be 16x16");
	depth = type[3] == '#' ? 1 : type[3] - '0';
	if(depth == 1){
		bits(out, s, bw != nil ? bw : px, 0);
		if(bw != nil){
			/* a pixel of the 1-bit form must lie inside the mask */
			for(i = 0; i < s*s; i++)
				if(opaque(bw[i]) && dark(bw[i]) && !opaque(px[i]))
					return seterr(err, nerr, "1-bit form draws outside the mask");
		}
		bits(out, s, px, 1);
	}else{
		for(i = 0; i < s*s; i++){
			c = px[i];
			if(!opaque(c))
				v = 0;
			else if(depth == 4)
				v = nearest(c, macpal4, 16);
			else
				v = nearest(c, macpal8, 256);
			if(depth == 8)
				rbufc(out, v);
			else if(i & 1){
				if(!out->err)
					out->p[out->n-1] |= v;
			}
			else
				rbufc(out, v << 4);
		}
	}
	return out->err ? seterr(err, nerr, "out of memory") : 0;
}

/* a CURS from a 16x16 image: dark opaque is black, light opaque white */
int
rescursenc(int w, int h, u32int *px, int hotx, int hoty, Rbuf *out, char *err, int nerr)
{
	if(w != 16 || h != 16)
		return seterr(err, nerr, "cursor image must be 16x16");
	if(hotx < 0 || hotx > 15 || hoty < 0 || hoty > 15)
		return seterr(err, nerr, "hot spot outside the cursor");
	bits(out, 16, px, 0);
	bits(out, 16, px, 1);
	rbufc(out, hoty >> 8);
	rbufc(out, hoty);
	rbufc(out, hotx >> 8);
	rbufc(out, hotx);
	return out->err ? seterr(err, nerr, "out of memory") : 0;
}
