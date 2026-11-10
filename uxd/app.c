/*
 * App registration (ux.md, Registration with uxd) and the winid map.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "dat.h"
#include "fns.h"

App *apps;

typedef struct Name Name;
struct Name {
	Name	*next;
	char	*name;
	int	last;	/* highest N handed out for name-N */
};
static Name *names;
static int serial;

static Name*
lookname(char *name)
{
	Name *n;

	for(n = names; n != nil; n = n->next)
		if(strcmp(n->name, name) == 0)
			return n;
	n = emalloc9p(sizeof *n);
	n->name = estrdup9p(name);
	n->last = 0;
	n->next = names;
	names = n;
	return n;
}

/* [a-z][a-z0-9-]* */
int
isname(char *s)
{
	if(*s < 'a' || *s > 'z')
		return 0;
	for(; *s; s++)
		if(!(*s >= 'a' && *s <= 'z' || *s >= '0' && *s <= '9' || *s == '-'))
			return 0;
	return 1;
}

App*
lookapp(char *id)
{
	App *a;

	for(a = apps; a != nil; a = a->next)
		if(strcmp(a->id, id) == 0)
			return a;
	return nil;
}

App*
winowner(int win)
{
	App *a;
	int i;

	for(a = apps; a != nil; a = a->next)
		for(i = 0; i < a->nwin; i++)
			if(a->win[i] == win)
				return a;
	return nil;
}

void
appincref(App *a)
{
	a->ref++;
}

void
appdecref(App *a)
{
	Help *h;
	int i;

	if(a == nil || --a->ref > 0)
		return;
	while((h = a->help) != nil){
		a->help = h->next;
		for(i = 0; i < h->n; i++)
			free(h->verb[i]);
		free(h->verb);
		free(h->right);
		free(h->path);
		free(h);
	}
	free(a->id);
	free(a->name);
	free(a->title);
	free(a->win);
	free(a);
}

static void
addwin(App *a, int win)
{
	a->win = erealloc9p(a->win, (a->nwin+1)*sizeof a->win[0]);
	a->win[a->nwin++] = win;
	postevent("window win=%d app=%s", win, a->id);
}

static int
delwin(App *a, int win)
{
	int i;

	for(i = 0; i < a->nwin; i++)
		if(a->win[i] == win){
			a->nwin--;
			memmove(a->win+i, a->win+i+1, (a->nwin-i)*sizeof a->win[0]);
			postevent("window win=%d", win);
			return 0;
		}
	return -1;
}

/* end a registration: the app closed it, hung up, or uxd is done with it */
void
unregister(App *a)
{
	App **l;
	Reg *g;
	Req *r;

	if(a->gone)
		return;
	a->gone = 1;
	for(l = &apps; *l != nil; l = &(*l)->next)
		if(*l == a){
			*l = a->next;
			break;
		}
	while(a->nwin > 0)
		delwin(a, a->win[a->nwin-1]);
	postevent("unregister app=%s", a->id);
	if(a->c != nil){
		a->c->app = nil;
		closeconn(a->c);
		a->c = nil;
	}
	if((g = a->reg) != nil){
		a->reg = nil;
		g->ended = 1;
		while(g->nrq > 0){
			r = g->rq[--g->nrq];
			((Op*)r->aux)->cancel = nil;
			r->ofcall.count = 0;
			reply(r->aux, nil);
		}
		appdecref(a);	/* the registration's reference */
	}
	appdecref(a);	/* the apps list's */
}

void
apphungup(App *a)
{
	unregister(a);
}

/* the register fid was clunked */
void
regclosed(Reg *g)
{
	App *a;

	if((a = g->app) != nil){
		g->app = nil;
		if(!a->gone && a->reg == g)
			unregister(a);
		else if(!g->ended)
			appdecref(a);
	}
	free(g->rq);
	free(g);
}

static int
validsrv(char *s)
{
	char *p;

	if(strncmp(s, "/srv/ux.", 8) != 0)
		return 0;
	p = s+8;
	if(*p == 0 || strchr(p, '/') != nil)
		return 0;
	return 1;
}

/* the N of "name-N" if id is that, else 0 */
static int
idnum(char *id, char *name)
{
	int n;
	char *p;

	n = strlen(name);
	if(strncmp(id, name, n) != 0 || id[n] != '-' || id[n+1] == 0)
		return 0;
	for(p = id+n+1; *p; p++)
		if(*p < '0' || *p > '9')
			return 0;
	if(id[n+1] == '0')
		return 0;
	return atoi(id+n+1);
}

/* "register name=... srv=... [title=...] [id=...] [win=...]..." */
static char*
doregister(Reg *g, char **f, int nf, Op *op)
{
	char *name, *srv, *title, *id, *v, *e;
	int wins[64], nw, i, fd, want;
	Conn *c;
	Fcall tx;
	Msg *m;
	App *a, **l;
	Qid qid;
	Name *nm;

	name = srv = title = id = nil;
	nw = 0;
	for(i = 1; i < nf; i++){
		if((v = strchr(f[i], '=')) == nil)
			return "bad syntax";
		*v++ = 0;
		if(strcmp(f[i], "name") == 0)
			name = v;
		else if(strcmp(f[i], "srv") == 0)
			srv = v;
		else if(strcmp(f[i], "title") == 0)
			title = v;
		else if(strcmp(f[i], "id") == 0)
			id = v;
		else if(strcmp(f[i], "win") == 0){
			if(nw == nelem(wins))
				return "too large: too many windows";
			if((wins[nw++] = atoi(v)) <= 0)
				return "bad value: win";
		}
	}
	if(name == nil || srv == nil)
		return "bad syntax: name= and srv= are needed";
	if(!isname(name))
		return "bad value: name";
	if(!validsrv(srv))
		return "bad value: srv must be /srv/ux.*";
	want = 0;
	if(id != nil && (want = idnum(id, name)) == 0)
		return "bad value: id";

	if((fd = open(srv, ORDWR)) < 0)
		return "not found: srv";
	remove(srv);
	c = newconn(fd);

	memset(&tx, 0, sizeof tx);
	tx.type = Tversion;
	tx.msize = Msize;
	tx.version = "9P2000";
	if((m = rpc(c, &tx, op)) == nil)
		goto Err;
	if(strncmp(m->f.version, "9P2000", 6) != 0 || m->f.msize < 256){
		freemsg(m);
		strcpy(op->err, "version mismatch: 9P");
		goto Err;
	}
	freemsg(m);
	memset(&tx, 0, sizeof tx);
	tx.type = Tattach;
	tx.fid = 0;
	tx.afid = NOFID;
	tx.uname = user;
	tx.aname = "";
	if((m = rpc(c, &tx, op)) == nil)
		goto Err;
	qid = m->f.qid;
	freemsg(m);
	if((v = readapp(c, 0, "version", op)) == nil)
		goto Err;
	if(strncmp(v, "ux 1.", 5) != 0){
		free(v);
		strcpy(op->err, "version mismatch");
		goto Err;
	}
	free(v);

	/* things may have changed while we waited on the app */
	if(g->app != nil){
		strcpy(op->err, "bad value: already registered");
		goto Err;
	}
	for(i = 0; i < nw; i++)
		if(winowner(wins[i]) != nil){
			strcpy(op->err, "in use: window");
			goto Err;
		}

	nm = lookname(name);
	a = emalloc9p(sizeof *a);
	memset(a, 0, sizeof *a);
	if(want > 0 && lookapp(id) == nil){
		a->id = estrdup9p(id);
		if(want > nm->last)
			nm->last = want;
	}else{
		do
			a->id = smprint("%s-%d", name, ++nm->last);
		while(lookapp(a->id) != nil && (free(a->id), 1));
	}
	a->ref = 2;	/* the apps list, the registration */
	a->serial = ++serial;
	a->name = estrdup9p(name);
	a->title = estrdup9p(title ? title : "");
	a->c = c;
	a->rootfid = 0;
	a->rootqid = qid;
	a->reg = g;
	c->app = a;
	c->nextfid = 1;
	g->app = a;
	for(l = &apps; *l != nil; l = &(*l)->next)
		;
	*l = a;
	postevent("register app=%s name=%s", a->id, a->name);
	for(i = 0; i < nw; i++)
		addwin(a, wins[i]);
	return nil;

Err:
	closeconn(c);
	e = op->err;
	if(*e == 0)
		strcpy(e, "gone");
	return e;
}

/* a write to register */
char*
regwrite(Reg *g, char *data, Op *op)
{
	char *lines[64], *f[80], *e;
	int i, n, nf, w;
	App *a, *o;

	n = getfields(data, lines, nelem(lines), 1, "\n");
	for(i = 0; i < n; i++){
		nf = tokenize(lines[i], f, nelem(f));
		if(nf == 0)
			continue;
		if(strcmp(f[0], "register") == 0){
			if(g->app != nil || g->ended)
				return "bad value: already registered";
			if((e = doregister(g, f, nf, op)) != nil)
				return e;
			continue;
		}
		a = g->app;
		if(strcmp(f[0], "window") == 0 || strcmp(f[0], "unwindow") == 0){
			if(nf != 2)
				return "bad syntax";
			if(a == nil || a->gone)
				return "bad value: not registered";
			if((w = atoi(f[1])) <= 0)
				return "bad value: winid";
			if(f[0][0] == 'w'){
				if((o = winowner(w)) == a)
					continue;
				if(o != nil)
					return "in use";
				addwin(a, w);
			}else if(delwin(a, w) < 0)
				return "not found";
		}else if(strcmp(f[0], "title") == 0){
			if(nf != 2)
				return "bad syntax";
			if(a == nil || a->gone)
				return "bad value: not registered";
			free(a->title);
			a->title = estrdup9p(f[1]);
		}else
			return "unknown command";
	}
	return nil;
}

static void
cancelreg(Op *op)
{
	Reg *g;
	int i;

	g = op->waiton;
	for(i = 0; i < g->nrq; i++)
		if(g->rq[i] == op->r){
			g->nrq--;
			memmove(g->rq+i, g->rq+i+1, (g->nrq-i)*sizeof g->rq[0]);
			break;
		}
	op->cancel = nil;
	reply(op, "interrupted");
}

/* a read of register: the app ID, then end of file when it ends */
void
regread(Reg *g, Op *op)
{
	Req *r;
	char *s;

	r = op->r;
	if(g->app == nil && !g->ended){
		reply(op, "bad value: not registered");
		return;
	}
	if(!g->idsent && g->app != nil){
		g->idsent = 1;
		s = smprint("%s\n", g->app->id);
		if(strlen(s) > r->ifcall.count){
			free(s);
			reply(op, "short read");
			return;
		}
		r->ofcall.data = s;
		r->ofcall.count = strlen(s);
		op->freeme = s;
		reply(op, nil);
		return;
	}
	if(g->ended){
		r->ofcall.count = 0;
		reply(op, nil);
		return;
	}
	g->rq = erealloc9p(g->rq, (g->nrq+1)*sizeof g->rq[0]);
	g->rq[g->nrq++] = r;
	op->cancel = cancelreg;
	op->waiton = g;
}

/* the windows file */
char*
winlist(void)
{
	App *a;
	char *s, *t;
	int i;

	s = estrdup9p("");
	for(a = apps; a != nil; a = a->next)
		for(i = 0; i < a->nwin; i++){
			t = smprint("%swindow win=%d app=%s\n", s, a->win[i], a->id);
			free(s);
			s = t;
		}
	return s;
}
