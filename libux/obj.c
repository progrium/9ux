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

QLock uxlk;
App ux;

void*
emalloc(ulong n)
{
	void *p;

	p = mallocz(n, 1);
	if(p == nil)
		sysfatal("libux: out of memory");
	setmalloctag(p, getcallerpc(&n));
	return p;
}

void*
erealloc(void *p, ulong n)
{
	p = realloc(p, n);
	if(p == nil)
		sysfatal("libux: out of memory");
	return p;
}

char*
estrdup(char *s)
{
	if(s == nil)
		s = "";
	s = strdup(s);
	if(s == nil)
		sysfatal("libux: out of memory");
	return s;
}

/* objects are numbered from 1; 0 is the app itself */
void
_addobj(Obj *o, int kind)
{
	if(ux.nobj == 0){
		ux.obj = emalloc(sizeof(Obj*));
		ux.nobj = 1;
	}
	o->kind = kind;
	o->oid = ux.nobj;
	ux.obj = erealloc(ux.obj, (ux.nobj+1)*sizeof(Obj*));
	ux.obj[ux.nobj++] = o;
}

Obj*
_lookobj(ulong oid)
{
	if(oid == 0 || oid >= ux.nobj)
		return nil;
	return ux.obj[oid];
}

/* the object's memory is kept: Uxctl and Uxwin pointers held by the app stay valid */
void
_delobj(Obj *o)
{
	if(o->oid < ux.nobj)
		ux.obj[o->oid] = nil;
	o->dead = 1;
}

Uxwin*
_findwin(int winid)
{
	int i;

	for(i = 0; i < ux.nwin; i++)
		if(ux.win[i]->winid == winid)
			return ux.win[i];
	return nil;
}

Uxctl*
_findctl(Uxwin *w, char *id)
{
	int i;

	for(i = 0; i < w->nctl; i++)
		if(strcmp(w->ctl[i]->id, id) == 0)
			return w->ctl[i];
	return nil;
}

Uxmenu*
_findmenu(char *id)
{
	int i;

	for(i = 0; i < ux.nmenu; i++)
		if(strcmp(ux.menu[i]->id, id) == 0)
			return ux.menu[i];
	return nil;
}

/* control, action and menu IDs: [a-z0-9][a-z0-9._-]*, at most 64 bytes */
int
_validid(char *s)
{
	char *p;

	if(s == nil || *s == 0 || strlen(s) > 64)
		return 0;
	if(!(*s >= 'a' && *s <= 'z') && !(*s >= '0' && *s <= '9'))
		return 0;
	for(p = s; *p; p++)
		if(!(*p >= 'a' && *p <= 'z') && !(*p >= '0' && *p <= '9')
		&& *p != '.' && *p != '_' && *p != '-')
			return 0;
	return 1;
}

char*
_ctlpath(Uxctl *c)
{
	return smprint("windows/%d/controls/%s", c->w->winid, c->id);
}

static char *ctlflags[] = {
	"multiline", "secret", "sensitive", "readonly", nil, nil, nil, nil,
	"enabled", "visible", "default", nil,
};

static char *winflags[] = {
	"current", "hidden", "modal", nil,
};

/* flags joined by sep; win selects the window flag names */
char*
_flags(int f, int win)
{
	char **names, buf[256], *p, *e;
	int i, n;

	names = win ? winflags : ctlflags;
	n = win ? 3 : 11;
	p = buf;
	e = buf+sizeof buf;
	*p = 0;
	for(i = 0; i < n; i++){
		if(names[i] == nil || (f & (1<<i)) == 0)
			continue;
		p = seprint(p, e, "%s%s", p == buf ? "" : " ", names[i]);
	}
	return estrdup(buf);
}

char*
_runestr(Rune *r, int n)
{
	if(r == nil || n == 0)
		return estrdup("");
	return smprint("%.*S", n, r);
}
