/*
 * Event streams: per-open subscriptions with bounded queues, as in
 * docs/protocols/README.md (Stream files) and ux.md (Events).
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

static char *vias[] = {
	[UXui]	"ui",
	[UXkey]	"key",
	[UXmenubar]	"menu",
	[UXctl]	"ctl",
};

char*
_via(int v)
{
	if(v < 0 || v >= nelem(vias))
		return "ui";
	return vias[v];
}

/* one name=value token, quoted as a whole when needed */
char*
_attr(char *name, char *val)
{
	char *s, *q;

	s = smprint("%s=%s", name, val);
	q = _quote(s);
	free(s);
	return q;
}

/* quote a token only where tokenize needs it: space, tab, quote, newline, or empty */
char*
_quote(char *s)
{
	Fmt f;
	char *p;

	if(*s != 0 && strpbrk(s, " \t'\n") == nil)
		return estrdup(s);
	fmtstrinit(&f);
	fmtrune(&f, '\'');
	for(p = s; *p; p++){
		if(*p == '\'')
			fmtrune(&f, '\'');
		fmtrune(&f, *p);
	}
	fmtrune(&f, '\'');
	return fmtstrflush(&f);
}

static int
matches(Sub *s, Obj *o)
{
	if(s->gone)
		return 0;
	switch(s->kind){
	case Oapp:
		return 1;
	case Owin:
		if(o == nil)
			return 0;
		if(o->kind == Owin)
			return o->oid == s->oid;
		if(o->kind == Octl)
			return ((Uxctl*)o)->w->oid == s->oid;
		return 0;
	default:
		return o != nil && o->oid == s->oid;
	}
}

static void
push(Sub *s, ulong seq, char *str)
{
	Msg *m, *n;
	int lost;

	if(s->n >= Qlen){
		lost = s->n + 1;
		for(m = s->head; m != nil; m = n){
			n = m->next;
			free(m->s);
			free(m);
		}
		s->head = s->tail = nil;
		s->n = 0;
		str = smprint("lost n=%d", lost);
	}else
		str = estrdup(str);
	m = emalloc(sizeof *m);
	m->seq = seq;
	m->s = str;
	if(s->tail)
		s->tail->next = m;
	else
		s->head = m;
	s->tail = m;
	s->n++;
}

static void
kick(void)
{
	if(ux.rkick)
		nbsendul(ux.rkick, 1);
}

/*
 * Queue an event about o (nil: the app) on every subscription that
 * follows it. The varargs are name, value pairs ending in nil; a nil
 * value skips the attribute. The object's own attribute is added.
 */
void
_event(Obj *o, char *ev, ...)
{
	va_list arg;
	char *name, *val, *a, *p;
	Fmt f;
	Sub *s;
	ulong seq;

	fmtstrinit(&f);
	fmtprint(&f, "%s", ev);
	if(o != nil)
		switch(o->kind){
		case Owin:
			fmtprint(&f, " win=%d", ((Uxwin*)o)->winid);
			break;
		case Octl:
			p = _ctlpath((Uxctl*)o);
			fmtprint(&f, " ctl=%s", p);
			free(p);
			break;
		case Omenu:
			fmtprint(&f, " menu=%s", ((Uxmenu*)o)->id);
			break;
		}
	va_start(arg, ev);
	while((name = va_arg(arg, char*)) != nil){
		val = va_arg(arg, char*);
		if(val == nil)
			continue;
		a = _attr(name, val);
		fmtprint(&f, " %s", a);
		free(a);
	}
	va_end(arg);
	p = fmtstrflush(&f);

	seq = ++ux.seq;
	for(s = ux.subs; s != nil; s = s->next)
		if(matches(s, o))
			push(s, seq, p);
	free(p);
	kick();
}

Sub*
_subscribe(Obj *o)
{
	Sub *s;

	s = emalloc(sizeof *s);
	if(o == nil)
		s->kind = Oapp;
	else{
		s->kind = o->kind;
		s->oid = o->oid;
	}
	push(s, ++ux.seq, "resync");
	s->next = ux.subs;
	ux.subs = s;
	return s;
}

void
_unsubscribe(Sub *s)
{
	Sub **l;
	Msg *m, *n;

	for(l = &ux.subs; *l != nil; l = &(*l)->next)
		if(*l == s){
			*l = s->next;
			break;
		}
	for(m = s->head; m != nil; m = n){
		n = m->next;
		free(m->s);
		free(m);
	}
	free(s);
}

/* the object went away: its own subscriptions get gone, then end of file */
void
_gone(Obj *o)
{
	Sub *s;
	ulong seq;

	seq = ++ux.seq;
	for(s = ux.subs; s != nil; s = s->next)
		if(s->kind != Oapp && s->oid == o->oid && !s->gone){
			push(s, seq, "gone");
			s->gone = 1;
		}
	kick();
}

/*
 * Fill r from s's queue. Returns 1 if r is ready to respond (with
 * *err set on error), 0 if it must wait.
 */
int
_subfill(Sub *s, Req *r, char **err)
{
	char *buf, line[64];
	long n, cnt, ln;
	Msg *m;

	*err = nil;
	if(s->head == nil){
		if(s->gone){
			r->ofcall.count = 0;
			return 1;
		}
		return 0;
	}
	cnt = r->ifcall.count;
	buf = r->ofcall.data;
	n = 0;
	while((m = s->head) != nil){
		snprint(line, sizeof line, "%lud ", m->seq);
		ln = strlen(line) + strlen(m->s) + 1;
		if(n + ln > cnt)
			break;
		memmove(buf+n, line, strlen(line));
		n += strlen(line);
		memmove(buf+n, m->s, strlen(m->s));
		n += strlen(m->s);
		buf[n++] = '\n';
		s->head = m->next;
		if(s->head == nil)
			s->tail = nil;
		s->n--;
		free(m->s);
		free(m);
	}
	if(n == 0){
		*err = "short read";
		return 1;
	}
	r->ofcall.count = n;
	return 1;
}

/* a read of s; called by the 9P proc with uxlk held. Returns 1 if r is answered by the caller. */
int
_subread(Sub *s, Req *r, char **err)
{
	if(s->r != nil){
		*err = "busy: one read at a time";
		return 1;
	}
	if(_subfill(s, r, err))
		return 1;
	s->r = r;
	return 0;
}

/* the responder: answers waiting reads and finished commands */
static void
responder(void*)
{
	Req **rs;
	char **errs;
	int n, a, i;
	Sub *s;
	Cmd *c, *next;
	char *err;

	threadsetname("uxresponder");
	a = 16;
	rs = emalloc(a*sizeof(Req*));
	errs = emalloc(a*sizeof(char*));
	for(;;){
		recvul(ux.rkick);
		qlock(&uxlk);
		n = 0;
		for(s = ux.subs; s != nil; s = s->next){
			if(s->r == nil || !_subfill(s, s->r, &err))
				continue;
			if(n == a){
				a *= 2;
				rs = erealloc(rs, a*sizeof(Req*));
				errs = erealloc(errs, a*sizeof(char*));
			}
			rs[n] = s->r;
			errs[n++] = err;
			s->r = nil;
		}
		c = ux.doneq;
		ux.doneq = nil;
		ux.responding = 1;
		qunlock(&uxlk);
		for(i = 0; i < n; i++)
			respond(rs[i], errs[i]);
		for(; c != nil; c = next){
			next = c->next;
			if(c->r != nil){
				if(c->err == nil)
					c->r->ofcall.count = c->r->ifcall.count;
				respond(c->r, c->err);
			}
			free(c->err);
			free(c->buf);
			free(c);
		}
		qlock(&uxlk);
		ux.responding = 0;
		qunlock(&uxlk);
	}
}

/*
 * Wait until the responder has answered every finished command; for
 * exit. Only the responder answers: postsrv gives the proc that posts
 * the service its own fd table without the connection.
 */
void
_flushdone(void)
{
	int i;

	for(i = 0; i < 1000; i++){
		qlock(&uxlk);
		if(ux.doneq == nil && !ux.responding){
			qunlock(&uxlk);
			return;
		}
		qunlock(&uxlk);
		nbsendul(ux.rkick, 1);
		sleep(1);
	}
}

void
_startresponder(void)
{
	ux.rkick = chancreate(sizeof(ulong), 1);
	proccreate(responder, nil, 16*1024);
}

/* Tflush from the 9P proc */
void
_subflush(Req *r)
{
	Req *old;
	Sub *s;
	Cmd **l, *c;

	old = r->oldreq;
	qlock(&uxlk);
	for(s = ux.subs; s != nil; s = s->next)
		if(s->r == old){
			s->r = nil;
			qunlock(&uxlk);
			respond(old, "interrupted");
			respond(r, nil);
			return;
		}
	for(l = &ux.cmdq; (c = *l) != nil; l = &c->next)
		if(c->r == old){
			*l = c->next;
			if(ux.cmdtail == &c->next)
				ux.cmdtail = l;
			qunlock(&uxlk);
			free(c->buf);
			free(c);
			respond(old, "interrupted");
			respond(r, nil);
			return;
		}
	qunlock(&uxlk);
	/* in progress: lib9p answers the flush after old's response */
	respond(r, nil);
}

/* the connection ended: answer its waiting reads */
void
_subhangup(Srv *srv)
{
	Sub *s;
	Req **rs;
	int n, i;

	qlock(&uxlk);
	n = 0;
	for(s = ux.subs; s != nil; s = s->next)
		if(s->r != nil && s->r->srv == srv)
			n++;
	rs = emalloc((n+1)*sizeof(Req*));
	n = 0;
	for(s = ux.subs; s != nil; s = s->next)
		if(s->r != nil && s->r->srv == srv){
			rs[n++] = s->r;
			s->r = nil;
		}
	qunlock(&uxlk);
	for(i = 0; i < n; i++)
		respond(rs[i], "hungup");
	free(rs);
}

/* queue a handler call; run by _runnotes once uxlk is released */
void
_notify(Uxfn *fn, void *arg, Uxev *e)
{
	Note *n;

	if(fn == nil)
		return;
	n = emalloc(sizeof *n);
	n->fn = fn;
	n->arg = arg;
	n->ev = *e;
	if(e->action){
		n->s1 = estrdup(e->action);
		n->ev.action = n->s1;
	}
	if(e->text){
		n->s2 = estrdup(e->text);
		n->ev.text = n->s2;
	}
	if(e->item){
		n->ev.item = estrdup(e->item);
	}
	if(ux.notetail == nil)
		ux.notetail = &ux.notes;
	*ux.notetail = n;
	ux.notetail = &n->next;
}

/* run queued handlers; called on the UI thread without uxlk */
void
_runnotes(void)
{
	Note *n, *next;

	for(;;){
		qlock(&uxlk);
		n = ux.notes;
		ux.notes = nil;
		ux.notetail = &ux.notes;
		qunlock(&uxlk);
		if(n == nil)
			return;
		for(; n != nil; n = next){
			next = n->next;
			n->fn(&n->ev, n->arg);
			free(n->s1);
			free(n->s2);
			free(n->ev.item);
			free(n);
		}
	}
}
