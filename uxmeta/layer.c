/*
 * Layer files: reading, noticing outside edits, atomic replacement.
 */
#include <u.h>
#include <libc.h>
#include "meta.h"

static char Trailer[] = "#uxmeta end\n";

static char *appkinds[] = { "app", "type", "ext", "icon", nil };

int
layeradmits(Layer *l, char *kind)
{
	char **k;

	if(l->class != Lapps)
		return 1;
	for(k = appkinds; *k; k++)
		if(strcmp(*k, kind) == 0)
			return 1;
	return 0;
}

/* read a whole file as a string; nil and errstr on failure */
char*
readfile(char *path, int *len)
{
	int fd;
	long n, m;
	char *buf;
	Dir *d;

	if((fd = open(path, OREAD)) < 0)
		return nil;
	if((d = dirfstat(fd)) == nil){
		close(fd);
		return nil;
	}
	if(d->length > Maxfile){
		free(d);
		close(fd);
		werrstr("file too large");
		return nil;
	}
	n = d->length;
	free(d);
	buf = emalloc(n+1);
	m = 0;
	for(;;){
		if(m == n){
			/* grew since the stat, or a length-0 synthetic file */
			if(n >= Maxfile)
				break;
			n = n ? 2*n : 8192;
			if(n > Maxfile)
				n = Maxfile;
			buf = erealloc(buf, n+1);
		}
		long r = read(fd, buf+m, n-m);
		if(r < 0){
			free(buf);
			close(fd);
			return nil;
		}
		if(r == 0)
			break;
		m += r;
	}
	close(fd);
	buf[m] = 0;
	if(len)
		*len = m;
	return buf;
}

static void
striptrailer(char *s)
{
	int n, t;

	n = strlen(s);
	t = strlen(Trailer);
	if(n >= t && strcmp(s+n-t, Trailer) == 0)
		s[n-t] = 0;
}

static int
endstrailer(char *path)
{
	char *s;
	int n, t, ok;

	if((s = readfile(path, &n)) == nil)
		return 0;
	t = strlen(Trailer);
	ok = n >= t && strcmp(s+n-t, Trailer) == 0;
	free(s);
	return ok;
}

static void
kindlist(Layer *l)
{
	Fmt f;
	int i, j, n;
	char *k;

	n = 0;
	fmtstrinit(&f);
	for(i = 0; i < l->f.nc; i++){
		if(l->f.c[i].rec == nil)
			continue;
		k = l->f.c[i].rec->t[0].attr;
		if(layeradmits(l, k))
			continue;
		for(j = 0; j < i; j++)
			if(l->f.c[j].rec != nil && strcmp(l->f.c[j].rec->t[0].attr, k) == 0)
				break;
		if(j < i)
			continue;
		fmtprint(&f, "%s%s", n++ ? " " : "ignored kinds: ", k);
	}
	free(l->err);
	l->err = fmtstrflush(&f);
	if(n == 0){
		free(l->err);
		l->err = nil;
	}
}

/*
 * (Re)read a layer's file.  A file that can't be read keeps its last
 * good contents and reports the error.
 */
int
layerload(Layer *l)
{
	Dir *d;
	char *s;
	char err[ERRMAX];

	if((d = dirstat(l->path)) == nil){
		nfilefree(&l->f);
		free(l->text);
		l->text = estrdup("");
		l->present = 0;
		memset(&l->qid, 0, sizeof l->qid);
		l->length = 0;
		l->mtime = 0;
		free(l->err);
		l->err = nil;
		return 0;
	}
	if((s = readfile(l->path, nil)) == nil){
		rerrstr(err, sizeof err);
		free(l->err);
		l->err = estrdup(err);
		l->qid = d->qid;
		l->length = d->length;
		l->mtime = d->mtime;
		free(d);
		return -1;
	}
	striptrailer(s);
	nfilefree(&l->f);
	ndbparse(s, &l->f);
	free(l->text);
	l->text = s;
	l->present = 1;
	l->qid = d->qid;
	l->length = d->length;
	l->mtime = d->mtime;
	l->dirty = 0;
	free(d);
	free(l->err);
	l->err = nil;
	kindlist(l);
	return 0;
}

int
layerchanged(Layer *l)
{
	Dir *d;
	int ch;

	if((d = dirstat(l->path)) == nil)
		return l->present;
	ch = !l->present || d->qid.path != l->qid.path || d->qid.vers != l->qid.vers
		|| d->length != l->length || d->mtime != l->mtime;
	free(d);
	return ch;
}

static char*
basename(char *p)
{
	char *s;

	s = strrchr(p, '/');
	return s ? s+1 : p;
}

int
mkdirs(char *path)
{
	char *p, *s;
	int fd;
	Dir *d;

	if((d = dirstat(path)) != nil){
		free(d);
		return 0;
	}
	p = estrdup(path);
	for(s = p+1; ; s++){
		if(*s == '/' || *s == 0){
			char c = *s;
			*s = 0;
			if((d = dirstat(p)) == nil){
				if((fd = create(p, OREAD, DMDIR|0775)) < 0){
					free(p);
					return -1;
				}
				close(fd);
			}else
				free(d);
			*s = c;
			if(c == 0)
				break;
		}
	}
	free(p);
	return 0;
}

static int
renameto(char *path, char *name)
{
	Dir d;

	nulldir(&d);
	d.name = name;
	return dirwstat(path, &d);
}

/*
 * Replace path with text: write path.new (ending with the trailer),
 * move path to path.old, move path.new to path.  Plan 9 can't rename
 * over an existing file, so this is the closest to atomic there is;
 * metarecover finishes an interrupted replacement.
 */
int
atomicwrite(char *path, char *text)
{
	char *new, *old, *dir;
	int fd, n, mode;
	Dir *d;

	dir = estrdup(path);
	if(strrchr(dir, '/') != nil){
		*strrchr(dir, '/') = 0;
		if(*dir && mkdirs(dir) < 0){
			free(dir);
			return -1;
		}
	}
	free(dir);
	mode = 0644;
	if((d = dirstat(path)) != nil){
		mode = d->mode & 0777;
		free(d);
	}
	new = smprint("%s.new", path);
	old = smprint("%s.old", path);
	if((fd = create(new, OWRITE, mode)) < 0)
		goto Err;
	n = strlen(text);
	if(write(fd, text, n) != n || write(fd, Trailer, strlen(Trailer)) != strlen(Trailer)){
		close(fd);
		remove(new);
		goto Err;
	}
	close(fd);
	remove(old);
	if((d = dirstat(path)) != nil){
		free(d);
		if(renameto(path, basename(old)) < 0){
			remove(new);
			goto Err;
		}
	}
	if(renameto(new, basename(path)) < 0){
		char err[ERRMAX];
		rerrstr(err, sizeof err);
		renameto(old, basename(path));
		werrstr("%s", err);
		goto Err;
	}
	free(new);
	free(old);
	return 0;
Err:
	free(new);
	free(old);
	return -1;
}

/* finish a replacement that was interrupted */
void
metarecover(char *path)
{
	char *new, *old;
	Dir *d;

	if((d = dirstat(path)) != nil){
		free(d);
		return;
	}
	new = smprint("%s.new", path);
	old = smprint("%s.old", path);
	if(endstrailer(new))
		renameto(new, basename(path));
	else if((d = dirstat(old)) != nil){
		free(d);
		renameto(old, basename(path));
	}
	free(new);
	free(old);
}

int
layersave(Layer *l)
{
	char *s;
	Dir *d;

	s = nfiletext(&l->f);
	if(atomicwrite(l->path, s) < 0){
		free(s);
		return -1;
	}
	nfilefree(&l->f);
	ndbparse(s, &l->f);
	free(l->text);
	l->text = s;
	l->present = 1;
	l->dirty = 0;
	if((d = dirstat(l->path)) != nil){
		l->qid = d->qid;
		l->length = d->length;
		l->mtime = d->mtime;
		free(d);
	}
	kindlist(l);
	return 0;
}

static Layer*
newlayer(char *name, char *path, int class)
{
	Layer *l;

	l = emalloc(sizeof *l);
	l->name = estrdup(name);
	l->path = estrdup(path);
	l->class = class;
	l->writable = class == Luser || class == Lworkspace;
	l->text = estrdup("");
	if(l->writable)
		metarecover(path);
	layerload(l);
	return l;
}

static void
freelayer(Layer *l)
{
	nfilefree(&l->f);
	free(l->name);
	free(l->path);
	free(l->text);
	free(l->err);
	free(l);
}

static int
dircmp(void *a, void *b)
{
	return strcmp(((Dir*)a)->name, ((Dir*)b)->name);
}

typedef struct Want Want;
struct Want {
	char	*name;
	char	*path;
	int	class;
};

static void
want(Want **w, int *n, char *name, char *path, int class)
{
	*w = erealloc(*w, (*n+1)*sizeof **w);
	(*w)[*n].name = name;
	(*w)[*n].path = path;
	(*w)[*n].class = class;
	(*n)++;
}

static void
listdir(char *dir, Want **w, int *nw, int class)
{
	int fd, i, n, len;
	Dir *d, *sd;
	char *p;

	if(dir == nil || (fd = open(dir, OREAD)) < 0)
		return;
	n = dirreadall(fd, &d);
	close(fd);
	if(n <= 0)
		return;
	qsort(d, n, sizeof d[0], dircmp);
	for(i = 0; i < n; i++){
		if(class == Lsystem){
			len = strlen(d[i].name);
			if(len <= 4 || strcmp(d[i].name+len-4, ".ndb") != 0 || (d[i].mode & DMDIR))
				continue;
			want(w, nw, smprint("system/%.*s", len-4, d[i].name),
				smprint("%s/%s", dir, d[i].name), class);
		}else{
			if(!(d[i].mode & DMDIR))
				continue;
			p = smprint("%s/%s/app.ndb", dir, d[i].name);
			if((sd = dirstat(p)) == nil){
				free(p);
				continue;
			}
			free(sd);
			want(w, nw, smprint("apps/%s", d[i].name), p, class);
		}
	}
	free(d);
}

/*
 * Bring the layer list in line with the files: pick up new and removed
 * system and app files, and re-read any file that changed.  Returns the
 * number of layers that changed.
 */
int
metarescan(Meta *m)
{
	Want *w;
	int nw, i, j, changed;
	Layer **nl, *l;

	w = nil;
	nw = 0;
	listdir(m->sysdir, &w, &nw, Lsystem);
	listdir(m->appsdir, &w, &nw, Lapps);
	if(m->userfile)
		want(&w, &nw, estrdup("user"), estrdup(m->userfile), Luser);
	if(m->wsfile)
		want(&w, &nw, estrdup("workspace"), estrdup(m->wsfile), Lworkspace);

	changed = 0;
	for(i = 0; i < m->nl; i++)
		m->l[i]->seen = 0;
	nl = emalloc((nw+1)*sizeof nl[0]);
	for(i = 0; i < nw; i++){
		l = nil;
		for(j = 0; j < m->nl; j++)
			if(!m->l[j]->seen && m->l[j]->class == w[i].class && strcmp(m->l[j]->path, w[i].path) == 0){
				l = m->l[j];
				break;
			}
		if(l == nil){
			l = newlayer(w[i].name, w[i].path, w[i].class);
			changed++;
		}else if(layerchanged(l)){
			layerload(l);
			changed++;
		}
		l->seen = 1;
		nl[i] = l;
		free(w[i].name);
		free(w[i].path);
	}
	for(j = 0; j < m->nl; j++)
		if(!m->l[j]->seen){
			freelayer(m->l[j]);
			changed++;
		}
	free(w);
	free(m->l);
	m->l = nl;
	m->nl = nw;
	return changed;
}

Layer*
metalayer(Meta *m, char *name)
{
	int i;

	for(i = 0; i < m->nl; i++)
		if(strcmp(m->l[i]->name, name) == 0)
			return m->l[i];
	return nil;
}

/* where an update of this kind goes when it names no layer */
Layer*
metadefault(Meta *m, char *kind)
{
	Layer *l;

	if(strcmp(kind, "folder") == 0 || strcmp(kind, "item") == 0)
		if((l = metalayer(m, "workspace")) != nil)
			return l;
	return metalayer(m, "user");
}

char*
metalayers(Meta *m)
{
	Fmt f;
	int i;
	Layer *l;
	char *state;

	fmtstrinit(&f);
	for(i = 0; i < m->nl; i++){
		l = m->l[i];
		if(l->err != nil && !l->present)
			state = "error";
		else if(l->err != nil && strncmp(l->err, "ignored", 7) != 0)
			state = "error";
		else if(!l->present)
			state = "missing";
		else if(l->writable)
			state = "rw";
		else
			state = "ok";
		fmtprint(&f, "%q %q %s", l->name, l->path, state);
		if(l->err)
			fmtprint(&f, " %q", l->err);
		fmtprint(&f, "\n");
	}
	return fmtstrflush(&f);
}

/*
 * Hold the single-writer lock for the life of the process: an
 * exclusive-use (DMEXCL) file in the user directory, which file servers
 * that support DMEXCL enforce across machines, and a /srv entry named
 * after the directory, removed by the kernel when this process exits,
 * which works on file servers that ignore DMEXCL.  Returns -1 if
 * another uxmeta holds either.
 */
int
metalock(char *path)
{
	int fd, sfd;
	uint h;
	uchar *p;
	char *srv, err[ERRMAX];

	fd = create(path, ORDWR, DMEXCL|0600);
	if(fd < 0)
		return -1;
	h = 5381;
	for(p = (uchar*)path; *p; p++)
		h = h*33 + *p;
	srv = smprint("/srv/uxmeta.lock.%s.%08ux", getuser(), h);
	sfd = create(srv, OWRITE|ORCLOSE, 0600);
	if(sfd < 0){
		rerrstr(err, sizeof err);
		if(strstr(err, "exist") != nil){
			close(fd);
			werrstr("%s exists", srv);
			free(srv);
			return -1;
		}
	}
	free(srv);
	return fd;
}

void
metaloadgen(Meta *m)
{
	char *s;

	m->gen = 0;
	if(m->genfile == nil || (s = readfile(m->genfile, nil)) == nil)
		return;
	m->gen = strtoull(s, nil, 10);
	free(s);
}

void
metasavegen(Meta *m)
{
	int fd;

	if(m->genfile == nil)
		return;
	if((fd = create(m->genfile, OWRITE, 0644)) < 0)
		return;
	fprint(fd, "%llud\n", m->gen);
	close(fd);
}
