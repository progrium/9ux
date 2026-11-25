/*
 * Handler calls, queued under uxlk and run without it, and the
 * quoting the drawing layer's ctl lines use.
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

/* one name=value token, quoted as a whole when needed */
char*
_attr(char *name, char *val)
{
	char *s, *q;

	s = smprint("%s=%s", name, val);
	q = _quote(s);
	free(s);
	return q;
}

/* quote a token only where tokenize needs it: space, tab, quote, newline, or empty */
char*
_quote(char *s)
{
	Fmt f;
	char *p;

	if(*s != 0 && strpbrk(s, " \t'\n") == nil)
		return estrdup(s);
	fmtstrinit(&f);
	fmtrune(&f, '\'');
	for(p = s; *p; p++){
		if(*p == '\'')
			fmtrune(&f, '\'');
		fmtrune(&f, *p);
	}
	fmtrune(&f, '\'');
	return fmtstrflush(&f);
}

/* queue a handler call; run by _runnotes once uxlk is released */
void
_notify(Uxfn *fn, void *arg, Uxev *e)
{
	Note *n;

	if(fn == nil)
		return;
	n = emalloc(sizeof *n);
	n->fn = fn;
	n->arg = arg;
	n->ev = *e;
	if(e->action){
		n->s1 = estrdup(e->action);
		n->ev.action = n->s1;
	}
	if(e->text){
		n->s2 = estrdup(e->text);
		n->ev.text = n->s2;
	}
	if(e->item){
		n->ev.item = estrdup(e->item);
	}
	if(ux.notetail == nil)
		ux.notetail = &ux.notes;
	*ux.notetail = n;
	ux.notetail = &n->next;
}

/* run queued handlers; called on the UI thread without uxlk */
void
_runnotes(void)
{
	Note *n, *next;

	for(;;){
		qlock(&uxlk);
		n = ux.notes;
		ux.notes = nil;
		ux.notetail = &ux.notes;
		qunlock(&uxlk);
		if(n == nil)
			return;
		for(; n != nil; n = next){
			next = n->next;
			n->fn(&n->ev, n->arg);
			free(n->s1);
			free(n->s2);
			free(n->ev.item);
			free(n);
		}
	}
}
