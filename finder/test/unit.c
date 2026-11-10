/*
 * Unit tests for libfcore: 6.unit [scratchdir]
 */
#include <u.h>
#include <libc.h>
#include "../fcore.h"

int nfail, npass;
char *T;

/* kencc has no __LINE__: failures name the test and the check's number in it */
char *tname;
int tcheck;
#define check(c) ckbool(c, ++tcheck)
#define checkstr(a, b) ckstr(a, b, ++tcheck)
#define checkerr(phrase) ckerr(phrase, ++tcheck)

static void
ckbool(int ok, int line)
{
	if(ok)
		npass++;
	else{
		nfail++;
		fprint(2, "FAIL %s #%d\n", tname, line);
	}
}

static void
ckstr(char *a, char *b, int line)
{
	if(a != nil && b != nil && strcmp(a, b) == 0)
		npass++;
	else{
		nfail++;
		fprint(2, "FAIL %s #%d: got %q want %q\n", tname, line, a, b);
	}
}

static void
ckerr(char *phrase, int line)
{
	char e[ERRMAX];

	rerrstr(e, sizeof e);
	if(strncmp(e, phrase, strlen(phrase)) == 0)
		npass++;
	else{
		nfail++;
		fprint(2, "FAIL %s #%d: error %q, want %q\n", tname, line, e, phrase);
	}
}

static char*
path(char *rel)
{
	return fcjoin(T, rel);
}

static void
put(char *rel, char *s)
{
	char *p;
	int fd;

	p = path(rel);
	fd = create(p, OWRITE, 0644);
	if(fd < 0)
		sysfatal("create %s: %r", p);
	write(fd, s, strlen(s));
	close(fd);
	free(p);
}

static char*
get(char *rel)
{
	static char buf[256];
	char *p;
	int fd, n;

	p = path(rel);
	fd = open(p, OREAD);
	free(p);
	if(fd < 0)
		return nil;
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if(n < 0)
		return nil;
	buf[n] = 0;
	return buf;
}

static void
mkd(char *rel)
{
	char *p;
	int fd;

	p = path(rel);
	fd = create(p, OREAD, DMDIR|0755);
	if(fd < 0)
		sysfatal("mkdir %s: %r", p);
	close(fd);
	free(p);
}

static int
there(char *rel)
{
	char *p;
	Dir *d;

	p = path(rel);
	d = dirstat(p);
	free(p);
	free(d);
	return d != nil;
}

static void
testparse(void)
{
	char text[] = "# gen 7\n"
		"volume=home path=/usr/glenda icon=disk\n"
		"\tdesktop=yes\n"
		"\n"
		"type=text desc=\"Plain text\" open=edit # comment\n"
		"flag\n";
	Fcrec *r;
	vlong gen;

	gen = -1;
	r = fcparserecs(text, &gen);
	check(gen == 7);
	check(r != nil);
	checkstr(r->a[0].attr, "volume");
	checkstr(fcreckey(r), "home");
	checkstr(fcrecget(r, "desktop"), "yes");
	checkstr(fcrecget(r, "path"), "/usr/glenda");
	check(r->next != nil);
	checkstr(fcrecget(r->next, "desc"), "Plain text");
	checkstr(fcrecget(r->next, "open"), "edit");
	check(fcrecget(r->next, "comment") == nil);
	check(r->next->next != nil && strcmp(fcreckey(r->next->next), "") == 0);
	fcrecfree(r);
}

static void
testerr(void)
{
	check(fcerrclass("wstat prohibited") == Enotsup);
	check(fcerrclass("rename not supported") == Enotsup);
	check(fcerrclass("file system read only") == Enotsup);
	check(fcerrclass("mounted directory forbids creation") == Enotsup);
	check(fcerrclass("permission denied") == Eperm);
	check(fcerrclass("wstat -- not owner") == Eperm);
	check(fcerrclass("i/o on hungup channel") == Egone);
	check(fcerrclass("mount rpc error") == Egone);
	check(fcerrclass("'/x' file does not exist") == Enotfound);
	check(fcerrclass("file already exists") == Eexist);
	check(fcerrclass("create/wstat -- file exists") == Eexist);
	check(fcerrclass("something else") == Eother);
	check(fcerrclass("not supported: ours") == Enotsup);
}

static void
testnames(void)
{
	char *s;

	check(fcvalidname("a b.txt"));
	check(fcvalidname("ünïcode"));
	check(!fcvalidname(""));
	check(!fcvalidname("."));
	check(!fcvalidname(".."));
	check(!fcvalidname("a/b"));
	check(!fcvalidname("a\nb"));
	checkstr(s = fcjoin("/", "x"), "/x"); free(s);
	checkstr(s = fcjoin("/a", "x"), "/a/x"); free(s);
	checkstr(s = fcdirname("/a/b/c"), "/a/b"); free(s);
	checkstr(s = fcdirname("/a"), "/"); free(s);
	checkstr(fcbasename("/a/b/c"), "c");
}

static void
testvolof(void)
{
	Fcws *w;
	Fcvol *v;
	char *s;

	w = fcwsopen("unit", nil);
	fcwsaddvol(w, "root", "/");
	fcwsaddvol(w, "home", "/usr/glenda");
	fcwsaddvol(w, "doc", "/usr/glenda/doc/");
	v = fcvolof(w, "/usr/glenda/doc/x");
	check(v != nil && strcmp(v->name, "doc") == 0);
	v = fcvolof(w, "/usr/glenda/docs");
	check(v != nil && strcmp(v->name, "home") == 0);
	v = fcvolof(w, "/tmp");
	check(v != nil && strcmp(v->name, "root") == 0);
	checkstr(s = fcresolve(w, "home:lib/profile"), "/usr/glenda/lib/profile"); free(s);
	checkstr(s = fcresolve(w, "doc:"), "/usr/glenda/doc"); free(s);
	checkstr(s = fcresolve(w, "nope:x"), "nope:x"); free(s);
	fcwsclose(w);
}

static int
haschange(Fcchange *c, int n, int op, char *name, char *old)
{
	int i;

	for(i = 0; i < n; i++)
		if(c[i].op == op && strcmp(c[i].name, name) == 0
		&& (old == nil || (c[i].old != nil && strcmp(c[i].old, old) == 0)))
			return 1;
	return 0;
}

static void
testfolder(void)
{
	Fcfolder *f;
	Fcchange *c;
	Dir nd, **v;
	char *p;
	int n;

	mkd("f");
	put("f/b.txt", "bb");
	put("f/A.c", "a");
	mkd("f/sub");
	p = path("f");
	f = fcfolderopen(p);
	check(f != nil);
	if(f == nil)
		return;
	check(f->n == 3);
	check(fcfind(f, "b.txt") != nil);
	check(fcfind(f, "nope") == nil);

	n = fcrescan(f, &c);
	check(n == 0);
	fcchangefree(c, n);

	put("f/new", "n");
	free(path("x"));
	p = path("f/A.c");
	remove(p);
	free(p);
	put("f/b.txt", "bbbbbb");
	n = fcrescan(f, &c);
	check(n == 3);
	check(haschange(c, n, Cadd, "new", nil));
	check(haschange(c, n, Cdel, "A.c", nil));
	check(haschange(c, n, Cmod, "b.txt", nil));
	fcchangefree(c, n);

	p = path("f/new");
	nulldir(&nd);
	nd.name = "renamed";
	check(dirwstat(p, &nd) >= 0);
	free(p);
	n = fcrescan(f, &c);
	check(n == 1);
	check(haschange(c, n, Cren, "renamed", "new"));
	fcchangefree(c, n);

	/* sorting */
	put("f/Zed", "zzzzzzzzzz");
	put("f/apple", "");
	fcrescan(f, &c);
	fcchangefree(c, 2);
	v = fcsorted(f, Sname);
	checkstr(v[0]->name, "apple");
	checkstr(v[1]->name, "b.txt");
	free(v);
	v = fcsorted(f, Skind);
	checkstr(v[0]->name, "sub");
	free(v);
	v = fcsorted(f, Ssize);
	checkstr(v[0]->name, "Zed");
	free(v);
	check(fcsortby("date") == Sdate);
	check(fcsortby(nil) == Sname);

	/* the folder going away */
	fcdelete(nil, f->path, Frecursive);
	n = fcrescan(f, &c);
	check(n == -1);
	check(strncmp(f->err, "not found", 9) == 0);
	check(f->n == 5);	/* old entries kept */
	fcfolderfree(f);
}

static void
testops(void)
{
	Fcws *w;
	Fcvol *v;
	char *s;

	w = fcwsopen("unit", nil);
	v = fcwsaddvol(w, "t", T);
	check(fcvolcheck(v) == Vonline);

	mkd("o");
	mkd("o/tree");
	mkd("o/tree/deep");
	put("o/tree/one", "1");
	put("o/tree/deep/two", "22");
	put("o/doc.txt", "doc");
	mkd("o/dst");

	/* copy */
	s = fccopy(w, path("o/tree"), path("o/dst"), nil);
	check(s != nil);
	free(s);
	checkstr(get("o/dst/tree/deep/two"), "22");
	checkstr(get("o/dst/tree/one"), "1");
	check(fccopy(w, path("o/tree"), path("o/dst"), nil) == nil);
	checkerr("exists");
	check(fccopy(w, path("o/tree"), path("o/tree/deep"), nil) == nil);
	checkerr("bad value");
	check(fccopy(w, path("o/nope"), path("o/dst"), nil) == nil);
	checkerr("not found");

	/* duplicate */
	s = fcduplicate(w, path("o/doc.txt"));
	checkstr(s ? fcbasename(s) : nil, "doc copy.txt");
	free(s);
	s = fcduplicate(w, path("o/doc.txt"));
	checkstr(s ? fcbasename(s) : nil, "doc copy 2.txt");
	free(s);
	s = fcduplicate(w, path("o/tree"));
	checkstr(s ? fcbasename(s) : nil, "tree copy");
	free(s);
	checkstr(get("o/tree copy/deep/two"), "22");

	/* rename */
	check(fcrename(w, path("o/doc.txt"), "doc copy.txt", 0) < 0);
	checkerr("exists");
	check(fcrename(w, path("o/doc.txt"), "a/b", 0) < 0);
	checkerr("bad value");
	check(fcrename(w, path("o/doc.txt"), "renamed.txt", 0) == 0);
	check(there("o/renamed.txt") && !there("o/doc.txt"));
	check(v->caps.c[Krename] == Cyes);
	check(fcrename(w, T, "x", 0) < 0);
	checkerr("bad value");

	/* move */
	check(fcmove(w, path("o/renamed.txt"), path("o/dst"), 0) < 0);
	checkerr("not supported");
	check(there("o/renamed.txt"));
	check(fcmove(w, path("o/renamed.txt"), path("o/dst"), Fcopydel) == 0);
	check(!there("o/renamed.txt") && there("o/dst/renamed.txt"));
	checkstr(get("o/dst/renamed.txt"), "doc");
	check(fcmove(w, path("o/tree"), path("o/tree/deep"), Fcopydel) < 0);
	checkerr("bad value");
	check(fcmove(w, path("o/dst/renamed.txt"), path("o/dst"), 0) == 0);	/* already there */

	/* new folder */
	s = fcmkdir(w, path("o"), nil);
	checkstr(s ? fcbasename(s) : nil, "untitled folder");
	free(s);
	s = fcmkdir(w, path("o"), nil);
	checkstr(s ? fcbasename(s) : nil, "untitled folder 2");
	free(s);
	check(fcmkdir(w, path("o"), "dst") == nil);
	checkerr("exists");

	/* chmod */
	check(fcchmod(w, path("o/dst/renamed.txt"), 0600) == 0);
	check(fcchmod(w, path("o/dst/renamed.txt"), 01000) < 0);
	checkerr("bad value");

	/* delete */
	check(fcdelete(w, path("o/tree"), 0) < 0);
	checkerr("not empty");
	check(fcdelete(w, path("o/tree"), Frecursive) == 0);
	check(!there("o/tree"));
	check(fcdelete(w, path("o/untitled folder"), 0) == 0);
	check(fcdelete(w, T, Frecursive) < 0);
	checkerr("bad value");

	/* info */
	{
		Fcinfo *fi;

		fi = fcinfo(w, path("o/dst"), 1);
		check(fi != nil);
		if(fi != nil){
			checkstr(fi->kind, "folder");
			check(fi->items == 5);	/* tree, tree/one, tree/deep, tree/deep/two, renamed.txt */
			check(fi->size == 6);
			check(fi->complete);
			check(fi->vol == v);
			fcinfofree(fi);
		}
		fi = fcinfo(w, T, 0);
		check(fi != nil && strcmp(fi->kind, "volume") == 0);
		fcinfofree(fi);
		fi = fcinfo(w, path("o/dst/renamed.txt"), 0);
		check(fi != nil && strcmp(fi->kind, "document") == 0 && fi->size == 3);
		fcinfofree(fi);
	}

	fcwsclose(w);
}

static int ncalls;

static int
cancelafter(void *arg, char*, vlong)
{
	return ++ncalls > *(int*)arg;
}

static void
testcancel(void)
{
	Fcws *w;
	int limit;
	char *s;

	w = fcwsopen("unit", nil);
	fcwsaddvol(w, "t", T);
	mkd("c");
	mkd("c/src");
	put("c/src/1", "1");
	put("c/src/2", "2");
	put("c/src/3", "3");
	limit = 1;
	w->progress = cancelafter;
	w->progarg = &limit;
	s = fccopy(w, path("c/src"), path("c"), "dst");
	check(s == nil);
	checkerr("cancelled");
	check(!there("c/dst"));	/* the partial copy is cleaned up */
	check(ncalls >= 2);
	fcwsclose(w);
}

void
main(int argc, char **argv)
{
	int fd;

	quotefmtinstall();
	T = argc > 1 ? argv[1] : smprint("/tmp/fcunit.%d", getpid());
	fd = create(T, OREAD, DMDIR|0755);
	if(fd < 0)
		sysfatal("create %s: %r", T);
	close(fd);

	tname = "testparse"; tcheck = 0; testparse();
	tname = "testerr"; tcheck = 0; testerr();
	tname = "testnames"; tcheck = 0; testnames();
	tname = "testvolof"; tcheck = 0; testvolof();
	tname = "testfolder"; tcheck = 0; testfolder();
	tname = "testops"; tcheck = 0; testops();
	tname = "testcancel"; tcheck = 0; testcancel();

	fcdelete(nil, T, Frecursive);
	print("unit: %d passed, %d failed\n", npass, nfail);
	exits(nfail ? "fail" : nil);
}
