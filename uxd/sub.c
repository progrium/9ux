/*
 * uxd's own events file (README.md, Stream files): per-open
 * subscriptions with bounded queues, resync first, lost on overflow.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "dat.h"
#include "fns.h"

static Sub *subs;
static ulong seq;

static void
enqueue(Sub *s, char *msg, ulong n)
{
	int i, dropped;

	if(s->nq == Qmax){
		dropped = s->nq + 1;
		for(i = 0; i < s->nq; i++)
			free(s->q[i]);
		s->nq = 0;
		free(msg);
		msg = smprint("%lud lost n=%d\n", n, dropped);
	}
	s->q[s->nq++] = msg;
}

/* answer as many blocked reads as there are messages for */
static void
serve(Sub *s)
{
	Req *r;
	Op *op;
	char *buf;
	long n, len;
	int i;

	while(s->nrq > 0 && s->nq > 0){
		r = s->rq[0];
		s->nrq--;
		memmove(s->rq, s->rq+1, s->nrq*sizeof s->rq[0]);
		op = r->aux;
		op->cancel = nil;
		len = strlen(s->q[0]);
		if(len > r->ifcall.count){
			reply(op, "short read");
			continue;
		}
		buf = emalloc9p(r->ifcall.count);
		n = 0;
		for(i = 0; i < s->nq; i++){
			len = strlen(s->q[i]);
			if(n+len > r->ifcall.count)
				break;
			memmove(buf+n, s->q[i], len);
			n += len;
			free(s->q[i]);
		}
		s->nq -= i;
		memmove(s->q, s->q+i, s->nq*sizeof s->q[0]);
		r->ofcall.data = buf;
		r->ofcall.count = n;
		op->freeme = buf;
		reply(op, nil);
	}
}

void
postevent(char *fmt, ...)
{
	va_list arg;
	char *body;
	Sub *s;
	ulong n;

	va_start(arg, fmt);
	body = vsmprint(fmt, arg);
	va_end(arg);
	n = ++seq;
	for(s = subs; s != nil; s = s->next){
		enqueue(s, smprint("%lud %s\n", n, body), n);
		serve(s);
	}
	free(body);
}

Sub*
newsub(void)
{
	Sub *s;

	s = emalloc9p(sizeof *s);
	memset(s, 0, sizeof *s);
	s->q[s->nq++] = smprint("%lud resync\n", ++seq);
	s->next = subs;
	subs = s;
	return s;
}

static void
cancelread(Op *op)
{
	Sub *s;
	int i;

	s = op->waiton;
	for(i = 0; i < s->nrq; i++)
		if(s->rq[i] == op->r){
			s->nrq--;
			memmove(s->rq+i, s->rq+i+1, (s->nrq-i)*sizeof s->rq[0]);
			break;
		}
	op->cancel = nil;
	reply(op, "interrupted");
}

void
subread(Sub *s, Op *op)
{
	if(s->nrq > 0){
		reply(op, "busy: a read is already pending");
		return;
	}
	s->rq = erealloc9p(s->rq, (s->nrq+1)*sizeof s->rq[0]);
	s->rq[s->nrq++] = op->r;
	op->cancel = cancelread;
	op->waiton = s;
	serve(s);
}

void
freesub(Sub *s)
{
	Sub **l;
	int i;

	for(l = &subs; *l != nil; l = &(*l)->next)
		if(*l == s){
			*l = s->next;
			break;
		}
	for(i = 0; i < s->nq; i++)
		free(s->q[i]);
	/* reads still blocked hold the fid, so there are none */
	free(s->rq);
	free(s);
}
