/*
 * dragapp: a libux app for test/drag.rc. Its window holds
 *
 *	src	a button (0,0)-(60,20): activating it starts a drag offering
 *		$dragfile as "file" and the text "hello from dragapp"
 *	dst	a label (100,100)-(200,200): a drop target for file and text,
 *		copy only
 *	icons	an icon view (0,210)-(300,300) taking text, with folder1 at 50,30
 *
 * and writes what happens to files in $dragout: drop (type, op, pt,
 * from self), text (a dropped text), list (a dropped file list),
 * saved (file item 0, saved), paths (uxdragpaths), end (UXdragend).
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>

static char *out;
static char *dragfile;

static void
note(char *name, char *fmt, ...)
{
	va_list arg;
	char *s, *p, *t;
	int fd;
	Dir d;

	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	p = smprint("%s/%s", out, name);
	t = smprint("%s.tmp", p);
	fd = create(t, OWRITE, 0666);
	if(fd >= 0){
		write(fd, s, strlen(s));
		close(fd);
		remove(p);
		nulldir(&d);
		d.name = strrchr(p, '/')+1;
		dirwstat(t, &d);
	}
	free(t);
	free(p);
	free(s);
}

static void
srcfn(Uxev *e, void*)
{
	Uxdrag *d;
	static char hello[] = "hello from dragapp";

	if(e->type == UXactivate){
		d = uxdragnew("copy,move", "a file");
		uxdragfile(d, dragfile);
		uxdragdata(d, "text", hello, strlen(hello));
		if(uxdragstart(d, e->ctl) < 0)
			note("starterr", "%r\n");
		return;
	}
	if(e->type == UXdragend)
		note("end", "end %s\n", e->text != nil ? e->text : "cancel");
}

static void
dstfn(Uxev *e, void*)
{
	Uxdropfile *fs;
	char *s, **p, *path;
	int n, i;
	long len;
	Fmt f;

	if(e->type != UXdrop)
		return;
	if(strcmp(e->text, "text") == 0){
		s = uxdropdata(e, "text", -1, &len);
		note("text", "%s", s != nil ? s : "error");
		free(s);
	}else{
		n = uxdropfiles(e, &fs);
		fmtstrinit(&f);
		for(i = 0; i < n; i++)
			fmtprint(&f, "%s %lld %s %luo\n", fs[i].name, fs[i].size, fs[i].dir ? "dir" : "file", fs[i].mode);
		s = fmtstrflush(&f);
		note("list", "%s", s);
		free(s);
		path = smprint("%s/saved", out);
		if(n > 0 && uxdropsave(e, 0, path, fs[0].mode) < 0)
			note("saveerr", "%r\n");
		free(path);
		uxdropfilesfree(fs, n);
		p = uxdragpaths(e, &n);
		fmtstrinit(&f);
		for(i = 0; i < n; i++){
			fmtprint(&f, "%s\n", p[i]);
			free(p[i]);
		}
		free(p);
		s = fmtstrflush(&f);
		note("paths", "%s", s);
		free(s);
	}
	note("drop", "drop %s %s %d,%d self=%d\n", e->text, e->action, e->pt.x, e->pt.y, uxdragfromself(e));
}

static void
iconfn(Uxev *e, void*)
{
	if(e->type == UXdrop)
		note("icondrop", "drop %s on %s\n", e->text, e->item != nil ? e->item : "nothing");
}

void
threadmain(int, char**)
{
	Uxwin *w;
	Uxctl *c;

	out = getenv("dragout");
	dragfile = getenv("dragfile");
	if(out == nil || dragfile == nil)
		sysfatal("dragout and dragfile must be set");
	uxinit("dragapp", "Drag App");
	w = uxwindow("window", "Drag", Rect(0, 0, 300, 300));
	c = uxbutton(w, "src", "Drag me", Rect(0, 0, 60, 20));
	uxhandler(c, srcfn, nil);
	c = uxlabel(w, "dst", "Drop here", Rect(100, 100, 200, 200));
	uxhandler(c, dstfn, nil);
	uxdroptypes(c, "file,text", "copy");
	c = uxiconview(w, "icons", Rect(0, 210, 300, 300));
	uxiconadd(c, "folder1", "Folder", "folder", Pt(50, 30));
	uxhandler(c, iconfn, nil);
	uxdroptypes(c, "text", nil);
	uxmain();
	threadexitsall(nil);
}
