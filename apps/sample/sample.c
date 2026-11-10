/*
 * sample: a small libux app using every control, for trying libux by
 * hand and for the libux tests.
 *
 * Its menus, dialogs, list strings and icon come from sample.rsrc, next
 * to the binary (made from sample.res), read through resfs.
 *
 *	sample		in a rio window
 *	uxheadless=1 uxpublic=1 sample	no display; tree at /srv/ux.$user.sample
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>

static Uxwin *win, *dlg;
static Uxctl *name, *save, *status, *notes, *pass, *agree, *list, *icons, *scroll;
static int saves;

static void
setstatus(char *fmt, ...)
{
	va_list arg;
	char *s;

	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	uxsetvalue(status, s);
	free(s);
}

/* the confirm dialog (DLOG 128) and the about alert (ALRT 129) */
static void
dlgev(Uxev *e, void*)
{
	char *what;

	if(dlg == nil || e->win != dlg)
		return;
	switch(e->type){
	case UXactivate:
		what = uxresitem(e->ctl) == 1 ? "confirmed" : "cancelled";
		break;
	case UXdefault:
		what = "confirmed";
		break;
	case UXcancel:
	case UXclose:
		what = "cancelled";
		break;
	default:
		return;
	}
	uxwinclose(dlg);
	dlg = nil;
	setstatus("%s", what);
}

static void
confirm(void)
{
	if(dlg != nil)
		return;
	dlg = uxresdialog(128, dlgev, nil);
	if(dlg == nil)
		setstatus("no dialog: %r");
}

static void
about(void)
{
	char buf[16];

	if(dlg != nil)
		return;
	snprint(buf, sizeof buf, "%d", saves);
	uxresparam(0, buf);
	dlg = uxresalert(129, dlgev, nil);
	if(dlg == nil)
		setstatus("no alert: %r");
}

static void
saved(Uxev *e, void*)
{
	char *s;

	if(e->type != UXactivate)
		return;
	s = uxgetvalue(name);
	saves++;
	setstatus("saved %s (%d)", s, saves);
	free(s);
}

static void
action(Uxev *e, void*)
{
	if(e->type == UXquit || strcmp(e->action, "quit") == 0)
		uxquit();
	else if(strcmp(e->action, "new") == 0){
		uxsetvalue(name, "");
		setstatus("new");
	}else if(strcmp(e->action, "confirm") == 0)
		confirm();
	else if(strcmp(e->action, "about-sample") == 0)
		about();
	else if(strcmp(e->action, "close") == 0)
		uxquit();
	else
		setstatus("action %s", e->action);
}

static void
appev(Uxev *e, void *arg)
{
	if(e->type == UXquit)
		uxquit();
	else if(e->type == UXaction)
		action(e, arg);
}

static void
listev(Uxev *e, void*)
{
	if(e->type == UXopen)
		setstatus("opened item %d", e->n);
}

static void
iconev(Uxev *e, void*)
{
	if(e->type == UXopen)
		setstatus("opened %s", e->item);
	else if(e->type == UXrename)
		setstatus("renamed %s to %s", e->item, e->text);
}

/* Notes takes text, and files (dropped from Finder, say): their contents */
static void
notesev(Uxev *e, void*)
{
	Uxdropfile *df;
	char *data;
	long n;
	int nf;

	if(e->type != UXdrop)
		return;
	if(strcmp(e->text, "file") == 0){
		nf = uxdropfiles(e, &df);
		if(nf < 1){
			setstatus("drop: no files: %r");
			return;
		}
		if(df[0].dir || (data = uxdropdata(e, "file", 0, &n)) == nil){
			setstatus("drop: can't read %s", df[0].name);
			uxdropresult(e, nil, "not supported: a folder");
		}else{
			uxsetvalue(notes, data);
			setstatus("dropped %s (%ld bytes)", df[0].name, n);
			free(data);
		}
		uxdropfilesfree(df, nf);
		return;
	}
	if((data = uxdropdata(e, "text", -1, &n)) != nil){
		uxsetvalue(notes, data);
		setstatus("dropped text (%ld bytes)", n);
		free(data);
	}
}

static void
usage(void)
{
	fprint(2, "usage: %s\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char *argv[])
{
	char *colors[16];
	int ncolors;

	ARGBEGIN{
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	uxinit("sample", "Sample");
	uxapphandler(appev, nil);

	if(uxres(nil) < 0)
		sysfatal("resources: %r");
	if(uxresmenu(128) == nil || uxresmenu(129) == nil)
		sysfatal("menus: %r");
	for(ncolors = 0; ncolors < nelem(colors); ncolors++)
		if((colors[ncolors] = uxresstr(128, ncolors+1)) == nil)
			break;

	win = uxwindow("window", "Sample", Rect(0, 0, 480, 400));
	uxlabel(win, "name-label", "Name:", Rect(16, 16, 70, 36));
	name = uxtextfield(win, "name", "Name", Rect(80, 16, 300, 36), 0);
	save = uxbutton(win, "save", "Save", Rect(316, 16, 396, 36));
	uxhandler(save, saved, nil);
	uxdefault(win, save);
	pass = uxtextfield(win, "password", "Password", Rect(80, 48, 300, 68), UXsecret);
	agree = uxcheckbox(win, "agree", "I agree", Rect(316, 48, 460, 68));
	notes = uxtextfield(win, "notes", "Notes", Rect(16, 80, 300, 160), UXmultiline);
	uxhandler(notes, notesev, nil);
	uxdroptypes(notes, "file,text", "copy");
	list = uxlist(win, "colors", Rect(316, 80, 460, 190));
	uxsetitems(list, colors, ncolors);
	uxhandler(list, listev, nil);
	scroll = uxscrollview(win, "files", Rect(16, 200, 460, 340));
	uxsetextent(scroll, Rect(0, 0, 440, 400));
	icons = uxiconview(win, "icons", Rect(17, 201, 443, 339));
	uxsetparent(icons, scroll);
	uxiconadd(icons, "readme", "Read Me", "doc-text", Pt(48, 40));
	uxiconadd(icons, "tmp", "tmp", "folder", Pt(140, 40));
	uxiconadd(icons, "sample", "Sample", "sample", Pt(232, 40));
	uxhandler(icons, iconev, nil);
	status = uxlabel(win, "status", "", Rect(16, 352, 460, 372));
	USED(pass, agree);
	uxfocus(name);

	uxmain();
	threadexitsall(nil);
}
