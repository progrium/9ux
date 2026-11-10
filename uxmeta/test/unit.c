/*
 * Unit tests for uxmeta's library: ndb round trips, layered precedence,
 * trust, transactions, rename, resolve, plumbing rules, atomic writes.
 * Run by mk test; uses a scratch directory under /tmp.
 */
#include <u.h>
#include <libc.h>
#include "meta.h"

static int failed, passed, nth;
static char *tname = "";
static char *dir;

#define check(c) _check(c, "", 0)

static void
_check(int c, char *s, int line)
{
	nth++;
	if(c){
		passed++;
		return;
	}
	failed++;
	USED(s, line);
	fprint(2, "%s: check %d: FAIL\n", tname, nth);
}

static void
checkstr(char *got, char *want, int line)
{
	nth++;
	USED(line);
	if(got != nil && want != nil && strcmp(got, want) == 0){
		passed++;
		return;
	}
	failed++;
	fprint(2, "%s: check %d: FAIL:\n--- got\n%s\n--- want\n%s\n---\n", tname, nth,
		got ? got : "(nil)", want ? want : "(nil)");
}

static void
put(char *path, char *s)
{
	int fd;

	if(atomicwrite(path, "") < 0)	/* makes the directories */
		sysfatal("atomicwrite %s: %r", path);
	remove(smprint("%s.old", path));
	fd = create(path, OWRITE, 0644);
	if(fd < 0)
		sysfatal("create %s: %r", path);
	write(fd, s, strlen(s));
	close(fd);
}

static char*
get(char *path)
{
	char *s;

	s = readfile(path, nil);
	return s ? s : estrdup("(missing)");
}

static void
rmrf(char *d)
{
	char *cmd;

	cmd = smprint("rm -rf %q", d);
	if(fork() == 0){
		execl("/bin/rc", "rc", "-c", cmd, nil);
		exits("exec");
	}
	waitpid();
	free(cmd);
}

/* --- ndb text --- */

static void
testroundtrip(void)
{
	Nfile f;
	char *s, *t;

	tname = "testroundtrip";
	nth = 0;
	s = "# header comment\n"
		"\n"
		"app=sketch exec=/apps/Sketch/sketch icon=sketch\n"
		"\tresources=/apps/Sketch/resources.rsrc\n"
		"# between\n"
		"ext=.txt   open=edit    icon=text   # trailing\n"
		"\n"
		"\n"
		"type=text desc=\"Plain text\" flag\n"
		"   \n"
		"\tcont=1\n"
		"no-newline=x";
	ndbparse(s, &f);
	t = nfiletext(&f);
	checkstr(t, s, 0);
	free(t);
	check(f.nc == 7);
	check(f.c[1].rec != nil && f.c[1].rec->nt == 4);
	check(strcmp(recget(f.c[1].rec, "resources"), "/apps/Sketch/resources.rsrc") == 0);
	check(f.c[3].rec != nil && strcmp(recget(f.c[3].rec, "icon"), "text") == 0);
	/* the type record spans the blank-ish line and its continuation */
	{
		Chunk *c = nfilefind(&f, "type", "text");
		check(c != nil && strcmp(recget(c->rec, "desc"), "Plain text") == 0);
		check(c != nil && recget(c->rec, "flag") != nil && *recget(c->rec, "flag") == 0);
		check(c != nil && strcmp(recget(c->rec, "cont"), "1") == 0);
	}
	check(nfilefind(&f, "no-newline", "x") != nil);
	nfilefree(&f);
}

static void
testformat(void)
{
	Rec *r;
	char *s;
	char *v[2];

	tname = "testformat";
	nth = 0;
	r = recnew("folder", "/usr/glenda/My Documents");
	recadd(r, "view", "icons");
	recadd(r, "note", "");
	recadd(r, "bare", nil);
	s = rectext(r);
	checkstr(s, "folder=\"/usr/glenda/My Documents\" view=icons note=\"\" bare\n", 0);
	free(s);
	v[0] = "list";
	recset(r, "view", v, 1);
	v[0] = "a";
	v[1] = "b";
	recset(r, "tag", v, 2);
	s = rectext(r);
	checkstr(s, "folder=\"/usr/glenda/My Documents\" view=list note=\"\" bare tag=a tag=b\n", 0);
	free(s);
	check(!validval("say \"hi\""));
	check(validval("a\"b"));
	check(!validval("two\nlines"));
	check(!validattr("a=b"));
	check(!validattr(""));
	check(validattr("x"));
	recfree(r);

	/* long records wrap */
	r = recnew("app", "sketch");
	recadd(r, "exec", "/apps/Sketch/sketch");
	recadd(r, "resources", "/apps/Sketch/resources.rsrc");
	recadd(r, "icon", "sketch");
	recadd(r, "name", "Sketch Pad Deluxe");
	s = rectext(r);
	checkstr(s, "app=sketch exec=/apps/Sketch/sketch\n"
		"\tresources=/apps/Sketch/resources.rsrc icon=sketch\n"
		"\tname=\"Sketch Pad Deluxe\"\n", 0);
	{
		Nfile f;
		ndbparse(s, &f);
		check(f.nc == 1 && receq(f.c[0].rec, r));
		nfilefree(&f);
	}
	free(s);
	recfree(r);
}

/* --- layers --- */

static Meta*
newmeta(void)
{
	Meta *m;

	m = emalloc(sizeof *m);
	m->sysdir = smprint("%s/sys", dir);
	m->appsdir = smprint("%s/apps", dir);
	m->userfile = smprint("%s/user/meta.ndb", dir);
	m->wsfile = smprint("%s/user/workspace/default.ndb", dir);
	m->genfile = smprint("%s/user/meta.gen", dir);
	m->plumbfile = smprint("%s/user/plumbing", dir);
	metaloadgen(m);
	metarescan(m);
	metacommit(m);
	return m;
}

static void
freemeta(Meta *m)
{
	viewfree(m->v);
	free(m);	/* leaks layers; fine in a test */
}

static char*
q(Meta *m, char *query)
{
	char *s, *p;

	s = metaquery(m, query);
	if(s == nil)
		return smprint("error: %r");
	/* drop the "# gen N" line */
	p = strchr(s, '\n');
	p = estrdup(p ? p+1 : s);
	free(s);
	return p;
}

static void
ctl(Meta *m, char *cmd, char *wanterr, int line)
{
	char *e;

	nth++;
	USED(line);
	e = metactl(m, cmd, Rrw|Rlay|Rro);
	if(wanterr == nil && e != nil){
		failed++;
		fprint(2, "%s: check %d: FAIL: %q: %s\n", tname, nth, cmd, e);
	}else if(wanterr != nil && (e == nil || strstr(e, wanterr) == nil)){
		failed++;
		fprint(2, "%s: check %d: FAIL: %q: want error %q, got %q\n", tname, nth, cmd, wanterr, e ? e : "none");
	}else
		passed++;
	free(e);
}

static void
setup(void)
{
	rmrf(dir);
	put(smprint("%s/sys/10-base.ndb", dir),
		"# system defaults\n"
		"ext=.txt type=text\n"
		"type=text open=edit icon=text desc=\"Plain text\"\n"
		"ext=.png open=page icon=image\n"
		"ext=.gz open=gunzip\n"
		"volume=root path=/ icon=disk desktop=yes x=20 y=40\n"
		"volume=tmp path=/tmp icon=disk\n");
	put(smprint("%s/sys/20-site.ndb", dir),
		"ext=.png icon=picture\n");
	put(smprint("%s/apps/Sketch/app.ndb", dir),
		"app=sketch exec=/apps/Sketch/sketch icon=sketch port=sketch\n"
		"ext=.skt type=sketch\n"
		"type=sketch open=sketch icon=sketchdoc\n"
		"volume=evil path=/n/evil desktop=yes\n"
		"folder=/usr/glenda x=1\n");
	put(smprint("%s/user/meta.ndb", dir),
		"# my settings\n"
		"\n"
		"ext=.png open=photo   # keep this comment\n"
		"volume=tmp desktop=yes x=\"\"\n"
		"volume=root removed=yes\n");
}

static void
testprecedence(void)
{
	Meta *m;
	char *s;

	tname = "testprecedence";
	nth = 0;
	setup();
	m = newmeta();
	s = q(m, "ext .png");
	checkstr(s, "ext=.png open=photo icon=picture\n", 0);
	s = q(m, "volume");
	checkstr(s, "volume=tmp path=/tmp icon=disk desktop=yes\n", 0);
	s = q(m, "app");
	checkstr(s, "app=sketch exec=/apps/Sketch/sketch icon=sketch port=sketch\n", 0);
	/* apps can't add volumes or layout */
	s = q(m, "folder");
	checkstr(s, "", 0);
	s = q(m, "volume evil");
	checkstr(s, "", 0);
	s = metalayers(m);
	check(strstr(s, "apps/Sketch") != nil && strstr(s, "ignored kinds: volume folder") != nil);
	check(strstr(s, "user ") != nil && strstr(s, " rw") != nil);
	check(strstr(s, "workspace ") != nil && strstr(s, " missing") != nil);
	s = q(m, "explain ext .png");
	checkstr(s, "# layer system/10-base\next=.png open=page icon=image\n"
		"# layer system/20-site\next=.png icon=picture\n"
		"# layer user\next=.png open=photo\n", 0);
	s = q(m, "* icon=disk");
	checkstr(s, "volume=tmp path=/tmp icon=disk desktop=yes\n", 0);
	s = q(m, "ext open=*");
	check(strstr(s, "ext=.gz") != nil && strstr(s, "ext=.txt") == nil);
	s = q(m, "bogus query=x y");
	check(strncmp(s, "error:", 6) == 0);
	freemeta(m);
}

static void
testresolve(void)
{
	Meta *m;
	char *s;

	tname = "testresolve";
	nth = 0;
	setup();
	put(smprint("%s/sys/30-more.ndb", dir), "ext=.tar.gz open=tar icon=archive\n");
	m = newmeta();
	s = q(m, "resolve /usr/glenda/notes.TXT");
	checkstr(s, "file=/usr/glenda/notes.TXT ext=.txt type=text open=edit icon=text port=edit desc=\"Plain text\"\n", 0);
	s = q(m, "resolve /a/b.tar.gz");
	checkstr(s, "file=/a/b.tar.gz ext=.tar.gz open=tar icon=archive port=tar\n", 0);
	s = q(m, "resolve /a/b.gz");
	checkstr(s, "file=/a/b.gz ext=.gz open=gunzip port=gunzip\n", 0);
	s = q(m, "resolve /a/pic.skt");
	checkstr(s, "file=/a/pic.skt ext=.skt type=sketch open=sketch icon=sketchdoc port=sketch exec=/apps/Sketch/sketch\n", 0);
	s = q(m, "resolve /a/README");
	checkstr(s, "file=/a/README\n", 0);
	ctl(m, "set item /a/notes.txt icon=special", nil, 0);
	s = q(m, "resolve /a/notes.txt");
	check(strstr(s, "icon=special") != nil);
	freemeta(m);
}

static void
testupdates(void)
{
	Meta *m;
	char *s, *u;
	uvlong g;

	tname = "testupdates";
	nth = 0;
	setup();
	m = newmeta();
	g = m->gen;
	ctl(m, "set folder /usr/glenda/doc view=icons x=100 y=90 width=550 height=400", nil, 0);
	check(m->gen == g+1);
	s = get(m->wsfile);
	checkstr(s, "folder=/usr/glenda/doc view=icons x=100 y=90 width=550 height=400\n#uxmeta end\n", 0);
	/* change one attribute: the others and their order stay */
	ctl(m, "set folder /usr/glenda/doc/ x=120", nil, 0);
	s = q(m, "folder");
	checkstr(s, "folder=/usr/glenda/doc view=icons x=120 y=90 width=550 height=400\n", 0);
	/* user layer: untouched lines and comments are kept byte for byte */
	ctl(m, "set ext .md open=edit icon=text", nil, 0);
	u = get(m->userfile);
	checkstr(u, "# my settings\n"
		"\n"
		"ext=.png open=photo   # keep this comment\n"
		"volume=tmp desktop=yes x=\"\"\n"
		"volume=root removed=yes\n"
		"\n"
		"ext=.md open=edit icon=text\n"
		"#uxmeta end\n", 0);
	/* a transaction is all or nothing */
	g = m->gen;
	ctl(m, "set ext .c open=acme\nset ext .h open=acme\nbogus x", "unknown command", 0);
	check(m->gen == g);
	check(strcmp(q(m, "ext .c"), "") == 0);
	ctl(m, "set ext .c open=acme\nset ext .h open=acme", nil, 0);
	check(m->gen == g+1);
	/* ifgen */
	ctl(m, smprint("ifgen=%llud set ext .c open=sam", m->gen-1), "generation mismatch", 0);
	ctl(m, smprint("ifgen=%llud set ext .c open=sam", m->gen), nil, 0);
	checkstr(q(m, "ext .c"), "ext=.c open=sam\n", 0);
	/* unset shows the lower layer again; unset of everything deletes */
	ctl(m, "set ext .txt icon=mine", nil, 0);
	checkstr(q(m, "ext .txt"), "ext=.txt type=text icon=mine\n", 0);
	ctl(m, "unset ext .txt icon", nil, 0);
	checkstr(q(m, "ext .txt"), "ext=.txt type=text\n", 0);
	check(strstr(get(m->userfile), ".txt") == nil);
	/* remove hides a lower record, delete brings it back */
	ctl(m, "remove ext .gz", nil, 0);
	checkstr(q(m, "ext .gz"), "", 0);
	check(strstr(get(m->userfile), "ext=.gz removed=yes") != nil);
	ctl(m, "delete ext .gz", nil, 0);
	checkstr(q(m, "ext .gz"), "ext=.gz open=gunzip\n", 0);
	/* remove of a record only this layer has just deletes it */
	ctl(m, "remove ext .md", nil, 0);
	check(strstr(get(m->userfile), ".md") == nil);
	/* errors */
	ctl(m, "layer=system/10-base set ext .x open=y", "read-only", 0);
	ctl(m, "set ext .x 'open=a \"b'", "can't be written", 0);
	ctl(m, "layer=nope set ext .x open=y", "not found", 0);
	ctl(m, "rename /a/b /a", "parent", 0);
	ctl(m, "set ext .x ext=.y", "key", 0);
	ctl(m, "set folder relative x=1", "absolute", 0);
	ctl(m, "set ext .x", "bad syntax", 0);
	ctl(m, "rename /a /a/b", "inside itself", 0);
	/* explicit layer */
	ctl(m, "layer=user set folder /usr/glenda view=list", nil, 0);
	check(strstr(get(m->userfile), "folder=/usr/glenda view=list") != nil);
	freemeta(m);
}

static void
testrights(void)
{
	Meta *m;
	char *e;

	tname = "testrights";
	nth = 0;
	setup();
	m = newmeta();
	e = metactl(m, "set folder /a x=1", Rro);
	check(e != nil && strstr(e, "permission") != nil);
	free(e);
	e = metactl(m, "set folder /a x=1 view=list", Rlay|Rro);
	check(e == nil);
	free(e);
	e = metactl(m, "set volume tmp x=5 y=6", Rlay|Rro);
	check(e == nil);
	free(e);
	e = metactl(m, "set volume tmp path=/n/other", Rlay|Rro);
	check(e != nil && strstr(e, "permission") != nil);
	free(e);
	e = metactl(m, "remove volume tmp", Rlay|Rro);
	check(e != nil && strstr(e, "permission") != nil);
	free(e);
	e = metactl(m, "rename /a /b", Rlay|Rro);
	check(e == nil);
	free(e);
	freemeta(m);
}

static void
testrename(void)
{
	Meta *m;
	char *s;

	tname = "testrename";
	nth = 0;
	setup();
	m = newmeta();
	ctl(m, "set folder /u/doc x=1\n"
		"set item /u/doc/a.txt x=10\n"
		"set item /u/doc/sub/b.txt x=20\n"
		"set item /u/docs/other x=30\n"
		"set item /u/new x=99\n"
		"layer=user set folder /u/doc/sub view=list", nil, 0);
	ctl(m, "rename /u/doc /u/new", nil, 0);
	s = q(m, "under folder /u");
	checkstr(s, "folder=/u/new x=1\nfolder=/u/new/sub view=list\n", 0);
	s = q(m, "under item /u/new");
	checkstr(s, "item=/u/new/a.txt x=10\nitem=/u/new/sub/b.txt x=20\n", 0);
	/* the stale record at the new path is replaced; the sibling stays */
	checkstr(q(m, "item /u/new"), "", 0);
	checkstr(q(m, "item /u/docs/other"), "item=/u/docs/other x=30\n", 0);
	freemeta(m);
}

static void
testexternal(void)
{
	Meta *m;
	char *s;
	uvlong g;

	tname = "testexternal";
	nth = 0;
	setup();
	m = newmeta();
	ctl(m, "set folder /u x=1", nil, 0);
	g = m->gen;
	/* someone edits the workspace file by hand */
	sleep(1100);	/* mtime has one-second resolution */
	put(m->wsfile, "folder=/u x=1\nfolder=/v x=2   # by hand\n");
	check(metarefresh(m) == 1);
	check(m->gen == g+1);
	check(m->nch == 1 && m->ch[0].op == Cset && strcmp(m->ch[0].key, "/v") == 0);
	/* a write keeps the hand edit */
	ctl(m, "set folder /u x=5", nil, 0);
	s = get(m->wsfile);
	checkstr(s, "folder=/u x=5\nfolder=/v x=2   # by hand\n#uxmeta end\n", 0);
	/* a new app appears */
	put(smprint("%s/apps/Paint/app.ndb", dir), "app=paint exec=/apps/Paint/paint\n");
	check(metarefresh(m) == 1);
	checkstr(q(m, "app paint"), "app=paint exec=/apps/Paint/paint\n", 0);
	/* and goes away */
	remove(smprint("%s/apps/Paint/app.ndb", dir));
	check(metarefresh(m) == 1);
	check(m->nch == 1 && m->ch[0].op == Cdel);
	check(metarefresh(m) == 0);
	freemeta(m);
}

static void
testrecover(void)
{
	char *p;

	tname = "testrecover";
	nth = 0;
	rmrf(dir);
	p = smprint("%s/r/f.ndb", dir);
	check(atomicwrite(p, "a=1\n") == 0);
	check(atomicwrite(p, "a=2\n") == 0);
	checkstr(get(p), "a=2\n#uxmeta end\n", 0);
	checkstr(get(smprint("%s.old", p)), "a=1\n#uxmeta end\n", 0);
	/* crash after moving f to f.old, with a complete f.new */
	put(smprint("%s.new", p), "a=3\n#uxmeta end\n");
	remove(p);
	metarecover(p);
	checkstr(get(p), "a=3\n#uxmeta end\n", 0);
	/* crash with a partial f.new: fall back to f.old */
	put(smprint("%s.new", p), "a=4\n");
	remove(p);
	metarecover(p);
	checkstr(get(p), "a=1\n#uxmeta end\n", 0);
}

static void
testgen(void)
{
	Meta *m;
	uvlong g;

	tname = "testgen";
	nth = 0;
	setup();
	m = newmeta();
	ctl(m, "set ext .q open=x", nil, 0);
	g = m->gen;
	freemeta(m);
	/* a restart continues from the saved generation */
	m = newmeta();
	check(m->gen > g);
	checkstr(q(m, "ext .q"), "ext=.q open=x\n", 0);
	freemeta(m);
}

static void
testplumb(void)
{
	Meta *m;
	char *s;

	tname = "testplumb";
	nth = 0;
	setup();
	m = newmeta();
	s = plumbrules(m);
	check(strstr(s, "# ext .txt: type text, open edit\ntype is text\n"
		"data matches '.+\\.[tT][xX][tT]'\narg isfile $0\nplumb to edit\n") != nil);
	check(strstr(s, "plumb to sketch\nplumb client /apps/Sketch/sketch\n") != nil);
	check(strstr(s, "open photo") != nil);
	free(s);
	ctl(m, "set ext .tar.gz open=tar\nset ext .gz plumb=no\nset app viewer exec='window page'\nset ext .jpg open=viewer", nil, 0);
	s = get(m->plumbfile);
	check(strstr(s, "'.+\\.[gG][zZ]'") == nil);
	check(strstr(s, "plumb start window page $file") != nil);
	/* longest suffix first */
	check(strstr(s, ".tar.gz") != nil && strstr(s, ".tar.gz") < strstr(s, "ext .txt"));
	freemeta(m);
}

void
main(int argc, char **argv)
{
	ARGBEGIN{
	}ARGEND
	USED(argc, argv);
	quotefmtinstall();
	dir = smprint("/tmp/uxmeta-unit.%d", getpid());

	testroundtrip();
	testformat();
	testprecedence();
	testresolve();
	testupdates();
	testrights();
	testrename();
	testexternal();
	testrecover();
	testgen();
	testplumb();

	rmrf(dir);
	print("unit: %d passed, %d failed\n", passed, failed);
	exits(failed ? "fail" : nil);
}
