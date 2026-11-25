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
	char	**ids;	/* a list's item ids, by row */
	int	nids;
};

/* a drawn window, or an open overlay (ov: its node) drawn as a dialog */
struct Uw {
	char	*name;
	int	ov;
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

static Uc*
ucbyid(int id)
{
	Uw *w;
	int i;

	for(w = uws; w != nil; w = w->next)
		for(i = 0; i < w->n; i++)
			if(w->c[i].id == id)
				return &w->c[i];
	return nil;
}

static int
ison(Node *n, char *prop)
{
	char *v;

	v = getprop(n, prop);
	return v != nil && strcmp(v, "on") == 0;
}

/* a person changed a value: mirror it and send change (or select) */
static void
uservalue(Node *n, char *ev, char *attr, char *v, char *show)
{
	char *q, *a;

	setprop(n, "value", v, 0);
	if(show == nil)
		a = estrdup("via=user");
	else{
		q = quoteval(show);
		a = smprint("%s=%s via=user", attr, q);
		free(q);
	}
	emitnode(ev, winof(n), n, a, 1);
	free(a);
}

/* the items' ids a list's selection names, space-separated */
static char*
selids(Uc *u)
{
	int *sel, n, i;
	Fmt f;

	n = uxgetsel(u->c, &sel);
	fmtstrinit(&f);
	for(i = 0; i < n; i++)
		if(sel[i] < u->nids)
			fmtprint(&f, "%s%s", f.nfmt ? " " : "", u->ids[sel[i]]);
	free(sel);
	return fmtstrflush(&f);
}

/* a native control's event */
static void
ctlev(Uxev *e, void *arg)
{
	Node *n;
	Uc *u;
	char *v;

	if(e->type == UXactivate){
		lockmodel();
		n = nodebyid((int)(uintptr)arg);
		if(n != nil && !quitting && !ison(n, "disabled") && strcmp(n->type, "button") == 0)
			emitnode("click", winof(n), n, nil, 0);
		unlockmodel();
		if(n == nil || strcmp(n->type, "button") != 0)
			nodeev(e, arg);
		return;
	}
	if(e->via == UXctl)
		return;
	lockmodel();
	n = nodebyid((int)(uintptr)arg);
	u = ucbyid((int)(uintptr)arg);
	if(n == nil || u == nil || quitting){
		unlockmodel();
		return;
	}
	switch(e->type){
	case UXvalue:
		v = uxgetvalue(e->ctl);
		if(strcmp(n->type, "textarea") == 0 || issecret(n))
			uservalue(n, "change", nil, v, nil);
		else
			uservalue(n, "change", "value", v, v);
		free(u->val);
		u->val = estrdup(v);
		free(v);
		break;
	case UXsubmit:
		emitnode("submit", winof(n), n, nil, 0);
		break;
	case UXselection:
		v = selids(u);
		uservalue(n, "select", strcmp(n->type, "table") == 0 ? "row" : "value", v, v);
		free(v);
		break;
	case UXopen:
		v = selids(u);
		emitnode("open", winof(n), n, nil, 0);
		free(v);
		break;
	}
	sweep();
	unlockmodel();
}

/* an alert dialog's buttons, and every overlay's close box and Esc */
static void
ovev(Uxev *e, void *arg)
{
	Node *n;
	char *id, *ev;

	lockmodel();
	n = nodebyid((int)(uintptr)arg);
	if(n == nil || quitting){
		unlockmodel();
		return;
	}
	ev = nil;
	switch(e->type){
	case UXactivate:
		id = uxctlid(e->ctl);
		if(strstr(id, "-confirm") != nil)
			ev = "confirm";
		else if(strstr(id, "-cancel") != nil)
			ev = "cancel";
		break;
	case UXdefault:
		if(strcmp(n->type, "alert-dialog") == 0)
			ev = "confirm";
		break;
	case UXclose:
	case UXcancel:
		ev = strcmp(n->type, "alert-dialog") == 0 ? "cancel" : "close";
		break;
	}
	if(ev != nil){
		emitnode(ev, winof(n), n, nil, 0);
		/* an alert dialog answered closes; a dialog's close is a request */
		if(strcmp(ev, "close") != 0 || strcmp(n->type, "popover") == 0)
			setprop(n, "open", "off", 0);
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
	if(strcmp(n->type, "button") == 0){
		t = getprop(n, "label");
		return smprint("%s\001%d", t != nil ? t : "", ison(n, "disabled"));
	}
	if(strcmp(n->type, "checkbox") == 0 || strcmp(n->type, "switch") == 0){
		t = getprop(n, "label");
		r = getprop(n, "value");
		return smprint("%s\001%s\001%d", r != nil ? r : "off", t != nil ? t : "", ison(n, "disabled"));
	}
	if(strcmp(n->type, "input") == 0 || strcmp(n->type, "textarea") == 0){
		t = getprop(n, "value");
		return estrdup(t != nil ? t : "");
	}
	if(strcmp(n->type, "list") == 0){
		t = getprop(n, "items");
		return estrdup(t != nil ? t : "");
	}
	if(strcmp(n->type, "table") == 0){
		t = getprop(n, "columns");
		r = getprop(n, "rows");
		return smprint("%s\001%s", t != nil ? t : "", r != nil ? r : "");
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

static int
splitlines(char *s, char ***lp)
{
	char *buf, *p, *nl, **l;
	int n;

	l = nil;
	n = 0;
	buf = estrdup(s);
	for(p = buf; p != nil && *p; p = nl){
		nl = strchr(p, '\n');
		if(nl != nil)
			*nl++ = 0;
		if(p[0] == 0)
			continue;
		l = erealloc(l, (n+1)*sizeof(char*));
		l[n++] = estrdup(p);
	}
	free(buf);
	*lp = l;
	return n;
}

static void
freelist(char **l, int n)
{
	while(n > 0)
		free(l[--n]);
	free(l);
}

/* fields of a data line: tab-separated, or words */
static int
fields(char *line, char **f, int nf)
{
	if(strchr(line, '\t') != nil)
		return getfields(line, f, nf, 0, "\t");
	return tokenize(line, f, nf);
}

static void
clearids(Uc *u)
{
	freelist(u->ids, u->nids);
	u->ids = nil;
	u->nids = 0;
}

/* a list's items: id label [icon=] [disabled]; # groups and - separators aren't rows */
static void
setlist(Uc *u, char *items)
{
	char **l, **rows, *f[16];
	int n, i, k, nf;

	clearids(u);
	n = splitlines(items, &l);
	rows = emalloc((n+1)*sizeof(char*));
	k = 0;
	for(i = 0; i < n; i++){
		if(l[i][0] == '#' || strcmp(l[i], "-") == 0)
			continue;
		nf = fields(l[i], f, nelem(f));
		if(nf < 1)
			continue;
		u->ids = erealloc(u->ids, (u->nids+1)*sizeof(char*));
		u->ids[u->nids++] = estrdup(f[0]);
		rows[k++] = estrdup(nf > 1 ? f[1] : f[0]);
	}
	uxsetitems(u->c, rows, k);
	freelist(rows, k);
	freelist(l, n);
}

/* a table: columns (id label width ...) and rows (id, then a field per column) */
static void
settable(Uc *u, char *cols, char *rowsv)
{
	char **l, **titles, **rows, *f[32];
	int *w, n, i, j, nf, nc;
	Fmt fm;

	clearids(u);
	n = splitlines(cols, &l);
	titles = emalloc((n+1)*sizeof(char*));
	w = emalloc((n+1)*sizeof(int));
	nc = 0;
	for(i = 0; i < n; i++){
		nf = fields(l[i], f, nelem(f));
		if(nf < 1)
			continue;
		titles[nc] = estrdup(nf > 1 ? f[1] : f[0]);
		w[nc] = nf > 2 ? atoi(f[2]) : 100;
		nc++;
	}
	freelist(l, n);
	uxsetcolumns(u->c, titles, w, nc);
	freelist(titles, nc);
	free(w);
	n = splitlines(rowsv, &l);
	rows = emalloc((n+1)*sizeof(char*));
	for(i = 0; i < n; i++){
		nf = fields(l[i], f, nelem(f));
		u->ids = erealloc(u->ids, (u->nids+1)*sizeof(char*));
		u->ids[u->nids++] = estrdup(nf > 0 ? f[0] : "");
		fmtstrinit(&fm);
		for(j = 1; j < nf; j++)
			fmtprint(&fm, "%s%s", j > 1 ? "\t" : "", f[j]);
		rows[i] = fmtstrflush(&fm);
	}
	uxsetitems(u->c, rows, n);
	freelist(rows, n);
	freelist(l, n);
}

/* set a native control from its node's value string (valof) */
static void
setnative(Uc *u, Node *n, char *v)
{
	char *f[4], *buf;
	int nf;

	buf = estrdup(v);
	nf = getfields(buf, f, nelem(f), 0, "\001");
	if(strcmp(n->type, "button") == 0 && nf >= 2){
		uxsetlabel(u->c, f[0]);
		uxenable(u->c, !atoi(f[1]));
	}else if((strcmp(n->type, "checkbox") == 0 || strcmp(n->type, "switch") == 0) && nf >= 3){
		uxsetvalue(u->c, f[0]);
		uxsetlabel(u->c, f[1]);
		uxenable(u->c, !atoi(f[2]));
	}else if(strcmp(n->type, "input") == 0 || strcmp(n->type, "textarea") == 0){
		uxsetvalue(u->c, v);
		uxenable(u->c, !ison(n, "disabled"));
	}else if(strcmp(n->type, "list") == 0)
		setlist(u, v);
	else if(strcmp(n->type, "table") == 0 && nf >= 2)
		settable(u, f[0], f[1]);
	free(buf);
}

static int
isnative(char *t)
{
	return strcmp(t, "button") == 0 || strcmp(t, "checkbox") == 0 || strcmp(t, "switch") == 0
		|| strcmp(t, "input") == 0 || strcmp(t, "textarea") == 0 || strcmp(t, "list") == 0
		|| strcmp(t, "table") == 0;
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
	else if(isnative(n->type))
		setnative(u, n, v);
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
	else if(strcmp(n->type, "button") == 0)
		c = uxbutton(w->uw, id, "", ZR);
	else if(strcmp(n->type, "checkbox") == 0 || strcmp(n->type, "switch") == 0)
		c = uxcheckbox(w->uw, id, "", ZR);
	else if(strcmp(n->type, "input") == 0)
		c = uxtextfield(w->uw, id, "", ZR, ison(n, "secret") ? UXsecret : 0);
	else if(strcmp(n->type, "textarea") == 0)
		c = uxtextfield(w->uw, id, "", ZR, UXmultiline);
	else if(strcmp(n->type, "list") == 0 || strcmp(n->type, "table") == 0)
		c = uxlist(w->uw, id, ZR);
	else
		c = uxdiv(w->uw, id);
	if(parent != nil)
		uxsetparent(c, parent);
	uxhandler(c, isnative(n->type) ? ctlev : nodeev, (void*)(uintptr)n->id);
	w->c = erealloc(w->c, (w->n+1)*sizeof(Uc));
	u = &w->c[w->n++];
	memset(u, 0, sizeof *u);
	u->id = n->id;
	u->c = c;
	u->type = estrdup(n->type);
	u->val = v;
	if(isnative(n->type))
		setnative(u, n, v);
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
		freelist(w->c[i].ids, w->c[i].nids);
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

/* is an overlay of this type drawn, as a dialog window? */
static int
isdialog(char *t)
{
	return strcmp(t, "dialog") == 0 || strcmp(t, "alert-dialog") == 0 || strcmp(t, "sheet") == 0;
}

/* is u still wanted: its window shown, or its overlay there and open */
static int
wanted(Uw *u)
{
	Win *w;
	Node *n;

	if(u->ov == 0)
		return (w = winbyname(u->name)) != nil && w->shown;
	n = nodebyid(u->ov);
	return n != nil && ison(n, "open") && (w = winof(n)) != nil && w->shown;
}

static void
freeuw(Uw *u)
{
	clearctls(u);
	uxwinclose(u->uw);
	free(u->name);
	free(u->title);
	free(u->style);
	free(u->sig);
	free(u);
}

/* build u's controls from the kids of n in order */
static void
mkkids(Uw *u, Node *n, Uxctl *parent)
{
	int j, k;

	for(k = 0; k < n->nord; k++)
		for(j = 0; j < n->nkid; j++)
			if(n->kid[j]->id == n->ord[k])
				mkctl(u, n->kid[j], parent);
}

static char*
propor(Node *n, char *p, char *d)
{
	char *v;

	v = getprop(n, p);
	return v != nil && v[0] != 0 ? v : d;
}

/* an alert dialog: its title, description, and cancel and confirm buttons */
static void
mkalert(Uw *u, Node *n)
{
	char id[40];
	Uxctl *c, *row;

	snprint(id, sizeof id, "a%d-title", n->id);
	c = uxlabel(u->uw, id, propor(n, "title", ""), ZR);
	uxsetstyle(c, "text-lg font-bold");
	snprint(id, sizeof id, "a%d-desc", n->id);
	c = uxlabel(u->uw, id, propor(n, "description", ""), ZR);
	uxsetstyle(c, "font-normal");
	snprint(id, sizeof id, "a%d-row", n->id);
	row = uxdiv(u->uw, id);
	uxsetstyle(row, "h-flex justify-end gap=8");
	if(getprop(n, "cancel") != nil){
		snprint(id, sizeof id, "a%d-cancel", n->id);
		c = uxbutton(u->uw, id, getprop(n, "cancel"), ZR);
		uxsetparent(c, row);
		uxhandler(c, ovev, (void*)(uintptr)n->id);
	}
	snprint(id, sizeof id, "a%d-confirm", n->id);
	c = uxbutton(u->uw, id, propor(n, "confirm", "OK"), ZR);
	uxsetparent(c, row);
	uxhandler(c, ovev, (void*)(uintptr)n->id);
	uxdefault(u->uw, c);
}

static void
syncwin(Uw *u, Node *root, char *title, char *rs)
{
	Fmt f;
	char *s;

	if(strcmp(u->title, title) != 0){
		free(u->title);
		u->title = estrdup(title);
		uxwinsettitle(u->uw, title);
	}
	if(u->style == nil || strcmp(u->style, rs) != 0){
		free(u->style);
		u->style = estrdup(rs);
		restyle(nil, u->uw, "size-full", rs);
	}
	fmtstrinit(&f);
	sig(&f, root);
	if(strcmp(root->type, "alert-dialog") == 0)
		fmtprint(&f, "%s\001%s\001%s\001%s", propor(root, "title", ""),
			propor(root, "description", ""), propor(root, "confirm", ""), propor(root, "cancel", ""));
	s = fmtstrflush(&f);
	if(u->sig == nil || strcmp(u->sig, s) != 0){
		clearctls(u);
		if(strcmp(root->type, "alert-dialog") == 0)
			mkalert(u, root);
		else
			mkkids(u, root, nil);
		free(u->sig);
		u->sig = s;
	}else{
		free(s);
		update(u, root);
	}
}

static Uw*
newuw(char *name, int ov, Uxwin *uw, char *title)
{
	Uw *u;

	u = emalloc(sizeof *u);
	memset(u, 0, sizeof *u);
	u->name = estrdup(name);
	u->ov = ov;
	u->uw = uw;
	u->title = estrdup(title);
	u->next = uws;
	uws = u;
	return u;
}

static Uw*
uwof(char *name, int ov)
{
	Uw *u;

	for(u = uws; u != nil; u = u->next)
		if(u->ov == ov && strcmp(u->name, name) == 0)
			return u;
	return nil;
}

/* an open dialog, alert dialog or sheet: a dialog window over its window */
static void
syncover(Uw *parent, Win *w, Node *n)
{
	Uw *u;
	Rectangle pr, r;
	int dx, dy;
	char *title, *rs;

	title = propor(n, "title", "");
	u = uwof(w->name, n->id);
	if(u == nil){
		pr = uxwinrect(parent->uw);
		dx = atoi(propor(n, "width", strcmp(n->type, "sheet") == 0 ? propor(n, "size", "350") : "400"));
		dy = strcmp(n->type, "alert-dialog") == 0 ? 120 : 260;
		r.min = Pt(pr.min.x + (Dx(pr)-dx)/2, pr.min.y + 60);
		r.max = Pt(r.min.x+dx, r.min.y+dy);
		u = newuw(w->name, n->id, uxdialog(title, r), title);
		uxwinhandler(u->uw, ovev, (void*)(uintptr)n->id);
	}
	rs = n->style != nil && n->style[0] != 0 ? n->style : "v-flex p=16 gap=10";
	syncwin(u, n, title, rs);
}

static void
dosync(void)
{
	Uw **l, *u, *p;
	Win *w;
	Node *o;
	int i, j;

	for(l = &uws; (u = *l) != nil; ){
		if(!wanted(u)){
			*l = u->next;
			/* a closed dialog gives the front back to its window */
			if(u->ov != 0 && (p = uwof(u->name, 0)) != nil)
				uxwinfront(p->uw);
			freeuw(u);
			continue;
		}
		l = &u->next;
	}
	for(i = 0; i < app->nwin; i++){
		w = app->win[i];
		if(!w->shown)
			continue;
		u = uwof(w->name, 0);
		if(u == nil){
			u = newuw(w->name, 0, uxwindow("window", w->title, nopened == 0 ? ZR
				: Rect(60+30*nopened, 60+30*nopened, 60+30*nopened+w->w, 60+30*nopened+w->h)), w->title);
			nopened++;
			uxwinhandler(u->uw, winev, u->name);
		}
		syncwin(u, w->root, w->title, w->root->style[0] != 0 ? w->root->style : "flex-col");
		o = w->over;
		for(j = 0; o != nil && j < o->nkid; j++)
			if(isdialog(o->kid[j]->type) && ison(o->kid[j], "open"))
				syncover(u, w, o->kid[j]);
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
	Uc *c;
	Rectangle r;

	if(!drawing)
		return nil;
	w = winof(n);
	if(w == nil)
		return nil;
	u = uwof(w->name, 0);
	if(u == nil)
		return nil;
	if(n->parent == nil){
		Point p;

		p = uxwinsize(u->uw);
		return smprint("0 0 %d %d", p.x, p.y);
	}
	/* a node in an open overlay is local to its dialog window */

	if((c = ucbyid(n->id)) != nil){
		r = uxctlrect(c->c);
		return smprint("%d %d %d %d", r.min.x, r.min.y, Dx(r), Dy(r));
	}
	return nil;
}
