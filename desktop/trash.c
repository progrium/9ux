/*
 * Trash, one per volume, at the volume's root in ".trash".
 *
 * 9P can't move a file between folders, so moving something to the
 * Trash copies it into the Trash of its own volume (same server, no
 * network round trip through Desktop's machine) and then removes the
 * original. Each item goes in its own slot, .trash/STAMP/NAME, with
 * .trash/STAMP/.origin recording where it came from and when, so Put
 * Back can return it. Nothing is ever moved to another volume's Trash:
 * a volume that can't hold one says so (dctrashlimit), and the UI
 * offers Delete instead.
 */
#include <u.h>
#include <libc.h>
#include "dcore.h"

static char trashname[] = ".trash";
static char originname[] = ".origin";

static int
isdir(char *path)
{
	Dir *d;
	int r;

	d = dirstat(path);
	r = d != nil && (d->qid.type & QTDIR);
	free(d);
	return r;
}

/* the path inside the volume, without its mount point: "docs/a.txt" */
static char*
relpath(Dcvol *v, char *path)
{
	int n;

	n = strlen(v->path);
	if(strcmp(v->path, "/") == 0)
		return path+1;
	if(strncmp(path, v->path, n) == 0 && path[n] == '/')
		return path+n+1;
	return nil;
}

char*
dctrashdir(Dcvol *v)
{
	return dcjoin(v->path, trashname);
}

int
dcintrash(Dcws *w, char *path)
{
	Dcvol *v;
	char *r;

	if((v = dcvolof(w, path)) == nil || (r = relpath(v, path)) == nil)
		return 0;
	return strncmp(r, trashname, sizeof trashname-1) == 0
		&& (r[sizeof trashname-1] == 0 || r[sizeof trashname-1] == '/');
}

/* make the volume's trash folder if it isn't there */
static char*
ensuretrash(Dcvol *v)
{
	char *t, err[ERRMAX];
	int fd;

	t = dctrashdir(v);
	if(isdir(t))
		return t;
	fd = create(t, OREAD, DMDIR|0777);
	if(fd < 0){
		rerrstr(err, sizeof err);
		dccapnote(&v->caps, Kmkdir, dcerrclass(err) == Eperm, err);
		if(dcerrclass(err) == Eperm)
			werrstr("permission denied: can't make %s's Trash: %s", v->name, err);
		else
			werrstr("not supported: %s can't hold a Trash (%s); delete instead", v->name, err);
		free(t);
		return nil;
	}
	close(fd);
	return t;
}

/* a new, empty slot in the trash folder */
static char*
newslot(char *trash)
{
	char *s, stamp[64];
	int i, fd;

	for(i = 0; i < 1000; i++){
		snprint(stamp, sizeof stamp, "%ld.%d", time(0), i);
		s = dcjoin(trash, stamp);
		fd = create(s, OREAD|OEXCL, DMDIR|0777);
		if(fd >= 0){
			close(fd);
			return s;
		}
		free(s);
	}
	werrstr("failed: no free slot in %s", trash);
	return nil;
}

static void
rmslot(Dcws *w, char *slot)
{
	dcdelete(w, slot, Frecursive);
}

char*
dctrash(Dcws *w, char *path, int flags)
{
	Dcvol *v;
	char *p, *rel, *trash, *slot, *o, *dst, err[ERRMAX];
	int fd;
	Dir *d;

	USED(flags);
	p = strdup(path);
	cleanname(p);
	v = dcvolof(w, p);
	if(v == nil || (rel = relpath(v, p)) == nil){
		werrstr("not supported: %s isn't on a volume, so it has no Trash; delete instead", p);
		free(p);
		return nil;
	}
	if(dcintrash(w, p)){
		werrstr("bad value: %s is already in the Trash", p);
		free(p);
		return nil;
	}
	d = dirstat(p);
	if(d == nil){
		rerrstr(err, sizeof err);
		werrstr("%s: %s: %s", dcerrname(dcerrclass(err)), p, err);
		free(p);
		return nil;
	}
	free(d);
	trash = ensuretrash(v);
	if(trash == nil){
		free(p);
		return nil;
	}
	slot = newslot(trash);
	free(trash);
	if(slot == nil){
		free(p);
		return nil;
	}
	o = dcjoin(slot, originname);
	fd = create(o, OWRITE, 0600);
	free(o);
	if(fd < 0 || fprint(fd, "path=%q\ndate=%ld\n", rel, time(0)) < 0){
		rerrstr(err, sizeof err);
		if(fd >= 0)
			close(fd);
		rmslot(w, slot);
		free(slot);
		free(p);
		werrstr("%s: writing to the Trash: %s", dcerrname(dcerrclass(err)), err);
		return nil;
	}
	close(fd);
	dst = dccopy(w, p, slot, nil);
	if(dst == nil){
		rerrstr(err, sizeof err);
		rmslot(w, slot);
		free(slot);
		free(p);
		werrstr("%s", err);
		return nil;
	}
	if(dcdelete(w, p, Frecursive) < 0){
		/* the original stays where it was; so must nothing else */
		rerrstr(err, sizeof err);
		if(strncmp(err, "partial", 7) == 0)
			werrstr("partial: moved part of %s to the Trash: %s", p, err);
		else{
			rmslot(w, slot);
			werrstr("%s", err);
		}
		free(dst);
		free(slot);
		free(p);
		return nil;
	}
	free(slot);
	free(p);
	return dst;
}

/* read a slot's .origin */
static int
origin(char *slot, char **rel, long *date)
{
	char *o, buf[1024], *line, *e, *f[2];
	int fd, n;

	*rel = nil;
	*date = 0;
	o = dcjoin(slot, originname);
	fd = open(o, OREAD);
	free(o);
	if(fd < 0)
		return -1;
	n = readn(fd, buf, sizeof buf-1);
	close(fd);
	if(n < 0)
		return -1;
	buf[n] = 0;
	for(line = buf; (e = strchr(line, '\n')) != nil; line = e+1){
		*e = 0;
		if(strncmp(line, "date=", 5) == 0)
			*date = strtol(line+5, nil, 10);
		else if(strncmp(line, "path=", 5) == 0 && tokenize(line+5, f, 2) == 1){
			free(*rel);
			*rel = strdup(f[0]);
		}
	}
	return *rel != nil ? 0 : -1;
}

/* the one item in a slot */
static char*
slotitem(char *slot)
{
	Dir *d;
	long i, n;
	int fd;
	char *r;

	fd = open(slot, OREAD);
	if(fd < 0)
		return nil;
	n = dirreadall(fd, &d);
	close(fd);
	r = nil;
	for(i = 0; i < n; i++)
		if(strcmp(d[i].name, originname) != 0){
			r = dcjoin(slot, d[i].name);
			break;
		}
	free(d);
	return r;
}

static int
itemcmp(void *a, void *b)
{
	Dctrashitem *x, *y;

	x = a;
	y = b;
	if(x->date != y->date)
		return x->date < y->date ? -1 : 1;
	return strcmp(x->path, y->path);
}

long
dctrashlist(Dcws *w, Dcvol *v, Dctrashitem **items)
{
	Dir *d;
	long i, n, k;
	int fd;
	char *t, *slot, *rel;
	Dctrashitem *it;
	long date;

	USED(w);
	*items = nil;
	t = dctrashdir(v);
	fd = open(t, OREAD);
	if(fd < 0){
		free(t);
		return 0;	/* no trash yet: empty */
	}
	n = dirreadall(fd, &d);
	close(fd);
	it = calloc(n+1, sizeof it[0]);
	k = 0;
	for(i = 0; i < n; i++){
		if(!(d[i].qid.type & QTDIR))
			continue;
		slot = dcjoin(t, d[i].name);
		it[k].path = slotitem(slot);
		if(it[k].path == nil){
			free(slot);
			continue;
		}
		if(origin(slot, &rel, &date) == 0){
			it[k].origin = dcjoin(v->path, rel);
			free(rel);
		}
		it[k].date = date != 0 ? date : d[i].mtime;
		it[k].name = strdup(dcbasename(it[k].path));
		it[k].vol = v;
		k++;
		free(slot);
	}
	free(d);
	free(t);
	qsort(it, k, sizeof it[0], itemcmp);
	*items = it;
	return k;
}

void
dctrashfree(Dctrashitem *it, long n)
{
	long i;

	for(i = 0; i < n; i++){
		free(it[i].path);
		free(it[i].name);
		free(it[i].origin);
	}
	free(it);
}

char*
dcputback(Dcws *w, char *item)
{
	char *p, *slot, *rel, *orig, *dir, *dst;
	Dcvol *v;
	long date;

	p = strdup(item);
	cleanname(p);
	if(!dcintrash(w, p) || (v = dcvolof(w, p)) == nil){
		werrstr("bad value: %s isn't in a Trash", p);
		free(p);
		return nil;
	}
	slot = dcdirname(p);
	if(origin(slot, &rel, &date) < 0){
		werrstr("not found: where %s came from isn't known", dcbasename(p));
		free(slot);
		free(p);
		return nil;
	}
	orig = dcjoin(v->path, rel);
	free(rel);
	dir = dcdirname(orig);
	dst = nil;
	if(access(orig, AEXIST) == 0)
		werrstr("exists: %s", orig);
	else if(!isdir(dir))
		werrstr("not found: the folder %s came from is gone", dcbasename(orig));
	else if((dst = dccopy(w, p, dir, dcbasename(orig))) != nil)
		rmslot(w, slot);
	free(dir);
	free(orig);
	free(slot);
	free(p);
	return dst;
}

int
dcemptytrash(Dcws *w, Dcvol *v)
{
	Dir *d;
	long i, n;
	int fd, r;
	char *t, *s, err[ERRMAX];

	t = dctrashdir(v);
	fd = open(t, OREAD);
	if(fd < 0){
		free(t);
		return 0;
	}
	n = dirreadall(fd, &d);
	close(fd);
	r = 0;
	for(i = 0; i < n; i++){
		s = dcjoin(t, d[i].name);
		if(dcdelete(w, s, Frecursive) < 0 && r == 0){
			rerrstr(err, sizeof err);
			r = -1;
		}
		free(s);
	}
	free(d);
	free(t);
	if(r < 0)
		werrstr("%s", err);
	return r;
}

char*
dctrashlimit(Dcws *w, Dcvol *v)
{
	USED(w);
	if(v->caps.c[Kremove] == Cno)
		return smprint("Nothing can be removed from %s (%s), so nothing on it can go in the Trash.",
			v->name, v->caps.why[Kremove]);
	if(v->caps.c[Kmkdir] == Cno || v->caps.c[Kcreate] == Cno)
		return smprint("%s can't hold a Trash (%s): delete items instead, and they are gone at once.",
			v->name, v->caps.why[v->caps.c[Kmkdir] == Cno ? Kmkdir : Kcreate]);
	return smprint("Items moved to the Trash are copied into %s's own Trash and then deleted, "
		"since 9P can't move files between folders: a large folder takes as long as copying it. "
		"Each volume has its own Trash.", v->name);
}
