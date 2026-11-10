/*
 * Control types: their files, verbs and behavior (ux.md, Control types).
 * Everything here runs with uxlk held.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <mouse.h>
#include <keyboard.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include "ux.h"
#include "impl.h"

static void
dirty(Uxctl *c)
{
	c->w->dirty = 1;
}

static void
note(Uxctl *c, int type, int via)
{
	Uxev e;

	memset(&e, 0, sizeof e);
	e.type = type;
	e.via = via;
	e.win = c->w;
	e.ctl = c;
	_notify(c->fn, c->arg, &e);
}

/* button */

void
_activate(Uxctl *c, int via)
{
	_event(c, "activate", "via", _via(via), nil);
	note(c, UXactivate, via);
}

static char*
vactivate(Uxctl *c, int, char**)
{
	_activate(c, UXctl);
	return nil;
}

/* text field */

static void
valueevent(Uxctl *c)
{
	char *s;

	if(c->state & UXsecret){
		_event(c, "value", "redacted", "on", nil);
		return;
	}
	s = _runestr(c->text, c->ntext);
	if(strlen(s) <= Maxevvalue && strchr(s, '\n') == nil)
		_event(c, "value", "value", s, nil);
	else
		_event(c, "value", nil);
	free(s);
}

static void
selevent(Uxctl *c)
{
	char a[16], b[16];

	snprint(a, sizeof a, "%d", c->q0);
	snprint(b, sizeof b, "%d", c->q1);
	_event(c, "select", "q0", a, "q1", b, nil);
}

/* replace runes q0..q1 with s */
static void
replace(Uxctl *c, int q0, int q1, char *s)
{
	Rune *r;
	int n, i;

	n = utflen(s);
	if(!(c->state & UXmultiline)){
		/* one-line fields keep no newlines */
		r = runesmprint("%s", s);
		for(i = 0; i < n; i++)
			if(r[i] == '\n')
				r[i] = ' ';
	}else
		r = runesmprint("%s", s);
	/* move the tail before shrinking, after growing */
	if(n < q1-q0)
		memmove(c->text+q0+n, c->text+q1, (c->ntext-q1)*sizeof(Rune));
	c->text = erealloc(c->text, (c->ntext - (q1-q0) + n + 1)*sizeof(Rune));
	if(n > q1-q0)
		memmove(c->text+q0+n, c->text+q1, (c->ntext-q1)*sizeof(Rune));
	memmove(c->text+q0, r, n*sizeof(Rune));
	c->ntext += n - (q1-q0);
	free(r);
	c->q0 = c->q1 = q0+n;
	dirty(c);
}

char*
_settext(Uxctl *c, char *s, int via)
{
	if(strlen(s) > Maxvalue)
		return "too large";
	replace(c, 0, c->ntext, s);
	valueevent(c);
	note(c, UXvalue, via);
	return nil;
}

static char*
vset(Uxctl *c, int, char **argv)
{
	if(c->state & UXreadonly)
		return "bad value: readonly";
	return _settext(c, argv[1], UXctl);
}

static char*
vinsert(Uxctl *c, int, char **argv)
{
	if(c->state & UXreadonly)
		return "bad value: readonly";
	replace(c, c->q0, c->q1, argv[1]);
	valueevent(c);
	selevent(c);
	note(c, UXvalue, UXctl);
	return nil;
}

/*
 * Cut, copy, paste and select all on a text field, with the clipboard
 * (clip.c: clipd's text if it runs, rio's snarf otherwise). uxlk held.
 */
char*
_tfedit(Uxctl *c, char *cmd)
{
	char *s;
	long n;

	if(c == nil || strcmp(c->t->name, "text-field") != 0)
		return "not found: no text field has focus";
	if(strcmp(cmd, "selectall") == 0){
		c->q0 = 0;
		c->q1 = c->ntext;
		dirty(c);
		selevent(c);
		return nil;
	}
	if(strcmp(cmd, "copy") == 0 || strcmp(cmd, "cut") == 0){
		if(c->state & UXsecret)
			return "permission denied: a secret field";
		if(cmd[1] == 'u' && (c->state & UXreadonly))
			return "bad value: readonly";
		if(c->q1 > c->q0){
			s = _runestr(c->text+c->q0, c->q1-c->q0);
			n = _clipput(s);
			free(s);
			if(n < 0)
				return "failed: can't write the clipboard";
		}
		if(cmd[1] == 'u' && c->q1 > c->q0){
			replace(c, c->q0, c->q1, "");
			valueevent(c);
			selevent(c);
			note(c, UXvalue, UXui);
		}
		return nil;
	}
	if(strcmp(cmd, "paste") == 0){
		if(c->state & UXreadonly)
			return "bad value: readonly";
		s = _clipget();
		if(s == nil)
			return "failed: can't read the clipboard";
		replace(c, c->q0, c->q1, s);
		free(s);
		valueevent(c);
		selevent(c);
		note(c, UXvalue, UXui);
		return nil;
	}
	return "unknown command";
}

static int
num(char *s, int *v)
{
	char *p;

	*v = strtol(s, &p, 10);
	return *s != 0 && *p == 0;
}

static char*
vselect(Uxctl *c, int, char **argv)
{
	int q0, q1;

	if(!num(argv[1], &q0) || !num(argv[2], &q1))
		return "bad syntax";
	if(q0 < 0 || q1 < q0 || q1 > c->ntext)
		return "bad value: selection out of range";
	c->q0 = q0;
	c->q1 = q1;
	dirty(c);
	selevent(c);
	return nil;
}

void
_setfocus(Uxwin *w, Uxctl *c, int)
{
	char *p;

	if(w->focus == c)
		return;
	if(w->focus)
		dirty(w->focus);
	w->focus = c;
	if(c){
		dirty(c);
		p = _ctlpath(c);
		_event(w, "focus", "ctl", p, nil);
		free(p);
	}else
		_event(w, "focus", nil);
}

static char*
vfocus(Uxctl *c, int, char**)
{
	_setfocus(c->w, c, UXctl);
	return nil;
}

static void
submit(Uxctl *c, int via)
{
	_event(c, "submit", "via", _via(via), nil);
	note(c, UXsubmit, via);
}

static char*
vsubmit(Uxctl *c, int, char**)
{
	if(c->state & UXmultiline)
		return "bad value: multi-line field";
	submit(c, UXctl);
	return nil;
}

static char*
tfwritevalue(Uxctl *c, char *s)
{
	if(c->state & UXreadonly)
		return "bad value: readonly";
	return _settext(c, s, UXctl);
}

static int
tfkey(Uxctl *c, Rune r)
{
	char buf[UTFmax+1];
	int n;

	switch(r){
	case '\n':
		if(!(c->state & UXmultiline)){
			submit(c, UXui);
			return 1;
		}
		break;
	case Kleft:
		if(c->q0 > 0)
			c->q0--;
		c->q1 = c->q0;
		dirty(c);
		selevent(c);
		return 1;
	case Kright:
		if(c->q1 < c->ntext)
			c->q1++;
		c->q0 = c->q1;
		dirty(c);
		selevent(c);
		return 1;
	case Khome:
	case Kend:
		c->q0 = c->q1 = r == Khome ? 0 : c->ntext;
		dirty(c);
		selevent(c);
		return 1;
	}
	if(c->state & UXreadonly)
		return 0;
	if(r == Kbs || r == 0x15 || r == 0x17){	/* backspace, ^U, ^W */
		if(c->q0 == c->q1 && c->q0 > 0){
			if(r == Kbs)
				c->q0--;
			else if(r == 0x15)
				c->q0 = 0;
			else{
				while(c->q0 > 0 && isspacerune(c->text[c->q0-1]))
					c->q0--;
				while(c->q0 > 0 && !isspacerune(c->text[c->q0-1]))
					c->q0--;
			}
		}
		replace(c, c->q0, c->q1, "");
	}else if(r == '\t' || r >= ' ' || r == '\n'){
		if(r >= KF && r < KF+0x1000)
			return 0;
		n = runetochar(buf, &r);
		buf[n] = 0;
		replace(c, c->q0, c->q1, buf);
	}else
		return 0;
	valueevent(c);
	note(c, UXvalue, UXui);
	return 1;
}

static int
tfmouse(Uxctl *c, Mouse *m)
{
	Rectangle r;
	Font *f;
	int q, x;

	if(!(m->buttons & 1))
		return 0;
	_setfocus(c->w, c, UXui);
	f = c->w->font;
	if(f == nil)
		return 0;
	r = _abs(c);
	x = r.min.x + 4;
	for(q = c->org; q < c->ntext; q++){
		x += runestringnwidth(f, c->text+q, 1);
		if(x > m->xy.x)
			break;
	}
	c->q0 = c->q1 = q;
	dirty(c);
	selevent(c);
	return 0;
}

/* checkbox */

void
_setcheck(Uxctl *c, int v, int via)
{
	c->check = v;
	dirty(c);
	_event(c, "value", "value", v == 2 ? "mixed" : v ? "on" : "off", nil);
	note(c, UXvalue, via);
}

static char*
vcheckset(Uxctl *c, int, char **argv)
{
	if(strcmp(argv[1], "on") == 0)
		_setcheck(c, 1, UXctl);
	else if(strcmp(argv[1], "off") == 0)
		_setcheck(c, 0, UXctl);
	else
		return "bad value: on or off";
	return nil;
}

static char*
vtoggle(Uxctl *c, int, char**)
{
	_setcheck(c, c->check != 1, UXctl);
	return nil;
}

/* list */

char*
_listsel(Uxctl *c, int sep)
{
	Fmt f;
	int i, n;

	fmtstrinit(&f);
	n = 0;
	for(i = 0; i < c->nitems; i++)
		if(c->sel[i]){
			if(n++)
				fmtprint(&f, "%c", sep);
			fmtprint(&f, "%d", i);
		}
	return fmtstrflush(&f);
}

void
_setsel(Uxctl *c, uchar *sel, int via)
{
	char *s;
	Uxev e;

	memmove(c->sel, sel, c->nitems);
	dirty(c);
	s = _listsel(c, ',');
	_event(c, "select", "sel", s, nil);
	free(s);
	memset(&e, 0, sizeof e);
	e.type = UXselection;
	e.via = via;
	e.win = c->w;
	e.ctl = c;
	_notify(c->fn, c->arg, &e);
}

static char*
vlistselect(Uxctl *c, int argc, char **argv)
{
	uchar *sel;
	int i, n;

	sel = emalloc(c->nitems+1);
	for(i = 1; i < argc; i++){
		if(!num(argv[i], &n)){
			free(sel);
			return "bad syntax";
		}
		if(n < 0 || n >= c->nitems){
			free(sel);
			return "bad value: no such item";
		}
		sel[n] = 1;
	}
	_setsel(c, sel, UXctl);
	free(sel);
	return nil;
}

void
_listopen(Uxctl *c, int n, int via)
{
	char buf[16];
	Uxev e;

	snprint(buf, sizeof buf, "%d", n);
	_event(c, "open", "item", buf, "via", _via(via), nil);
	memset(&e, 0, sizeof e);
	e.type = UXopen;
	e.via = via;
	e.win = c->w;
	e.ctl = c;
	e.n = n;
	_notify(c->fn, c->arg, &e);
}

static char*
vlistopen(Uxctl *c, int, char **argv)
{
	int n;

	if(!num(argv[1], &n))
		return "bad syntax";
	if(n < 0 || n >= c->nitems)
		return "bad value: no such item";
	_listopen(c, n, UXctl);
	return nil;
}

static int
lineheight(Uxctl*)
{
	return UXlistrowheight;
}

static int
listmouse(Uxctl *c, Mouse *m)
{
	Rectangle r;
	uchar *sel;
	int n;

	if(!(m->buttons & 1))
		return 0;
	r = _abs(c);
	if(c->t == ctltypes[8])
		r.min.y += UXlistrowheight + 1;
	n = c->top + (m->xy.y - r.min.y - 1) / lineheight(c);
	if(n < 0 || n >= c->nitems)
		return 0;
	if(c->lastitem == n && m->msec - c->lastclick < 400){
		c->lastclick = 0;
		if(c->t == ctltypes[8])	/* file-chooser */
			return 2+n;
		_listopen(c, n, UXui);
		return 0;
	}
	c->lastitem = n;
	c->lastclick = m->msec;
	sel = emalloc(c->nitems+1);
	sel[n] = 1;
	_setsel(c, sel, UXui);
	free(sel);
	return 0;
}

static int
listkey(Uxctl *c, Rune r)
{
	uchar *sel;
	int i, cur;

	cur = -1;
	for(i = 0; i < c->nitems; i++)
		if(c->sel[i]){
			cur = i;
			break;
		}
	switch(r){
	case Kup:
		cur = cur <= 0 ? 0 : cur-1;
		break;
	case Kdown:
		cur = cur+1 >= c->nitems ? c->nitems-1 : cur+1;
		break;
	case '\n':
		if(cur >= 0)
			_listopen(c, cur, UXui);
		return 1;
	default:
		return 0;
	}
	if(cur < 0)
		return 1;
	sel = emalloc(c->nitems+1);
	sel[cur] = 1;
	_setsel(c, sel, UXui);
	free(sel);
	return 1;
}

/* icon view */

static int
findicon(Uxctl *c, char *id)
{
	int i;

	for(i = 0; i < c->nicons; i++)
		if(strcmp(c->icons[i].id, id) == 0)
			return i;
	return -1;
}

static char*
iconsel(Uxctl *c, int sep)
{
	Fmt f;
	int i, n;

	fmtstrinit(&f);
	n = 0;
	for(i = 0; i < c->nicons; i++)
		if(c->icons[i].sel){
			if(n++)
				fmtprint(&f, "%c", sep);
			fmtprint(&f, "%s", c->icons[i].id);
		}
	return fmtstrflush(&f);
}

void
_iconsel(Uxctl *c, int via)
{
	char *s;
	Uxev e;

	dirty(c);
	s = iconsel(c, ',');
	_event(c, "select", "sel", s, nil);
	free(s);
	memset(&e, 0, sizeof e);
	e.type = UXselection;
	e.via = via;
	e.win = c->w;
	e.ctl = c;
	_notify(c->fn, c->arg, &e);
}

static char*
viconselect(Uxctl *c, int argc, char **argv)
{
	int i, j;

	for(i = 1; i < argc; i++)
		if(findicon(c, argv[i]) < 0)
			return "not found";
	for(j = 0; j < c->nicons; j++)
		c->icons[j].sel = 0;
	for(i = 1; i < argc; i++)
		c->icons[findicon(c, argv[i])].sel = 1;
	_iconsel(c, UXctl);
	return nil;
}

void
_iconopen(Uxctl *c, char *items, int via)
{
	Uxev e;
	char *p;

	_event(c, "open", "items", items, "via", _via(via), nil);
	/* one handler call per item */
	items = estrdup(items);
	for(p = strtok(items, ","); p != nil; p = strtok(nil, ",")){
		memset(&e, 0, sizeof e);
		e.type = UXopen;
		e.via = via;
		e.win = c->w;
		e.ctl = c;
		e.item = p;
		_notify(c->fn, c->arg, &e);
	}
	free(items);
}

static char*
viconopen(Uxctl *c, int argc, char **argv)
{
	Fmt f;
	char *s;
	int i;

	for(i = 1; i < argc; i++)
		if(findicon(c, argv[i]) < 0)
			return "not found";
	fmtstrinit(&f);
	for(i = 1; i < argc; i++)
		fmtprint(&f, i > 1 ? ",%s" : "%s", argv[i]);
	s = fmtstrflush(&f);
	_iconopen(c, s, UXctl);
	free(s);
	return nil;
}

static int
parsept(char *s, Point *p)
{
	char *q;

	p->x = strtol(s, &q, 10);
	if(q == s || *q != ',')
		return 0;
	s = q+1;
	p->y = strtol(s, &q, 10);
	return q != s && *q == 0;
}

static char*
viconmove(Uxctl *c, int, char **argv)
{
	int i;
	Point p;
	Uxev e;

	if((i = findicon(c, argv[1])) < 0)
		return "not found";
	if(!parsept(argv[2], &p))
		return "bad syntax";
	c->icons[i].pt = p;
	dirty(c);
	_event(c, "items", nil);
	memset(&e, 0, sizeof e);
	e.type = UXmove;
	e.via = UXctl;
	e.win = c->w;
	e.ctl = c;
	e.item = c->icons[i].id;
	e.pt = p;
	_notify(c->fn, c->arg, &e);
	return nil;
}

static char*
viconrename(Uxctl *c, int, char **argv)
{
	int i;
	Uxev e;

	if((i = findicon(c, argv[1])) < 0)
		return "not found";
	if(argv[2][0] == 0 || strchr(argv[2], '\n'))
		return "bad value: name";
	free(c->icons[i].label);
	c->icons[i].label = estrdup(argv[2]);
	dirty(c);
	_event(c, "items", nil);
	memset(&e, 0, sizeof e);
	e.type = UXrename;
	e.via = UXctl;
	e.win = c->w;
	e.ctl = c;
	e.item = c->icons[i].id;
	e.text = argv[2];
	_notify(c->fn, c->arg, &e);
	return nil;
}

static int
iconat(Uxctl *c, Point p)
{
	Rectangle r, ir;
	int i;

	r = _abs(c);
	for(i = c->nicons-1; i >= 0; i--){
		ir = Rect(-UXiconviewcellw/2, -UXiconlarge/2, UXiconviewcellw/2, UXiconviewcellh-UXiconlarge/2);
		ir = rectaddpt(ir, subpt(addpt(r.min, c->icons[i].pt), c->origin));
		if(ptinrect(p, ir))
			return i;
	}
	return -1;
}

int	(*_icondragout)(Uxctl*, Mouse*);

static Rectangle
iconrect(Uxctl *c, int i)
{
	Rectangle r, ir;

	r = _abs(c);
	ir = Rect(-UXiconlarge/2, -UXiconlarge/2, UXiconlarge/2, UXiconviewcellh-UXiconlarge/2);
	return rectaddpt(ir, subpt(addpt(r.min, c->icons[i].pt), c->origin));
}

static int
nsel(Uxctl *c)
{
	int i, n;

	n = 0;
	for(i = 0; i < c->nicons; i++)
		n += c->icons[i].sel;
	return n;
}

/* move the selected icons by d (live feedback while dragging) */
static void
movesel(Uxctl *c, Point d)
{
	int i;

	for(i = 0; i < c->nicons; i++)
		if(c->icons[i].sel)
			c->icons[i].pt = addpt(c->icons[i].pt, d);
	dirty(c);
}

/* the drag ended with the icons moved: report each one */
static void
moved(Uxctl *c)
{
	Uxev e;
	int i;

	_event(c, "items", nil);
	for(i = 0; i < c->nicons; i++){
		if(!c->icons[i].sel)
			continue;
		memset(&e, 0, sizeof e);
		e.type = UXmove;
		e.via = UXui;
		e.win = c->w;
		e.ctl = c;
		e.item = c->icons[i].id;
		e.pt = c->icons[i].pt;
		_notify(c->fn, c->arg, &e);
	}
}

/*
 * Button 1 on an icon selects it (or keeps a selection it's part of)
 * and dragging moves the selection; on empty space it starts a rubber
 * band. A double click opens the selection. Returns 1 while the button
 * is down so the window's grab keeps the mouse for us.
 */
static int
iconmouse(Uxctl *c, Mouse *m)
{
	int i, j;
	char *s;
	Rectangle b;

	switch(c->drag){
	case Dnone:
		if(!(m->buttons & 1))
			return 0;
		i = iconat(c, m->xy);
		if(i >= 0 && c->lastitem == i && m->msec - c->lastclick < 400){
			c->lastclick = 0;
			s = iconsel(c, ',');
			_iconopen(c, s, UXui);
			free(s);
			c->drag = Dpress;	/* swallow the rest of the click */
			c->dragp = c->dragl = m->xy;
			c->lastitem = -1;
			return 1;
		}
		c->lastitem = i;
		c->lastclick = m->msec;
		c->dragp = c->dragl = m->xy;
		if(i >= 0){
			if(!c->icons[i].sel){
				for(j = 0; j < c->nicons; j++)
					c->icons[j].sel = j == i;
				_iconsel(c, UXui);
			}
			c->drag = Dpress;
		}else{
			if(nsel(c) > 0){
				for(j = 0; j < c->nicons; j++)
					c->icons[j].sel = 0;
				_iconsel(c, UXui);
			}
			c->drag = Dband;
			c->band = Rpt(m->xy, m->xy);
		}
		return 1;
	case Dpress:
		if(!(m->buttons & 1)){
			/* a click on one of several selected icons selects just it */
			i = iconat(c, m->xy);
			if(i >= 0 && nsel(c) > 1 && c->lastclick != 0){
				for(j = 0; j < c->nicons; j++)
					c->icons[j].sel = j == i;
				_iconsel(c, UXui);
			}
			c->drag = Dnone;
			return 0;
		}
		if(abs(m->xy.x - c->dragp.x) + abs(m->xy.y - c->dragp.y) < 4 || c->lastitem < 0)
			return 1;
		c->drag = Dmove;
		/* fall through */
	case Dmove:
		if(!ptinrect(m->xy, _abs(c)) && _icondragout != nil && _icondragout(c, m)){
			movesel(c, subpt(c->dragp, c->dragl));
			c->drag = Dnone;
			return 0;
		}
		movesel(c, subpt(m->xy, c->dragl));
		c->dragl = m->xy;
		if(!(m->buttons & 1)){
			c->drag = Dnone;
			if(!eqpt(c->dragp, m->xy))
				moved(c);
			return 0;
		}
		return 1;
	case Dband:
		c->band = canonrect(Rpt(c->dragp, m->xy));
		for(i = 0; i < c->nicons; i++){
			b = iconrect(c, i);
			c->icons[i].sel = rectXrect(b, c->band);
		}
		dirty(c);
		if(!(m->buttons & 1)){
			c->drag = Dnone;
			_iconsel(c, UXui);
			return 0;
		}
		return 1;
	}
	c->drag = Dnone;
	return 0;
}

/* scroll view */

void
_setorigin(Uxctl *c, Point p, int via)
{
	char buf[32];
	Uxev e;

	c->origin = p;
	dirty(c);
	snprint(buf, sizeof buf, "%d,%d", p.x, p.y);
	_event(c, "origin", "pt", buf, nil);
	memset(&e, 0, sizeof e);
	e.type = UXscroll;
	e.via = via;
	e.win = c->w;
	e.ctl = c;
	e.pt = p;
	_notify(c->fn, c->arg, &e);
}

static char*
vscroll(Uxctl *c, int, char **argv)
{
	Point p;

	if(!parsept(argv[1], &p))
		return "bad syntax";
	_setorigin(c, p, UXctl);
	return nil;
}

static int
scrollmouse(Uxctl *c, Mouse *m)
{
	int d;

	d = lineheight(c)*3;
	if(m->buttons & 8)
		_setorigin(c, Pt(c->origin.x, c->origin.y - d < c->extent.min.y ? c->extent.min.y : c->origin.y - d), UXui);
	else if(m->buttons & 16)
		_setorigin(c, Pt(c->origin.x, c->origin.y + d), UXui);
	return 0;
}

/* file chooser */

static int
dircmp(void *a, void *b)
{
	return strcmp(*(char**)a, *(char**)b);
}

void
_chooserload(Uxctl *c)
{
	Dir *d;
	int fd, n, i;

	for(i = 0; i < c->nitems; i++)
		free(c->items[i]);
	free(c->items);
	free(c->sel);
	c->items = nil;
	c->nitems = 0;
	fd = open(c->dir, OREAD);
	if(fd >= 0){
		n = dirreadall(fd, &d);
		close(fd);
		if(n > 0){
			c->items = emalloc(n*sizeof(char*));
			for(i = 0; i < n; i++)
				c->items[i] = smprint("%s%s", d[i].name, (d[i].qid.type&QTDIR) ? "/" : "");
			c->nitems = n;
			qsort(c->items, n, sizeof(char*), dircmp);
			free(d);
		}
	}
	c->sel = emalloc(c->nitems+1);
	c->top = 0;
	dirty(c);
}

static void
chooserevent(Uxctl *c, char *ev, char *path, int type, int via)
{
	Uxev e;

	_event(c, ev, "path", path, nil);
	if(type == 0)
		return;
	memset(&e, 0, sizeof e);
	e.type = type;
	e.via = via;
	e.win = c->w;
	e.ctl = c;
	e.text = path;
	_notify(c->fn, c->arg, &e);
}

static char*
choosercd(Uxctl *c, char *path, int via)
{
	char *p;
	Dir *d;

	if(path[0] == '/')
		p = estrdup(path);
	else
		p = smprint("%s/%s", c->dir, path);
	cleanname(p);
	d = dirstat(p);
	if(d == nil || !(d->qid.type & QTDIR)){
		free(d);
		free(p);
		return "not found";
	}
	free(d);
	free(c->dir);
	c->dir = p;
	_chooserload(c);
	chooserevent(c, "dir", c->dir, 0, via);
	return nil;
}

static char*
vcd(Uxctl *c, int, char **argv)
{
	return choosercd(c, argv[1], UXctl);
}

static char*
choose(Uxctl *c, char *path, int via)
{
	char *p;

	if(path[0] == '/')
		p = estrdup(path);
	else
		p = smprint("%s/%s", c->dir, path);
	cleanname(p);
	free(c->name);
	c->name = estrdup(utfrrune(p, '/') ? utfrrune(p, '/')+1 : p);
	_event(c, "value", "value", c->name, nil);
	chooserevent(c, "choose", p, UXchoose, via);
	free(p);
	return nil;
}

static char*
vchoose(Uxctl *c, int, char **argv)
{
	return choose(c, argv[1], UXctl);
}

static char*
vccancel(Uxctl *c, int, char**)
{
	Uxev e;

	_event(c, "cancel", nil);
	memset(&e, 0, sizeof e);
	e.type = UXcancel;
	e.via = UXctl;
	e.win = c->w;
	e.ctl = c;
	_notify(c->fn, c->arg, &e);
	return nil;
}

static int
choosermouse(Uxctl *c, Mouse *m)
{
	int n;
	char *s;

	n = listmouse(c, m);
	if(n >= 2){
		n -= 2;
		s = estrdup(c->items[n]);
		if(s[0] && s[strlen(s)-1] == '/'){
			s[strlen(s)-1] = 0;
			choosercd(c, s, UXui);
		}else
			choose(c, s, UXui);
		free(s);
	}
	return 0;
}

/* types */

static char *nofiles[] = { nil };
static char *tffiles[] = { "value", "selection", nil };
static char *valfiles[] = { "value", nil };
static char *listfiles[] = { "items", "selection", nil };
static char *scrollfiles[] = { "origin", "extent", nil };
static char *chooserfiles[] = { "dir", "value", nil };

static Verb buttonverbs[] = {
	"activate", Ractivate, 0, 0, vactivate,
	nil,
};

static Verb tfverbs[] = {
	"set", Redit, 1, 1, vset,
	"insert", Redit, 1, 1, vinsert,
	"select", Redit, 2, 2, vselect,
	"focus", Ractivate, 0, 0, vfocus,
	"submit", Ractivate, 0, 0, vsubmit,
	nil,
};

static Verb checkverbs[] = {
	"set", Ractivate, 1, 1, vcheckset,
	"toggle", Ractivate, 0, 0, vtoggle,
	nil,
};

static Verb noverbs[] = {
	nil,
};

static Verb listverbs[] = {
	"select", Ractivate, 0, -1, vlistselect,
	"open", Ractivate, 1, 1, vlistopen,
	nil,
};

static Verb iconverbs[] = {
	"select", Ractivate, 0, -1, viconselect,
	"open", Ractivate, 1, -1, viconopen,
	"move", Redit, 2, 2, viconmove,
	"rename", Redit, 2, 2, viconrename,
	nil,
};

static Verb scrollverbs[] = {
	"scroll", Ractivate, 1, 1, vscroll,
	nil,
};

static Verb chooserverbs[] = {
	"cd", Ractivate, 1, 1, vcd,
	"choose", Redit, 1, 1, vchoose,
	"cancel", Ractivate, 0, 0, vccancel,
	nil,
};

static int
buttonmouse(Uxctl *c, Mouse *m)
{
	Rectangle r;

	r = _abs(c);
	if(m->buttons & 1){
		if(!c->pressed)
			c->pressed = 1;
		c->pressed = ptinrect(m->xy, r) ? 1 : 2;
		dirty(c);
		return 1;
	}
	if(c->pressed){
		if(c->pressed == 1 && ptinrect(m->xy, r))
			_activate(c, UXui);
		c->pressed = 0;
		dirty(c);
	}
	return 0;
}

static int
checkmouse(Uxctl *c, Mouse *m)
{
	if(m->buttons & 1){
		c->pressed = 1;
		return 1;
	}
	if(c->pressed && ptinrect(m->xy, _abs(c)))
		_setcheck(c, c->check != 1, UXui);
	c->pressed = 0;
	return 0;
}

static int
checkkey(Uxctl *c, Rune r)
{
	if(r == ' '){
		_setcheck(c, c->check != 1, UXui);
		return 1;
	}
	return 0;
}

static int
buttonkey(Uxctl *c, Rune r)
{
	if(r == ' '){
		_activate(c, UXui);
		return 1;
	}
	return 0;
}

static Ctltype buttontype = { "button", nofiles, buttonverbs, nil, nil, nil, buttonmouse, buttonkey, 1 };
static Ctltype tftype = { "text-field", tffiles, tfverbs, nil, tfwritevalue, nil, tfmouse, tfkey, 1 };
static Ctltype checktype = { "checkbox", valfiles, checkverbs, nil, nil, nil, checkmouse, checkkey, 1 };
static Ctltype labeltype = { "label", valfiles, noverbs, nil, nil, nil, nil, nil, 0 };
static Ctltype grouptype = { "group", nofiles, noverbs, nil, nil, nil, nil, nil, 0 };
static Ctltype listtype = { "list", listfiles, listverbs, nil, nil, nil, listmouse, listkey, 1 };
static Ctltype icontype = { "icon-view", listfiles, iconverbs, nil, nil, nil, iconmouse, nil, 1 };
static Ctltype scrolltype = { "scroll-view", scrollfiles, scrollverbs, nil, nil, nil, scrollmouse, nil, 0 };
static Ctltype choosertype = { "file-chooser", chooserfiles, chooserverbs, nil, nil, nil, choosermouse, listkey, 1 };

Ctltype *ctltypes[] = {
	&buttontype,	/* 0 */
	&tftype,
	&checktype,
	&labeltype,
	&grouptype,
	&listtype,
	&icontype,
	&scrolltype,
	&choosertype,	/* 8 */
	nil,
};

int
_hasfile(Uxctl *c, char *name)
{
	char **f;

	for(f = c->t->files; *f; f++)
		if(strcmp(*f, name) == 0)
			return 1;
	return 0;
}

static char*
ctlstate(Uxctl *c)
{
	Fmt f;

	fmtstrinit(&f);
	if(c->state & Cenabled)	fmtprint(&f, " enabled");
	if(c->state & Cvisible)	fmtprint(&f, " visible");
	if(c->w->focus == c)	fmtprint(&f, " focused");
	if(c->state & Cdefault)	fmtprint(&f, " default");
	if(c->state & UXreadonly)	fmtprint(&f, " readonly");
	if(c->state & UXmultiline)	fmtprint(&f, " multiline");
	if(c->state & UXsensitive)	fmtprint(&f, " sensitive");
	if(c->state & UXsecret)	fmtprint(&f, " secret");
	return fmtstrflush(&f);
}

/* a snapshot of one of c's files */
char*
_ctlread(Uxctl *c, char *file)
{
	char *s, *t;
	Fmt f;
	int i;

	if(strcmp(file, "type") == 0)
		return smprint("%s\n", c->t->name);
	if(strcmp(file, "label") == 0)
		return smprint("%s\n", c->label);
	if(strcmp(file, "state") == 0){
		t = ctlstate(c);
		s = smprint("%s\n", t[0] ? t+1 : t);
		free(t);
		return s;
	}
	if(strcmp(file, "rect") == 0)
		return smprint("%d %d %d %d\n", c->r.min.x, c->r.min.y, c->r.max.x, c->r.max.y);
	if(strcmp(file, "parent") == 0)
		return smprint("%s\n", c->parent ? c->parent->id : "");
	if(strcmp(file, "ctl") == 0)
		return _ctlhelp(c);
	if(strcmp(file, "value") == 0){
		if(c->t == &checktype)
			return smprint("%s\n", c->check == 2 ? "mixed" : c->check ? "on" : "off");
		if(c->t == &choosertype)
			return smprint("%s\n", c->name ? c->name : "");
		if(c->state & UXsecret)
			return estrdup(c->state & UXmultiline ? "" : "\n");
		t = _runestr(c->text, c->ntext);
		if(c->state & UXmultiline)
			return t;
		s = smprint("%s\n", t);
		free(t);
		return s;
	}
	if(strcmp(file, "selection") == 0){
		if(c->t == &tftype)
			return smprint("%d %d\n", c->q0, c->q1);
		if(c->t == &icontype)
			t = iconsel(c, ' ');
		else
			t = _listsel(c, ' ');
		s = smprint("%s\n", t);
		free(t);
		return s;
	}
	if(strcmp(file, "items") == 0){
		fmtstrinit(&f);
		if(c->t == &icontype){
			for(i = 0; i < c->nicons; i++){
				t = _attr("label", c->icons[i].label);
				s = _attr("icon", c->icons[i].icon);
				fmtprint(&f, "item %s %s %s pos=%d,%d%s\n", c->icons[i].id, t, s,
					c->icons[i].pt.x, c->icons[i].pt.y, c->icons[i].sel ? " state=selected" : "");
				free(t);
				free(s);
			}
		}else
			for(i = 0; i < c->nitems; i++)
				fmtprint(&f, "%s\n", c->items[i]);
		return fmtstrflush(&f);
	}
	if(strcmp(file, "origin") == 0)
		return smprint("%d %d\n", c->origin.x, c->origin.y);
	if(strcmp(file, "extent") == 0)
		return smprint("%d %d %d %d\n", c->extent.min.x, c->extent.min.y, c->extent.max.x, c->extent.max.y);
	if(strcmp(file, "dir") == 0)
		return smprint("%s\n", c->dir);
	return nil;
}

Uxctl*
_newctl(Uxwin *w, char *id, Ctltype *t, char *label, Rectangle r)
{
	Uxctl *c;
	char *p;

	if(!_validid(id))
		sysfatal("libux: bad control id %q", id);
	if(_findctl(w, id) != nil)
		sysfatal("libux: duplicate control id %q", id);
	c = emalloc(sizeof *c);
	c->w = w;
	c->id = estrdup(id);
	c->t = t;
	c->label = estrdup(label);
	c->r = r;
	c->state = Cenabled|Cvisible;
	c->lastitem = -1;
	_addobj(c, Octl);
	w->ctl = erealloc(w->ctl, (w->nctl+1)*sizeof(Uxctl*));
	w->ctl[w->nctl++] = c;
	w->dirty = 1;
	p = _ctlpath(c);
	_event(w, "new", "ctl", p, "type", t->name, nil);
	free(p);
	return c;
}
