/*
 * Drawing (uxkit -d): mirror the uitree model into libux windows and
 * controls, laid out by the spike's flexbox, and send the clicks and
 * close boxes back as uitree events.
 *
 * uisync runs after every change, on the 9P proc with the model held.
 * libux's handlers run on its UI thread and take the model with
 * lockmodel before they touch it.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <ux.h>
#include "dat.h"

int	drawing;

typedef struct Uc Uc;
typedef struct Uw Uw;

struct Uc {
	int	id;
	Uxctl	*c;
	char	*type;
	char	*style;
	char	*val;
};

struct Uw {
	char	*name;
	Uxwin	*uw;
	char	*title;
	char	*style;
	char	*sig;
	Uc	*c;
	int	n;
	Uw	*next;
};

static Uw	*uws;
static int	nopened;

static int
drawn(Node *n, Node *k)
{
	int i;

	for(i = 0; i < n->nord; i++)
		if(n->ord[i] == k->id)
			return 1;
	return 0;
}

static void
sig(Fmt *f, Node *n)
{
	int i, j;

	fmtprint(f, "%d:%s[", n->id, n->type);
	for(i = 0; i < n->nord; i++)
		for(j = 0; j < n->nkid; j++)
			if(n->kid[j]->id == n->ord[i])
				sig(f, n->kid[j]);
	fmtprint(f, "]");
}

static int
hasword(char *list, char *w)
{
	char *buf, *f[32];
	int i, n;

	if(list == nil)
		return 0;
	buf = estrdup(list);
	n = getfields(buf, f, nelem(f), 1, " \t,");
	for(i = 0; i < n; i++)
		if(strcmp(f[i], w) == 0){
			free(buf);
			return 1;
		}
	free(buf);
	return 0;
}

static Win*
winof(Node *n)
{
	while(n->parent != nil)
		n = n->parent;
	return n->win;
}

static void
nodeev(Uxev *e, void *arg)
{
	Node *n;

	if(e->type != UXactivate)
		return;
	lockmodel();
	for(n = nodebyid((int)(uintptr)arg); n != nil; n = n->parent)
		if(hasword(getprop(n, "on"), "click"))
			break;
	if(n != nil && !quitting)
		emitnode("click", winof(n), n, nil, 0);
	unlockmodel();
}

static void
winev(Uxev *e, void *arg)
{
	Win *w;
	Point p;

	lockmodel();
	w = winbyname(arg);
	if(w != nil && !quitting){
		switch(e->type){
		case UXclose:
			closerequest(w, "titlebar");
			break;
		case UXresize:
			p = uxwinsize(e->win);
			w->w = p.x;
			w->h = p.y;
			break;
		}
	}
	sweep();
	unlockmodel();
}

/* style words one at a time, so a word libux doesn't draw drops alone */
static void
restyle(Uxctl *c, Uxwin *uw, char *pre, char *style)
{
	char *buf, *f[128];
	int i, n;

	if(c != nil)
		uxsetstyle(c, pre);
	else
		uxwinsetstyle(uw, pre);
	buf = estrdup(style);
	n = getfields(buf, f, nelem(f), 1, " \t\n");
	for(i = 0; i < n; i++){
		if(f[i][0] == 0)
			continue;
		if(c != nil)
			uxstyle(c, f[i]);
		else
			uxwinstyle(uw, f[i]);
	}
	free(buf);
}

static char*
valof(Node *n)
{
	char *t, *r;

	t = getprop(n, "text");
	if(strcmp(n->type, "styled-text") == 0){
		r = getprop(n, "runs");
		return smprint("%s\001%s", t != nil ? t : "", r != nil ? r : "");
	}
	if(strcmp(n->type, "svg") == 0 || strcmp(n->type, "img") == 0){
		t = getprop(n, "src");
		return estrdup(t != nil ? t : "");
	}
	return estrdup(t != nil ? t : "");
}

static char*
svgsrc(char *v)
{
	if(v[0] == 0)
		return "<svg viewBox='0 0 1 1'/>";
	return v;
}

static void
setval(Uc *u, Node *n)
{
	char *v, *r;

	v = valof(n);
	if(u->val != nil && strcmp(u->val, v) == 0){
		free(v);
		return;
	}
	free(u->val);
	u->val = v;
	if(strcmp(n->type, "text") == 0)
		uxsetvalue(u->c, v);
	else if(strcmp(n->type, "styled-text") == 0){
		r = strchr(v, '\001');
		*r = 0;
		uxsetstext(u->c, v, r+1);
		*r = '\001';
	}else if(strcmp(n->type, "svg") == 0)
		uxsetsvg(u->c, svgsrc(v));
}

static void
setstyle(Uc *u, Node *n)
{
	if(u->style != nil && strcmp(u->style, n->style) == 0)
		return;
	free(u->style);
	u->style = estrdup(n->style);
	/* uitree's text is regular weight unless styled bold; libux's labels are bold */
	restyle(u->c, nil, strcmp(n->type, "spacer") == 0 ? "flex-1"
		: strcmp(n->type, "text") == 0 ? "font-normal" : "", n->style);
}

static void
mkctl(Uw *w, Node *n, Uxctl *parent)
{
	char id[32];
	Uxctl *c;
	Uc *u;
	char *v;
	int i, j;

	snprint(id, sizeof id, "n%d", n->id);
	v = valof(n);
	if(strcmp(n->type, "text") == 0)
		c = uxlabel(w->uw, id, v, ZR);
	else if(strcmp(n->type, "styled-text") == 0){
		*strchr(v, '\001') = 0;
		c = uxstyledtext(w->uw, id, v, v+strlen(v)+1);
		v[strlen(v)] = '\001';
	}else if(strcmp(n->type, "svg") == 0)
		c = uxsvg(w->uw, id, svgsrc(v));
	else
		c = uxdiv(w->uw, id);
	if(parent != nil)
		uxsetparent(c, parent);
	uxhandler(c, nodeev, (void*)(uintptr)n->id);
	w->c = erealloc(w->c, (w->n+1)*sizeof(Uc));
	u = &w->c[w->n++];
	memset(u, 0, sizeof *u);
	u->id = n->id;
	u->c = c;
	u->type = estrdup(n->type);
	u->val = v;
	setstyle(u, n);
	for(i = 0; i < n->nord; i++)
		for(j = 0; j < n->nkid; j++)
			if(n->kid[j]->id == n->ord[i])
				mkctl(w, n->kid[j], c);
}

static void
clearctls(Uw *w)
{
	int i;

	for(i = w->n-1; i >= 0; i--){
		uxdelete(w->c[i].c);
		free(w->c[i].type);
		free(w->c[i].style);
		free(w->c[i].val);
	}
	free(w->c);
	w->c = nil;
	w->n = 0;
}

static void
update(Uw *w, Node *n)
{
	int i;

	for(i = 0; i < w->n; i++)
		if(w->c[i].id == n->id){
			setstyle(&w->c[i], n);
			setval(&w->c[i], n);
			break;
		}
	for(i = 0; i < n->nkid; i++)
		if(drawn(n, n->kid[i]))
			update(w, n->kid[i]);
}

static int	queued;
static void	dosync(void);

static void
syncnote(Uxev*, void*)
{
	lockmodel();
	queued = 0;
	dosync();
	unlockmodel();
}

/* after a change: mirror it on the UI thread, which owns the display */
void
uisync(void)
{
	if(!drawing || queued)
		return;
	queued = 1;
	uxcall(syncnote, nil);
}

static void
dosync(void)
{
	Uw **l, *u;
	Win *w;
	Fmt f;
	char *s, *rs;
	int i, j, k;

	for(l = &uws; (u = *l) != nil; ){
		if(winbyname(u->name) == nil || !winbyname(u->name)->shown){
			clearctls(u);
			uxwinclose(u->uw);
			*l = u->next;
			free(u->name);
			free(u->title);
			free(u->style);
			free(u->sig);
			free(u);
			continue;
		}
		l = &u->next;
	}
	for(i = 0; i < app->nwin; i++){
		w = app->win[i];
		if(!w->shown)
			continue;
		for(u = uws; u != nil; u = u->next)
			if(strcmp(u->name, w->name) == 0)
				break;
		if(u == nil){
			u = emalloc(sizeof *u);
			memset(u, 0, sizeof *u);
			u->name = estrdup(w->name);
			u->uw = uxwindow("window", w->title, nopened == 0 ? ZR
				: Rect(60+30*nopened, 60+30*nopened, 60+30*nopened+w->w, 60+30*nopened+w->h));
			nopened++;
			u->title = estrdup(w->title);
			uxwinhandler(u->uw, winev, u->name);
			u->next = uws;
			uws = u;
		}
		if(strcmp(u->title, w->title) != 0){
			free(u->title);
			u->title = estrdup(w->title);
			uxwinsettitle(u->uw, w->title);
		}
		rs = w->root->style[0] != 0 ? w->root->style : "flex-col";
		if(u->style == nil || strcmp(u->style, rs) != 0){
			free(u->style);
			u->style = estrdup(rs);
			restyle(nil, u->uw, "size-full", rs);
		}
		fmtstrinit(&f);
		sig(&f, w->root);
		s = fmtstrflush(&f);
		if(u->sig == nil || strcmp(u->sig, s) != 0){
			clearctls(u);
			for(k = 0; k < w->root->nord; k++)
				for(j = 0; j < w->root->nkid; j++)
					if(w->root->kid[j]->id == w->root->ord[k])
						mkctl(u, w->root->kid[j], nil);
			free(u->sig);
			u->sig = s;
		}else{
			free(s);
			update(u, w->root);
		}
	}
}

static void
appev(Uxev *e, void*)
{
	if(e->type != UXquit)
		return;
	lockmodel();
	if(!quitting)
		quitrequest("user");
	sweep();
	unlockmodel();
}

void
uistart(void)
{
	uxinit("uxkit", "uitree");
	uxapphandler(appev, nil);
}

void
uirun(void)
{
	uxmain();
}

/* a node's bounds from the last layout: nil if it isn't drawn */
char*
uibounds(Node *n)
{
	Win *w;
	Uw *u;
	Rectangle r;
	int i;

	if(!drawing)
		return nil;
	w = winof(n);
	if(w == nil)
		return nil;
	for(u = uws; u != nil; u = u->next)
		if(strcmp(u->name, w->name) == 0)
			break;
	if(u == nil)
		return nil;
	if(n->parent == nil){
		Point p;

		p = uxwinsize(u->uw);
		return smprint("0 0 %d %d", p.x, p.y);
	}
	for(i = 0; i < u->n; i++)
		if(u->c[i].id == n->id){
			r = uxctlrect(u->c[i].c);
			return smprint("%d %d %d %d", r.min.x, r.min.y, Dx(r), Dy(r));
		}
	return nil;
}
