/*
 * What a file server supports: classifying its errors, and learning
 * its capabilities by probing or from the results of operations.
 */
#include <u.h>
#include <libc.h>
#include "fcore.h"

static char *capnames[Nkind] = {
[Kread]		"read",
[Kcreate]	"create",
[Kmkdir]	"mkdir",
[Kwrite]	"write",
[Kremove]	"remove",
[Krename]	"rename",
[Kchmod]	"chmod",
[Kmtime]	"mtime",
};

char*
fccapname(int kind)
{
	if(kind < 0 || kind >= Nkind)
		return "?";
	return capnames[kind];
}

char*
fccapval(int val)
{
	switch(val){
	case Cyes:	return "yes";
	case Cno:	return "no";
	}
	return "unknown";
}

static int
has(char *s, char *sub)
{
	return cistrstr(s, sub) != nil;
}

/*
 * Map a server's error string to a class. The strings come from
 * 9front's file servers, lib9p, exportfs and the kernel's mount
 * driver; a new server's wording may need adding here.
 */
int
fcerrclass(char *err)
{
	if(err == nil || *err == 0)
		return Eother;
	/* our own phrases first */
	if(strncmp(err, "not supported", 13) == 0)
		return Enotsup;
	if(strncmp(err, "gone", 4) == 0)
		return Egone;
	if(has(err, "hungup") || has(err, "hangup") || has(err, "mount rpc")
	|| has(err, "phase error") || has(err, "unmounted") || has(err, "connection refused")
	|| has(err, "connection reset") || has(err, "server gone") || has(err, "eof on mount")
	|| has(err, "clone failed") || has(err, "broken pipe") || has(err, "timed out"))
		return Egone;
	if(has(err, "not supported") || has(err, "unsupported") || has(err, "prohibited")
	|| has(err, "not implemented") || has(err, "read only") || has(err, "read-only")
	|| has(err, "no wstat") || has(err, "forbids creation") || has(err, "bad use of fid")
	|| has(err, "attempt to change"))
		return Enotsup;
	if(has(err, "permission denied") || has(err, "not owner") || has(err, "not in group")
	|| has(err, "access denied"))
		return Eperm;
	if(has(err, "exist") && (has(err, "does not") || has(err, "doesn't") || has(err, "not exist")))
		return Enotfound;
	if(has(err, "not found") || has(err, "no such") || has(err, "file does not exist"))
		return Enotfound;
	if(has(err, "exists"))
		return Eexist;
	return Eother;
}

char*
fcerrname(int class)
{
	switch(class){
	case Eperm:	return "permission denied";
	case Enotsup:	return "not supported";
	case Egone:	return "gone";
	case Enotfound:	return "not found";
	case Eexist:	return "exists";
	}
	return "failed";
}

/*
 * Record what an operation showed. Permission errors say nothing
 * about the server, and other errors only maybe; just "not supported"
 * marks a capability as missing.
 */
void
fccapnote(Fccaps *c, int kind, int ok, char *err)
{
	if(c == nil || kind < 0 || kind >= Nkind)
		return;
	if(ok){
		c->c[kind] = Cyes;
		c->why[kind][0] = 0;
		return;
	}
	if(fcerrclass(err) == Enotsup){
		c->c[kind] = Cno;
		strecpy(c->why[kind], c->why[kind]+ERRMAX, err);
	}
}

/*
 * Ask the server what it supports by trying each operation on a
 * scratch file in dir (the volume's root by default). Everything the
 * probe makes is removed; it never touches existing files.
 */
int
fcprobe(Fcvol *v, char *dir)
{
	char err[ERRMAX], *f, *g, *sub, name[64];
	Fccaps *c;
	Dir nd, *d;
	int fd, ok;

	if(dir == nil)
		dir = v->path;
	c = &v->caps;
	d = dirstat(dir);
	if(d == nil){
		rerrstr(err, sizeof err);
		fccapnote(c, Kread, 0, err);
		werrstr("%s: %s", fcerrname(fcerrclass(err)), err);
		return -1;
	}
	free(d);
	fd = open(dir, OREAD);
	if(fd < 0){
		rerrstr(err, sizeof err);
		fccapnote(c, Kread, 0, err);
		werrstr("%s: %s", fcerrname(fcerrclass(err)), err);
		return -1;
	}
	close(fd);
	fccapnote(c, Kread, 1, nil);

	snprint(name, sizeof name, ".fcprobe.%d", getpid());
	f = fcjoin(dir, name);
	fd = create(f, OWRITE|OEXCL, 0644);
	if(fd < 0){
		rerrstr(err, sizeof err);
		fccapnote(c, Kcreate, 0, err);
		if(fcerrclass(err) == Eperm){
			free(f);
			werrstr("permission denied: can't probe %s: %s", dir, err);
			return -1;
		}
		/* without a scratch file, nothing else can be tried */
		free(f);
		return 0;
	}
	fccapnote(c, Kcreate, 1, nil);
	ok = write(fd, "probe\n", 6) == 6;
	rerrstr(err, sizeof err);
	fccapnote(c, Kwrite, ok, err);
	close(fd);

	nulldir(&nd);
	nd.mode = 0600;
	ok = dirwstat(f, &nd) >= 0;
	rerrstr(err, sizeof err);
	fccapnote(c, Kchmod, ok, err);

	nulldir(&nd);
	nd.mtime = 1000000000;
	ok = dirwstat(f, &nd) >= 0;
	rerrstr(err, sizeof err);
	fccapnote(c, Kmtime, ok, err);

	snprint(name, sizeof name, ".fcprobe.%d.r", getpid());
	nulldir(&nd);
	nd.name = name;
	ok = dirwstat(f, &nd) >= 0;
	rerrstr(err, sizeof err);
	fccapnote(c, Krename, ok, err);
	g = f;
	if(ok){
		g = fcjoin(dir, name);
		free(f);
	}

	snprint(name, sizeof name, ".fcprobe.%d.d", getpid());
	sub = fcjoin(dir, name);
	fd = create(sub, OREAD|OEXCL, DMDIR|0755);
	if(fd >= 0){
		close(fd);
		fccapnote(c, Kmkdir, 1, nil);
		remove(sub);
	}else{
		rerrstr(err, sizeof err);
		fccapnote(c, Kmkdir, 0, err);
	}
	free(sub);

	ok = remove(g) >= 0;
	rerrstr(err, sizeof err);
	fccapnote(c, Kremove, ok, err);
	if(!ok){
		werrstr("partial: probe file %s left behind: %s", g, err);
		free(g);
		return -1;
	}
	free(g);
	return 0;
}
