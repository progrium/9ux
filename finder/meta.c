/*
 * Client of uxmeta's /mnt/meta (docs/protocols/meta.md).
 */
#include <u.h>
#include <libc.h>
#include "fcore.h"

static char*
readall(int fd)
{
	char *buf;
	long n, tot, sz;

	sz = 8192;
	tot = 0;
	buf = malloc(sz+1);
	if(buf == nil)
		return nil;
	for(;;){
		if(tot == sz){
			sz *= 2;
			buf = realloc(buf, sz+1);
			if(buf == nil)
				return nil;
		}
		n = pread(fd, buf+tot, sz-tot, tot);
		if(n < 0){
			free(buf);
			return nil;
		}
		if(n == 0)
			break;
		tot += n;
	}
	buf[tot] = 0;
	return buf;
}

Fcmeta*
fcmetaopen(char *root)
{
	Fcmeta *m;
	char *p, *v;
	int fd;

	if(root == nil)
		root = "/mnt/meta";
	p = smprint("%s/version", root);
	fd = open(p, OREAD);
	free(p);
	if(fd < 0){
		werrstr("not found: no metadata service at %s", root);
		return nil;
	}
	v = readall(fd);
	close(fd);
	/* "uxmeta MAJOR.MINOR": any minor of major 1 (protocols/README.md) */
	if(v == nil || strncmp(v, "uxmeta 1", 8) != 0 || strchr(". \n", v[8]) == nil){
		free(v);
		werrstr("version mismatch: %s/version", root);
		return nil;
	}
	free(v);
	m = mallocz(sizeof *m, 1);
	m->root = strdup(root);
	m->gen = -1;
	return m;
}

void
fcmetaclose(Fcmeta *m)
{
	if(m == nil)
		return;
	free(m->root);
	free(m);
}

/*
 * Write one line to file and read the reply on the same fid.
 */
static char*
transact(Fcmeta *m, char *file, char *line)
{
	char *p, *r;
	int fd;

	p = smprint("%s/%s", m->root, file);
	fd = open(p, ORDWR);
	free(p);
	if(fd < 0)
		return nil;
	if(write(fd, line, strlen(line)) != strlen(line)){
		close(fd);
		return nil;
	}
	r = readall(fd);
	close(fd);
	return r;
}

Fcrec*
fcmetaquery(Fcmeta *m, char *fmt, ...)
{
	va_list arg;
	char *q, *r;
	Fcrec *recs;

	if(m == nil){
		werrstr("not found: no metadata service");
		return nil;
	}
	va_start(arg, fmt);
	q = vsmprint(fmt, arg);
	va_end(arg);
	r = transact(m, "query", q);
	free(q);
	if(r == nil)
		return nil;
	recs = fcparserecs(r, &m->gen);
	free(r);
	if(recs == nil)
		werrstr("not found: no records");
	return recs;
}

int
fcmetactl(Fcmeta *m, char *fmt, ...)
{
	va_list arg;
	char *p, *q;
	int fd, n;

	if(m == nil){
		werrstr("not found: no metadata service");
		return -1;
	}
	p = smprint("%s/ctl", m->root);
	fd = open(p, OWRITE);
	free(p);
	if(fd < 0)
		return -1;
	va_start(arg, fmt);
	q = vsmprint(fmt, arg);
	va_end(arg);
	n = strlen(q);
	if(write(fd, q, n) != n){
		free(q);
		close(fd);
		return -1;
	}
	free(q);
	close(fd);
	return 0;
}

vlong
fcmetagen(Fcmeta *m)
{
	char *p, *r;
	int fd;
	vlong g;

	if(m == nil)
		return -1;
	p = smprint("%s/gen", m->root);
	fd = open(p, OREAD);
	free(p);
	if(fd < 0)
		return -1;
	r = readall(fd);
	close(fd);
	if(r == nil)
		return -1;
	g = strtoll(r, nil, 10);
	free(r);
	return g;
}

static void
addattr(Fcrec *r, char *attr, char *val)
{
	if((r->na & 7) == 0)
		r->a = realloc(r->a, (r->na+8)*sizeof r->a[0]);
	r->a[r->na].attr = strdup(attr);
	r->a[r->na].val = strdup(val);
	r->na++;
}

/*
 * ndb text: a record starts at the left margin and continues on
 * indented lines; attr=value pairs, values may be "double quoted";
 * # starts a comment. A "# gen N" comment line sets *gen.
 */
Fcrec*
fcparserecs(char *text, vlong *gen)
{
	Fcrec *head, **tail, *cur;
	char *p, *e, *line, *a, *v;

	head = nil;
	tail = &head;
	cur = nil;
	for(line = text; line != nil && *line; line = e){
		e = strchr(line, '\n');
		if(e != nil)
			*e++ = 0;
		if(line[0] == '#'){
			if(gen != nil && strncmp(line, "# gen ", 6) == 0)
				*gen = strtoll(line+6, nil, 10);
			continue;
		}
		if(line[0] != ' ' && line[0] != '\t'){
			cur = nil;
			if(line[0] == 0)
				continue;
		}
		p = line;
		for(;;){
			while(*p == ' ' || *p == '\t')
				p++;
			if(*p == 0 || *p == '#')
				break;
			a = p;
			while(*p && *p != '=' && *p != ' ' && *p != '\t')
				p++;
			if(*p == '='){
				*p++ = 0;
				if(*p == '"'){
					v = ++p;
					while(*p && *p != '"')
						p++;
				}else{
					v = p;
					while(*p && *p != ' ' && *p != '\t')
						p++;
				}
				if(*p)
					*p++ = 0;
			}else{
				v = "";
				if(*p)
					*p++ = 0;
			}
			if(cur == nil){
				cur = mallocz(sizeof *cur, 1);
				*tail = cur;
				tail = &cur->next;
			}
			addattr(cur, a, v);
		}
	}
	return head;
}

char*
fcrecget(Fcrec *r, char *attr)
{
	int i;

	if(r == nil)
		return nil;
	for(i = 0; i < r->na; i++)
		if(strcmp(r->a[i].attr, attr) == 0)
			return r->a[i].val;
	return nil;
}

char*
fcreckey(Fcrec *r)
{
	if(r == nil || r->na == 0)
		return nil;
	return r->a[0].val;
}

void
fcrecfree(Fcrec *r)
{
	Fcrec *n;
	int i;

	for(; r != nil; r = n){
		n = r->next;
		for(i = 0; i < r->na; i++){
			free(r->a[i].attr);
			free(r->a[i].val);
		}
		free(r->a);
		free(r);
	}
}
