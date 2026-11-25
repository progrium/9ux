/*
 * Events: subscriptions, the queue rules of core.md § Events (state
 * events coalesce, actions are never dropped), and blocking reads.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "tree.h"

enum {
	Maxq = 65536,
};

Sub	*subs;
int	seq;
int	holding;
static Pend	*held;
static Pend	**heldtail = &held;

char *statenames[] = {
	"change", "select", "focus", "blur", "resize", "move", "scroll",
	"hover", "state", "theme", nil,
};

static void
enqueue(Sub *s, char *line, char *name, char *win, int node, int state)
{
	Ev *e, **l, *prev;

	if(state){
		prev = nil;
		for(l = &s->head; *l != nil; l = &(*l)->next){
			e = *l;
			if(e->state && strcmp(e->name, name) == 0 && e->node == node
			&& ((e->win == nil && win == nil) || (e->win != nil && win != nil && strcmp(e->win, win) == 0))){
				*l = e->next;
				if(s->tail == e)
					s->tail = prev;
				free(e->line);
				free(e->name);
				free(e->win);
				free(e);
				s->nq--;
				break;
			}
			prev = e;
		}
	}else if(s->nq >= Maxq){
		s->lost++;
		return;
	}
	e = emalloc(sizeof *e);
	e->line = estrdup(line);
	e->name = estrdup(name);
	e->win = win != nil ? estrdup(win) : nil;
	e->node = node;
	e->state = state;
	e->next = nil;
	if(s->tail != nil)
		s->tail->next = e;
	else
		s->head = e;
	s->tail = e;
	s->nq++;
}

Sub*
newsub(int kind, char *win, int node)
{
	Sub *s;
	char *l;

	s = emalloc(sizeof *s);
	memset(s, 0, sizeof *s);
	s->kind = kind;
	s->win = win != nil ? estrdup(win) : nil;
	s->node = node;
	l = smprint("resync - - seq=%d\n", seq);
	enqueue(s, l, "resync", nil, 0, 0);
	free(l);
	s->next = subs;
	subs = s;
	return s;
}

void
freesub(Sub *s)
{
	Sub **l;
	Ev *e, *n;
	Rq *q, *nq;

	for(l = &subs; *l != nil; l = &(*l)->next)
		if(*l == s){
			*l = s->next;
			break;
		}
	for(e = s->head; e != nil; e = n){
		n = e->next;
		free(e->line);
		free(e->name);
		free(e->win);
		free(e);
	}
	for(q = s->rq; q != nil; q = nq){
		nq = q->next;
		respond(q->r, "gone");
		free(q);
	}
	free(s->cur);
	free(s->win);
	free(s);
}

int
anysub(void)
{
	Sub *s;

	for(s = subs; s != nil; s = s->next)
		if(!s->gone)
			return 1;
	return 0;
}

static int
reaches(Sub *s, char *win, int *anc, int nanc)
{
	int i;

	switch(s->kind){
	case Sapp:
		return 1;
	case Swin:
		return win != nil && strcmp(s->win, win) == 0;
	case Snode:
		if(win == nil || strcmp(s->win, win) != 0)
			return 0;
		for(i = 0; i < nanc; i++)
			if(anc[i] == s->node)
				return 1;
		return 0;
	}
	return 0;
}

/* split attrs at spaces outside '' quotes */
static int
attrsplit(char *s, char **f, int nf)
{
	int n, q;

	n = 0;
	while(*s != 0 && n < nf){
		while(*s == ' ')
			*s++ = 0;
		if(*s == 0)
			break;
		f[n++] = s;
		for(q = 0; *s != 0 && (q || *s != ' '); s++)
			if(*s == '\'')
				q = !q;
	}
	return n;
}

static int
attrcmp(void *a, void *b)
{
	char *x, *y;
	int lx, ly, r;

	x = *(char**)a;
	y = *(char**)b;
	lx = strcspn(x, "=");
	ly = strcspn(y, "=");
	r = strncmp(x, y, lx < ly ? lx : ly);
	if(r != 0)
		return r;
	return lx - ly;
}

/* an event's attrs and win=, sorted by name */
static char*
attrline(char *attrs, char *win)
{
	char *buf, *f[64], *w;
	int n, i;
	Fmt fm;

	buf = estrdup(attrs != nil ? attrs : "");
	n = attrsplit(buf, f, nelem(f)-1);
	w = nil;
	if(win != nil){
		w = smprint("win=%s", win);
		f[n++] = w;
	}
	qsort(f, n, sizeof f[0], attrcmp);
	fmtstrinit(&fm);
	for(i = 0; i < n; i++)
		fmtprint(&fm, " %s", f[i]);
	free(buf);
	free(w);
	return fmtstrflush(&fm);
}

/* libux's C API sees each event first, to call the app's handlers */
void	(*evhook)(char*, char*, char*, char*, char*);

static void
deliver(Pend *p)
{
	char *line, *a;
	Sub *s;

	if(evhook != nil)
		evhook(p->name, p->key, p->path, p->attrs, p->win);
	a = attrline(p->attrs, p->win);
	line = smprint("%s %s %s%s seq=%d\n", p->name, p->key, p->path, a, p->seq);
	free(a);
	for(s = subs; s != nil; s = s->next)
		if(!s->gone && reaches(s, p->win, p->anc, p->nanc))
			enqueue(s, line, p->name, p->win, p->nanc > 0 ? p->anc[0] : 0, p->state);
	free(line);
}

static void
freepend(Pend *p)
{
	free(p->name);
	free(p->key);
	free(p->path);
	free(p->attrs);
	free(p->win);
	free(p->anc);
	free(p);
}

/*
 * Send an event: name key path attrs [win=W] seq=N. anc is the node
 * and its ancestors, for node subscriptions. Returns its seq.
 */
int
emit(char *name, char *key, char *path, char *attrs, char *win, int *anc, int nanc, int state)
{
	Pend *p;

	p = emalloc(sizeof *p);
	memset(p, 0, sizeof *p);
	p->name = estrdup(name);
	p->key = estrdup(key != nil && key[0] ? key : "-");
	p->path = estrdup(path != nil && path[0] ? path : "-");
	p->attrs = attrs != nil ? estrdup(attrs) : nil;
	p->win = win != nil ? estrdup(win) : nil;
	if(nanc > 0){
		p->anc = emalloc(nanc*sizeof(int));
		memmove(p->anc, anc, nanc*sizeof(int));
	}
	p->nanc = nanc;
	p->state = state;
	p->seq = ++seq;
	if(holding){
		*heldtail = p;
		heldtail = &p->next;
		return p->seq;
	}
	deliver(p);
	freepend(p);
	servesubs();
	return seq;
}

/* the key of n: its tag, or its nearest tagged ancestor's */
static char*
keyof(Node *n)
{
	char *t;

	for(; n != nil; n = n->parent)
		if((t = getprop(n, "tag")) != nil && t[0] != 0)
			return t;
	return "-";
}

int
emitnode(char *name, Win *w, Node *n, char *attrs, int state)
{
	int *anc, nanc, s;
	char *path;

	anc = nil;
	nanc = 0;
	if(n != nil){
		nanc = ancestors(n, &anc);
		path = relpath(n);
	}else
		path = estrdup("-");
	s = emit(name, n != nil ? keyof(n) : "-", path, attrs, w != nil ? w->name : nil, anc, nanc, state);
	free(path);
	free(anc);
	return s;
}

void
hold(void)
{
	holding = 1;
	held = nil;
	heldtail = &held;
}

void
release(int ok)
{
	Pend *p, *n;

	holding = 0;
	for(p = held; p != nil; p = n){
		n = p->next;
		if(ok)
			deliver(p);
		freepend(p);
	}
	held = nil;
	heldtail = &held;
	servesubs();
}

static char *phrases[] = {
	"unknown command", "bad syntax", "bad value", "unknown type",
	"unknown style word", "type is fixed", "not found", "in use", "disabled",
	"permission denied", "unsupported", "too large", "no batch", "app gone",
	"gone", nil,
};

/* the error's code: its phrase with dashes */
char*
codeof(char *err)
{
	char *s, *p;
	int i, l;

	for(i = 0; phrases[i] != nil; i++){
		l = strlen(phrases[i]);
		if(strncmp(err, phrases[i], l) == 0 && (err[l] == 0 || err[l] == ':'))
			break;
	}
	s = estrdup(phrases[i] != nil ? phrases[i] : "bad value");
	for(p = s; *p; p++)
		if(*p == ' ')
			*p = '-';
	return s;
}

/*
 * A write failed: say so loudly, and send the error event. upath is the
 * file's path from $ui.
 */
void
emiterr(char *upath, char *err, Node *n, Win *w, char *wname)
{
	char *code, *attrs, *rel, *pre;
	int *anc, nanc;

	fprint(2, "uitree: write %s: %s\n", upath, err);
	code = codeof(err);
	attrs = smprint("code=%s", code);
	rel = estrdup(upath);
	if(wname != nil){
		pre = smprint("win/%s/", wname);
		if(strncmp(upath, pre, strlen(pre)) == 0){
			free(rel);
			rel = estrdup(upath+strlen(pre));
		}else if(strcmp(upath, pre) == 0 || strlen(upath) == strlen(pre)-1){
			free(rel);
			rel = estrdup(".");
		}
		free(pre);
	}
	anc = nil;
	nanc = 0;
	if(n != nil)
		nanc = ancestors(n, &anc);
	USED(w);
	emit("error", "-", rel, attrs, wname, anc, nanc, 0);
	free(anc);
	free(rel);
	free(attrs);
	free(code);
}

/* end the streams of windows and nodes that are gone */
void
sweep(void)
{
	Sub *s;
	char *l;

	for(s = subs; s != nil; s = s->next){
		if(s->gone)
			continue;
		if((s->kind == Swin && winbyname(s->win) == nil)
		|| (s->kind == Snode && (winbyname(s->win) == nil || nodebyid(s->node) == nil))){
			l = smprint("gone - - seq=%d\n", ++seq);
			enqueue(s, l, "gone", nil, 0, 0);
			free(l);
			s->gone = 1;
		}
	}
	servesubs();
	uisync();
}

static int
servesub(Sub *s)
{
	Req *r;
	Rq *q;
	Ev *e;
	long n, m, cnt;
	char *buf;

	while((q = s->rq) != nil){
		r = q->r;
		if(s->cur == nil && s->head != nil && !quitting){
			e = s->head;
			s->head = e->next;
			if(s->tail == e)
				s->tail = nil;
			s->nq--;
			s->cur = e->line;
			s->curoff = 0;
			free(e->name);
			free(e->win);
			free(e);
		}
		if(s->cur == nil){
			if(!s->gone && !quitting)
				return 0;
			s->rq = q->next;
			free(q);
			r->ofcall.count = 0;
			respond(r, nil);
			continue;
		}
		cnt = r->ifcall.count;
		buf = r->ofcall.data;
		n = 0;
		while(n < cnt && s->cur != nil){
			m = strlen(s->cur) - s->curoff;
			if(m > cnt - n)
				m = cnt - n;
			memmove(buf+n, s->cur+s->curoff, m);
			n += m;
			s->curoff += m;
			if(s->cur[s->curoff] == 0){
				free(s->cur);
				s->cur = nil;
				if(s->head != nil && n < cnt){
					e = s->head;
					s->head = e->next;
					if(s->tail == e)
						s->tail = nil;
					s->nq--;
					s->cur = e->line;
					s->curoff = 0;
					free(e->name);
					free(e->win);
					free(e);
				}
			}
		}
		s->rq = q->next;
		free(q);
		r->ofcall.count = n;
		respond(r, nil);
	}
	return 1;
}

/*
 * Replies go out on the server proc's descriptors, which the UI thread
 * doesn't share; from anywhere else, the responder proc answers.
 */
int	srvpid;
static Channel	*servec;

static void
responder(void*)
{
	Sub *s;

	for(;;){
		recvul(servec);
		lockmodel();
		for(s = subs; s != nil; s = s->next)
			servesub(s);
		unlockmodel();
	}
}

void
startresponder(void)
{
	srvpid = getpid();
	servec = chancreate(sizeof(ulong), 1);
	proccreate(responder, nil, 32*1024);
}

void
servesubs(void)
{
	Sub *s;

	if(servec != nil && getpid() != srvpid){
		nbsendul(servec, 1);
		return;
	}
	for(s = subs; s != nil; s = s->next)
		servesub(s);
}

void
subread(Sub *s, Req *r)
{
	Rq *q, **l;

	q = emalloc(sizeof *q);
	q->r = r;
	q->next = nil;
	for(l = &s->rq; *l != nil; l = &(*l)->next)
		;
	*l = q;
	servesub(s);
}

void
subflush(Req *r)
{
	Sub *s;
	Rq **l, *q;

	for(s = subs; s != nil; s = s->next)
		for(l = &s->rq; *l != nil; l = &(*l)->next)
			if((*l)->r == r->oldreq){
				q = *l;
				*l = q->next;
				respond(q->r, "interrupted");
				free(q);
				respond(r, nil);
				return;
			}
	respond(r, nil);
}
