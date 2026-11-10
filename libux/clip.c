/*
 * The clipboard: clipd's /mnt/clip when it is there
 * (docs/protocols/clip.md), rio's /dev/snarf otherwise. $uxclip and
 * $uxsnarf move them, for tests.
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

static char*
where(char *env, char *def)
{
	char *s;

	s = getenv(env);
	if(s != nil && *s != 0)
		return s;
	free(s);
	return estrdup(def);
}

/* the clip root if clipd is mounted there, else nil; malloc'd */
static char*
clipdir(void)
{
	char *d, *v, buf[16];
	int fd, n;

	d = where("uxclip", "/mnt/clip");
	v = smprint("%s/version", d);
	fd = open(v, OREAD);
	free(v);
	if(fd < 0){
		free(d);
		return nil;
	}
	n = read(fd, buf, sizeof buf-1);
	close(fd);
	if(n < 5 || strncmp(buf, "clip ", 5) != 0){
		free(d);
		return nil;
	}
	return d;
}

static char*
readall(char *path, long *np)
{
	char *buf;
	long n, m;
	int fd;

	if(np != nil)
		*np = 0;
	fd = open(path, OREAD);
	if(fd < 0)
		return nil;
	buf = emalloc(8192+1);
	n = 0;
	while((m = read(fd, buf+n, 8192)) > 0){
		n += m;
		buf = erealloc(buf, n+8192+1);
	}
	close(fd);
	if(m < 0){
		free(buf);
		return nil;
	}
	buf[n] = 0;
	if(np != nil)
		*np = n;
	return buf;
}

static int
writeall(char *path, void *data, long n)
{
	int fd;
	long k, m;

	fd = create(path, OWRITE, 0666);
	if(fd < 0)
		return -1;
	for(k = 0; k < n; k += m){
		m = n-k > 8192 ? 8192 : n-k;
		if(write(fd, (char*)data+k, m) != m){
			close(fd);
			return -1;
		}
	}
	close(fd);
	return 0;
}

char*
uxclipget(char *type, long *np)
{
	char *d, *p, *s;

	if(np != nil)
		*np = 0;
	d = clipdir();
	if(d != nil){
		p = smprint("%s/data/%s", d, type);
		s = readall(p, np);
		free(p);
		free(d);
		return s;
	}
	if(strcmp(type, "text") != 0){
		werrstr("not found: no clipd, so only text");
		return nil;
	}
	p = where("uxsnarf", "/dev/snarf");
	s = readall(p, np);
	free(p);
	return s;
}

int
uxclipputn(char **types, char **data, long *n, int ntypes)
{
	char *d, *p, *ctl;
	int i, fd, r;

	d = clipdir();
	if(d == nil){
		for(i = 0; i < ntypes; i++)
			if(strcmp(types[i], "text") == 0){
				p = where("uxsnarf", "/dev/snarf");
				r = writeall(p, data[i], n[i]);
				free(p);
				return r;
			}
		werrstr("not found: no clipd, so only text");
		return -1;
	}
	ctl = smprint("%s/ctl", d);
	fd = -1;
	if(ntypes > 1 && (fd = open(ctl, OWRITE)) >= 0 && write(fd, "begin\n", 6) != 6){
		close(fd);
		fd = -1;
	}
	r = 0;
	for(i = 0; i < ntypes && r == 0; i++){
		p = smprint("%s/data/%s", d, types[i]);
		r = writeall(p, data[i], n[i]);
		free(p);
	}
	if(fd >= 0){
		if(r == 0 && write(fd, "commit\n", 7) != 7)
			r = -1;
		close(fd);	/* aborts if not committed */
	}
	free(ctl);
	free(d);
	return r;
}

int
uxclipput(char *type, void *data, long n)
{
	return uxclipputn(&type, (char**)&data, &n, 1);
}

char**
uxcliptypes(int *np)
{
	char *d, *p, *s, *line, *e, *f[4];
	char **t;
	int n;

	*np = 0;
	d = clipdir();
	if(d == nil){
		s = uxclipget("text", nil);
		if(s == nil || *s == 0){
			free(s);
			return nil;
		}
		free(s);
		t = emalloc(2*sizeof t[0]);
		t[0] = estrdup("text");
		*np = 1;
		return t;
	}
	p = smprint("%s/types", d);
	s = readall(p, nil);
	free(p);
	free(d);
	if(s == nil)
		return nil;
	t = nil;
	n = 0;
	for(line = s; (e = strchr(line, '\n')) != nil; line = e+1){
		*e = 0;
		if(tokenize(line, f, nelem(f)) >= 2 && strcmp(f[0], "type") == 0){
			t = erealloc(t, (n+2)*sizeof t[0]);
			t[n++] = estrdup(f[1]);
		}
	}
	free(s);
	*np = n;
	return t;
}

/* for text fields (ctl.c); uxlk held, which nothing here needs */
long
_clipput(char *s)
{
	return uxclipput("text", s, strlen(s));
}

char*
_clipget(void)
{
	return uxclipget("text", nil);
}

int
uxedit(char *cmd)
{
	Uxwin *w;
	char *err;

	qlock(&uxlk);
	w = ux.front;
	if(w == nil && ux.nwin > 0)
		w = ux.win[0];
	err = _tfedit(w != nil ? w->focus : nil, cmd);
	qunlock(&uxlk);
	if(err != nil){
		werrstr("%s", err);
		return -1;
	}
	return 0;
}
