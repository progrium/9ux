/*
 * The volume registry and the workspace context.
 *
 * A volume is a named mount point declared in uxmeta's volume records
 * (or added by the caller); Desktop never infers one from the namespace.
 * Its state is checked by stat, and its identity (server type, device,
 * root qid) is remembered while online, so a volume whose server goes
 * away, or whose mount is replaced underneath, shows as offline instead
 * of silently showing whatever is now at the path.
 */
#include <u.h>
#include <libc.h>
#include <auth.h>
#include "dcore.h"

Dcws*
dcwsopen(char *name, Dcmeta *m)
{
	Dcws *w;

	w = mallocz(sizeof *w, 1);
	if(name == nil)
		name = getenv("workspace");
	else
		name = strdup(name);
	if(name == nil || *name == 0){
		free(name);
		name = strdup("default");
	}
	w->name = name;
	w->meta = m;
	w->gen = -1;
	if(m != nil && dcwsload(w) < 0){
		dcwsclose(w);
		return nil;
	}
	return w;
}

static void
volfree(Dcvol *v)
{
	free(v->name);
	free(v->path);
	free(v->icon);
	free(v->addr);
	free(v->srv);
	free(v->spec);
	free(v);
}

void
dcwsclose(Dcws *w)
{
	int i;

	if(w == nil)
		return;
	for(i = 0; i < w->nvol; i++)
		volfree(w->vol[i]);
	free(w->vol);
	free(w->name);
	free(w);
}

static Dcvol*
newvol(Dcws *w, char *name)
{
	Dcvol *v;

	v = mallocz(sizeof *v, 1);
	v->name = strdup(name);
	v->state = Vunknown;
	w->vol = realloc(w->vol, (w->nvol+1)*sizeof w->vol[0]);
	w->vol[w->nvol++] = v;
	return v;
}

Dcvol*
dcvolbyname(Dcws *w, char *name)
{
	int i;

	for(i = 0; i < w->nvol; i++)
		if(strcmp(w->vol[i]->name, name) == 0)
			return w->vol[i];
	return nil;
}

/* replace *s with t (copied, or nil); return whether it changed */
static int
setstr(char **s, char *t)
{
	if(*s == nil && t == nil)
		return 0;
	if(*s != nil && t != nil && strcmp(*s, t) == 0)
		return 0;
	free(*s);
	*s = t ? strdup(t) : nil;
	return 1;
}

static char*
cleanpath(char *p)
{
	char *s;

	s = strdup(p);
	cleanname(s);
	return s;
}

/*
 * (Re)load volume records from uxmeta. Volumes keep their state and
 * learned capabilities across reloads unless their path or address
 * changed.
 */
int
dcwsload(Dcws *w)
{
	Dcrec *recs, *r;
	Dcvol *v;
	char *path, *s;
	int i, j, moved;

	if(w->meta == nil)
		return 0;
	recs = dcmetaquery(w->meta, "volume");
	if(recs == nil){
		char err[ERRMAX];

		rerrstr(err, sizeof err);
		if(strncmp(err, "not found", 9) != 0)
			return -1;
	}
	for(i = 0; i < w->nvol; i++)
		w->vol[i]->seen = 0;
	for(r = recs; r != nil; r = r->next){
		if(strcmp(r->a[0].attr, "volume") != 0)
			continue;
		s = dcrecget(r, "path");
		if(s == nil || s[0] != '/')
			continue;	/* a volume needs an absolute mount point */
		path = cleanpath(s);
		v = dcvolbyname(w, dcreckey(r));
		if(v == nil)
			v = newvol(w, dcreckey(r));
		moved = setstr(&v->path, path);
		moved |= setstr(&v->addr, dcrecget(r, "addr"));
		moved |= setstr(&v->srv, dcrecget(r, "srv"));
		moved |= setstr(&v->spec, dcrecget(r, "spec"));
		free(path);
		if(moved){
			v->state = Vunknown;
			v->haveid = 0;
			v->err[0] = 0;
			memset(&v->caps, 0, sizeof v->caps);
		}
		setstr(&v->icon, dcrecget(r, "icon"));
		s = dcrecget(r, "desktop");
		v->desktop = s != nil && (strcmp(s, "yes") == 0 || strcmp(s, "on") == 0);
		s = dcrecget(r, "x");
		v->x = s ? atoi(s) : 0;
		s = dcrecget(r, "y");
		v->y = s ? atoi(s) : 0;
		v->seen = 1;
	}
	w->gen = w->meta->gen;
	dcrecfree(recs);
	for(i = j = 0; i < w->nvol; i++){
		if(w->vol[i]->seen)
			w->vol[j++] = w->vol[i];
		else
			volfree(w->vol[i]);
	}
	w->nvol = j;
	return 0;
}

Dcvol*
dcwsaddvol(Dcws *w, char *name, char *path)
{
	Dcvol *v;

	v = dcvolbyname(w, name);
	if(v == nil)
		v = newvol(w, name);
	free(v->path);
	v->path = cleanpath(path);
	v->state = Vunknown;
	v->haveid = 0;
	v->seen = 1;
	return v;
}

/* the volume whose mount point is the longest prefix of path */
Dcvol*
dcvolof(Dcws *w, char *path)
{
	Dcvol *v, *best;
	int i, n, bn;

	best = nil;
	bn = -1;
	for(i = 0; i < w->nvol; i++){
		v = w->vol[i];
		n = strlen(v->path);
		if(strncmp(path, v->path, n) != 0)
			continue;
		if(path[n] != 0 && path[n] != '/' && strcmp(v->path, "/") != 0)
			continue;
		if(n > bn){
			best = v;
			bn = n;
		}
	}
	return best;
}

char*
dcvolstate(int state)
{
	switch(state){
	case Vonline:	return "online";
	case Voffline:	return "offline";
	}
	return "unknown";
}

static int
offline(Dcvol *v, char *fmt, ...)
{
	va_list arg;

	va_start(arg, fmt);
	vsnprint(v->err, sizeof v->err, fmt, arg);
	va_end(arg);
	v->state = Voffline;
	werrstr("%s", v->err);
	return v->state;
}

int
dcvolcheck(Dcvol *v)
{
	char err[ERRMAX];
	Dir *d;
	int fd;

	d = dirstat(v->path);
	if(d == nil){
		rerrstr(err, sizeof err);
		if(dcerrclass(err) == Egone)
			return offline(v, "gone: %s: %s", v->name, err);
		return offline(v, "%s: %s: %s", dcerrname(dcerrclass(err)), v->name, err);
	}
	if((d->qid.type & QTDIR) == 0){
		free(d);
		return offline(v, "bad value: %s: %s is not a directory", v->name, v->path);
	}
	if(v->haveid && (d->type != v->type || d->dev != v->dev)){
		free(d);
		return offline(v, "gone: %s: no longer mounted at %s", v->name, v->path);
	}
	/* stat can be answered from a cache; reading can't */
	fd = open(v->path, OREAD);
	if(fd < 0){
		free(d);
		rerrstr(err, sizeof err);
		if(dcerrclass(err) == Egone)
			return offline(v, "gone: %s: %s", v->name, err);
		return offline(v, "%s: %s: %s", dcerrname(dcerrclass(err)), v->name, err);
	}
	close(fd);
	if(!v->haveid){
		v->haveid = 1;
		v->type = d->type;
		v->dev = d->dev;
		v->qid = d->qid;
	}
	free(d);
	v->state = Vonline;
	v->err[0] = 0;
	return v->state;
}

/*
 * Mount a volume that declares addr or srv at its path. Volumes
 * without either are expected to be in the namespace already.
 */
int
dcvolattach(Dcvol *v)
{
	char err[ERRMAX];
	Dir *d;
	int fd, mfd;

	if(v->addr == nil && v->srv == nil){
		if(dcvolcheck(v) != Vonline)
			return -1;
		return 0;
	}
	if(v->attached){
		unmount(nil, v->path);
		v->attached = 0;
	}
	if(v->srv != nil)
		fd = open(v->srv, ORDWR);
	else
		fd = dial(netmkaddr(v->addr, "tcp", "9fs"), nil, nil, nil);
	if(fd < 0){
		rerrstr(err, sizeof err);
		offline(v, "gone: %s: can't reach %s: %s", v->name, v->srv ? v->srv : v->addr, err);
		return -1;
	}
	d = dirstat(v->path);
	if(d == nil){
		mfd = create(v->path, OREAD, DMDIR|0755);
		if(mfd >= 0)
			close(mfd);
	}
	free(d);
	if(amount(fd, v->path, MREPL|MCREATE, v->spec ? v->spec : "") < 0){
		rerrstr(err, sizeof err);
		close(fd);
		offline(v, "%s: %s: mount: %s", dcerrname(dcerrclass(err)), v->name, err);
		return -1;
	}
	close(fd);
	v->attached = 1;
	v->haveid = 0;
	memset(&v->caps, 0, sizeof v->caps);
	if(dcvolcheck(v) != Vonline)
		return -1;
	return 0;
}

int
dcvoldetach(Dcvol *v)
{
	if(!v->attached){
		werrstr("bad value: %s wasn't attached by Desktop", v->name);
		return -1;
	}
	unmount(nil, v->path);
	v->attached = 0;
	v->haveid = 0;
	v->state = Vunknown;
	return 0;
}

/*
 * Turn a command-line name into a path: "VOL:rel/path" names a path
 * on volume VOL; anything else is used as it is.
 */
char*
dcresolve(Dcws *w, char *arg)
{
	char *c, *name, *p;
	Dcvol *v;

	if(arg[0] != '/' && (c = strchr(arg, ':')) != nil){
		name = mallocz(c-arg+1, 1);
		memmove(name, arg, c-arg);
		v = dcvolbyname(w, name);
		free(name);
		if(v != nil){
			p = dcjoin(v->path, c+1);
			cleanname(p);
			return p;
		}
	}
	p = strdup(arg);
	cleanname(p);
	return p;
}
