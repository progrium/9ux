/*
 * ndb(6) text that keeps its formatting.  A file is a list of chunks:
 * comments and blank lines are kept as text, records as text plus their
 * parsed tuples.  Writing the file back gives the same bytes for every
 * chunk whose record hasn't changed.
 *
 * Tuple syntax follows libndb's _ndbparsetuple: attr, attr=value,
 * attr="value with white space"; '#' at the start of a tuple begins a
 * comment.
 */
#include <u.h>
#include <libc.h>
#include "meta.h"

#define ISWHITE(c) ((c) == ' ' || (c) == '\t' || (c) == '\r')

void*
emalloc(ulong n)
{
	void *p;

	p = mallocz(n, 1);
	if(p == nil)
		sysfatal("out of memory");
	setmalloctag(p, getcallerpc(&n));
	return p;
}

void*
erealloc(void *p, ulong n)
{
	p = realloc(p, n);
	if(p == nil)
		sysfatal("out of memory");
	return p;
}

char*
estrdup(char *s)
{
	s = strdup(s);
	if(s == nil)
		sysfatal("out of memory");
	return s;
}

static char*
strndup(char *s, long n)
{
	char *t;

	t = emalloc(n+1);
	memmove(t, s, n);
	t[n] = 0;
	return t;
}

Rec*
recnew(char *kind, char *key)
{
	Rec *r;

	r = emalloc(sizeof *r);
	recadd(r, kind, key);
	return r;
}

void
recadd(Rec *r, char *attr, char *val)
{
	r->t = erealloc(r->t, (r->nt+1)*sizeof r->t[0]);
	r->t[r->nt].attr = estrdup(attr);
	r->t[r->nt].val = val ? estrdup(val) : nil;
	r->nt++;
}

Rec*
recdup(Rec *r)
{
	Rec *n;
	int i;

	n = emalloc(sizeof *n);
	for(i = 0; i < r->nt; i++)
		recadd(n, r->t[i].attr, r->t[i].val);
	return n;
}

void
recfree(Rec *r)
{
	int i;

	if(r == nil)
		return;
	for(i = 0; i < r->nt; i++){
		free(r->t[i].attr);
		free(r->t[i].val);
	}
	free(r->t);
	free(r);
}

/* first value of attr (not the kind tuple), "" for a bare attribute */
char*
recget(Rec *r, char *attr)
{
	int i;

	for(i = 1; i < r->nt; i++)
		if(strcmp(r->t[i].attr, attr) == 0)
			return r->t[i].val ? r->t[i].val : "";
	return nil;
}

/* remove every value of attr (never the kind tuple); returns how many */
int
recdel(Rec *r, char *attr)
{
	int i, j, n;

	n = 0;
	for(i = j = 1; i < r->nt; i++){
		if(strcmp(r->t[i].attr, attr) == 0){
			free(r->t[i].attr);
			free(r->t[i].val);
			n++;
		}else
			r->t[j++] = r->t[i];
	}
	r->nt = j;
	return n;
}

/*
 * replace all values of attr with val[0..n-1], at the position of its
 * first old value (or at the end).
 */
void
recset(Rec *r, char *attr, char **val, int n)
{
	int i, at, k;

	at = -1;
	for(i = 1; i < r->nt; i++)
		if(strcmp(r->t[i].attr, attr) == 0){
			at = i;
			break;
		}
	recdel(r, attr);
	if(at < 0 || at > r->nt)
		at = r->nt;
	r->t = erealloc(r->t, (r->nt+n)*sizeof r->t[0]);
	memmove(r->t+at+n, r->t+at, (r->nt-at)*sizeof r->t[0]);
	for(k = 0; k < n; k++){
		r->t[at+k].attr = estrdup(attr);
		r->t[at+k].val = val[k] ? estrdup(val[k]) : nil;
	}
	r->nt += n;
}

static int
streq(char *a, char *b)
{
	if(a == nil || b == nil)
		return a == b;
	return strcmp(a, b) == 0;
}

int
receq(Rec *a, Rec *b)
{
	int i;

	if(a == nil || b == nil)
		return a == b;
	if(a->nt != b->nt)
		return 0;
	for(i = 0; i < a->nt; i++)
		if(strcmp(a->t[i].attr, b->t[i].attr) != 0 || !streq(a->t[i].val, b->t[i].val))
			return 0;
	return 1;
}

int
validattr(char *a)
{
	char *p;

	if(*a == 0 || strlen(a) > Attrmax){
		werrstr("bad value: attribute name %q", a);
		return 0;
	}
	for(p = a; *p; p++)
		if(*p == '=' || *p == '"' || *p == '#' || *p == '\n' || ISWHITE(*p)){
			werrstr("bad value: attribute name %q", a);
			return 0;
		}
	return 1;
}

static int
needquote(char *v)
{
	char *p;

	if(*v == 0 || *v == '"' || *v == '#')
		return 1;
	for(p = v; *p; p++)
		if(ISWHITE(*p))
			return 1;
	return 0;
}

int
validval(char *v)
{
	if(v == nil)
		return 1;
	if(strchr(v, '\n') != nil || (needquote(v) && strchr(v, '"') != nil)){
		werrstr("bad value: can't be written in ndb: %q", v);
		return 0;
	}
	return 1;
}

static int
tupfmt(Fmt *f, Tup *t)
{
	if(t->val == nil)
		return fmtprint(f, "%s", t->attr);
	if(needquote(t->val))
		return fmtprint(f, "%s=\"%s\"", t->attr, t->val);
	return fmtprint(f, "%s=%s", t->attr, t->val);
}

int
recfmt(Fmt *f, Rec *r)
{
	return recfmtw(f, r, 1);
}

/* canonical form: one line, or wrapped at ~72 columns with tabs */
int
recfmtw(Fmt *f, Rec *r, int wrap)
{
	int i, col, n;
	char *s;

	col = 0;
	for(i = 0; i < r->nt; i++){
		s = smprint("%s%s%s", r->t[i].attr, r->t[i].val ? "=" : "",
			r->t[i].val ? (needquote(r->t[i].val) ? "\"" : "") : "");
		n = strlen(s) + (r->t[i].val ? strlen(r->t[i].val) : 0) + 1;
		free(s);
		if(i > 0){
			if(wrap && col + n > 72){
				fmtprint(f, "\n\t");
				col = 8;
			}else{
				fmtprint(f, " ");
				col++;
			}
		}
		tupfmt(f, &r->t[i]);
		col += n;
	}
	return fmtprint(f, "\n");
}

char*
rectext(Rec *r)
{
	Fmt f;

	fmtstrinit(&f);
	recfmt(&f, r);
	return fmtstrflush(&f);
}

/* parse the tuples of one line (up to '\n' or end) into r */
static void
parseline(char *p, char *e, Rec *r)
{
	char *a, *v, *attr, *val;
	int bare;

	for(;;){
		while(p < e && ISWHITE(*p))
			p++;
		if(p >= e || *p == '#' || *p == '\n')
			return;
		a = p;
		while(p < e && *p != '=' && !ISWHITE(*p) && *p != '\n')
			p++;
		attr = strndup(a, p-a);
		while(p < e && ISWHITE(*p))
			p++;
		bare = 1;
		val = nil;
		if(p < e && *p == '='){
			bare = 0;
			p++;
			if(p < e && *p == '"'){
				v = ++p;
				while(p < e && *p != '\n' && *p != '"')
					p++;
				val = strndup(v, p-v);
				if(p < e && *p == '"')
					p++;
			}else if(p < e && *p == '#'){
				val = estrdup("");
			}else{
				v = p;
				while(p < e && !ISWHITE(*p) && *p != '\n')
					p++;
				val = strndup(v, p-v);
			}
		}
		USED(bare);
		r->t = erealloc(r->t, (r->nt+1)*sizeof r->t[0]);
		r->t[r->nt].attr = attr;
		r->t[r->nt].val = val;
		r->nt++;
	}
}

/* does the line [p,e) hold any tuple? */
static int
hastuple(char *p, char *e)
{
	while(p < e && ISWHITE(*p))
		p++;
	return p < e && *p != '#' && *p != '\n';
}

static void
addchunk(Nfile *f, char *text, long n, Rec *r)
{
	f->c = erealloc(f->c, (f->nc+1)*sizeof f->c[0]);
	f->c[f->nc].text = strndup(text, n);
	f->c[f->nc].rec = r;
	f->nc++;
}

/*
 * Split text into chunks.  As in libndb, a record starts with a line at
 * the left margin and continues over indented lines; an empty line ends
 * it, while comment and white-space-only lines don't.  Lines between a
 * record's last tuple line and the next record are kept as text.
 */
int
ndbparse(char *text, Nfile *f)
{
	char *p, *e, *le, *rs, *re, *ts;
	Rec *r;

	memset(f, 0, sizeof *f);
	p = text;
	e = text + strlen(text);
	ts = p;		/* start of pending text */
	rs = re = nil;	/* current record span */
	r = nil;
	while(p < e){
		le = strchr(p, '\n');
		le = le ? le+1 : e;
		if(*p == '\n' || (*p == '\r' && p[1] == '\n')){
			/* empty line ends a record */
			if(r != nil){
				addchunk(f, rs, re-rs, r);
				r = nil;
				ts = re;
			}
		}else if(hastuple(p, le) && (!ISWHITE(*p) || r == nil)){
			/* start of a record */
			if(r != nil){
				addchunk(f, rs, re-rs, r);
				ts = re;
			}
			if(p > ts)
				addchunk(f, ts, p-ts, nil);
			r = emalloc(sizeof *r);
			rs = p;
			parseline(p, le, r);
			re = le;
		}else if(r != nil && hastuple(p, le)){
			parseline(p, le, r);
			re = le;
		}
		p = le;
	}
	if(r != nil){
		addchunk(f, rs, re-rs, r);
		ts = re;
	}
	if(e > ts)
		addchunk(f, ts, e-ts, nil);
	return 0;
}

void
nfilefree(Nfile *f)
{
	int i;

	for(i = 0; i < f->nc; i++){
		free(f->c[i].text);
		recfree(f->c[i].rec);
	}
	free(f->c);
	f->c = nil;
	f->nc = 0;
}

char*
nfiletext(Nfile *f)
{
	Fmt fmt;
	int i;

	fmtstrinit(&fmt);
	for(i = 0; i < f->nc; i++){
		if(f->c[i].text != nil)
			fmtprint(&fmt, "%s", f->c[i].text);
		else if(f->c[i].rec != nil)
			recfmt(&fmt, f->c[i].rec);
	}
	return fmtstrflush(&fmt);
}

/* the last record in f with this kind and key */
Chunk*
nfilefind(Nfile *f, char *kind, char *key)
{
	int i;
	Rec *r;

	for(i = f->nc-1; i >= 0; i--){
		r = f->c[i].rec;
		if(r != nil && r->nt > 0 && strcmp(r->t[0].attr, kind) == 0
		&& streq(r->t[0].val ? r->t[0].val : "", key))
			return &f->c[i];
	}
	return nil;
}

/* do the first n chunks end with a blank line (or are there none)? */
static int
endsblank(Nfile *f, int n)
{
	Chunk *c;
	int l;

	if(n == 0)
		return 1;
	c = &f->c[n-1];
	if(c->rec != nil || c->text == nil)
		return 0;
	l = strlen(c->text);
	return l >= 1 && c->text[l-1] == '\n' && (l == 1 || c->text[l-2] == '\n');
}

/* append a record, separated from what's before by a blank line */
void
nfileappend(Nfile *f, Rec *r)
{
	Chunk *c;
	int n;

	if(f->nc > 0){
		c = &f->c[f->nc-1];
		if(c->text != nil){
			n = strlen(c->text);
			if(n > 0 && c->text[n-1] != '\n')
				addchunk(f, "\n", 1, nil);
		}
		if(!endsblank(f, f->nc))
			addchunk(f, "\n", 1, nil);
	}
	f->c = erealloc(f->c, (f->nc+1)*sizeof f->c[0]);
	f->c[f->nc].text = nil;
	f->c[f->nc].rec = r;
	f->nc++;
}

/* delete a record's chunk, and a blank line that would be left doubled */
void
nfiledel(Nfile *f, Chunk *c)
{
	int i;
	char *t;

	i = c - f->c;
	free(c->text);
	recfree(c->rec);
	memmove(f->c+i, f->c+i+1, (f->nc-i-1)*sizeof f->c[0]);
	f->nc--;
	if(i < f->nc && f->c[i].rec == nil && (t = f->c[i].text) != nil
	&& t[0] == '\n' && endsblank(f, i))
		memmove(t, t+1, strlen(t));
}
