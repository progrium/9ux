/*
 * A 9P client multiplexer for one app's server.
 *
 * A reader proc turns the app's replies into Msgs on rxchan and a
 * writer proc sends what rpc() queues, so neither a slow app nor a
 * full pipe blocks the main proc. dispatch() runs in the main proc and
 * wakes the thread waiting on each reply.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "dat.h"
#include "fns.h"


static Rpc reserved;	/* a tag flushed but not yet answered */

Channel *rxchan;

static void
reader(void *v)
{
	Conn *c;
	Msg *m;
	uchar *buf;
	int n;

	c = v;
	threadsetname("uxd reader");
	for(;;){
		if(c->closing)
			break;
		buf = emalloc9p(Msize);
		n = read9pmsg(c->fd, buf, Msize);
		if(n <= 0){
			free(buf);
			break;
		}
		m = emalloc9p(sizeof *m);
		m->kind = Mrx;
		m->c = c;
		m->buf = buf;
		if(convM2S(buf, n, &m->f) != n){
			free(buf);
			free(m);
			break;
		}
		sendp(rxchan, m);
	}
	m = emalloc9p(sizeof *m);
	m->kind = Mhangup;
	m->c = c;
	sendp(rxchan, m);
}

static void
writer(void *v)
{
	Conn *c;
	Msg *m;
	uchar *b;
	long n;

	c = v;
	threadsetname("uxd writer");
	while((b = recvp(c->wc)) != nil){
		n = GBIT32(b);
		if(!c->wdead && write(c->fd, b, n) != n)
			c->wdead = 1;
		free(b);
	}
	m = emalloc9p(sizeof *m);
	m->kind = Mwdone;
	m->c = c;
	sendp(rxchan, m);
}

Conn*
newconn(int fd)
{
	Conn *c;

	c = emalloc9p(sizeof *c);
	memset(c, 0, sizeof *c);
	c->fd = fd;
	c->ref = 3;	/* the owner, the reader, the writer */
	c->wc = chancreate(sizeof(void*), 64);
	c->nextfid = 1;
	c->readerid = proccreate(reader, c, Stack);
	proccreate(writer, c, Stack);
	return c;
}

static void
connfree(Conn *c)
{
	if(--c->ref > 0)
		return;
	close(c->fd);
	chanfree(c->wc);
	free(c);
}

/* the owner is done with c: stop its procs, fail what's pending */
void
closeconn(Conn *c)
{
	if(c == nil || c->closing)
		return;
	c->closing = 1;
	sendp(c->wc, nil);
	threadint(c->readerid);
	connfree(c);
}

static void
complete(Rpc *r, Msg *m, char *err)
{
	r->done = 1;
	r->rx = m;
	r->err = err;
	if(r->flush != nil)
		r->flush->old = nil;
	if(r->ch != nil)
		sendul(r->ch, 1);
	else{
		freemsg(m);
		free(r);
	}
}

static int
alloctag(Conn *c, Rpc *r)
{
	int i;

	for(i = 0; i < nelem(c->tags); i++)
		if(c->tags[i] == nil){
			c->tags[i] = r;
			return i;
		}
	return -1;
}

static int
sendmsg(Conn *c, Fcall *tx)
{
	uchar *b;
	uint n;

	n = sizeS2M(tx);
	b = emalloc9p(n);
	if(convS2M(tx, b, n) != n){
		free(b);
		return -1;
	}
	sendp(c->wc, b);
	return 0;
}

/* a reply or hangup from a Conn's procs; main proc only */
void
dispatch(Msg *m)
{
	Conn *c;
	Rpc *r, *o;
	int i, tag;

	c = m->c;
	switch(m->kind){
	case Mwdone:
		free(m);
		connfree(c);
		return;
	case Mhangup:
		free(m);
		c->dead = 1;
		for(i = 0; i < nelem(c->tags); i++){
			r = c->tags[i];
			c->tags[i] = nil;
			if(r == nil || r == &reserved)
				continue;
			r->flush = nil;
			if(r->oldtag >= 0)
				free(r);	/* a Tflush: nobody waits */
			else
				complete(r, nil, "gone: the app hung up");
		}
		if(c->vrpc != nil){
			r = c->vrpc;
			c->vrpc = nil;
			complete(r, nil, "gone: the app hung up");
		}
		if(c->app != nil && !c->closing)
			apphungup(c->app);
		connfree(c);
		return;
	}

	tag = m->f.tag;
	if(tag == NOTAG){
		r = c->vrpc;
		c->vrpc = nil;
	}else if(tag < nelem(c->tags)){
		r = c->tags[tag];
		c->tags[tag] = nil;
	}else
		r = nil;
	if(r == nil || r == &reserved){
		freemsg(m);
		return;
	}
	if(r->flush != nil && tag != NOTAG)
		c->tags[tag] = &reserved;	/* until the Rflush */
	if(r->oldtag >= 0){
		/* the answer to a Tflush */
		o = r->old;
		if(o != nil && !o->done){
			if(o->tag >= 0 && o->tag < nelem(c->tags))
				c->tags[o->tag] = nil;
			o->flush = nil;
			complete(o, nil, "interrupted");
		}else if(r->oldtag >= 0 && r->oldtag < nelem(c->tags)
		&& c->tags[r->oldtag] == &reserved)
			c->tags[r->oldtag] = nil;
		freemsg(m);
		free(r);
		return;
	}
	complete(r, m, nil);
}

void
freemsg(Msg *m)
{
	if(m == nil)
		return;
	free(m->buf);
	free(m);
}

static void
seterr(Op *op, char *e)
{
	if(op != nil)
		strecpy(op->err, op->err+sizeof op->err, e);
}

/*
 * Send tx and wait for its reply. Returns the reply, or nil with the
 * error in op->err. While it waits, op->rpc lets a flush reach it.
 */
Msg*
rpc(Conn *c, Fcall *tx, Op *op)
{
	Rpc r;
	int tag;

	if(c == nil || c->dead || c->closing){
		seterr(op, "gone");
		return nil;
	}
	if(op != nil && op->flushed){
		seterr(op, "interrupted");
		return nil;
	}
	memset(&r, 0, sizeof r);
	r.c = c;
	r.oldtag = -1;
	if(tx->type == Tversion){
		if(c->vrpc != nil){
			seterr(op, "busy");
			return nil;
		}
		c->vrpc = &r;
		tag = NOTAG;
	}else if((tag = alloctag(c, &r)) < 0){
		seterr(op, "busy: too many requests to the app");
		return nil;
	}
	r.tag = tag;
	tx->tag = tag;
	r.ch = chancreate(sizeof(ulong), 1);
	if(sendmsg(c, tx) < 0){
		if(tag == NOTAG)
			c->vrpc = nil;
		else
			c->tags[tag] = nil;
		chanfree(r.ch);
		seterr(op, "bad value: message too large");
		return nil;
	}
	if(op != nil)
		op->rpc = &r;
	recvul(r.ch);
	if(op != nil)
		op->rpc = nil;
	chanfree(r.ch);
	if(r.err != nil){
		seterr(op, r.err);
		return nil;
	}
	if(r.rx->f.type == Rerror){
		seterr(op, r.rx->f.ename);
		freemsg(r.rx);
		return nil;
	}
	if(r.rx->f.type != tx->type+1){
		seterr(op, "bad reply from app");
		freemsg(r.rx);
		return nil;
	}
	return r.rx;
}

/* flush the RPC op is waiting on, if any */
void
flushrpc(Op *op)
{
	Rpc *o, *f;
	Fcall tx;
	Conn *c;
	int tag;

	o = op->rpc;
	if(o == nil || o->done || o->flush != nil || o->tag == NOTAG)
		return;
	c = o->c;
	if(c->dead || c->closing)
		return;
	f = emalloc9p(sizeof *f);
	memset(f, 0, sizeof *f);
	f->c = c;
	f->old = o;
	f->oldtag = o->tag;
	if((tag = alloctag(c, f)) < 0){
		free(f);
		return;
	}
	f->tag = tag;
	memset(&tx, 0, sizeof tx);
	tx.type = Tflush;
	tx.tag = tag;
	tx.oldtag = o->tag;
	o->flush = f;
	sendmsg(c, &tx);
}

/* send without waiting (clunks) */
void
rpcasync(Conn *c, Fcall *tx)
{
	Rpc *r;
	int tag;

	if(c == nil || c->dead || c->closing)
		return;
	r = emalloc9p(sizeof *r);
	memset(r, 0, sizeof *r);
	r->c = c;
	r->oldtag = -1;
	if((tag = alloctag(c, r)) < 0){
		free(r);
		return;
	}
	r->tag = tag;
	tx->tag = tag;
	if(sendmsg(c, tx) < 0){
		c->tags[tag] = nil;
		free(r);
	}
}

/*
 * Convenience wrappers.
 */
ulong
newfid(Conn *c)
{
	return c->nextfid++;
}

void
tclunk(Conn *c, ulong fid)
{
	Fcall tx;

	memset(&tx, 0, sizeof tx);
	tx.type = Tclunk;
	tx.fid = fid;
	rpcasync(c, &tx);
}

Msg*
twalk(Conn *c, ulong fid, ulong nfid, int n, char **names, Op *op)
{
	Fcall tx;
	int i;

	if(n > MAXWELEM){
		seterr(op, "bad value: walk too long");
		return nil;
	}
	memset(&tx, 0, sizeof tx);
	tx.type = Twalk;
	tx.fid = fid;
	tx.newfid = nfid;
	tx.nwname = n;
	for(i = 0; i < n; i++)
		tx.wname[i] = names[i];
	return rpc(c, &tx, op);
}

Msg*
topen(Conn *c, ulong fid, int mode, Op *op)
{
	Fcall tx;

	memset(&tx, 0, sizeof tx);
	tx.type = Topen;
	tx.fid = fid;
	tx.mode = mode;
	return rpc(c, &tx, op);
}

Msg*
tread(Conn *c, ulong fid, vlong off, ulong count, Op *op)
{
	Fcall tx;

	memset(&tx, 0, sizeof tx);
	tx.type = Tread;
	tx.fid = fid;
	tx.offset = off;
	tx.count = count;
	return rpc(c, &tx, op);
}

Msg*
twrite(Conn *c, ulong fid, vlong off, ulong count, char *data, Op *op)
{
	Fcall tx;

	memset(&tx, 0, sizeof tx);
	tx.type = Twrite;
	tx.fid = fid;
	tx.offset = off;
	tx.count = count;
	tx.data = data;
	return rpc(c, &tx, op);
}

Msg*
tstat(Conn *c, ulong fid, Op *op)
{
	Fcall tx;

	memset(&tx, 0, sizeof tx);
	tx.type = Tstat;
	tx.fid = fid;
	return rpc(c, &tx, op);
}

/*
 * Read a whole small file below fid (path relative to it, "" for fid
 * itself) into a NUL-terminated string. Used for version, state and
 * ctl help. Returns nil with op->err set.
 */
char*
readapp(Conn *c, ulong fid, char *path, Op *op)
{
	char *names[MAXWELEM], *p, *s;
	ulong nf;
	long n, tot;
	int nn;
	Msg *m;

	p = estrdup9p(path);
	nn = *p ? getfields(p, names, MAXWELEM, 0, "/") : 0;
	nf = newfid(c);
	if((m = twalk(c, fid, nf, nn, names, op)) == nil){
		free(p);
		return nil;
	}
	if(m->f.nwqid != nn){
		freemsg(m);
		free(p);
		seterr(op, "not found");
		return nil;
	}
	freemsg(m);
	free(p);
	if((m = topen(c, nf, OREAD, op)) == nil){
		tclunk(c, nf);
		return nil;
	}
	freemsg(m);
	s = nil;
	tot = 0;
	for(;;){
		if((m = tread(c, nf, tot, 8192, op)) == nil){
			free(s);
			tclunk(c, nf);
			return nil;
		}
		n = m->f.count;
		if(n <= 0){
			freemsg(m);
			break;
		}
		if(tot+n > 64*1024){
			freemsg(m);
			free(s);
			tclunk(c, nf);
			seterr(op, "too large");
			return nil;
		}
		s = erealloc9p(s, tot+n+1);
		memmove(s+tot, m->f.data, n);
		tot += n;
		freemsg(m);
	}
	tclunk(c, nf);
	if(s == nil)
		s = estrdup9p("");
	s[tot] = 0;
	return s;
}
