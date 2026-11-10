/*
 * 9/UX look for what rio draws: colours, the system font, glyphs and
 * the menu bar mark, all from assets/ (see assets/README.md).
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include "dat.h"
#include "fns.h"
#include "ux.h"
#include "uxmark.h"	/* made by mkfile from assets/gen/icons/mark.16.img */

/*
 * assets/gen/uxglyphs.c's table, with its fields named here: the ink
 * bitmap and the paper (fill) bitmap, one bit per pixel, rows padded to
 * bytes.
 */
typedef struct Glyph Glyph;
struct Glyph
{
	char		*name;
	int		wid;
	int		ht;
	uchar	*ink;
	uchar	*fill;
};
extern Glyph uxglyphs[];

Font	*uxsysfont;
Image	*uxmark;

static struct {
	ulong	c;
	Image	*i;
} ucols[32];
static int ncols;

static struct {
	char		*name;
	Image	*ink;
	Image	*fill;
	Point	size;
} masks[32];
static int nmasks;

Image*
uxcolor(ulong c)
{
	int i;

	for(i=0; i<ncols; i++)
		if(ucols[i].c == c)
			return ucols[i].i;
	if(ncols == nelem(ucols))
		return display->black;
	ucols[ncols].c = c;
	ucols[ncols].i = allocimage(display, Rect(0, 0, 1, 1), RGB24, 1, c);
	if(ucols[ncols].i == nil)
		return display->black;
	return ucols[ncols++].i;
}

static Image*
mask(uchar *bits, int w, int h)
{
	Image *m;
	int i, n;

	n = (w+7)/8*h;
	for(i=0; i<n; i++)
		if(bits[i])
			break;
	if(i == n)
		return nil;
	m = allocimage(display, Rect(0, 0, w, h), GREY1, 0, DNofill);
	if(m != nil)
		loadimage(m, m->r, bits, n);
	return m;
}

static int
findglyph(char *name)
{
	Glyph *g;
	int i;

	for(i=0; i<nmasks; i++)
		if(strcmp(masks[i].name, name) == 0)
			return i;
	for(g=uxglyphs; g->name != nil; g++)
		if(strcmp(g->name, name) == 0)
			break;
	if(g->name == nil || nmasks == nelem(masks))
		return -1;
	masks[nmasks].name = g->name;
	masks[nmasks].size = Pt(g->wid, g->ht);
	masks[nmasks].ink = mask(g->ink, g->wid, g->ht);
	masks[nmasks].fill = mask(g->fill, g->wid, g->ht);
	return nmasks++;
}

int
uxglyphwidth(char *name)
{
	int i;

	i = findglyph(name);
	return i < 0 ? 0 : masks[i].size.x;
}

int
uxglyphheight(char *name)
{
	int i;

	i = findglyph(name);
	return i < 0 ? 0 : masks[i].size.y;
}

/* draw glyph name with its top left at p, ink in col */
void
uxglyph(Image *dst, Point p, char *name, Image *col)
{
	int i;
	Rectangle r;

	if((i = findglyph(name)) < 0)
		return;
	r = Rpt(p, addpt(p, masks[i].size));
	if(masks[i].fill != nil)
		draw(dst, r, uxcolor(UXpaper), masks[i].fill, ZP);
	if(masks[i].ink != nil)
		draw(dst, r, col, masks[i].ink, ZP);
}

/* an uncompressed image(6) file in memory */
static Image*
memimage(uchar *b, int n)
{
	char hdr[5*12+1];
	ulong chan;
	Rectangle r;
	Image *i;
	int m;

	if(n < 5*12)
		return nil;
	memmove(hdr, b, 5*12);
	hdr[5*12] = 0;
	hdr[11] = 0;
	if((chan = strtochan(hdr)) == 0)
		return nil;
	r.min.x = atoi(hdr+1*12);
	r.min.y = atoi(hdr+2*12);
	r.max.x = atoi(hdr+3*12);
	r.max.y = atoi(hdr+4*12);
	m = bytesperline(r, chantodepth(chan))*Dy(r);
	if(n < 5*12+m)
		return nil;
	i = allocimage(display, r, chan, 0, DNofill);
	if(i != nil)
		loadimage(i, r, b+5*12, m);
	return i;
}

void
uxstyleinit(void)
{
	if(uxsysfont == nil){
		uxsysfont = openfont(display, "/lib/font/bit/lucidasans/boldunicode.7.font");
		if(uxsysfont == nil)
			uxsysfont = font;
	}
	if(uxmark == nil)
		uxmark = memimage(markimg, sizeof markimg);
}
