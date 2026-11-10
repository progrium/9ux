/*
 * sample: a small libux app using every control, for trying libux by
 * hand and for the libux tests.
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

static void
dlgev(Uxev *e, void*)
{
	switch(e->type){
	case UXdefault:
	case UXcancel:
	case UXclose:
		uxwinclose(dlg);
		dlg = nil;
		setstatus(e->type == UXdefault ? "confirmed" : "cancelled");
		break;
	}
}

static void
dlgbutton(Uxev *e, void *arg)
{
	if(e->type != UXactivate)
		return;
	uxwinclose(dlg);
	dlg = nil;
	setstatus("%s", (char*)arg);
}

static void
confirm(void)
{
	Uxctl *ok, *cancel;

	if(dlg != nil)
		return;
	dlg = uxdialog("Confirm", Rect(200, 200, 520, 330));
	uxwinhandler(dlg, dlgev, nil);
	uxlabel(dlg, "msg", "Save the document?", Rect(16, 16, 300, 36));
	cancel = uxbutton(dlg, "cancel", "Cancel", Rect(140, 80, 210, 104));
	ok = uxbutton(dlg, "ok", "OK", Rect(226, 80, 296, 104));
	uxhandler(cancel, dlgbutton, "cancelled");
	uxhandler(ok, dlgbutton, "confirmed");
	uxdefault(dlg, ok);
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

static void
usage(void)
{
	fprint(2, "usage: %s\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char *argv[])
{
	Uxmenu *m;
	static char *colors[] = { "Red", "Green", "Blue", "Cyan", "Magenta", "Yellow" };

	ARGBEGIN{
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	uxinit("sample", "Sample");
	uxapphandler(appev, nil);

	m = uxmenu("file", "File");
	uxitem(m, "new", "New", "n");
	uxitem(m, "confirm", "Confirm…", "k");
	uxsep(m, "s1");
	uxitem(m, "close", "Close", "w");
	uxitem(m, "quit", "Quit", "q");
	m = uxmenu("edit", "Edit");
	uxitem(m, "undo", "Undo", "z");
	uxitemenable("undo", 0);

	win = uxwindow("window", "Sample", Rect(0, 0, 480, 400));
	uxlabel(win, "name-label", "Name:", Rect(16, 18, 70, 34));
	name = uxtextfield(win, "name", "Name", Rect(80, 14, 300, 36), 0);
	save = uxbutton(win, "save", "Save", Rect(316, 14, 396, 38));
	uxhandler(save, saved, nil);
	uxdefault(win, save);
	pass = uxtextfield(win, "password", "Password", Rect(80, 46, 300, 68), UXsecret);
	agree = uxcheckbox(win, "agree", "I agree", Rect(316, 46, 460, 68));
	notes = uxtextfield(win, "notes", "Notes", Rect(16, 80, 300, 160), UXmultiline);
	list = uxlist(win, "colors", Rect(316, 80, 460, 190));
	uxsetitems(list, colors, nelem(colors));
	uxhandler(list, listev, nil);
	scroll = uxscrollview(win, "files", Rect(16, 200, 460, 340));
	uxsetextent(scroll, Rect(0, 0, 440, 400));
	icons = uxiconview(win, "icons", Rect(17, 201, 447, 339));
	uxsetparent(icons, scroll);
	uxiconadd(icons, "readme", "Read Me", "document", Pt(48, 40));
	uxiconadd(icons, "tmp", "tmp", "folder", Pt(140, 40));
	uxhandler(icons, iconev, nil);
	status = uxlabel(win, "status", "", Rect(16, 352, 460, 372));
	USED(pass, agree, notes);
	uxfocus(name);

	uxmain();
	threadexitsall(nil);
}
