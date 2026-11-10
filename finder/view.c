/*
 * The desktop and folder windows: their items, what's selected, keeping
 * up with the file system by rescanning, and the events from their
 * controls.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>
#include "fcore.h"
#include "finder.h"
#include "uxstyle.h"

enum
{
	Infoh	= 23,	/* the info bar at the top of a folder window, with its double rule */
	Statush	= 20,	/* the info bar's text */
	Infopad	= 8,	/* its text from either side */
};

static void	populate(View*);

Lock	itemlk;

void
itemsetpath(Item *it, char *path)
{
	char *old;

	lock(&itemlk);
	old = it->path;
	it->path = path;
	unlock(&itemlk);
	free(old);
}

static void
addview(View *v)
{
	v->next = views;
	views = v;
}

/* hatch the icons of folders and volumes whose windows are open (assets/README.md, Finder: Open) */
static void
markopen(void)
{
	View *v, *o;
	Item *it;
	int i, open;

	for(v = views; v != nil; v = v->next){
		if(v->icons == nil || v->kind == Ktrash)
			continue;
		for(i = 0; i < v->nitem; i++){
			it = v->item[i];
			open = 0;
			if(it->path != nil && (it->dir || it->vol != nil))
				for(o = views; o != nil; o = o->next)
					if(o->kind == Kfolder && o->path != nil && strcmp(o->path, it->path) == 0)
						open = 1;
			uxiconopen(v->icons, it->id, open);
		}
	}
}

View*
viewbywin(Uxwin *w)
{
	View *v;

	for(v = views; v != nil; v = v->next)
		if(v->w == w)
			return v;
	return nil;
}

View*
viewbypath(char *path)
{
	View *v;

	for(v = views; v != nil; v = v->next)
		if(v->kind == Kfolder && strcmp(v->path, path) == 0)
			return v;
	return nil;
}

Item*
itembyid(View *v, char *id)
{
	int i;

	for(i = 0; i < v->nitem; i++)
		if(strcmp(v->item[i]->id, id) == 0)
			return v->item[i];
	return nil;
}

Item*
itembyname(View *v, char *name)
{
	int i;

	for(i = 0; i < v->nitem; i++)
		if(strcmp(v->item[i]->name, name) == 0)
			return v->item[i];
	return nil;
}

static void
itemfree(Item *it)
{
	free(it->id);
	free(it->name);
	free(it->path);
	free(it->icon);
	free(it);
}

static Item*
newitem(View *v, char *name, char *path)
{
	Item *it;

	it = femalloc(sizeof *it);
	it->id = smprint("i%d", ++v->nextid);
	it->name = festrdup(name);
	it->path = festrdup(path);
	lock(&itemlk);
	v->item = realloc(v->item, (v->nitem+1)*sizeof(Item*));
	v->item[v->nitem++] = it;
	unlock(&itemlk);
	return it;
}

static void
delitem(View *v, Item *it)
{
	int i;

	lock(&itemlk);
	for(i = 0; i < v->nitem; i++)
		if(v->item[i] == it){
			memmove(v->item+i, v->item+i+1, (v->nitem-i-1)*sizeof(Item*));
			v->nitem--;
			break;
		}
	unlock(&itemlk);
	if(v->editing == it)
		renameend(v, 0);
	if(v->icons != nil)
		uxicondel(v->icons, it->id);
	itemfree(it);
}

/* what an icon shows; offline volumes say so */
char*
viewlabel(Item *it)
{
	if(it->offline)
		return smprint("%s (offline)", it->name);
	return festrdup(it->name);
}

/* the icon name for a folder entry: uxmeta's association, or a generic one */
static char*
iconfor(char *path, Dir *d, Fcrec *rec)
{
	Fcrec *r;
	char *q, *s, *home;

	if(rec != nil && (s = fcrecget(rec, "icon")) != nil && s[0])
		return festrdup(s);
	if(d->qid.type & QTDIR){
		home = getenv("home");
		s = home != nil && strcmp(home, path) == 0 ? "folder-home" : "folder";
		free(home);
		return festrdup(s);
	}
	if(meta != nil){
		q = quotestrdup(path);
		r = fcmetaquery(meta, "resolve %s", q);
		free(q);
		if(r != nil && (s = fcrecget(r, "icon")) != nil && s[0]){
			s = festrdup(s);
			fcrecfree(r);
			return s;
		}
		fcrecfree(r);
	}
	if(d->mode & 0111)
		return festrdup("app");
	return festrdup("doc");
}

static Fcrec*
recfor(Fcrec *recs, char *path)
{
	for(; recs != nil; recs = recs->next)
		if(strcmp(recs->a[0].attr, "item") == 0 && strcmp(fcreckey(recs), path) == 0)
			return recs;
	return nil;
}

static void
setpos(Item *it, Fcrec *rec)
{
	char *x, *y;

	if(rec == nil)
		return;
	x = fcrecget(rec, "x");
	y = fcrecget(rec, "y");
	if(x != nil && y != nil && x[0] && y[0]){
		it->pt = Pt(atoi(x), atoi(y));
		it->placed = 1;
	}
}

static Item*
entryitem(View *v, Dir *d, Fcrec *recs)
{
	Item *it;
	char *p;
	Fcrec *rec;

	p = fcjoin(v->path, d->name);
	it = newitem(v, d->name, p);
	free(p);
	rec = recfor(recs, it->path);
	it->dir = (d->qid.type & QTDIR) != 0;
	it->icon = iconfor(it->path, d, rec);
	setpos(it, rec);
	return it;
}

static void
placeall(View *v)
{
	int i;

	for(i = 0; i < v->nitem; i++)
		if(!v->item[i]->placed && v->item[i]->pt.x == 0 && v->item[i]->pt.y == 0)
			v->item[i]->pt = freecell(v);
}

static void
showitem(View *v, Item *it)
{
	char *l;

	if(v->icons == nil)
		return;
	l = viewlabel(it);
	uxiconadd(v->icons, it->id, l, it->icon, it->pt);
	free(l);
}

/* the visible content: the desktop, or a folder window's below its info bar and left of its bars */
static Rectangle
contentrect(View *v)
{
	if(v->kind == Kdesk || v->scroll == nil)
		return Rect(0, 0, v->size.x, v->size.y);
	return uxscrollport(v->scroll);
}

/* where the icon view's 0,0 is in the window, scrolled */
Point
viewport(View *v)
{
	Point o;

	if(v->scroll == nil)
		return contentrect(v).min;
	o = uxgetorigin(v->scroll);
	return subpt(contentrect(v).min, o);
}

Rectangle
viewcontent(View *v)
{
	return contentrect(v);
}

/* list view columns, as in the mockups' list window */
static char *coltitles[] = { "Name", "Kind", "Size" };
static int colwidths[] = { 170, 100, 70 };

/*
 * A folder (or the Trash) window's frame inside rio's: a scroll view
 * with both bars and the grow box, always shown as in a classic folder
 * window, and the info bar above the content: the item count at the
 * left, the size of what's there at the right, over a double rule.
 */
void
folderchrome(View *v)
{
	v->scroll = uxscrollview(v->w, "scroll", Rect(-1, -1, v->size.x+1, v->size.y+1));
	uxscrollbars(v->scroll, UXvbar|UXhbar|UXgrowbox, Infoh);
	uxsetextent(v->scroll, Rect(0, 0, 0, 0));
	v->status = uxlabel(v->w, "status", "", Rect(Infopad, 0, v->size.x-Infopad, Statush));
	uxlabelstyle(v->status, UXsmall);
	v->space = uxlabel(v->w, "space", "", Rect(Infopad, 0, v->size.x-Infopad, Statush));
	uxlabelstyle(v->space, UXsmall|UXright);
}

/* everything in a folder window placed for its size and content */
void
folderlayout(View *v)
{
	Rectangle port, ext, cr, ir;
	int i, w, info;
	Item *it;

	if(v->scroll == nil)
		return;
	/*
	 * icon view: both bars and the info bar; list view, as in the
	 * mockups' list window: the vertical bar, and the column header
	 * right under the title bar (unless there's an error to show)
	 */
	info = v->list == nil || v->err[0] || (v->vol != nil && v->vol->state == Voffline);
	if(v->look != 1 + (v->list != nil) + 2*info){
		v->look = 1 + (v->list != nil) + 2*info;
		uxscrollbars(v->scroll, v->list != nil ? UXvbar|UXgrowbox : UXvbar|UXhbar|UXgrowbox, info ? Infoh : 0);
		if(v->status != nil)
			uxshow(v->status, info);
		if(v->space != nil)
			uxshow(v->space, info);
	}
	uxsetrect(v->scroll, Rect(-1, -1, v->size.x+1, v->size.y+1));
	port = uxscrollport(v->scroll);
	if(v->status != nil)
		uxsetrect(v->status, Rect(Infopad, 0, port.max.x-Infopad, Statush));
	if(v->space != nil)
		uxsetrect(v->space, Rect(Infopad, 0, port.max.x-Infopad, Statush));
	/* the content's extent: every icon's cell, or every row and column */
	ext = Rect(0, 0, 0, 0);
	if(v->list != nil){
		w = UXlistpad;
		for(i = 0; i < nelem(colwidths); i++)
			w += colwidths[i];
		ext = Rect(0, 0, w, UXlistrowheight+2 + v->nrows*UXlistrowheight);
	}else if(v->icons != nil){
		lock(&itemlk);
		for(i = 0; i < v->nitem; i++){
			it = v->item[i];
			ir = Rect(it->pt.x - UXiconviewcellw/2, it->pt.y - UXiconlarge/2,
				it->pt.x + UXiconviewcellw/2, it->pt.y - UXiconlarge/2 + UXiconviewcellh);
			combinerect(&ext, ir);
		}
		unlock(&itemlk);
		ext.min = ZP;	/* nothing scrolls above or left of the folder's top left */
	}
	uxsetextent(v->scroll, ext);
	cr = Rect(port.min.x, port.min.y,
		port.min.x + (Dx(ext) > Dx(port) ? ext.max.x : Dx(port)),
		port.min.y + (Dy(ext) > Dy(port) ? ext.max.y : Dy(port)));
	if(v->icons != nil)
		uxsetrect(v->icons, cr);
	if(v->list != nil)
		uxsetrect(v->list, cr);
}

/* the desktop */

View*
deskopen(void)
{
	View *v;

	v = femalloc(sizeof *v);
	v->kind = Kdesk;
	v->title = festrdup("Desktop");
	v->w = uxwindow("desktop", "Desktop", ZR);
	v->r = uxwinrect(v->w);
	v->size = uxwinsize(v->w);
	v->icons = uxiconview(v->w, "icons", contentrect(v));
	uxwinhandler(v->w, viewhandler, v);
	uxhandler(v->icons, viewhandler, v);
	dragsetup(v);
	addview(v);
	return v;
}

static Item*
desktrash(View *v)
{
	int i;

	for(i = 0; i < v->nitem; i++)
		if(v->item[i]->trash)
			return v->item[i];
	return nil;
}

static char*
volicon(Fcvol *vol)
{
	if(vol->icon != nil && vol->icon[0])
		return vol->icon;
	if(vol->addr != nil || vol->srv != nil)
		return "disk-remote";
	return "disk";
}

/* (re)build the desktop's volumes from the registry */
void
deskload(void)
{
	View *v;
	Fcvol *vol;
	Item *it;
	int i, j, found;

	v = desk;
	if(v == nil)
		return;
	if(meta != nil && fcwsload(ws) < 0)
		fprint(2, "finder: volumes: %r\n");
	/* drop volumes that left the registry or the desktop */
	for(i = 0; i < v->nitem; ){
		it = v->item[i];
		found = it->trash;
		for(j = 0; j < ws->nvol; j++)
			if(ws->vol[j] == it->vol && ws->vol[j]->desktop)
				found = 1;
		if(!found)
			delitem(v, it);
		else
			i++;
	}
	for(j = 0; j < ws->nvol; j++){
		vol = ws->vol[j];
		if(!vol->desktop)
			continue;
		it = nil;
		for(i = 0; i < v->nitem; i++)
			if(v->item[i]->vol == vol)
				it = v->item[i];
		if(it == nil){
			it = newitem(v, vol->name, vol->path);
			it->vol = vol;
			it->dir = 1;
			it->icon = festrdup(volicon(vol));
			if(vol->x != 0 || vol->y != 0){
				it->pt = Pt(vol->x, vol->y);
				it->placed = 1;
			}else
				it->pt = freecell(v);
			fcvolcheck(vol);
			it->offline = vol->state == Voffline;
			showitem(v, it);
			continue;
		}
		/* path, icon or position changed in the registry */
		if(strcmp(it->path, vol->path) != 0)
			itemsetpath(it, festrdup(vol->path));
		if(strcmp(it->icon, volicon(vol)) != 0){
			free(it->icon);
			it->icon = festrdup(volicon(vol));
		}
		if((vol->x != 0 || vol->y != 0) && !eqpt(it->pt, Pt(vol->x, vol->y))){
			it->pt = Pt(vol->x, vol->y);
			it->placed = 1;
		}
		uxiconset(v->icons, it->id, nil, it->icon, it->pt);
	}
	if(desktrash(v) == nil){
		it = newitem(v, "Trash", "");
		it->trash = 1;
		it->icon = festrdup("trash");
		it->pt = trashpt(v);
		showitem(v, it);
		trashcheck();
	}
	deskcheck();
}

/* show volumes going offline and coming back */
void
deskcheck(void)
{
	View *v;
	Item *it;
	int i, off;
	char *l;

	v = desk;
	if(v == nil)
		return;
	for(i = 0; i < v->nitem; i++){
		it = v->item[i];
		if(it->vol == nil)
			continue;
		fcvolcheck(it->vol);
		off = it->vol->state == Voffline;
		if(off != it->offline){
			it->offline = off;
			l = viewlabel(it);
			uxiconset(v->icons, it->id, l, nil, it->pt);
			free(l);
		}
	}
}

/* folder windows */

/* each volume's Trash is shown on the desktop, not in its root folder */
static int
hidden(View *v, char *name)
{
	return strcmp(name, ".trash") == 0 && v->vol != nil && strcmp(v->vol->path, v->path) == 0;
}

static void
loaditems(View *v)
{
	Dir **dv;
	Fcrec *recs;
	int i;

	recs = itemrecs(v->path);
	dv = fcsorted(v->f, v->sort);
	for(i = 0; i < v->f->n; i++)
		if(!hidden(v, dv[i]->name))
			entryitem(v, dv[i], recs);
	free(dv);
	fcrecfree(recs);
	placeall(v);
}

View*
folderopen(char *path, int remember)
{
	View *v;
	Rectangle r;
	Fcfolder *f;
	char *p;

	p = festrdup(path);
	cleanname(p);
	v = viewbypath(p);
	if(v != nil){
		free(p);
		uxwinfront(v->w);
		setfront(v);
		return v;
	}
	f = fcfolderopen(p);
	if(f == nil){
		alerterr(p);
		free(p);
		return nil;
	}
	v = femalloc(sizeof *v);
	v->kind = Kfolder;
	v->path = p;
	v->f = f;
	v->vol = fcvolof(ws, p);
	if(v->vol != nil && strcmp(v->vol->path, p) == 0)
		v->title = festrdup(v->vol->name);
	else
		v->title = festrdup(fcbasename(p));
	v->mode = winmode(p, &v->sort);
	r = winrect(p);
	if(Dx(r) == 0)
		r = cascade();
	v->w = uxwindow("folder", v->title, r);
	v->r = uxwinrect(v->w);
	v->size = uxwinsize(v->w);
	uxwinhandler(v->w, viewhandler, v);
	folderchrome(v);
	addview(v);
	loaditems(v);
	populate(v);
	if(remember)
		savewin(v);
	setfront(v);
	markopen();
	return v;
}

void
viewclose(View *v, int forget)
{
	View **l;
	int i;

	for(l = &views; *l != nil; l = &(*l)->next)
		if(*l == v){
			*l = v->next;
			break;
		}
	if(forget)
		forgetwin(v);
	if(front == v)
		front = nil;
	uxwinclose(v->w);
	lock(&itemlk);
	for(i = 0; i < v->nitem; i++)
		itemfree(v->item[i]);
	free(v->item);
	v->item = nil;
	v->nitem = 0;
	unlock(&itemlk);
	for(i = 0; i < v->nrows; i++)
		free(v->rows[i]);
	free(v->rows);
	fcfolderfree(v->f);
	free(v->path);
	free(v->title);
	free(v);
	setfront(desk);
	markopen();
}

/* a size the way a list window shows it: 2 K, 487 M, 4.1 G */
static char*
sizestr(vlong n)
{
	if(n < 1024*1024)
		return smprint("%lld K", (n+1023)/1024);
	if(n < 1024LL*1024*1024)
		return smprint("%lld M", (n+512*1024)/(1024*1024));
	return smprint("%.1f G", n/(1024.0*1024*1024));
}

/* what a list row calls an item: folder, its type from uxmeta (text, image...), program or file */
static char*
kindof(Item *it, Dir *d)
{
	Fcrec *r;
	char *q, *s;

	if(it->dir)
		return festrdup("folder");
	if(meta != nil){
		q = quotestrdup(it->path);
		r = fcmetaquery(meta, "resolve %s", q);
		free(q);
		if(r != nil && (s = fcrecget(r, "type")) != nil && s[0]){
			s = festrdup(s);
			fcrecfree(r);
			return s;
		}
		fcrecfree(r);
	}
	return festrdup((d->mode & 0111) ? "program" : "file");
}

/* a list row: name, kind and size, in tab-separated columns */
static char*
row(Item *it, Dir *d)
{
	char *s, *sz, *kind;

	sz = it->dir ? festrdup("--") : sizestr(d->length);
	kind = kindof(it, d);
	s = smprint("%s\t%s\t%s", it->name, kind, sz);
	free(kind);
	free(sz);
	return s;
}

/* put the items into the window's control, for its mode */
static void
populate(View *v)
{
	Dir **dv;
	Item *it;
	char **rows, **icons;
	int i, n;

	if(v->kind == Kdesk)
		return;
	if(v->editing != nil)
		renameend(v, 0);
	if(v->mode == Micons){
		if(v->list != nil){
			uxdelete(v->list);
			v->list = nil;
		}
		if(v->icons == nil){
			v->icons = uxiconview(v->w, "icons", contentrect(v));
			if(v->scroll != nil)
				uxsetparent(v->icons, v->scroll);
			uxhandler(v->icons, viewhandler, v);
			dragsetup(v);
		}else
			uxiconclear(v->icons);
		for(i = 0; i < v->nitem; i++)
			showitem(v, v->item[i]);
		markopen();
	}else{
		if(v->icons != nil){
			uxdelete(v->icons);
			v->icons = nil;
		}
		if(v->list == nil){
			v->list = uxlist(v->w, "list", contentrect(v));
			if(v->scroll != nil)
				uxsetparent(v->list, v->scroll);
			uxsetcolumns(v->list, coltitles, colwidths, nelem(coltitles));
			uxhandler(v->list, viewhandler, v);
		}
		for(i = 0; i < v->nrows; i++)
			free(v->rows[i]);
		free(v->rows);
		dv = fcsorted(v->f, v->sort);
		rows = femalloc((v->f->n+1)*sizeof(char*));
		icons = femalloc((v->f->n+1)*sizeof(char*));
		v->rows = femalloc((v->f->n+1)*sizeof(char*));
		n = 0;
		for(i = 0; i < v->f->n; i++){
			it = itembyname(v, dv[i]->name);
			if(it == nil)
				continue;
			rows[n] = row(it, dv[i]);
			icons[n] = it->icon;
			v->rows[n] = festrdup(it->id);
			n++;
		}
		v->nrows = n;
		uxsetitems(v->list, rows, n);
		uxsetitemicons(v->list, icons, n);
		free(icons);
		for(i = 0; i < n; i++)
			free(rows[i]);
		free(rows);
		free(dv);
	}
	viewstatus(v);
}

void
viewstatus(View *v)
{
	Item **sel;
	char *s, *sz;
	vlong tot;
	int i, n;

	if(v->status == nil)
		return;
	n = viewsel(v, &sel);
	if(n > 0)
		free(sel);
	if(v->err[0])
		s = smprint("can't read this folder: %s", v->err);
	else if(v->vol != nil && v->vol->state == Voffline)
		s = smprint("%s is offline: %s", v->vol->name, v->vol->err);
	else if(n > 0)
		s = smprint("%d item%s, %d selected", v->nitem, v->nitem == 1 ? "" : "s", n);
	else
		s = smprint("%d item%s", v->nitem, v->nitem == 1 ? "" : "s");
	uxsetvalue(v->status, s);
	free(s);
	if(v->space != nil){
		/* 9P has no free space to ask for: the size of the files here */
		tot = 0;
		if(v->f != nil && !v->err[0])
			for(i = 0; i < v->f->n; i++)
				if(!(v->f->d[i].qid.type & QTDIR))
					tot += v->f->d[i].length;
		sz = sizestr(tot);
		s = smprint("%s in folder", sz);
		uxsetvalue(v->space, v->err[0] ? "" : s);
		free(s);
		free(sz);
	}
	folderlayout(v);
}

/* the selected items; *items is malloc'd when the count is above 0 */
int
viewsel(View *v, Item ***items)
{
	char **ids;
	int *rows;
	int i, n, m;
	Item *it;

	*items = nil;
	m = 0;
	if(v->icons != nil){
		n = uxicongetsel(v->icons, &ids);
		if(n > 0)
			*items = femalloc(n*sizeof(Item*));
		for(i = 0; i < n; i++){
			if((it = itembyid(v, ids[i])) != nil)
				(*items)[m++] = it;
			free(ids[i]);
		}
		free(ids);
	}else if(v->list != nil){
		n = uxgetsel(v->list, &rows);
		if(n > 0)
			*items = femalloc(n*sizeof(Item*));
		for(i = 0; i < n; i++)
			if(rows[i] < v->nrows && (it = itembyid(v, v->rows[rows[i]])) != nil)
				(*items)[m++] = it;
		free(rows);
	}
	if(m == 0){
		free(*items);
		*items = nil;
	}
	return m;
}

void
viewselectall(View *v)
{
	char **ids;
	int i;

	if(v->icons != nil){
		ids = femalloc((v->nitem+1)*sizeof(char*));
		for(i = 0; i < v->nitem; i++)
			ids[i] = v->item[i]->id;
		uxiconselect(v->icons, ids, v->nitem);
		free(ids);
	}
	viewstatus(v);
}

void
viewselectname(View *v, char *name)
{
	Item *it;

	it = itembyname(v, name);
	if(it != nil && v->icons != nil)
		uxiconselect(v->icons, &it->id, 1);
	viewstatus(v);
}

/* read the folder again and show what changed */
void
viewrescan(View *v)
{
	Fcchange *c;
	Fcrec *recs;
	Dir *d;
	Item *it;
	char *l;
	int i, n, wasbad;

	if(v->kind != Kfolder)
		return;
	wasbad = v->err[0] != 0;
	n = fcrescan(v->f, &c);
	if(n < 0){
		if(strcmp(v->err, v->f->err) != 0){
			strecpy(v->err, v->err+sizeof v->err, v->f->err);
			viewstatus(v);
		}
		return;
	}
	v->err[0] = 0;
	if(n == 0){
		if(wasbad)
			viewstatus(v);
		return;
	}
	recs = nil;
	for(i = 0; i < n; i++)
		if(c[i].op == Cadd){
			recs = itemrecs(v->path);
			break;
		}
	for(i = 0; i < n; i++){
		switch(c[i].op){
		case Cadd:
			if(itembyname(v, c[i].name) != nil || hidden(v, c[i].name))
				break;
			d = fcfind(v->f, c[i].name);
			if(d == nil)
				break;
			it = entryitem(v, d, recs);
			if(!it->placed)
				it->pt = freecell(v);
			showitem(v, it);
			break;
		case Cdel:
			it = itembyname(v, c[i].name);
			if(it != nil)
				delitem(v, it);
			break;
		case Cren:
			it = itembyname(v, c[i].old);
			if(it == nil)
				break;	/* we renamed it ourselves */
			free(it->name);
			it->name = festrdup(c[i].name);
			itemsetpath(it, fcjoin(v->path, it->name));
			l = viewlabel(it);
			if(v->icons != nil)
				uxiconset(v->icons, it->id, l, nil, it->pt);
			free(l);
			break;
		}
	}
	fcchangefree(c, n);
	fcrecfree(recs);
	if(v->mode == Mlist)
		populate(v);
	else
		viewstatus(v);
	updatemenus();
}

void
viewsetmode(View *v, int mode)
{
	if(v->kind != Kfolder || v->mode == mode)
		return;
	v->mode = mode;
	populate(v);
	savewin(v);
}

/* like the classic Finder, sorting shows the list */
void
viewsetsort(View *v, int sort)
{
	if(v->kind != Kfolder)
		return;
	v->sort = sort;
	v->mode = Mlist;
	populate(v);
	savewin(v);
}

/* put every icon in a grid cell, in sort order */
void
viewcleanup(View *v)
{
	Dir **dv;
	Item *it;
	int i;

	if(v->icons == nil)
		return;
	for(i = 0; i < v->nitem; i++)
		v->item[i]->pt = ZP;
	if(v->kind == Kdesk){
		for(i = 0; i < v->nitem; i++){
			it = v->item[i];
			if(it->trash){
				it->pt = trashpt(v);
				it->placed = 0;
				uxiconset(v->icons, it->id, nil, nil, it->pt);
				continue;
			}
			it->pt = freecell(v);
			it->placed = 1;
			uxiconset(v->icons, it->id, nil, nil, it->pt);
			saveitem(v, it);
		}
		return;
	}
	dv = fcsorted(v->f, v->sort);
	for(i = 0; i < v->f->n; i++){
		it = itembyname(v, dv[i]->name);
		if(it == nil)
			continue;
		it->pt = freecell(v);
		it->placed = 1;
		uxiconset(v->icons, it->id, nil, nil, it->pt);
		saveitem(v, it);
	}
	free(dv);
}

/* the window was moved or resized */
static void
resized(View *v)
{
	int i;
	Item *it;

	v->r = uxwinrect(v->w);
	v->size = uxwinsize(v->w);
	if(v->kind != Kdesk && v->scroll != nil)
		folderlayout(v);
	else if(v->icons != nil)
		uxsetrect(v->icons, contentrect(v));
	if(v->kind == Kdesk){
		/* volumes nobody placed follow the right edge */
		for(i = 0; i < v->nitem; i++)
			if(!v->item[i]->placed)
				v->item[i]->pt = ZP;
		for(i = 0; i < v->nitem; i++){
			it = v->item[i];
			if(!it->placed){
				it->pt = it->trash ? trashpt(v) : freecell(v);
				uxiconset(v->icons, it->id, nil, nil, it->pt);
			}
		}
		return;
	}
	savewin(v);
}

void
openit(View *v, Item *it)
{
	Fev e;

	/* the same as File > Open on just this item */
	USED(v);
	memset(&e, 0, sizeof e);
	if(it->trash){
		trashopen();
		return;
	}
	if(v->kind == Ktrash)
		return;	/* things in the Trash don't open; Put Back first */
	if(it->vol != nil || it->dir){
		if(it->vol != nil && fcvolcheck(it->vol) != Vonline
		&& (it->vol->addr != nil || it->vol->srv != nil) && !it->vol->attached
		&& fcvolattach(it->vol) < 0){
			alerterr(it->name);
			return;
		}
		if(it->vol != nil && fcvolcheck(it->vol) != Vonline){
			alerterr(it->name);
			deskcheck();
			return;
		}
		if(it->vol != nil)
			deskcheck();	/* it may have just been attached */
		folderopen(it->path, 1);
		return;
	}
	{
		char *dst;
		int r;

		dst = nil;
		r = fcopen(ws, it->path, &dst);
		free(dst);
		if(r < 0)
			alerterr(it->name);
		else if(r == Ofolder)
			folderopen(it->path, 1);
	}
}

void
viewevent(View *v, Fev *e)
{
	Item *it;
	char *l;

	switch(e->type){
	case UXopen:
		if(e->ctl == v->list && v->list != nil){
			if(e->n >= 0 && e->n < v->nrows && (it = itembyid(v, v->rows[e->n])) != nil)
				openit(v, it);
		}else if(e->item != nil && (it = itembyid(v, e->item)) != nil)
			openit(v, it);
		break;
	case UXselection:
		if(v->edit != nil && e->ctl != v->edit)
			renameend(v, 1);
		if(v->kind == Ktrash)
			trashstatus(v);
		else
			viewstatus(v);
		front = v;	/* selecting in a window is working in it */
		updatemenus();
		break;
	case UXmove:
		if(e->item == nil || (it = itembyid(v, e->item)) == nil)
			break;
		it->pt = e->pt;
		folderlayout(v);
		if(it->trash || v->kind == Ktrash)
			break;	/* the Trash's place isn't saved */
		it->placed = 1;
		saveitem(v, it);
		break;
	case UXrename:
		if(e->item == nil || (it = itembyid(v, e->item)) == nil)
			break;
		if(it->trash || v->kind == Ktrash){
			l = viewlabel(it);
			uxiconset(v->icons, it->id, l, nil, it->pt);
			free(l);
			break;
		}
		if(it->vol != nil){
			l = viewlabel(it);
			uxiconset(v->icons, it->id, l, nil, it->pt);
			free(l);
			alert("A volume's name comes from the volume registry (uxmeta). Rename it there.", "OK", nil, nil);
			break;
		}
		itemrename(v, it, e->text);
		break;
	case UXsubmit:
		if(e->ctl == v->edit)
			renameend(v, 1);
		break;
	case UXclose:
		if(v->kind == Kfolder || v->kind == Ktrash)
			viewclose(v, v->kind == Kfolder);
		break;
	case UXresize:
		resized(v);
		break;
	case UXdrop:
		viewdrop(v, e);
		break;
	case UXdragend:
		viewrescan(v);
		break;
	}
}

static void
copydel(Alert *a, int choice)
{
	View *v;
	Item *it;

	if(choice != 2)
		return;
	if(fcrename(ws, a->path, a->arg, Fcopydel) < 0){
		alerterr(fcbasename(a->path));
		return;
	}
	for(v = views; v != nil; v = v->next)
		if(v == a->v)
			break;
	if(v == nil)
		return;
	/* the copy is a new file: rescanning shows it in its old place */
	{
		char *name;

		name = fcbasename(a->path);
		it = itembyname(v, name);
	}
	if(it != nil){
		Point pt;

		pt = it->pt;
		delitem(v, it);
		viewrescan(v);
		it = itembyname(v, a->arg);
		if(it != nil && !it->placed){
			it->pt = pt;
			if(v->icons != nil)
				uxiconset(v->icons, it->id, nil, nil, pt);
			saveitem(v, it);
		}
	}
}

/* rename it: the label already shows newname if the icon view was edited */
void
itemrename(View *v, Item *it, char *newname)
{
	char err[ERRMAX], *l, *msg;
	Alert *a;

	if(strcmp(it->name, newname) == 0)
		return;
	if(fcrename(ws, it->path, newname, 0) == 0){
		free(it->name);
		it->name = festrdup(newname);
		itemsetpath(it, fcjoin(v->path, newname));
		l = viewlabel(it);
		if(v->icons != nil)
			uxiconset(v->icons, it->id, l, nil, it->pt);
		free(l);
		if(v->mode == Mlist)
			populate(v);
		return;
	}
	rerrstr(err, sizeof err);
	/* put the old name back */
	l = viewlabel(it);
	if(v->icons != nil)
		uxiconset(v->icons, it->id, l, nil, it->pt);
	free(l);
	if(fcerrclass(err) == Enotsup && strstr(err, "copy and delete") != nil){
		msg = smprint("%s can't be renamed in place: %s\n\nMake a copy named %s and delete the original?",
			it->name, err, newname);
		a = alert(msg, "Cancel", "Copy and Delete", copydel);
		free(msg);
		a->path = festrdup(it->path);
		a->arg = festrdup(newname);
		a->v = v;
		return;
	}
	werrstr("%s", err);
	alerterr(it->name);
}

/* rename in place: a text field over the label of the one selected item */
void
renamestart(View *v)
{
	Item **sel;
	Item *it;
	Rectangle r;
	int n;

	if(v->edit != nil)
		renameend(v, 1);
	n = viewsel(v, &sel);
	if(n != 1){
		free(sel);
		return;
	}
	it = sel[0];
	free(sel);
	if(it->vol != nil || it->trash || v->kind == Ktrash)
		return;
	if(v->mode == Micons){
		r = Rect(it->pt.x - UXiconviewlabelmax/2 - 8, it->pt.y + UXiconlarge/2 + UXiconviewlabelgap,
			it->pt.x + UXiconviewlabelmax/2 + 8, it->pt.y + UXiconlarge/2 + UXiconviewlabelgap + UXfieldheight);
		r = rectaddpt(r, viewport(v));
	}else
		r = Rect(4, 0, contentrect(v).max.x-4, Statush);
	v->editing = it;
	v->edit = uxtextfield(v->w, "rename", "Name", r, 0);
	uxsetvalue(v->edit, it->name);
	uxhandler(v->edit, viewhandler, v);
	uxfocus(v->edit);
}

void
renameend(View *v, int commit)
{
	char *s;
	Item *it;
	Uxctl *c;

	c = v->edit;
	it = v->editing;
	if(c == nil)
		return;
	v->edit = nil;
	v->editing = nil;
	s = uxgetvalue(c);
	uxdelete(c);
	if(commit && it != nil && s[0] != 0)
		itemrename(v, it, s);
	free(s);
}

/* icon positions from uxmeta, for items whose record changed */
static void
reloadpos(View *v)
{
	Fcrec *recs, *r;
	Item *it;
	Point p;
	char *x, *y;
	int i;

	recs = itemrecs(v->path);
	for(i = 0; i < v->nitem; i++){
		it = v->item[i];
		r = recfor(recs, it->path);
		if(r == nil)
			continue;
		x = fcrecget(r, "x");
		y = fcrecget(r, "y");
		if(x == nil || y == nil || !x[0] || !y[0])
			continue;
		p = Pt(atoi(x), atoi(y));
		if(!eqpt(p, it->pt)){
			it->pt = p;
			it->placed = 1;
			if(v->icons != nil)
				uxiconset(v->icons, it->id, nil, nil, p);
		}
	}
	fcrecfree(recs);
}

/* a line from uxmeta's changes: SEQ set|del KIND KEY gen=N */
void
metachange(char *line)
{
	char *f[8], *dir, *x, *y;
	int nf, mode, sort;
	View *v;
	Item *it;
	Fcrec *r, *recs;

	nf = tokenize(line, f, nelem(f));
	if(nf >= 2 && (strcmp(f[1], "lost") == 0 || strcmp(f[1], "resync") == 0)){
		/* messages may be missing: read everything again */
		deskload();
		for(v = views; v != nil; v = v->next)
			if(v->kind == Kfolder){
				mode = winmode(v->path, &sort);
				if(mode != v->mode || sort != v->sort){
					v->mode = mode;
					v->sort = sort;
					populate(v);
				}
				reloadpos(v);
			}
		updatemenus();
		return;
	}
	if(nf < 4)
		return;
	if(strcmp(f[2], "volume") == 0){
		deskload();
		return;
	}
	if(strcmp(f[2], "folder") == 0 && (v = viewbypath(f[3])) != nil){
		mode = winmode(v->path, &sort);
		if(mode != v->mode || sort != v->sort){
			v->mode = mode;
			v->sort = sort;
			populate(v);
			updatemenus();
		}
		return;
	}
	if(strcmp(f[2], "item") != 0 || strcmp(f[1], "set") != 0)
		return;
	dir = fcdirname(f[3]);
	v = viewbypath(dir);
	free(dir);
	if(v == nil || (it = itembyname(v, fcbasename(f[3]))) == nil)
		return;
	recs = itemrecs(f[3]);
	for(r = recs; r != nil; r = r->next)
		if(strcmp(fcreckey(r), f[3]) == 0)
			break;
	if(r != nil){
		x = fcrecget(r, "x");
		y = fcrecget(r, "y");
		if(x && y && x[0] && y[0] && !eqpt(it->pt, Pt(atoi(x), atoi(y)))){
			it->pt = Pt(atoi(x), atoi(y));
			it->placed = 1;
			if(v->icons != nil)
				uxiconset(v->icons, it->id, nil, nil, it->pt);
		}
	}
	fcrecfree(recs);
}

/* drag and drop */

/* on libux's UI thread, with its lock held: offer the dragged icons' files */
static void
dragfn(Uxdrag *d, char **ids, int n, void *arg)
{
	View *v;
	Item *it;
	int i;

	v = arg;
	lock(&itemlk);
	for(i = 0; i < n; i++)
		if((it = itembyid(v, ids[i])) != nil && !it->trash && it->vol == nil && it->path[0])
			uxdragfile(d, it->path);
	unlock(&itemlk);
	uxdragops(d, "move,copy");
}

/* icon views are drag sources and take files dropped on them */
void
dragsetup(View *v)
{
	if(v->icons == nil)
		return;
	uxicondrag(v->icons, dragfn, v);
	if(v->kind != Ktrash)
		uxdroptypes(v->icons, "file", "move,copy");
}

/*
 * On libux's UI thread, inside the drop's handler: where the drop goes
 * (a folder icon under the pointer, the Trash, or the window's folder),
 * and what's dropped. Our own drags carry paths, which Finder's proc
 * moves or copies; another app's files are saved here and now, since
 * their data is only there while the handler runs.
 */
void
droplookup(View *v, Uxev *e, Fev *f)
{
	Item *it;
	Uxdropfile *df;
	char *p;
	int i, n;

	lock(&itemlk);
	it = e->item != nil ? itembyid(v, e->item) : nil;
	if(it != nil && it->trash)
		f->dest = festrdup("trash");
	else if(it != nil && it->dir && it->path[0])
		f->dest = festrdup(it->path);
	else if(v->kind == Kfolder && v->path != nil)
		f->dest = festrdup(v->path);
	unlock(&itemlk);
	if(f->dest == nil){
		uxdropresult(e, nil, "not supported: drop onto a volume, a folder or the Trash");
		return;
	}
	if(uxdragfromself(e)){
		f->paths = uxdragpaths(e, &f->npaths);
		return;
	}
	if(strcmp(f->dest, "trash") == 0){
		uxdropresult(e, nil, "not supported: only Finder's own items go in the Trash");
		free(f->dest);
		f->dest = nil;
		return;
	}
	/* from another app: save each file into the folder */
	n = uxdropfiles(e, &df);
	for(i = 0; i < n; i++){
		if(df[i].dir)
			continue;
		p = fcjoin(f->dest, df[i].name);
		if(access(p, AEXIST) == 0 || uxdropsave(e, i, p, df[i].mode) < 0){
			char err[ERRMAX];

			rerrstr(err, sizeof err);
			uxdropresult(e, nil, access(p, AEXIST) == 0 ? "exists" : err);
		}
		free(p);
	}
	uxdropfilesfree(df, n);
	uxdropresult(e, "copy", nil);
}

static View*
folderview(char *path)
{
	View *v;

	for(v = views; v != nil; v = v->next)
		if(v->kind == Kfolder && strcmp(v->path, path) == 0)
			return v;
	return nil;
}

/* in Finder's proc: carry out a drop of our own items */
void
viewdrop(View *v, Fev *e)
{
	View *w;
	char *p, *dir, *r;
	int i, move;
	Fcvol *sv, *dv;

	if(e->dest == nil)
		return;
	move = e->action == nil || strcmp(e->action, "move") == 0;
	for(i = 0; i < e->npaths; i++){
		p = e->paths[i];
		dir = fcdirname(p);
		if(strcmp(e->dest, "trash") == 0){
			r = fctrash(ws, p, 0);
			if(r == nil)
				alerterr(fcbasename(p));
			free(r);
		}else if(strcmp(dir, e->dest) != 0 && strcmp(p, e->dest) != 0){
			/*
			 * Dragging is asking to move (on one volume) or copy (onto
			 * another). 9P can't move between folders, so a move is a
			 * copy and then a delete; the user asked for exactly that.
			 */
			sv = fcvolof(ws, p);
			dv = fcvolof(ws, e->dest);
			if(move && sv == dv){
				if(fcmove(ws, p, e->dest, Fcopydel) < 0)
					alerterr(fcbasename(p));
			}else{
				r = fccopy(ws, p, e->dest, nil);
				if(r == nil)
					alerterr(fcbasename(p));
				free(r);
			}
		}
		if((w = folderview(dir)) != nil)
			viewrescan(w);
		free(dir);
	}
	if((w = folderview(e->dest)) != nil)
		viewrescan(w);
	if(v->kind == Kfolder)
		viewrescan(v);
	trashcheck();
}
