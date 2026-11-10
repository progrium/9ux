/*
 * uxmeta: serve the merged metadata over 9P.  See docs/protocols/meta.md.
 *
 * All state is behind one lock, taken by the 9P handlers and by the
 * poller that notices outside edits, so there is exactly one writer.
 * Handlers never block: reads of changes wait as queued requests.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "meta.h"

enum {
	Qroot,
	Qversion,
	Qgen,
	Qctl,
	Qquery,
	Qdb,
	Qchanges,
	Qlayers,
	Qplumbing,
	Nqid,

	Qmax = 256,	/* lines held per changes reader */
};

static struct {
	char	*name;
	int	mode;
} tab[Nqid] = {
	[Qroot]		"/",		DMDIR|0555,
	[Qversion]	"version",	0444,
	[Qgen]		"gen",		0444,
	[Qctl]		"ctl",		0666,
	[Qquery]	"query",	0666,
	[Qdb]		"db",		0444,
	[Qchanges]	"changes",	0444,
	[Qlayers]	"layers",	0444,
	[Qplumbing]	"plumbing",	0444,
};

static char Version[] =
	"uxmeta 1.0\n"
	"feature query\n"
	"feature resolve\n"
	"feature explain\n"
	"feature changes\n"
	"feature rename\n"
	"feature ifgen\n"
	"feature plumbing\n";

static char Ctlhelp[] =
	"set layout\n"
	"unset layout\n"
	"remove layout\n"
	"delete layout\n"
	"rename layout\n"
	"reload rw\n";

typedef struct Reader Reader;
struct Reader {
	char	*q[Qmax];
	int	n;
	Req	**wait;
	int	nwait;
	Reader	*next;
};

typedef struct Faux Faux;
struct Faux {
	int	rights;
	char	*buf;	/* snapshot, query result, or ctl reply */
	long	pos;	/* query and ctl: read position, reset by each write */
	Reader	*rd;
};

static QLock lk;
static Meta meta;
static Reader *readers;
static char *owner;
static int pollms = 1000;
static uvlong seq;	/* event sequence, shared by every changes file */

static void
qidof(int q, Qid *qid)
{
	qid->path = q;
	qid->vers = 0;
	qid->type = q == Qroot ? QTDIR : QTFILE;
}

static void
fillstat(Dir *d, int q)
{
	memset(d, 0, sizeof *d);
	d->name = estrdup9p(q == Qroot ? "/" : tab[q].name);
	d->uid = estrdup9p(owner);
	d->gid = estrdup9p(owner);
	d->muid = estrdup9p(owner);
	qidof(q, &d->qid);
	d->mode = tab[q].mode;
	d->atime = d->mtime = time(0);
}

static int
rootgen(int i, Dir *d, void*)
{
	if(i+1 >= Nqid)
		return -1;
	fillstat(d, i+1);
	return 0;
}

static void
fsattach(Req *r)
{
	Faux *a;
	char *s;
	int rights;

	if(strcmp(r->ifcall.uname, owner) != 0){
		respond(r, "permission denied");
		return;
	}
	s = r->ifcall.aname;
	if(s == nil || *s == 0 || strcmp(s, "rw") == 0)
		rights = Rrw|Rlay|Rro;
	else if(strcmp(s, "layout") == 0)
		rights = Rlay|Rro;
	else if(strcmp(s, "ro") == 0)
		rights = Rro;
	else{
		respond(r, "bad value: unknown attach name");
		return;
	}
	a = emalloc9p(sizeof *a);
	a->rights = rights;
	r->fid->aux = a;
	qidof(Qroot, &r->fid->qid);
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

static char*
fswalk1(Fid *fid, char *name, Qid *qid)
{
	int i;

	if(fid->qid.path != Qroot)
		return "not a directory";
	if(strcmp(name, "..") == 0){
		qidof(Qroot, qid);
		fid->qid = *qid;
		return nil;
	}
	for(i = 1; i < Nqid; i++)
		if(strcmp(name, tab[i].name) == 0){
			qidof(i, qid);
			fid->qid = *qid;
			return nil;
		}
	return "file does not exist";
}

static char*
fsclone(Fid *old, Fid *new)
{
	Faux *a, *o;

	o = old->aux;
	a = emalloc9p(sizeof *a);
	a->rights = o ? o->rights : Rro;
	new->aux = a;
	return nil;
}

static void
fsstat(Req *r)
{
	fillstat(&r->d, r->fid->qid.path);
	respond(r, nil);
}

/* queue a message; one that doesn't fit replaces the queue with "lost" */
static void
pushline(Reader *rd, uvlong sq, char *line)
{
	int i, n;

	if(rd->n >= Qmax){
		n = rd->n + 1;
		for(i = 0; i < rd->n; i++)
			free(rd->q[i]);
		rd->n = 0;
		rd->q[rd->n++] = smprint("%llud lost n=%d\n", sq, n);
		free(line);
		return;
	}
	rd->q[rd->n++] = line;
}

/*
 * Answer waiting reads with whole messages; a read too small for the
 * next one fails with "short read" (see protocols/README.md).
 */
static void
deliver(Reader *rd)
{
	Req *r;
	long n, l, max;
	int i;

	while(rd->nwait > 0 && rd->n > 0){
		r = rd->wait[0];
		rd->nwait--;
		memmove(rd->wait, rd->wait+1, rd->nwait*sizeof rd->wait[0]);
		max = r->ifcall.count;
		n = 0;
		for(i = 0; i < rd->n; i++){
			l = strlen(rd->q[i]);
			if(n + l > max)
				break;
			memmove(r->ofcall.data+n, rd->q[i], l);
			n += l;
		}
		if(i == 0){
			respond(r, "short read");
			continue;
		}
		r->ofcall.count = n;
		while(i-- > 0){
			free(rd->q[0]);
			rd->n--;
			memmove(rd->q, rd->q+1, rd->n*sizeof rd->q[0]);
		}
		respond(r, nil);
	}
}

static void
fsnotify(Meta *m, Change *ch, int n)
{
	Reader *rd;
	int i;
	uvlong sq;

	for(i = 0; i < n; i++){
		sq = ++seq;
		for(rd = readers; rd; rd = rd->next)
			pushline(rd, sq, smprint("%llud %s %q %q gen=%llud\n", sq,
				ch[i].op == Cset ? "set" : "del", ch[i].kind, ch[i].key, m->gen));
	}
	for(rd = readers; rd; rd = rd->next)
		deliver(rd);
}

static void
fsopen(Req *r)
{
	Faux *a;
	int q;

	a = r->fid->aux;
	q = r->fid->qid.path;
	if((r->ifcall.mode&3) != OREAD && !(tab[q].mode & 0222)){
		respond(r, "permission denied");
		return;
	}
	qlock(&lk);
	switch(q){
	case Qdb:
		metarefresh(&meta);
		a->buf = metadb(&meta);
		break;
	case Qlayers:
		metarefresh(&meta);
		a->buf = metalayers(&meta);
		break;
	case Qplumbing:
		metarefresh(&meta);
		a->buf = estrdup(meta.rules ? meta.rules : "");
		break;
	case Qgen:
		metarefresh(&meta);
		a->buf = smprint("%llud\n", meta.gen);
		break;
	case Qchanges:
		a->rd = emalloc9p(sizeof *a->rd);
		a->rd->next = readers;
		readers = a->rd;
		++seq;
		pushline(a->rd, seq, smprint("%llud resync gen=%llud\n", seq, meta.gen));
		break;
	}
	qunlock(&lk);
	respond(r, nil);
}

static void
fsread(Req *r)
{
	Faux *a;
	Reader *rd;

	a = r->fid->aux;
	switch((int)r->fid->qid.path){
	case Qroot:
		dirread9p(r, rootgen, nil);
		respond(r, nil);
		return;
	case Qversion:
		readstr(r, Version);
		respond(r, nil);
		return;
	case Qchanges:
		qlock(&lk);
		rd = a->rd;
		rd->wait = erealloc9p(rd->wait, (rd->nwait+1)*sizeof rd->wait[0]);
		rd->wait[rd->nwait++] = r;
		deliver(rd);
		qunlock(&lk);
		return;
	}
	if(r->fid->qid.path == Qctl){
		readstr(r, Ctlhelp);
		respond(r, nil);
		return;
	}
	if(r->fid->qid.path == Qquery){
		/* results read from their start after each write, whatever the offset */
		r->ifcall.offset = a->pos;
		readstr(r, a->buf ? a->buf : "");
		a->pos += r->ofcall.count;
	}else
		readstr(r, a->buf ? a->buf : "");
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	Faux *a;
	char *s, *err;

	a = r->fid->aux;
	s = emalloc9p(r->ifcall.count+1);
	memmove(s, r->ifcall.data, r->ifcall.count);
	s[r->ifcall.count] = 0;
	err = nil;
	a->pos = 0;
	qlock(&lk);
	switch((int)r->fid->qid.path){
	case Qctl:
		free(a->buf);
		a->buf = nil;
		err = metactl(&meta, s, a->rights);
		break;
	case Qquery:
		free(a->buf);
		metarefresh(&meta);
		if((a->buf = metaquery(&meta, s)) == nil){
			char e[ERRMAX];
			rerrstr(e, sizeof e);
			err = estrdup(e);
		}
		break;
	default:
		err = estrdup("permission denied");
	}
	qunlock(&lk);
	free(s);
	if(err == nil)
		r->ofcall.count = r->ifcall.count;
	respond(r, err);
	free(err);
}

static void
fsflush(Req *r)
{
	Reader *rd;
	Req *o;
	int i;

	o = r->oldreq;
	qlock(&lk);
	for(rd = readers; rd; rd = rd->next)
		for(i = 0; i < rd->nwait; i++)
			if(rd->wait[i] == o){
				rd->nwait--;
				memmove(rd->wait+i, rd->wait+i+1, (rd->nwait-i)*sizeof rd->wait[0]);
				respond(o, "interrupted");
				goto Out;
			}
Out:
	qunlock(&lk);
	respond(r, nil);
}

static void
fsdestroyfid(Fid *fid)
{
	Faux *a;
	Reader **l, *rd;
	int i;

	a = fid->aux;
	if(a == nil)
		return;
	if((rd = a->rd) != nil){
		qlock(&lk);
		for(l = &readers; *l; l = &(*l)->next)
			if(*l == rd){
				*l = rd->next;
				break;
			}
		for(i = 0; i < rd->n; i++)
			free(rd->q[i]);
		for(i = 0; i < rd->nwait; i++)
			respond(rd->wait[i], "interrupted");
		free(rd->wait);
		free(rd);
		qunlock(&lk);
	}
	free(a->buf);
	free(a);
}

static void
fsend(Srv*)
{
	threadexitsall(nil);
}

static Srv fs = {
	.attach =	fsattach,
	.walk1 =	fswalk1,
	.clone =	fsclone,
	.stat =	fsstat,
	.open =	fsopen,
	.read =	fsread,
	.write =	fswrite,
	.flush =	fsflush,
	.destroyfid =	fsdestroyfid,
	.end =	fsend,
};

/*
 * The poller can answer blocked changes reads, so it must share the
 * file descriptors of the proc serving 9P: start it from there.
 */
static void
poller(void*)
{
	for(;;){
		sleep(pollms);
		qlock(&lk);
		metarefresh(&meta);
		qunlock(&lk);
	}
}

static void
startpoller(Srv*)
{
	proccreate(poller, nil, 8192);
}

static void
usage(void)
{
	fprint(2, "usage: %s [-DP] [-s srvname] [-m mtpt | -n] [-d sysdir] [-a appsdir] [-u userdir] [-w workspace] [-i pollms]\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char **argv)
{
	char *srvname, *mtpt, *sysdir, *appsdir, *userdir, *ws, *home, *s;
	int lockfd;

	owner = getuser();
	home = getenv("home");
	if(home == nil)
		home = smprint("/usr/%s", owner);
	srvname = smprint("uxmeta.%s", owner);
	mtpt = "/mnt/meta";
	sysdir = "/lib/ux/meta";
	appsdir = "/apps";
	userdir = smprint("%s/lib/ux", home);
	ws = getenv("workspace");
	if(ws == nil || *ws == 0)
		ws = "default";
	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 'P':
		meta.reloadplumb = 1;
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 'n':
		mtpt = nil;
		break;
	case 'd':
		sysdir = EARGF(usage());
		break;
	case 'a':
		appsdir = EARGF(usage());
		break;
	case 'u':
		userdir = EARGF(usage());
		break;
	case 'w':
		ws = EARGF(usage());
		break;
	case 'i':
		pollms = atoi(EARGF(usage()));
		if(pollms < 10)
			pollms = 10;
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();
	if(strchr(ws, '/') != nil || strcmp(ws, ".") == 0 || strcmp(ws, "..") == 0)
		sysfatal("bad workspace name %q", ws);

	quotefmtinstall();
	if(mkdirs(userdir) < 0)
		sysfatal("mkdir %s: %r", userdir);
	s = smprint("%s/.uxmeta.lock", userdir);
	if((lockfd = metalock(s)) < 0)
		sysfatal("lock %s: %r (is another uxmeta running?)", s);
	USED(lockfd);
	free(s);

	meta.sysdir = sysdir;
	meta.appsdir = appsdir;
	meta.userfile = smprint("%s/meta.ndb", userdir);
	meta.wsfile = smprint("%s/workspace/%s.ndb", userdir, ws);
	meta.genfile = smprint("%s/meta.gen", userdir);
	meta.plumbfile = smprint("%s/plumbing", userdir);
	meta.userplumbing = smprint("%s/lib/plumbing", home);
	meta.plumbrulesfile = "/mnt/plumb/rules";
	if((meta.rules = readfile(meta.plumbfile, nil)) != nil){
		/* compare against what's on disk, without the trailer */
		int n = strlen(meta.rules), t = strlen("#uxmeta end\n");
		if(n >= t && strcmp(meta.rules+n-t, "#uxmeta end\n") == 0)
			meta.rules[n-t] = 0;
	}
	metaloadgen(&meta);
	metarescan(&meta);
	metacommit(&meta);	/* gen+1 if there is anything at all */
	metaplumb(&meta);
	meta.notify = fsnotify;

	fs.start = startpoller;
	threadpostmountsrv(&fs, srvname, mtpt, MREPL|MCREATE);
	threadexits(nil);
}
