/*
 * ctlapp: a libux app for test/controls.rc, with the controls the
 * sample doesn't have: a file chooser, a list with columns, and the
 * spike's laid out by style words (select, radios, slider, switch,
 * tabs, svg, styled-text, divs). It writes what its handlers get to $ctlout/NAME.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>

static char *out;

static void
note(char *name, char *fmt, ...)
{
	va_list arg;
	char *s, *p;
	int fd;

	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	p = smprint("%s/%s", out, name);
	fd = create(p, OWRITE, 0666);
	if(fd >= 0){
		write(fd, s, strlen(s));
		close(fd);
	}
	free(p);
	free(s);
}

static void
chooserev(Uxev *e, void*)
{
	char *d;

	if(e->type == UXchoose){
		d = uxchooserdir(e->ctl);
		note("chose", "%s in %s\n", e->text, d);
		free(d);
	}else if(e->type == UXcancel)
		note("cancelled", "cancelled\n");
}

/* the rows the drawing layer has selected, from 0 */
static void
listev(Uxev *e, void*)
{
	int *sel, n, i;
	char buf[128], *p;

	if(e->type != UXselection)
		return;
	n = uxgetsel(e->ctl, &sel);
	p = buf;
	buf[0] = 0;
	for(i = 0; i < n && p < buf+sizeof buf-16; i++)
		p = seprint(p, buf+sizeof buf, "%s%d", i > 0 ? " " : "", sel[i]);
	free(sel);
	note("people.sel", "%s\n", buf);
}

static void
choiceev(Uxev *e, void*)
{
	char name[64];

	if(e->type != UXvalue)
		return;
	snprint(name, sizeof name, "%s.choice", uxctlid(e->ctl));
	note(name, "%d\n", uxchoice(e->ctl));
}

void
threadmain(int, char**)
{
	Uxwin *w, *w2;
	Uxctl *c, *box, *row;
	char *fruit[] = { "Apple", "Banana", "Cherry" };
	char *tabs[] = { "One", "Two" };
	char *cols[] = { "Name", "Age" };
	int widths[] = { 100, 40 };
	char *people[] = { "Ann\t31", "Bob\t27" };

	out = getenv("ctlout");
	if(out == nil)
		sysfatal("ctlout must be set");
	uxinit("ctlapp", "Controls");
	w = uxwindow("window", "Choose", Rect(0, 0, 300, 240));
	c = uxfilechooser(w, "files", getenv("ctldir"), Rect(10, 10, 290, 200));
	uxhandler(c, chooserev, nil);
	c = uxlist(w, "people", Rect(10, 205, 290, 235));
	uxsetcolumns(c, cols, widths, nelem(cols));
	uxsetitems(c, people, nelem(people));
	uxhandler(c, listev, nil);
	w2 = uxwindow("window", "Spike", Rect(0, 0, 400, 300));
	uxwinstyle(w2, "v-flex p=12 gap=8");
	box = uxdiv(w2, "box");
	uxstyle(box, "h-flex gap=6");
	row = uxdiv(w2, "row");
	uxstyle(row, "h-flex gap=4");
	c = uxpopup(w2, "fruit", "Fruit");
	uxstyle(c, "flex-1");	/* kept when it moves into box */
	uxsetitems(c, fruit, nelem(fruit));
	uxhandler(c, choiceev, nil);
	uxsetparent(c, box);
	c = uxradios(w2, "size", 1);
	uxsetitems(c, fruit, nelem(fruit));
	uxhandler(c, choiceev, nil);
	uxsetparent(c, box);
	c = uxslider(w2, "level", 0, 10, 1);
	uxhandler(c, choiceev, nil);
	uxsetparent(c, row);
	c = uxswitch(w2, "wifi", "Wi-Fi");
	uxhandler(c, choiceev, nil);
	uxsetparent(c, row);
	c = uxtabbar(w2, "pages");
	uxsetitems(c, tabs, nelem(tabs));
	uxhandler(c, choiceev, nil);
	uxsvg(w2, "pic", "<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\"/></svg>");
	uxstyledtext(w2, "para", "Hello world", "0 5 weight=bold");
	uxmain();
	threadexitsall(nil);
}
