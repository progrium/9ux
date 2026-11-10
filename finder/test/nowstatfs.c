/*
 * nowstatfs: an in-memory file server that can create, read, write and
 * remove, but has no wstat at all, so it can't rename or change modes.
 * Finder's tests use it as the server that can't rename, and kill it
 * to see a volume go away.
 *
 *	nowstatfs [-s srvname] [-m mtpt]
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

typedef struct Data Data;
struct Data
{
	uchar	*p;
	vlong	n;
};

static void
fscreate(Req *r)
{
	File *f;

	f = createfile(r->fid->file, r->ifcall.name, r->fid->uid, r->ifcall.perm, nil);
	if(f == nil){
		responderror(r);
		return;
	}
	f->aux = mallocz(sizeof(Data), 1);
	r->fid->file = f;
	r->ofcall.qid = f->qid;
	respond(r, nil);
}

static void
fsread(Req *r)
{
	Data *d;

	d = r->fid->file->aux;
	if(d == nil){
		r->ofcall.count = 0;
		respond(r, nil);
		return;
	}
	readbuf(r, d->p, d->n);
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	Data *d;
	vlong end;

	d = r->fid->file->aux;
	end = r->ifcall.offset + r->ifcall.count;
	if(end > d->n){
		d->p = realloc(d->p, end);
		memset(d->p + d->n, 0, end - d->n);
		d->n = end;
	}
	memmove(d->p + r->ifcall.offset, r->ifcall.data, r->ifcall.count);
	r->fid->file->length = d->n;
	r->ofcall.count = r->ifcall.count;
	respond(r, nil);
}

static void
fsopen(Req *r)
{
	Data *d;

	if((r->ifcall.mode & OTRUNC) && (d = r->fid->file->aux) != nil){
		d->n = 0;
		r->fid->file->length = 0;
	}
	respond(r, nil);
}

static void
destroy(File *f)
{
	Data *d;

	d = f->aux;
	if(d != nil){
		free(d->p);
		free(d);
	}
}

Srv fs = {
	.open=	fsopen,
	.create=	fscreate,
	.read=	fsread,
	.write=	fswrite,
};

static void
usage(void)
{
	fprint(2, "usage: nowstatfs [-s srvname] [-m mtpt]\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *srvname, *mtpt;

	srvname = nil;
	mtpt = nil;
	ARGBEGIN{
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 'D':
		chatty9p++;
		break;
	default:
		usage();
	}ARGEND
	if(srvname == nil && mtpt == nil)
		usage();
	fs.tree = alloctree(nil, nil, DMDIR|0777, destroy);
	postmountsrv(&fs, srvname, mtpt, MREPL|MCREATE);
	exits(nil);
}
