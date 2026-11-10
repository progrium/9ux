/*
 * Opening documents through the plumber, and launching applications
 * in Finder's own namespace and workspace.
 */
#include <u.h>
#include <libc.h>
#include <plumb.h>
#include "fcore.h"

static Fcrec*
resolve(Fcws *w, char *path)
{
	Fcrec *r;
	char *q;

	if(w == nil || w->meta == nil)
		return nil;
	q = quotestrdup(path);
	r = fcmetaquery(w->meta, "resolve %s", q);
	free(q);
	return r;
}

static Fcrec*
appreq(Fcws *w, char *app)
{
	Fcrec *r;
	char *q;

	if(w == nil || w->meta == nil)
		return nil;
	q = quotestrdup(app);
	r = fcmetaquery(w->meta, "app %s", q);
	free(q);
	return r;
}

static int
plumbit(Fcws *w, char *path, char *dst)
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
	dir = fcdirname(path);
	memset(&m, 0, sizeof m);
	m.src = "finder";
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
		werrstr("not found: nothing opens %s: %s", fcbasename(path), err);
		return -1;
	}
	return 0;
}

/*
 * Open path the way a double-click does. A folder is Finder's own
 * business (Ofolder). A program (a file with an execute bit) that no
 * association claims is an application: it's launched (Olaunched), as
 * fclaunch does, in its own folder. Anything else goes to the plumber,
 * addressed to the port its association names, if any; otherwise the
 * plumbing rules uxmeta generated (and the user's own) decide. *dst
 * gets the port the message was addressed to ("" for none, or for a
 * launch).
 */
int
fcopen(Fcws *w, char *path, char **dst)
{
	Dir *d;
	Fcrec *r;
	char *port, *dir, err[ERRMAX];
	ulong mode;
	int pid;

	if(dst != nil)
		*dst = nil;
	d = dirstat(path);
	if(d == nil){
		rerrstr(err, sizeof err);
		werrstr("%s: %s: %s", fcerrname(fcerrclass(err)), path, err);
		return -1;
	}
	if(d->qid.type & QTDIR){
		free(d);
		return Ofolder;
	}
	mode = d->mode;
	free(d);
	r = resolve(w, path);
	if((mode & 0111) != 0 && fcrecget(r, "port") == nil
	&& fcrecget(r, "open") == nil && fcrecget(r, "exec") == nil){
		fcrecfree(r);
		dir = fcdirname(path);
		pid = fclaunch(w, path, dir, nil);
		free(dir);
		if(pid < 0)
			return -1;
		if(dst != nil)
			*dst = strdup("");
		return Olaunched;
	}
	port = fcrecget(r, "port");
	if(port == nil && fcrecget(r, "exec") == nil)
		port = fcrecget(r, "open");	/* open names a port, not an app */
	if(plumbit(w, path, port) < 0){
		fcrecfree(r);
		return -1;
	}
	if(dst != nil)
		*dst = strdup(port ? port : "");
	fcrecfree(r);
	return Oplumbed;
}

/* Open With: a particular application, by its app record */
int
fcopenwith(Fcws *w, char *path, char *app)
{
	Fcrec *r;
	char *port, *dir, *argv[2];
	int n;

	r = appreq(w, app);
	if(r == nil){
		werrstr("not found: no application %s", app);
		return -1;
	}
	port = fcrecget(r, "port");
	if(port != nil){
		n = plumbit(w, path, port);
		fcrecfree(r);
		return n < 0 ? -1 : Oplumbed;
	}
	fcrecfree(r);
	argv[0] = path;
	argv[1] = nil;
	dir = fcdirname(path);
	n = fclaunch(w, app, dir, argv);
	free(dir);
	if(n < 0)
		return -1;
	return Olaunched;
}

/*
 * Start an application: app names an app record (its exec is run) or
 * is an absolute command path. It runs in a copy of Finder's
 * namespace and environment with $workspace set, in a new rio window
 * if Finder has one ($wsys), started in dir. Returns the pid.
 */
int
fclaunch(Fcws *w, char *app, char *dir, char **argv)
{
	Fcrec *r;
	char *exec, *cmd, *q, *t, *wsys, *spec;
	int i, pid, fd;

	r = appreq(w, app);
	exec = fcrecget(r, "exec");
	if(exec == nil){
		if(app[0] != '/'){
			fcrecfree(r);
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
	fcrecfree(r);
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
