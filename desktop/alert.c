/*
 * Alerts: an error, or a question with two answers, in a modal dialog.
 * Desktop never decides for the user when a server can't do something
 * (design §4): the question says what happened and what else it can do.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <ux.h>
#include "dcore.h"
#include "desktop.h"
#include "uxstyle.h"

enum
{
	Width	= 420,
	Wrap	= 52,	/* characters per line of message */
	Maxlines	= 8,
};

static Alert	*alerts;

Alert*
alertbywin(Uxwin *w)
{
	Alert *a;

	for(a = alerts; a != nil; a = a->next)
		if(a->w == w)
			return a;
	return nil;
}

/* break msg into lines of at most Wrap characters, at spaces when it can */
static int
wrap(char *msg, char **line, int max)
{
	char *s, *p, *brk;
	Rune rr;
	int n, i;

	n = 0;
	s = msg;
	while(*s && n < max){
		brk = nil;
		for(p = s, i = 0; *p && *p != '\n' && i < Wrap; p += chartorune(&rr, p), i++)
			if(*p == ' ')
				brk = p;
		if(*p == 0 || *p == '\n')
			brk = p;
		else if(brk == nil)
			brk = p;
		line[n++] = smprint("%.*s", (int)(brk-s), s);
		s = brk;
		while(*s == ' ')
			s++;
		if(*s == '\n')
			s++;
	}
	return n;
}

Alert*
alert(char *msg, char *def, char *other, void (*fn)(Alert*, int))
{
	Alert *a;
	char *line[Maxlines], id[16];
	int n, i, h, y, bw;
	Uxctl *b;
	Rectangle r;

	n = wrap(msg, line, nelem(line));
	h = UXdialogmargin + (n > 2 ? n : 2)*UXlistrowheight + UXcontrolgroupgap + UXbuttonheight + UXdialogmargin;
	r = Rect(300, 200, 300+Width, 200+h);
	a = demalloc(sizeof *a);
	a->fn = fn;
	a->w = uxdialog("Desktop", r);
	r = Rect(0, 0, uxwinsize(a->w).x, uxwinsize(a->w).y);
	b = uxiconview(a->w, "icon", Rect(UXdialogmargin, UXdialogmargin, UXdialogmargin+UXdialogiconsize, UXdialogmargin+UXdialogiconsize));
	uxiconadd(b, "caution", "", fn != nil ? "alert-caution" : "alert-stop", Pt(UXdialogiconsize/2, UXdialogiconsize/2));
	y = UXdialogmargin;
	for(i = 0; i < n; i++){
		snprint(id, sizeof id, "msg%d", i+1);
		uxlabel(a->w, id, line[i], Rect(UXdialogtextx, y, r.max.x-UXdialogmargin, y+UXlistrowheight));
		free(line[i]);
		y += UXlistrowheight;
	}
	y = r.max.y - UXdialogmargin - UXbuttonheight;
	bw = UXbuttonminwidth + 16;
	b = uxbutton(a->w, "ok", def, Rect(r.max.x-UXdialogmargin-bw, y, r.max.x-UXdialogmargin, y+UXbuttonheight));
	uxhandler(b, alerthandler, a);
	uxdefault(a->w, b);
	if(other != nil){
		bw = UXbuttonminwidth + 8*strlen(other)/2;
		b = uxbutton(a->w, "other", other,
			Rect(r.max.x-2*UXdialogmargin-UXbuttonminwidth-16-bw, y, r.max.x-2*UXdialogmargin-UXbuttonminwidth-16, y+UXbuttonheight));
		uxhandler(b, alerthandler, a);
	}
	uxwinhandler(a->w, alerthandler, a);
	a->next = alerts;
	alerts = a;
	return a;
}

void
alertevent(Alert *a, Dev *e)
{
	Alert **l;
	int choice;

	for(l = &alerts; *l != nil; l = &(*l)->next)
		if(*l == a)
			break;
	if(*l == nil)
		return;	/* already answered */
	switch(e->type){
	case UXactivate:
		choice = strcmp(uxctlid(e->ctl), "other") == 0 ? 2 : 1;
		break;
	case UXdefault:
		choice = 1;
		break;
	case UXcancel:
	case UXclose:
		choice = 0;
		break;
	default:
		return;
	}
	*l = a->next;
	uxwinclose(a->w);
	if(a->fn != nil)
		a->fn(a, choice);
	free(a->path);
	free(a->arg);
	free(a);
}

/* show the error string: what failed, and the server's reason */
void
alerterr(char *what)
{
	char err[ERRMAX], *s;

	rerrstr(err, sizeof err);
	s = smprint("%s: %s", what, err);
	fprint(2, "desktop: %s\n", s);
	alert(s, "OK", nil, nil);
	free(s);
}
