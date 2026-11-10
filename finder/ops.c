/*
 * File operations: rename, move, copy, duplicate, delete, new folder,
 * permissions.
 *
 * 9P renames only within a directory (wstat of the name), so moving
 * between folders, or renaming on a server without wstat, can only be
 * done by copying and deleting. That is never done silently: without
 * Fcopydel those operations fail with "not supported" and say so, and
 * the caller (Finder's UI) offers the copy and delete explicitly.
 */
#include <u.h>
#include <libc.h>
#include "fcore.h"

char*
fcjoin(char *dir, char *name)
{
	if(name == nil || *name == 0)
		return strdup(dir);
	if(strcmp(dir, "/") == 0)
		return smprint("/%s", name);
	return smprint("%s/%s", dir, name);
}

char*
fcdirname(char *path)
{
	char *p, *s;

	s = strdup(path);
	cleanname(s);
	p = strrchr(s, '/');
	if(p == nil){
		free(s);
		return strdup(".");
	}
	if(p == s)
		p[1] = 0;
	else
		*p = 0;
	return s;
}

char*
fcbasename(char *path)
{
	char *p;

	p = strrchr(path, '/');
	return p ? p+1 : path;
}

int
fcvalidname(char *name)
{
	Rune r;
	char *p;

	if(name == nil || *name == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
		return 0;
	if(strlen(name) > 255)
		return 0;
	for(p = name; *p; ){
		p += chartorune(&r, p);
		if(r == '/' || r < ' ' || r == 0x7F || r == Runeerror)
			return 0;
	}
	return 1;
}

static int
fail(char *op, char *path)
{
	char err[ERRMAX];
	int class;

	rerrstr(err, sizeof err);
	class = fcerrclass(err);
	werrstr("%s: %s %s: %s", fcerrname(class), op, path, err);
	return -1;
}

static Fccaps*
capsof(Fcws *w, char *path)
{
	Fcvol *v;

	if(w == nil || (v = fcvolof(w, path)) == nil)
		return nil;
	return &v->caps;
}

static char*
volname(Fcws *w, char *path)
{
	Fcvol *v;

	if(w == nil || (v = fcvolof(w, path)) == nil)
		return "this file server";
	return v->name;
}

static int
isvolroot(Fcws *w, char *path)
{
	Fcvol *v;

	if(w == nil || (v = fcvolof(w, path)) == nil)
		return 0;
	return strcmp(v->path, path) == 0;
}

static int
exists(char *path)
{
	Dir *d;

	d = dirstat(path);
	free(d);
	return d != nil;
}

/* tell uxmeta, so folder and item layout follows the file */
static void
metarename(Fcws *w, char *old, char *new)
{
	char *qo, *qn;

	if(w == nil || w->meta == nil)
		return;
	qo = quotestrdup(old);
	qn = quotestrdup(new);
	fcmetactl(w->meta, "rename %s %s\n", qo, qn);
	free(qo);
	free(qn);
}

/* is path at or below dir? */
static int
within(char *path, char *dir)
{
	int n;

	n = strlen(dir);
	if(strncmp(path, dir, n) != 0)
		return 0;
	return path[n] == 0 || path[n] == '/' || strcmp(dir, "/") == 0;
}

/* copying */

enum { Bufsize = 64*1024 };

static int
progress(Fcws *w, char *path, vlong n)
{
	if(w != nil && w->progress != nil && w->progress(w->progarg, path, n) != 0){
		werrstr("cancelled: %s", path);
		return -1;
	}
	return 0;
}

static void
keeptimes(Fcws *w, char *dst, Dir *sd)
{
	Dir nd;
	char err[ERRMAX];
	int ok;

	nulldir(&nd);
	nd.mtime = sd->mtime;
	ok = dirwstat(dst, &nd) >= 0;
	rerrstr(err, sizeof err);
	fccapnote(capsof(w, dst), Kmtime, ok, err);
}

static int copytree(Fcws*, char*, char*, Dir*);

static int
copyfile(Fcws *w, char *src, char *dst, Dir *sd)
{
	int in, out;
	long n;
	char *buf, err[ERRMAX];

	in = open(src, OREAD);
	if(in < 0)
		return fail("open", src);
	out = create(dst, OWRITE|OEXCL, sd->mode & (0777|DMAPPEND|DMEXCL|DMTMP));
	if(out < 0){
		rerrstr(err, sizeof err);
		fccapnote(capsof(w, dst), Kcreate, 0, err);
		close(in);
		return fail("create", dst);
	}
	fccapnote(capsof(w, dst), Kcreate, 1, nil);
	buf = malloc(Bufsize);
	for(;;){
		n = read(in, buf, Bufsize);
		if(n < 0){
			fail("read", src);
			goto Err;
		}
		if(n == 0)
			break;
		if(write(out, buf, n) != n){
			rerrstr(err, sizeof err);
			fccapnote(capsof(w, dst), Kwrite, 0, err);
			fail("write", dst);
			goto Err;
		}
		if(progress(w, src, n) < 0)
			goto Err;
	}
	free(buf);
	close(in);
	close(out);
	keeptimes(w, dst, sd);
	return 0;
Err:
	free(buf);
	close(in);
	close(out);
	return -1;
}

static int
copydir(Fcws *w, char *src, char *dst, Dir *sd)
{
	int fd;
	long i, n;
	Dir *d;
	char *s, *t, err[ERRMAX];

	fd = open(src, OREAD);
	if(fd < 0)
		return fail("open", src);
	n = dirreadall(fd, &d);
	close(fd);
	if(n < 0)
		return fail("read", src);
	fd = create(dst, OREAD|OEXCL, DMDIR|(sd->mode&0777));
	if(fd < 0){
		rerrstr(err, sizeof err);
		fccapnote(capsof(w, dst), Kmkdir, 0, err);
		free(d);
		return fail("create", dst);
	}
	fccapnote(capsof(w, dst), Kmkdir, 1, nil);
	close(fd);
	for(i = 0; i < n; i++){
		s = fcjoin(src, d[i].name);
		t = fcjoin(dst, d[i].name);
		if(copytree(w, s, t, &d[i]) < 0){
			free(s);
			free(t);
			free(d);
			return -1;
		}
		free(s);
		free(t);
	}
	free(d);
	if(progress(w, src, 0) < 0)
		return -1;
	keeptimes(w, dst, sd);
	return 0;
}

static int
copytree(Fcws *w, char *src, char *dst, Dir *sd)
{
	if(sd->qid.type & QTDIR)
		return copydir(w, src, dst, sd);
	return copyfile(w, src, dst, sd);
}

static int removetree(Fcws*, char*, Dir*, int*);

/*
 * Copy path (a file or a folder and everything in it) into dstdir,
 * named newname or its own name. If it fails part way, what was
 * copied is removed again. Returns the new path.
 */
char*
fccopy(Fcws *w, char *path, char *dstdir, char *newname)
{
	Dir *sd, *dd;
	char *dst, err[ERRMAX];
	int n;

	if(newname == nil)
		newname = fcbasename(path);
	if(!fcvalidname(newname)){
		werrstr("bad value: %q is not a valid name", newname);
		return nil;
	}
	sd = dirstat(path);
	if(sd == nil){
		fail("copy", path);
		return nil;
	}
	dd = dirstat(dstdir);
	if(dd == nil){
		free(sd);
		fail("copy to", dstdir);
		return nil;
	}
	if((dd->qid.type & QTDIR) == 0){
		free(sd);
		free(dd);
		werrstr("bad value: %s is not a folder", dstdir);
		return nil;
	}
	free(dd);
	dst = fcjoin(dstdir, newname);
	if(exists(dst)){
		free(sd);
		werrstr("exists: %s", dst);
		free(dst);
		return nil;
	}
	if((sd->qid.type & QTDIR) && within(dstdir, path)){
		free(sd);
		free(dst);
		werrstr("bad value: can't copy a folder into itself");
		return nil;
	}
	if(copytree(w, path, dst, sd) < 0){
		rerrstr(err, sizeof err);
		n = 0;
		if(exists(dst)){
			/* clean up without asking the progress hook, which may still be cancelling */
			int (*pf)(void*, char*, vlong);

			pf = w ? w->progress : nil;
			if(w)
				w->progress = nil;
			removetree(w, dst, nil, &n);
			if(w)
				w->progress = pf;
		}
		werrstr("%s", err);
		free(sd);
		free(dst);
		return nil;
	}
	free(sd);
	return dst;
}

/* removing */

static int
removetree(Fcws *w, char *path, Dir *pd, int *count)
{
	Dir *d, *cd;
	long i, n;
	int fd, r;
	char *c, err[ERRMAX];

	d = dirstat(path);
	if(d == nil)
		return fail("remove", path);
	if(pd != nil && (d->type != pd->type || d->dev != pd->dev)){
		free(d);
		werrstr("in use: %s is a mount point", path);
		return -1;
	}
	if(d->qid.type & QTDIR){
		fd = open(path, OREAD);
		if(fd < 0){
			free(d);
			return fail("open", path);
		}
		n = dirreadall(fd, &cd);
		close(fd);
		if(n < 0){
			free(d);
			return fail("read", path);
		}
		for(i = 0; i < n; i++){
			c = fcjoin(path, cd[i].name);
			r = removetree(w, c, d, count);
			free(c);
			if(r < 0){
				free(cd);
				free(d);
				return -1;
			}
		}
		free(cd);
	}
	free(d);
	if(remove(path) < 0){
		rerrstr(err, sizeof err);
		fccapnote(capsof(w, path), Kremove, 0, err);
		return fail("remove", path);
	}
	fccapnote(capsof(w, path), Kremove, 1, nil);
	(*count)++;
	return progress(w, path, 0);
}

/*
 * Delete path. A folder with anything in it needs Frecursive. If a
 * recursive delete stops part way, the error starts with "partial"
 * and says how much went.
 */
int
fcdelete(Fcws *w, char *path, int flags)
{
	Dir *d, *cd;
	long n;
	int fd, count;
	char err[ERRMAX];

	if(isvolroot(w, path)){
		werrstr("bad value: %s is a volume; remove it from the registry instead", path);
		return -1;
	}
	d = dirstat(path);
	if(d == nil)
		return fail("delete", path);
	if((d->qid.type & QTDIR) && !(flags & Frecursive)){
		fd = open(path, OREAD);
		if(fd < 0){
			free(d);
			return fail("open", path);
		}
		n = dirread(fd, &cd);
		close(fd);
		free(cd);
		if(n > 0){
			free(d);
			werrstr("not empty: %s", path);
			return -1;
		}
	}
	free(d);
	count = 0;
	if(removetree(w, path, nil, &count) < 0){
		if(count > 0){
			rerrstr(err, sizeof err);
			werrstr("partial: deleted %d items, then %s", count, err);
		}
		return -1;
	}
	return 0;
}

/* copy, then delete the original; for moves and renames 9P can't do */
static int
copydel(Fcws *w, char *path, char *dstdir, char *newname)
{
	char *dst, err[ERRMAX];
	int count;

	dst = fccopy(w, path, dstdir, newname);
	if(dst == nil)
		return -1;
	count = 0;
	if(removetree(w, path, nil, &count) < 0){
		rerrstr(err, sizeof err);
		werrstr("partial: copied to %s, but the original remains: %s", dst, err);
		free(dst);
		return -1;
	}
	metarename(w, path, dst);
	free(dst);
	return 0;
}

int
fcrename(Fcws *w, char *path, char *newname, int flags)
{
	Dir nd, *d;
	char *dir, *dst, err[ERRMAX];
	int class, r;

	if(!fcvalidname(newname)){
		werrstr("bad value: %q is not a valid name", newname);
		return -1;
	}
	if(isvolroot(w, path)){
		werrstr("bad value: %s is a volume; rename it in the registry instead", path);
		return -1;
	}
	d = dirstat(path);
	if(d == nil)
		return fail("rename", path);
	if(strcmp(d->name, newname) == 0){
		free(d);
		return 0;
	}
	free(d);
	dir = fcdirname(path);
	dst = fcjoin(dir, newname);
	if(exists(dst)){
		werrstr("exists: %s", dst);
		free(dir);
		free(dst);
		return -1;
	}
	nulldir(&nd);
	nd.name = newname;
	if(dirwstat(path, &nd) >= 0){
		fccapnote(capsof(w, path), Krename, 1, nil);
		metarename(w, path, dst);
		free(dir);
		free(dst);
		return 0;
	}
	rerrstr(err, sizeof err);
	class = fcerrclass(err);
	fccapnote(capsof(w, path), Krename, 0, err);
	if(class != Enotsup){
		free(dir);
		free(dst);
		werrstr("%s", err);
		return fail("rename", path);
	}
	if(!(flags & Fcopydel)){
		werrstr("not supported: %s can't rename (%s); copy and delete instead", volname(w, path), err);
		free(dir);
		free(dst);
		return -1;
	}
	r = copydel(w, path, dir, newname);
	free(dir);
	free(dst);
	return r;
}

int
fcmove(Fcws *w, char *path, char *dstdir, int flags)
{
	char *dir, *dst, *src;
	Dir *d;
	int r;

	src = strdup(path);
	cleanname(src);
	dir = fcdirname(src);
	d = dirstat(dstdir);
	if(d == nil){
		free(src);
		free(dir);
		return fail("move to", dstdir);
	}
	if((d->qid.type & QTDIR) == 0){
		free(d);
		free(src);
		free(dir);
		werrstr("bad value: %s is not a folder", dstdir);
		return -1;
	}
	free(d);
	r = 0;
	if(strcmp(dir, dstdir) == 0)
		goto Out;
	r = -1;
	if(isvolroot(w, src)){
		werrstr("bad value: %s is a volume", src);
		goto Out;
	}
	if(within(dstdir, src)){
		werrstr("bad value: can't move a folder into itself");
		goto Out;
	}
	dst = fcjoin(dstdir, fcbasename(src));
	if(exists(dst)){
		werrstr("exists: %s", dst);
		free(dst);
		goto Out;
	}
	free(dst);
	if(!(flags & Fcopydel)){
		werrstr("not supported: 9P can't move between folders; copy and delete instead");
		goto Out;
	}
	r = copydel(w, src, dstdir, nil);
Out:
	free(src);
	free(dir);
	return r;
}

/* "name copy.ext", then "name copy 2.ext", ... */
char*
fcduplicate(Fcws *w, char *path)
{
	Dir *d;
	char *dir, *name, *base, *ext, *e, *cand, *r, err[ERRMAX];
	int i;

	d = dirstat(path);
	if(d == nil){
		fail("duplicate", path);
		return nil;
	}
	if(isvolroot(w, path)){
		free(d);
		werrstr("bad value: can't duplicate a volume");
		return nil;
	}
	dir = fcdirname(path);
	name = strdup(fcbasename(path));
	ext = "";
	if((d->qid.type & QTDIR) == 0 && (e = strrchr(name, '.')) != nil && e != name){
		ext = strdup(e);
		*e = 0;
	}else
		ext = strdup(ext);
	base = name;
	free(d);
	r = nil;
	for(i = 1; i < 1000; i++){
		if(i == 1)
			cand = smprint("%s copy%s", base, ext);
		else
			cand = smprint("%s copy %d%s", base, i, ext);
		r = fccopy(w, path, dir, cand);
		free(cand);
		if(r != nil)
			break;
		rerrstr(err, sizeof err);
		if(strncmp(err, "exists", 6) != 0)
			break;
	}
	free(dir);
	free(name);
	free(ext);
	return r;
}

/* a new folder in dir, named name or "untitled folder", "untitled folder 2", ... */
char*
fcmkdir(Fcws *w, char *dir, char *name)
{
	char *p, *cand, err[ERRMAX];
	int i, fd;

	if(name != nil && !fcvalidname(name)){
		werrstr("bad value: %q is not a valid name", name);
		return nil;
	}
	for(i = 1; i < 1000; i++){
		if(name != nil)
			cand = strdup(name);
		else if(i == 1)
			cand = strdup("untitled folder");
		else
			cand = smprint("untitled folder %d", i);
		p = fcjoin(dir, cand);
		free(cand);
		if(exists(p)){
			if(name != nil){
				werrstr("exists: %s", p);
				free(p);
				return nil;
			}
			free(p);
			continue;
		}
		fd = create(p, OREAD|OEXCL, DMDIR|0777);
		if(fd >= 0){
			close(fd);
			fccapnote(capsof(w, p), Kmkdir, 1, nil);
			return p;
		}
		rerrstr(err, sizeof err);
		fccapnote(capsof(w, p), Kmkdir, 0, err);
		fail("new folder", p);
		free(p);
		return nil;
	}
	werrstr("exists: too many untitled folders in %s", dir);
	return nil;
}

int
fcchmod(Fcws *w, char *path, ulong mode)
{
	Dir nd, *d;
	char err[ERRMAX];

	if(mode & ~0777){
		werrstr("bad value: mode %luo", mode);
		return -1;
	}
	d = dirstat(path);
	if(d == nil)
		return fail("chmod", path);
	nulldir(&nd);
	nd.mode = (d->mode & ~0777) | mode;
	free(d);
	if(dirwstat(path, &nd) < 0){
		rerrstr(err, sizeof err);
		fccapnote(capsof(w, path), Kchmod, 0, err);
		return fail("chmod", path);
	}
	fccapnote(capsof(w, path), Kchmod, 1, nil);
	return 0;
}
