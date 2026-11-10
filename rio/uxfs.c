/*
 * 9/UX: serving the /dev/ux files.  See docs/protocols/devux.md.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include "dat.h"
#include "fns.h"
#include "ux.h"

static Dirtab uxdir[] =
{
	{ ".",		QTDIR,	Qux,			0500|DMDIR },
	{ "version",	QTFILE,	Quxversion,		0400 },
	{ "ctl",		QTFILE,	Quxctl,		0600 },
	{ "events",	QTFILE,	Quxevents,		0400 },
	{ "windows",	QTFILE,	Quxwindows,	0400 },
	{ "desktop",	QTDIR,	Quxdesk,		0500|DMDIR },
	{ "menubar",	QTDIR,	Quxmenu,		0500|DMDIR },
	{ "drag",		QTDIR,	Quxdrag,		0500|DMDIR },
	{ nil, }
};

static Dirtab uxdeskdir[] =
{
	{ ".",		QTDIR,	Quxdesk,		0500|DMDIR },
	{ "ctl",		QTFILE,	Quxdeskctl,	0600 },
	{ "state",	QTFILE,	Quxdeskstate,	0400 },
	{ "events",	QTFILE,	Quxdeskevents,	0400 },
	{ nil, }
};

static Dirtab uxmenudir[] =
{
	{ ".",		QTDIR,	Quxmenu,		0500|DMDIR },
	{ "ctl",		QTFILE,	Quxmenuctl,	0600 },
	{ "menus",	QTFILE,	Quxmenumenus,	0400 },
	{ "events",	QTFILE,	Quxmenuevents,	0400 },
	{ nil, }
};

static Dirtab uxdragdir[] =
{
	{ ".",		QTDIR,	Quxdrag,		0500|DMDIR },
	{ "ctl",		QTFILE,	Quxdragctl,	0600 },
	{ "state",	QTFILE,	Quxdragstate,	0400 },
	{ "events",	QTFILE,	Quxdragevents,	0400 },
	{ "data",		QTFILE,	Quxdragdata,	0600 },
	{ nil, }
};

static char version[] =
	"devux 1.1\n"
	"feature focus\n"
	"feature desktop\n"
	"feature menubar\n"
	"feature drag\n";

static char draghelp[] =
	"start -\n"
	"accept -\n"
	"refuse -\n"
	"done -\n"
	"cancel -\n";

static char ctlhelp[] =
	"mode -\n"
	"cmdkey -\n";

static char deskhelp[] =
	"claim -\n"
	"release -\n";

static char menuhelp[] =
	"app -\n"
	"title -\n"
	"window -\n"
	"unwindow -\n"
	"menu -\n"
	"item -\n"
	"set -\n"
	"del -\n"
	"clear -\n"
	"begin -\n"
	"commit -\n"
	"abort -\n"
	"invoke -\n";

/* ux.c */
Uxsub*	uxsubscribe(int);
void		uxunsubscribe(Uxsub*);
void		uxsubfree(Uxsub*);
char*	uxwindows(int*);
char*	uxctlread(int*);
char*	uxctlwrite(char*);
char*	uxdeskctl(Uxfid*, Window*, char*);
void		uxdeskclose(Uxfid*);
char*	uxdeskstate(int*);

int
uxisqid(int q)
{
	return q >= Qux && q <= Quxdragdata;
}

Dirtab*
uxdirtab(int q)
{
	switch(q){
	case Qux:	return uxdir;
	case Quxdesk:	return uxdeskdir;
	case Quxmenu:	return uxmenudir;
	case Quxdrag:	return uxdragdir;
	}
	return nil;
}

/* walk one name from a /dev/ux directory */
int
uxwalk1(char *name, Qid *qid, Dirtab **dir)
{
	Dirtab *d;
	int q, id;

	q = FILE(*qid);
	id = WIN(*qid);
	if(strcmp(name, "..") == 0){
		if(q == Qux){
			qid->path = QID(id, Qdir);
			qid->type = QTDIR;
			*dir = nil;	/* the caller resets it to the root */
			return 1;
		}
		qid->path = QID(id, Qux);
		qid->type = QTDIR;
		*dir = uxdir;
		return 0;
	}
	d = uxdirtab(q);
	if(d == nil)
		return -1;
	for(d++; d->name != nil; d++)
		if(strcmp(d->name, name) == 0){
			qid->path = QID(id, d->qid);
			qid->type = d->type;
			qid->vers = 0;
			*dir = d;
			return 0;
		}
	return -1;
}

static Uxfid*
uxfid(Xfid *x)
{
	if(x->f->aux == nil)
		x->f->aux = emalloc(sizeof(Uxfid));
	return x->f->aux;
}

static void
snap(Uxfid *u, char *s, int n)
{
	free(u->snap);
	u->snap = s;
	u->nsnap = n;
}

void
uxopen(Xfid *x)
{
	Fcall t;
	Uxfid *u;
	char *s;
	int n;

	u = uxfid(x);
	switch(FILE(x->f->qid)){
	case Quxversion:
		snap(u, estrdup(version), strlen(version));
		break;
	case Quxctl:
		s = uxctlread(&n);
		snap(u, s, n);
		break;
	case Quxwindows:
		s = uxwindows(&n);
		snap(u, s, n);
		break;
	case Quxdeskstate:
		s = uxdeskstate(&n);
		snap(u, s, n);
		break;
	case Quxdeskctl:
		snap(u, estrdup(deskhelp), strlen(deskhelp));
		break;
	case Quxmenuctl:
		snap(u, estrdup(menuhelp), strlen(menuhelp));
		break;
	case Quxmenumenus:
		s = uxmenusnap(&n);
		snap(u, s, n);
		break;
	case Quxevents:
		u->sub = uxsubscribe(Sux);
		break;
	case Quxdeskevents:
		u->sub = uxsubscribe(Sdesk);
		break;
	case Quxmenuevents:
		u->sub = uxsubscribe(Smenu);
		break;
	case Quxdragctl:
		snap(u, estrdup(draghelp), strlen(draghelp));
		break;
	case Quxdragstate:
		s = uxdragstate(&n);
		snap(u, s, n);
		break;
	case Quxdragevents:
		u->sub = uxsubscribe(Sdrag);
		break;
	case Quxdragdata:
		u->data = uxdataopen();
		break;
	}
	t.qid = x->f->qid;
	t.iounit = messagesize-IOHDRSZ;
	x->f->open = TRUE;
	x->f->mode = x->mode;
	filsysrespond(x->fs, x, &t, nil);
}

static void
readsub(Xfid *x, Uxsub *s)
{
	Fcall t;
	char *buf;
	int n, k, l;
	enum { Awake, Aflush, Aend };
	Alt alts[Aend+1];

	if(s->dead){
		t.count = 0;
		t.data = nil;
		filsysrespond(x->fs, x, &t, nil);
		return;
	}
	if(s->ref > 1){
		/* one read at a time on an events fid */
		filsysrespond(x->fs, x, &t, "busy");
		return;
	}
	s->ref++;
	while(s->nq == 0 && !s->dead){
		alts[Awake].c = s->wake;
		alts[Awake].v = nil;
		alts[Awake].op = CHANRCV;
		alts[Aflush].c = x->flushc;
		alts[Aflush].v = nil;
		alts[Aflush].op = CHANRCV;
		alts[Aend].op = CHANEND;
		if(alt(alts) == Aflush){
			uxsubfree(s);
			filsyscancel(x);
			return;
		}
	}
	if(s->nq == 0){
		uxsubfree(s);
		t.count = 0;
		t.data = nil;
		filsysrespond(x->fs, x, &t, nil);
		return;
	}
	l = strlen(s->q[0]);
	if(l > x->count){
		uxsubfree(s);
		filsysrespond(x->fs, x, &t, "short read");
		return;
	}
	buf = emalloc(x->count+1);
	n = 0;
	for(k=0; k<s->nq; k++){
		l = strlen(s->q[k]);
		if(n+l > x->count)
			break;
		memmove(buf+n, s->q[k], l);
		n += l;
		free(s->q[k]);
	}
	s->nq -= k;
	memmove(s->q, s->q+k, s->nq*sizeof(char*));
	uxsubfree(s);
	t.data = buf;
	t.count = n;
	filsysrespond(x->fs, x, &t, nil);
	free(buf);
}

void
uxread(Xfid *x)
{
	Fcall t;
	Uxfid *u;
	char *s;
	int n, off, cnt;

	u = uxfid(x);
	if(u->sub != nil){
		readsub(x, u->sub);
		return;
	}
	if(u->data != nil){
		uxdataread(x, u->data);
		return;
	}
	s = u->snap;
	n = u->nsnap;
	off = x->offset;
	cnt = x->count;
	if(s == nil)
		n = 0;
	if(off > n)
		off = n;
	if(off+cnt > n)
		cnt = n-off;
	t.data = s != nil ? s+off : nil;
	t.count = cnt;
	filsysrespond(x->fs, x, &t, nil);
}

void
uxwrite(Xfid *x)
{
	Fcall t;
	Uxfid *u;
	char *p, *e, *err, *buf;
	Window *own;
	int q;

	u = uxfid(x);
	q = FILE(x->f->qid);
	own = x->f->w;
	if(q == Quxdragdata){
		err = uxdatawrite(u->data, (char*)x->data, x->count);
		uxchanged();
		if(err != nil){
			filsysrespond(x->fs, x, &t, err);
			return;
		}
		t.count = x->count;
		filsysrespond(x->fs, x, &t, nil);
		return;
	}
	buf = emalloc(x->count+1);
	memmove(buf, x->data, x->count);
	buf[x->count] = 0;
	err = nil;
	for(p=buf; *p && err==nil; p=e){
		e = strchr(p, '\n');
		if(e != nil)
			*e++ = 0;
		else
			e = p+strlen(p);
		switch(q){
		case Quxctl:
			err = uxctlwrite(p);
			break;
		case Quxdeskctl:
			err = uxdeskctl(u, own, p);
			break;
		case Quxmenuctl:
			err = uxmenuctl(u, own, p);
			break;
		case Quxdragctl:
			err = uxdragctl(u, own, p);
			break;
		default:
			err = "permission denied";
			break;
		}
	}
	free(buf);
	uxchanged();
	if(err != nil){
		filsysrespond(x->fs, x, &t, err);
		return;
	}
	t.count = x->count;
	filsysrespond(x->fs, x, &t, nil);
}

/* called by xfidclose; it responds */
void
uxclose(Xfid *x)
{
	Uxfid *u;

	u = x->f->aux;
	if(u == nil)
		return;
	x->f->aux = nil;
	if(u->sub != nil)
		uxunsubscribe(u->sub);
	uxmenuclose(u);
	uxdeskclose(u);
	uxdragclose(u);
	if(u->data != nil)
		uxdataclose(u->data);
	free(u->snap);
	free(u);
	uxchanged();
}
