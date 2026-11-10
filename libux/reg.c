/*
 * Publishing the tree: registration with uxd (ux.md, Registration with
 * uxd), or a public /srv entry for tests (uxpublic=1).
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

/* send a line to uxd on the registration fid; uxlk held */
static void
reg(char *fmt, ...)
{
	va_list arg;
	char *s;

	if(ux.regfd < 0)
		return;
	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	if(write(ux.regfd, s, strlen(s)) < 0)
		fprint(2, "libux: uxd: %s: %r\n", s);
	free(s);
}

void
_regwin(int winid, int add)
{
	reg(add ? "window %d\n" : "unwindow %d\n", winid);
}

static int
mountuxd(void)
{
	char *s;
	int fd;

	if(access("/mnt/ux/register", AEXIST) == 0)
		return 0;
	s = smprint("/srv/uxd.%s", ux.user);
	fd = open(s, ORDWR);
	free(s);
	if(fd < 0)
		return -1;
	if(mount(fd, -1, "/mnt/ux", MREPL, "") < 0){
		close(fd);
		return -1;
	}
	return 0;
}

static int
register1(void)
{
	char name[128], path[160], buf[128], *t, *id;
	Fmt f;
	int fd, sfd, n, i;
	ulong n0, n1;

	n0 = truerand();
	n1 = truerand();
	snprint(name, sizeof name, "ux.%s.%d.%08lux%08lux", ux.user, getpid(), n0, n1);
	snprint(path, sizeof path, "/srv/%s", name);
	fd = open("/mnt/ux/register", ORDWR);
	if(fd < 0)
		return -1;
	sfd = _newsrv(name);
	if(sfd < 0){
		close(fd);
		return -1;
	}
	qlock(&uxlk);
	fmtstrinit(&f);
	fmtprint(&f, "register name=%s srv=%s", ux.name, path);
	t = _attr("title", ux.title);
	fmtprint(&f, " %s", t);
	free(t);
	if(ux.id != nil)
		fmtprint(&f, " id=%s", ux.id);
	for(i = 0; i < ux.nwin; i++)
		fmtprint(&f, " win=%d", ux.win[i]->winid);
	fmtprint(&f, "\n");
	t = fmtstrflush(&f);
	qunlock(&uxlk);
	n = write(fd, t, strlen(t));
	free(t);
	close(sfd);	/* uxd has taken it, or it is no use */
	if(n < 0){
		fprint(2, "libux: register: %r\n");
		close(fd);
		return -1;
	}
	n = read(fd, buf, sizeof buf-1);
	if(n <= 0){
		close(fd);
		return -1;
	}
	buf[n] = 0;
	if((t = strchr(buf, '\n')) != nil)
		*t = 0;
	id = estrdup(buf);
	qlock(&uxlk);
	if(ux.id == nil || strcmp(ux.id, id) != 0){
		free(ux.id);
		ux.id = id;
		ux.rebar = 1;
		nbsendul(ux.uikick, 1);
	}else
		free(id);
	ux.regfd = fd;
	/* windows made while registering */
	qunlock(&uxlk);
	/* blocks until the registration ends */
	read(fd, buf, sizeof buf);
	qlock(&uxlk);
	ux.regfd = -1;
	qunlock(&uxlk);
	close(fd);
	return 0;
}

static void
regproc(void*)
{
	int wait;

	threadsetname("uxregister");
	wait = 250;
	for(;;){
		if(mountuxd() == 0 && register1() == 0)
			wait = 250;
		sleep(wait);
		wait *= 2;
		if(wait > 1000)
			wait = 1000;
	}
}

void
_register(void)
{
	char *s, name[128];

	s = getenv("uxpublic");
	if(s != nil && strcmp(s, "1") == 0){
		free(s);
		snprint(name, sizeof name, "ux.%s.%s", ux.user, ux.name);
		/* the entry stays while the app runs */
		if(_newsrv(name) < 0)
			fprint(2, "libux: can't post /srv/%s: %r\n", name);
		return;
	}
	free(s);
	if(access("/mnt/ux/register", AEXIST) == 0 || mountuxd() == 0)
		proccreate(regproc, nil, 16*1024);
}
