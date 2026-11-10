/*
 * Updates written to ctl: parse every line, check rights, then apply
 * them all to the layers in memory, write the changed files, and commit
 * once.  Any failure puts the layers back as they were.
 */
#include <u.h>
#include <libc.h>
#include "meta.h"

typedef struct Op Op;
struct Op {
	char	*line;	/* tokenized in place */
	char	*f[256];
	int	nf;
	char	*layer;
	int	ifgen;
	uvlong	gen;
	char	*verb;
	char	**arg;	/* f+ai; reset after ops moves */
	int	ai;
	int	narg;
};

static char *layoutattrs[] = {
	"x", "y", "width", "height", "view", "sort", "scrollx", "scrolly", "desktop", nil,
};

static int
islayoutkind(char *k)
{
	return strcmp(k, "folder") == 0 || strcmp(k, "item") == 0;
}

static int
islayoutattr(char *a)
{
	char **p;

	for(p = layoutattrs; *p; p++)
		if(strcmp(*p, a) == 0)
			return 1;
	return 0;
}

static char*
attrname(char *tok, char *buf, int n)
{
	char *eq;

	eq = strchr(tok, '=');
	if(eq == nil)
		snprint(buf, n, "%s", tok);
	else
		snprint(buf, n, "%.*s", (int)(eq-tok), tok);
	return buf;
}

static char*
cleanpath(char *p)
{
	if(p[0] != '/'){
		werrstr("not an absolute path: %s", p);
		return nil;
	}
	return cleanname(p);
}

/* parse and check one line; nil op for a blank line */
static int
parseop(Meta *m, Op *o, int rights)
{
	int i;
	char buf[64], *a;
	Layer *l;

	o->nf = tokenize(o->line, o->f, nelem(o->f));
	if(o->nf == nelem(o->f)){
		werrstr("too many fields");
		return -1;
	}
	i = 0;
	for(; i < o->nf; i++){
		if(strncmp(o->f[i], "layer=", 6) == 0)
			o->layer = o->f[i]+6;
		else if(strncmp(o->f[i], "ifgen=", 6) == 0){
			o->ifgen = 1;
			o->gen = strtoull(o->f[i]+6, nil, 10);
		}else
			break;
	}
	if(i >= o->nf){
		werrstr("missing command");
		return -1;
	}
	o->verb = o->f[i];
	o->ai = i+1;
	o->arg = o->f+o->ai;
	o->narg = o->nf-i-1;
	if(rights == Rro){
		werrstr("permission denied");
		return -1;
	}
	if(o->layer != nil){
		if((l = metalayer(m, o->layer)) == nil){
			werrstr("no layer %s", o->layer);
			return -1;
		}
		if(!l->writable){
			werrstr("layer %s is read-only", o->layer);
			return -1;
		}
	}
	if(strcmp(o->verb, "reload") == 0){
		if(o->narg != 0)
			goto Usage;
		return 0;
	}
	if(strcmp(o->verb, "rename") == 0){
		if(o->narg != 2)
			goto Usage;
		if(cleanpath(o->arg[0]) == nil || cleanpath(o->arg[1]) == nil)
			return -1;
		if(strcmp(o->arg[0], "/") == 0){
			werrstr("can't rename /");
			return -1;
		}
		i = strlen(o->arg[0]);
		if(strncmp(o->arg[1], o->arg[0], i) == 0 && o->arg[1][i] == '/'){
			werrstr("can't move %s inside itself", o->arg[0]);
			return -1;
		}
		i = strlen(o->arg[1]);
		if(strncmp(o->arg[0], o->arg[1], i) == 0 && (o->arg[0][i] == '/' || strcmp(o->arg[1], "/") == 0)){
			werrstr("can't move %s over its parent", o->arg[0]);
			return -1;
		}
		return 0;
	}
	if(strcmp(o->verb, "set") != 0 && strcmp(o->verb, "unset") != 0
	&& strcmp(o->verb, "remove") != 0 && strcmp(o->verb, "delete") != 0){
		werrstr("unknown command %s", o->verb);
		return -1;
	}
	if(o->narg < 2)
		goto Usage;
	if(!validattr(o->arg[0]) || !validval(o->arg[1]))
		return -1;
	if(islayoutkind(o->arg[0]) && cleanpath(o->arg[1]) == nil)
		return -1;
	if((strcmp(o->verb, "remove") == 0 || strcmp(o->verb, "delete") == 0) && o->narg != 2)
		goto Usage;
	if((strcmp(o->verb, "set") == 0 || strcmp(o->verb, "unset") == 0) && o->narg < 3)
		goto Usage;
	for(i = 2; i < o->narg; i++){
		a = attrname(o->arg[i], buf, sizeof buf);
		if(strcmp(o->verb, "set") == 0 && strchr(o->arg[i], '=') == nil){
			werrstr("set needs attr=value, not %s", o->arg[i]);
			return -1;
		}
		if(!validattr(a))
			return -1;
		if(strcmp(a, o->arg[0]) == 0){
			werrstr("can't change a record's key");
			return -1;
		}
		if(strchr(o->arg[i], '=') && !validval(strchr(o->arg[i], '=')+1))
			return -1;
		if(!(rights & Rrw) && !islayoutkind(o->arg[0]) && !islayoutattr(a)){
			werrstr("permission denied");
			return -1;
		}
	}
	if(!(rights & Rrw) && !islayoutkind(o->arg[0])
	&& (strcmp(o->verb, "remove") == 0 || strcmp(o->verb, "delete") == 0)){
		werrstr("permission denied");
		return -1;
	}
	return 0;
Usage:
	werrstr("usage: %s", o->verb);
	return -1;
}

static Layer*
target(Meta *m, Op *o, char *kind)
{
	Layer *l;

	l = o->layer ? metalayer(m, o->layer) : metadefault(m, kind);
	if(l == nil)
		werrstr("no writable layer for %s", kind);
	return l;
}

static void
touch(Chunk *c)
{
	free(c->text);
	c->text = nil;
}

static int
doset(Meta *m, Op *o)
{
	Layer *l;
	Chunk *c;
	Rec *r;
	char buf[64], *a, **vals, *eq;
	int i, j, n;

	if((l = target(m, o, o->arg[0])) == nil)
		return -1;
	c = nfilefind(&l->f, o->arg[0], o->arg[1]);
	if(c == nil){
		nfileappend(&l->f, recnew(o->arg[0], o->arg[1]));
		c = &l->f.c[l->f.nc-1];
	}else
		touch(c);
	r = c->rec;
	recdel(r, "removed");
	vals = emalloc(o->narg*sizeof vals[0]);
	for(i = 2; i < o->narg; i++){
		a = attrname(o->arg[i], buf, sizeof buf);
		for(j = 2; j < i; j++)
			if(strncmp(o->arg[j], a, strlen(a)) == 0 && o->arg[j][strlen(a)] == '=')
				break;
		if(j < i)
			continue;
		n = 0;
		for(j = i; j < o->narg; j++)
			if(strncmp(o->arg[j], a, strlen(a)) == 0 && o->arg[j][strlen(a)] == '='){
				eq = strchr(o->arg[j], '=');
				vals[n++] = eq+1;
			}
		recset(r, a, vals, n);
	}
	free(vals);
	l->dirty = 1;
	return 0;
}

static int
dounset(Meta *m, Op *o)
{
	Layer *l;
	Chunk *c;
	int i;
	char buf[64];

	if((l = target(m, o, o->arg[0])) == nil)
		return -1;
	if((c = nfilefind(&l->f, o->arg[0], o->arg[1])) == nil)
		return 0;
	for(i = 2; i < o->narg; i++)
		recdel(c->rec, attrname(o->arg[i], buf, sizeof buf));
	if(c->rec->nt == 1)
		nfiledel(&l->f, c);
	else
		touch(c);
	l->dirty = 1;
	return 0;
}

static int
dodelete(Meta *m, Op *o, int hide)
{
	Layer *l;
	Chunk *c;
	Rec *r;

	if((l = target(m, o, o->arg[0])) == nil)
		return -1;
	while((c = nfilefind(&l->f, o->arg[0], o->arg[1])) != nil){
		nfiledel(&l->f, c);
		l->dirty = 1;
	}
	if(hide && lowerhas(m, l, o->arg[0], o->arg[1])){
		r = recnew(o->arg[0], o->arg[1]);
		recadd(r, "removed", "yes");
		nfileappend(&l->f, r);
		l->dirty = 1;
	}
	return 0;
}

static int
inside(char *k, char *path, int len)
{
	return strncmp(k, path, len) == 0 && (k[len] == 0 || k[len] == '/');
}

/*
 * Move folder and item records at old or below it to new.  Anything
 * already at new or below is stale (a rename can't overwrite), so it
 * goes first.
 */
static void
renamein(Layer *l, char *old, char *new)
{
	Rec **moved, *r;
	Chunk *c;
	int i, n, len, nlen;
	char *k, *nk;

	len = strlen(old);
	nlen = strlen(new);
	moved = nil;
	n = 0;
	for(i = 0; i < l->f.nc; ){
		r = l->f.c[i].rec;
		if(r != nil && islayoutkind(r->t[0].attr) && (k = r->t[0].val) != nil
		&& inside(k, new, nlen)){
			nfiledel(&l->f, &l->f.c[i]);
			l->dirty = 1;
			continue;
		}
		i++;
	}
	for(i = 0; i < l->f.nc; ){
		r = l->f.c[i].rec;
		if(r != nil && islayoutkind(r->t[0].attr) && (k = r->t[0].val) != nil
		&& inside(k, old, len)){
			nk = smprint("%s%s", new, k+len);
			free(r->t[0].val);
			r->t[0].val = nk;
			moved = erealloc(moved, (n+1)*sizeof moved[0]);
			moved[n++] = r;
			l->f.c[i].rec = nil;
			nfiledel(&l->f, &l->f.c[i]);
			continue;
		}
		i++;
	}
	for(i = 0; i < n; i++){
		while((c = nfilefind(&l->f, moved[i]->t[0].attr, moved[i]->t[0].val)) != nil)
			nfiledel(&l->f, c);
	}
	for(i = 0; i < n; i++)
		nfileappend(&l->f, moved[i]);
	free(moved);
	if(n > 0)
		l->dirty = 1;
}

static int
dorename(Meta *m, Op *o)
{
	Layer *l;
	int i;

	if(strcmp(o->arg[0], o->arg[1]) == 0)
		return 0;
	if(o->layer){
		renamein(metalayer(m, o->layer), o->arg[0], o->arg[1]);
		return 0;
	}
	for(i = 0; i < m->nl; i++){
		l = m->l[i];
		if(l->writable)
			renamein(l, o->arg[0], o->arg[1]);
	}
	return 0;
}

static void
rollback(Meta *m)
{
	int i;
	Layer *l;

	for(i = 0; i < m->nl; i++){
		l = m->l[i];
		if(!l->dirty)
			continue;
		nfilefree(&l->f);
		ndbparse(l->text, &l->f);
		l->dirty = 0;
	}
}

/* pick up outside edits; returns the number of changes committed */
int
metarefresh(Meta *m)
{
	if(metarescan(m) > 0)
		return metacommit(m);
	return 0;
}

/*
 * Apply the commands in text as one transaction.  Returns nil, or an
 * error string (which the caller frees).
 */
char*
metactl(Meta *m, char *text, int rights)
{
	char *s, *p, *nl, err[ERRMAX];
	Op *ops;
	int i, nops, reload;

	ops = nil;
	nops = 0;
	s = estrdup(text);
	for(p = s; p && *p; p = nl){
		nl = strchr(p, '\n');
		if(nl)
			*nl++ = 0;
		while(*p == ' ' || *p == '\t')
			p++;
		if(*p == 0 || *p == '#')
			continue;
		ops = erealloc(ops, (nops+1)*sizeof ops[0]);
		memset(&ops[nops], 0, sizeof ops[0]);
		ops[nops].line = p;
		if(parseop(m, &ops[nops], rights) < 0)
			goto Err;
		nops++;
	}
	if(nops == 0){
		werrstr("no command");
		goto Err;
	}
	for(i = 0; i < nops; i++)
		ops[i].arg = ops[i].f + ops[i].ai;

	/* outside edits first, so ifgen sees them and writes keep them */
	reload = 0;
	for(i = 0; i < nops; i++)
		if(strcmp(ops[i].verb, "reload") == 0)
			reload = 1;
	if(reload)
		for(i = 0; i < m->nl; i++)
			layerload(m->l[i]);
	metarescan(m);
	metacommit(m);
	for(i = 0; i < nops; i++)
		if(ops[i].ifgen && ops[i].gen != m->gen){
			werrstr("generation mismatch");
			goto Err;
		}

	for(i = 0; i < nops; i++){
		if(strcmp(ops[i].verb, "set") == 0){
			if(doset(m, &ops[i]) < 0)
				goto Back;
		}else if(strcmp(ops[i].verb, "unset") == 0){
			if(dounset(m, &ops[i]) < 0)
				goto Back;
		}else if(strcmp(ops[i].verb, "delete") == 0){
			if(dodelete(m, &ops[i], 0) < 0)
				goto Back;
		}else if(strcmp(ops[i].verb, "remove") == 0){
			if(dodelete(m, &ops[i], 1) < 0)
				goto Back;
		}else if(strcmp(ops[i].verb, "rename") == 0){
			if(dorename(m, &ops[i]) < 0)
				goto Back;
		}
	}
	for(i = 0; i < m->nl; i++)
		if(m->l[i]->dirty && layersave(m->l[i]) < 0){
			rerrstr(err, sizeof err);
			werrstr("writing %s: %s", m->l[i]->path, err);
			goto Back;
		}
	metacommit(m);
	free(ops);
	free(s);
	return nil;
Back:
	rerrstr(err, sizeof err);
	rollback(m);
	werrstr("%s", err);
Err:
	rerrstr(err, sizeof err);
	free(ops);
	free(s);
	return estrdup(err);
}
