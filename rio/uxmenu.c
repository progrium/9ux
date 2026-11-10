/*
 * 9/UX menu bar registrations: /dev/ux/menubar/ctl and menus.
 * See docs/protocols/devux.md#menu-bar.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <cursor.h>
#include <mouse.h>
#include <keyboard.h>
#include <frame.h>
#include <fcall.h>
#include "dat.h"
#include "fns.h"
#include "ux.h"

Uxapp	*uxapps;

static char Ebadsyntax[] = "bad syntax";
static char Ebadvalue[] = "bad value";
static char Enotfound[] = "not found";
static char Einuse[] = "in use";
static char Enoapp[] = "bad syntax: no app";

/*
 * Lookups
 */
Uxapp*
uxfindapp(char *id)
{
	Uxapp *a;

	for(a=uxapps; a; a=a->next)
		if(strcmp(a->id, id) == 0)
			return a;
	return nil;
}

Uxapp*
uxappof(int win)
{
	Uxapp *a;
	int i;

	if(win == 0)
		return nil;
	for(a=uxapps; a; a=a->next)
		for(i=0; i<a->nwins; i++)
			if(a->wins[i] == win)
				return a;
	return nil;
}

char*
uxappid(int win)
{
	Uxapp *a;

	a = uxappof(win);
	return a != nil ? a->id : nil;
}

static void
delwin(Uxapp *a, int win)
{
	int i;

	for(i=0; i<a->nwins; i++)
		if(a->wins[i] == win){
			a->nwins--;
			memmove(a->wins+i, a->wins+i+1, (a->nwins-i)*sizeof(int));
			return;
		}
}

/* the window was deleted */
void
uxappdelwin(int win)
{
	Uxapp *a;

	for(a=uxapps; a; a=a->next)
		delwin(a, win);
}

/* the focused window and its app (nil for none or a legacy window) */
int
uxforeground(Uxapp **ap)
{
	int id;

	id = 0;
	if(input != nil && !input->deleted && input->i != nil)
		id = input->id;
	*ap = uxappof(id);
	return id;
}

Uxmenu*
uxfindmenu(Uxapp *a, char *id, int win)
{
	Uxmenu *m;

	for(m=a->menus; m; m=m->next)
		if(m->win == win && strcmp(m->id, id) == 0)
			return m;
	return nil;
}

/* the menu with that id as seen from window win: its override, or the app-wide one */
Uxmenu*
uxmenufor(Uxapp *a, char *id, int win)
{
	Uxmenu *m;

	if(win != 0 && (m = uxfindmenu(a, id, win)) != nil)
		return m;
	return uxfindmenu(a, id, 0);
}

static Uxitem*
finditem(Uxmenu *m, char *action)
{
	Uxitem *i;

	for(i=m->items; i; i=i->next)
		if(strcmp(i->action, action) == 0)
			return i;
	return nil;
}

/*
 * Copies and frees
 */
static void
freeitem(Uxitem *i)
{
	free(i->action);
	free(i->label);
	free(i->key);
	free(i->sub);
	free(i);
}

static void
freemenu(Uxmenu *m)
{
	Uxitem *i, *n;

	for(i=m->items; i; i=n){
		n = i->next;
		freeitem(i);
	}
	free(m->id);
	free(m->title);
	free(m);
}

void
uxfreemenus(Uxmenu *m)
{
	Uxmenu *n;

	for(; m; m=n){
		n = m->next;
		freemenu(m);
	}
}

static char*
dupnil(char *s)
{
	return s != nil ? estrdup(s) : nil;
}

Uxitem*
uxdupitem(Uxitem *i)
{
	Uxitem *n;

	n = emalloc(sizeof(Uxitem));
	*n = *i;
	n->next = nil;
	n->action = estrdup(i->action);
	n->label = dupnil(i->label);
	n->key = dupnil(i->key);
	n->sub = dupnil(i->sub);
	return n;
}

Uxmenu*
uxdupmenu(Uxmenu *m)
{
	Uxmenu *n;
	Uxitem *i, **l;

	n = emalloc(sizeof(Uxmenu));
	*n = *m;
	n->next = nil;
	n->id = estrdup(m->id);
	n->title = dupnil(m->title);
	n->items = nil;
	l = &n->items;
	for(i=m->items; i; i=i->next){
		*l = uxdupitem(i);
		l = &(*l)->next;
	}
	return n;
}

static Uxmenu*
dupmenus(Uxmenu *m)
{
	Uxmenu *h, **l;

	h = nil;
	l = &h;
	for(; m; m=m->next){
		*l = uxdupmenu(m);
		l = &(*l)->next;
	}
	return h;
}

/*
 * Attribute parsing
 */
static int
validid(char *s)
{
	char *p;

	if(*s == 0 || strlen(s) > 64)
		return 0;
	if(!((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9')))
		return 0;
	for(p=s; *p; p++)
		if(!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '.' || *p == '_' || *p == '-'))
			return 0;
	return 1;
}

static int
validappid(char *s)
{
	char *p;

	if(!(*s >= 'a' && *s <= 'z'))
		return 0;
	for(p=s; *p; p++)
		if(!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
			return 0;
	return 1;
}

static int
parsebool(char *s, int *v)
{
	if(strcmp(s, "on") == 0)
		*v = 1;
	else if(strcmp(s, "off") == 0)
		*v = 0;
	else
		return -1;
	return 0;
}

static int
parsecheck(char *s, int *v)
{
	if(strcmp(s, "off") == 0)
		*v = Chkoff;
	else if(strcmp(s, "on") == 0)
		*v = Chkon;
	else if(strcmp(s, "mixed") == 0)
		*v = Chkmixed;
	else
		return -1;
	return 0;
}

static int
parsewin(char *s)
{
	char *e;
	long n;

	n = strtol(s, &e, 10);
	if(*s == 0 || *e != 0 || n <= 0)
		return -1;
	return n;
}

static struct {
	char	*name;
	Rune	r;
} keynames[] = {
	"up",		Kup,
	"down",	Kdown,
	"left",	Kleft,
	"right",	Kright,
	"home",	Khome,
	"end",	Kend,
	"pgup",	Kpgup,
	"pgdown",	Kpgdown,
	"ins",	Kins,
	"del",	Kdel,
	"bs",		'\b',
	"tab",	'\t',
	"esc",	Kesc,
	"enter",	'\n',
	"space",	' ',
};

/* parse a shortcut: [shift+][alt+][ctl+]key */
int
uxparsekey(char *s, Rune *rp, int *mp)
{
	int mods, n, i;
	Rune r;

	mods = 0;
	for(;;){
		if(strncmp(s, "shift+", 6) == 0 && s[6] != 0){
			mods |= Mshift;
			s += 6;
		}else if(strncmp(s, "alt+", 4) == 0 && s[4] != 0){
			mods |= Malt;
			s += 4;
		}else if(strncmp(s, "ctl+", 4) == 0 && s[4] != 0){
			mods |= Mctl;
			s += 4;
		}else
			break;
	}
	for(i=0; i<nelem(keynames); i++)
		if(strcmp(s, keynames[i].name) == 0){
			*rp = keynames[i].r;
			*mp = mods;
			return 0;
		}
	if(s[0] == 'f' && s[1] >= '1' && s[1] <= '9'){
		n = atoi(s+1);
		if(n >= 1 && n <= 12 && (s[2] == 0 || (s[3] == 0 && s[2] >= '0' && s[2] <= '9'))){
			*rp = KF|n;
			*mp = mods;
			return 0;
		}
	}
	n = chartorune(&r, s);
	if(s[n] != 0 || r < ' ' || r == Runeerror || (r >= 'A' && r <= 'Z'))
		return -1;
	*rp = r;
	*mp = mods;
	return 0;
}

/* split name=value; nil if not an attribute */
static char*
attr(char *tok, char **name)
{
	char *e;

	e = strchr(tok, '=');
	if(e == nil)
		return nil;
	*e = 0;
	*name = tok;
	return e+1;
}

/*
 * Commands
 */
static Uxmenu**
menutail(Uxapp *a)
{
	Uxmenu **l;

	for(l=&a->menus; *l; l=&(*l)->next)
		;
	return l;
}

static char*
cmdmenu(Uxapp *a, char **f, int nf)
{
	char *id, *n, *v, *title, *after;
	int i, win, top;
	Uxmenu *m, **l, *am;

	if(nf < 2)
		return Ebadsyntax;
	id = f[1];
	if(!validid(id))
		return Ebadvalue;
	title = nil;
	after = nil;
	win = 0;
	top = 1;
	for(i=2; i<nf; i++){
		if((v = attr(f[i], &n)) == nil)
			return Ebadsyntax;
		if(strcmp(n, "title") == 0)
			title = v;
		else if(strcmp(n, "win") == 0){
			if((win = parsewin(v)) < 0)
				return Ebadvalue;
		}else if(strcmp(n, "after") == 0)
			after = v;
		else if(strcmp(n, "top") == 0){
			if(parsebool(v, &top) < 0)
				return Ebadvalue;
		}else if(n[0] == 'x' && n[1] == '.')
			continue;
		else
			return Ebadsyntax;
	}
	am = nil;
	if(after != nil){
		am = uxfindmenu(a, after, win);
		if(am == nil && win != 0)
			am = uxfindmenu(a, after, 0);
		if(am == nil)
			return Enotfound;
	}
	m = uxfindmenu(a, id, win);
	if(m != nil){
		/* replace its attributes; its items stay */
		free(m->title);
		m->title = estrdup(title != nil ? title : id);
		m->top = top;
		if(am == nil || am == m)
			return nil;
		/* unlink, to move it */
		for(l=&a->menus; *l; l=&(*l)->next)
			if(*l == m){
				*l = m->next;
				break;
			}
	}else{
		m = emalloc(sizeof(Uxmenu));
		m->id = estrdup(id);
		m->title = estrdup(title != nil ? title : id);
		m->win = win;
		m->top = top;
	}
	if(am != nil){
		m->next = am->next;
		am->next = m;
	}else{
		m->next = nil;
		*menutail(a) = m;
	}
	return nil;
}

static char*
setitem(Uxitem *it, char **f, int nf, char **after, int all)
{
	int i, b;
	char *n, *v;
	Rune r;
	int mods;

	for(i=0; i<nf; i++){
		if((v = attr(f[i], &n)) == nil)
			return Ebadsyntax;
		if(strcmp(n, "label") == 0){
			free(it->label);
			it->label = estrdup(v);
		}else if(strcmp(n, "key") == 0){
			free(it->key);
			it->key = nil;
			it->krune = 0;
			it->kmods = 0;
			if(v[0] != 0){
				if(uxparsekey(v, &r, &mods) < 0)
					return Ebadvalue;
				it->key = estrdup(v);
				it->krune = r;
				it->kmods = mods;
			}
		}else if(strcmp(n, "enabled") == 0){
			if(parsebool(v, &b) < 0)
				return Ebadvalue;
			it->enabled = b;
		}else if(strcmp(n, "check") == 0){
			if(parsecheck(v, &b) < 0)
				return Ebadvalue;
			it->check = b;
		}else if(all && strcmp(n, "sub") == 0){
			free(it->sub);
			it->sub = nil;
			if(v[0] != 0){
				if(!validid(v))
					return Ebadvalue;
				it->sub = estrdup(v);
			}
		}else if(all && strcmp(n, "sep") == 0){
			if(parsebool(v, &b) < 0)
				return Ebadvalue;
			it->sep = b;
		}else if(all && strcmp(n, "after") == 0){
			*after = v;
		}else if(n[0] == 'x' && n[1] == '.')
			continue;
		else
			return Ebadsyntax;
	}
	return nil;
}

/* every sub= names a menu (checked at commit, or at once outside a batch) */
static char*
checksubs(Uxapp *a)
{
	Uxmenu *m;
	Uxitem *i;

	for(m=a->menus; m; m=m->next)
		for(i=m->items; i; i=i->next)
			if(i->sub != nil && uxmenufor(a, i->sub, m->win) == nil)
				return Enotfound;
	return nil;
}

static char*
cmditem(Uxapp *a, char **f, int nf)
{
	Uxmenu *m;
	Uxitem tmp, *it, *old, **l, *am;
	char *after, *err;

	if(nf < 3)
		return Ebadsyntax;
	if(!validid(f[2]))
		return Ebadvalue;
	/* an item goes into the app-wide menu, or the only override with that id */
	m = uxfindmenu(a, f[1], 0);
	if(m == nil)
		for(m=a->menus; m; m=m->next)
			if(strcmp(m->id, f[1]) == 0)
				break;
	if(m == nil)
		return Enotfound;
	old = finditem(m, f[2]);
	memset(&tmp, 0, sizeof tmp);
	tmp.action = f[2];
	tmp.enabled = 1;
	after = nil;
	err = setitem(&tmp, f+3, nf-3, &after, 1);
	if(err != nil){
		free(tmp.label);
		free(tmp.key);
		free(tmp.sub);
		return err;
	}
	am = nil;
	if(after != nil && (am = finditem(m, after)) == nil){
		free(tmp.label);
		free(tmp.key);
		free(tmp.sub);
		return Enotfound;
	}
	it = emalloc(sizeof(Uxitem));
	*it = tmp;
	it->action = estrdup(f[2]);
	if(old != nil){
		/* replace in place, or move after am */
		for(l=&m->items; *l; l=&(*l)->next)
			if(*l == old)
				break;
		if(am == nil || am == old){
			it->next = old->next;
			*l = it;
			freeitem(old);
			return nil;
		}
		*l = old->next;
		freeitem(old);
	}
	if(am != nil){
		it->next = am->next;
		am->next = it;
	}else{
		for(l=&m->items; *l; l=&(*l)->next)
			;
		*l = it;
	}
	if(!a->batch && it->sub != nil && uxmenufor(a, it->sub, m->win) == nil){
		/* undo */
		for(l=&m->items; *l; l=&(*l)->next)
			if(*l == it){
				*l = it->next;
				break;
			}
		freeitem(it);
		return Enotfound;
	}
	return nil;
}

static char*
cmdset(Uxapp *a, char **f, int nf)
{
	Uxmenu *m;
	Uxitem *i, tmp;
	char *err, *after;
	int found;

	if(nf < 2)
		return Ebadsyntax;
	/* validate first, on a scratch item */
	memset(&tmp, 0, sizeof tmp);
	tmp.action = f[1];
	err = setitem(&tmp, f+2, nf-2, &after, 0);
	free(tmp.label);
	free(tmp.key);
	if(err != nil)
		return err;
	found = 0;
	for(m=a->menus; m; m=m->next)
		for(i=m->items; i; i=i->next)
			if(strcmp(i->action, f[1]) == 0){
				setitem(i, f+2, nf-2, &after, 0);
				found = 1;
			}
	if(!found)
		return Enotfound;
	return nil;
}

static char*
cmddel(Uxapp *a, char **f, int nf)
{
	Uxmenu *m, **l;
	Uxitem *i, **il;
	int found;

	if(nf != 2 && nf != 3)
		return Ebadsyntax;
	found = 0;
	if(nf == 2){
		for(l=&a->menus; *l; ){
			m = *l;
			if(strcmp(m->id, f[1]) == 0){
				*l = m->next;
				freemenu(m);
				found = 1;
			}else
				l = &m->next;
		}
	}else{
		for(m=a->menus; m; m=m->next){
			if(strcmp(m->id, f[1]) != 0)
				continue;
			for(il=&m->items; *il; ){
				i = *il;
				if(strcmp(i->action, f[2]) == 0){
					*il = i->next;
					freeitem(i);
					found = 1;
				}else
					il = &i->next;
			}
		}
	}
	return found ? nil : Enotfound;
}

static char*
cmdwindow(Uxapp *a, Window *own, char **f, int nf)
{
	Window *w;
	Uxapp *o;
	int id;

	if(nf > 2)
		return Ebadsyntax;
	if(nf == 2){
		if((id = parsewin(f[1])) < 0)
			return Ebadvalue;
		w = uxwin(id);
	}else{
		if(own == nil)
			return "bad syntax: no window";
		w = uxwin(own->id);
	}
	if(w == nil)
		return Enotfound;
	o = uxappof(w->id);
	if(o == a)
		return nil;
	if(o != nil)
		return Einuse;
	a->wins = erealloc(a->wins, (a->nwins+1)*sizeof(int));
	a->wins[a->nwins++] = w->id;
	return nil;
}

static char*
cmdinvoke(char **f, int nf)
{
	Uxapp *a;
	Uxmenu *m;
	Uxitem *i;
	char *n, *v;
	int k, win, found, enabled;

	if(nf < 3)
		return Ebadsyntax;
	win = 0;
	for(k=3; k<nf; k++){
		if((v = attr(f[k], &n)) == nil)
			return Ebadsyntax;
		if(strcmp(n, "win") == 0){
			if((win = parsewin(v)) < 0)
				return Ebadvalue;
		}else if(n[0] == 'x' && n[1] == '.')
			continue;
		else
			return Ebadsyntax;
	}
	if((a = uxfindapp(f[1])) == nil)
		return Enotfound;
	found = enabled = 0;
	for(m=a->menus; m; m=m->next)
		for(i=m->items; i; i=i->next)
			if(!i->sep && strcmp(i->action, f[2]) == 0){
				found = 1;
				if(i->enabled)
					enabled = 1;
			}
	if(!found)
		return Enotfound;
	if(!enabled)
		return "disabled";
	uxpost(Smenu, "action app=%s win=%d id=%s via=ctl", a->id, win, f[2]);
	return nil;
}

static void
freeapp(Uxapp *a)
{
	uxfreemenus(a->menus);
	uxfreemenus(a->saved);
	free(a->savedtitle);
	free(a->title);
	free(a->id);
	free(a->wins);
	free(a);
}

static void
endbatch(Uxapp *a)
{
	uxfreemenus(a->saved);
	a->saved = nil;
	free(a->savedtitle);
	a->savedtitle = nil;
	a->batch = 0;
}

/* one command line; returns an error or nil */
char*
uxmenuctl(Uxfid *u, Window *own, char *line)
{
	char *f[64], *n, *v, *title, *err;
	int nf, i;
	Uxapp *a;

	nf = tokenize(line, f, nelem(f));
	if(nf == 0)
		return nil;
	if(nf == nelem(f))
		return "too large";
	if(strcmp(f[0], "invoke") == 0)
		return cmdinvoke(f, nf);
	a = u->app;
	if(strcmp(f[0], "app") == 0){
		if(a != nil)
			return Einuse;
		if(nf < 2)
			return Ebadsyntax;
		if(!validappid(f[1]))
			return Ebadvalue;
		title = nil;
		for(i=2; i<nf; i++){
			if((v = attr(f[i], &n)) == nil)
				return Ebadsyntax;
			if(strcmp(n, "title") == 0)
				title = v;
			else if(n[0] == 'x' && n[1] == '.')
				continue;
			else
				return Ebadsyntax;
		}
		if(uxfindapp(f[1]) != nil)
			return Einuse;
		a = emalloc(sizeof(Uxapp));
		a->id = estrdup(f[1]);
		a->title = estrdup(title != nil ? title : f[1]);
		a->next = uxapps;
		uxapps = a;
		u->app = a;
		return nil;
	}
	if(a == nil)
		return Enoapp;
	err = nil;
	if(strcmp(f[0], "title") == 0){
		if(nf != 2)
			return Ebadsyntax;
		free(a->title);
		a->title = estrdup(f[1]);
	}else if(strcmp(f[0], "window") == 0)
		err = cmdwindow(a, own, f, nf);
	else if(strcmp(f[0], "unwindow") == 0){
		if(nf != 2)
			return Ebadsyntax;
		if((i = parsewin(f[1])) < 0)
			return Ebadvalue;
		if(uxappof(i) != a)
			return Enotfound;
		delwin(a, i);
	}else if(strcmp(f[0], "menu") == 0)
		err = cmdmenu(a, f, nf);
	else if(strcmp(f[0], "item") == 0)
		err = cmditem(a, f, nf);
	else if(strcmp(f[0], "set") == 0)
		err = cmdset(a, f, nf);
	else if(strcmp(f[0], "del") == 0)
		err = cmddel(a, f, nf);
	else if(strcmp(f[0], "clear") == 0){
		if(nf != 1)
			return Ebadsyntax;
		uxfreemenus(a->menus);
		a->menus = nil;
	}else if(strcmp(f[0], "begin") == 0){
		if(nf != 1)
			return Ebadsyntax;
		if(a->batch)
			return Einuse;
		a->saved = dupmenus(a->menus);
		a->savedtitle = estrdup(a->title);
		a->batch = 1;
		return nil;
	}else if(strcmp(f[0], "commit") == 0){
		if(nf != 1)
			return Ebadsyntax;
		if(!a->batch)
			return Ebadsyntax;
		if((err = checksubs(a)) != nil)
			return err;
		endbatch(a);
	}else if(strcmp(f[0], "abort") == 0){
		if(nf != 1)
			return Ebadsyntax;
		if(!a->batch)
			return Ebadsyntax;
		uxfreemenus(a->menus);
		a->menus = a->saved;
		a->saved = nil;
		free(a->title);
		a->title = a->savedtitle;
		a->savedtitle = nil;
		a->batch = 0;
		return nil;
	}else
		return "unknown command";
	if(err == nil){
		uxchanged();
		if(!a->batch)
			uxbardraw();
	}
	return err;
}

void
uxmenuclose(Uxfid *u)
{
	Uxapp *a, **l;

	a = u->app;
	if(a == nil)
		return;
	u->app = nil;
	for(l=&uxapps; *l; l=&(*l)->next)
		if(*l == a){
			*l = a->next;
			break;
		}
	freeapp(a);
	uxchanged();
	uxbardraw();
}

/*
 * The menus snapshot, in the ctl grammar
 */
static void
fmtitem(Fmt *f, char *menu, Uxitem *i)
{
	fmtprint(f, "item %s %s", menu, i->action);
	if(i->sep)
		fmtprint(f, " sep=on");
	else{
		fmtattr(f, "label", i->label);
		fmtattr(f, "key", i->key);
		if(!i->enabled)
			fmtprint(f, " enabled=off");
		if(i->check == Chkon)
			fmtprint(f, " check=on");
		else if(i->check == Chkmixed)
			fmtprint(f, " check=mixed");
		fmtattr(f, "sub", i->sub);
	}
	fmtprint(f, "\n");
}

char*
uxmenusnap(int *np)
{
	Fmt f;
	Uxapp *a, **as;
	Uxmenu *m;
	Uxitem *i;
	int k, n, win;
	char *s;

	fmtstrinit(&f);
	win = uxforeground(&a);
	if(a != nil)
		fmtprint(&f, "focus app=%s win=%d\n", a->id, win);
	else
		fmtprint(&f, "focus win=%d\n", win);
	/* oldest registration first */
	n = 0;
	for(a=uxapps; a; a=a->next)
		n++;
	as = emalloc((n+1)*sizeof(Uxapp*));
	k = n;
	for(a=uxapps; a; a=a->next)
		as[--k] = a;
	for(k=0; k<n; k++){
		a = as[k];
		fmtprint(&f, "app %s", a->id);
		fmtattr(&f, "title", a->title);
		fmtprint(&f, "\n");
		for(win=0; win<a->nwins; win++)
			fmtprint(&f, "window %d\n", a->wins[win]);
		for(m=a->menus; m; m=m->next){
			fmtprint(&f, "menu %s", m->id);
			fmtattr(&f, "title", m->title);
			if(m->win != 0)
				fmtprint(&f, " win=%d", m->win);
			if(!m->top)
				fmtprint(&f, " top=off");
			fmtprint(&f, "\n");
			for(i=m->items; i; i=i->next)
				fmtitem(&f, m->id, i);
		}
	}
	free(as);
	s = fmtstrflush(&f);
	*np = strlen(s);
	return s;
}

/*
 * The menus the bar shows for window win of app a, in bar order:
 * the app-wide top-level menus, each replaced by the window's override
 * with the same id, then the window's other overrides.  Copies, so the
 * bar can track them while the app changes its registration.
 */
Uxmenu*
uxbarmenus(Uxapp *a, int win)
{
	Uxmenu *h, **l, *m, *o;

	h = nil;
	l = &h;
	for(m=a->menus; m; m=m->next){
		if(m->win != 0 || !m->top)
			continue;
		o = uxfindmenu(a, m->id, win);
		if(win == 0 || o == nil)
			o = m;
		*l = uxdupmenu(o);
		l = &(*l)->next;
	}
	if(win != 0)
		for(m=a->menus; m; m=m->next){
			if(m->win != win || !m->top || uxfindmenu(a, m->id, 0) != nil)
				continue;
			*l = uxdupmenu(m);
			l = &(*l)->next;
		}
	return h;
}

/*
 * A Command shortcut was typed in window win.  Returns 1 if a menu
 * item has the shortcut (and so the key is swallowed), 0 if not.
 */
static Uxitem*
matchmenu(Uxmenu *m, Rune r, int mods)
{
	Uxitem *i;

	for(i=m->items; i; i=i->next)
		if(!i->sep && i->key != nil && i->krune == r && i->kmods == mods)
			return i;
	return nil;
}

int
uxmatchkey(Rune r, int mods)
{
	Uxapp *a;
	Uxmenu *m;
	Uxitem *i;
	int win;

	win = uxforeground(&a);
	if(a == nil)
		return 0;
	i = nil;
	/* the window's overrides, then top-level menus in bar order, then submenus */
	for(m=a->menus; m && i==nil; m=m->next)
		if(m->win == win && m->win != 0)
			i = matchmenu(m, r, mods);
	for(m=a->menus; m && i==nil; m=m->next)
		if(m->win == 0 && m->top && uxfindmenu(a, m->id, win) == nil)
			i = matchmenu(m, r, mods);
	for(m=a->menus; m && i==nil; m=m->next)
		if(m->win == 0 && !m->top)
			i = matchmenu(m, r, mods);
	if(i == nil)
		return 0;
	if(i->enabled)
		uxpost(Smenu, "action app=%s win=%d id=%s via=key", a->id, win, i->action);
	return 1;
}

void
uxfreeitem(Uxitem *i)
{
	freeitem(i);
}
