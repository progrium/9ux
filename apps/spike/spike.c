/*
 * spike: uitree's flexbox layout, svg and styled-text on libux.
 *
 *	spike		in a rio window; resize it to see the layout reflow
 *	spike -s svg	just the svg gallery
 *
 * Every rectangle here is ZR: the layout places everything.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>

static Uxwin *win;

#define SV(body) "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>" body "</svg>"

static char *icinbox = SV("<path d='M22 12h-6l-2 3h-4l-2-3H2'/><path d='M5.45 5.11 2 12v6a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2v-6l-3.45-6.89A2 2 0 0 0 16.76 4H7.24a2 2 0 0 0-1.79 1.11z'/>");
static char *icsend = SV("<path d='M22 2 11 13'/><path d='M22 2 15 22 11 13 2 9z'/>");
static char *icfile = SV("<path d='M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z'/><path d='M14 2v6h6'/>");
static char *ictrash = SV("<path d='M3 6h18'/><path d='M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6'/><path d='M8 6V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2'/><line x1='10' y1='11' x2='10' y2='17'/><line x1='14' y1='11' x2='14' y2='17'/>");
static char *icsearch = SV("<circle cx='11' cy='11' r='8'/><path d='m21 21-4.3-4.3'/>");
static char *icgear = SV("<circle cx='12' cy='12' r='4'/><path d='M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M6.34 17.66l-1.41 1.41M19.07 4.93l-1.41 1.41'/>");
/* a circle from two arcs, the second with its command letter left out */
static char *icdisc = "<svg viewBox='0 0 24 24'><path fill='#000099' d='M2 12a10 10 0 1 1 20 0 10 10 0 1 1-20 0z'/><path fill='#FFFFFF' d='M8 12h8M12 8v8' stroke='#FFFFFF' stroke-width='2.5'/></svg>";
static char *icstar = "<svg viewBox='0 0 24 24'><polygon fill='currentColor' points='12 2 15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26 12 2'/></svg>";
static char *icheart = "<svg viewBox='0 0 24 24'><path fill='#CC3333' d='M12 21s-7-4.35-9.5-8.5C.5 9 2.5 4.5 6.5 4.5c2.1 0 3.6 1.2 5.5 3.2 1.9-2 3.4-3.2 5.5-3.2 4 0 6 4.5 4 8C19 16.65 12 21 12 21z'/></svg>";
static char *icring = "<svg viewBox='0 0 24 24'><path fill='#226666' fill-rule='evenodd' d='M12 2a10 10 0 1 0 0 20a10 10 0 1 0 0-20zm0 5a5 5 0 1 1 0 10a5 5 0 1 1 0-10z'/></svg>";
static char *iclogo = "<svg viewBox='0 0 32 32'><rect x='1' y='1' width='30' height='30' rx='7' fill='#226666'/>"
	"<circle cx='16' cy='16' r='9' fill='#EEEE9E'/>"
	"<path d='M11.5 16.5l3 3 6-7' fill='none' stroke='#000000' stroke-width='2.5' stroke-linecap='round'/></svg>";
static char *icglenda = "<svg viewBox='0 0 64 64'>"
	"<ellipse cx='32' cy='38' rx='22' ry='20' fill='#55AAAA'/>"
	"<ellipse cx='14' cy='14' rx='8' ry='10' fill='#55AAAA'/><ellipse cx='50' cy='14' rx='8' ry='10' fill='#55AAAA'/>"
	"<circle cx='24' cy='32' r='7' fill='#FFFFFF'/><circle cx='40' cy='32' r='7' fill='#FFFFFF'/>"
	"<circle cx='25' cy='33' r='3' fill='#000000'/><circle cx='39' cy='33' r='3' fill='#000000'/>"
	"<path d='M24 46 Q32 54 40 46' fill='none' stroke='#000000' stroke-width='2.5'/></svg>";

static char *para =
	"uitree lays this window out with flexbox: no control here was given a rectangle. "
	"This paragraph is a styled-text node with runs in bold, italic, underlined, "
	"struck through, highlighted, coloured and larger text, wrapped to the width "
	"of its box.\nA newline starts a new line.";

static void
quit(Uxev*, void*)
{
	uxquit();
}

static Uxctl*
st(Uxctl *c, char *words)
{
	if(words != nil && uxstyle(c, words) < 0)
		sysfatal("style %q: %r", words);
	return c;
}

static Uxctl*
box(Uxctl *parent, char *id, char *words)
{
	Uxctl *c;

	c = st(uxdiv(win, id), words);
	if(parent != nil)
		uxsetparent(c, parent);
	return c;
}

static Uxctl*
in(Uxctl *parent, Uxctl *c, char *words)
{
	st(c, words);
	if(parent != nil)
		uxsetparent(c, parent);
	return c;
}

static Uxctl*
text(Uxctl *parent, char *id, char *s, char *words)
{
	return in(parent, uxlabel(win, id, s, ZR), words);
}

static Uxctl*
svg(Uxctl *parent, char *id, char *src, char *words)
{
	return in(parent, uxsvg(win, id, src), words);
}

/* byte offset of the first s in para */
static char*
run(char *s, char *words)
{
	char *p;

	p = strstr(para, s);
	if(p == nil)
		sysfatal("no %q", s);
	return smprint("%ld %ld %s\n", (long)(p - para), (long)strlen(s), words);
}

static char*
runs(void)
{
	char *r[] = {
		run("flexbox", "font-bold"),
		run("styled-text", "text-color=deep font-bold"),
		run("bold", "font-bold"),
		run("italic", "italic"),
		run("underlined", "underline"),
		run("struck through", "line-through text-color=grey4"),
		run("highlighted", "bg=butter"),
		run("coloured", "text-color=red"),
		run("larger text", "text-lg font-bold"),
		run("newline", "italic text-color=navy"),
	};
	char *s, *t;
	int i;

	s = strdup("");
	for(i = 0; i < nelem(r); i++){
		t = smprint("%s%s", s, r[i]);
		free(s);
		free(r[i]);
		s = t;
	}
	return s;
}

static void
nav(Uxctl *side, char *id, char *icon, char *label, int sel)
{
	Uxctl *row;
	char *rid, *iid;

	rid = smprint("nav-%s", id);
	iid = smprint("ic-%s", id);
	row = box(side, rid, sel ? "h-flex gap=6 px=6 py=3 bg=navy" : "h-flex gap=6 px=6 py=3");
	svg(row, iid, icon, sel ? "w=16 h=16 text-color=paper" : "w=16 h=16");
	text(row, id, label, sel ? "text-color=paper" : nil);
	free(rid);
	free(iid);
}

static void
mailscreen(void)
{
	Uxctl *head, *body, *side, *main, *form, *row, *btns, *foot, *icons;

	if(uxwinstyle(win, "v-flex p=12 gap=10") < 0)
		sysfatal("root style: %r");

	head = box(nil, "head", "h-flex gap=8");
	svg(head, "logo", iclogo, "w=28 h=28");
	text(head, "title", "uitree on libux", "text-xl font-bold");
	box(head, "spacer", "flex-1");
	in(head, uxtextfield(win, "search", "Search", ZR, 0), "w=140");
	svg(head, "ic-search", icsearch, "w=18 h=18");
	in(head, uxbutton(win, "settings", "Settings", ZR), nil);

	body = box(nil, "body", "h-flex flex-1 gap=10 items-stretch");
	side = box(body, "side", "v-flex w=128 gap=2 p=6 bg=grey1 border-1");
	nav(side, "inbox", icinbox, "Inbox", 1);
	nav(side, "sent", icsend, "Sent", 0);
	nav(side, "drafts", icfile, "Drafts", 0);
	nav(side, "trash", ictrash, "Trash", 0);
	box(side, "side-fill", "flex-1");
	nav(side, "prefs", icgear, "Display", 0);

	main = box(body, "main", "v-flex flex-1 gap=10 p=12 border-1");
	in(main, uxstyledtext(win, "para", para, runs()), nil);
	form = box(main, "form", "v-flex gap=6");
	row = box(form, "row-to", "h-flex gap=8");
	text(row, "l-to", "To:", "w=56 text-right");
	in(row, uxtextfield(win, "to", "To", ZR, 0), "flex-1");
	row = box(form, "row-subj", "h-flex gap=8");
	text(row, "l-subj", "Subject:", "w=56 text-right");
	in(row, uxtextfield(win, "subj", "Subject", ZR, 0), "flex-1");
	row = box(form, "row-opt", "h-flex gap=8");
	box(row, "indent", "w=56");
	in(row, uxcheckbox(win, "receipt", "Ask for a receipt", ZR), nil);
	box(main, "main-fill", "flex-1");
	btns = box(main, "btns", "h-flex justify-end gap=8");
	in(btns, uxbutton(win, "cancel", "Cancel", ZR), nil);
	in(btns, uxbutton(win, "send", "Send", ZR), nil);

	foot = box(nil, "foot", "h-flex justify-between");
	text(foot, "status", "3 messages, 1 unread", "text-sm text-color=grey4");
	icons = box(foot, "icons", "h-flex gap=6");
	svg(icons, "star", icstar, "w=16 h=16 text-color=olive");
	svg(icons, "heart", icheart, "w=16 h=16");
	svg(icons, "ring", icring, "w=16 h=16");
}

/* the same icons at several sizes, and absolute placement */
static void
svgscreen(void)
{
	Uxctl *row, *col, *abs;
	char *srcs[] = { icinbox, icsend, icfile, ictrash, icsearch, icgear, icstar, icheart, icring, icdisc, iclogo, icglenda };
	char *sizes[] = { "w=16 h=16", "w=24 h=24", "w=44 h=44" };
	char id[32];
	int i, j;

	if(uxwinstyle(win, "v-flex p=16 gap=14") < 0)
		sysfatal("root style: %r");
	text(nil, "title", "svg: one subset, three sizes", "text-lg font-bold");
	for(j = 0; j < nelem(sizes); j++){
		snprint(id, sizeof id, "row%d", j);
		row = box(nil, strdup(id), "h-flex gap=14");
		for(i = 0; i < nelem(srcs); i++){
			snprint(id, sizeof id, "s%d-%d", j, i);
			svg(row, strdup(id), srcs[i], sizes[j]);
		}
	}
	col = box(nil, "big", "h-flex gap=20 items-end");
	svg(col, "glenda", icglenda, "w=128 h=128");
	svg(col, "logo-big", iclogo, "w=96 h=96");
	svg(col, "sun-big", icgear, "w=96 h=96 text-color=deep");
	svg(col, "inbox-big", icinbox, "w=96 h=96 text-color=red");
	abs = box(nil, "abs", "h=60 border-1 bg=cream");
	text(abs, "abs-l", "absolute left=12 top=8", "absolute left=12 top=8 text-sm");
	svg(abs, "abs-s", icheart, "absolute right=12 bottom=8 w=32 h=32");
}

void
threadmain(int argc, char **argv)
{
	Uxmenu *m;
	char *screen;

	screen = "mail";
	ARGBEGIN{
	case 's':
		screen = EARGF(sysfatal("usage: spike [-s mail|svg]"));
		break;
	}ARGEND
	uxinit("spike", "Spike");
	m = uxmenu("file", "File");
	uxitem(m, "quit", "Quit", "q");
	uxaction("quit", quit, nil);
	win = uxwindow("window", "uitree spike", ZR);
	if(strcmp(screen, "svg") == 0)
		svgscreen();
	else
		mailscreen();
	uxmain();
	threadexitsall(nil);
}
