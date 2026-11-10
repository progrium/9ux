/*
 * Get Info windows: what fcinfo knows about a file, folder or volume,
 * with the name editable (Enter renames).
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>
#include "fcore.h"
#include "desktop.h"
#include "uxstyle.h"

enum
{
	Width	= 380,
	Keyw	= 92,
};

static char *rows[] = {
	"kind",	"Kind:",
	"size",	"Size:",
	"where",	"Where:",
	"volume",	"Volume:",
	"modified",	"Modified:",
	"owner",	"Owner:",
	"group",	"Group:",
	"mode",	"Permissions:",
	"writable",	"Writable:",
	"opens",	"Opens with:",
};

static View*
infobypath(char *path)
{
	View *v;

	for(v = views; v != nil; v = v->next)
		if(v->kind == Kinfo && strcmp(v->path, path) == 0)
			return v;
	return nil;
}

static void
set(View *v, char *id, char *val)
{
	Uxctl *c;
	int i;

	c = nil;
	for(i = 0; i < v->nrows; i++)
		if(strcmp(uxctlid((Uxctl*)v->rows[i]), id) == 0)
			c = (Uxctl*)v->rows[i];
	if(c != nil)
		uxsetvalue(c, val);
}

/* d-rwxr-xr-x */
static void
modestr(char *buf, ulong m)
{
	char *rwx = "rwx";
	int i;

	buf[0] = (m & DMDIR) ? 'd' : (m & DMAPPEND) ? 'a' : (m & DMEXCL) ? 'l' : '-';
	for(i = 0; i < 9; i++)
		buf[1+i] = (m & (1<<(8-i))) ? rwx[i%3] : '-';
	buf[10] = 0;
}

static char*
sizestr(Fcinfo *fi)
{
	char *s, *t;

	if(fi->d->qid.type & QTDIR){
		t = smprint("%lld bytes", fi->size);
		s = smprint("%s in %ld item%s%s", t, fi->items, fi->items == 1 ? "" : "s",
			fi->complete ? "" : " (or more: stopped counting)");
		free(t);
		return s;
	}
	return smprint("%lld bytes", fi->size);
}

static void
fill(View *v)
{
	Fcinfo *fi;
	char *s, *o;
	Tm *tm;
	char buf[64];

	fi = fcinfo(ws, v->path, 1);
	if(fi == nil){
		rerrstr(buf, sizeof buf);
		set(v, "where", buf);
		v->gone = 1;
		return;
	}
	v->gone = 0;
	v->qid = fi->d->qid;
	v->mtime = fi->d->mtime;
	set(v, "kind", fi->kind);
	s = sizestr(fi);
	set(v, "size", s);
	free(s);
	set(v, "where", fi->where != nil ? fi->where : "");
	set(v, "volume", fi->vol != nil ? fi->vol->name : "none (not under a registered volume)");
	tm = localtime(fi->d->mtime);
	snprint(buf, sizeof buf, "%04d-%02d-%02d %02d:%02d:%02d",
		tm->year+1900, tm->mon+1, tm->mday, tm->hour, tm->min, tm->sec);
	set(v, "modified", buf);
	set(v, "owner", fi->d->uid);
	set(v, "group", fi->d->gid);
	modestr(buf, fi->d->mode);
	set(v, "mode", buf);
	set(v, "writable", fi->writable ? "yes" : "no");
	o = nil;
	if(fi->assoc != nil){
		s = fcrecget(fi->assoc, "open");
		if(s == nil)
			s = fcrecget(fi->assoc, "port");
		if(s != nil)
			o = s;
	}
	set(v, "opens", o != nil ? o : (fi->d->qid.type & QTDIR) ? "Desktop" : "the plumbing rules decide");
	fcinfofree(fi);
}

View*
infoopen(char *path)
{
	View *v;
	Rectangle r;
	int i, y, n;
	Uxctl *c, *ic;
	char *t, *icon, kid[32];
	Fcvol *vol;
	Dir *d;

	v = infobypath(path);
	if(v != nil){
		uxwinfront(v->w);
		return v;
	}
	d = dirstat(path);
	if(d == nil){
		alerterr(fcbasename(path));
		return nil;
	}
	v = femalloc(sizeof *v);
	v->kind = Kinfo;
	v->path = festrdup(path);
	vol = fcvolof(ws, path);
	if(vol != nil && strcmp(vol->path, path) == 0)
		v->title = festrdup(vol->name);
	else
		v->title = festrdup(fcbasename(path));
	n = nelem(rows)/2;
	r = cascade();
	r.max.x = r.min.x + Width;
	r.max.y = r.min.y + 2*UXdialogmargin + UXdialogiconsize + UXcontrolgroupgap + n*UXlistrowheight;
	t = smprint("%s Info", v->title);
	v->w = uxwindow("info", t, r);
	free(t);
	uxwinhandler(v->w, viewhandler, v);

	ic = uxiconview(v->w, "icon", Rect(UXdialogmargin, UXdialogmargin, UXdialogmargin+UXdialogiconsize, UXdialogmargin+UXdialogiconsize));
	if(vol != nil && strcmp(vol->path, path) == 0)
		icon = vol->icon != nil ? vol->icon : (vol->addr || vol->srv) ? "disk-remote" : "disk";
	else
		icon = (d->qid.type & QTDIR) ? "folder" : (d->mode & 0111) ? "app" : "doc";
	uxiconadd(ic, "icon", "", icon, Pt(UXdialogiconsize/2, UXdialogiconsize/2));
	free(d);
	v->name = uxtextfield(v->w, "name", "Name", Rect(UXdialogtextx, UXdialogmargin+6, Width-UXdialogmargin, UXdialogmargin+6+UXfieldheight), 0);
	uxsetvalue(v->name, v->title);
	if(vol != nil && strcmp(vol->path, path) == 0)
		uxenable(v->name, 0);
	uxhandler(v->name, viewhandler, v);

	/* rows: a key label and a value label each; v->rows holds the values */
	v->rows = femalloc(n*sizeof(char*));
	y = 2*UXdialogmargin + UXdialogiconsize;
	for(i = 0; i < n; i++){
		snprint(kid, sizeof kid, "%s-key", rows[2*i]);
		uxlabel(v->w, kid, rows[2*i+1], Rect(UXdialogmargin, y, UXdialogmargin+Keyw, y+UXlistrowheight));
		c = uxlabel(v->w, rows[2*i], "", Rect(UXdialogmargin+Keyw+8, y, Width-UXdialogmargin, y+UXlistrowheight));
		v->rows[i] = (char*)c;
		y += UXlistrowheight;
	}
	v->nrows = n;
	v->next = views;
	views = v;
	fill(v);
	return v;
}

/* show it again if the file changed */
void
inforefresh(View *v)
{
	Dir *d;

	d = dirstat(v->path);
	if(d == nil){
		if(!v->gone){
			set(v, "where", "(gone: no longer there)");
			v->gone = 1;
		}
		return;
	}
	if(v->gone || d->qid.path != v->qid.path || d->qid.vers != v->qid.vers || d->mtime != v->mtime)
		fill(v);
	free(d);
}

static void
infoclose(View *v)
{
	View **l;

	for(l = &views; *l != nil; l = &(*l)->next)
		if(*l == v){
			*l = v->next;
			break;
		}
	uxwinclose(v->w);
	free(v->rows);	/* the controls go with the window */
	free(v->path);
	free(v->title);
	free(v);
}

static void
inforename(View *v)
{
	char *s, *np, *t;
	View *fv;
	Item *it;

	s = uxgetvalue(v->name);
	if(s[0] == 0 || strcmp(s, fcbasename(v->path)) == 0){
		free(s);
		return;
	}
	if(fcrename(ws, v->path, s, 0) < 0){
		alerterr(fcbasename(v->path));
		uxsetvalue(v->name, v->title);
		free(s);
		return;
	}
	t = fcdirname(v->path);
	np = fcjoin(t, s);
	/* an open folder window shows the new name at once */
	if((fv = viewbypath(t)) != nil && (it = itembyname(fv, fcbasename(v->path))) != nil){
		free(it->name);
		it->name = festrdup(s);
		free(it->path);
		it->path = festrdup(np);
		if(fv->icons != nil)
			uxiconset(fv->icons, it->id, s, nil, it->pt);
	}
	free(t);
	free(v->path);
	v->path = np;
	free(v->title);
	v->title = s;
	t = smprint("%s Info", s);
	uxwinsettitle(v->w, t);
	free(t);
	fill(v);
}

void
infoevent(View *v, Fev *e)
{
	switch(e->type){
	case UXsubmit:
		inforename(v);
		break;
	case UXclose:
		infoclose(v);
		break;
	}
}
