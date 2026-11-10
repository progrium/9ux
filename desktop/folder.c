/*
 * The directory model: a folder's entries, kept sorted by name, and
 * change detection by rescanning and comparing.
 *
 * Qids are used only as hints within one session: an entry that
 * disappears while another with the same qid.path appears in the same
 * rescan is reported as a rename. Nothing here assumes qids survive a
 * remount or a server restart (design §4).
 */
#include <u.h>
#include <libc.h>
#include "dcore.h"

static int
namecmp(void *a, void *b)
{
	return strcmp(((Dir*)a)->name, ((Dir*)b)->name);
}

static long
scan(char *path, Dir **dp, Dir **self)
{
	int fd;
	long n;
	Dir *d;

	fd = open(path, OREAD);
	if(fd < 0)
		return -1;
	d = dirfstat(fd);
	if(d == nil){
		close(fd);
		return -1;
	}
	if((d->qid.type & QTDIR) == 0){
		free(d);
		close(fd);
		werrstr("bad value: %s is not a folder", path);
		return -1;
	}
	n = dirreadall(fd, dp);
	close(fd);
	if(n < 0){
		free(d);
		return -1;
	}
	qsort(*dp, n, sizeof(Dir), namecmp);
	*self = d;
	return n;
}

static void
seterr(Dcfolder *f)
{
	char err[ERRMAX];

	rerrstr(err, sizeof err);
	if(strncmp(err, "bad value", 9) == 0)
		snprint(f->err, sizeof f->err, "%s", err);
	else
		snprint(f->err, sizeof f->err, "%s: %s: %s", dcerrname(dcerrclass(err)), f->path, err);
	werrstr("%s", f->err);
}

Dcfolder*
dcfolderopen(char *path)
{
	Dcfolder *f;

	f = mallocz(sizeof *f, 1);
	f->path = strdup(path);
	cleanname(f->path);
	f->n = scan(f->path, &f->d, &f->self);
	if(f->n < 0){
		seterr(f);
		dcfolderfree(f);
		return nil;
	}
	f->scans = 1;
	return f;
}

void
dcfolderfree(Dcfolder *f)
{
	if(f == nil)
		return;
	free(f->path);
	free(f->d);
	free(f->self);
	free(f);
}

Dir*
dcfind(Dcfolder *f, char *name)
{
	long lo, hi, mid;
	int r;

	lo = 0;
	hi = f->n;
	while(lo < hi){
		mid = (lo+hi)/2;
		r = strcmp(name, f->d[mid].name);
		if(r == 0)
			return &f->d[mid];
		if(r < 0)
			hi = mid;
		else
			lo = mid+1;
	}
	return nil;
}

char*
dcchangename(int op)
{
	switch(op){
	case Cadd:	return "add";
	case Cdel:	return "del";
	case Cmod:	return "mod";
	case Cren:	return "rename";
	}
	return "?";
}

static int
changed(Dir *a, Dir *b)
{
	return a->qid.path != b->qid.path || a->qid.vers != b->qid.vers
		|| a->qid.type != b->qid.type || a->mode != b->mode
		|| a->length != b->length || a->mtime != b->mtime
		|| strcmp(a->uid, b->uid) != 0 || strcmp(a->gid, b->gid) != 0;
}

static void
addchg(Dcchange **c, int *n, int op, char *name, char *old)
{
	if((*n & 15) == 0)
		*c = realloc(*c, (*n+16)*sizeof(Dcchange));
	(*c)[*n].op = op;
	(*c)[*n].name = strdup(name);
	(*c)[*n].old = old ? strdup(old) : nil;
	(*n)++;
}

/*
 * Read the folder again and report what changed since the last scan.
 * Returns the number of changes, or -1 if the folder can't be read
 * (the old entries are kept, and f->err says why).
 */
int
dcrescan(Dcfolder *f, Dcchange **chg)
{
	Dir *nd, *self, *a, *b;
	long nn, i, j;
	int nc, *gone, *born, ng, nb, k, l;
	Dcchange *c;

	*chg = nil;
	nn = scan(f->path, &nd, &self);
	if(nn < 0){
		seterr(f);
		return -1;
	}
	f->err[0] = 0;
	c = nil;
	nc = 0;
	gone = malloc((f->n+1)*sizeof(int));
	born = malloc((nn+1)*sizeof(int));
	ng = nb = 0;
	i = j = 0;
	while(i < f->n || j < nn){
		a = i < f->n ? &f->d[i] : nil;
		b = j < nn ? &nd[j] : nil;
		if(b == nil || (a != nil && strcmp(a->name, b->name) < 0)){
			gone[ng++] = i++;
		}else if(a == nil || strcmp(a->name, b->name) > 0){
			born[nb++] = j++;
		}else{
			if(changed(a, b))
				addchg(&c, &nc, Cmod, b->name, nil);
			i++;
			j++;
		}
	}
	/* a name that went away and one that came with the same file: a rename */
	for(k = 0; k < ng; k++){
		a = &f->d[gone[k]];
		for(l = 0; l < nb; l++){
			if(born[l] < 0)
				continue;
			b = &nd[born[l]];
			if(a->qid.path == b->qid.path && a->qid.type == b->qid.type
			&& a->type == b->type && a->dev == b->dev){
				addchg(&c, &nc, Cren, b->name, a->name);
				born[l] = -1;
				gone[k] = -1;
				break;
			}
		}
	}
	for(k = 0; k < ng; k++)
		if(gone[k] >= 0)
			addchg(&c, &nc, Cdel, f->d[gone[k]].name, nil);
	for(l = 0; l < nb; l++)
		if(born[l] >= 0)
			addchg(&c, &nc, Cadd, nd[born[l]].name, nil);
	free(gone);
	free(born);
	free(f->d);
	free(f->self);
	f->d = nd;
	f->n = nn;
	f->self = self;
	f->scans++;
	*chg = c;
	return nc;
}

void
dcchangefree(Dcchange *c, int n)
{
	int i;

	for(i = 0; i < n; i++){
		free(c[i].name);
		free(c[i].old);
	}
	free(c);
}

/* sorting for display */

static char*
ext(char *name)
{
	char *p;

	p = strrchr(name, '.');
	if(p == nil || p == name)
		return "";
	return p;
}

static int
cmpname(void *a, void *b)
{
	Dir *x, *y;
	int r;

	x = *(Dir**)a;
	y = *(Dir**)b;
	r = cistrcmp(x->name, y->name);
	if(r == 0)
		r = strcmp(x->name, y->name);
	return r;
}

static int
cmpkind(void *a, void *b)
{
	Dir *x, *y;
	int r;

	x = *(Dir**)a;
	y = *(Dir**)b;
	r = ((y->qid.type&QTDIR) != 0) - ((x->qid.type&QTDIR) != 0);
	if(r == 0)
		r = cistrcmp(ext(x->name), ext(y->name));
	if(r == 0)
		r = cmpname(a, b);
	return r;
}

static int
cmpdate(void *a, void *b)
{
	Dir *x, *y;

	x = *(Dir**)a;
	y = *(Dir**)b;
	if(x->mtime != y->mtime)
		return x->mtime > y->mtime ? -1 : 1;
	return cmpname(a, b);
}

static int
cmpsize(void *a, void *b)
{
	Dir *x, *y;

	x = *(Dir**)a;
	y = *(Dir**)b;
	if(x->length != y->length)
		return x->length > y->length ? -1 : 1;
	return cmpname(a, b);
}

/*
 * The entries in display order: by name (ignoring case), kind
 * (folders first, then by extension), date (newest first) or size
 * (largest first), ties by name. The array is freed by the caller;
 * the Dirs belong to the folder until its next rescan.
 */
Dir**
dcsorted(Dcfolder *f, int how)
{
	Dir **v;
	long i;
	int (*cmp)(void*, void*);

	v = malloc((f->n+1)*sizeof(Dir*));
	for(i = 0; i < f->n; i++)
		v[i] = &f->d[i];
	v[f->n] = nil;
	switch(how){
	case Skind:	cmp = cmpkind; break;
	case Sdate:	cmp = cmpdate; break;
	case Ssize:	cmp = cmpsize; break;
	default:	cmp = cmpname; break;
	}
	qsort(v, f->n, sizeof(Dir*), cmp);
	return v;
}

/* uxmeta's folder sort= names */
int
dcsortby(char *name)
{
	if(name == nil)
		return Sname;
	if(strcmp(name, "kind") == 0)
		return Skind;
	if(strcmp(name, "date") == 0)
		return Sdate;
	if(strcmp(name, "size") == 0)
		return Ssize;
	return Sname;
}
