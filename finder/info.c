/*
 * Get Info: what Finder shows about one file, folder or volume.
 */
#include <u.h>
#include <libc.h>
#include "fcore.h"

enum { Deeplimit = 100000 };

/* add up a folder's contents, staying on its own server */
static int
walk(char *path, Dir *pd, vlong *size, long *items)
{
	Dir *d, *sd;
	long i, n;
	int fd;
	char *c;

	fd = open(path, OREAD);
	if(fd < 0)
		return 0;
	n = dirreadall(fd, &d);
	close(fd);
	if(n < 0)
		return 0;
	for(i = 0; i < n; i++){
		if(++*items > Deeplimit){
			free(d);
			return -1;
		}
		*size += d[i].length;
		if((d[i].qid.type & QTDIR) == 0)
			continue;
		c = fcjoin(path, d[i].name);
		sd = dirstat(c);
		if(sd != nil && sd->type == pd->type && sd->dev == pd->dev)
		if(walk(c, sd, size, items) < 0){
			free(sd);
			free(c);
			free(d);
			return -1;
		}
		free(sd);
		free(c);
	}
	free(d);
	return 0;
}

/*
 * Whether the user can change it. A file is opened for writing (which
 * doesn't change it); a directory can't be, so its permission bits are
 * read the way most servers apply them, which is only a guide: group
 * membership isn't visible from here, and the server decides.
 */
static int
canwrite(char *path, Dir *d)
{
	char *u;

	if((d->qid.type & QTDIR) == 0)
		return access(path, AWRITE) == 0;
	u = getuser();
	if(strcmp(d->uid, u) == 0)
		return (d->mode & 0200) != 0;
	if(strcmp(d->gid, u) == 0)
		return (d->mode & 0020) != 0;
	return (d->mode & 0002) != 0;
}

Fcinfo*
fcinfo(Fcws *w, char *path, int deep)
{
	Fcinfo *fi;
	Dir *d, *cd;
	char *p, *s;
	int fd;
	long n;

	p = strdup(path);
	cleanname(p);
	d = dirstat(p);
	if(d == nil){
		char err[ERRMAX];

		rerrstr(err, sizeof err);
		werrstr("%s: %s: %s", fcerrname(fcerrclass(err)), p, err);
		free(p);
		return nil;
	}
	fi = mallocz(sizeof *fi, 1);
	fi->path = p;
	fi->name = strdup(strcmp(p, "/") == 0 ? "/" : fcbasename(p));
	fi->where = fcdirname(p);
	fi->d = d;
	fi->vol = w ? fcvolof(w, p) : nil;
	fi->writable = canwrite(p, d);
	fi->complete = 1;
	if(fi->vol != nil && strcmp(fi->vol->path, p) == 0)
		fi->kind = strdup("volume");
	else if(d->qid.type & QTDIR)
		fi->kind = strdup("folder");
	if(d->qid.type & QTDIR){
		if(deep){
			fi->size = 0;
			fi->items = 0;
			fi->complete = walk(p, d, &fi->size, &fi->items) == 0;
		}else{
			fd = open(p, OREAD);
			if(fd >= 0){
				n = dirreadall(fd, &cd);
				close(fd);
				if(n >= 0){
					fi->items = n;
					free(cd);
				}
			}
			fi->size = -1;
			fi->complete = 0;
		}
		return fi;
	}
	fi->size = d->length;
	if(w != nil && w->meta != nil){
		s = quotestrdup(p);
		fi->assoc = fcmetaquery(w->meta, "resolve %s", s);
		free(s);
	}
	if((s = fcrecget(fi->assoc, "desc")) != nil)
		fi->kind = strdup(s);
	else if((s = fcrecget(fi->assoc, "type")) != nil)
		fi->kind = smprint("%s document", s);
	else if(d->mode & 0111)
		fi->kind = strdup("program");
	else
		fi->kind = strdup("document");
	return fi;
}

void
fcinfofree(Fcinfo *fi)
{
	if(fi == nil)
		return;
	free(fi->path);
	free(fi->name);
	free(fi->where);
	free(fi->d);
	free(fi->kind);
	fcrecfree(fi->assoc);
	free(fi);
}
