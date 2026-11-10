/*
 * clipd: the typed clipboard, /mnt/clip (docs/protocols/clip.md).
 *
 * Holds several representations ("types") of one copied thing and keeps
 * the text one in step with rio's snarf both ways: a copy with a text
 * type is written to snarf, and text a legacy program put in snarf
 * becomes the clipboard.
 *
 * Usage: clipd [-D] [-n snarffile] [-s srvname] [-m mtpt]
 *
 * lib9p runs the file server in its own proc; a second proc looks at
 * snarf once a second while anyone follows events. One QLock guards
 * everything.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

typedef struct Rep Rep;
typedef struct Sub Sub;
typedef struct Aux Aux;

enum {
	Qroot,
	Qversion,
	Qctl,
	Qtypes,
	Qevents,
	Qdatadir,
	Qdata,

	Maxrep	= 16*1024*1024,
	Maxq	= 256,
	Maxtypes	= 64,
};

struct Rep {
	char	*type;
	char	*buf;
	long	n;
	int	ref;
	Rep	*next;
};

struct Sub {
	char	*q[Maxq];
	int	nq;
	Req	*r;
	Sub	*next;
};

struct Aux {
	/* data written: the type and its bytes so far */
	char	*wtype;
	char	*wbuf;
	long	wn;
	int	wfail;
	/* data read: the representation at open */
	Rep	*rep;
	/* types: the snapshot */
	char	*snap;
	/* ctl: holds the batch */
	int	batch;
	/* events */
	Sub	*sub;
};

static QLock	lk;
static Rep	*reps;		/* the clipboard, in the order written */
static Rep	*pending;	/* collected inside begin ... commit */
static Fid	*batchfid;
static ulong	gen;
static ulong	seq;
static Sub	*subs;
static char	*snarf = "/dev/snarf";
static char	*lastsnarf;	/* what we last wrote to or saw in snarf */
static long	nlastsnarf;
static File	*datadir;
static char	*names[Maxtypes];	/* the files in data/ */
static int	nnames;
static char	version[] = "clip 1.0\n";
static char	ctlhelp[] = "clear -\nbegin -\ncommit -\nabort -\n";

static int
validtype(char *s)
{
	char *p;

	if(s == nil || *s == 0 || strlen(s) > 64)
		return 0;
	if(!(*s >= 'a' && *s <= 'z') && !(*s >= '0' && *s <= '9'))
		return 0;
	for(p = s; *p; p++)
		if(!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9')
		&& *p != '.' && *p != '_' && *p != '-')
			return 0;
	return 1;
}

static void
repput(Rep *r)
{
	if(r == nil || --r->ref > 0)
		return;
	free(r->type);
	free(r->buf);
	free(r);
}

static void
repsfree(Rep *r)
{
	Rep *n;

	for(; r != nil; r = n){
		n = r->next;
		repput(r);
	}
}

static Rep*
findrep(Rep *list, char *type)
{
	for(; list != nil; list = list->next)
		if(strcmp(list->type, type) == 0)
			return list;
	return nil;
}

/* add or replace a type at the end of a list */
static Rep*
addrep(Rep *list, char *type, char *buf, long n)
{
	Rep *r, **l;

	for(l = &list; *l != nil; )
		if(strcmp((*l)->type, type) == 0){
			r = *l;
			*l = r->next;
			repput(r);
		}else
			l = &(*l)->next;
	r = emalloc9p(sizeof *r);
	memset(r, 0, sizeof *r);
	r->type = estrdup9p(type);
	r->buf = buf;
	r->n = n;
	r->ref = 1;
	*l = r;
	return list;
}

/*
 * Events
 */
static void
answer(Sub *s)
{
	Req *r;
	long n, l;
	int k;

	if(s->r == nil || s->nq == 0)
		return;
	r = s->r;
	n = 0;
	for(k = 0; k < s->nq; k++){
		l = strlen(s->q[k]);
		if(n+l > r->ifcall.count)
			break;
		memmove(r->ofcall.data+n, s->q[k], l);
		n += l;
		free(s->q[k]);
	}
	if(k == 0){
		s->r = nil;
		respond(r, "short read");
		return;
	}
	s->nq -= k;
	memmove(s->q, s->q+k, s->nq*sizeof s->q[0]);
	r->ofcall.count = n;
	s->r = nil;
	respond(r, nil);
}

static void
enq(Sub *s, ulong sq, char *msg)
{
	int i, n;

	if(s->nq == Maxq){
		n = s->nq+1;
		for(i = 0; i < s->nq; i++)
			free(s->q[i]);
		s->nq = 0;
		s->q[s->nq++] = smprint("%lud lost n=%d\n", sq, n);
	}else
		s->q[s->nq++] = smprint("%lud %s\n", sq, msg);
	answer(s);
}

static char*
typelist(Rep *r)
{
	Fmt f;
	int n;

	fmtstrinit(&f);
	for(n = 0; r != nil; r = r->next)
		fmtprint(&f, "%s%s", n++ ? "," : "", r->type);
	return fmtstrflush(&f);
}

/* the clipboard changed: data/ follows it, and subscribers hear */
static void
changed(char *via)
{
	Sub *s;
	char *t, *msg;
	File *f;
	Rep *r;
	int i;

	gen++;
	/* data/: one file per type held */
	if(datadir != nil){
		for(i = 0; i < nnames; ){
			if(findrep(reps, names[i]) == nil){
				incref(datadir);
				if((f = walkfile(datadir, names[i])) != nil)
					removefile(f);
				free(names[i]);
				names[i] = names[--nnames];
			}else
				i++;
		}
		for(r = reps; r != nil; r = r->next){
			incref(datadir);	/* walkfile uses up a reference */
			f = walkfile(datadir, r->type);
			if(f == nil)
				f = createfile(datadir, r->type, nil, 0666, (void*)Qdata);
			if(f != nil)
				closefile(f);
			for(i = 0; i < nnames; i++)
				if(strcmp(names[i], r->type) == 0)
					break;
			if(i == nnames && nnames < Maxtypes)
				names[nnames++] = estrdup9p(r->type);
		}
	}
	t = typelist(reps);
	msg = smprint("change gen=%lud types=%s via=%s", gen, t, via);
	free(t);
	seq++;
	for(s = subs; s != nil; s = s->next)
		enq(s, seq, msg);
	free(msg);
}

/*
 * Snarf
 */
static char*
readall(char *path, long *np)
{
	char *buf;
	long n, m;
	int fd;

	*np = 0;
	fd = open(path, OREAD);
	if(fd < 0)
		return nil;
	buf = emalloc9p(8192+1);
	n = 0;
	while((m = read(fd, buf+n, 8192)) > 0){
		n += m;
		if(n > Maxrep)
			break;
		buf = erealloc9p(buf, n+8192+1);
	}
	close(fd);
	buf[n] = 0;
	*np = n;
	return buf;
}

static void
setlast(char *buf, long n)
{
	free(lastsnarf);
	lastsnarf = emalloc9p(n+1);
	memmove(lastsnarf, buf, n);
	nlastsnarf = n;
}

static void
tosnarf(void)
{
	Rep *r;
	int fd;

	r = findrep(reps, "text");
	if(r == nil)
		return;
	fd = open(snarf, OWRITE|OTRUNC);
	if(fd < 0)
		return;
	write(fd, r->buf, r->n);
	close(fd);
	setlast(r->buf, r->n);
}

/* has a legacy program copied? lk held */
static void
fromsnarf(void)
{
	char *buf;
	long n;

	buf = readall(snarf, &n);
	if(buf == nil)
		return;
	if(n == 0 || (n == nlastsnarf && memcmp(buf, lastsnarf, n) == 0)){
		free(buf);
		return;
	}
	setlast(buf, n);
	repsfree(reps);
	reps = addrep(nil, "text", buf, n);
	changed("snarf");
}

static void
poller(void*)
{
	for(;;){
		sleep(1000);
		qlock(&lk);
		if(subs != nil)
			fromsnarf();
		qunlock(&lk);
	}
}

/*
 * 9P
 */
static long
qtype(Fid *f)
{
	return (long)(uintptr)f->file->aux;
}

static Aux*
aux(Fid *f)
{
	if(f->aux == nil){
		f->aux = emalloc9p(sizeof(Aux));
		memset(f->aux, 0, sizeof(Aux));
	}
	return f->aux;
}

static char*
typesnap(void)
{
	Fmt f;
	Rep *r;

	fmtstrinit(&f);
	fmtprint(&f, "gen %lud\n", gen);
	for(r = reps; r != nil; r = r->next)
		fmtprint(&f, "type %s size=%ld\n", r->type, r->n);
	return fmtstrflush(&f);
}

static void
openwrite(Fid *fid, char *type)
{
	Aux *a;

	a = aux(fid);
	a->wtype = estrdup9p(type);
	a->wbuf = nil;
	a->wn = 0;
}

static void
fsopen(Req *r)
{
	Aux *a;
	Sub *s;
	int mode;

	qlock(&lk);
	a = aux(r->fid);
	mode = r->ifcall.mode & 3;
	switch(qtype(r->fid)){
	case Qtypes:
		fromsnarf();
		a->snap = typesnap();
		break;
	case Qdatadir:
		fromsnarf();
		break;
	case Qevents:
		s = emalloc9p(sizeof *s);
		memset(s, 0, sizeof *s);
		seq++;
		s->q[s->nq++] = smprint("%lud resync\n", seq);
		s->next = subs;
		subs = s;
		a->sub = s;
		break;
	case Qdata:
		if(mode == OWRITE || mode == ORDWR || (r->ifcall.mode & OTRUNC)){
			openwrite(r->fid, r->fid->file->name);
			break;
		}
		fromsnarf();
		a->rep = findrep(reps, r->fid->file->name);
		if(a->rep == nil){
			qunlock(&lk);
			respond(r, "not found");
			return;
		}
		a->rep->ref++;
		break;
	}
	qunlock(&lk);
	respond(r, nil);
}

static void
fscreate(Req *r)
{
	File *f;

	if(qtype(r->fid) != Qdatadir){
		respond(r, "permission denied");
		return;
	}
	if(!validtype(r->ifcall.name) || (r->ifcall.perm & DMDIR)){
		respond(r, "bad value: not a type name");
		return;
	}
	qlock(&lk);
	incref(r->fid->file);	/* walkfile uses up a reference */
	f = walkfile(r->fid->file, r->ifcall.name);
	if(f == nil)
		f = createfile(r->fid->file, r->ifcall.name, nil, 0666, (void*)Qdata);
	if(f == nil){
		qunlock(&lk);
		respond(r, "failed: can't create");
		return;
	}
	closefile(r->fid->file);
	r->fid->file = f;
	r->ofcall.qid = f->qid;
	openwrite(r->fid, r->ifcall.name);
	qunlock(&lk);
	respond(r, nil);
}

static void
fsread(Req *r)
{
	Aux *a;

	qlock(&lk);
	a = aux(r->fid);
	switch(qtype(r->fid)){
	case Qversion:
		readstr(r, version);
		break;
	case Qctl:
		readstr(r, ctlhelp);
		break;
	case Qtypes:
		readstr(r, a->snap != nil ? a->snap : "");
		break;
	case Qdata:
		if(a->rep == nil){
			qunlock(&lk);
			respond(r, "bad syntax: open for reading");
			return;
		}
		readbuf(r, a->rep->buf, a->rep->n);
		break;
	case Qevents:
		if(a->sub->r != nil){
			qunlock(&lk);
			respond(r, "busy");
			return;
		}
		a->sub->r = r;
		answer(a->sub);
		qunlock(&lk);
		return;
	default:
		qunlock(&lk);
		respond(r, "bug: read");
		return;
	}
	qunlock(&lk);
	respond(r, nil);
}

static char*
ctlcmd(Fid *fid, char *line)
{
	char *f[4];
	int nf;
	Aux *a;

	a = aux(fid);
	nf = tokenize(line, f, nelem(f));
	if(nf == 0)
		return nil;
	if(nf != 1)
		return "bad syntax";
	if(strcmp(f[0], "begin") == 0){
		if(batchfid != nil && batchfid != fid)
			return "busy";
		repsfree(pending);
		pending = nil;
		batchfid = fid;
		a->batch = 1;
		return nil;
	}
	if(strcmp(f[0], "commit") == 0 || strcmp(f[0], "abort") == 0){
		if(batchfid != fid)
			return "bad syntax: no batch";
		batchfid = nil;
		a->batch = 0;
		if(f[0][0] == 'a'){
			repsfree(pending);
			pending = nil;
			return nil;
		}
		repsfree(reps);
		reps = pending;
		pending = nil;
		tosnarf();
		changed("clip");
		return nil;
	}
	if(strcmp(f[0], "clear") == 0){
		repsfree(reps);
		reps = nil;
		changed("clip");
		return nil;
	}
	return "unknown command";
}

static void
fswrite(Req *r)
{
	Aux *a;
	char *buf, *p, *e, *err;
	long n;

	qlock(&lk);
	a = aux(r->fid);
	n = r->ifcall.count;
	switch(qtype(r->fid)){
	case Qctl:
		buf = emalloc9p(n+1);
		memmove(buf, r->ifcall.data, n);
		buf[n] = 0;
		err = nil;
		for(p = buf; *p && err == nil; p = e){
			e = strchr(p, '\n');
			if(e != nil)
				*e++ = 0;
			else
				e = p+strlen(p);
			err = ctlcmd(r->fid, p);
		}
		free(buf);
		qunlock(&lk);
		if(err == nil)
			r->ofcall.count = n;
		respond(r, err);
		return;
	case Qdata:
		if(a->wtype == nil){
			qunlock(&lk);
			respond(r, "bad syntax: open for writing");
			return;
		}
		if(a->wn+n > Maxrep){
			a->wfail = 1;
			qunlock(&lk);
			respond(r, "too large");
			return;
		}
		a->wbuf = erealloc9p(a->wbuf, a->wn+n+1);
		memmove(a->wbuf+a->wn, r->ifcall.data, n);
		a->wn += n;
		a->wbuf[a->wn] = 0;
		qunlock(&lk);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	qunlock(&lk);
	respond(r, "permission denied");
}

static void
fsflush(Req *r)
{
	Sub *s;

	qlock(&lk);
	for(s = subs; s != nil; s = s->next)
		if(s->r == r->oldreq){
			s->r = nil;
			respond(r->oldreq, "interrupted");
			break;
		}
	qunlock(&lk);
	respond(r, nil);
}

static void
fsdestroyfid(Fid *fid)
{
	Aux *a;
	Sub *s, **l;
	int i;

	a = fid->aux;
	if(a == nil)
		return;
	qlock(&lk);
	if(a->wtype != nil && !a->wfail){
		if(a->wbuf == nil)
			a->wbuf = estrdup9p("");
		if(batchfid != nil)
			pending = addrep(pending, a->wtype, a->wbuf, a->wn);
		else{
			repsfree(reps);
			reps = addrep(nil, a->wtype, a->wbuf, a->wn);
			tosnarf();
			changed("clip");
		}
		a->wbuf = nil;
	}
	free(a->wtype);
	free(a->wbuf);
	repput(a->rep);
	free(a->snap);
	if(batchfid == fid){
		batchfid = nil;
		repsfree(pending);
		pending = nil;
	}
	if(a->sub != nil){
		for(l = &subs; (s = *l) != nil; l = &s->next)
			if(s == a->sub){
				*l = s->next;
				break;
			}
		for(i = 0; i < a->sub->nq; i++)
			free(a->sub->q[i]);
		free(a->sub);
	}
	free(a);
	fid->aux = nil;
	qunlock(&lk);
}

static Srv fs = {
	.open = fsopen,
	.create = fscreate,
	.read = fsread,
	.write = fswrite,
	.flush = fsflush,
	.destroyfid = fsdestroyfid,
};

static void
mk(File *dir, char *name, int q, ulong perm)
{
	File *f;

	f = createfile(dir, name, nil, perm, (void*)q);
	if(f == nil)
		sysfatal("createfile %s: %r", name);
	closefile(f);
}

static void
usage(void)
{
	fprint(2, "usage: %s [-D] [-n snarffile] [-s srvname] [-m mtpt]\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char **argv)
{
	char *srvname, *mtpt, *buf;
	long n;

	srvname = smprint("clip.%s", getuser());
	mtpt = "/mnt/clip";
	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 'n':
		snarf = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		if(strcmp(mtpt, "-") == 0)
			mtpt = nil;
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	/* what's in snarf now is not a new copy */
	buf = readall(snarf, &n);
	if(buf != nil){
		setlast(buf, n);
		if(n > 0)
			reps = addrep(nil, "text", buf, n);
		else
			free(buf);
	}

	fs.tree = alloctree(nil, nil, DMDIR|0777, nil);
	mk(fs.tree->root, "version", Qversion, 0444);
	mk(fs.tree->root, "ctl", Qctl, 0666);
	mk(fs.tree->root, "types", Qtypes, 0444);
	mk(fs.tree->root, "events", Qevents, 0444);
	datadir = createfile(fs.tree->root, "data", nil, DMDIR|0777, (void*)Qdatadir);
	if(datadir == nil)
		sysfatal("createfile data: %r");
	if(reps != nil){
		gen = 0;
		changed("snarf");
		gen = 1;
	}
	proccreate(poller, nil, 16*1024);
	threadpostmountsrv(&fs, srvname, mtpt, MREPL|MCREATE);
	threadexits(nil);
}
