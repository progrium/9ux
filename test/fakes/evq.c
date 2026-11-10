#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <draw.h>
#include "fake.h"

int	qmax = 256;
vlong	seq;

static Sub *subs;

void
fakeinit(void)
{
	quotefmtinstall();
	fmtinstall('R', Rfmt);
	fmtinstall('P', Pfmt);
}

Sub*
subopen(char *topic)
{
	Sub *s;
	Msg m;

	s = emalloc9p(sizeof *s);
	memset(s, 0, sizeof *s);
	s->topic = estrdup9p(topic);
	s->max = qmax;
	s->next = subs;
	subs = s;
	mbegin(&m, "resync");
	emit1nl(s, mend(&m));
	return s;
}

static void
qfree(Sub *s)
{
	int i;

	for(i = 0; i < s->nq; i++)
		free(s->q[i]);
	free(s->q);
	s->q = nil;
	s->nq = 0;
}

void
subclose(Sub *s)
{
	Sub **l;
	int i;

	for(l = &subs; *l != nil; l = &(*l)->next)
		if(*l == s){
			*l = s->next;
			break;
		}
	for(i = 0; i < s->nwait; i++)
		respond(s->wait[i], "gone");
	free(s->wait);
	qfree(s);
	free(s->topic);
	free(s);
}

/* answer blocked reads while there is something to give them */
static void
drain(Sub *s)
{
	Req *r;
	long n, cnt, len;
	char *buf;
	int i;

	while(s->nwait > 0 && (s->nq > 0 || s->gone)){
		r = s->wait[0];
		s->nwait--;
		memmove(s->wait, s->wait+1, s->nwait*sizeof s->wait[0]);
		if(s->nq == 0){	/* gone and drained: end of file */
			r->ofcall.count = 0;
			respond(r, nil);
			continue;
		}
		cnt = r->ifcall.count;
		len = strlen(s->q[0]);
		if(len > cnt){
			respond(r, "short read");
			continue;
		}
		buf = r->ofcall.data;
		n = 0;
		for(i = 0; i < s->nq; i++){
			len = strlen(s->q[i]);
			if(n+len > cnt)
				break;
			memmove(buf+n, s->q[i], len);
			n += len;
			free(s->q[i]);
		}
		s->nq -= i;
		memmove(s->q, s->q+i, s->nq*sizeof s->q[0]);
		r->ofcall.count = n;
		respond(r, nil);
	}
}

void
subread(Sub *s, Req *r)
{
	s->wait = erealloc9p(s->wait, (s->nwait+1)*sizeof s->wait[0]);
	s->wait[s->nwait++] = r;
	drain(s);
}

int
subflush(Req *old)
{
	Sub *s;
	int i;

	for(s = subs; s != nil; s = s->next)
		for(i = 0; i < s->nwait; i++)
			if(s->wait[i] == old){
				s->nwait--;
				memmove(s->wait+i, s->wait+i+1, (s->nwait-i)*sizeof s->wait[0]);
				respond(old, "interrupted");
				return 1;
			}
	return 0;
}

/* queue one line (msg has no seq yet; we add it) */
static void
enq(Sub *s, char *line)
{
	Msg m;
	long dropped;

	if(s->gone)
		return;
	if(s->nq >= s->max){
		dropped = s->nq + 1;
		qfree(s);
		seq++;
		mbegin(&m, "lost");
		mattr(&m, "n", "%ld", dropped);
		line = mend(&m);
		s->q = erealloc9p(s->q, sizeof s->q[0]);
		s->q[s->nq++] = smprint("%lld %s\n", seq, line);
		free(line);
		return;
	}
	s->q = erealloc9p(s->q, (s->nq+1)*sizeof s->q[0]);
	s->q[s->nq++] = smprint("%lld %s\n", seq, line);
}

/* only used for resync, which belongs to one subscription */
void
emit1nl(Sub *s, char *msg)
{
	seq++;
	enq(s, msg);
	free(msg);
}

static int
wants(Sub *s, char **topics, int ntopic)
{
	int i;

	for(i = 0; i < ntopic; i++)
		if(strcmp(s->topic, topics[i]) == 0)
			return 1;
	return 0;
}

/* deliver msg (malloced, no seq, no newline) to every subscriber of any topic */
void
emit(char **topics, int ntopic, char *msg)
{
	Sub *s;

	seq++;
	for(s = subs; s != nil; s = s->next)
		if(wants(s, topics, ntopic)){
			enq(s, msg);
			drain(s);
		}
	free(msg);
}

void
emit1(char *topic, char *msg)
{
	emit(&topic, 1, msg);
}

/* the object behind topic is gone: queue `gone`, then EOF */
void
subgone(char *topic)
{
	Sub *s;
	Msg m;

	seq++;
	for(s = subs; s != nil; s = s->next)
		if(strcmp(s->topic, topic) == 0 && !s->gone){
			mbegin(&m, "gone");
			enq(s, mend(&m));
			s->gone = 1;
			drain(s);
		}
}

/* quote a token only when tokenize needs it (README.md "Messages") */
char*
tokq(char *s)
{
	Fmt f;
	char *p;

	if(*s != 0 && strpbrk(s, " \t\n'") == nil)
		return estrdup9p(s);
	fmtstrinit(&f);
	fmtprint(&f, "'");
	for(p = s; *p; p++)
		if(*p == '\'')
			fmtprint(&f, "''");
		else if(*p == '\n')
			fmtprint(&f, " ");
		else
			fmtprint(&f, "%c", *p);
	fmtprint(&f, "'");
	return fmtstrflush(&f);
}

void
mbegin(Msg *m, char *verb)
{
	fmtstrinit(&m->f);
	fmtprint(&m->f, "%s", verb);
	m->started = 1;
}

void
mtok(Msg *m, char *fmt, ...)
{
	va_list arg;
	char *s, *v;

	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	v = tokq(s);
	fmtprint(&m->f, " %s", v);
	free(v);
	free(s);
}

void
mattr(Msg *m, char *name, char *fmt, ...)
{
	va_list arg;
	char *v, *s;

	va_start(arg, fmt);
	v = vsmprint(fmt, arg);
	va_end(arg);
	s = smprint("%s=%s", name, v);
	free(v);
	v = tokq(s);
	fmtprint(&m->f, " %s", v);
	free(v);
	free(s);
}

char*
mend(Msg *m)
{
	return fmtstrflush(&m->f);
}

int
cmdparse(Cmd *c, char *line)
{
	int i;

	memset(c, 0, sizeof *c);
	c->buf = estrdup9p(line);
	c->ntok = tokenize(c->buf, c->tok, Maxtok);
	for(i = 0; i < c->ntok; i++)
		if(strchr(c->tok[i], '=') != nil)
			break;
	c->npos = i;
	return c->ntok;
}

void
cmdfree(Cmd *c)
{
	free(c->buf);
	c->buf = nil;
}

char*
cmdattr(Cmd *c, char *name)
{
	int i, n;

	n = strlen(name);
	for(i = c->npos; i < c->ntok; i++)
		if(strncmp(c->tok[i], name, n) == 0 && c->tok[i][n] == '=')
			return c->tok[i]+n+1;
	return nil;
}

/* every token after the positional ones is a known name=value */
int
cmdattrok(Cmd *c, char **names)
{
	int i, j, n, ok;

	for(i = c->npos; i < c->ntok; i++){
		ok = 0;
		for(j = 0; names[j] != nil; j++){
			n = strlen(names[j]);
			if(strncmp(c->tok[i], names[j], n) == 0 && c->tok[i][n] == '='){
				ok = 1;
				break;
			}
		}
		if(!ok && strncmp(c->tok[i], "x.", 2) != 0)
			return 0;
	}
	return 1;
}

int
isid(char *s)
{
	char *p;

	if(s == nil || *s == 0 || strlen(s) > 64)
		return 0;
	if(!(*s >= 'a' && *s <= 'z') && !(*s >= '0' && *s <= '9'))
		return 0;
	for(p = s; *p; p++)
		if(!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9')
		&& *p != '.' && *p != '_' && *p != '-')
			return 0;
	return 1;
}

int
isbool(char *s)
{
	return s != nil && (strcmp(s, "on") == 0 || strcmp(s, "off") == 0);
}

int
parserect(char *s, Rectangle *r)
{
	char *f[5], buf[128];

	if(s == nil || strlen(s) >= sizeof buf)
		return -1;
	strcpy(buf, s);
	if(getfields(buf, f, 5, 0, ",") != 4)
		return -1;
	r->min.x = atoi(f[0]);
	r->min.y = atoi(f[1]);
	r->max.x = atoi(f[2]);
	r->max.y = atoi(f[3]);
	if(Dx(*r) <= 0 || Dy(*r) <= 0)
		return -1;
	return 0;
}

char*
fmtrect(Rectangle r)
{
	return smprint("%d,%d,%d,%d", r.min.x, r.min.y, r.max.x, r.max.y);
}

char*
eachline(char *data, long n, char *(*fn)(char*, void*), void *arg)
{
	char *buf, *p, *e, *err;

	buf = emalloc9p(n+1);
	memmove(buf, data, n);
	buf[n] = 0;
	err = nil;
	for(p = buf; p != nil && *p != 0; p = e){
		e = strchr(p, '\n');
		if(e != nil)
			*e++ = 0;
		if((err = fn(p, arg)) != nil)
			break;
	}
	free(buf);
	return err;
}

char*
attrq(char *name, char *val)
{
	char *s, *q;

	s = smprint("%s=%s", name, val);
	q = tokq(s);
	free(s);
	return q;
}
