/*
 * The controls' own verbs, as ctl lines: the tree runs them (tapi.c)
 * through _dxrun, with uxlk held.
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

char*
_ctlhelp(Obj *o)
{
	Fmt f;
	Verb *v;

	fmtstrinit(&f);
	if(o != nil && o->kind == Octl)
		for(v = ((Uxctl*)o)->t->verbs; v && v->name; v++)
			fmtprint(&f, "%s %s\n", v->name, rights[v->right]);
	return fmtstrflush(&f);
}

static char*
run1(Obj *o, int argc, char **argv)
{
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
	return "unknown command";
}

/* run ctl lines: one command per line, stopping at the first error */
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

/* for the tree (tapi.c): run one of a control's own verbs */
char*
_dxrun(Uxctl *c, char *line)
{
	char *buf, *err;

	buf = estrdup(line);
	qlock(&uxlk);
	err = _runcmd((Obj*)c, 0, buf);
	qunlock(&uxlk);
	free(buf);
	if(ux.uikick)
		nbsendul(ux.uikick, 1);
	return err;
}
