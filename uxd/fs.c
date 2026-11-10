/*
 * The /mnt/ux tree: uxd's own files, and each app's tree passed
 * through with policy (ux.md, /mnt/ux and Rights).
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "dat.h"
#include "fns.h"

typedef struct Tab Tab;
struct Tab {
	char	*name;
	int	q;
	int	mode;
};

static Tab roottab[] = {
	"version",	Qversion,	0444,
	"rights",	Qrights,	0444,
	"register",	Qregister,	0666,
	"windows",	Qwindows,	0444,
	"events",	Qevents,	0444,
	"apps",		Qapps,		DMDIR|0555,
	"grants",	Qgrants,	DMDIR|0500,
};

static Tab granttab[] = {
	"new",		Qgnew,		0600,
	"list",		Qglist,		0400,
	"ctl",		Qgctl,		0600,
};

static char *qname[] = {
[Qroot]	"/",
[Qapps]	"apps",
[Qgrants]	"grants",
};

static Qid
localqid(int q)
{
	Qid qid;

	qid.path = q;
	qid.vers = 0;
	qid.type = (q == Qroot || q == Qapps || q == Qgrants) ? QTDIR : QTFILE;
	return qid;
}

/* an app's qid, made unique across apps */
Qid
passqid(App *a, Qid q)
{
	q.path = (1ULL<<63) | ((uvlong)(a->serial & 0x7fffff) << 40) | (q.path & ((1ULL<<40)-1));
	return q;
}

static void
fillstat(Dir *d, char *name, Qid qid, int mode)
{
	memset(d, 0, sizeof *d);
	d->name = estrdup9p(name);
	d->uid = estrdup9p(user);
	d->gid = estrdup9p(user);
	d->muid = estrdup9p(user);
	d->qid = qid;
	d->mode = mode | (qid.type&QTDIR ? DMDIR : 0);
	d->atime = d->mtime = starttime;
}

static int
localmode(int q)
{
	int i;

	for(i = 0; i < nelem(roottab); i++)
		if(roottab[i].q == q)
			return roottab[i].mode;
	for(i = 0; i < nelem(granttab); i++)
		if(granttab[i].q == q)
			return granttab[i].mode;
	return DMDIR|0555;
}

static char*
localname(int q)
{
	int i;

	for(i = 0; i < nelem(roottab); i++)
		if(roottab[i].q == q)
			return roottab[i].name;
	for(i = 0; i < nelem(granttab); i++)
		if(granttab[i].q == q)
			return granttab[i].name;
	return qname[q];
}

/*
 * Fx life cycle
 */
static Att*
attincref(Att *a)
{
	a->ref++;
	return a;
}

static void
attdecref(Att *a)
{
	if(a == nil || --a->ref > 0)
		return;
	free(a);
}

static Fx*
newfx(int q, Att *a)
{
	Fx *f;

	f = emalloc9p(sizeof *f);
	memset(f, 0, sizeof *f);
	f->q = q;
	f->att = attincref(a);
	f->afid = -1;
	f->qid = localqid(q);
	return f;
}

/* a copy of where f is, not opened */
static Fx*
clonefx(Fx *f)
{
	Fx *n;

	n = newfx(f->q, f->att);
	n->qid = f->qid;
	if(f->app != nil){
		n->app = f->app;
		appincref(n->app);
		n->path = estrdup9p(f->path);
		n->depth = f->depth;
	}
	return n;
}

void
freefx(Fx *f)
{
	if(f == nil)
		return;
	if(f->app != nil){
		if(f->afid >= 0 && !f->app->gone)
			tclunk(f->app->c, f->afid);
		appdecref(f->app);
	}
	if(f->sub != nil)
		freesub(f->sub);
	if(f->reg != nil)
		regclosed(f->reg);
	attdecref(f->att);
	free(f->path);
	free(f->pend);
	free(f->buf);
	free(f);
}

/*
 * attach
 */
void
fsattach(Op *op, int admin)
{
	Req *r;
	Att *a;
	Grant *g;
	Fx *f;

	r = op->r;
	a = emalloc9p(sizeof *a);
	memset(a, 0, sizeof *a);
	a->admin = admin;
	if(!admin && r->ifcall.aname != nil && r->ifcall.aname[0] != 0){
		if((g = lookgrant(r->ifcall.aname)) == nil){
			free(a);
			reply(op, "permission denied: unknown grant");
			return;
		}
		a->g = g;
	}
	f = newfx(Qroot, a);
	r->fid->aux = f;
	r->fid->qid = f->qid;
	r->ofcall.qid = f->qid;
	reply(op, nil);
}

/*
 * walk
 */
static char*
pathjoin(char *p, char *name)
{
	char *s, *q;

	if(strcmp(name, "..") == 0){
		s = estrdup9p(p);
		if((q = strrchr(s, '/')) != nil)
			*q = 0;
		else
			*s = 0;
		return s;
	}
	if(*p == 0)
		return estrdup9p(name);
	return smprint("%s/%s", p, name);
}

/* walk f one local step; 0 on success */
static int
localstep(Fx *f, char *name)
{
	App *a;
	int i;

	if(strcmp(name, "..") == 0){
		switch(f->q){
		case Qapps:
		case Qgrants:
			f->q = Qroot;
			break;
		case Qpass:	/* only at depth 0 */
			appdecref(f->app);
			f->app = nil;
			free(f->path);
			f->path = nil;
			f->q = Qapps;
			break;
		}
		f->qid = localqid(f->q);
		return 0;
	}
	switch(f->q){
	case Qroot:
		for(i = 0; i < nelem(roottab); i++)
			if(strcmp(roottab[i].name, name) == 0){
				if(roottab[i].q == Qgrants && !(attrights(f->att) & Radmin))
					return -1;
				f->q = roottab[i].q;
				f->qid = localqid(f->q);
				return 0;
			}
		return -1;
	case Qgrants:
		for(i = 0; i < nelem(granttab); i++)
			if(strcmp(granttab[i].name, name) == 0){
				f->q = granttab[i].q;
				f->qid = localqid(f->q);
				return 0;
			}
		return -1;
	case Qapps:
		if((a = lookapp(name)) == nil)
			return -1;
		appincref(a);
		f->q = Qpass;
		f->app = a;
		f->path = estrdup9p("");
		f->depth = 0;
		f->qid = passqid(a, a->rootqid);
		return 0;
	}
	return -1;
}

void
fswalk(Op *op)
{
	Req *r;
	Fx *src, *f;
	char **wname, *p;
	int i, j, n, nw, d, owned;
	long base, nf;
	App *a;
	Msg *m;

	r = op->r;
	src = r->fid->aux;
	f = clonefx(src);
	wname = r->ifcall.wname;
	nw = r->ifcall.nwname;
	n = 0;		/* qids so far */
	base = src->afid;	/* app fid for where f is; -1: the app's root */
	owned = 0;	/* base is ours to clunk */
	if(f->q == Qpass && (src->app->gone || src->app != f->app))
		base = -1;
	while(n < nw){
		if(f->q != Qpass || f->depth == 0 && strcmp(wname[n], "..") == 0){
			if(owned){
				tclunk(f->app->c, base);
				owned = 0;
			}
			if(localstep(f, wname[n]) < 0)
				break;
			base = -1;
			r->ofcall.wqid[n++] = f->qid;
			continue;
		}
		/* a run of names inside the app */
		a = f->app;
		if(a->gone){
			strcpy(op->err, "gone");
			break;
		}
		d = f->depth;
		for(j = n; j < nw; j++){
			if(strcmp(wname[j], "..") == 0){
				if(d == 0)
					break;
				d--;
			}else
				d++;
		}
		nf = newfid(a->c);
		m = twalk(a->c, base >= 0 ? base : a->rootfid, nf, j-n, wname+n, op);
		if(m == nil)
			break;
		for(i = 0; i < m->f.nwqid; i++){
			r->ofcall.wqid[n+i] = passqid(a, m->f.wqid[i]);
			p = pathjoin(f->path, wname[n+i]);
			free(f->path);
			f->path = p;
			f->depth += strcmp(wname[n+i], "..") == 0 ? -1 : 1;
			f->qid = r->ofcall.wqid[n+i];
		}
		if(m->f.nwqid < j-n){
			n += m->f.nwqid;
			freemsg(m);
			break;
		}
		n = j;
		freemsg(m);
		if(owned)
			tclunk(a->c, base);
		base = nf;
		owned = 1;
	}
	if(n < nw){
		if(owned)
			tclunk(f->app->c, base);
		freefx(f);
		r->ofcall.nwqid = n;
		if(n == 0 && op->err[0] == 0)
			strcpy(op->err, "file does not exist");
		reply(op, n == 0 ? op->err : nil);
		return;
	}
	if(f->q == Qpass && !owned){
		/* need a fid of our own on the app for this place */
		a = f->app;
		nf = newfid(a->c);
		if((m = twalk(a->c, base >= 0 ? base : a->rootfid, nf, 0, nil, op)) == nil){
			freefx(f);
			reply(op, op->err);
			return;
		}
		freemsg(m);
		base = nf;
	}
	if(f->q == Qpass)
		f->afid = base;
	r->ofcall.nwqid = n;
	if(r->newfid == r->fid)
		freefx(src);
	r->newfid->aux = f;
	reply(op, nil);
}

/*
 * Policy helpers for an app's files
 */
static char*
basename(char *path)
{
	char *p;

	if((p = strrchr(path, '/')) != nil)
		return p+1;
	return path;
}

/* if path is windows/W/controls/C/file, the control's dir; else nil */
static char*
ctldir(char *path)
{
	char *p, *f[8];
	int n;

	p = estrdup9p(path);
	n = getfields(p, f, nelem(f), 0, "/");
	if(n == 5 && strcmp(f[0], "windows") == 0 && strcmp(f[2], "controls") == 0){
		free(p);
		p = estrdup9p(path);
		*strrchr(p, '/') = 0;
		return p;
	}
	free(p);
	return nil;
}

static int
hasflag(char *s, char *flag)
{
	char *f[32];
	int i, n;

	n = tokenize(s, f, nelem(f));
	for(i = 0; i < n; i++)
		if(strcmp(f[i], flag) == 0)
			return 1;
	return 0;
}

/* is the control at dir (windows/W/controls/C) sensitive? -1 if unknown */
static int
sensitive(App *a, char *dir, Op *op)
{
	char *p, *s;
	int r;

	p = smprint("%s/state", dir);
	s = readapp(a->c, a->rootfid, p, op);
	free(p);
	if(s == nil)
		return -1;
	r = hasflag(s, "sensitive");
	free(s);
	return r;
}

static int
helpright(char *class)
{
	if(strcmp(class, "-") == 0)
		return -1;	/* counts as unlisted (ux.md, Rights) */
	return rightbit(class);
}

static Help*
loadhelp(App *a, char *path, Op *op)
{
	Help *h, **l;
	char *s, *lines[128], *f[4];
	int i, n, nf;

	if((s = readapp(a->c, a->rootfid, path, op)) == nil)
		return nil;
	for(l = &a->help; *l != nil; l = &(*l)->next)
		if(strcmp((*l)->path, path) == 0){
			h = *l;
			*l = h->next;
			for(i = 0; i < h->n; i++)
				free(h->verb[i]);
			free(h->verb);
			free(h->right);
			free(h->path);
			free(h);
			break;
		}
	h = emalloc9p(sizeof *h);
	memset(h, 0, sizeof *h);
	h->path = estrdup9p(path);
	n = getfields(s, lines, nelem(lines), 1, "\n");
	h->verb = emalloc9p((n+1)*sizeof h->verb[0]);
	h->right = emalloc9p((n+1)*sizeof h->right[0]);
	for(i = 0; i < n; i++){
		nf = tokenize(lines[i], f, nelem(f));
		if(nf < 2)
			continue;
		h->verb[h->n] = estrdup9p(f[0]);
		h->right[h->n] = helpright(f[1]);
		h->n++;
	}
	free(s);
	h->next = a->help;
	a->help = h;
	return h;
}

/* the right verb needs on the ctl file at path; -1: refused, -2: error */
static int
verbright(App *a, char *path, char *verb, Op *op)
{
	Help *h;
	int i, tries;

	for(tries = 0; tries < 2; tries++){
		for(h = a->help; h != nil; h = h->next)
			if(strcmp(h->path, path) == 0)
				break;
		if(h == nil || tries > 0)
			if((h = loadhelp(a, path, op)) == nil)
				return -2;
		for(i = 0; i < h->n; i++)
			if(strcmp(h->verb[i], verb) == 0)
				return h->right[i];
		/* not listed: maybe the control changed; read its help again */
	}
	return -1;
}

/* check every line of a ctl write; nil if all are allowed */
static char*
checkctl(Fx *f, char *data, long count, Op *op)
{
	char *s, *lines[256], *tok[2];
	int i, n, need, rights;

	rights = rightsfor(f->att, f->app);
	if(rights == Rall)
		return nil;	/* the session: the app decides */
	s = emalloc9p(count+1);
	memmove(s, data, count);
	s[count] = 0;
	n = getfields(s, lines, nelem(lines), 1, "\n");
	for(i = 0; i < n; i++){
		if(tokenize(lines[i], tok, nelem(tok)) < 1)
			continue;
		need = verbright(f->app, f->path, tok[0], op);
		if(need == -2){
			free(s);
			return op->err;
		}
		if(need < 0 || (rights & need) != need){
			free(s);
			return "permission denied";
		}
	}
	free(s);
	return nil;
}

/*
 * open
 */
static void
passopen(Op *op, Fx *f)
{
	Req *r;
	App *a;
	char *base, *dir;
	int mode, rd, wr, rights, s;
	Msg *m;

	r = op->r;
	a = f->app;
	if(a->gone){
		reply(op, "gone");
		return;
	}
	mode = r->ifcall.mode;
	rd = (mode&3) == OREAD || (mode&3) == ORDWR || (mode&3) == OEXEC;
	wr = (mode&3) == OWRITE || (mode&3) == ORDWR || (mode&OTRUNC);
	if(mode & ORCLOSE){
		reply(op, "permission denied");
		return;
	}
	rights = rightsfor(f->att, a);
	base = basename(f->path);
	if(wr && strcmp(base, "ctl") != 0){
		if(strcmp(base, "value") == 0){
			if(!(rights & Redit)){
				reply(op, "permission denied");
				return;
			}
		}else if(!(rights & Radmin)){
			reply(op, "permission denied");
			return;
		}
	}
	if(rd && !(rights & Rsensitive)
	&& (strcmp(base, "value") == 0 || strcmp(base, "items") == 0 || strcmp(base, "selection") == 0)
	&& (dir = ctldir(f->path)) != nil){
		s = sensitive(a, dir, op);
		free(dir);
		if(s != 0){
			reply(op, s < 0 ? op->err : "permission denied");
			return;
		}
	}
	if((m = topen(a->c, f->afid, mode, op)) == nil){
		reply(op, op->err);
		return;
	}
	f->qid = passqid(a, m->f.qid);
	f->isevents = strcmp(base, "events") == 0 && !(f->qid.type & QTDIR);
	r->ofcall.qid = f->qid;
	r->ofcall.iounit = m->f.iounit;
	if(r->ofcall.iounit == 0 || r->ofcall.iounit > Msize-IOHDRSZ)
		r->ofcall.iounit = Msize-IOHDRSZ;
	freemsg(m);
	f->open = 1;
	f->omode = mode;
	reply(op, nil);
}

void
fsopen(Op *op)
{
	Req *r;
	Fx *f;
	int wr;

	r = op->r;
	f = r->fid->aux;
	if(f->q == Qpass){
		passopen(op, f);
		return;
	}
	wr = (r->ifcall.mode&3) == OWRITE || (r->ifcall.mode&3) == ORDWR || (r->ifcall.mode&OTRUNC);
	if(wr && !(localmode(f->q) & 0222)){
		reply(op, "permission denied");
		return;
	}
	switch(f->q){
	case Qversion:
		f->buf = estrdup9p("ux 1.0\nfeature grants\n");
		break;
	case Qrights:
		f->buf = rightstr(attrights(f->att), " ");
		f->buf = erealloc9p(f->buf, strlen(f->buf)+2);
		strcat(f->buf, "\n");
		break;
	case Qwindows:
		f->buf = winlist();
		break;
	case Qglist:
		f->buf = grantlist();
		break;
	case Qevents:
		f->sub = newsub();
		break;
	case Qregister:
		f->reg = emalloc9p(sizeof *f->reg);
		memset(f->reg, 0, sizeof *f->reg);
		break;
	}
	if(f->buf != nil)
		f->nbuf = strlen(f->buf);
	f->open = 1;
	f->omode = r->ifcall.mode;
	r->ofcall.qid = f->qid;
	reply(op, nil);
}

/*
 * read
 */
static int
rootgen(int i, Dir *d, void *aux)
{
	Fx *f;

	f = aux;
	if(i == nelem(roottab)-1 && !(attrights(f->att) & Radmin))
		return -1;
	if(i >= nelem(roottab))
		return -1;
	fillstat(d, roottab[i].name, localqid(roottab[i].q), roottab[i].mode&0777);
	return 0;
}

static int
grantgen(int i, Dir *d, void*)
{
	if(i >= nelem(granttab))
		return -1;
	fillstat(d, granttab[i].name, localqid(granttab[i].q), granttab[i].mode&0777);
	return 0;
}

static int
appgen(int i, Dir *d, void*)
{
	App *a;

	for(a = apps; a != nil && i > 0; a = a->next)
		i--;
	if(a == nil)
		return -1;
	fillstat(d, a->id, passqid(a, a->rootqid), 0555);
	return 0;
}

/* rewrite the qids in directory entries read from an app */
static void
fixdir(App *a, uchar *p, long n)
{
	long m;
	Qid q;

	while(n >= BIT16SZ){
		m = GBIT16(p) + BIT16SZ;
		if(m > n || m < STATFIXLEN)
			break;
		/* size[2] type[2] dev[4] qid[13] */
		q.type = p[8];
		q.vers = GBIT32(p+9);
		q.path = GBIT64(p+13);
		q = passqid(a, q);
		PBIT64(p+13, q.path);
		p += m;
		n -= m;
	}
}

/* attributes that carry a sensitive control's contents */
static int
issecretattr(char *t)
{
	return strncmp(t, "value=", 6) == 0 || strncmp(t, "sel=", 4) == 0
		|| strncmp(t, "item=", 5) == 0 || strncmp(t, "items=", 6) == 0;
}

/* events lines with sensitive values removed (ux.md, Sensitive and secret controls) */
static char*
redact(Fx *f, char *line, Op *op)
{
	char *tok[64], *s, *t, *q, *ctl, *dir, *cp;
	int i, n, has, sens;

	cp = estrdup9p(line);
	n = tokenize(cp, tok, nelem(tok));
	ctl = nil;
	has = 0;
	for(i = 2; i < n; i++){
		if(strncmp(tok[i], "ctl=", 4) == 0)
			ctl = tok[i]+4;
		if(issecretattr(tok[i]))
			has = 1;
	}
	if(!has || ctl == nil){
		free(cp);
		return nil;
	}
	dir = estrdup9p(ctl);
	sens = sensitive(f->app, dir, op);
	free(dir);
	if(sens == 0){
		free(cp);
		return nil;
	}
	/* sensitive, or we can't tell: leave the value out */
	s = estrdup9p("");
	for(i = 0; i < n; i++){
		if(issecretattr(tok[i]) || strncmp(tok[i], "redacted=", 9) == 0)
			continue;
		q = qtok(tok[i]);
		t = smprint("%s%s%s", s, *s ? " " : "", q);
		free(q);
		free(s);
		s = t;
	}
	t = smprint("%s redacted=on\n", s);
	free(s);
	free(cp);
	return t;
}

/* move whole lines from f->pend into the reply */
static int
servepend(Op *op, Fx *f)
{
	Req *r;
	char *p, *e;
	long n;

	r = op->r;
	n = 0;
	p = f->pend;
	e = f->pend + f->npend;
	while(p < e){
		char *nl = memchr(p, '\n', e-p);
		long len = (nl ? nl+1 : e) - p;
		if(n+len > r->ifcall.count)
			break;
		n += len;
		p += len;
	}
	if(n == 0)
		return 0;
	op->freeme = emalloc9p(n);
	memmove(op->freeme, f->pend, n);
	f->npend -= n;
	memmove(f->pend, f->pend+n, f->npend);
	r->ofcall.data = op->freeme;
	r->ofcall.count = n;
	reply(op, nil);
	return 1;
}

static void
passread(Op *op, Fx *f)
{
	Req *r;
	App *a;
	Msg *m;
	char *data, *lines, *l, *nl, *red, *out;
	long n, len;

	r = op->r;
	a = f->app;
	if(f->isevents && f->npend > 0){
		if(servepend(op, f))
			return;
		reply(op, "short read");
		return;
	}
	if(a->gone){
		reply(op, "gone");
		return;
	}
	if((m = tread(a->c, f->afid, r->ifcall.offset, r->ifcall.count, op)) == nil){
		reply(op, op->err);
		return;
	}
	data = m->f.data;
	n = m->f.count;
	if(f->qid.type & QTDIR)
		fixdir(a, (uchar*)data, n);
	else if(f->isevents && n > 0 && !(rightsfor(f->att, a) & Rsensitive)){
		lines = emalloc9p(n+1);
		memmove(lines, data, n);
		lines[n] = 0;
		freemsg(m);

		out = nil;
		len = 0;
		for(l = lines; *l; l = nl){
			if((nl = strchr(l, '\n')) != nil)
				*nl++ = 0;
			else
				nl = l+strlen(l);
			red = redact(f, l, op);
			if(red == nil)
				red = smprint("%s\n", l);
			out = erealloc9p(out, len+strlen(red));
			memmove(out+len, red, strlen(red));
			len += strlen(red);
			free(red);
		}
		free(lines);
		f->pend = erealloc9p(f->pend, f->npend+len);
		memmove(f->pend+f->npend, out, len);
		f->npend += len;
		free(out);
		if(!servepend(op, f))
			reply(op, "short read");
		return;
	}
	op->msg = m;
	r->ofcall.data = data;
	r->ofcall.count = n;
	reply(op, nil);
}

void
fsread(Op *op)
{
	Req *r;
	Fx *f;

	r = op->r;
	f = r->fid->aux;
	switch(f->q){
	case Qpass:
		passread(op, f);
		return;
	case Qroot:
		dirread9p(r, rootgen, f);
		break;
	case Qgrants:
		dirread9p(r, grantgen, f);
		break;
	case Qapps:
		dirread9p(r, appgen, f);
		break;
	case Qevents:
		subread(f->sub, op);
		return;
	case Qregister:
		regread(f->reg, op);
		return;
	case Qgnew:
		if(f->newg == nil){
			reply(op, "bad value: write a grant command first");
			return;
		}
		/* a stream: the token once, whatever the offset (it follows a write) */
		if(f->buf == nil){
			f->buf = smprint("%s\n", f->newg->token);
			f->nbuf = strlen(f->buf);
			if(r->ifcall.count < f->nbuf){
				reply(op, "short read");
				return;
			}
			r->ofcall.data = f->buf;
			r->ofcall.count = f->nbuf;
		}else
			r->ofcall.count = 0;
		break;
	case Qgctl:
		readstr(r, "revoke admin\n");
		break;
	default:
		if(f->buf != nil)
			readbuf(r, f->buf, f->nbuf);
		break;
	}
	reply(op, nil);
}

/*
 * write
 */
static void
passwrite(Op *op, Fx *f)
{
	Req *r;
	App *a;
	char *base, *e;
	Msg *m;
	int rights;

	r = op->r;
	a = f->app;
	if(a->gone){
		reply(op, "gone");
		return;
	}
	rights = rightsfor(f->att, a);
	base = basename(f->path);
	if(strcmp(base, "ctl") == 0){
		if((e = checkctl(f, r->ifcall.data, r->ifcall.count, op)) != nil){
			reply(op, e);
			return;
		}
	}else if(strcmp(base, "value") == 0){
		if(!(rights & Redit)){
			reply(op, "permission denied");
			return;
		}
	}else if(!(rights & Radmin)){
		reply(op, "permission denied");
		return;
	}
	if((m = twrite(a->c, f->afid, r->ifcall.offset, r->ifcall.count, r->ifcall.data, op)) == nil){
		reply(op, op->err);
		return;
	}
	r->ofcall.count = m->f.count;
	freemsg(m);
	reply(op, nil);
}

static char*
copystr(Req *r)
{
	char *s;

	s = emalloc9p(r->ifcall.count+1);
	memmove(s, r->ifcall.data, r->ifcall.count);
	s[r->ifcall.count] = 0;
	return s;
}

void
fswrite(Op *op)
{
	Req *r;
	Fx *f;
	char *s, *e, *lines[64];
	int i, n;

	r = op->r;
	f = r->fid->aux;
	e = nil;
	switch(f->q){
	case Qpass:
		passwrite(op, f);
		return;
	case Qregister:
		s = copystr(r);
		e = regwrite(f->reg, s, op);
		free(s);
		break;
	case Qgnew:
		if(f->newg != nil){
			e = "bad value: one grant per open";
			break;
		}
		s = copystr(r);
		if((n = strlen(s)) > 0 && s[n-1] == '\n')
			s[n-1] = 0;
		e = newgrant(s, &f->newg);
		free(s);
		break;
	case Qgctl:
		s = copystr(r);
		n = getfields(s, lines, nelem(lines), 1, "\n");
		for(i = 0; i < n && e == nil; i++)
			e = grantctl(lines[i]);
		free(s);
		break;
	default:
		e = "permission denied";
		break;
	}
	if(e == nil)
		r->ofcall.count = r->ifcall.count;
	reply(op, e);
}

/*
 * stat
 */
void
fsstat(Op *op)
{
	Req *r;
	Fx *f;
	App *a;
	Msg *m;
	Dir d;
	uchar *buf;
	char *strs;

	r = op->r;
	f = r->fid->aux;
	if(f->q != Qpass){
		fillstat(&r->d, localname(f->q), f->qid, localmode(f->q)&0777);
		reply(op, nil);
		return;
	}
	a = f->app;
	if(a->gone){
		reply(op, "gone");
		return;
	}
	if((m = tstat(a->c, f->afid, op)) == nil){
		reply(op, op->err);
		return;
	}
	buf = m->f.stat;
	strs = emalloc9p(m->f.nstat);
	if(convM2D(buf, m->f.nstat, &d, strs) == 0){
		free(strs);
		freemsg(m);
		reply(op, "bad reply from app");
		return;
	}
	r->d = d;
	r->d.name = estrdup9p(f->depth == 0 ? a->id : d.name);
	r->d.uid = estrdup9p(d.uid);
	r->d.gid = estrdup9p(d.gid);
	r->d.muid = estrdup9p(d.muid);
	r->d.qid = passqid(a, d.qid);
	free(strs);
	freemsg(m);
	reply(op, nil);
}

/*
 * flush
 */
void
fsflush(Op *op)
{
	Req *old;
	Op *oop;

	old = op->r->oldreq;
	oop = old != nil ? old->aux : nil;
	if(oop != nil){
		oop->flushed = 1;
		if(oop->cancel != nil)
			oop->cancel(oop);
		else if(oop->rpc != nil)
			flushrpc(oop);
	}
	/* lib9p holds this answer until old is answered */
	reply(op, nil);
}
