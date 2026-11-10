/*
 * fakedevux: a fake of rio's /dev/ux (docs/protocols/devux.md) for
 * testing clients without the rio fork.
 *
 * It serves a tree with ux/ (the protocol) and fake/ (test controls):
 *
 *	fake/ctl	write commands that play the part of rio and the
 *			person at the screen: windows, focus, clicks, keys
 *	fake/log	every ctl line received, and what keys did
 *
 * Usage: fakedevux [-D] [-q qmax] [-s srvname] [-m mtpt]
 * Default: post /srv/fakedevux and mount nothing. Then
 * `mount -a /srv/fakedevux /dev` gives /dev/ux and /dev/fake. The
 * attach name is the fid's own window, as with rio:
 * `mount -a /srv/fakedevux /dev 7`.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <draw.h>
#include "fake.h"

typedef struct Win Win;
typedef struct Sess Sess;
typedef struct Menu Menu;
typedef struct Item Item;

enum {
	Qversion = 1,
	Qctl,
	Qevents,
	Qwindows,
	Qdeskctl,
	Qdeskstate,
	Qdeskevents,
	Qbarctl,
	Qbarmenus,
	Qbarevents,
	Qfakectl,
	Qfakelog,

	Barheight = 20,
};

struct Win {
	int	id;
	Rectangle	r;
	char	*label;
	int	hidden;
	Sess	*app;
};

struct Item {
	char	*id;
	char	*label;
	char	*key;
	int	enabled;
	char	*check;		/* on, off, mixed */
	char	*sub;
	int	sep;
	Item	*next;
};

struct Menu {
	char	*id;
	char	*title;
	int	win;		/* window override, or 0 */
	int	top;
	Item	*items;
	Menu	*next;
};

struct Sess {
	char	*app;		/* nil until `app` */
	char	*title;
	Menu	*menus;
	int	inbatch;
	char	**batch;
	int	nbatch;
	int	ownwin;
	Sess	*next;
};

/* per-open state, in Fid.aux */
typedef struct Open Open;
struct Open {
	Sub	*sub;
	Sess	*sess;
};

static Win	**wins;		/* stacking order, bottom first */
static int	nwins;
static int	cur;		/* focused window, 0 for none */
static int	modeon = 1;
static char	*cmdkey = "mod4";
static Rectangle	screenr = {0, 0, 1024, 768};
static int	deskwin;
static Fid	*deskfid;
static Sess	*sessions;
static char	*logtext;
static long	nlog;
static char	*lastfocusapp;	/* foreground app as last reported */

static void
logf(char *fmt, ...)
{
	va_list arg;
	char *s;
	long n;

	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	n = strlen(s);
	logtext = erealloc9p(logtext, nlog+n+2);
	memmove(logtext+nlog, s, n);
	nlog += n;
	logtext[nlog++] = '\n';
	logtext[nlog] = 0;
	free(s);
}

static Win*
findwin(int id)
{
	int i;

	for(i = 0; i < nwins; i++)
		if(wins[i]->id == id)
			return wins[i];
	return nil;
}

static int
winpos(Win *w)
{
	int i;

	for(i = 0; i < nwins; i++)
		if(wins[i] == w)
			return i;
	return -1;
}

static Rectangle
deskrect(void)
{
	Rectangle r;

	r = screenr;
	if(modeon)
		r.min.y += Barheight;
	return r;
}

static void
ev(char *topic, char *msg)
{
	emit1(topic, msg);
}

static Sess*
foreground(void)
{
	Win *w;

	w = findwin(cur);
	if(w == nil)
		return nil;
	return w->app;
}

/* tell menubar/events when the foreground app changes */
static void
checkbarfocus(void)
{
	Sess *s;
	char *app;
	Msg m;

	s = foreground();
	app = s != nil ? s->app : nil;
	if(app == nil && lastfocusapp == nil)
		return;
	if(app != nil && lastfocusapp != nil && strcmp(app, lastfocusapp) == 0)
		return;
	free(lastfocusapp);
	lastfocusapp = app != nil ? estrdup9p(app) : nil;
	mbegin(&m, "focus");
	if(app != nil)
		mattr(&m, "app", "%s", app);
	mattr(&m, "win", "%d", cur);
	ev("menubar", mend(&m));
}

static void
setfocus(int id)
{
	Win *w;
	Msg m;

	w = findwin(id);
	cur = id;
	mbegin(&m, "focus");
	mattr(&m, "win", "%d", id);
	if(w != nil && w->app != nil)
		mattr(&m, "app", "%s", w->app->app);
	ev("events", mend(&m));
	checkbarfocus();
}

static void
appevent(Win *w)
{
	Msg m;

	mbegin(&m, "app");
	mattr(&m, "win", "%d", w->id);
	if(w->app != nil)
		mattr(&m, "app", "%s", w->app->app);
	ev("events", mend(&m));
	if(w->id == cur)
		checkbarfocus();
}

/*
 * Menus
 */
static void
freeitems(Item *it)
{
	Item *n;

	for(; it != nil; it = n){
		n = it->next;
		free(it->id);
		free(it->label);
		free(it->key);
		free(it->check);
		free(it->sub);
		free(it);
	}
}

static void
freemenus(Menu *m)
{
	Menu *n;

	for(; m != nil; m = n){
		n = m->next;
		freeitems(m->items);
		free(m->id);
		free(m->title);
		free(m);
	}
}

static Menu*
findmenu(Sess *s, char *id, int win)
{
	Menu *m;

	for(m = s->menus; m != nil; m = m->next)
		if(strcmp(m->id, id) == 0 && m->win == win)
			return m;
	return nil;
}

static Menu*
findanymenu(Sess *s, char *id)
{
	Menu *m;

	for(m = s->menus; m != nil; m = m->next)
		if(strcmp(m->id, id) == 0)
			return m;
	return nil;
}

static char*
setkey(Item *it, char *v)
{
	char *p;

	if(v == nil)
		return nil;
	/* modifiers then one key; just check the modifiers */
	p = v;
	for(;;){
		if(strncmp(p, "shift+", 6) == 0)
			p += 6;
		else if(strncmp(p, "alt+", 4) == 0)
			p += 4;
		else if(strncmp(p, "ctl+", 4) == 0)
			p += 4;
		else
			break;
	}
	if(*p == 0)
		return "bad value: key";
	free(it->key);
	it->key = estrdup9p(v);
	return nil;
}

static char*
setitemattrs(Item *it, Cmd *c)
{
	char *v, *e;

	if((v = cmdattr(c, "label")) != nil){
		free(it->label);
		it->label = estrdup9p(v);
	}
	if((e = setkey(it, cmdattr(c, "key"))) != nil)
		return e;
	if((v = cmdattr(c, "enabled")) != nil){
		if(!isbool(v))
			return "bad value: enabled";
		it->enabled = strcmp(v, "on") == 0;
	}
	if((v = cmdattr(c, "check")) != nil){
		if(!isbool(v) && strcmp(v, "mixed") != 0)
			return "bad value: check";
		free(it->check);
		it->check = estrdup9p(v);
	}
	return nil;
}

static char *menuattrs[] = {"title", "win", "after", "top", nil};
static char *itemattrs[] = {"label", "key", "enabled", "check", "sub", "sep", "after", "win", nil};
static char *winattrs[] = {"win", nil};
static char *titleattrs[] = {"title", nil};
static char *setattrs[] = {"label", "key", "enabled", "check", nil};

static char*
barcmd(Sess *s, Cmd *c)
{
	char *verb, *v, *e;
	Menu *m, **l, *after;
	Item *it, **il;
	Win *w;
	int win, id, n;
	Msg msg;

	verb = c->tok[0];
	n = c->npos;
	if(strcmp(verb, "invoke") == 0){
		Sess *t;
		Item *found;
		int any;

		if(n != 3 || !cmdattrok(c, winattrs))
			return "bad syntax";
		for(t = sessions; t != nil; t = t->next)
			if(t->app != nil && strcmp(t->app, c->tok[1]) == 0)
				break;
		if(t == nil)
			return "not found: app";
		found = nil;
		any = 0;
		for(m = t->menus; m != nil; m = m->next)
			for(it = m->items; it != nil; it = it->next)
				if(!it->sep && strcmp(it->id, c->tok[2]) == 0){
					any = 1;
					if(it->enabled)
						found = it;
				}
		if(!any)
			return "not found: action";
		if(found == nil)
			return "disabled";
		win = cur;
		if((v = cmdattr(c, "win")) != nil)
			win = atoi(v);
		mbegin(&msg, "action");
		mattr(&msg, "app", "%s", t->app);
		mattr(&msg, "win", "%d", win);
		mattr(&msg, "id", "%s", c->tok[2]);
		mattr(&msg, "via", "ctl");
		ev("menubar", mend(&msg));
		return nil;
	}
	if(strcmp(verb, "app") == 0){
		Sess *t;

		if(n != 2 || !cmdattrok(c, titleattrs))
			return "bad syntax";
		if(s->app != nil)
			return "bad syntax: app already declared";
		for(t = sessions; t != nil; t = t->next)
			if(t->app != nil && strcmp(t->app, c->tok[1]) == 0)
				return "in use";
		s->app = estrdup9p(c->tok[1]);
		if((v = cmdattr(c, "title")) != nil)
			s->title = estrdup9p(v);
		return nil;
	}
	if(s->app == nil)
		return "bad syntax: no app";
	if(strcmp(verb, "begin") == 0){
		if(n != 1)
			return "bad syntax";
		s->inbatch = 1;
		return nil;
	}
	if(strcmp(verb, "abort") == 0){
		int i;

		for(i = 0; i < s->nbatch; i++)
			free(s->batch[i]);
		s->nbatch = 0;
		s->inbatch = 0;
		return nil;
	}
	if(strcmp(verb, "commit") == 0){
		int i, nb;
		char **b;
		Cmd bc;

		if(!s->inbatch)
			return "bad syntax: no batch";
		b = s->batch;
		nb = s->nbatch;
		s->batch = nil;
		s->nbatch = 0;
		s->inbatch = 0;
		e = nil;
		for(i = 0; i < nb; i++){
			if(e == nil){
				cmdparse(&bc, b[i]);
				e = barcmd(s, &bc);
				cmdfree(&bc);
			}
			free(b[i]);
		}
		free(b);
		return e;
	}
	if(s->inbatch && strcmp(verb, "window") != 0 && strcmp(verb, "unwindow") != 0
	&& strcmp(verb, "title") != 0){
		/* defer; syntax is checked when it runs at commit */
		char *line;
		Fmt f;
		int i;

		fmtstrinit(&f);
		for(i = 0; i < c->ntok; i++)
			fmtprint(&f, "%s%s", i ? " " : "", tokq(c->tok[i]));
		line = fmtstrflush(&f);
		s->batch = erealloc9p(s->batch, (s->nbatch+1)*sizeof s->batch[0]);
		s->batch[s->nbatch++] = line;
		return nil;
	}
	if(strcmp(verb, "title") == 0){
		if(n != 2)
			return "bad syntax";
		free(s->title);
		s->title = estrdup9p(c->tok[1]);
		return nil;
	}
	if(strcmp(verb, "window") == 0 || strcmp(verb, "unwindow") == 0){
		if(n > 2)
			return "bad syntax";
		if(n == 2)
			id = atoi(c->tok[1]);
		else if(verb[0] == 'w' && s->ownwin != 0)
			id = s->ownwin;
		else
			return "bad syntax: no window";
		if((w = findwin(id)) == nil)
			return "not found: window";
		if(verb[0] == 'w'){
			if(w->app != nil && w->app != s)
				return "in use";
			if(w->app == s)
				return nil;
			w->app = s;
		}else{
			if(w->app != s)
				return "not found: window not in this app";
			w->app = nil;
		}
		appevent(w);
		return nil;
	}
	if(strcmp(verb, "menu") == 0){
		if(n != 2 || !isid(c->tok[1]) || !cmdattrok(c, menuattrs))
			return "bad syntax";
		if((v = cmdattr(c, "title")) == nil)
			return "bad syntax: menu needs title";
		win = 0;
		if(cmdattr(c, "win") != nil)
			win = atoi(cmdattr(c, "win"));
		if(cmdattr(c, "top") != nil && !isbool(cmdattr(c, "top")))
			return "bad value: top";
		m = findmenu(s, c->tok[1], win);
		if(m != nil){	/* replace: drop it, re-add in place below */
			for(l = &s->menus; *l != m; l = &(*l)->next)
				;
			*l = m->next;
			freeitems(m->items);
			m->items = nil;
		}else{
			m = emalloc9p(sizeof *m);
			memset(m, 0, sizeof *m);
			m->id = estrdup9p(c->tok[1]);
			m->win = win;
		}
		free(m->title);
		m->title = estrdup9p(v);
		m->top = cmdattr(c, "top") == nil || strcmp(cmdattr(c, "top"), "on") == 0;
		after = nil;
		if((v = cmdattr(c, "after")) != nil && (after = findanymenu(s, v)) == nil){
			freemenus(m);
			return "not found: after";
		}
		if(after != nil){
			m->next = after->next;
			after->next = m;
		}else{
			for(l = &s->menus; *l != nil; l = &(*l)->next)
				;
			m->next = nil;
			*l = m;
		}
		return nil;
	}
	if(strcmp(verb, "item") == 0){
		if(n != 3 || !isid(c->tok[1]) || !isid(c->tok[2]) || !cmdattrok(c, itemattrs))
			return "bad syntax";
		win = 0;
		if(cmdattr(c, "win") != nil)
			win = atoi(cmdattr(c, "win"));
		if((m = findmenu(s, c->tok[1], win)) == nil)
			return "not found: menu";
		if((v = cmdattr(c, "sub")) != nil && findanymenu(s, v) == nil)
			return "not found: sub";
		if(cmdattr(c, "sep") != nil && !isbool(cmdattr(c, "sep")))
			return "bad value: sep";
		for(il = &m->items; *il != nil; il = &(*il)->next)
			if(strcmp((*il)->id, c->tok[2]) == 0){
				it = *il;
				*il = it->next;
				it->next = nil;
				freeitems(it);
				break;
			}
		it = emalloc9p(sizeof *it);
		memset(it, 0, sizeof *it);
		it->id = estrdup9p(c->tok[2]);
		it->enabled = 1;
		it->check = estrdup9p("off");
		it->sep = cmdattr(c, "sep") != nil && strcmp(cmdattr(c, "sep"), "on") == 0;
		if(v != nil)
			it->sub = estrdup9p(v);
		if((e = setitemattrs(it, c)) != nil){
			freeitems(it);
			return e;
		}
		il = &m->items;
		if((v = cmdattr(c, "after")) != nil){
			for(; *il != nil; il = &(*il)->next)
				if(strcmp((*il)->id, v) == 0)
					break;
			if(*il == nil){
				freeitems(it);
				return "not found: after";
			}
			il = &(*il)->next;
		}else
			for(; *il != nil; il = &(*il)->next)
				;
		it->next = *il;
		*il = it;
		return nil;
	}
	if(strcmp(verb, "set") == 0){
		int any;

		if(n != 2 || !cmdattrok(c, setattrs))
			return "bad syntax";
		any = 0;
		for(m = s->menus; m != nil; m = m->next)
			for(it = m->items; it != nil; it = it->next)
				if(strcmp(it->id, c->tok[1]) == 0){
					any = 1;
					if((e = setitemattrs(it, c)) != nil)
						return e;
				}
		return any ? nil : "not found: action";
	}
	if(strcmp(verb, "del") == 0){
		if(n != 2 && n != 3)
			return "bad syntax";
		if((m = findanymenu(s, c->tok[1])) == nil)
			return "not found: menu";
		if(n == 2){
			for(l = &s->menus; *l != nil;){
				if(strcmp((*l)->id, c->tok[1]) == 0){
					m = *l;
					*l = m->next;
					m->next = nil;
					freemenus(m);
				}else
					l = &(*l)->next;
			}
			return nil;
		}
		for(il = &m->items; *il != nil; il = &(*il)->next)
			if(strcmp((*il)->id, c->tok[2]) == 0){
				it = *il;
				*il = it->next;
				it->next = nil;
				freeitems(it);
				return nil;
			}
		return "not found: item";
	}
	if(strcmp(verb, "clear") == 0){
		freemenus(s->menus);
		s->menus = nil;
		return nil;
	}
	return "unknown command";
}

static void
endsess(Sess *s)
{
	Sess **l;
	int i;

	for(i = 0; i < nwins; i++)
		if(wins[i]->app == s){
			wins[i]->app = nil;
			appevent(wins[i]);
		}
	for(l = &sessions; *l != nil; l = &(*l)->next)
		if(*l == s){
			*l = s->next;
			break;
		}
	freemenus(s->menus);
	for(i = 0; i < s->nbatch; i++)
		free(s->batch[i]);
	free(s->batch);
	free(s->app);
	free(s->title);
	free(s);
	checkbarfocus();
}

static void
fmtitem(Fmt *f, Menu *m, Item *it)
{
	fmtprint(f, "item %s %s", m->id, it->id);
	if(it->sep)
		fmtprint(f, " sep=on");
	else{
		if(it->label != nil)
			fmtprint(f, " %s", attrq("label", it->label));
		if(it->key != nil)
			fmtprint(f, " %s", attrq("key", it->key));
		if(!it->enabled)
			fmtprint(f, " enabled=off");
		if(strcmp(it->check, "off") != 0)
			fmtprint(f, " check=%s", it->check);
		if(it->sub != nil)
			fmtprint(f, " sub=%s", it->sub);
	}
	if(m->win)
		fmtprint(f, " win=%d", m->win);
	fmtprint(f, "\n");
}

static char*
menusnapshot(void)
{
	Fmt f;
	Sess *s, *fg;
	Menu *m;
	Item *it;
	int i;

	fmtstrinit(&f);
	fg = foreground();
	fmtprint(&f, "focus");
	if(fg != nil)
		fmtprint(&f, " app=%s", fg->app);
	fmtprint(&f, " win=%d\n", cur);
	for(s = sessions; s != nil; s = s->next){
		if(s->app == nil)
			continue;
		fmtprint(&f, "app %s", s->app);
		if(s->title != nil)
			fmtprint(&f, " %s", attrq("title", s->title));
		fmtprint(&f, "\n");
		for(i = 0; i < nwins; i++)
			if(wins[i]->app == s)
				fmtprint(&f, "window %d\n", wins[i]->id);
		for(m = s->menus; m != nil; m = m->next){
			fmtprint(&f, "menu %s %s", m->id, attrq("title", m->title));
			if(m->win)
				fmtprint(&f, " win=%d", m->win);
			if(!m->top)
				fmtprint(&f, " top=off");
			fmtprint(&f, "\n");
			for(it = m->items; it != nil; it = it->next)
				fmtitem(&f, m, it);
		}
	}
	return fmtstrflush(&f);
}

/*
 * Keyboard shortcut lookup for fake `key`: window overrides first,
 * then app-wide top-level menus, then submenus.
 */
static Item*
findkey(Sess *s, int win, char *key, int *disabled)
{
	Menu *m;
	Item *it, *dis;
	int pass;

	dis = nil;
	for(pass = 0; pass < 3; pass++)
		for(m = s->menus; m != nil; m = m->next){
			if(pass == 0 && m->win != win)
				continue;
			if(pass == 1 && (m->win != 0 || !m->top))
				continue;
			if(pass == 2 && (m->win != 0 || m->top))
				continue;
			if(pass > 0 && findmenu(s, m->id, win) != nil && win != 0)
				continue;	/* overridden by the window */
			for(it = m->items; it != nil; it = it->next)
				if(!it->sep && it->key != nil && cistrcmp(it->key, key) == 0){
					if(it->enabled)
						return it;
					if(dis == nil)
						dis = it;
				}
		}
	*disabled = dis != nil;
	return nil;
}

/*
 * fake/ctl: play rio and the person
 */
static char*
fakecmd(char *line, void*)
{
	Cmd c;
	char *verb, *e, *v;
	Win *w;
	Msg m;
	int id, i;
	Rectangle r;

	if(cmdparse(&c, line) == 0){
		cmdfree(&c);
		return nil;
	}
	logf("fake/ctl %s", line);
	verb = c.tok[0];
	e = nil;
	id = c.npos > 1 ? atoi(c.tok[1]) : 0;
	w = findwin(id);
	if(strcmp(verb, "new") == 0){
		if(c.npos != 2 || id <= 0)
			e = "bad syntax";
		else if(w != nil)
			e = "in use";
		else{
			w = emalloc9p(sizeof *w);
			memset(w, 0, sizeof *w);
			w->id = id;
			w->r = Rect(100, 100, 500, 400);
			if(cmdattr(&c, "rect") != nil && parserect(cmdattr(&c, "rect"), &w->r) < 0){
				free(w);
				e = "bad value: rect";
				goto Out;
			}
			v = cmdattr(&c, "label");
			w->label = estrdup9p(v != nil ? v : "");
			wins = erealloc9p(wins, (nwins+1)*sizeof wins[0]);
			wins[nwins++] = w;
			mbegin(&m, "new");
			mattr(&m, "win", "%d", id);
			mattr(&m, "rect", "%s", fmtrect(w->r));
			mattr(&m, "label", "%s", w->label);
			ev("events", mend(&m));
		}
	}else if(strcmp(verb, "focus") == 0){
		if(c.npos != 2 || (id != 0 && w == nil))
			e = "not found";
		else
			setfocus(id);
	}else if(strcmp(verb, "screen") == 0){
		if(parserect(cmdattr(&c, "rect"), &r) < 0)
			e = "bad value: rect";
		else{
			screenr = r;
			mbegin(&m, "screen");
			mattr(&m, "rect", "%s", fmtrect(r));
			ev("events", mend(&m));
			mbegin(&m, "resize");
			mattr(&m, "rect", "%s", fmtrect(deskrect()));
			ev("desktop", mend(&m));
		}
	}else if(strcmp(verb, "click") == 0 || strcmp(verb, "key") == 0){
		Sess *s;
		Item *it;
		int dis;

		s = foreground();
		if(c.npos != 2)
			e = "bad syntax";
		else if(s == nil){
			logf("key %s passed", c.tok[1]);
			if(verb[0] == 'c')
				e = "not found: no foreground app";
		}else if(verb[0] == 'k'){
			if(!modeon || strcmp(cmdkey, "none") == 0){
				logf("key %s passed", c.tok[1]);
				goto Out;
			}
			it = findkey(s, cur, c.tok[1], &dis);
			if(it == nil){
				logf("key %s %s", c.tok[1], dis ? "swallowed" : "passed");
				goto Out;
			}
			logf("key %s action %s", c.tok[1], it->id);
			mbegin(&m, "action");
			mattr(&m, "app", "%s", s->app);
			mattr(&m, "win", "%d", cur);
			mattr(&m, "id", "%s", it->id);
			mattr(&m, "via", "key");
			ev("menubar", mend(&m));
		}else{
			Menu *mn;

			it = nil;
			for(mn = s->menus; mn != nil && it == nil; mn = mn->next){
				if(mn->win != 0 && mn->win != cur)
					continue;
				for(it = mn->items; it != nil; it = it->next)
					if(!it->sep && strcmp(it->id, c.tok[1]) == 0)
						break;
			}
			if(it == nil)
				e = "not found: action";
			else if(!it->enabled)
				e = "disabled";
			else{
				mbegin(&m, "action");
				mattr(&m, "app", "%s", s->app);
				mattr(&m, "win", "%d", cur);
				mattr(&m, "id", "%s", it->id);
				mattr(&m, "via", "click");
				ev("menubar", mend(&m));
			}
		}
	}else if(w == nil){
		e = c.npos < 2 ? "bad syntax" : (strcmp(verb, "delete") == 0 || strcmp(verb, "rect") == 0
			|| strcmp(verb, "hide") == 0 || strcmp(verb, "unhide") == 0
			|| strcmp(verb, "top") == 0 || strcmp(verb, "label") == 0) ? "not found" : "unknown command";
	}else if(strcmp(verb, "delete") == 0){
		i = winpos(w);
		nwins--;
		memmove(wins+i, wins+i+1, (nwins-i)*sizeof wins[0]);
		if(deskwin == id){
			deskwin = 0;
			deskfid = nil;
			mbegin(&m, "release");
			mattr(&m, "win", "%d", id);
			ev("desktop", mend(&m));
			mbegin(&m, "desktop");
			mattr(&m, "win", "0");
			ev("events", mend(&m));
		}
		mbegin(&m, "delete");
		mattr(&m, "win", "%d", id);
		ev("events", mend(&m));
		if(cur == id)
			setfocus(0);
		free(w->label);
		free(w);
	}else if(strcmp(verb, "rect") == 0){
		if(parserect(cmdattr(&c, "rect"), &r) < 0)
			e = "bad value: rect";
		else{
			w->r = r;
			mbegin(&m, "rect");
			mattr(&m, "win", "%d", id);
			mattr(&m, "rect", "%s", fmtrect(r));
			ev("events", mend(&m));
		}
	}else if(strcmp(verb, "hide") == 0 || strcmp(verb, "unhide") == 0){
		w->hidden = verb[0] == 'h';
		mbegin(&m, verb);
		mattr(&m, "win", "%d", id);
		ev("events", mend(&m));
	}else if(strcmp(verb, "top") == 0){
		if(id != deskwin){
			i = winpos(w);
			memmove(wins+i, wins+i+1, (nwins-i-1)*sizeof wins[0]);
			wins[nwins-1] = w;
			mbegin(&m, "top");
			mattr(&m, "win", "%d", id);
			ev("events", mend(&m));
		}
	}else if(strcmp(verb, "label") == 0){
		if(c.npos != 3)
			e = "bad syntax";
		else{
			free(w->label);
			w->label = estrdup9p(c.tok[2]);
			mbegin(&m, "label");
			mattr(&m, "win", "%d", id);
			mattr(&m, "label", "%s", w->label);
			ev("events", mend(&m));
		}
	}else
		e = "unknown command";
Out:
	cmdfree(&c);
	return e;
}

static char*
uxctlcmd(char *line, void*)
{
	Cmd c;
	char *e;
	Msg m;
	int on;

	if(cmdparse(&c, line) == 0){
		cmdfree(&c);
		return nil;
	}
	logf("ctl %s", line);
	e = nil;
	if(strcmp(c.tok[0], "mode") == 0){
		if(c.ntok != 2 || !isbool(c.tok[1]))
			e = "bad syntax";
		else{
			on = strcmp(c.tok[1], "on") == 0;
			if(on != modeon){
				modeon = on;
				mbegin(&m, "mode");
				mattr(&m, "ux", "%s", c.tok[1]);
				ev("events", mend(&m));
				mbegin(&m, "resize");
				mattr(&m, "rect", "%s", fmtrect(deskrect()));
				ev("desktop", mend(&m));
			}
		}
	}else if(strcmp(c.tok[0], "cmdkey") == 0){
		if(c.ntok != 2)
			e = "bad syntax";
		else if(strcmp(c.tok[1], "mod4") == 0 || strcmp(c.tok[1], "alt") == 0
		|| strcmp(c.tok[1], "ctl") == 0 || strcmp(c.tok[1], "none") == 0)
			cmdkey = estrdup9p(c.tok[1]);
		else
			e = "bad value: cmdkey";
	}else
		e = "unknown command";
	cmdfree(&c);
	return e;
}

typedef struct Deskarg Deskarg;
struct Deskarg {
	Fid	*fid;
	int	ownwin;
};

static char*
deskcmd(char *line, void *v)
{
	Deskarg *a;
	Cmd c;
	char *e;
	Msg m;
	int id;

	a = v;
	if(cmdparse(&c, line) == 0){
		cmdfree(&c);
		return nil;
	}
	logf("desktop/ctl %s", line);
	e = nil;
	if(strcmp(c.tok[0], "claim") == 0){
		id = c.ntok > 1 ? atoi(c.tok[1]) : a->ownwin;
		if(c.ntok > 2)
			e = "bad syntax";
		else if(id == 0)
			e = "bad syntax: no window";
		else if(findwin(id) == nil)
			e = "not found";
		else if(deskfid != nil && deskfid != a->fid)
			e = "in use";
		else{
			Win *w;
			int i;

			deskfid = a->fid;
			deskwin = id;
			w = findwin(id);	/* to the bottom */
			i = winpos(w);
			memmove(wins+1, wins, i*sizeof wins[0]);
			wins[0] = w;
			w->r = insetrect(deskrect(), -Borderwidth);
			mbegin(&m, "claim");
			mattr(&m, "win", "%d", id);
			ev("desktop", mend(&m));
			mbegin(&m, "desktop");
			mattr(&m, "win", "%d", id);
			ev("events", mend(&m));
		}
	}else if(strcmp(c.tok[0], "release") == 0){
		if(deskfid != a->fid)
			e = "not found: no claim";
		else{
			mbegin(&m, "release");
			mattr(&m, "win", "%d", deskwin);
			ev("desktop", mend(&m));
			mbegin(&m, "desktop");
			mattr(&m, "win", "0");
			ev("events", mend(&m));
			deskfid = nil;
			deskwin = 0;
		}
	}else
		e = "unknown command";
	cmdfree(&c);
	return e;
}

static char*
barline(char *line, void *v)
{
	Cmd c;
	char *e;

	if(cmdparse(&c, line) == 0){
		cmdfree(&c);
		return nil;
	}
	logf("menubar/ctl %s", line);
	e = barcmd(v, &c);
	cmdfree(&c);
	return e;
}

/*
 * 9P
 */
static int
ownwin(Fid *f)
{
	char *p;

	/* fsattach stored "uname aname" in uid */
	p = strchr(f->uid, ' ');
	return p != nil ? atoi(p+1) : 0;
}

static void
fsattach(Req *r)
{
	char *u;

	u = smprint("%s %s", r->ifcall.uname, r->ifcall.aname);
	free(r->fid->uid);
	r->fid->uid = u;
	respond(r, nil);
}

static long
qid(Fid *f)
{
	return (long)(uintptr)f->file->aux;
}

static void
fsopen(Req *r)
{
	Open *o;
	Sess *s;

	o = emalloc9p(sizeof *o);
	memset(o, 0, sizeof *o);
	r->fid->aux = o;
	switch(qid(r->fid)){
	case Qevents:
		o->sub = subopen("events");
		break;
	case Qdeskevents:
		o->sub = subopen("desktop");
		break;
	case Qbarevents:
		o->sub = subopen("menubar");
		break;
	case Qbarctl:
		if((r->ifcall.mode&OMASK) != OREAD){
			s = emalloc9p(sizeof *s);
			memset(s, 0, sizeof *s);
			s->ownwin = ownwin(r->fid);
			s->next = sessions;
			sessions = s;
			o->sess = s;
		}
		break;
	}
	respond(r, nil);
}

static char*
winsnapshot(void)
{
	Fmt f;
	Win *w;
	int i;

	fmtstrinit(&f);
	for(i = 0; i < nwins; i++){
		w = wins[i];
		fmtprint(&f, "window win=%d rect=%s %s", w->id, fmtrect(w->r),
			attrq("label", w->label));
		if(w->app != nil)
			fmtprint(&f, " app=%s", w->app->app);
		fmtprint(&f, " state=");
		if(w->id == cur)
			fmtprint(&f, "current");
		if(w->hidden)
			fmtprint(&f, "%shidden", w->id == cur ? "," : "");
		if(w->id == deskwin)
			fmtprint(&f, "%sdesktop", w->id == cur || w->hidden ? "," : "");
		fmtprint(&f, "\n");
	}
	return fmtstrflush(&f);
}

static void
readsnap(Req *r, char *s)
{
	readstr(r, s);
	free(s);
}

static void
fsread(Req *r)
{
	Open *o;
	char buf[256];

	o = r->fid->aux;
	switch(qid(r->fid)){
	case Qversion:
		readstr(r, "devux 1.0\nfeature focus\nfeature desktop\nfeature menubar\n");
		break;
	case Qctl:
		snprint(buf, sizeof buf, "mode %s\ncmdkey %s\n", modeon ? "on" : "off", cmdkey);
		readstr(r, buf);
		break;
	case Qwindows:
		readsnap(r, winsnapshot());
		break;
	case Qdeskctl:
		readstr(r, "claim -\nrelease -\n");
		break;
	case Qdeskstate:
		snprint(buf, sizeof buf, "win=%d rect=%s\n", deskwin, fmtrect(deskrect()));
		readstr(r, buf);
		break;
	case Qbarctl:
		readstr(r, "app -\ntitle -\nwindow -\nunwindow -\nmenu -\nitem -\nset -\n"
			"del -\nclear -\nbegin -\ncommit -\nabort -\ninvoke -\n");
		break;
	case Qbarmenus:
		readsnap(r, menusnapshot());
		break;
	case Qfakectl:
		readstr(r, "new -\nfocus -\ndelete -\nrect -\nhide -\nunhide -\ntop -\n"
			"label -\nscreen -\nclick -\nkey -\n");
		break;
	case Qfakelog:
		readstr(r, logtext != nil ? logtext : "");
		break;
	case Qevents:
	case Qdeskevents:
	case Qbarevents:
		subread(o->sub, r);
		return;
	default:
		respond(r, "bug: read");
		return;
	}
	respond(r, nil);
}

static void
fswrite(Req *r)
{
	Open *o;
	char *e;
	Deskarg a;

	o = r->fid->aux;
	if(r->ifcall.count > Maxmsg){
		respond(r, "too large");
		return;
	}
	switch(qid(r->fid)){
	case Qctl:
		e = eachline(r->ifcall.data, r->ifcall.count, uxctlcmd, nil);
		break;
	case Qdeskctl:
		a.fid = r->fid;
		a.ownwin = ownwin(r->fid);
		e = eachline(r->ifcall.data, r->ifcall.count, deskcmd, &a);
		break;
	case Qbarctl:
		e = eachline(r->ifcall.data, r->ifcall.count, barline, o->sess);
		break;
	case Qfakectl:
		e = eachline(r->ifcall.data, r->ifcall.count, fakecmd, nil);
		break;
	default:
		e = "permission denied";
		break;
	}
	if(e == nil)
		r->ofcall.count = r->ifcall.count;
	respond(r, e);
}

static void
fsflush(Req *r)
{
	subflush(r->oldreq);
	respond(r, nil);
}

static void
fsdestroyfid(Fid *f)
{
	Open *o;
	Msg m;

	o = f->aux;
	if(o == nil)
		return;
	if(o->sub != nil)
		subclose(o->sub);
	if(o->sess != nil)
		endsess(o->sess);
	if(deskfid == f){
		mbegin(&m, "release");
		mattr(&m, "win", "%d", deskwin);
		ev("desktop", mend(&m));
		mbegin(&m, "desktop");
		mattr(&m, "win", "0");
		ev("events", mend(&m));
		deskfid = nil;
		deskwin = 0;
	}
	free(o);
	f->aux = nil;
}

static Srv fs = {
	.attach = fsattach,
	.open = fsopen,
	.read = fsread,
	.write = fswrite,
	.flush = fsflush,
	.destroyfid = fsdestroyfid,
};

static void
mk(File *dir, char *name, int q, ulong perm)
{
	File *f;

	f = createfile(dir, name, nil, perm, (void*)(uintptr)q);
	if(f == nil)
		sysfatal("createfile %s: %r", name);
	closefile(f);
}

static void
usage(void)
{
	fprint(2, "usage: %s [-D] [-q qmax] [-s srvname] [-m mtpt]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *srvname, *mtpt;
	File *ux, *d, *fk;

	srvname = "fakedevux";
	mtpt = nil;
	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 'q':
		qmax = atoi(EARGF(usage()));
		if(qmax < 2)
			usage();
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc != 0)
		usage();

	fakeinit();
	/*
	 * Everything is mode 0666: the attach name rides in Fid.uid,
	 * so lib9p's owner checks wouldn't match anyway.
	 */
	fs.tree = alloctree(nil, nil, DMDIR|0777, nil);
	ux = createfile(fs.tree->root, "ux", nil, DMDIR|0777, nil);
	mk(ux, "version", Qversion, 0444);
	mk(ux, "ctl", Qctl, 0666);
	mk(ux, "events", Qevents, 0444);
	mk(ux, "windows", Qwindows, 0444);
	d = createfile(ux, "desktop", nil, DMDIR|0777, nil);
	mk(d, "ctl", Qdeskctl, 0666);
	mk(d, "state", Qdeskstate, 0444);
	mk(d, "events", Qdeskevents, 0444);
	closefile(d);
	d = createfile(ux, "menubar", nil, DMDIR|0777, nil);
	mk(d, "ctl", Qbarctl, 0666);
	mk(d, "menus", Qbarmenus, 0444);
	mk(d, "events", Qbarevents, 0444);
	closefile(d);
	closefile(ux);
	fk = createfile(fs.tree->root, "fake", nil, DMDIR|0777, nil);
	mk(fk, "ctl", Qfakectl, 0666);
	mk(fk, "log", Qfakelog, 0444);
	closefile(fk);

	postmountsrv(&fs, srvname, mtpt, MREPL|MCREATE);
	exits(nil);
}
