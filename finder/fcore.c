/*
 * fcore: Finder's core operations from the shell, for scripts and
 * tests. See README.md.
 */
#include <u.h>
#include <libc.h>
#include <bio.h>
#include "fcore.h"

Biobuf out;
Fcws *ws;
int interactive;

static void
usage(void)
{
	fprint(2, "usage: fcore [-M] [-m metaroot] [-w workspace] [-v name=path[@addr]]... [verb args...]\n");
	exits("usage");
}

static int
err(void)
{
	Bflush(&out);
	if(interactive){
		Bprint(&out, "error: %r\n");
		Bflush(&out);
	}else
		fprint(2, "fcore: %r\n");
	return -1;
}

static Fcvol*
vol(char *name)
{
	Fcvol *v;

	v = fcvolbyname(ws, name);
	if(v == nil){
		v = fcvolof(ws, name);
		if(v == nil)
			werrstr("not found: no volume %s", name);
	}
	return v;
}

static void
prvol(Fcvol *v)
{
	Bprint(&out, "%q %q %s desktop=%s", v->name, v->path, fcvolstate(v->state), v->desktop ? "yes" : "no");
	if(v->icon)
		Bprint(&out, " icon=%q", v->icon);
	if(v->addr)
		Bprint(&out, " addr=%q", v->addr);
	if(v->srv)
		Bprint(&out, " srv=%q", v->srv);
	if(v->state == Voffline)
		Bprint(&out, " %q", smprint("err=%s", v->err));
	Bprint(&out, "\n");
}

static void
prcaps(Fcvol *v)
{
	int i;

	for(i = 0; i < Nkind; i++){
		Bprint(&out, "%s %s", fccapname(i), fccapval(v->caps.c[i]));
		if(v->caps.c[i] == Cno)
			Bprint(&out, " %q", v->caps.why[i]);
		Bprint(&out, "\n");
	}
}

static int
flag(int *argc, char ***argv, char *f)
{
	if(*argc > 0 && strcmp((*argv)[0], f) == 0){
		(*argc)--;
		(*argv)++;
		return 1;
	}
	return 0;
}

static int
need(int argc, int min, int max)
{
	if(argc < min || (max >= 0 && argc > max)){
		werrstr("bad syntax: wrong number of arguments");
		return -1;
	}
	return 0;
}

static int
run(int argc, char **argv)
{
	char *verb, *p, *q, *dst;
	int i, n, f;
	Fcvol *v;
	Fcfolder *fo;
	Fcchange *c;
	Fcinfo *fi;
	Dir **dv;

	if(argc == 0)
		return 0;
	verb = argv[0];
	argc--;
	argv++;
	if(strcmp(verb, "volumes") == 0){
		if(fcwsload(ws) < 0)
			return err();
		for(i = 0; i < ws->nvol; i++){
			fcvolcheck(ws->vol[i]);
			prvol(ws->vol[i]);
		}
	}else if(strcmp(verb, "reload") == 0){
		if(fcwsload(ws) < 0)
			return err();
	}else if(strcmp(verb, "check") == 0){
		if(need(argc, 1, 1) < 0 || (v = vol(argv[0])) == nil)
			return err();
		fcvolcheck(v);
		prvol(v);
		if(v->state != Vonline){
			werrstr("%s", v->err);
			return err();
		}
	}else if(strcmp(verb, "attach") == 0){
		if(need(argc, 1, 1) < 0 || (v = vol(argv[0])) == nil || fcvolattach(v) < 0)
			return err();
		prvol(v);
	}else if(strcmp(verb, "detach") == 0){
		if(need(argc, 1, 1) < 0 || (v = vol(argv[0])) == nil || fcvoldetach(v) < 0)
			return err();
	}else if(strcmp(verb, "caps") == 0 || strcmp(verb, "probe") == 0){
		if(need(argc, 1, 2) < 0 || (v = vol(argv[0])) == nil)
			return err();
		if(verb[0] == 'p'){
			p = argc > 1 ? fcresolve(ws, argv[1]) : nil;
			n = fcprobe(v, p);
			free(p);
			if(n < 0)
				return err();
		}
		prcaps(v);
	}else if(strcmp(verb, "ls") == 0){
		if(need(argc, 1, 2) < 0)
			return err();
		p = fcresolve(ws, argv[0]);
		fo = fcfolderopen(p);
		free(p);
		if(fo == nil)
			return err();
		dv = fcsorted(fo, fcsortby(argc > 1 ? argv[1] : nil));
		for(i = 0; dv[i] != nil; i++)
			Bprint(&out, "%q %s %lld %luo %lud\n", dv[i]->name,
				(dv[i]->qid.type&QTDIR) ? "d" : "-", dv[i]->length,
				dv[i]->mode&0777, dv[i]->mtime);
		free(dv);
		fcfolderfree(fo);
	}else if(strcmp(verb, "watch") == 0){
		/* watch PATH MS N: rescan N times, MS apart, printing changes */
		if(need(argc, 3, 3) < 0)
			return err();
		p = fcresolve(ws, argv[0]);
		fo = fcfolderopen(p);
		free(p);
		if(fo == nil)
			return err();
		Bprint(&out, "scan %ld\n", fo->n);
		Bflush(&out);
		for(i = 0; i < atoi(argv[2]); i++){
			sleep(atoi(argv[1]));
			n = fcrescan(fo, &c);
			if(n < 0){
				Bprint(&out, "error: %s\n", fo->err);
				Bflush(&out);
				continue;
			}
			for(f = 0; f < n; f++){
				if(c[f].op == Cren)
					Bprint(&out, "rename %q %q\n", c[f].old, c[f].name);
				else
					Bprint(&out, "%s %q\n", fcchangename(c[f].op), c[f].name);
			}
			Bflush(&out);
			fcchangefree(c, n);
		}
		fcfolderfree(fo);
	}else if(strcmp(verb, "info") == 0){
		f = flag(&argc, &argv, "-d");
		if(need(argc, 1, 1) < 0)
			return err();
		p = fcresolve(ws, argv[0]);
		fi = fcinfo(ws, p, f);
		free(p);
		if(fi == nil)
			return err();
		Bprint(&out, "name %q\n", fi->name);
		Bprint(&out, "path %q\n", fi->path);
		Bprint(&out, "where %q\n", fi->where);
		Bprint(&out, "kind %q\n", fi->kind);
		Bprint(&out, "volume %q\n", fi->vol ? fi->vol->name : "");
		if(fi->size >= 0)
			Bprint(&out, "size %lld\n", fi->size);
		if(fi->d->qid.type & QTDIR)
			Bprint(&out, "items %ld%s\n", fi->items, fi->complete ? "" : (f ? "+" : ""));
		Bprint(&out, "mode %luo\n", fi->d->mode & 0777);
		Bprint(&out, "owner %q\n", fi->d->uid);
		Bprint(&out, "group %q\n", fi->d->gid);
		Bprint(&out, "modified %lud\n", fi->d->mtime);
		Bprint(&out, "writable %s\n", fi->writable ? "yes" : "no");
		if((q = fcrecget(fi->assoc, "type")) != nil)
			Bprint(&out, "type %q\n", q);
		if((q = fcrecget(fi->assoc, "open")) != nil)
			Bprint(&out, "open %q\n", q);
		if((q = fcrecget(fi->assoc, "icon")) != nil)
			Bprint(&out, "icon %q\n", q);
		fcinfofree(fi);
	}else if(strcmp(verb, "rename") == 0 || strcmp(verb, "move") == 0){
		f = flag(&argc, &argv, "-c") ? Fcopydel : 0;
		if(need(argc, 2, 2) < 0)
			return err();
		p = fcresolve(ws, argv[0]);
		if(verb[0] == 'r')
			n = fcrename(ws, p, argv[1], f);
		else{
			q = fcresolve(ws, argv[1]);
			n = fcmove(ws, p, q, f);
			free(q);
		}
		free(p);
		if(n < 0)
			return err();
	}else if(strcmp(verb, "copy") == 0 || strcmp(verb, "dup") == 0 || strcmp(verb, "mkdir") == 0){
		if(need(argc, 1, verb[0] == 'd' ? 1 : 3) < 0)
			return err();
		p = fcresolve(ws, argv[0]);
		if(verb[0] == 'd')
			dst = fcduplicate(ws, p);
		else if(verb[0] == 'm')
			dst = fcmkdir(ws, p, argc > 1 ? argv[1] : nil);
		else if(argc < 2){
			free(p);
			werrstr("bad syntax: copy PATH DIR [NAME]");
			return err();
		}else{
			q = fcresolve(ws, argv[1]);
			dst = fccopy(ws, p, q, argc > 2 ? argv[2] : nil);
			free(q);
		}
		free(p);
		if(dst == nil)
			return err();
		Bprint(&out, "%q\n", dst);
		free(dst);
	}else if(strcmp(verb, "rm") == 0){
		f = flag(&argc, &argv, "-r") ? Frecursive : 0;
		if(need(argc, 1, 1) < 0)
			return err();
		p = fcresolve(ws, argv[0]);
		n = fcdelete(ws, p, f);
		free(p);
		if(n < 0)
			return err();
	}else if(strcmp(verb, "chmod") == 0){
		if(need(argc, 2, 2) < 0)
			return err();
		p = fcresolve(ws, argv[1]);
		n = fcchmod(ws, p, strtoul(argv[0], nil, 8));
		free(p);
		if(n < 0)
			return err();
	}else if(strcmp(verb, "open") == 0){
		if(need(argc, 1, 1) < 0)
			return err();
		p = fcresolve(ws, argv[0]);
		n = fcopen(ws, p, &dst);
		if(n < 0){
			free(p);
			return err();
		}
		if(n == Ofolder)
			Bprint(&out, "folder %q\n", p);
		else
			Bprint(&out, "plumbed %q %q\n", p, dst);
		free(dst);
		free(p);
	}else if(strcmp(verb, "openwith") == 0){
		if(need(argc, 2, 2) < 0)
			return err();
		p = fcresolve(ws, argv[0]);
		n = fcopenwith(ws, p, argv[1]);
		free(p);
		if(n < 0)
			return err();
		Bprint(&out, "%s\n", n == Oplumbed ? "plumbed" : "launched");
	}else if(strcmp(verb, "launch") == 0){
		if(need(argc, 1, -1) < 0)
			return err();
		p = argc > 1 ? fcresolve(ws, argv[1]) : nil;
		n = fclaunch(ws, argv[0], p, argc > 2 ? argv+2 : nil);
		free(p);
		if(n < 0)
			return err();
		Bprint(&out, "launched %d\n", n);
	}else{
		werrstr("unknown command: %s", verb);
		return err();
	}
	Bflush(&out);
	return 0;
}

void
main(int argc, char **argv)
{
	char *metaroot, *wsname, *p, *line, *args[32];
	int nometa, n, status;
	Fcmeta *m;
	Biobuf in;
	struct { char *name; char *path; char *addr; } vols[16];
	int nvols, i;
	Fcvol *v;

	quotefmtinstall();
	metaroot = "/mnt/meta";
	wsname = nil;
	nometa = 0;
	nvols = 0;
	ARGBEGIN{
	case 'M':
		nometa = 1;
		break;
	case 'm':
		metaroot = EARGF(usage());
		break;
	case 'w':
		wsname = EARGF(usage());
		break;
	case 'v':
		p = EARGF(usage());
		if(nvols >= nelem(vols) || strchr(p, '=') == nil)
			usage();
		vols[nvols].name = p;
		vols[nvols].path = strchr(p, '=')+1;
		*strchr(p, '=') = 0;
		vols[nvols].addr = strchr(vols[nvols].path, '@');
		if(vols[nvols].addr != nil)
			*vols[nvols].addr++ = 0;
		nvols++;
		break;
	default:
		usage();
	}ARGEND

	Binit(&out, 1, OWRITE);
	m = nil;
	if(!nometa){
		m = fcmetaopen(metaroot);
		if(m == nil && strcmp(metaroot, "/mnt/meta") != 0)
			sysfatal("%r");
	}
	ws = fcwsopen(wsname, m);
	if(ws == nil)
		sysfatal("workspace: %r");
	for(i = 0; i < nvols; i++){
		v = fcwsaddvol(ws, vols[i].name, vols[i].path);
		if(vols[i].addr != nil)
			v->addr = strdup(vols[i].addr);
	}

	if(argc > 0){
		if(run(argc, argv) < 0)
			exits("error");
		exits(nil);
	}
	/* no verb: one command per line from stdin, sharing what's learned */
	interactive = 1;
	status = 0;
	Binit(&in, 0, OREAD);
	while((line = Brdstr(&in, '\n', 1)) != nil){
		n = tokenize(line, args, nelem(args));
		if(n > 0 && args[0][0] != '#'){
			Bprint(&out, "> %s", args[0]);
			for(i = 1; i < n; i++)
				Bprint(&out, " %q", args[i]);
			Bprint(&out, "\n");
			if(run(n, args) < 0)
				status = 1;
		}
		free(line);
	}
	Bflush(&out);
	exits(status ? "error" : nil);
}
