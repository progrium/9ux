/*
 * resfs: serve classic resource files, decoded, over 9P.
 *
 *	resfs [-D] [-m mtpt] [-s srvname] [name=]file ...
 *
 * See docs/protocols/res.md for the tree. Each file is parsed and
 * decoded once at startup; the tree is read-only after that.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "res.h"

typedef struct Content Content;
struct Content {
	uchar	*p;
	u32int	n;
	int	owned;	/* p was allocated for this file */
};

static char *user;
static char version[] = "res 1.0\n";
static Srv fs;

static void
usage(void)
{
	fprint(2, "usage: resfs [-D] [-m mtpt] [-s srvname] [name=]file ...\n");
	exits("usage");
}

static File*
mkdirf(File *parent, char *name)
{
	File *f;

	f = createfile(parent, name, user, DMDIR|0555, nil);
	if(f == nil)
		sysfatal("create %s: %r", name);
	return f;
}

static void
mkcontent(File *dir, char *name, uchar *p, u32int n, int owned)
{
	Content *c;
	File *f;

	c = emalloc9p(sizeof *c);
	c->p = p;
	c->n = n;
	c->owned = owned;
	f = createfile(dir, name, user, 0444, c);
	if(f == nil)
		sysfatal("create %s: %r", name);
	f->length = n;
	closefile(f);
}

/* takes b's buffer when it has content; frees it otherwise */
static void
mkbuf(File *dir, char *name, Rbuf *b)
{
	if(b->err){
		rbuffree(b);
		return;
	}
	mkcontent(dir, name, b->p, b->n, 1);
	rbufinit(b);
}

static void
addres(File *top, Resfile *rf)
{
	char tname[16], id[16];
	File *tdir, *rdir;
	uchar last[4];
	Rbuf b;
	Res *r;
	int i;

	tdir = nil;
	rbufinit(&b);
	for(i = 0; i < rf->nres; i++){
		r = &rf->res[i];
		if(tdir == nil || memcmp(last, r->type, 4) != 0){
			if(tdir != nil)
				closefile(tdir);
			resescname(r->type, tname, sizeof tname);
			tdir = mkdirf(top, tname);
			memmove(last, r->type, 4);
		}
		snprint(id, sizeof id, "%d", r->id);
		rdir = mkdirf(tdir, id);
		mkcontent(rdir, "raw", r->data, r->len, 0);
		if(r->name != nil){
			rbufs(&b, r->name);
			rbufc(&b, '\n');
			mkbuf(rdir, "name", &b);
		}
		if(isicontype(r) && resimage(rf, r, &b, nil, 0) == 0)
			mkbuf(rdir, "image", &b);
		rbuffree(&b);
		if(restypeeq(r, "CURS") && rescursor(r, &b, nil, 0) == 0)
			mkbuf(rdir, "cursor", &b);
		rbuffree(&b);
		if(istexttype(r) && restext(r, &b, nil, 0) == 0)
			mkbuf(rdir, "text", &b);
		rbuffree(&b);
		closefile(rdir);
	}
	if(tdir != nil)
		closefile(tdir);
}

static int
hasicon(Res *r)
{
	static char *types[] = {"ICN#", "icl4", "icl8", "ics#", "ics4", "ics8"};
	int i;

	for(i = 0; i < nelem(types); i++)
		if(restypeeq(r, types[i]))
			return 1;
	return 0;
}

/* icon/<id>/{image,small}: the deepest member of each family */
static void
addicons(File *top, Resfile *rf)
{
	static uchar seen[65536/8];
	File *icons, *dir;
	char id[16];
	Rbuf large, small;
	int i, okl, oks, rid, bit;

	memset(seen, 0, sizeof seen);
	icons = nil;
	rbufinit(&large);
	rbufinit(&small);
	for(i = 0; i < rf->nres; i++){
		if(!hasicon(&rf->res[i]))
			continue;
		rid = rf->res[i].id;
		bit = rid + 32768;
		if(seen[bit/8] & (1<<(bit%8)))
			continue;
		seen[bit/8] |= 1<<(bit%8);
		okl = resicon(rf, rid, Iconlarge, &large, nil, 0) == 0;
		oks = resicon(rf, rid, Iconsmall, &small, nil, 0) == 0;
		if(okl || oks){
			if(icons == nil)
				icons = mkdirf(top, "icon");
			snprint(id, sizeof id, "%d", rid);
			dir = mkdirf(icons, id);
			if(okl)
				mkbuf(dir, "image", &large);
			if(oks)
				mkbuf(dir, "small", &small);
			closefile(dir);
		}
		rbuffree(&large);
		rbuffree(&small);
	}
	if(icons != nil)
		closefile(icons);
}

static void
addfile(char *arg)
{
	char err[ERRMAX], *name, *path, *p;
	uchar *buf;
	u32int n;
	Resfile *rf;
	File *top, *f;
	Rbuf b;

	path = arg;
	p = strchr(arg, '=');
	if(p != nil){
		*p = 0;
		name = arg;
		path = p+1;
	}else{
		name = strrchr(path, '/');
		name = estrdup9p(name != nil ? name+1 : path);
		if(strncmp(name, "._", 2) == 0 && name[2] != 0)	/* AppleDouble's ._name */
			memmove(name, name+2, strlen(name+2)+1);
		p = strrchr(name, '.');
		if(p != nil && p != name)
			*p = 0;
	}
	if(*name == 0 || strcmp(name, "version") == 0 || strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strchr(name, '/') != nil)
		sysfatal("%s: bad name '%s'", path, name);
	f = walkfile(fs.tree->root, name);
	if(f != nil)
		sysfatal("%s: name '%s' already used; give one with name=file", path, name);
	buf = resreadfile(path, &n, err, sizeof err);
	if(buf == nil)
		sysfatal("%s: %s", path, err);
	rf = emalloc9p(sizeof *rf);
	if(resparse(buf, n, rf, err, sizeof err) < 0)
		sysfatal("%s: %s", path, err);
	if(rf->nbad > 0 || rf->ndup > 0)
		fprint(2, "resfs: %s: skipped %d out-of-bounds and %d repeated resources\n",
			path, rf->nbad, rf->ndup);
	top = mkdirf(fs.tree->root, name);
	rbufinit(&b);
	resindex(rf, &b);
	mkbuf(top, "index", &b);
	addicons(top, rf);
	addres(top, rf);
	closefile(top);
}

static void
fsread(Req *r)
{
	Content *c;

	c = r->fid->file->aux;
	if(c == nil){
		respond(r, "no content");
		return;
	}
	readbuf(r, c->p, c->n);
	respond(r, nil);
}

void
main(int argc, char **argv)
{
	char *mtpt, *srvname;
	int i;

	mtpt = "/mnt/res";
	srvname = nil;
	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc == 0)
		usage();
	if(*mtpt == 0)
		mtpt = nil;
	if(mtpt == nil && srvname == nil)
		sysfatal("nothing to do: give a mount point or a srv name");

	user = getuser();
	fs.read = fsread;
	fs.tree = alloctree(nil, nil, DMDIR|0555, nil);
	mkcontent(fs.tree->root, "version", (uchar*)version, strlen(version), 0);
	for(i = 0; i < argc; i++)
		addfile(argv[i]);
	postmountsrv(&fs, srvname, mtpt, MREPL);
	exits(nil);
}
