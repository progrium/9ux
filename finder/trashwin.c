/*
 * The Trash on the desktop and its window. Each volume keeps its own
 * Trash (trash.c); the desktop's Trash shows them all together, and
 * says what each volume's Trash can't do.
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
	Statush	= 18,
};

static long	ntrash = -1;	/* items in every volume's Trash, when last counted */
static char	*trashsig;	/* what the Trash window shows, to see changes */

/* the Trash sits in the desktop's bottom right corner */
Point
trashpt(View *v)
{
	return Pt(v->size.x - UXdesktopmargin - UXiconviewcellw/2,
		v->size.y - UXdesktopmargin - UXiconviewcellh + UXiconlarge/2);
}

static View*
trashview(void)
{
	View *v;

	for(v = views; v != nil; v = v->next)
		if(v->kind == Ktrash)
			return v;
	return nil;
}

/* every trashed item on the desktop's volumes; *sig describes them */
static long
listall(Fctrashitem ***all, char **sig)
{
	Fctrashitem *t, **a;
	long n, m, i;
	int j;
	Fmt f;

	a = nil;
	n = 0;
	fmtstrinit(&f);
	for(j = 0; j < ws->nvol; j++){
		if(!ws->vol[j]->desktop || ws->vol[j]->state == Voffline)
			continue;
		m = fctrashlist(ws, ws->vol[j], &t);
		if(m <= 0)
			continue;
		a = realloc(a, (n+m)*sizeof(Fctrashitem*));
		for(i = 0; i < m; i++){
			/* keep the item; the array goes */
			a[n] = femalloc(sizeof(Fctrashitem));
			*a[n] = t[i];
			fmtprint(&f, "%s\n", t[i].path);
			n++;
		}
		free(t);
	}
	*all = a;
	*sig = fmtstrflush(&f);
	return n;
}

static void
freeall(Fctrashitem **a, long n)
{
	long i;

	for(i = 0; i < n; i++){
		free(a[i]->path);
		free(a[i]->name);
		free(a[i]->origin);
		free(a[i]);
	}
	free(a);
}

void
trashstatus(View *v)
{
	char *lim, *s;
	int j;

	lim = nil;
	for(j = 0; j < ws->nvol && lim == nil; j++)
		if(ws->vol[j]->desktop && ws->vol[j]->state != Voffline)
			lim = fctrashlimit(ws, ws->vol[j]);
	s = smprint("%d item%s. %s", v->nitem, v->nitem == 1 ? "" : "s", lim != nil ? lim : "");
	uxsetvalue(v->status, s);
	free(s);
	free(lim);
}

/* (re)fill the Trash window */
void
trashitems(View *v)
{
	Fctrashitem **a;
	char *sig;
	long n, i;
	Item *it;
	Dir *d;

	n = listall(&a, &sig);
	if(trashsig != nil && strcmp(sig, trashsig) == 0 && v->nitem == n){
		free(sig);
		freeall(a, n);
		return;
	}
	free(trashsig);
	trashsig = sig;
	uxiconclear(v->icons);
	lock(&itemlk);
	for(i = 0; i < v->nitem; i++){
		free(v->item[i]->id);
		free(v->item[i]->name);
		free(v->item[i]->path);
		free(v->item[i]->icon);
		free(v->item[i]);
	}
	v->nitem = 0;
	unlock(&itemlk);
	for(i = 0; i < n; i++){
		it = femalloc(sizeof *it);
		it->id = smprint("i%d", ++v->nextid);
		it->name = festrdup(a[i]->name);
		it->path = festrdup(a[i]->path);
		d = dirstat(it->path);
		it->dir = d != nil && (d->qid.type & QTDIR);
		free(d);
		it->icon = festrdup(it->dir ? "folder" : "doc");
		lock(&itemlk);
		v->item = realloc(v->item, (v->nitem+1)*sizeof(Item*));
		v->item[v->nitem++] = it;
		unlock(&itemlk);
		it->pt = freecell(v);
		uxiconadd(v->icons, it->id, it->name, it->icon, it->pt);
	}
	freeall(a, n);
	trashstatus(v);
}

View*
trashopen(void)
{
	View *v;

	v = trashview();
	if(v != nil){
		uxwinfront(v->w);
		setfront(v);
		return v;
	}
	v = femalloc(sizeof *v);
	v->kind = Ktrash;
	v->title = festrdup("Trash");
	v->mode = Micons;
	v->w = uxwindow("trash", "Trash", cascade());
	v->r = uxwinrect(v->w);
	v->size = uxwinsize(v->w);
	uxwinhandler(v->w, viewhandler, v);
	v->status = uxlabel(v->w, "status", "", Rect(8, 2, v->size.x-8, Statush-2));
	v->icons = uxiconview(v->w, "icons", Rect(0, Statush, v->size.x, v->size.y));
	uxhandler(v->icons, viewhandler, v);
	dragsetup(v);
	v->next = views;
	views = v;
	free(trashsig);
	trashsig = nil;
	trashitems(v);
	setfront(v);
	return v;
}

/* the desktop's Trash icon: empty or full; and the window, if open */
void
trashcheck(void)
{
	Fctrashitem **a;
	char *sig, *icon;
	long n;
	int i;
	View *v;

	n = listall(&a, &sig);
	freeall(a, n);
	free(sig);
	if(desk != nil){
		icon = n > 0 ? "trash-full" : "trash";
		for(i = 0; i < desk->nitem; i++)
			if(desk->item[i]->trash && strcmp(desk->item[i]->icon, icon) != 0){
				free(desk->item[i]->icon);
				desk->item[i]->icon = festrdup(icon);
				uxiconset(desk->icons, desk->item[i]->id, nil, icon, desk->item[i]->pt);
			}
	}
	ntrash = n;
	if((v = trashview()) != nil)
		trashitems(v);
}

static void
deleteit(Alert *a, int choice)
{
	View *v;

	if(choice != 2)
		return;
	if(fcdelete(ws, a->path, Frecursive) < 0)
		alerterr(fcbasename(a->path));
	for(v = views; v != nil; v = v->next)
		if(v == a->v && v->kind == Kfolder)
			viewrescan(v);
}

/* File > Move to Trash; a volume that can't hold a Trash offers Delete */
void
totrash(View *v, Item *it)
{
	char *r, err[ERRMAX], *msg;
	Alert *a;

	if(it->vol != nil || it->trash || v->kind == Ktrash)
		return;
	r = fctrash(ws, it->path, 0);
	if(r != nil){
		free(r);
		viewrescan(v);
		trashcheck();
		return;
	}
	rerrstr(err, sizeof err);
	if(fcerrclass(err) == Enotsup){
		msg = smprint("%s can't go in the Trash: %s\n\nDelete it now? This can't be undone.", it->name, err);
		a = alert(msg, "Cancel", "Delete", deleteit);
		free(msg);
		a->path = festrdup(it->path);
		a->v = v;
		return;
	}
	werrstr("%s", err);
	alerterr(it->name);
}

/* File > Put Back, in the Trash window */
void
putback(View *v, Item *it)
{
	char *r;
	View *w;

	if(v->kind != Ktrash)
		return;
	r = fcputback(ws, it->path);
	if(r == nil){
		alerterr(it->name);
		return;
	}
	{
		char *dir;

		dir = fcdirname(r);
		if((w = viewbypath(dir)) != nil)
			viewrescan(w);
		free(dir);
	}
	free(r);
	trashcheck();
}

static void
empty(Alert*, int choice)
{
	int j;

	if(choice != 2)
		return;
	for(j = 0; j < ws->nvol; j++)
		if(ws->vol[j]->desktop && ws->vol[j]->state != Voffline)
			if(fcemptytrash(ws, ws->vol[j]) < 0)
				alerterr(ws->vol[j]->name);
	trashcheck();
}

/* Special > Empty Trash: asks first */
void
emptytrash(void)
{
	char *msg;

	if(ntrash <= 0)
		return;
	msg = smprint("The Trash has %ld item%s. Delete %s for good? This can't be undone.",
		ntrash, ntrash == 1 ? "" : "s", ntrash == 1 ? "it" : "them");
	alert(msg, "Cancel", "Empty Trash", empty);
	free(msg);
}
