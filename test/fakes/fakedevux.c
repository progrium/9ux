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
	Qdragctl,
	Qdragstate,
	Qdragevents,
	Qdragdata,
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
	int	chrome;
	int	zoomed;
	Rectangle	unzoomed;
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
typedef struct Dataopen Dataopen;
struct Open {
	Sub	*sub;
	Sess	*sess;
	Dataopen	*data;	/* drag/data */
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

static int	ownwin(Fid*);

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
 * Drag and drop (devux 1.1): the broker, with fake/ctl's press, move
 * and release standing in for the pointer.
 */
typedef struct Drag Drag;
typedef struct Blob Blob;

enum {
	Dover,
	Ddropped,

	Maxdrag = 16*1024*1024,

	/* data fid modes */
	Mnone = 0,
	Mput,
	Mreply,
	Mfetch,
};

struct Blob {
	char	*type;
	int	item;		/* -1: the type as a whole */
	int	req;		/* 0: put by the source */
	char	*buf;
	long	n;
	int	done;
	char	*err;
	int	ref;
	Drag	*d;		/* nil once the drag has ended */
	Blob	*next;
};

struct Drag {
	int	id;
	int	src;
	char	*app;
	char	*types;
	char	*ops;
	char	*label;
	Fid	*sess;
	int	state;
	int	target;
	char	*atype;		/* accepted type and op, nil if not accepted */
	char	*aop;
	Point	pt;
	long	total;
	Blob	*blobs;
	Drag	*next;
};

static Drag	*drags;
static int	dragseq;
static int	reqseq;
static int	down;		/* a button is down (fake/ctl press) */
static int	ptwin;		/* window under the pointer */
static Point	ptat;
static Req	**dwait;	/* blocked data reads */
static int	ndwait;

static Drag*
finddrag(int id)
{
	Drag *d;

	for(d = drags; d != nil; d = d->next)
		if(d->id == id)
			return d;
	return nil;
}

static Drag*
flying(void)
{
	Drag *d;

	for(d = drags; d != nil; d = d->next)
		if(d->state == Dover)
			return d;
	return nil;
}

/* is word in the comma-separated list? */
static int
inlist(char *list, char *word)
{
	char *p, *e;
	int n;

	n = strlen(word);
	for(p = list; p != nil && *p; p = e){
		e = strchr(p, ',');
		if(e == nil)
			e = p+strlen(p);
		if(e-p == n && strncmp(p, word, n) == 0)
			return 1;
		if(*e == ',')
			e++;
	}
	return 0;
}

static char*
firstof(char *list)
{
	char *s, *p;

	s = estrdup9p(list);
	if((p = strchr(s, ',')) != nil)
		*p = 0;
	return s;
}

/* a list of IDs, or of ops */
static int
goodlist(char *list, int ops)
{
	char *s, *f[32];
	int i, n;

	if(list == nil || *list == 0)
		return 0;
	s = estrdup9p(list);
	n = getfields(s, f, nelem(f), 0, ",");
	for(i = 0; i < n; i++)
		if(ops ? strcmp(f[i], "copy") != 0 && strcmp(f[i], "move") != 0 && strcmp(f[i], "link") != 0 : !isid(f[i])){
			free(s);
			return 0;
		}
	free(s);
	return 1;
}

static void dragwake(void);

static void
blobput(Blob *b)
{
	if(--b->ref > 0)
		return;
	free(b->type);
	free(b->buf);
	free(b->err);
	free(b);
}

static void
dragmsg(char *verb, Drag *d, int win)
{
	Msg m;

	mbegin(&m, verb);
	mattr(&m, "id", "%d", d->id);
	if(win >= 0)
		mattr(&m, "win", "%d", win);
	ev("drag", mend(&m));
}

static void
dragend(Drag *d)
{
	Drag **l;
	Blob *b, *nb;

	for(l = &drags; *l != nil; l = &(*l)->next)
		if(*l == d){
			*l = d->next;
			break;
		}
	for(b = d->blobs; b != nil; b = nb){
		nb = b->next;
		b->d = nil;
		if(!b->done){
			b->done = 1;
			if(b->err == nil)
				b->err = estrdup9p("gone");
		}
		blobput(b);
	}
	dragmsg("end", d, -1);
	free(d->app);
	free(d->types);
	free(d->ops);
	free(d->label);
	free(d->atype);
	free(d->aop);
	free(d);
	dragwake();
}

static void
unaccept(Drag *d)
{
	free(d->atype);
	free(d->aop);
	d->atype = d->aop = nil;
}

static void
dragcancel(Drag *d)
{
	dragmsg("cancel", d, -1);
	dragend(d);
}

/* the pointer moved (or the button came up) while a drag is in flight */
static void
dragpointer(void)
{
	Drag *d;
	Msg m;

	d = flying();
	if(d == nil)
		return;
	if(ptwin != d->target){
		if(d->target != 0)
			dragmsg("leave", d, d->target);
		unaccept(d);
		d->target = ptwin;
		d->pt = ptat;
		if(ptwin != 0){
			mbegin(&m, "enter");
			mattr(&m, "id", "%d", d->id);
			mattr(&m, "win", "%d", ptwin);
			mattr(&m, "pt", "%d,%d", ptat.x, ptat.y);
			mattr(&m, "types", "%s", d->types);
			mattr(&m, "ops", "%s", d->ops);
			ev("drag", mend(&m));
		}
	}else if(ptwin != 0 && !eqpt(ptat, d->pt)){
		d->pt = ptat;
		mbegin(&m, "over");
		mattr(&m, "id", "%d", d->id);
		mattr(&m, "win", "%d", ptwin);
		mattr(&m, "pt", "%d,%d", ptat.x, ptat.y);
		ev("drag", mend(&m));
	}
	if(down)
		return;
	if(d->target == 0 || d->atype == nil){
		dragcancel(d);
		return;
	}
	d->state = Ddropped;
	mbegin(&m, "drop");
	mattr(&m, "id", "%d", d->id);
	mattr(&m, "win", "%d", d->target);
	mattr(&m, "pt", "%d,%d", d->pt.x, d->pt.y);
	mattr(&m, "type", "%s", d->atype);
	mattr(&m, "op", "%s", d->aop);
	ev("drag", mend(&m));
}

static char*
dragstart(Fid *fid, Cmd *c)
{
	Drag *d;
	Win *w;
	char *v;
	int win;
	Msg m;

	v = cmdattr(c, "win");
	win = v != nil ? atoi(v) : ownwin(fid);
	if(win == 0)
		return "bad syntax: no window";
	if((w = findwin(win)) == nil)
		return "not found";
	if(!goodlist(cmdattr(c, "types"), 0))
		return "bad value: types";
	v = cmdattr(c, "ops");
	if(v != nil && !goodlist(v, 1))
		return "bad value: ops";
	if(!down)
		return "bad value: no button down";
	if(flying() != nil)
		return "busy";
	d = emalloc9p(sizeof *d);
	memset(d, 0, sizeof *d);
	d->id = ++dragseq;
	d->src = win;
	d->app = w->app != nil && w->app->app != nil ? estrdup9p(w->app->app) : nil;
	d->types = estrdup9p(cmdattr(c, "types"));
	d->ops = estrdup9p(v != nil ? v : "copy");
	d->label = cmdattr(c, "label") != nil ? estrdup9p(cmdattr(c, "label")) : nil;
	d->sess = fid;
	d->state = Dover;
	d->next = drags;
	drags = d;
	mbegin(&m, "start");
	mattr(&m, "id", "%d", d->id);
	mattr(&m, "win", "%d", win);
	if(d->app != nil)
		mattr(&m, "app", "%s", d->app);
	mattr(&m, "types", "%s", d->types);
	mattr(&m, "ops", "%s", d->ops);
	if(d->label != nil)
		mattr(&m, "label", "%s", d->label);
	ev("drag", mend(&m));
	dragpointer();
	return nil;
}

typedef struct Dragarg Dragarg;
struct Dragarg {
	Fid	*fid;
};

static char*
dragcmd(char *line, void *v)
{
	Dragarg *a;
	Cmd c;
	Drag *d;
	char *e, *type, *op;
	int win;
	Msg m;

	a = v;
	if(cmdparse(&c, line) == 0){
		cmdfree(&c);
		return nil;
	}
	logf("drag/ctl %s", line);
	e = nil;
	d = c.npos > 1 ? finddrag(atoi(c.tok[1])) : nil;
	win = cmdattr(&c, "win") != nil ? atoi(cmdattr(&c, "win")) : ownwin(a->fid);
	if(strcmp(c.tok[0], "start") == 0){
		e = c.npos != 1 ? "bad syntax" : dragstart(a->fid, &c);
	}else if(strcmp(c.tok[0], "cancel") == 0){
		if(c.npos == 1)
			d = flying();
		if(c.npos > 2)
			e = "bad syntax";
		else if(d == nil || d->state != Dover)
			e = "not found";
		else
			dragcancel(d);
	}else if(strcmp(c.tok[0], "accept") == 0 || strcmp(c.tok[0], "refuse") == 0){
		if(c.npos != 2)
			e = "bad syntax";
		else if(d == nil)
			e = "not found";
		else if(win == 0)
			e = "bad syntax: no window";
		else if(c.tok[0][0] == 'r'){
			if(d->state == Dover && d->target == win && d->atype != nil){
				unaccept(d);
				dragmsg("refuse", d, win);
			}
		}else{
			type = cmdattr(&c, "type");
			op = cmdattr(&c, "op");
			if(type != nil && !inlist(d->types, type))
				e = "bad value: type not offered";
			else if(op != nil && !inlist(d->ops, op))
				e = "bad value: op not allowed";
			else if(d->state == Dover && d->target == win){
				unaccept(d);
				d->atype = type != nil ? estrdup9p(type) : firstof(d->types);
				d->aop = op != nil ? estrdup9p(op) : firstof(d->ops);
				mbegin(&m, "accept");
				mattr(&m, "id", "%d", d->id);
				mattr(&m, "win", "%d", win);
				mattr(&m, "type", "%s", d->atype);
				mattr(&m, "op", "%s", d->aop);
				ev("drag", mend(&m));
			}
		}
	}else if(strcmp(c.tok[0], "done") == 0){
		op = cmdattr(&c, "op");
		if(c.npos != 2)
			e = "bad syntax";
		else if(d == nil || d->state != Ddropped)
			e = "not found";
		else if(op != nil && !inlist(d->ops, op))
			e = "bad value: op not allowed";
		else{
			mbegin(&m, "done");
			mattr(&m, "id", "%d", d->id);
			mattr(&m, "win", "%d", d->target);
			mattr(&m, "op", "%s", op != nil ? op : d->aop);
			if(cmdattr(&c, "err") != nil)
				mattr(&m, "err", "%s", cmdattr(&c, "err"));
			ev("drag", mend(&m));
			dragend(d);
		}
	}else
		e = "unknown command";
	cmdfree(&c);
	return e;
}

static char*
dragsnapshot(void)
{
	Fmt f;
	Drag *d, **v;
	int i, n;

	n = 0;
	for(d = drags; d != nil; d = d->next)
		n++;
	v = emalloc9p((n+1)*sizeof v[0]);
	i = n;
	for(d = drags; d != nil; d = d->next)
		v[--i] = d;	/* oldest first */
	fmtstrinit(&f);
	for(i = 0; i < n; i++){
		d = v[i];
		fmtprint(&f, "drag id=%d win=%d", d->id, d->src);
		if(d->app != nil)
			fmtprint(&f, " app=%s", d->app);
		fmtprint(&f, " types=%s ops=%s", d->types, d->ops);
		if(d->label != nil)
			fmtprint(&f, " %s", attrq("label", d->label));
		fmtprint(&f, " state=%s target=%d", d->state == Dover ? "over" : "dropped", d->target);
		if(d->atype != nil)
			fmtprint(&f, " type=%s op=%s", d->atype, d->aop);
		fmtprint(&f, "\n");
	}
	free(v);
	return fmtstrflush(&f);
}

/*
 * drag/data
 */
struct Dataopen {
	int	mode;
	Blob	*b;
	long	off;
};

static Blob*
newblob(Drag *d, char *type, int item, int req)
{
	Blob *b;

	b = emalloc9p(sizeof *b);
	memset(b, 0, sizeof *b);
	b->type = estrdup9p(type);
	b->item = item;
	b->req = req;
	b->ref = 2;	/* the drag's list, and the fid */
	b->d = d;
	b->next = d->blobs;
	d->blobs = b;
	return b;
}

static Blob*
findreq(int req)
{
	Drag *d;
	Blob *b;

	for(d = drags; d != nil; d = d->next)
		for(b = d->blobs; b != nil; b = b->next)
			if(b->req == req && !b->done)
				return b;
	return nil;
}

/* the first write on a data fid */
static char*
datafirst(Dataopen *o, Fid *fid, char *line)
{
	Cmd c;
	Drag *d;
	Blob *b;
	char *e, *v;
	int item;
	Msg m;

	USED(fid);
	if(cmdparse(&c, line) == 0){
		cmdfree(&c);
		return "bad syntax";
	}
	logf("drag/data %s", line);
	e = nil;
	v = cmdattr(&c, "item");
	item = v != nil ? atoi(v) : -1;
	if(strcmp(c.tok[0], "put") == 0 || strcmp(c.tok[0], "fetch") == 0){
		d = c.npos == 3 ? finddrag(atoi(c.tok[1])) : nil;
		if(c.npos != 3)
			e = "bad syntax";
		else if(d == nil)
			e = "not found";
		else if(!inlist(d->types, c.tok[2]))
			e = "bad value: type not offered";
		else if(v != nil && item < 0)
			e = "bad value: item";
		else if(c.tok[0][0] == 'p'){
			o->mode = Mput;
			o->b = newblob(d, c.tok[2], item, 0);
		}else if(d->state != Ddropped)
			e = "bad value: not dropped";
		else{
			o->mode = Mfetch;
			for(b = d->blobs; b != nil; b = b->next)
				if(b->req == 0 && b->item == item && strcmp(b->type, c.tok[2]) == 0)
					break;
			if(b != nil){
				b->ref++;
				o->b = b;
			}else{
				o->b = newblob(d, c.tok[2], item, ++reqseq);
				mbegin(&m, "fetch");
				mattr(&m, "id", "%d", d->id);
				mattr(&m, "type", "%s", c.tok[2]);
				if(item >= 0)
					mattr(&m, "item", "%d", item);
				mattr(&m, "req", "%d", o->b->req);
				ev("drag", mend(&m));
			}
		}
	}else if(strcmp(c.tok[0], "reply") == 0 || strcmp(c.tok[0], "fail") == 0){
		b = c.npos >= 2 ? findreq(atoi(c.tok[1])) : nil;
		if(c.npos < 2 || (c.tok[0][0] == 'r' && c.npos != 2))
			e = "bad syntax";
		else if(b == nil)
			e = "not found";
		else if(c.tok[0][0] == 'r'){
			o->mode = Mreply;
			b->ref++;
			o->b = b;
		}else{
			b->err = estrdup9p(c.npos > 2 ? c.tok[2] : "failed");
			b->done = 1;
			dragwake();
		}
	}else
		e = "unknown command";
	cmdfree(&c);
	return e;
}

static char*
datawrite(Dataopen *o, Fid *fid, char *data, long n)
{
	char *line, *e, *p;
	Blob *b;
	long k;

	if(o->mode == Mnone){
		/* the first line is the command; the rest of the write is payload */
		p = memchr(data, '\n', n);
		k = p != nil ? p-data : n;
		line = emalloc9p(k+1);
		memmove(line, data, k);
		line[k] = 0;
		e = datafirst(o, fid, line);
		free(line);
		if(e != nil || p == nil || k+1 == n)
			return e;
		if(o->mode != Mput && o->mode != Mreply)
			return "bad syntax: data after the command";
		data += k+1;
		n -= k+1;
	}
	if(o->mode == Mfetch)
		return "bad syntax: fetching";
	b = o->b;
	if(b->d == nil)
		return "gone";
	if(b->d->total+n > Maxdrag)
		return "too large";
	b->buf = erealloc9p(b->buf, b->n+n);
	memmove(b->buf+b->n, data, n);
	b->n += n;
	b->d->total += n;
	dragwake();
	return nil;
}

/* answer a data read if it can be; 0 if it must wait */
static int
dataread(Req *r)
{
	Dataopen *o;
	Blob *b;
	long n;

	o = ((Open*)r->fid->aux)->data;
	if(o == nil || o->mode != Mfetch){
		respond(r, "bad syntax: not fetching");
		return 1;
	}
	b = o->b;
	if(o->off < b->n){
		n = b->n - o->off;
		if(n > r->ifcall.count)
			n = r->ifcall.count;
		memmove(r->ofcall.data, b->buf+o->off, n);
		o->off += n;
		r->ofcall.count = n;
		respond(r, nil);
		return 1;
	}
	if(!b->done)
		return 0;
	if(b->err != nil){
		respond(r, b->err);
		return 1;
	}
	r->ofcall.count = 0;
	respond(r, nil);
	return 1;
}

static void
dragwake(void)
{
	int i;

	for(i = 0; i < ndwait; ){
		if(dataread(dwait[i])){
			ndwait--;
			memmove(dwait+i, dwait+i+1, (ndwait-i)*sizeof dwait[0]);
		}else
			i++;
	}
}

static void
datawait(Req *r)
{
	if(dataread(r))
		return;
	dwait = erealloc9p(dwait, (ndwait+1)*sizeof dwait[0]);
	dwait[ndwait++] = r;
}

static int
dataflush(Req *old)
{
	int i;

	for(i = 0; i < ndwait; i++)
		if(dwait[i] == old){
			ndwait--;
			memmove(dwait+i, dwait+i+1, (ndwait-i)*sizeof dwait[0]);
			respond(old, "interrupted");
			return 1;
		}
	return 0;
}

static void
dataclose(Dataopen *o)
{
	if(o->b != nil){
		if(o->mode == Mput || o->mode == Mreply){
			o->b->done = 1;
			dragwake();
		}
		blobput(o->b);
	}
	free(o);
}

/* the fid that started drags is gone: so are they */
static void
dragsessclose(Fid *f)
{
	Drag *d, *nd;

	for(d = drags; d != nil; d = nd){
		nd = d->next;
		if(d->sess != f)
			continue;
		if(d->state == Dover)
			dragmsg("cancel", d, -1);
		dragend(d);
	}
}

/* fake/ctl: press, move, release */
static char*
fakepointer(Cmd *c)
{
	char *v;
	Point p;

	p = ZP;
	if((v = cmdattr(c, "pt")) != nil){
		p.x = atoi(v);
		v = strchr(v, ',');
		if(v == nil)
			return "bad value: pt";
		p.y = atoi(v+1);
	}
	if(strcmp(c->tok[0], "release") == 0){
		if(c->npos != 1)
			return "bad syntax";
		down = 0;
		dragpointer();
		return nil;
	}
	if(c->npos != 2)
		return "bad syntax";
	if(atoi(c->tok[1]) != 0 && findwin(atoi(c->tok[1])) == nil)
		return "not found";
	ptwin = atoi(c->tok[1]);
	ptat = p;
	if(c->tok[0][0] == 'p')
		down = 1;
	dragpointer();
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
	if(strcmp(verb, "press") == 0 || strcmp(verb, "move") == 0 || strcmp(verb, "release") == 0){
		e = fakepointer(&c);
	}else if(strcmp(verb, "new") == 0){
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
			|| strcmp(verb, "top") == 0 || strcmp(verb, "label") == 0
			|| strcmp(verb, "closebox") == 0 || strcmp(verb, "zoombox") == 0) ? "not found" : "unknown command";
	}else if(strcmp(verb, "closebox") == 0 || strcmp(verb, "zoombox") == 0){
		/* a click in a window's chrome (devux 1.2) */
		if(!w->chrome || !modeon)
			e = "bad value: no chrome";
		else if(verb[0] == 'c'){
			mbegin(&m, "close");
			mattr(&m, "win", "%d", id);
			ev("events", mend(&m));
			if(w->app == nil)
				e = fakecmd(smprint("delete %d", id), nil);
		}else{
			if(w->zoomed)
				w->r = w->unzoomed;
			else{
				w->unzoomed = w->r;
				w->r = deskrect();
				w->r.min.y += 20;	/* title bar */
				w->r = insetrect(w->r, 2);	/* frame and shadow */
			}
			w->zoomed = !w->zoomed;
			mbegin(&m, "rect");
			mattr(&m, "win", "%d", id);
			mattr(&m, "rect", "%s", fmtrect(w->r));
			ev("events", mend(&m));
		}
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
uxctlcmd(char *line, void *arg)
{
	Cmd c;
	char *e;
	Msg m;
	int on, id;
	Win *w;

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
	}else if(strcmp(c.tok[0], "chrome") == 0){
		/* chrome [winid] on|off (devux 1.2) */
		id = c.ntok == 3 ? atoi(c.tok[1]) : (int)(uintptr)arg;
		if((c.ntok != 2 && c.ntok != 3) || !isbool(c.tok[c.ntok-1]))
			e = "bad syntax";
		else if(id == 0)
			e = "bad syntax: no window";
		else if((w = findwin(id)) == nil)
			e = "not found";
		else if(id == deskwin)
			e = "bad value: the desktop window has no chrome";
		else{
			on = strcmp(c.tok[c.ntok-1], "on") == 0;
			if(on != w->chrome){
				w->chrome = on;
				mbegin(&m, "chrome");
				mattr(&m, "win", "%d", id);
				mattr(&m, "chrome", "%s", on ? "on" : "off");
				ev("events", mend(&m));
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
	case Qdragevents:
		o->sub = subopen("drag");
		break;
	case Qdragdata:
		o->data = emalloc9p(sizeof *o->data);
		memset(o->data, 0, sizeof *o->data);
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
		if(w->chrome)
			fmtprint(&f, "%schrome", w->id == cur || w->hidden || w->id == deskwin ? "," : "");
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
		readstr(r, "devux 1.2\nfeature focus\nfeature desktop\nfeature menubar\nfeature drag\nfeature chrome\n");
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
	case Qdragctl:
		readstr(r, "start -\naccept -\nrefuse -\ndone -\ncancel -\n");
		break;
	case Qdragstate:
		readsnap(r, dragsnapshot());
		break;
	case Qdragdata:
		datawait(r);
		return;
	case Qfakectl:
		readstr(r, "new -\nfocus -\ndelete -\nrect -\nhide -\nunhide -\ntop -\nclosebox -\nzoombox -\n"
			"label -\nscreen -\nclick -\nkey -\npress -\nmove -\nrelease -\n");
		break;
	case Qfakelog:
		readstr(r, logtext != nil ? logtext : "");
		break;
	case Qevents:
	case Qdeskevents:
	case Qbarevents:
	case Qdragevents:
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
	if(r->ifcall.count > Maxmsg && qid(r->fid) != Qdragdata){
		respond(r, "too large");
		return;
	}
	switch(qid(r->fid)){
	case Qctl:
		e = eachline(r->ifcall.data, r->ifcall.count, uxctlcmd, (void*)(uintptr)ownwin(r->fid));
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
	case Qdragctl:
		{
			Dragarg da;

			da.fid = r->fid;
			e = eachline(r->ifcall.data, r->ifcall.count, dragcmd, &da);
		}
		break;
	case Qdragdata:
		e = datawrite(o->data, r->fid, r->ifcall.data, r->ifcall.count);
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
	if(!subflush(r->oldreq))
		dataflush(r->oldreq);
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
	if(o->data != nil)
		dataclose(o->data);
	dragsessclose(f);
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
	d = createfile(ux, "drag", nil, DMDIR|0777, nil);
	mk(d, "ctl", Qdragctl, 0666);
	mk(d, "state", Qdragstate, 0444);
	mk(d, "events", Qdragevents, 0444);
	mk(d, "data", Qdragdata, 0666);
	closefile(d);
	closefile(ux);
	fk = createfile(fs.tree->root, "fake", nil, DMDIR|0777, nil);
	mk(fk, "ctl", Qfakectl, 0666);
	mk(fk, "log", Qfakelog, 0444);
	closefile(fk);

	postmountsrv(&fs, srvname, mtpt, MREPL|MCREATE);
	exits(nil);
}
