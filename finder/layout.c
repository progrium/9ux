/*
 * Layout: icon placement on a grid, and window geometry, view mode and
 * icon positions kept in uxmeta (folder, item and volume records).
 *
 * An item's x,y is the centre of its icon in the folder's icon view
 * (the pos= of libux's icon-view items). A folder record's x,y,width,
 * height is its window's rectangle on the screen; open=yes marks the
 * windows to reopen when Finder starts.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>
#include "fcore.h"
#include "finder.h"
#include "uxstyle.h"

void*
femalloc(ulong n)
{
	void *p;

	p = mallocz(n, 1);
	if(p == nil)
		sysfatal("out of memory");
	setmalloctag(p, getcallerpc(&n));
	return p;
}

char*
festrdup(char *s)
{
	s = strdup(s);
	if(s == nil)
		sysfatal("out of memory");
	return s;
}

enum
{
	Margin	= 10,
	Top	= 4,
};

/* the centre of an icon in grid cell col,row of a folder's view */
Point
cellpt(int col, int row)
{
	return Pt(Margin + UXiconviewcellw/2 + col*UXiconviewcellw,
		Top + UXiconlarge/2 + row*UXiconviewcellh);
}

static Point
deskpt(View *v, int col, int row)
{
	return Pt(v->size.x - UXdesktopmargin - UXiconviewcellw/2 - col*UXiconviewcellw,
		UXdesktopmargin + UXiconlarge/2 + row*UXdesktopcellh);
}

static int
taken(View *v, Point p)
{
	int i;
	Item *it;

	for(i = 0; i < v->nitem; i++){
		it = v->item[i];
		if(it->pt.x == 0 && it->pt.y == 0)
			continue;
		if(abs(it->pt.x - p.x) < UXiconviewcellw/2 && abs(it->pt.y - p.y) < UXiconviewcellh/2)
			return 1;
	}
	return 0;
}

/*
 * The first free cell: in a folder, left to right then down, as wide
 * as the window; on the desktop, down the right edge then leftwards.
 */
Point
freecell(View *v)
{
	int col, row, ncol, nrow;
	Point p;

	if(v->kind == Kdesk){
		nrow = (v->size.y - UXdesktopmargin) / UXdesktopcellh;
		if(nrow < 1)
			nrow = 1;
		for(col = 0; ; col++)
			for(row = 0; row < nrow; row++){
				p = deskpt(v, col, row);
				if(!taken(v, p) || p.x < 0)
					return p;
			}
	}
	ncol = (v->size.x - 2*Margin) / UXiconviewcellw;
	if(ncol < 1)
		ncol = 1;
	for(row = 0; ; row++)
		for(col = 0; col < ncol; col++){
			p = cellpt(col, row);
			if(!taken(v, p))
				return p;
		}
}

/* where Finder puts a folder window that has no saved place */
Rectangle
cascade(void)
{
	static int n;
	Rectangle r;

	r = Rect(80, 60, 80+440, 60+300);
	r = rectaddpt(r, Pt(24*(n%8), 24*(n%8)));
	n++;
	return r;
}

static void
metaerr(char *what)
{
	fprint(2, "finder: saving %s: %r\n", what);
}

void
savewin(View *v)
{
	char *q;

	if(meta == nil || v->kind != Kfolder)
		return;
	q = quotestrdup(v->path);
	if(fcmetactl(meta, "set folder %s x=%d y=%d width=%d height=%d view=%s sort=%s open=yes\n",
		q, v->r.min.x, v->r.min.y, Dx(v->r), Dy(v->r),
		v->mode == Mlist ? "list" : "icons",
		v->sort == Skind ? "kind" : v->sort == Sdate ? "date" : v->sort == Ssize ? "size" : "name") < 0)
		metaerr(v->path);
	free(q);
}

/* the window was closed: don't reopen it next time */
void
forgetwin(View *v)
{
	char *q;

	if(meta == nil || v->kind != Kfolder)
		return;
	q = quotestrdup(v->path);
	if(fcmetactl(meta, "set folder %s open=no\n", q) < 0)
		metaerr(v->path);
	free(q);
}

void
saveitem(View *v, Item *it)
{
	char *q;

	if(it->vol != nil){
		savevol(it);
		return;
	}
	if(meta == nil || v->kind != Kfolder)
		return;
	q = quotestrdup(it->path);
	if(fcmetactl(meta, "set item %s x=%d y=%d\n", q, it->pt.x, it->pt.y) < 0)
		metaerr(it->path);
	free(q);
}

void
savevol(Item *it)
{
	char *q;

	if(meta == nil || it->vol == nil)
		return;
	q = quotestrdup(it->vol->name);
	if(fcmetactl(meta, "set volume %s x=%d y=%d\n", q, it->pt.x, it->pt.y) < 0)
		metaerr(it->vol->name);
	free(q);
	it->vol->x = it->pt.x;
	it->vol->y = it->pt.y;
}

static Fcrec*
folderrec(char *path)
{
	char *q;
	Fcrec *r;

	if(meta == nil)
		return nil;
	q = quotestrdup(path);
	r = fcmetaquery(meta, "folder %s", q);
	free(q);
	return r;
}

/* the saved window rectangle, or ZR */
Rectangle
winrect(char *path)
{
	Fcrec *r;
	char *x, *y, *w, *h;
	Rectangle rr;

	rr = ZR;
	r = folderrec(path);
	if(r == nil)
		return rr;
	x = fcrecget(r, "x");
	y = fcrecget(r, "y");
	w = fcrecget(r, "width");
	h = fcrecget(r, "height");
	if(x && y && w && h && atoi(w) >= UXwindowminwidth && atoi(h) >= UXwindowminheight)
		rr = Rect(atoi(x), atoi(y), atoi(x)+atoi(w), atoi(y)+atoi(h));
	fcrecfree(r);
	return rr;
}

/* the saved view mode (Micons, Mlist) and sort */
int
winmode(char *path, int *sort)
{
	Fcrec *r;
	char *s;
	int m;

	m = Micons;
	*sort = Sname;
	r = folderrec(path);
	if(r == nil)
		return m;
	s = fcrecget(r, "view");
	if(s != nil && strcmp(s, "list") == 0)
		m = Mlist;
	s = fcrecget(r, "sort");
	if(s != nil && fcsortby(s) >= 0)
		*sort = fcsortby(s);
	fcrecfree(r);
	return m;
}

/* item records for dir's own entries */
Fcrec*
itemrecs(char *dir)
{
	char *q;
	Fcrec *recs;

	if(meta == nil)
		return nil;
	q = quotestrdup(dir);
	recs = fcmetaquery(meta, "under item %s", q);
	free(q);
	return recs;
}
