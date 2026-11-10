/*
 * The merged view of all layers, and queries on it.
 */
#include <u.h>
#include <libc.h>
#include "meta.h"

static uint
hash(char *kind, char *key)
{
	uint h;
	uchar *p;

	h = 0;
	for(p = (uchar*)kind; *p; p++)
		h = h*31 + *p;
	h = h*31;
	for(p = (uchar*)key; *p; p++)
		h = h*31 + *p;
	return h % Nhash;
}

static char*
keyof(Rec *r)
{
	return r->t[0].val ? r->t[0].val : "";
}

Ent*
viewlook(View *v, char *kind, char *key)
{
	Ent *e;

	for(e = v->h[hash(kind, key)]; e; e = e->next)
		if(strcmp(e->kind, kind) == 0 && strcmp(e->key, key) == 0)
			return e;
	return nil;
}

static Ent*
viewadd(View *v, char *kind, char *key)
{
	Ent *e;
	uint h;

	if((e = viewlook(v, kind, key)) != nil)
		return e;
	h = hash(kind, key);
	e = emalloc(sizeof *e);
	e->kind = estrdup(kind);
	e->key = estrdup(key);
	e->next = v->h[h];
	v->h[h] = e;
	v->n++;
	return e;
}

void
viewfree(View *v)
{
	int i;
	Ent *e, *n;

	if(v == nil)
		return;
	for(i = 0; i < Nhash; i++)
		for(e = v->h[i]; e; e = n){
			n = e->next;
			free(e->kind);
			free(e->key);
			recfree(e->r);
			free(e);
		}
	free(v);
}

static int
isremoved(Rec *r)
{
	char *s;

	s = recget(r, "removed");
	return s != nil && strcmp(s, "yes") == 0;
}

/* merge one layer's record over the entry */
static void
merge(Ent *e, Rec *r)
{
	int i, j, n;
	char **vals;

	if(isremoved(r)){
		recfree(e->r);
		e->r = nil;
		return;
	}
	if(e->r == nil)
		e->r = recnew(e->kind, e->key);
	vals = emalloc(r->nt*sizeof vals[0]);
	for(i = 1; i < r->nt; i++){
		if(strcmp(r->t[i].attr, "removed") == 0)
			continue;
		for(j = 1; j < i; j++)
			if(strcmp(r->t[j].attr, r->t[i].attr) == 0)
				break;
		if(j < i)
			continue;	/* already done */
		n = 0;
		for(j = i; j < r->nt; j++)
			if(strcmp(r->t[j].attr, r->t[i].attr) == 0
			&& (r->t[j].val == nil || r->t[j].val[0] != 0))
				vals[n++] = r->t[j].val;
		if(n == 0)
			recdel(e->r, r->t[i].attr);	/* attr="" hides */
		else
			recset(e->r, r->t[i].attr, vals, n);
	}
	free(vals);
}

View*
viewbuild(Meta *m)
{
	View *v;
	Layer *l;
	Rec *r;
	int i, j;

	v = emalloc(sizeof *v);
	for(i = 0; i < m->nl; i++){
		l = m->l[i];
		for(j = 0; j < l->f.nc; j++){
			r = l->f.c[j].rec;
			if(r == nil || r->nt == 0 || !layeradmits(l, r->t[0].attr))
				continue;
			merge(viewadd(v, r->t[0].attr, keyof(r)), r);
		}
	}
	return v;
}

static int
entcmp(void *a, void *b)
{
	Ent *x, *y;
	int c;

	x = *(Ent**)a;
	y = *(Ent**)b;
	if((c = strcmp(x->kind, y->kind)) != 0)
		return c;
	return strcmp(x->key, y->key);
}

/* the present entries, sorted by kind then key */
Ent**
viewsorted(View *v, int *np)
{
	Ent **a, *e;
	int i, n;

	a = emalloc((v->n+1)*sizeof a[0]);
	n = 0;
	for(i = 0; i < Nhash; i++)
		for(e = v->h[i]; e; e = e->next)
			if(e->r != nil)
				a[n++] = e;
	qsort(a, n, sizeof a[0], entcmp);
	*np = n;
	return a;
}

static void
addchange(Change **ch, int *n, int op, Ent *e)
{
	*ch = erealloc(*ch, (*n+1)*sizeof **ch);
	(*ch)[*n].op = op;
	(*ch)[*n].kind = estrdup(e->kind);
	(*ch)[*n].key = estrdup(e->key);
	(*n)++;
}

int
viewdiff(View *old, View *new, Change **chp)
{
	Ent **a, *e;
	int i, na, n;
	Change *ch;

	ch = nil;
	n = 0;
	a = viewsorted(new, &na);
	for(i = 0; i < na; i++){
		e = old ? viewlook(old, a[i]->kind, a[i]->key) : nil;
		if(e == nil || !receq(e->r, a[i]->r))
			addchange(&ch, &n, Cset, a[i]);
	}
	free(a);
	if(old != nil){
		a = viewsorted(old, &na);
		for(i = 0; i < na; i++){
			e = viewlook(new, a[i]->kind, a[i]->key);
			if(e == nil || e->r == nil)
				addchange(&ch, &n, Cdel, a[i]);
		}
		free(a);
	}
	*chp = ch;
	return n;
}

void
changesfree(Change *ch, int n)
{
	int i;

	for(i = 0; i < n; i++){
		free(ch[i].kind);
		free(ch[i].key);
	}
	free(ch);
}

/*
 * Rebuild the view from the layers.  If anything changed, bump the
 * generation, regenerate plumbing rules when associations changed, and
 * tell the listener.  Returns the number of changes.
 */
int
metacommit(Meta *m)
{
	View *v;
	Change *ch;
	int i, n, assoc;

	v = viewbuild(m);
	n = viewdiff(m->v, v, &ch);
	viewfree(m->v);
	m->v = v;
	changesfree(m->ch, m->nch);
	m->ch = ch;
	m->nch = n;
	if(n == 0)
		return 0;
	m->gen++;
	metasavegen(m);
	assoc = 0;
	for(i = 0; i < n; i++)
		if(strcmp(ch[i].kind, "ext") == 0 || strcmp(ch[i].kind, "type") == 0
		|| strcmp(ch[i].kind, "app") == 0)
			assoc = 1;
	if(assoc)
		metaplumb(m);
	if(m->notify)
		m->notify(m, ch, n);
	return n;
}

/* is kind key present in the layers below l? */
int
lowerhas(Meta *m, Layer *l, char *kind, char *key)
{
	int i, has;
	Chunk *c;

	has = 0;
	for(i = 0; i < m->nl && m->l[i] != l; i++){
		if(!layeradmits(m->l[i], kind))
			continue;
		if((c = nfilefind(&m->l[i]->f, kind, key)) != nil)
			has = !isremoved(c->rec);
	}
	return has;
}

static int
hassuffix(char *s, char *suf)
{
	int n, m;

	n = strlen(s);
	m = strlen(suf);
	return m > 0 && m <= n && cistrcmp(s+n-m, suf) == 0;
}

static void
copyattr(Rec *dst, Rec *src, char *attr)
{
	char *v;

	if(src == nil || (v = recget(src, attr)) == nil)
		return;
	recdel(dst, attr);
	recadd(dst, attr, v);
}

/* how to show and open path: see meta.md, "resolve" */
Rec*
metaresolve(Meta *m, char *path)
{
	Rec *out, *ext, *type, *app, *item;
	Ent **a, *e;
	char *base, *open, *tn;
	int i, n, best;

	out = recnew("file", path);
	base = strrchr(path, '/');
	base = base ? base+1 : path;
	ext = nil;
	best = 0;
	a = viewsorted(m->v, &n);
	for(i = 0; i < n; i++)
		if(strcmp(a[i]->kind, "ext") == 0 && hassuffix(base, a[i]->key)
		&& strlen(a[i]->key) > best){
			ext = a[i]->r;
			best = strlen(a[i]->key);
		}
	free(a);
	type = nil;
	if(ext != nil){
		recadd(out, "ext", keyof(ext));
		if((tn = recget(ext, "type")) != nil && (e = viewlook(m->v, "type", tn)) != nil)
			type = e->r;
		if(tn != nil)
			recadd(out, "type", tn);
	}
	open = nil;
	if(ext != nil && (open = recget(ext, "open")) == nil && type != nil)
		open = recget(type, "open");
	if(open != nil)
		recadd(out, "open", open);
	copyattr(out, type, "icon");
	copyattr(out, ext, "icon");
	app = nil;
	if(open != nil && (e = viewlook(m->v, "app", open)) != nil)
		app = e->r;
	if(app != nil){
		copyattr(out, app, "port");
		copyattr(out, app, "exec");
	}else if(open != nil)
		recadd(out, "port", open);
	copyattr(out, type, "desc");
	copyattr(out, ext, "desc");
	if((e = viewlook(m->v, "item", path)) != nil && (item = e->r) != nil)
		copyattr(out, item, "icon");
	return out;
}

static int
matches(Rec *r, char *attr, char *val)
{
	int i;

	for(i = 0; i < r->nt; i++)
		if(strcmp(r->t[i].attr, attr) == 0){
			if(strcmp(val, "*") == 0)
				return 1;
			if(strcmp(r->t[i].val ? r->t[i].val : "", val) == 0)
				return 1;
		}
	return 0;
}

static int
under(char *key, char *path)
{
	int n;

	n = strlen(path);
	if(n > 1 && path[n-1] == '/')
		n--;
	if(strcmp(path, "/") == 0)
		return key[0] == '/';
	return strncmp(key, path, n) == 0 && (key[n] == 0 || key[n] == '/');
}

char*
metaquery(Meta *m, char *q)
{
	char *s, *f[64], *p;
	int nf, i, j, n, ok;
	Ent **a;
	Fmt fmt;
	Rec *r;
	Chunk *c;

	s = estrdup(q);
	nf = tokenize(s, f, nelem(f));
	if(nf == 0){
		free(s);
		werrstr("bad syntax: empty query");
		return nil;
	}
	fmtstrinit(&fmt);
	fmtprint(&fmt, "# gen %llud\n", m->gen);
	if(strcmp(f[0], "resolve") == 0){
		if(nf != 2)
			goto Usage;
		r = metaresolve(m, f[1]);
		recfmtw(&fmt, r, 0);
		recfree(r);
	}else if(strcmp(f[0], "explain") == 0){
		if(nf != 3)
			goto Usage;
		for(i = 0; i < m->nl; i++){
			if((c = nfilefind(&m->l[i]->f, f[1], f[2])) == nil)
				continue;
			fmtprint(&fmt, "# layer %s%s\n", m->l[i]->name,
				layeradmits(m->l[i], f[1]) ? "" : " (ignored)");
			recfmtw(&fmt, c->rec, 0);
		}
	}else if(strcmp(f[0], "under") == 0){
		if(nf != 3)
			goto Usage;
		a = viewsorted(m->v, &n);
		for(i = 0; i < n; i++)
			if(strcmp(a[i]->kind, f[1]) == 0 && under(a[i]->key, f[2]))
				recfmtw(&fmt, a[i]->r, 0);
		free(a);
	}else{
		/* KIND [KEY] [ATTR=VALUE...] or * [ATTR=VALUE...] */
		j = 1;
		p = nil;
		if(nf > 1 && strcmp(f[0], "*") != 0 && strchr(f[1], '=') == nil){
			p = f[1];
			j = 2;
		}
		for(i = j; i < nf; i++)
			if(strchr(f[i], '=') == nil)
				goto Usage;
		a = viewsorted(m->v, &n);
		for(i = 0; i < n; i++){
			if(strcmp(f[0], "*") != 0 && strcmp(a[i]->kind, f[0]) != 0)
				continue;
			if(p != nil && strcmp(a[i]->key, p) != 0)
				continue;
			ok = 1;
			for(j = (p ? 2 : 1); j < nf && ok; j++){
				char *eq = strchr(f[j], '=');
				*eq = 0;
				ok = matches(a[i]->r, f[j], eq+1);
				*eq = '=';
			}
			if(ok)
				recfmtw(&fmt, a[i]->r, 0);
		}
		free(a);
	}
	free(s);
	return fmtstrflush(&fmt);
Usage:
	free(s);
	free(fmtstrflush(&fmt));
	werrstr("bad syntax: %s", q);
	return nil;
}

char*
metadb(Meta *m)
{
	return metaquery(m, "*");
}
