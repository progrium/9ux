/*
 * The app's 9P tree (docs/protocols/ux.md, The app tree).
 * Runs in lib9p's proc; takes uxlk only briefly and never waits on
 * the UI thread.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <mouse.h>
#include <keyboard.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "ux.h"
#include "impl.h"

/* file numbers within an object; qid.path = oid<<8 | file */
enum {
	Fdir,
	/* app */
	Aversion, Aname, Aid, Atitle, Actl, Aevents, Amenus, Awindows,
	/* window */
	Wkind = 1, Wtitle, Wrect, Wstate, Wctl, Wevents, Wcontrols,
	/* menu */
	Mtitle = 1, Mitems, Mctl, Mevents,
	/* control: common, then type files */
	Ctype = 1, Clabel, Cstate, Crect, Cparent, Cctl, Cevents,
	Cvalue, Cselection, Citems, Corigin, Cextent, Cdirf,
	Nfile,
};

static char *appfiles[] = { ".", "version", "name", "id", "title", "ctl", "events", "menus", "windows" };
static char *winfiles[] = { ".", "kind", "title", "rect", "state", "ctl", "events", "controls" };
static char *menufiles[] = { ".", "title", "items", "ctl", "events" };
static char *ctlfiles[] = { ".", "type", "label", "state", "rect", "parent", "ctl", "events",
	"value", "selection", "items", "origin", "extent", "dir" };

typedef struct Faux Faux;
struct Faux {
	ulong	oid;
	int	file;
	char	*snap;
	Sub	*sub;
	char	*wbuf;	/* value being written */
	long	wlen;
	int	writing;
};

#define QPATH(oid, f)	((uvlong)(oid)<<8 | (f))
#define QOID(p)	((ulong)((p)>>8))
#define QFILE(p)	((int)((p)&0xFF))

static int
kindof(ulong oid)
{
	Obj *o;

	if(oid == 0)
		return Oapp;
	o = _lookobj(oid);
	return o == nil ? -1 : o->kind;
}

static int
isdir(ulong oid, int f)
{
	if(f == Fdir)
		return 1;
	switch(kindof(oid)){
	case Oapp:
		return f == Amenus || f == Awindows;
	case Owin:
		return f == Wcontrols;
	}
	return 0;
}

static char*
fname(ulong oid, int f)
{
	static char buf[32];
	Obj *o;

	if(f == Fdir){
		o = _lookobj(oid);
		if(o == nil)
			return "/";
		switch(o->kind){
		case Owin:
			snprint(buf, sizeof buf, "%d", ((Uxwin*)o)->winid);
			return buf;
		case Octl:
			return ((Uxctl*)o)->id;
		case Omenu:
			return ((Uxmenu*)o)->id;
		}
		return "?";
	}
	switch(kindof(oid)){
	case Oapp:	return appfiles[f];
	case Owin:	return winfiles[f];
	case Omenu:	return menufiles[f];
	case Octl:	return ctlfiles[f];
	}
	return "?";
}

/* writable files */
static int
fmode(ulong oid, int f)
{
	Obj *o;

	if(isdir(oid, f))
		return DMDIR|0555;
	switch(kindof(oid)){
	case Oapp:
		return f == Actl ? 0644 : 0444;
	case Owin:
		return f == Wctl ? 0644 : 0444;
	case Omenu:
		return f == Mctl ? 0644 : 0444;
	case Octl:
		if(f == Cctl)
			return 0644;
		o = _lookobj(oid);
		if(f == Cvalue && ((Uxctl*)o)->t->writevalue != nil)
			return 0644;
		return 0444;
	}
	return 0444;
}

static void
mkqid(Qid *q, ulong oid, int f)
{
	q->path = QPATH(oid, f);
	q->vers = 0;
	q->type = isdir(oid, f) ? QTDIR : QTFILE;
}

static void
mkdir(Dir *d, ulong oid, int f)
{
	memset(d, 0, sizeof *d);
	mkqid(&d->qid, oid, f);
	d->mode = fmode(oid, f);
	d->name = estrdup9p(fname(oid, f));
	d->uid = estrdup9p(ux.user);
	d->gid = estrdup9p(ux.user);
	d->muid = estrdup9p(ux.user);
	d->atime = d->mtime = time(0);
}

static int
ctlhas(Uxctl *c, int f)
{
	if(f < Cvalue)
		return 1;
	return _hasfile(c, ctlfiles[f]);
}

/* the i'th entry of directory (oid, f): fills oid/file; 0 at the end */
static int
dirent(ulong oid, int f, int i, ulong *coid, int *cf)
{
	Obj *o;
	Uxwin *w;
	Uxctl *c;
	int j;

	switch(kindof(oid)){
	case Oapp:
		if(f == Fdir){
			if(i+1 >= nelem(appfiles))
				return 0;
			*coid = 0;
			*cf = i+1;
			return 1;
		}
		if(f == Amenus){
			if(i >= ux.nmenu)
				return 0;
			*coid = ux.menu[i]->oid;
			*cf = Fdir;
			return 1;
		}
		if(f == Awindows){
			if(i >= ux.nwin)
				return 0;
			*coid = ux.win[i]->oid;
			*cf = Fdir;
			return 1;
		}
		return 0;
	case Owin:
		w = (Uxwin*)_lookobj(oid);
		if(f == Fdir){
			if(i+1 >= nelem(winfiles))
				return 0;
			*coid = oid;
			*cf = i+1;
			return 1;
		}
		if(i >= w->nctl)
			return 0;
		*coid = w->ctl[i]->oid;
		*cf = Fdir;
		return 1;
	case Omenu:
		if(i+1 >= nelem(menufiles))
			return 0;
		*coid = oid;
		*cf = i+1;
		return 1;
	case Octl:
		o = _lookobj(oid);
		c = (Uxctl*)o;
		for(j = 1; j < nelem(ctlfiles); j++)
			if(ctlhas(c, j) && i-- == 0){
				*coid = oid;
				*cf = j;
				return 1;
			}
		return 0;
	}
	return 0;
}

static int
dirgen(int i, Dir *d, void *aux)
{
	Faux *fa;
	ulong coid;
	int cf, ok;

	fa = aux;
	qlock(&uxlk);
	ok = 0;
	if(kindof(fa->oid) >= 0 && dirent(fa->oid, fa->file, i, &coid, &cf)){
		mkdir(d, coid, cf);
		ok = 1;
	}
	qunlock(&uxlk);
	return ok ? 0 : -1;
}

static void
fsattach(Req *r)
{
	Faux *fa;

	fa = emalloc9p(sizeof *fa);
	r->fid->aux = fa;
	mkqid(&r->fid->qid, 0, Fdir);
	r->ofcall.qid = r->fid->qid;
	respond(r, nil);
}

static char*
fswalk1(Fid *fid, char *name, Qid *qid)
{
	Faux *fa;
	ulong oid;
	int f, i;
	Uxwin *w;
	Uxctl *c;
	Uxmenu *m;
	Obj *o;
	char *err, *p;

	fa = fid->aux;
	oid = fa->oid;
	f = fa->file;
	err = nil;
	qlock(&uxlk);
	switch(kindof(oid)){
	case -1:
		err = "gone";
		break;
	case Oapp:
		if(f == Fdir){
			if(strcmp(name, "..") == 0)
				break;
			for(i = 1; i < nelem(appfiles); i++)
				if(strcmp(name, appfiles[i]) == 0){
					f = i;
					goto Found;
				}
		}else if(f == Amenus || f == Awindows){
			if(strcmp(name, "..") == 0){
				f = Fdir;
				break;
			}
			if(f == Amenus){
				if((m = _findmenu(name)) != nil){
					oid = m->oid;
					f = Fdir;
					goto Found;
				}
			}else{
				i = strtol(name, &p, 10);
				if(*p == 0 && (w = _findwin(i)) != nil){
					oid = w->oid;
					f = Fdir;
					goto Found;
				}
			}
		}
		err = "file does not exist";
		break;
	case Owin:
		w = (Uxwin*)_lookobj(oid);
		if(f == Fdir){
			if(strcmp(name, "..") == 0){
				oid = 0;
				f = Awindows;
				break;
			}
			for(i = 1; i < nelem(winfiles); i++)
				if(strcmp(name, winfiles[i]) == 0){
					f = i;
					goto Found;
				}
		}else{
			if(strcmp(name, "..") == 0){
				f = Fdir;
				break;
			}
			if((c = _findctl(w, name)) != nil){
				oid = c->oid;
				f = Fdir;
				goto Found;
			}
		}
		err = "file does not exist";
		break;
	case Omenu:
		if(strcmp(name, "..") == 0){
			oid = 0;
			f = Amenus;
			break;
		}
		for(i = 1; i < nelem(menufiles); i++)
			if(strcmp(name, menufiles[i]) == 0){
				f = i;
				goto Found;
			}
		err = "file does not exist";
		break;
	case Octl:
		o = _lookobj(oid);
		c = (Uxctl*)o;
		if(strcmp(name, "..") == 0){
			oid = c->w->oid;
			f = Wcontrols;
			break;
		}
		for(i = 1; i < nelem(ctlfiles); i++)
			if(strcmp(name, ctlfiles[i]) == 0 && ctlhas(c, i)){
				f = i;
				goto Found;
			}
		err = "file does not exist";
		break;
	}
Found:
	if(err == nil){
		fa->oid = oid;
		fa->file = f;
		mkqid(qid, oid, f);
		fid->qid = *qid;
	}
	qunlock(&uxlk);
	return err;
}

static char*
fsclone(Fid *old, Fid *new)
{
	Faux *fa;

	fa = emalloc9p(sizeof *fa);
	fa->oid = ((Faux*)old->aux)->oid;
	fa->file = ((Faux*)old->aux)->file;
	new->aux = fa;
	return nil;
}

static char*
version(void)
{
	return estrdup("ux 1.0\nfeature menus\nfeature value-write\n");
}

static char*
valline(char *s)
{
	char *p;

	p = smprint("%s\n", s ? s : "");
	return p;
}

/* a snapshot of a state file; nil with *err set on error */
static char*
snapshot(ulong oid, int f, char **err)
{
	Obj *o;
	Uxwin *w;
	Uxmenu *m;
	char *s, *t;

	*err = nil;
	switch(kindof(oid)){
	case Oapp:
		switch(f){
		case Aversion:	return version();
		case Aname:	return valline(ux.name);
		case Aid:	return valline(ux.id);
		case Atitle:	return valline(ux.title);
		case Actl:	return _ctlhelp(nil);
		}
		break;
	case Owin:
		w = (Uxwin*)_lookobj(oid);
		switch(f){
		case Wkind:	return valline(w->kind);
		case Wtitle:	return valline(w->title);
		case Wrect:
			return smprint("%d %d %d %d\n", w->r.min.x, w->r.min.y, w->r.max.x, w->r.max.y);
		case Wstate:
			t = _flags(w->state, 1);
			s = valline(t);
			free(t);
			return s;
		case Wctl:	return _ctlhelp(w);
		}
		break;
	case Omenu:
		m = (Uxmenu*)_lookobj(oid);
		switch(f){
		case Mtitle:	return valline(m->title);
		case Mitems:	return _menuitems(m);
		case Mctl:	return _ctlhelp(m);
		}
		break;
	case Octl:
		o = _lookobj(oid);
		s = _ctlread((Uxctl*)o, ctlfiles[f]);
		if(s == nil)
			*err = "permission denied";
		return s;
	}
	*err = "file does not exist";
	return nil;
}

static void
fsopen(Req *r)
{
	Faux *fa;
	char *err;
	int mode, wr;

	fa = r->fid->aux;
	mode = r->ifcall.mode;
	wr = (mode&3) == OWRITE || (mode&3) == ORDWR || (mode&OTRUNC);
	qlock(&uxlk);
	if(kindof(fa->oid) < 0){
		qunlock(&uxlk);
		respond(r, "gone");
		return;
	}
	if(wr && (fmode(fa->oid, fa->file) & 0200) == 0){
		qunlock(&uxlk);
		respond(r, "permission denied");
		return;
	}
	err = nil;
	if(isdir(fa->oid, fa->file)){
		/* nothing to snapshot */
	}else if(fa->file == Aevents && kindof(fa->oid) == Oapp)
		fa->sub = _subscribe(nil);
	else if((fa->file == Wevents && kindof(fa->oid) == Owin)
	|| (fa->file == Mevents && kindof(fa->oid) == Omenu)
	|| (fa->file == Cevents && kindof(fa->oid) == Octl))
		fa->sub = _subscribe(_lookobj(fa->oid));
	else if(kindof(fa->oid) == Octl && fa->file == Cvalue && wr){
		/* uxd enforces rights; the app trusts its connection */
		fa->writing = 1;
		fa->wbuf = nil;
		fa->wlen = 0;
	}else
		fa->snap = snapshot(fa->oid, fa->file, &err);
	qunlock(&uxlk);
	respond(r, err);
}

static void
fsread(Req *r)
{
	Faux *fa;
	char *err;

	fa = r->fid->aux;
	if(r->fid->qid.type & QTDIR){
		dirread9p(r, dirgen, fa);
		respond(r, nil);
		return;
	}
	if(fa->sub){
		qlock(&uxlk);
		if(!_subread(fa->sub, r, &err)){
			qunlock(&uxlk);
			return;	/* the responder answers it */
		}
		qunlock(&uxlk);
		respond(r, err);
		return;
	}
	if(fa->snap == nil){
		respond(r, fa->writing ? "permission denied" : "gone");
		return;
	}
	readstr(r, fa->snap);
	respond(r, nil);
}

static void
queuecmd(Req *r, ulong oid, int f, char *buf)
{
	Cmd *c;

	c = emalloc(sizeof *c);
	c->r = r;
	c->oid = oid;
	c->file = f;
	c->buf = buf;
	if(ux.cmdtail == nil)
		ux.cmdtail = &ux.cmdq;
	*ux.cmdtail = c;
	ux.cmdtail = &c->next;
	nbsendul(ux.uikick, 1);
}

static void
fswrite(Req *r)
{
	Faux *fa;
	char *buf;
	long n;

	fa = r->fid->aux;
	n = r->ifcall.count;
	if(fa->writing){
		if(fa->wlen + n > Maxvalue){
			respond(r, "too large");
			return;
		}
		fa->wbuf = erealloc(fa->wbuf, fa->wlen+n+1);
		memmove(fa->wbuf+fa->wlen, r->ifcall.data, n);
		fa->wlen += n;
		fa->wbuf[fa->wlen] = 0;
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	if(n > 8192){
		respond(r, "too large");
		return;
	}
	buf = emalloc(n+1);
	memmove(buf, r->ifcall.data, n);
	buf[n] = 0;
	qlock(&uxlk);
	queuecmd(r, fa->oid, fa->file, buf);
	qunlock(&uxlk);
}

static void
fsstat(Req *r)
{
	Faux *fa;

	fa = r->fid->aux;
	qlock(&uxlk);
	if(kindof(fa->oid) < 0){
		qunlock(&uxlk);
		respond(r, "gone");
		return;
	}
	mkdir(&r->d, fa->oid, fa->file);
	qunlock(&uxlk);
	respond(r, nil);
}

static void
fsflush(Req *r)
{
	_subflush(r);
}

static void
fsdestroyfid(Fid *fid)
{
	Faux *fa;

	fa = fid->aux;
	if(fa == nil)
		return;
	qlock(&uxlk);
	if(fa->sub)
		_unsubscribe(fa->sub);
	if(fa->writing && fid->omode != -1){
		queuecmd(nil, fa->oid, fa->file, fa->wbuf ? fa->wbuf : estrdup(""));
		fa->wbuf = nil;
	}
	qunlock(&uxlk);
	free(fa->snap);
	free(fa->wbuf);
	free(fa);
}

static void
fsend(Srv *srv)
{
	_subhangup(srv);
}

static Srv uxsrv = {
	.attach = fsattach,
	.walk1 = fswalk1,
	.clone = fsclone,
	.open = fsopen,
	.read = fsread,
	.write = fswrite,
	.stat = fsstat,
	.flush = fsflush,
	.destroyfid = fsdestroyfid,
	.end = fsend,
};

static void
srvfree(Srv *s)
{
	close(s->infd);
	free(s);
}

static void
srvproc(void *v)
{
	threadsetname("uxsrv");
	threadsrv(v);
}

/*
 * Serve a new connection posted as /srv/name. Unlike postsrv, this
 * keeps one fd table for the whole app, so the responder proc can
 * answer requests on every connection. Returns the fd of the /srv
 * entry (closing it removes the entry), or -1.
 */
int
_newsrv(char *name)
{
	char path[160];
	int fd[2], cfd;
	Srv *s;

	if(pipe(fd) < 0)
		return -1;
	snprint(path, sizeof path, "/srv/%s", name);
	cfd = create(path, OWRITE|ORCLOSE|OCEXEC, 0600);
	if(cfd < 0 || fprint(cfd, "%d", fd[0]) < 0){
		if(cfd >= 0)
			close(cfd);
		close(fd[0]);
		close(fd[1]);
		return -1;
	}
	close(fd[0]);
	s = emalloc(sizeof *s);
	*s = uxsrv;
	s->infd = s->outfd = fd[1];
	s->forker = threadsrvforker;
	s->free = srvfree;
	proccreate(srvproc, s, 32*1024);
	return cfd;
}
