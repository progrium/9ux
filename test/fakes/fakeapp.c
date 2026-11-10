/*
 * fakeapp: a fake libux app serving a fixed control tree
 * (docs/protocols/ux.md), for testing uxd and other clients before
 * libux exists.
 *
 * The tree: app "fake" (title Fake), one window with
 *	ok		button "OK" (the default button)
 *	name		text-field "Name"
 *	password	text-field "Password", secret
 *	agree		checkbox "I agree"
 * and a File menu: new (key n), close (key w), quit (key q).
 *
 * A file outside the protocol, fakectl, plays the person at the
 * screen (events with via=ui) and changes the tree:
 *	click ctlid		press a button or toggle a checkbox
 *	type ctlid text		type text into a field
 *	focus ctlid
 *	menu actionid		choose from the menu bar (via=menu)
 *	key actionid		use its shortcut (via=key)
 *	add ctlid type [label=text] [parent=ctlid]
 *	del ctlid
 *	state ctlid flag on|off	set a state flag (enabled, sensitive, ...)
 *	label ctlid text
 *	close			close the window
 *	quit			exit
 *
 * Usage: fakeapp [-D] [-r] [-q qmax] [-w winid] [-n name] [-s srvname] [-m mtpt]
 *	-r	register with uxd through /mnt/ux/register (and again
 *		whenever uxd restarts) instead of posting a public /srv
 * Without -r it posts /srv/ux.$user.$name (the uxpublic=1 mode in
 * ux.md) and, with -m, mounts it.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <draw.h>
#include "fake.h"

typedef struct Ctl Ctl;
typedef struct Node Node;
typedef struct Open Open;

enum {
	Qversion = 1,
	Qname,
	Qappid,
	Qtitle,
	Qappctl,
	Qappevents,
	Qfakectl,
	Qmtitle,
	Qmitems,
	Qmctl,
	Qmevents,
	Qwkind,
	Qwtitle,
	Qwrect,
	Qwstate,
	Qwctl,
	Qwevents,
	Qctype,
	Qclabel,
	Qcstate,
	Qcrect,
	Qcparent,
	Qcctl,
	Qcevents,
	Qcvalue,
	Qcselection,

	Maxvalue = 1024*1024,
};

enum {	/* state flags */
	Fenabled = 1<<0,
	Fvisible = 1<<1,
	Ffocused = 1<<2,
	Fdefault = 1<<3,
	Freadonly = 1<<4,
	Fmultiline = 1<<5,
	Fsensitive = 1<<6,
	Fsecret = 1<<7,
};

static char *flagname[] = {
	"enabled", "visible", "focused", "default",
	"readonly", "multiline", "sensitive", "secret", nil,
};

struct Ctl {
	char	*id;
	char	*type;		/* button, text-field, checkbox, label, group */
	char	*label;
	int	flags;
	Rectangle	r;
	char	*parent;
	char	*value;
	int	q0, q1;
	File	*dir;
	char	*path;		/* windows/N/controls/id */
	char	*topic;
	Ctl	*next;
};

struct Node {
	int	q;
	Ctl	*c;
};

struct Open {
	Sub	*sub;
	char	*buf;		/* value write, applied at clunk */
	long	n;
	int	writing;
};

typedef struct Action Action;
struct Action {
	char	*id;
	char	*label;
	char	*key;
	int	sep;
};

static Action actions[] = {
	{"new", "New", "n", 0},
	{"close", "Close", "w", 0},
	{"s1", nil, nil, 1},
	{"quit", "Quit", "q", 0},
};

static Srv	fs;
static char	*appname = "fake";
static char	*appid = "";
static char	*apptitle = "Fake";
static int	winid = 1;
static char	*wintitle = "Fake Window";
static Rectangle	winr = {100, 100, 500, 400};
static int	winhidden;
static int	winopen = 1;
static File	*wdir, *cdir;
static char	*wtopic;
static Ctl	*ctls;
static char	*user;
static int	mainpid;	/* the registering proc, with -r */

static Node*
node(int q, Ctl *c)
{
	Node *n;

	n = emalloc9p(sizeof *n);
	n->q = q;
	n->c = c;
	return n;
}

static void
mk(File *dir, char *name, int q, Ctl *c, ulong perm)
{
	File *f;

	f = createfile(dir, name, user, perm, node(q, c));
	if(f == nil)
		sysfatal("createfile %s: %r", name);
	closefile(f);
}

static Ctl*
findctl(char *id)
{
	Ctl *c;

	for(c = ctls; c != nil; c = c->next)
		if(strcmp(c->id, id) == 0)
			return c;
	return nil;
}

static char*
flagstr(int flags, char *sep)
{
	Fmt f;
	int i, n;

	fmtstrinit(&f);
	n = 0;
	for(i = 0; flagname[i] != nil; i++)
		if(flags & (1<<i))
			fmtprint(&f, "%s%s", n++ ? sep : "", flagname[i]);
	return fmtstrflush(&f);
}

static int
flagbit(char *name)
{
	int i;

	for(i = 0; flagname[i] != nil; i++)
		if(strcmp(flagname[i], name) == 0)
			return 1<<i;
	return 0;
}

/* control events go to the app, the window and the control */
static void
cev(Ctl *c, char *msg)
{
	char *t[3];

	t[0] = "app";
	t[1] = wtopic;
	t[2] = c->topic;
	emit(t, 3, msg);
}

static void
wev(char *msg)
{
	char *t[2];

	t[0] = "app";
	t[1] = wtopic;
	emit(t, 2, msg);
}

static void
mev(char *msg)
{
	char *t[2];

	t[0] = "app";
	t[1] = "menu/file";
	emit(t, 2, msg);
}

static void
valueev(Ctl *c)
{
	Msg m;

	mbegin(&m, "value");
	mattr(&m, "ctl", "%s", c->path);
	/* a secret never carries its value */
	if(c->flags & Fsecret)
		mattr(&m, "redacted", "on");
	else if(strchr(c->value, '\n') == nil && strlen(c->value) <= 1024)
		mattr(&m, "value", "%s", c->value);
	cev(c, mend(&m));
}

static void
stateev(Ctl *c)
{
	Msg m;

	mbegin(&m, "state");
	mattr(&m, "ctl", "%s", c->path);
	mattr(&m, "state", "%s", flagstr(c->flags, ","));
	cev(c, mend(&m));
}

static void
addctl(char *id, char *type, char *label, char *parent, int flags, Rectangle r)
{
	Ctl *c, **l;
	Msg m;

	c = emalloc9p(sizeof *c);
	memset(c, 0, sizeof *c);
	c->id = estrdup9p(id);
	c->type = estrdup9p(type);
	c->label = estrdup9p(label != nil ? label : "");
	c->parent = estrdup9p(parent != nil ? parent : "");
	c->flags = flags;
	c->r = r;
	c->value = estrdup9p(strcmp(type, "checkbox") == 0 ? "off" : "");
	c->path = smprint("windows/%d/controls/%s", winid, id);
	c->topic = smprint("ctl/%s", c->path);
	for(l = &ctls; *l != nil; l = &(*l)->next)
		;
	*l = c;

	c->dir = createfile(cdir, id, user, DMDIR|0555, nil);
	mk(c->dir, "type", Qctype, c, 0444);
	mk(c->dir, "label", Qclabel, c, 0444);
	mk(c->dir, "state", Qcstate, c, 0444);
	mk(c->dir, "rect", Qcrect, c, 0444);
	mk(c->dir, "parent", Qcparent, c, 0444);
	mk(c->dir, "ctl", Qcctl, c, 0666);
	mk(c->dir, "events", Qcevents, c, 0444);
	if(strcmp(type, "text-field") == 0){
		mk(c->dir, "value", Qcvalue, c, 0666);
		mk(c->dir, "selection", Qcselection, c, 0444);
	}else if(strcmp(type, "checkbox") == 0 || strcmp(type, "label") == 0)
		mk(c->dir, "value", Qcvalue, c, 0444);

	mbegin(&m, "new");
	mattr(&m, "ctl", "%s", c->path);
	mattr(&m, "type", "%s", type);
	wev(mend(&m));
}

static void
rmtree(File *f)
{
	/* lib9p removes only empty directories: children first */
	Readdir *rd;
	uchar buf[DIRMAX*16];
	long n, off;
	Dir d;
	File *ch;
	int i, m;
	char name[1024];
	char **names;
	int nnames;

	names = nil;
	nnames = 0;
	rd = opendirfile(f);
	off = 0;
	while((n = readdirfile(rd, buf, sizeof buf, off)) > 0){
		for(i = 0; i < n; i += m){
			m = BIT16SZ + GBIT16(buf+i);
			if(convM2D(buf+i, m, &d, name) != m)
				break;
			names = erealloc9p(names, (nnames+1)*sizeof names[0]);
			names[nnames++] = estrdup9p(d.name);
		}
		off += n;
	}
	closedirfile(rd);
	for(i = 0; i < nnames; i++){
		incref(f);
		ch = walkfile(f, names[i]);
		if(ch != nil){
			if(ch->qid.type & QTDIR)
				rmtree(ch);
			else
				removefile(ch);
		}
		free(names[i]);
	}
	free(names);
	removefile(f);
}

static void
delctl(Ctl *c)
{
	Ctl **l;
	Msg m;

	for(l = &ctls; *l != nil; l = &(*l)->next)
		if(*l == c){
			*l = c->next;
			break;
		}
	mbegin(&m, "delete");
	mattr(&m, "ctl", "%s", c->path);
	wev(mend(&m));
	subgone(c->topic);
	rmtree(c->dir);
	/* the Ctl itself is leaked: open fids may still point at it */
}

static void
closewin(void)
{
	Msg m;

	if(!winopen)
		return;
	winopen = 0;
	while(ctls != nil)
		delctl(ctls);
	mbegin(&m, "delete");
	mattr(&m, "win", "%d", winid);
	emit1("app", mend(&m));
	subgone(wtopic);
	rmtree(wdir);
}

static void
quit(void)
{
	subgone("app");
	subgone("menu/file");
	/* with -r, the registering proc is in another note group */
	if(mainpid != 0 && mainpid != getpid())
		postnote(PNPROC, mainpid, "kill");
	postnote(PNGROUP, getpid(), "kill");
	exits(nil);
}

/*
 * Commands. `via` is ui for fakectl, ctl for 9P.
 */
static char*
setfocus(Ctl *c)
{
	Ctl *o;
	Msg m;

	for(o = ctls; o != nil; o = o->next)
		if(o != c && (o->flags & Ffocused)){
			o->flags &= ~Ffocused;
			stateev(o);
		}
	if(!(c->flags & Ffocused)){
		c->flags |= Ffocused;
		stateev(c);
	}
	mbegin(&m, "focus");
	mattr(&m, "win", "%d", winid);
	mattr(&m, "ctl", "%s", c->path);
	wev(mend(&m));
	return nil;
}

static char*
activate(Ctl *c, char *via)
{
	Msg m;
	char *v;

	if(strcmp(c->type, "button") == 0){
		mbegin(&m, "activate");
		mattr(&m, "ctl", "%s", c->path);
		mattr(&m, "via", "%s", via);
		cev(c, mend(&m));
		return nil;
	}
	if(strcmp(c->type, "checkbox") == 0){
		v = strcmp(c->value, "on") == 0 ? "off" : "on";
		free(c->value);
		c->value = estrdup9p(v);
		valueev(c);
		return nil;
	}
	return "unknown command";
}

static void
setvalue(Ctl *c, char *v)
{
	free(c->value);
	c->value = estrdup9p(v);
	c->q0 = c->q1 = utflen(v);
	valueev(c);
}

static char*
insert(Ctl *c, char *text)
{
	char *s, *a, *b;
	Msg m;
	Rune rr;
	int i;

	/* rune offsets q0, q1 to byte offsets */
	a = c->value;
	for(i = 0; i < c->q0 && *a; i++)
		a += chartorune(&rr, a);
	b = a;
	for(; i < c->q1 && *b; i++)
		b += chartorune(&rr, b);
	s = smprint("%.*s%s%s", (int)(a - c->value), c->value, text, b);
	c->q0 += utflen(text);
	c->q1 = c->q0;
	free(c->value);
	c->value = s;
	valueev(c);
	mbegin(&m, "select");
	mattr(&m, "ctl", "%s", c->path);
	mattr(&m, "q0", "%d", c->q0);
	mattr(&m, "q1", "%d", c->q1);
	cev(c, mend(&m));
	return nil;
}

static char *buttonhelp = "activate activate\n";
static char *fieldhelp = "set edit\ninsert edit\nselect edit\nfocus activate\nsubmit activate\n";
static char *checkhelp = "set activate\ntoggle activate\n";

static char*
ctlhelp(Ctl *c)
{
	if(strcmp(c->type, "button") == 0)
		return buttonhelp;
	if(strcmp(c->type, "text-field") == 0)
		return fieldhelp;
	if(strcmp(c->type, "checkbox") == 0)
		return checkhelp;
	return "";
}

static char*
ctlcmd(char *line, void *v)
{
	Ctl *c;
	Cmd k;
	char *e, *verb;
	Msg m;
	int n;

	c = v;
	if(cmdparse(&k, line) == 0){
		cmdfree(&k);
		return nil;
	}
	verb = k.tok[0];
	n = k.ntok;
	e = nil;
	if(strstr(ctlhelp(c), smprint("%s ", verb)) == nil)
		e = "unknown command";
	else if(!(c->flags & Fenabled))
		e = "disabled";
	else if(strcmp(c->type, "button") == 0)
		e = n == 1 ? activate(c, "ctl") : "bad syntax";
	else if(strcmp(c->type, "checkbox") == 0){
		if(strcmp(verb, "toggle") == 0)
			e = n == 1 ? activate(c, "ctl") : "bad syntax";
		else if(n != 2 || !isbool(k.tok[1]))
			e = "bad syntax";
		else if(strcmp(c->value, k.tok[1]) != 0)
			e = activate(c, "ctl");
	}else if(strcmp(verb, "set") == 0){
		if(n != 2)
			e = "bad syntax";
		else if(c->flags & Freadonly)
			e = "bad value: readonly";
		else
			setvalue(c, k.tok[1]);
	}else if(strcmp(verb, "insert") == 0){
		if(n != 2)
			e = "bad syntax";
		else if(c->flags & Freadonly)
			e = "bad value: readonly";
		else
			e = insert(c, k.tok[1]);
	}else if(strcmp(verb, "select") == 0){
		int q0, q1, len;

		len = utflen(c->value);
		if(n != 3)
			e = "bad syntax";
		else if((q0 = atoi(k.tok[1])) < 0 || (q1 = atoi(k.tok[2])) < q0 || q1 > len)
			e = "bad value: selection";
		else{
			c->q0 = q0;
			c->q1 = q1;
			mbegin(&m, "select");
			mattr(&m, "ctl", "%s", c->path);
			mattr(&m, "q0", "%d", q0);
			mattr(&m, "q1", "%d", q1);
			cev(c, mend(&m));
		}
	}else if(strcmp(verb, "focus") == 0)
		e = n == 1 ? setfocus(c) : "bad syntax";
	else if(strcmp(verb, "submit") == 0){
		if(n != 1)
			e = "bad syntax";
		else if(c->flags & Fmultiline)
			e = "bad value: multiline";
		else{
			mbegin(&m, "submit");
			mattr(&m, "ctl", "%s", c->path);
			mattr(&m, "via", "ctl");
			cev(c, mend(&m));
		}
	}else
		e = "unknown command";
	cmdfree(&k);
	return e;
}

static Action*
findaction(char *id)
{
	int i;

	for(i = 0; i < nelem(actions); i++)
		if(!actions[i].sep && strcmp(actions[i].id, id) == 0)
			return &actions[i];
	return nil;
}

static void
fireaction(Action *a, int win, char *via)
{
	Msg m;

	mbegin(&m, "action");
	mattr(&m, "menu", "file");
	mattr(&m, "id", "%s", a->id);
	mattr(&m, "win", "%d", win);
	mattr(&m, "via", "%s", via);
	mev(mend(&m));
}

static char*
menucmd(char *line, void*)
{
	Cmd k;
	char *e, *v;
	Action *a;

	if(cmdparse(&k, line) == 0){
		cmdfree(&k);
		return nil;
	}
	e = nil;
	if(strcmp(k.tok[0], "invoke") != 0)
		e = "unknown command";
	else if(k.npos != 2)
		e = "bad syntax";
	else if((a = findaction(k.tok[1])) == nil)
		e = "not found: action";
	else{
		v = cmdattr(&k, "win");
		fireaction(a, v != nil ? atoi(v) : (winopen ? winid : 0), "ctl");
	}
	cmdfree(&k);
	return e;
}

static char*
winctlcmd(char *line, void*)
{
	Cmd k;
	char *e, *verb;
	Ctl *c;
	Msg m;

	if(cmdparse(&k, line) == 0){
		cmdfree(&k);
		return nil;
	}
	verb = k.tok[0];
	e = nil;
	if(strcmp(verb, "front") == 0){
		/* one window: already in front */
	}else if(strcmp(verb, "close") == 0)
		closewin();
	else if(strcmp(verb, "focus") == 0){
		if(k.ntok != 2)
			e = "bad syntax";
		else if((c = findctl(k.tok[1])) == nil)
			e = "not found";
		else if(!(c->flags & Fenabled))
			e = "disabled";
		else
			e = setfocus(c);
	}else if(strcmp(verb, "default") == 0 || strcmp(verb, "cancel") == 0)
		e = "bad value: not a dialog";
	else
		e = "unknown command";
	USED(m);
	cmdfree(&k);
	return e;
}

static char*
appctlcmd(char *line, void*)
{
	Cmd k;
	char *e, *verb;
	Msg m;

	if(cmdparse(&k, line) == 0){
		cmdfree(&k);
		return nil;
	}
	verb = k.tok[0];
	e = nil;
	if(strcmp(verb, "front") == 0){
		/* one window: already in front */
	}else if(strcmp(verb, "hide") == 0 || strcmp(verb, "show") == 0){
		if(winopen && winhidden != (verb[0] == 'h')){
			winhidden = verb[0] == 'h';
			mbegin(&m, "state");
			mattr(&m, "win", "%d", winid);
			mattr(&m, "state", "%s", winhidden ? "hidden" : "current");
			wev(mend(&m));
		}
	}else if(strcmp(verb, "quit") == 0)
		e = "quit";	/* handled by the caller after responding */
	else
		e = "unknown command";
	cmdfree(&k);
	return e;
}

static char*
fakecmd(char *line, void*)
{
	Cmd k;
	char *e, *verb;
	Ctl *c;
	Action *a;
	Msg m;
	int bit;

	if(cmdparse(&k, line) == 0){
		cmdfree(&k);
		return nil;
	}
	verb = k.tok[0];
	e = nil;
	c = k.npos > 1 ? findctl(k.tok[1]) : nil;
	if(strcmp(verb, "menu") == 0 || strcmp(verb, "key") == 0){
		if(k.npos != 2)
			e = "bad syntax";
		else if((a = findaction(k.tok[1])) == nil)
			e = "not found";
		else
			fireaction(a, winopen ? winid : 0, verb);
	}else if(strcmp(verb, "add") == 0){
		if(k.npos != 3 || !isid(k.tok[1]))
			e = "bad syntax";
		else if(c != nil)
			e = "in use";
		else if(!winopen)
			e = "gone";
		else
			addctl(k.tok[1], k.tok[2], cmdattr(&k, "label"), cmdattr(&k, "parent"),
				Fenabled|Fvisible, Rect(10, 200, 110, 220));
	}else if(strcmp(verb, "close") == 0)
		closewin();
	else if(strcmp(verb, "quit") == 0)
		e = "quit";
	else if(c == nil)
		e = k.npos < 2 ? "bad syntax" : "not found";
	else if(strcmp(verb, "click") == 0)
		e = (c->flags & Fenabled) ? activate(c, "ui") : "disabled";
	else if(strcmp(verb, "type") == 0){
		if(k.npos != 3)
			e = "bad syntax";
		else if(strcmp(c->type, "text-field") != 0)
			e = "bad value: not a text field";
		else
			e = insert(c, k.tok[2]);
	}else if(strcmp(verb, "focus") == 0)
		e = setfocus(c);
	else if(strcmp(verb, "del") == 0)
		delctl(c);
	else if(strcmp(verb, "state") == 0){
		if(k.npos != 4 || (bit = flagbit(k.tok[2])) == 0 || !isbool(k.tok[3]))
			e = "bad syntax";
		else{
			if(strcmp(k.tok[3], "on") == 0)
				c->flags |= bit;
			else
				c->flags &= ~bit;
			stateev(c);
		}
	}else if(strcmp(verb, "label") == 0){
		if(k.npos != 3)
			e = "bad syntax";
		else{
			free(c->label);
			c->label = estrdup9p(k.tok[2]);
			mbegin(&m, "label");
			mattr(&m, "ctl", "%s", c->path);
			mattr(&m, "label", "%s", c->label);
			cev(c, mend(&m));
		}
	}else
		e = "unknown command";
	cmdfree(&k);
	return e;
}

/*
 * 9P
 */
static Node*
fnode(Fid *f)
{
	return f->file->aux;
}

static void
fsopen(Req *r)
{
	Node *n;
	Open *o;

	n = fnode(r->fid);
	o = emalloc9p(sizeof *o);
	memset(o, 0, sizeof *o);
	r->fid->aux = o;
	if(n == nil){
		respond(r, nil);
		return;
	}
	switch(n->q){
	case Qappevents:
		o->sub = subopen("app");
		break;
	case Qmevents:
		o->sub = subopen("menu/file");
		break;
	case Qwevents:
		o->sub = subopen(wtopic);
		break;
	case Qcevents:
		o->sub = subopen(n->c->topic);
		break;
	case Qcvalue:
		if((r->ifcall.mode&OMASK) != OREAD){
			if(strcmp(n->c->type, "text-field") != 0){
				respond(r, "permission denied");
				return;
			}
			if(n->c->flags & Freadonly){
				respond(r, "bad value: readonly");
				return;
			}
			o->writing = 1;
			if(!(r->ifcall.mode & OTRUNC)){
				o->buf = estrdup9p(n->c->value);
				o->n = strlen(o->buf);
			}
		}
		break;
	}
	respond(r, nil);
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
	Node *n;
	Open *o;
	Ctl *c;
	Fmt f;
	int i;

	n = fnode(r->fid);
	o = r->fid->aux;
	c = n->c;
	switch(n->q){
	case Qversion:
		readstr(r, "ux 1.0\nfeature menus\nfeature value-write\n");
		break;
	case Qname:
		readsnap(r, smprint("%s\n", appname));
		break;
	case Qappid:
		readsnap(r, smprint("%s\n", appid));
		break;
	case Qtitle:
		readsnap(r, smprint("%s\n", apptitle));
		break;
	case Qappctl:
		readstr(r, "front activate\nhide activate\nshow activate\nquit activate\n");
		break;
	case Qfakectl:
		readstr(r, "click -\ntype -\nfocus -\nmenu -\nkey -\nadd -\ndel -\nstate -\n"
			"label -\nclose -\nquit -\n");
		break;
	case Qmtitle:
		readstr(r, "File\n");
		break;
	case Qmitems:
		fmtstrinit(&f);
		for(i = 0; i < nelem(actions); i++){
			if(actions[i].sep){
				fmtprint(&f, "item %s sep=on\n", actions[i].id);
				continue;
			}
			fmtprint(&f, "item %s %s %s\n", actions[i].id,
				attrq("label", actions[i].label), attrq("key", actions[i].key));
		}
		readsnap(r, fmtstrflush(&f));
		break;
	case Qmctl:
		readstr(r, "invoke activate\n");
		break;
	case Qwkind:
		readstr(r, "window\n");
		break;
	case Qwtitle:
		readsnap(r, smprint("%s\n", wintitle));
		break;
	case Qwrect:
		readsnap(r, smprint("%d %d %d %d\n", winr.min.x, winr.min.y, winr.max.x, winr.max.y));
		break;
	case Qwstate:
		readstr(r, winhidden ? "hidden\n" : "current\n");
		break;
	case Qwctl:
		readstr(r, "front activate\nclose activate\nfocus activate\n"
			"default activate\ncancel activate\n");
		break;
	case Qctype:
		readsnap(r, smprint("%s\n", c->type));
		break;
	case Qclabel:
		readsnap(r, smprint("%s\n", c->label));
		break;
	case Qcstate:
		readsnap(r, smprint("%s\n", flagstr(c->flags, " ")));
		break;
	case Qcrect:
		readsnap(r, smprint("%d %d %d %d\n", c->r.min.x, c->r.min.y, c->r.max.x, c->r.max.y));
		break;
	case Qcparent:
		readsnap(r, smprint("%s\n", c->parent));
		break;
	case Qcctl:
		readstr(r, ctlhelp(c));
		break;
	case Qcvalue:
		if(c->flags & Fsecret)
			readstr(r, (c->flags & Fmultiline) ? "" : "\n");
		else if(c->flags & Fmultiline)
			readstr(r, c->value);
		else
			readsnap(r, smprint("%s\n", c->value));
		break;
	case Qcselection:
		readsnap(r, smprint("%d %d\n", c->q0, c->q1));
		break;
	case Qappevents:
	case Qmevents:
	case Qwevents:
	case Qcevents:
		subread(o->sub, r);
		return;
	default:
		respond(r, "bug: read");
		return;
	}
	USED(o);
	respond(r, nil);
}

static int quitting;

static void
fswrite(Req *r)
{
	Node *n;
	Open *o;
	char *e;
	long off, cnt;

	n = fnode(r->fid);
	o = r->fid->aux;
	cnt = r->ifcall.count;
	if(n->q == Qcvalue){
		off = r->ifcall.offset;
		if(off+cnt > Maxvalue){
			respond(r, "too large");
			return;
		}
		if(off+cnt > o->n){
			o->buf = erealloc9p(o->buf, off+cnt+1);
			memset(o->buf+o->n, 0, off+cnt-o->n);
			o->n = off+cnt;
		}
		memmove(o->buf+off, r->ifcall.data, cnt);
		o->buf[o->n] = 0;
		r->ofcall.count = cnt;
		respond(r, nil);
		return;
	}
	if(cnt > Maxmsg){
		respond(r, "too large");
		return;
	}
	switch(n->q){
	case Qappctl:
		e = eachline(r->ifcall.data, cnt, appctlcmd, nil);
		break;
	case Qfakectl:
		e = eachline(r->ifcall.data, cnt, fakecmd, nil);
		break;
	case Qmctl:
		e = eachline(r->ifcall.data, cnt, menucmd, nil);
		break;
	case Qwctl:
		e = eachline(r->ifcall.data, cnt, winctlcmd, nil);
		break;
	case Qcctl:
		e = eachline(r->ifcall.data, cnt, ctlcmd, n->c);
		break;
	default:
		e = "permission denied";
		break;
	}
	if(e != nil && strcmp(e, "quit") == 0){
		e = nil;
		quitting = 1;
	}
	if(e == nil)
		r->ofcall.count = cnt;
	respond(r, e);
	if(quitting)
		quit();
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
	Node *n;
	char *v;

	o = f->aux;
	if(o == nil)
		return;
	if(o->sub != nil)
		subclose(o->sub);
	if(o->writing && f->file != nil && (n = f->file->aux) != nil && n->c != nil){
		v = o->buf != nil ? o->buf : "";
		/* a one-line field drops the newline `echo` adds */
		if(!(n->c->flags & Fmultiline) && o->n > 0 && v[o->n-1] == '\n')
			v[o->n-1] = 0;
		setvalue(n->c, v);
	}
	free(o->buf);
	free(o);
	f->aux = nil;
}

/*
 * Registration with uxd (ux.md "Registration with uxd")
 */
static Srv*
newsrv(void)
{
	Srv *s;

	s = emalloc9p(sizeof *s);
	memset(s, 0, sizeof *s);
	s->tree = fs.tree;
	s->open = fsopen;
	s->read = fsread;
	s->write = fswrite;
	s->flush = fsflush;
	s->destroyfid = fsdestroyfid;
	return s;
}

static int
register1(void)
{
	char name[64], buf[128], *line;
	int fd, n;

	snprint(name, sizeof name, "ux.%s.%d.%.8lux%.8lux", user, getpid(), (ulong)truerand(), (ulong)truerand());
	if(postsrv(newsrv(), name) < 0){
		fprint(2, "%s: postsrv: %r\n", argv0);
		return -1;
	}
	if((fd = open("/mnt/ux/register", ORDWR)) < 0){
		fprint(2, "%s: open /mnt/ux/register: %r\n", argv0);
		return -1;
	}
	line = smprint("register %s %s %s%s%s %s", attrq("name", appname),
		attrq("srv", smprint("/srv/%s", name)), attrq("title", apptitle),
		*appid ? " " : "", *appid ? attrq("id", appid) : "",
		attrq("win", smprint("%d", winid)));
	if(write(fd, line, strlen(line)) < 0){
		fprint(2, "%s: register: %r\n", argv0);
		close(fd);
		return -1;
	}
	n = read(fd, buf, sizeof buf - 1);
	if(n <= 0){
		close(fd);
		return -1;
	}
	buf[n] = 0;
	if(n > 0 && buf[n-1] == '\n')
		buf[n-1] = 0;
	appid = estrdup9p(buf);
	fprint(2, "%s: registered as %s\n", argv0, appid);
	/* blocks while registered */
	while(read(fd, buf, sizeof buf) > 0)
		;
	close(fd);
	return 0;
}

static void
registerloop(void)
{
	int delay;

	delay = 250;
	for(;;){
		if(register1() == 0)
			delay = 250;
		fprint(2, "%s: not registered; retrying\n", argv0);
		sleep(delay);
		if(delay < 1000)
			delay *= 2;
		/* uxd may be back: remount it */
		if(access(smprint("/srv/uxd.%s", user), AEXIST) == 0){
			int fd;

			/* the public service: registering is open to every attach */
			if((fd = open(smprint("/srv/uxd.%s", user), ORDWR)) >= 0){
				if(mount(fd, -1, "/mnt/ux", MREPL, "") < 0)
					close(fd);
			}
		}
	}
}

static void
usage(void)
{
	fprint(2, "usage: %s [-D] [-r] [-q qmax] [-w winid] [-n name] [-s srvname] [-m mtpt]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *srvname, *mtpt;
	int reg;
	File *root, *d;

	srvname = nil;
	mtpt = nil;
	reg = 0;
	ARGBEGIN{
	case 'D':
		chatty9p++;
		break;
	case 'r':
		reg = 1;
		break;
	case 'q':
		qmax = atoi(EARGF(usage()));
		if(qmax < 2)
			usage();
		break;
	case 'w':
		winid = atoi(EARGF(usage()));
		break;
	case 'n':
		appname = EARGF(usage());
		if(!isid(appname))
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
	if(argc != 0 || winid <= 0)
		usage();

	fakeinit();
	user = getuser();
	if(srvname == nil)
		srvname = smprint("ux.%s.%s", user, appname);
	wtopic = smprint("win/%d", winid);

	fs.tree = alloctree(user, user, DMDIR|0555, nil);
	root = fs.tree->root;
	mk(root, "version", Qversion, nil, 0444);
	mk(root, "name", Qname, nil, 0444);
	mk(root, "id", Qappid, nil, 0444);
	mk(root, "title", Qtitle, nil, 0444);
	mk(root, "ctl", Qappctl, nil, 0666);
	mk(root, "events", Qappevents, nil, 0444);
	mk(root, "fakectl", Qfakectl, nil, 0666);
	d = createfile(root, "menus", user, DMDIR|0555, nil);
	d = createfile(d, "file", user, DMDIR|0555, nil);
	mk(d, "title", Qmtitle, nil, 0444);
	mk(d, "items", Qmitems, nil, 0444);
	mk(d, "ctl", Qmctl, nil, 0666);
	mk(d, "events", Qmevents, nil, 0444);
	d = createfile(root, "windows", user, DMDIR|0555, nil);
	wdir = createfile(d, smprint("%d", winid), user, DMDIR|0555, nil);
	mk(wdir, "kind", Qwkind, nil, 0444);
	mk(wdir, "title", Qwtitle, nil, 0444);
	mk(wdir, "rect", Qwrect, nil, 0444);
	mk(wdir, "state", Qwstate, nil, 0444);
	mk(wdir, "ctl", Qwctl, nil, 0666);
	mk(wdir, "events", Qwevents, nil, 0444);
	cdir = createfile(wdir, "controls", user, DMDIR|0555, nil);

	addctl("ok", "button", "OK", nil, Fenabled|Fvisible|Fdefault, Rect(300, 250, 380, 275));
	addctl("name", "text-field", "Name", nil, Fenabled|Fvisible, Rect(10, 10, 380, 30));
	addctl("password", "text-field", "Password", nil, Fenabled|Fvisible|Fsecret, Rect(10, 40, 380, 60));
	addctl("agree", "checkbox", "I agree", nil, Fenabled|Fvisible, Rect(10, 70, 200, 90));
	seq = 0;	/* the setup above isn't something anyone saw */

	fs.open = fsopen;
	fs.read = fsread;
	fs.write = fswrite;
	fs.flush = fsflush;
	fs.destroyfid = fsdestroyfid;

	if(reg){
		/* own namespace: remounting /mnt/ux must not touch the caller's */
		rfork(RFNOTEG|RFNAMEG);
		mainpid = getpid();
		registerloop();
	}
	postmountsrv(&fs, srvname, mtpt, MREPL);
	exits(nil);
}
