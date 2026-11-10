/*
 * Menus: served under menus/ (ux.md, Menus) and registered with rio's
 * menu bar through /dev/ux/menubar (devux.md, Menu bar). uxlk held
 * unless noted.
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
bar(char *fmt, ...)
{
	va_list arg;
	char *s;

	if(ux.menufd < 0)
		return;
	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	if(write(ux.menufd, s, strlen(s)) < 0)
		fprint(2, "libux: menubar: %s: %r\n", s);
	free(s);
}

/* an item's attributes, as in the devux item grammar */
static char*
itemattrs(Item *it)
{
	Fmt f;
	char *a;

	fmtstrinit(&f);
	if(it->sep){
		fmtprint(&f, " sep=on");
		return fmtstrflush(&f);
	}
	a = _attr("label", it->label);
	fmtprint(&f, " %s", a);
	free(a);
	if(it->key && it->key[0]){
		a = _attr("key", it->key);
		fmtprint(&f, " %s", a);
		free(a);
	}
	if(!it->enabled)
		fmtprint(&f, " enabled=off");
	if(it->check)
		fmtprint(&f, " check=%s", it->check == 2 ? "mixed" : "on");
	return fmtstrflush(&f);
}

char*
_menuitems(Uxmenu *m)
{
	Fmt f;
	char *a;
	int i;

	fmtstrinit(&f);
	for(i = 0; i < m->nitem; i++){
		a = itemattrs(&m->item[i]);
		fmtprint(&f, "item %s%s\n", m->item[i].action, a);
		free(a);
	}
	return fmtstrflush(&f);
}

static void
barmenu(Uxmenu *m)
{
	char *a, *t;
	int i;

	t = _attr("title", m->title);
	bar("menu %s %s\n", m->id, t);
	free(t);
	for(i = 0; i < m->nitem; i++){
		a = itemattrs(&m->item[i]);
		bar("item %s %s%s\n", m->id, m->item[i].action, a);
		free(a);
	}
}

static void
barreader(void*)
{
	char buf[8192], *f[16], *p, *e, *app, *line;
	int fd, n, nf, i, off;

	threadsetname("uxmenubar");
	fd = open("/dev/ux/menubar/events", OREAD);
	if(fd < 0)
		return;
	off = 0;
	while((n = read(fd, buf+off, sizeof buf-1-off)) > 0){
		n += off;
		buf[n] = 0;
		p = buf;
		while((e = strchr(p, '\n')) != nil){
			*e = 0;
			line = estrdup(p);
			nf = tokenize(line, f, nelem(f));
			app = nil;
			for(i = 2; i < nf; i++)
				if(strncmp(f[i], "app=", 4) == 0)
					app = f[i]+4;
			if(nf >= 2 && strcmp(f[1], "action") == 0 && app != nil){
				qlock(&uxlk);
				i = ux.barid != nil && strcmp(app, ux.barid) == 0;
				qunlock(&uxlk);
				if(i){
					free(line);
					sendp(ux.menuc, estrdup(p));
					p = e+1;
					continue;
				}
			}
			free(line);
			p = e+1;
		}
		off = n - (p-buf);
		memmove(buf, p, off);
	}
	close(fd);
}

/* /dev/ux/events: close boxes in rio's window chrome (devux 1.2), and focus */
static void
uxevreader(void*)
{
	char buf[8192], *p, *e;
	int fd, n, off;

	threadsetname("uxdevuxevents");
	fd = open("/dev/ux/events", OREAD);
	if(fd < 0)
		return;
	off = 0;
	while((n = read(fd, buf+off, sizeof buf-1-off)) > 0){
		n += off;
		buf[n] = 0;
		p = buf;
		while((e = strchr(p, '\n')) != nil){
			*e = 0;
			if(strstr(p, " close win=") != nil || strstr(p, " focus win=") != nil)
				sendp(ux.menuc, estrdup(p));
			p = e+1;
		}
		off = n - (p-buf);
		memmove(buf, p, off);
	}
	close(fd);
}

/*
 * ask rio for classic chrome on a window, unless it's a dialog or an
 * alert (they draw their own frame); and follow rio's events. uxlk held.
 */
void
_chrome(Uxwin *w)
{
	int fd;

	fd = open("/dev/ux/ctl", OWRITE);
	if(fd < 0)
		return;
	if(strcmp(w->kind, "dialog") != 0 && strcmp(w->kind, "alert") != 0)
		fprint(fd, "chrome %d on\n", w->winid);	/* fails harmlessly before devux 1.2 */
	close(fd);
	if(!ux.evreader){
		ux.evreader = 1;
		proccreate(uxevreader, nil, 16*1024);
	}
}

/* (re)register everything with the bar */
void
_menuregister(void)
{
	char *t;
	int i;

	if(ux.menufd >= 0){
		close(ux.menufd);
		ux.menufd = -1;
	}
	free(ux.barid);
	ux.barid = nil;
	ux.menufd = open("/dev/ux/menubar/ctl", ORDWR);
	if(ux.menufd < 0)
		return;
	ux.barid = ux.id ? estrdup(ux.id) : smprint("%s-%d", ux.name, getpid());
	t = _attr("title", ux.title);
	bar("app %s %s\n", ux.barid, t);
	free(t);
	bar("begin\n");
	for(i = 0; i < ux.nwin; i++)
		bar("window %d\n", ux.win[i]->winid);
	for(i = 0; i < ux.nmenu; i++)
		barmenu(ux.menu[i]);
	bar("commit\n");
	if(!ux.barreader){
		ux.barreader = 1;
		proccreate(barreader, nil, 16*1024);
	}
}

void
_menuwin(Uxwin *w, int add)
{
	if(add)
		bar("window %d\n", w->winid);
	else
		bar("unwindow %d\n", w->winid);
}

/* an action chosen: from the bar, a shortcut, or ctl invoke */
void
_menuaction(char *action, int win, char *via)
{
	Uxmenu *m;
	Action *a;
	Uxev e;
	char wbuf[16];
	int i, j, v;

	m = nil;
	for(i = 0; i < ux.nmenu && m == nil; i++)
		for(j = 0; j < ux.menu[i]->nitem; j++)
			if(!ux.menu[i]->item[j].sep && strcmp(ux.menu[i]->item[j].action, action) == 0){
				m = ux.menu[i];
				break;
			}
	snprint(wbuf, sizeof wbuf, "%d", win);
	if(m != nil)
		_event(m, "action", "id", action, "win", wbuf, "via", via, nil);
	else
		_event(nil, "action", "id", action, "win", wbuf, "via", via, nil);
	if(strcmp(via, "key") == 0)
		v = UXkey;
	else if(strcmp(via, "ctl") == 0)
		v = UXctl;
	else
		v = UXmenubar;
	memset(&e, 0, sizeof e);
	e.type = UXaction;
	e.via = v;
	e.action = action;
	e.win = _findwin(win);
	for(a = ux.actions; a != nil; a = a->next)
		if(strcmp(a->id, action) == 0){
			_notify(a->fn, a->arg, &e);
			return;
		}
	_notify(ux.fn, ux.arg, &e);
}

/* a line from menubar/events: seq action app=... win=... id=... via=... */
void
_barevent(char *line)
{
	char *f[16], *id, *via;
	int nf, i, win;
	Uxwin *w;

	nf = tokenize(line, f, nelem(f));
	if(nf >= 3 && strcmp(f[1], "focus") == 0){
		for(i = 2; i < nf; i++)
			if(strncmp(f[i], "win=", 4) == 0)
				_focusto(atoi(f[i]+4));
		return;
	}
	if(nf >= 3 && strcmp(f[1], "close") == 0){
		for(i = 2; i < nf; i++)
			if(strncmp(f[i], "win=", 4) == 0 && (w = _findwin(atoi(f[i]+4))) != nil)
				_winclose(w, UXui);
		return;
	}
	id = nil;
	via = "menu";
	win = 0;
	for(i = 2; i < nf; i++){
		if(strncmp(f[i], "id=", 3) == 0)
			id = f[i]+3;
		else if(strncmp(f[i], "win=", 4) == 0)
			win = atoi(f[i]+4);
		else if(strncmp(f[i], "via=", 4) == 0){
			if(strcmp(f[i]+4, "key") == 0)
				via = "key";
			else if(strcmp(f[i]+4, "ctl") == 0)
				via = "ctl";
		}
	}
	if(id != nil)
		_menuaction(id, win, via);
}

/* public */

Uxmenu*
uxmenu(char *id, char *title)
{
	Uxmenu *m;

	qlock(&uxlk);
	if(!_validid(id))
		sysfatal("libux: bad menu id %q", id);
	m = _findmenu(id);
	if(m == nil){
		m = emalloc(sizeof *m);
		m->id = estrdup(id);
		_addobj(m, Omenu);
		ux.menu = erealloc(ux.menu, (ux.nmenu+1)*sizeof(Uxmenu*));
		ux.menu[ux.nmenu++] = m;
		_event(nil, "new", "menu", id, nil);
	}
	free(m->title);
	m->title = estrdup(title);
	barmenu(m);
	qunlock(&uxlk);
	return m;
}

static void
additem(Uxmenu *m, Item *it)
{
	char *a;

	m->item = erealloc(m->item, (m->nitem+1)*sizeof(Item));
	m->item[m->nitem++] = *it;
	a = itemattrs(it);
	bar("item %s %s%s\n", m->id, it->action, a);
	free(a);
	_event(m, "items", nil);
}

void
uxitem(Uxmenu *m, char *action, char *label, char *key)
{
	Item it;

	qlock(&uxlk);
	if(!_validid(action))
		sysfatal("libux: bad action id %q", action);
	memset(&it, 0, sizeof it);
	it.action = estrdup(action);
	it.label = estrdup(label);
	it.key = key ? estrdup(key) : nil;
	it.enabled = 1;
	additem(m, &it);
	qunlock(&uxlk);
}

void
uxsep(Uxmenu *m, char *id)
{
	Item it;

	qlock(&uxlk);
	memset(&it, 0, sizeof it);
	it.action = estrdup(id);
	it.label = estrdup("");
	it.sep = 1;
	it.enabled = 1;
	additem(m, &it);
	qunlock(&uxlk);
}

static void
setitems(char *action, int what, int v)
{
	int i, j, ch;
	Item *it;

	qlock(&uxlk);
	for(i = 0; i < ux.nmenu; i++){
		ch = 0;
		for(j = 0; j < ux.menu[i]->nitem; j++){
			it = &ux.menu[i]->item[j];
			if(it->sep || strcmp(it->action, action) != 0)
				continue;
			if(what == 0)
				it->enabled = v;
			else
				it->check = v;
			ch = 1;
		}
		if(ch)
			_event(ux.menu[i], "items", nil);
	}
	if(what == 0)
		bar("set %s enabled=%s\n", action, v ? "on" : "off");
	else
		bar("set %s check=%s\n", action, v == 2 ? "mixed" : v ? "on" : "off");
	qunlock(&uxlk);
}

void
uxitemenable(char *action, int on)
{
	setitems(action, 0, on != 0);
}

void
uxitemcheck(char *action, int on)
{
	setitems(action, 1, on);
}

void
uxaction(char *action, Uxfn *fn, void *arg)
{
	Action *a;

	qlock(&uxlk);
	a = emalloc(sizeof *a);
	a->id = estrdup(action);
	a->fn = fn;
	a->arg = arg;
	a->next = ux.actions;
	ux.actions = a;
	qunlock(&uxlk);
}
