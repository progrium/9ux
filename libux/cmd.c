/*
 * ctl commands, run on the UI thread with uxlk held.
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

static char *rights[] = { "-", "activate", "edit" };

typedef struct Gverb Gverb;
struct Gverb {
	char	*name;
	int	right;
	int	min, max;
	char*	(*fn)(Obj*, int, char**);
};

/* app */
static char*
afront(Obj*, int, char**)
{
	if(ux.front)
		_front(ux.front);
	else if(ux.nwin > 0)
		_front(ux.win[0]);
	return nil;
}

static char*
ahide(Obj*, int, char **argv)
{
	int i, hide;

	hide = strcmp(argv[0], "hide") == 0;
	for(i = 0; i < ux.nwin; i++)
		_winstate(ux.win[i], hide ? ux.win[i]->state|Whidden : ux.win[i]->state&~Whidden);
	return nil;
}

static char*
aquit(Obj*, int, char**)
{
	Uxev e;

	memset(&e, 0, sizeof e);
	e.type = UXquit;
	e.via = UXctl;
	if(ux.fn)
		_notify(ux.fn, ux.arg, &e);
	else
		ux.quit = 1;
	return nil;
}

static Gverb appverbs[] = {
	"front", Ractivate, 0, 0, afront,
	"hide", Ractivate, 0, 0, ahide,
	"show", Ractivate, 0, 0, ahide,
	"quit", Ractivate, 0, 0, aquit,
	nil,
};

/* window */
static char*
wfront(Obj *o, int, char**)
{
	_front((Uxwin*)o);
	return nil;
}

static char*
wclose(Obj *o, int, char**)
{
	_winclose((Uxwin*)o, UXctl);
	return nil;
}

static char*
wfocus(Obj *o, int, char **argv)
{
	Uxwin *w;
	Uxctl *c;

	w = (Uxwin*)o;
	c = _findctl(w, argv[1]);
	if(c == nil)
		return "not found";
	if((c->state & Cenabled) == 0)
		return "disabled";
	if(!c->t->focusable)
		return "bad value: control can't take focus";
	_setfocus(w, c, UXctl);
	return nil;
}

static char*
wdefault(Obj *o, int, char **argv)
{
	Uxwin *w;
	Uxev e;

	w = (Uxwin*)o;
	if(strcmp(w->kind, "dialog") != 0 && strcmp(w->kind, "alert") != 0)
		return "bad value: not a dialog";
	if(strcmp(argv[0], "default") == 0 && w->def != nil){
		if((w->def->state & Cenabled) == 0)
			return "disabled";
		_activate(w->def, UXctl);
		return nil;
	}
	memset(&e, 0, sizeof e);
	e.type = strcmp(argv[0], "default") == 0 ? UXdefault : UXcancel;
	e.via = UXctl;
	e.win = w;
	_notify(w->fn, w->arg, &e);
	return nil;
}

static Gverb winverbs[] = {
	"front", Ractivate, 0, 0, wfront,
	"close", Ractivate, 0, 0, wclose,
	"focus", Ractivate, 1, 1, wfocus,
	"default", Ractivate, 0, 0, wdefault,
	"cancel", Ractivate, 0, 0, wdefault,
	nil,
};

/* menu */
static char*
minvoke(Obj *o, int argc, char **argv)
{
	Uxmenu *m;
	char *p;
	int i, win;

	m = (Uxmenu*)o;
	win = 0;
	for(i = 2; i < argc; i++){
		if(strncmp(argv[i], "win=", 4) != 0)
			return "bad syntax";
		win = strtol(argv[i]+4, &p, 10);
		if(*p != 0)
			return "bad value: win";
	}
	for(i = 0; i < m->nitem; i++)
		if(!m->item[i].sep && strcmp(m->item[i].action, argv[1]) == 0)
			break;
	if(i == m->nitem)
		return "not found";
	if(!m->item[i].enabled)
		return "disabled";
	_menuaction(argv[1], win, "ctl");
	return nil;
}

static Gverb menuverbs[] = {
	"invoke", Ractivate, 1, 2, minvoke,
	nil,
};

static Gverb*
gverbs(Obj *o)
{
	if(o == nil)
		return appverbs;
	switch(o->kind){
	case Owin:	return winverbs;
	case Omenu:	return menuverbs;
	}
	return nil;
}

char*
_ctlhelp(Obj *o)
{
	Fmt f;
	Gverb *g;
	Verb *v;

	fmtstrinit(&f);
	if(o != nil && o->kind == Octl){
		for(v = ((Uxctl*)o)->t->verbs; v && v->name; v++)
			fmtprint(&f, "%s %s\n", v->name, rights[v->right]);
	}else
		for(g = gverbs(o); g && g->name; g++)
			fmtprint(&f, "%s %s\n", g->name, rights[g->right]);
	return fmtstrflush(&f);
}

static char*
run1(Obj *o, int argc, char **argv)
{
	Gverb *g;
	Verb *v;
	Uxctl *c;

	if(o != nil && o->kind == Octl){
		c = (Uxctl*)o;
		for(v = c->t->verbs; v && v->name; v++)
			if(strcmp(v->name, argv[0]) == 0)
				break;
		if(v == nil || v->name == nil)
			return "unknown command";
		if(argc-1 < v->min || (v->max >= 0 && argc-1 > v->max))
			return "bad syntax";
		if((c->state & Cenabled) == 0)
			return "disabled";
		return v->fn(c, argc, argv);
	}
	for(g = gverbs(o); g && g->name; g++)
		if(strcmp(g->name, argv[0]) == 0){
			if(argc-1 < g->min || argc-1 > g->max)
				return "bad syntax";
			return g->fn(o, argc, argv);
		}
	return "unknown command";
}

/* run a ctl write: one command per line, stopping at the first error */
char*
_runcmd(Obj *o, int, char *buf)
{
	char *p, *nl, *args[64], *err;
	int n;

	for(p = buf; p != nil && *p != 0; p = nl){
		nl = strchr(p, '\n');
		if(nl != nil)
			*nl++ = 0;
		n = tokenize(p, args, nelem(args));
		if(n == 0)
			continue;
		if(n == nelem(args))
			return estrdup("bad syntax: too many arguments");
		if((err = run1(o, n, args)) != nil)
			return estrdup(err);
	}
	return nil;
}

/* run queued 9P commands; UI thread, uxlk held */
void
_docmds(void)
{
	Cmd *c, *next;
	Obj *o;

	c = ux.cmdq;
	ux.cmdq = nil;
	ux.cmdtail = &ux.cmdq;
	for(; c != nil; c = next){
		next = c->next;
		c->next = nil;
		o = _lookobj(c->oid);
		if(c->oid != 0 && o == nil)
			c->err = estrdup("gone");
		else if(c->r == nil){
			/* a value file closed after writing */
			if(o != nil && o->kind == Octl && ((Uxctl*)o)->t->writevalue)
				c->err = ((Uxctl*)o)->t->writevalue((Uxctl*)o, c->buf);
		}else
			c->err = _runcmd(o, c->file, c->buf);
		c->next = ux.doneq;
		ux.doneq = c;
	}
	if(ux.doneq)
		nbsendul(ux.rkick, 1);
}
