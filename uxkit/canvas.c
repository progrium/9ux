/*
 * canvas: a retained display list in ops (components.md § Data file
 * formats), changed by item id on the node's ctl.
 *
 *	set ID GEOMETRY... [ATTR=V...]	(or update)
 *	add KIND ID GEOMETRY... [ATTR=V...]
 *	del ID...
 *	raise ID / lower ID
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "dat.h"

static char *kinds[] = {
	"rect", "circle", "ellipse", "line", "path", "polyline", "polygon",
	"text", "image", "group", nil,
};

static int
iskind(char *s)
{
	int i;

	for(i = 0; kinds[i] != nil; i++)
		if(strcmp(kinds[i], s) == 0)
			return 1;
	return 0;
}

/* an ops line: kind and id are its first two words */
char*
checkopsline(char **f, int n)
{
	if(n < 2 || !iskind(f[0]))
		return smprint("bad value: ops kind %s", n > 0 ? f[0] : "");
	return nil;
}

typedef struct Ops Ops;
struct Ops {
	char	**line;
	int	n;
};

static void
opsread(Node *n, Ops *o)
{
	char *v, *buf, *p, *nl;

	o->line = nil;
	o->n = 0;
	v = getprop(n, "ops");
	if(v == nil)
		return;
	buf = estrdup(v);
	for(p = buf; p != nil && *p; p = nl){
		nl = strchr(p, '\n');
		if(nl != nil)
			*nl++ = 0;
		if(p[0] == 0)
			continue;
		o->line = erealloc(o->line, (o->n+1)*sizeof(char*));
		o->line[o->n++] = estrdup(p);
	}
	free(buf);
}

static void
opswrite(Node *n, Ops *o)
{
	Fmt f;
	int i;

	fmtstrinit(&f);
	for(i = 0; i < o->n; i++){
		fmtprint(&f, "%s%s", i > 0 ? "\n" : "", o->line[i]);
		free(o->line[i]);
	}
	free(o->line);
	setprop(n, "ops", fmtstrflush(&f), 0);
	uisync();
}

static void
opsfree(Ops *o)
{
	int i;

	for(i = 0; i < o->n; i++)
		free(o->line[i]);
	free(o->line);
}

/* the index of the item with that id, or -1 */
static int
opsfind(Ops *o, char *id)
{
	char *f[3], *s;
	int i, n;

	for(i = 0; i < o->n; i++){
		s = estrdup(o->line[i]);
		n = tokenize(s, f, nelem(f));
		if(n >= 2 && strcmp(f[1], id) == 0){
			free(s);
			return i;
		}
		free(s);
	}
	return -1;
}

static void
addtok(Fmt *f, char *s)
{
	char *q;

	q = quoteval(s);
	fmtprint(f, "%s%s", f->nfmt ? " " : "", q);
	free(q);
}

/* set: new geometry and attrs over the old item; old attrs not named stay */
static char*
opsset(char *old, Tok *t, int nt)
{
	char *s, *f[64], *eq;
	int n, i, j, l;
	Fmt fm;

	s = estrdup(old);
	n = tokenize(s, f, nelem(f));
	fmtstrinit(&fm);
	addtok(&fm, f[0]);
	addtok(&fm, f[1]);
	for(i = 0; i < nt; i++)
		if(strchr(t[i].s, '=') == nil)
			addtok(&fm, t[i].s);
	/* an old word that isn't a number (a text's label) stays */
	for(j = 2; j < n; j++)
		if(strcmp(f[0], "text") == 0 && strchr(f[j], '=') == nil && strtod(f[j], &eq) == 0 && eq == f[j])
			addtok(&fm, f[j]);
	for(j = 2; j < n; j++){
		if((eq = strchr(f[j], '=')) == nil)
			continue;
		l = eq - f[j] + 1;
		for(i = 0; i < nt; i++)
			if(strncmp(t[i].s, f[j], l) == 0)
				break;
		if(i == nt)
			addtok(&fm, f[j]);
	}
	for(i = 0; i < nt; i++)
		if(strchr(t[i].s, '=') != nil)
			addtok(&fm, t[i].s);
	free(s);
	return fmtstrflush(&fm);
}

/* a canvas's ctl verb, or (char*)-1 when v isn't one */
char*
canvasverb(Node *n, Tok *t, int nt)
{
	Ops o;
	Fmt fm;
	char *v, *tmp;
	int i, k;

	v = t[0].s;
	if(strcmp(v, "set") != 0 && strcmp(v, "update") != 0 && strcmp(v, "add") != 0
	&& strcmp(v, "del") != 0 && strcmp(v, "raise") != 0 && strcmp(v, "lower") != 0)
		return (char*)-1;
	opsread(n, &o);
	if(strcmp(v, "add") == 0){
		if(nt < 3 || !iskind(t[1].s)){
			opsfree(&o);
			return smprint("bad value: add KIND ID GEOMETRY...");
		}
		if(opsfind(&o, t[2].s) >= 0){
			opsfree(&o);
			return smprint("in use: item %s", t[2].s);
		}
		fmtstrinit(&fm);
		for(i = 1; i < nt; i++)
			addtok(&fm, t[i].s);
		o.line = erealloc(o.line, (o.n+1)*sizeof(char*));
		o.line[o.n++] = fmtstrflush(&fm);
		opswrite(n, &o);
		return nil;
	}
	if(nt < 2){
		opsfree(&o);
		return smprint("bad syntax: %s ID", v);
	}
	if(strcmp(v, "del") == 0){
		for(i = 1; i < nt; i++)
			if(opsfind(&o, t[i].s) < 0){
				opsfree(&o);
				return smprint("not found: item %s", t[i].s);
			}
		for(i = 1; i < nt; i++){
			k = opsfind(&o, t[i].s);
			free(o.line[k]);
			memmove(o.line+k, o.line+k+1, (o.n-k-1)*sizeof(char*));
			o.n--;
		}
		opswrite(n, &o);
		return nil;
	}
	if((k = opsfind(&o, t[1].s)) < 0){
		opsfree(&o);
		return smprint("not found: item %s", t[1].s);
	}
	if(strcmp(v, "raise") == 0 || strcmp(v, "lower") == 0){
		tmp = o.line[k];
		memmove(o.line+k, o.line+k+1, (o.n-k-1)*sizeof(char*));
		if(strcmp(v, "raise") == 0)
			o.line[o.n-1] = tmp;
		else{
			memmove(o.line+1, o.line, (o.n-1)*sizeof(char*));
			o.line[0] = tmp;
		}
		opswrite(n, &o);
		return nil;
	}
	tmp = opsset(o.line[k], t+2, nt-2);
	free(o.line[k]);
	o.line[k] = tmp;
	opswrite(n, &o);
	return nil;
}
