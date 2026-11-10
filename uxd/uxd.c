/*
 * uxd: discovers running 9/UX apps and serves them as one tree, with
 * policy (docs/protocols/ux.md).
 *
 *	uxd [-D] [-s srvname] [-m mtpt] [-g grantfile]
 *
 * Posts /srv/$srvname (the public service: read, or a grant's rights)
 * and /srv/$srvname.admin (the session service: every right). srvname
 * defaults to uxd.$user. With -m it mounts the session service on
 * mtpt, normally /mnt/ux. Grants are kept in grantfile, by default
 * $home/lib/ux/grants.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "dat.h"
#include "fns.h"

char *user;
ulong starttime;

static Channel *reqchan;	/* Req* from the lib9p procs */
static Channel *fidchan;	/* Fx* of destroyed fids */
static Srv pubsrv, adminsrv;
int mainstacksize = 64*1024;

void
reply(Op *op, char *err)
{
	Req *r;

	r = op->r;
	r->aux = nil;
	respond(r, err);
	free(op->freeme);
	freemsg(op->msg);
	free(op);
}

static void
handle(void *v)
{
	Op *op;
	Req *r;

	op = v;
	r = op->r;
	threadsetname("uxd req");
	if(op->flushed && r->ifcall.type != Tflush){
		reply(op, "interrupted");
		return;
	}
	switch(r->ifcall.type){
	case Tattach:
		fsattach(op, r->srv == &adminsrv);
		break;
	case Twalk:
		fswalk(op);
		break;
	case Topen:
		fsopen(op);
		break;
	case Tread:
		fsread(op);
		break;
	case Twrite:
		fswrite(op);
		break;
	case Tstat:
		fsstat(op);
		break;
	case Tflush:
		fsflush(op);
		break;
	default:
		reply(op, "permission denied");
		break;
	}
}

static void
reqthread(void*)
{
	Req *r;
	Op *op;

	threadsetname("uxd reqs");
	while((r = recvp(reqchan)) != nil){
		op = emalloc9p(sizeof *op);
		memset(op, 0, sizeof *op);
		op->r = r;
		r->aux = op;
		threadcreate(handle, op, Stack);
	}
}

static void
fidthread(void*)
{
	Fx *f;

	threadsetname("uxd fids");
	while((f = recvp(fidchan)) != nil)
		freefx(f);
}

static void
rxthread(void*)
{
	Msg *m;

	threadsetname("uxd replies");
	while((m = recvp(rxchan)) != nil)
		dispatch(m);
}

static void
workproc(void*)
{
	threadsetname("uxd main");
	threadcreate(fidthread, nil, Stack);
	threadcreate(rxthread, nil, Stack);
	reqthread(nil);
}

/* lib9p procs: hand everything to the main proc */
static void
fsreq(Req *r)
{
	sendp(reqchan, r);
}

static void
fsdestroyfid(Fid *f)
{
	if(f->aux != nil){
		sendp(fidchan, f->aux);
		f->aux = nil;
	}
}

static void
initsrv(Srv *s)
{
	s->attach = fsreq;
	s->walk = fsreq;
	s->open = fsreq;
	s->read = fsreq;
	s->write = fsreq;
	s->stat = fsreq;
	s->create = fsreq;
	s->remove = fsreq;
	s->wstat = fsreq;
	s->flush = fsreq;
	s->destroyfid = fsdestroyfid;
}

static void
srvproc(void *v)
{
	threadsetname("uxd srv");
	srv(v);
}

/*
 * Like threadpostsrv, but every proc keeps sharing one fd table:
 * replies are written from the main proc, which must see the srv's fd.
 * The /srv entry is open ORCLOSE, so it goes away when uxd exits.
 */
static int
post(Srv *s, char *name)
{
	char buf[128];
	int p[2], fd;

	snprint(buf, sizeof buf, "/srv/%s", name);
	if(pipe(p) < 0)
		return -1;
	if((fd = create(buf, OWRITE|ORCLOSE|OCEXEC, 0600)) < 0)
		return -1;
	if(fprint(fd, "%d", p[0]) < 0){
		close(fd);
		return -1;
	}
	s->infd = s->outfd = p[1];
	proccreate(srvproc, s, Stack);
	return p[0];
}

char*
readfile(char *file)
{
	char *s;
	long n, tot;
	int fd;

	if((fd = open(file, OREAD)) < 0)
		return nil;
	s = nil;
	tot = 0;
	for(;;){
		s = erealloc9p(s, tot+8192+1);
		if((n = read(fd, s+tot, 8192)) <= 0)
			break;
		tot += n;
	}
	close(fd);
	s[tot] = 0;
	return s;
}

void
mkdirs(char *dir)
{
	char *p, *s;
	int fd;

	s = estrdup9p(dir);
	for(p = s+1;; p++){
		if(*p == '/' || *p == 0){
			char c = *p;
			*p = 0;
			if(access(s, AEXIST) < 0 && (fd = create(s, OREAD, DMDIR|0700)) >= 0)
				close(fd);
			*p = c;
			if(c == 0)
				break;
		}
	}
	free(s);
}

/* a token quoted by the shared rules (README.md, Messages) */
char*
qtok(char *t)
{
	char *s, *p, *q;

	if(*t != 0 && strpbrk(t, " \t'\n") == nil)
		return estrdup9p(t);
	s = emalloc9p(2*strlen(t)+3);
	q = s;
	*q++ = '\'';
	for(p = t; *p; p++){
		if(*p == '\'')
			*q++ = '\'';
		*q++ = *p;
	}
	*q++ = '\'';
	*q = 0;
	return s;
}

static void
usage(void)
{
	fprint(2, "usage: uxd [-D] [-s srvname] [-m mtpt] [-g grantfile]\n");
	threadexitsall("usage");
}

void
threadmain(int argc, char **argv)
{
	char *srvname, *mtpt, *home, *admin;
	int fd;

	srvname = nil;
	mtpt = nil;
	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 'g':
		grantfile = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	rfork(RFNOTEG);
	quotefmtinstall();
	fmtinstall('F', fcallfmt);
	user = getuser();
	starttime = time(0);
	if(srvname == nil)
		srvname = smprint("uxd.%s", user);
	admin = smprint("%s.admin", srvname);
	if(grantfile == nil && (home = getenv("home")) != nil)
		grantfile = smprint("%s/lib/ux/grants", home);
	loadgrants();

	reqchan = chancreate(sizeof(void*), 64);
	fidchan = chancreate(sizeof(void*), 64);
	rxchan = chancreate(sizeof(void*), 64);
	proccreate(workproc, nil, Stack);

	initsrv(&pubsrv);
	initsrv(&adminsrv);
	if(post(&pubsrv, srvname) < 0)
		sysfatal("post /srv/%s: %r", srvname);
	if((fd = post(&adminsrv, admin)) < 0)
		sysfatal("post /srv/%s: %r", admin);
	if(mtpt != nil && mount(fd, -1, mtpt, MREPL, "") < 0)
		sysfatal("mount %s: %r", mtpt);
	threadexits(nil);
}
