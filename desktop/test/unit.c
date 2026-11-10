/*
 * Unit tests for libdcore: 6.unit [scratchdir]
 */
#include <u.h>
#include <libc.h>
#include "../dcore.h"

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
	return dcjoin(T, rel);
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
	Dcrec *r;
	vlong gen;

	gen = -1;
	r = dcparserecs(text, &gen);
	check(gen == 7);
	check(r != nil);
	checkstr(r->a[0].attr, "volume");
	checkstr(dcreckey(r), "home");
	checkstr(dcrecget(r, "desktop"), "yes");
	checkstr(dcrecget(r, "path"), "/usr/glenda");
	check(r->next != nil);
	checkstr(dcrecget(r->next, "desc"), "Plain text");
	checkstr(dcrecget(r->next, "open"), "edit");
	check(dcrecget(r->next, "comment") == nil);
	check(r->next->next != nil && strcmp(dcreckey(r->next->next), "") == 0);
	dcrecfree(r);
}

static void
testerr(void)
{
	check(dcerrclass("wstat prohibited") == Enotsup);
	check(dcerrclass("rename not supported") == Enotsup);
	check(dcerrclass("file system read only") == Enotsup);
	check(dcerrclass("mounted directory forbids creation") == Enotsup);
	check(dcerrclass("permission denied") == Eperm);
	check(dcerrclass("wstat -- not owner") == Eperm);
	check(dcerrclass("i/o on hungup channel") == Egone);
	check(dcerrclass("mount rpc error") == Egone);
	check(dcerrclass("Broken pipe") == Egone);
	check(dcerrclass("clone failed: '/n/x'") == Egone);
	check(dcerrclass("'/x' file does not exist") == Enotfound);
	check(dcerrclass("file already exists") == Eexist);
	check(dcerrclass("create/wstat -- file exists") == Eexist);
	check(dcerrclass("something else") == Eother);
	check(dcerrclass("not supported: ours") == Enotsup);
}

static void
testnames(void)
{
	char *s;

	check(dcvalidname("a b.txt"));
	check(dcvalidname("ünïcode"));
	check(!dcvalidname(""));
	check(!dcvalidname("."));
	check(!dcvalidname(".."));
	check(!dcvalidname("a/b"));
	check(!dcvalidname("a\nb"));
	checkstr(s = dcjoin("/", "x"), "/x"); free(s);
	checkstr(s = dcjoin("/a", "x"), "/a/x"); free(s);
	checkstr(s = dcdirname("/a/b/c"), "/a/b"); free(s);
	checkstr(s = dcdirname("/a"), "/"); free(s);
	checkstr(dcbasename("/a/b/c"), "c");
}

static void
testvolof(void)
{
	Dcws *w;
	Dcvol *v;
	char *s;

	w = dcwsopen("unit", nil);
	dcwsaddvol(w, "root", "/");
	dcwsaddvol(w, "home", "/usr/glenda");
	dcwsaddvol(w, "doc", "/usr/glenda/doc/");
	v = dcvolof(w, "/usr/glenda/doc/x");
	check(v != nil && strcmp(v->name, "doc") == 0);
	v = dcvolof(w, "/usr/glenda/docs");
	check(v != nil && strcmp(v->name, "home") == 0);
	v = dcvolof(w, "/tmp");
	check(v != nil && strcmp(v->name, "root") == 0);
	checkstr(s = dcresolve(w, "home:lib/profile"), "/usr/glenda/lib/profile"); free(s);
	checkstr(s = dcresolve(w, "doc:"), "/usr/glenda/doc"); free(s);
	checkstr(s = dcresolve(w, "nope:x"), "nope:x"); free(s);
	dcwsclose(w);
}

static int
haschange(Dcchange *c, int n, int op, char *name, char *old)
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
	Dcfolder *f;
	Dcchange *c;
	Dir nd, **v;
	char *p;
	int n;

	mkd("f");
	put("f/b.txt", "bb");
	put("f/A.c", "a");
	mkd("f/sub");
	p = path("f");
	f = dcfolderopen(p);
	check(f != nil);
	if(f == nil)
		return;
	check(f->n == 3);
	check(dcfind(f, "b.txt") != nil);
	check(dcfind(f, "nope") == nil);

	n = dcrescan(f, &c);
	check(n == 0);
	dcchangefree(c, n);

	put("f/new", "n");
	free(path("x"));
	p = path("f/A.c");
	remove(p);
	free(p);
	put("f/b.txt", "bbbbbb");
	n = dcrescan(f, &c);
	check(n == 3);
	check(haschange(c, n, Cadd, "new", nil));
	check(haschange(c, n, Cdel, "A.c", nil));
	check(haschange(c, n, Cmod, "b.txt", nil));
	dcchangefree(c, n);

	p = path("f/new");
	nulldir(&nd);
	nd.name = "renamed";
	check(dirwstat(p, &nd) >= 0);
	free(p);
	n = dcrescan(f, &c);
	check(n == 1);
	check(haschange(c, n, Cren, "renamed", "new"));
	dcchangefree(c, n);

	/* sorting */
	put("f/Zed", "zzzzzzzzzz");
	put("f/apple", "");
	dcrescan(f, &c);
	dcchangefree(c, 2);
	v = dcsorted(f, Sname);
	checkstr(v[0]->name, "apple");
	checkstr(v[1]->name, "b.txt");
	free(v);
	v = dcsorted(f, Skind);
	checkstr(v[0]->name, "sub");
	free(v);
	v = dcsorted(f, Ssize);
	checkstr(v[0]->name, "Zed");
	free(v);
	check(dcsortby("date") == Sdate);
	check(dcsortby(nil) == Sname);

	/* the folder going away */
	dcdelete(nil, f->path, Frecursive);
	n = dcrescan(f, &c);
	check(n == -1);
	check(strncmp(f->err, "not found", 9) == 0);
	check(f->n == 5);	/* old entries kept */
	dcfolderfree(f);
}

static void
testops(void)
{
	Dcws *w;
	Dcvol *v;
	char *s;

	w = dcwsopen("unit", nil);
	v = dcwsaddvol(w, "t", T);
	check(dcvolcheck(v) == Vonline);

	mkd("o");
	mkd("o/tree");
	mkd("o/tree/deep");
	put("o/tree/one", "1");
	put("o/tree/deep/two", "22");
	put("o/doc.txt", "doc");
	mkd("o/dst");

	/* copy */
	s = dccopy(w, path("o/tree"), path("o/dst"), nil);
	check(s != nil);
	free(s);
	checkstr(get("o/dst/tree/deep/two"), "22");
	checkstr(get("o/dst/tree/one"), "1");
	check(dccopy(w, path("o/tree"), path("o/dst"), nil) == nil);
	checkerr("exists");
	check(dccopy(w, path("o/tree"), path("o/tree/deep"), nil) == nil);
	checkerr("bad value");
	check(dccopy(w, path("o/nope"), path("o/dst"), nil) == nil);
	checkerr("not found");

	/* duplicate */
	s = dcduplicate(w, path("o/doc.txt"));
	checkstr(s ? dcbasename(s) : nil, "doc copy.txt");
	free(s);
	s = dcduplicate(w, path("o/doc.txt"));
	checkstr(s ? dcbasename(s) : nil, "doc copy 2.txt");
	free(s);
	s = dcduplicate(w, path("o/tree"));
	checkstr(s ? dcbasename(s) : nil, "tree copy");
	free(s);
	checkstr(get("o/tree copy/deep/two"), "22");

	/* rename */
	check(dcrename(w, path("o/doc.txt"), "doc copy.txt", 0) < 0);
	checkerr("exists");
	check(dcrename(w, path("o/doc.txt"), "a/b", 0) < 0);
	checkerr("bad value");
	check(dcrename(w, path("o/doc.txt"), "renamed.txt", 0) == 0);
	check(there("o/renamed.txt") && !there("o/doc.txt"));
	check(v->caps.c[Krename] == Cyes);
	check(dcrename(w, T, "x", 0) < 0);
	checkerr("bad value");

	/* move */
	check(dcmove(w, path("o/renamed.txt"), path("o/dst"), 0) < 0);
	checkerr("not supported");
	check(there("o/renamed.txt"));
	check(dcmove(w, path("o/renamed.txt"), path("o/dst"), Fcopydel) == 0);
	check(!there("o/renamed.txt") && there("o/dst/renamed.txt"));
	checkstr(get("o/dst/renamed.txt"), "doc");
	check(dcmove(w, path("o/tree"), path("o/tree/deep"), Fcopydel) < 0);
	checkerr("bad value");
	check(dcmove(w, path("o/dst/renamed.txt"), path("o/dst"), 0) == 0);	/* already there */

	/* new folder */
	s = dcmkdir(w, path("o"), nil);
	checkstr(s ? dcbasename(s) : nil, "untitled folder");
	free(s);
	s = dcmkdir(w, path("o"), nil);
	checkstr(s ? dcbasename(s) : nil, "untitled folder 2");
	free(s);
	check(dcmkdir(w, path("o"), "dst") == nil);
	checkerr("exists");

	/* chmod */
	check(dcchmod(w, path("o/dst/renamed.txt"), 0600) == 0);
	check(dcchmod(w, path("o/dst/renamed.txt"), 01000) < 0);
	checkerr("bad value");

	/* delete */
	check(dcdelete(w, path("o/tree"), 0) < 0);
	checkerr("not empty");
	check(dcdelete(w, path("o/tree"), Frecursive) == 0);
	check(!there("o/tree"));
	check(dcdelete(w, path("o/untitled folder"), 0) == 0);
	check(dcdelete(w, T, Frecursive) < 0);
	checkerr("bad value");

	/* info */
	{
		Dcinfo *fi;

		fi = dcinfo(w, path("o/dst"), 1);
		check(fi != nil);
		if(fi != nil){
			checkstr(fi->kind, "folder");
			check(fi->items == 5);	/* tree, tree/one, tree/deep, tree/deep/two, renamed.txt */
			check(fi->size == 6);
			check(fi->complete);
			check(fi->vol == v);
			dcinfofree(fi);
		}
		fi = dcinfo(w, T, 0);
		check(fi != nil && strcmp(fi->kind, "volume") == 0);
		dcinfofree(fi);
		fi = dcinfo(w, path("o/dst/renamed.txt"), 0);
		check(fi != nil && strcmp(fi->kind, "document") == 0 && fi->size == 3);
		dcinfofree(fi);
	}

	dcwsclose(w);
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
	Dcws *w;
	int limit;
	char *s;

	w = dcwsopen("unit", nil);
	dcwsaddvol(w, "t", T);
	mkd("c");
	mkd("c/src");
	put("c/src/1", "1");
	put("c/src/2", "2");
	put("c/src/3", "3");
	limit = 1;
	w->progress = cancelafter;
	w->progarg = &limit;
	s = dccopy(w, path("c/src"), path("c"), "dst");
	check(s == nil);
	checkerr("cancelled");
	check(!there("c/dst"));	/* the partial copy is cleaned up */
	check(ncalls >= 2);
	dcwsclose(w);
}

void
main(int argc, char **argv)
{
	int fd;

	quotefmtinstall();
	T = argc > 1 ? argv[1] : smprint("/tmp/dcunit.%d", getpid());
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

	dcdelete(nil, T, Frecursive);
	print("unit: %d passed, %d failed\n", npass, nfail);
	exits(nfail ? "fail" : nil);
}
