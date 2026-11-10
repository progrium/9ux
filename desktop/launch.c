/*
 * Opening documents through the plumber, and launching applications
 * in Desktop's own namespace and workspace.
 */
#include <u.h>
#include <libc.h>
#include <plumb.h>
#include "dcore.h"

static Dcrec*
resolve(Dcws *w, char *path)
{
	Dcrec *r;
	char *q;

	if(w == nil || w->meta == nil)
		return nil;
	q = quotestrdup(path);
	r = dcmetaquery(w->meta, "resolve %s", q);
	free(q);
	return r;
}

static Dcrec*
appreq(Dcws *w, char *app)
{
	Dcrec *r;
	char *q;

	if(w == nil || w->meta == nil)
		return nil;
	q = quotestrdup(app);
	r = dcmetaquery(w->meta, "app %s", q);
	free(q);
	return r;
}

static int
plumbit(Dcws *w, char *path, char *dst)
{
	Plumbmsg m;
	Plumbattr a;
	char *dir, err[ERRMAX];
	int fd, r;

	fd = plumbopen("send", OWRITE);
	if(fd < 0){
		werrstr("not found: the plumber isn't running");
		return -1;
	}
	dir = dcdirname(path);
	memset(&m, 0, sizeof m);
	m.src = "desktop";
	m.dst = dst ? dst : "";
	m.wdir = dir;
	m.type = "text";
	a.name = "workspace";
	a.value = w ? w->name : "default";
	a.next = nil;
	m.attr = &a;
	m.data = path;
	m.ndata = strlen(path);
	r = plumbsend(fd, &m);
	close(fd);
	free(dir);
	if(r < 0){
		rerrstr(err, sizeof err);
		werrstr("not found: nothing opens %s: %s", dcbasename(path), err);
		return -1;
	}
	return 0;
}

/*
 * Open path the way a double-click does. A folder is Desktop's own
 * business (Ofolder). A program (a file with an execute bit) that no
 * association claims is an application: it's launched (Olaunched), as
 * dclaunch does, in its own folder. Anything else goes to the plumber,
 * addressed to the port its association names, if any; otherwise the
 * plumbing rules uxmeta generated (and the user's own) decide. *dst
 * gets the port the message was addressed to ("" for none, or for a
 * launch).
 */
int
dcopen(Dcws *w, char *path, char **dst)
{
	Dir *d;
	Dcrec *r;
	char *port, *dir, err[ERRMAX];
	ulong mode;
	int pid;

	if(dst != nil)
		*dst = nil;
	d = dirstat(path);
	if(d == nil){
		rerrstr(err, sizeof err);
		werrstr("%s: %s: %s", dcerrname(dcerrclass(err)), path, err);
		return -1;
	}
	if(d->qid.type & QTDIR){
		free(d);
		return Ofolder;
	}
	mode = d->mode;
	free(d);
	r = resolve(w, path);
	if((mode & 0111) != 0 && dcrecget(r, "port") == nil
	&& dcrecget(r, "open") == nil && dcrecget(r, "exec") == nil){
		dcrecfree(r);
		dir = dcdirname(path);
		pid = dclaunch(w, path, dir, nil);
		free(dir);
		if(pid < 0)
			return -1;
		if(dst != nil)
			*dst = strdup("");
		return Olaunched;
	}
	port = dcrecget(r, "port");
	if(port == nil && dcrecget(r, "exec") == nil)
		port = dcrecget(r, "open");	/* open names a port, not an app */
	if(plumbit(w, path, port) < 0){
		dcrecfree(r);
		return -1;
	}
	if(dst != nil)
		*dst = strdup(port ? port : "");
	dcrecfree(r);
	return Oplumbed;
}

/* Open With: a particular application, by its app record */
int
dcopenwith(Dcws *w, char *path, char *app)
{
	Dcrec *r;
	char *port, *dir, *argv[2];
	int n;

	r = appreq(w, app);
	if(r == nil){
		werrstr("not found: no application %s", app);
		return -1;
	}
	port = dcrecget(r, "port");
	if(port != nil){
		n = plumbit(w, path, port);
		dcrecfree(r);
		return n < 0 ? -1 : Oplumbed;
	}
	dcrecfree(r);
	argv[0] = path;
	argv[1] = nil;
	dir = dcdirname(path);
	n = dclaunch(w, app, dir, argv);
	free(dir);
	if(n < 0)
		return -1;
	return Olaunched;
}

/*
 * Start an application: app names an app record (its exec is run) or
 * is an absolute command path. It runs in a copy of Desktop's
 * namespace and environment with $workspace set, in a new rio window
 * if Desktop has one ($wsys), started in dir. Returns the pid.
 */
int
dclaunch(Dcws *w, char *app, char *dir, char **argv)
{
	Dcrec *r;
	char *exec, *cmd, *q, *t, *wsys, *spec;
	int i, pid, fd;

	r = appreq(w, app);
	exec = dcrecget(r, "exec");
	if(exec == nil){
		if(app[0] != '/'){
			dcrecfree(r);
			werrstr("not found: no application %s", app);
			return -1;
		}
		exec = app;
	}
	/* a command line from an app record; a path, quoted as rc needs */
	cmd = exec == app ? quotestrdup(app) : strdup(exec);
	for(i = 0; argv != nil && argv[i] != nil; i++){
		q = quotestrdup(argv[i]);
		t = smprint("%s %s", cmd, q);
		free(q);
		free(cmd);
		cmd = t;
	}
	dcrecfree(r);
	if(dir == nil)
		dir = "/";

	switch(pid = rfork(RFPROC|RFNOWAIT|RFNAMEG|RFENVG|RFFDG|RFNOTEG)){
	case -1:
		free(cmd);
		werrstr("launch %s: %r", app);
		return -1;
	case 0:
		putenv("workspace", w ? w->name : "default");
		wsys = getenv("wsys");
		if(wsys != nil && (fd = open(wsys, ORDWR)) >= 0){
			spec = smprint("new -cd %s", dir);
			if(mount(fd, -1, "/mnt/wsys", MREPL, spec) >= 0){
				bind("/mnt/wsys", "/dev", MBEFORE);
				close(0);
				close(1);
				close(2);
				open("/dev/cons", OREAD);
				open("/dev/cons", OWRITE);
				dup(1, 2);
			}
			free(spec);
		}
		chdir(dir);
		execl("/bin/rc", "rc", "-c", cmd, nil);
		_exits("exec");
	}
	free(cmd);
	return pid;
}
