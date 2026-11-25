/*
 * The app's state: windows, node trees, props, theme, keys and menus,
 * the type catalog, and the checks every write goes through.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "tree.h"

App	*app;
Type	*types;
int	ntypes;
char	*mode = "light";
char	*clip;
long	nclip;
int	now;
int	quitting;
static int	nextid = 1;

void*
emalloc(ulong n)
{
	return emalloc9p(n);
}

void*
erealloc(void *p, ulong n)
{
	return erealloc9p(p, n);
}

char*
estrdup(char *s)
{
	return estrdup9p(s != nil ? s : "");
}

/* s[0:n] as a string, less one trailing newline */
char*
trim1(char *s, long n)
{
	char *t;

	if(n > 0 && s[n-1] == '\n')
		n--;
	t = emalloc(n+1);
	memmove(t, s, n);
	t[n] = 0;
	return t;
}

extern char *catalog[];

static char*
attrof(char **f, int n, char *name)
{
	int i, l;

	l = strlen(name);
	for(i = 0; i < n; i++)
		if(strncmp(f[i], name, l) == 0 && f[i][l] == '=')
			return estrdup(f[i]+l+1);
	return nil;
}

static int
hasfield(char **f, int n, char *w)
{
	int i;

	for(i = 0; i < n; i++)
		if(strcmp(f[i], w) == 0)
			return 1;
	return 0;
}

static void
parsespec(Type *t)
{
	char *buf, *line, *nl, *f[32], *kind, *s, *p;
	int n, i;
	Pdesc *pd;

	buf = estrdup(t->spec);
	for(line = buf; line != nil && *line; line = nl){
		nl = strchr(line, '\n');
		if(nl != nil)
			*nl++ = 0;
		n = getfields(line, f, nelem(f), 1, " \t");
		if(n < 1 || f[0][0] == 0 || f[0][0] == '#')
			continue;
		if(strcmp(f[0], "type") == 0)
			t->overlay = hasfield(f+1, n-1, "tier=overlay");
		else if(strcmp(f[0], "prop") == 0 && n >= 3){
			t->p = erealloc(t->p, (t->np+1)*sizeof(Pdesc));
			pd = &t->p[t->np++];
			memset(pd, 0, sizeof *pd);
			pd->name = estrdup(f[1]);
			pd->form = estrdup(f[2]);
			pd->file = hasfield(f+3, n-3, "file");
			pd->readonly = hasfield(f+3, n-3, "readonly");
			pd->dflt = attrof(f+3, n-3, "default");
			pd->min = attrof(f+3, n-3, "min");
			pd->max = attrof(f+3, n-3, "max");
		}else if(strcmp(f[0], "word") == 0){
			kind = attrof(f+1, n-1, "kind");
			if(kind == nil)
				kind = estrdup("flag");
			for(i = 1; i < n; i++){
				if(strchr(f[i], '=') != nil)
					continue;
				t->word = erealloc(t->word, (t->nword+1)*sizeof(char*));
				t->wkind = erealloc(t->wkind, (t->nword+1)*sizeof(char*));
				t->word[t->nword] = estrdup(f[i]);
				t->wkind[t->nword] = kind;
				t->nword++;
			}
		}else if(strcmp(f[0], "file") == 0 && n >= 2){
			for(i = 0; i < t->np; i++)
				if(strcmp(t->p[i].name, f[1]) == 0){
					free(t->p[i].fmt);
					t->p[i].fmt = attrof(f+2, n-2, "format");
				}
		}else if(strcmp(f[0], "verb") == 0 && n >= 2){
			t->verb = erealloc(t->verb, (t->nverb+1)*sizeof(char*));
			t->vclass = erealloc(t->vclass, (t->nverb+1)*sizeof(char*));
			t->verb[t->nverb] = estrdup(f[1]);
			s = attrof(f+2, n-2, "class");
			t->vclass[t->nverb] = s != nil ? s : estrdup("edit");
			t->nverb++;
		}else if(strcmp(f[0], "slot") == 0 && n >= 2){
			t->slot = erealloc(t->slot, (t->nslot+1)*sizeof(char*));
			t->slotok = erealloc(t->slotok, (t->nslot+1)*sizeof(char*));
			t->slot[t->nslot] = estrdup(f[1]);
			t->slotok[t->nslot] = attrof(f+2, n-2, "accepts");
			t->nslot++;
		}else if(strcmp(f[0], "child") == 0 && n >= 2){
			if(strcmp(f[1], "none") == 0)
				t->child = 0;
			else if(strcmp(f[1], "any") == 0)
				t->child = 1;
			else{
				t->child = 2;
				s = estrdup(f[1]);
				for(p = strtok(s, ","); p != nil; p = strtok(nil, ",")){
					t->childok = erealloc(t->childok, (t->nchildok+1)*sizeof(char*));
					t->childok[t->nchildok++] = estrdup(p);
				}
			}
		}
	}
	free(buf);
}

Type*
typebyname(char *name)
{
	int i;

	for(i = 0; i < ntypes; i++)
		if(strcmp(types[i].name, name) == 0)
			return &types[i];
	return nil;
}

Pdesc*
pdesc(Type *t, char *name)
{
	int i;

	if(t == nil)
		return nil;
	for(i = 0; i < t->np; i++)
		if(strcmp(t->p[i].name, name) == 0)
			return &t->p[i];
	return nil;
}

/* the kind of bare word w on n's type, or nil */
char*
nodeword(Node *n, char *w)
{
	Type *t;
	int i;

	t = typebyname(n->type);
	if(t == nil)
		return nil;
	for(i = 0; i < t->nword; i++)
		if(strcmp(t->word[i], w) == 0)
			return t->wkind[i];
	return nil;
}

/*
 * Theme tokens: the portable ones, in 9/UX's palette.
 */
char *tokname[Ntok] = {
	"background", "foreground", "muted", "muted-foreground",
	"primary", "primary-foreground", "accent", "accent-foreground",
	"border", "ring", "danger", "success", "warning",
	"secondary", "secondary-foreground", "card", "card-foreground",
	"popover", "popover-foreground", "input", "destructive",
	"radius",
};

static char *tokdefault[Ntok] = {
	"#ffffff", "#000000", "#cccccc", "#555555",
	"#1f6f6f", "#ffffff", "#ffe98c", "#000000",
	"#000000", "#1f6f6f", "#c0392b", "#2e7d32", "#c77c02",
	"#e0e0e0", "#000000", "#ffffff", "#000000",
	"#fffff0", "#000000", "#ffffff", "#c0392b",
	"0",
};

/*
 * Style words: core.md's portable ones, as style/words lists them.
 */
char *stylewords[] = {
	"flex", "flex-row", "flex-col", "v-flex", "h-flex", "flex-1", "flex-wrap", "flex-none",
	"items-start", "items-center", "items-end", "items-stretch", "items-baseline",
	"justify-start", "justify-center", "justify-end", "justify-between",
	"gap=", "p=", "px=", "py=", "pt=", "pb=", "pl=", "pr=", "m=", "mx=", "my=",
	"w=", "h=", "min-w=", "min-h=", "max-w=", "max-h=", "size-full", "w-full", "h-full",
	"absolute", "relative", "top=", "left=", "right=", "bottom=",
	"overflow-hidden", "overflow-y-scroll", "overflow-x-scroll",
	"bg=", "text-color=", "border-1", "border-color=", "rounded-sm", "rounded-md", "rounded-lg",
	"text-xs", "text-sm", "text-base", "text-lg", "text-xl", "text-2xl", "text-3xl",
	"font-normal", "font-semibold", "font-bold",
	"text-left", "text-center", "text-right", "truncate",
	nil,
};

static char *stateprefix[] = {
	"hover", "active", "focus", "focus-visible", "disabled", "selected",
	"checked", "open", "drop-over", "group-hover", nil,
};

static int
inlist(char **l, char *s)
{
	for(; *l != nil; l++)
		if(strcmp(*l, s) == 0)
			return 1;
	return 0;
}

static int
islen(char *v, int pct)
{
	char *e;

	if(strcmp(v, "full") == 0)
		return pct;
	strtod(v, &e);
	if(e == v)
		return 0;
	if(*e == 0 || strcmp(e, "px") == 0 || strcmp(e, "rem") == 0)
		return 1;
	if(strcmp(e, "%") == 0)
		return pct;
	return 0;
}

char*
checkcolor(char *v)
{
	int i, n;

	if(v[0] == '#'){
		n = strlen(v+1);
		if(n != 3 && n != 4 && n != 6 && n != 8)
			return "bad value";
		for(i = 1; v[i]; i++)
			if(strchr("0123456789abcdefABCDEF", v[i]) == nil)
				return "bad value";
		return nil;
	}
	for(i = 0; i < Ntok; i++)
		if(strcmp(tokname[i], v) == 0 && strcmp(v, "radius") != 0)
			return nil;
	return "bad value";
}

static char*
checkword(char *w)
{
	char *q, *v, *name;
	char buf[64];
	int r;

	q = strchr(w, ':');
	v = strchr(w, '=');
	if(q != nil && (v == nil || q < v)){
		if(q - w >= sizeof buf)
			return smprint("unknown style word: %s", w);
		memmove(buf, w, q-w);
		buf[q-w] = 0;
		if(!inlist(stateprefix, buf))
			return smprint("unknown style word: %s", w);
		return checkword(q+1) != nil ? smprint("%s", checkword(q+1)) : nil;
	}
	if(v == nil){
		if(inlist(stylewords, w))
			return nil;
		snprint(buf, sizeof buf, "%s=", w);
		if(inlist(stylewords, buf))
			return smprint("bad value: %s needs a value", w);
		return smprint("unknown style word: %s", w);
	}
	name = emalloc(v-w+2);
	memmove(name, w, v-w+1);
	name[v-w+1] = 0;
	v++;
	if(!inlist(stylewords, name)){
		name[v-w-1] = 0;
		if(inlist(stylewords, name)){
			free(name);
			return smprint("bad value: %s takes no value", w);
		}
		free(name);
		return smprint("unknown style word: %s", w);
	}
	name[v-w-1] = 0;
	if(strcmp(name, "bg") == 0 || strcmp(name, "text-color") == 0 || strcmp(name, "border-color") == 0)
		r = checkcolor(v) == nil;
	else if(name[0] == 'w' || name[0] == 'h' || strncmp(name, "min-", 4) == 0 || strncmp(name, "max-", 4) == 0)
		r = islen(v, 1);
	else
		r = islen(v, 0);
	free(name);
	if(!r)
		return smprint("bad value: %s", w);
	return nil;
}

/* nil, or the error for the first bad word */
char*
checkstyle(char *s)
{
	char *buf, *f[256], *e;
	int i, n;

	buf = estrdup(s);
	n = getfields(buf, f, nelem(f), 1, " \t\n");
	e = nil;
	for(i = 0; i < n && e == nil; i++)
		if(f[i][0] != 0)
			e = checkword(f[i]);
	free(buf);
	return e;
}

static char *onwords[] = {
	"click", "hover", "mouse-down", "mouse-up", "move", "scroll", "key",
	"focus", "blur", "drop", "drag", "change", "submit", nil,
};

char*
checkon(char *s)
{
	char *buf, *f[64], *e;
	int i, n;

	buf = estrdup(s);
	n = getfields(buf, f, nelem(f), 1, " \t\n,");
	e = nil;
	for(i = 0; i < n && e == nil; i++)
		if(f[i][0] != 0 && !inlist(onwords, f[i]))
			e = smprint("bad value: on %s", f[i]);
	free(buf);
	return e;
}

static char *mods[] = { "cmd", "ctrl", "alt", "shift", "super", nil };
static char *namedkeys[] = {
	"escape", "esc", "enter", "return", "tab", "space", "backspace", "delete",
	"del", "insert", "home", "end", "pageup", "pagedown", "up", "down", "left",
	"right", "plus", "minus", nil,
};

char*
checkstroke(char *s)
{
	char *buf, *f[16], *k;
	int n, i, j;
	Rune r;

	buf = estrdup(s);
	n = getfields(buf, f, nelem(f), 0, "-");
	for(i = 0; i < n-1; i++){
		if(!inlist(mods, f[i]))
			goto bad;
		for(j = 0; j < i; j++)
			if(strcmp(f[i], f[j]) == 0)
				goto bad;
	}
	k = f[n-1];
	if(n < 1 || k[0] == 0)
		goto bad;
	if(inlist(namedkeys, k))
		goto ok;
	if((k[0] == 'f' || k[0] == 'F') && k[1] >= '1' && k[1] <= '9' && atoi(k+1) <= 24 && strspn(k+1, "0123456789") == strlen(k+1))
		goto ok;
	if(strlen(k) == chartorune(&r, k) && r > ' ')
		goto ok;
bad:
	free(buf);
	return smprint("bad value: keystroke %s", s);
ok:
	free(buf);
	return nil;
}

static Win*
winof(Node *n)
{
	while(n->parent != nil)
		n = n->parent;
	return n->win;
}

/* a prop's value, checked against its descriptor */
/*
 * A data file, by its format (components.md § Data file formats): a
 * malformed line refuses the whole file.
 */
static int
isnum(char *s)
{
	char *e;

	strtod(s, &e);
	return e != s && *e == 0;
}

static char*
checkline(char *fmt, char **f, int n, char **ids, int nids)
{
	int i, j;

	if(strcmp(fmt, "items") == 0){
		if(f[0][0] == '#' || (n == 1 && strcmp(f[0], "-") == 0))
			return nil;
		if(f[0][0] == 0)
			return smprint("bad value: an item needs an id");
	}else if(strcmp(fmt, "columns") == 0){
		if(f[0][0] == 0)
			return smprint("bad value: a column needs an id");
		if(n >= 3 && f[2][0] != 0 && !isnum(f[2]))
			return smprint("bad value: width %s", f[2]);
		for(i = 3; i < n; i++)
			if(strcmp(f[i], "sort") != 0 && strcmp(f[i], "fixed") != 0
			&& strcmp(f[i], "align=left") != 0 && strcmp(f[i], "align=center") != 0
			&& strcmp(f[i], "align=right") != 0)
				return smprint("bad value: column flag %s", f[i]);
	}else if(strcmp(fmt, "tree-items") == 0){
		if(f[0][0] == 0 || n < 2)
			return smprint("bad value: a tree item is id parent label");
		if(strcmp(f[1], "-") != 0){
			for(j = 0; j < nids; j++)
				if(strcmp(ids[j], f[1]) == 0)
					break;
			if(j == nids)
				return smprint("not found: parent %s", f[1]);
		}
	}else if(strcmp(fmt, "ops") == 0){
		return checkopsline(f, n);
	}else if(strcmp(fmt, "desc-items") == 0){
		if(f[0][0] == 0)
			return smprint("bad value: an item needs a label");
	}
	return nil;
}

char*
checkdata(char *fmt, char *v)
{
	char *buf, *line, *nl, *f[64], *e, **ids, *cp;
	int n, nids, pass;

	e = nil;
	ids = nil;
	nids = 0;
	cp = nil;
	/* the first pass gathers ids, so a parent may come after its child */
	for(pass = 0; pass < 2 && e == nil; pass++){
		free(cp);
		cp = buf = estrdup(v);
		for(line = buf; line != nil && *line && e == nil; line = nl){
			nl = strchr(line, '\n');
			if(nl != nil)
				*nl++ = 0;
			if(line[0] == 0)
				continue;
			if(strchr(line, '\t') != nil)
				n = getfields(line, f, nelem(f), 0, "\t");
			else
				n = tokenize(line, f, nelem(f));
			if(n < 1)
				continue;
			if(pass == 0){
				ids = erealloc(ids, (nids+1)*sizeof(char*));
				ids[nids++] = estrdup(f[0]);
			}else
				e = checkline(fmt, f, n, ids, nids);
		}
	}
	free(cp);
	while(nids > 0)
		free(ids[--nids]);
	free(ids);
	return e;
}

char*
checkvalue(Node *n, Pdesc *p, char *v)
{
	char *e, *f, *s, *t;
	double d;
	Node *x;
	Win *w;

	if(p->readonly)
		return smprint("bad value: %s is read-only", p->name);
	if(strcmp(p->form, "data") == 0 && p->fmt != nil)
		return checkdata(p->fmt, v);
	f = p->form;
	if(strcmp(f, "word") == 0){
		if(v[0] == 0 || strpbrk(v, " \t\n") != nil)
			return smprint("bad value: %s takes one word", p->name);
	}else if(strcmp(f, "bool") == 0){
		if(strcmp(v, "on") != 0 && strcmp(v, "off") != 0)
			return smprint("bad value: %s is on or off", p->name);
	}else if(strcmp(f, "number") == 0){
		/* an empty value is unset: progress's indeterminate */
		if(v[0] == 0 && strcmp(p->name, "value") == 0)
			return nil;
		d = strtod(v, &e);
		if(e == v || *e != 0)
			return smprint("bad value: %s is a number", p->name);
		if(p->min != nil && d < atof(p->min))
			return smprint("bad value: %s under %s", p->name, p->min);
		if(p->max != nil && d > atof(p->max))
			return smprint("bad value: %s over %s", p->name, p->max);
	}else if(strncmp(f, "enum=", 5) == 0){
		s = estrdup(f+5);
		for(t = strtok(s, ","); t != nil; t = strtok(nil, ","))
			if(strcmp(t, v) == 0)
				break;
		free(s);
		if(t == nil)
			return smprint("bad value: %s", v);
	}else if(strcmp(f, "child") == 0){
		if(kidbyname(n, v) == nil)
			return smprint("not found: no child %s", v);
	}else if(strcmp(f, "path") == 0){
		w = winof(n);
		if(w == nil || resolve(w->root, v, &x) != nil)
			return smprint("not found: %s", v);
	}
	return nil;
}

/* the node files every node has */
static char *nodefiles[] = {
	"type", "style", "order", "tag", "on", "context", "ctl", "events",
	"tree", "bounds", nil,
};
static char *winfiles[] = { "title", "inner", "state", "focus", "over", nil };

int
isnodefile(char *s, int root)
{
	return inlist(nodefiles, s) || (root && inlist(winfiles, s));
}

/*
 * Nodes.
 */
Node*
newnode(Node *parent, char *name, char *type)
{
	Node *n;

	n = emalloc(sizeof *n);
	memset(n, 0, sizeof *n);
	n->id = nextid++;
	n->name = estrdup(name);
	n->type = estrdup(type);
	n->style = estrdup("");
	n->bounds = estrdup("0 0 0 0");
	n->parent = parent;
	if(parent != nil){
		parent->kid = erealloc(parent->kid, (parent->nkid+1)*sizeof(Node*));
		parent->kid[parent->nkid++] = n;
		parent->ord = erealloc(parent->ord, (parent->nord+1)*sizeof(int));
		parent->ord[parent->nord++] = n->id;
		n->win = parent->win;
		if(parent->parent == nil && parent->win != nil)
			parent->win->shown = 1;
	}
	return n;
}

static void
freenode(Node *n)
{
	int i;

	for(i = 0; i < n->nkid; i++)
		freenode(n->kid[i]);
	for(i = 0; i < n->nprop; i++){
		free(n->prop[i].name);
		free(n->prop[i].val);
	}
	free(n->prop);
	free(n->kid);
	free(n->ord);
	free(n->name);
	free(n->type);
	free(n->style);
	free(n->bounds);
	free(n);
}

static int
under(Node *n, int id)
{
	int i;

	if(n->id == id)
		return 1;
	for(i = 0; i < n->nkid; i++)
		if(under(n->kid[i], id))
			return 1;
	return 0;
}

void
delnode(Node *n)
{
	Node *p;
	Win *w;
	int i, j;

	p = n->parent;
	w = winof(n);
	if(w != nil && w->focus != 0 && under(n, w->focus))
		w->focus = 0;
	if(p != nil){
		for(i = j = 0; i < p->nkid; i++)
			if(p->kid[i] != n)
				p->kid[j++] = p->kid[i];
		p->nkid = j;
		for(i = j = 0; i < p->nord; i++)
			if(p->ord[i] != n->id)
				p->ord[j++] = p->ord[i];
		p->nord = j;
	}
	freenode(n);
}

Node*
kidbyname(Node *n, char *name)
{
	int i;

	for(i = 0; i < n->nkid; i++)
		if(strcmp(n->kid[i]->name, name) == 0)
			return n->kid[i];
	return nil;
}

/* a child, or a window root's over/ */
Node*
walkkid(Node *n, char *name)
{
	Node *c;

	if((c = kidbyname(n, name)) != nil)
		return c;
	if(n->parent == nil && n->win != nil && n->win->over != nil && strcmp(name, "over") == 0)
		return n->win->over;
	return nil;
}

static Node*
findid(Node *n, int id)
{
	Node *x;
	int i;

	if(n->id == id)
		return n;
	for(i = 0; i < n->nkid; i++)
		if((x = findid(n->kid[i], id)) != nil)
			return x;
	return nil;
}

Node*
nodebyid(int id)
{
	Node *n;
	int i;

	if(id <= 0)
		return nil;
	for(i = 0; i < app->nwin; i++)
		if((n = findid(app->win[i]->root, id)) != nil
		|| (n = findid(app->win[i]->over, id)) != nil)
			return n;
	return nil;
}

Win*
winbyname(char *name)
{
	int i;

	for(i = 0; i < app->nwin; i++)
		if(strcmp(app->win[i]->name, name) == 0)
			return app->win[i];
	return nil;
}

/* the window-relative path: "." for the root */
char*
relpath(Node *n)
{
	char *s, *t;

	if(n->parent == nil)
		return estrdup(".");
	s = estrdup(n->name);
	for(n = n->parent; n->parent != nil; n = n->parent){
		t = smprint("%s/%s", n->name, s);
		free(s);
		s = t;
	}
	return s;
}

/* the path from $ui */
char*
uipath(Node *n)
{
	char *r, *s;
	Win *w;

	w = winof(n);
	r = relpath(n);
	if(strcmp(r, ".") == 0)
		s = smprint("win/%s", w != nil ? w->name : "?");
	else
		s = smprint("win/%s/%s", w != nil ? w->name : "?", r);
	free(r);
	return s;
}

int
ancestors(Node *n, int **a)
{
	int k;

	k = 0;
	*a = nil;
	for(; n != nil; n = n->parent){
		*a = erealloc(*a, (k+1)*sizeof(int));
		(*a)[k++] = n->id;
	}
	return k;
}

char*
getprop(Node *n, char *name)
{
	int i;

	for(i = 0; i < n->nprop; i++)
		if(strcmp(n->prop[i].name, name) == 0)
			return n->prop[i].val;
	return nil;
}

void
setprop(Node *n, char *name, char *val, int bare)
{
	int i;

	for(i = 0; i < n->nprop; i++)
		if(strcmp(n->prop[i].name, name) == 0){
			free(n->prop[i].val);
			n->prop[i].val = estrdup(val);
			n->prop[i].bare = bare;
			return;
		}
	n->prop = erealloc(n->prop, (n->nprop+1)*sizeof(Prop));
	n->prop[n->nprop].name = estrdup(name);
	n->prop[n->nprop].val = estrdup(val);
	n->prop[n->nprop].bare = bare;
	n->nprop++;
}

void
rmprop(Node *n, char *name)
{
	int i, j;

	for(i = j = 0; i < n->nprop; i++){
		if(strcmp(n->prop[i].name, name) == 0){
			free(n->prop[i].name);
			free(n->prop[i].val);
			continue;
		}
		n->prop[j++] = n->prop[i];
	}
	n->nprop = j;
}

static int
inlistc(char *list, char *s)
{
	char *buf, *p;
	int r;

	if(list == nil)
		return 1;
	buf = estrdup(list);
	r = 0;
	for(p = strtok(buf, ","); p != nil; p = strtok(nil, ","))
		if(strcmp(p, s) == 0)
			r = 1;
	free(buf);
	return r;
}

/* can n hold a child of type t named name? A slot takes what it accepts. */
char*
canhold(Node *n, char *t, char *name)
{
	Type *pt;
	int i;

	/* overlays live in over/, and over/ holds only overlays */
	if(strcmp(n->type, "@over") == 0){
		if(typebyname(t) == nil || !typebyname(t)->overlay)
			return smprint("unknown type: over/ holds only overlays, not %s", t);
		return nil;
	}
	if(typebyname(t) != nil && typebyname(t)->overlay)
		return smprint("unknown type: %s lives in over/", t);
	pt = typebyname(n->type);
	if(pt == nil)
		return nil;
	if(name != nil)
		for(i = 0; i < pt->nslot; i++)
			if(strcmp(pt->slot[i], name) == 0){
				if(inlistc(pt->slotok[i], t))
					return nil;
				return smprint("unknown type: %s's %s slot takes %s", n->type, name, pt->slotok[i]);
			}
	if(pt->child == 0)
		return smprint("unknown type: %s holds no children", n->type);
	if(pt->child == 2){
		for(i = 0; i < pt->nchildok; i++)
			if(strcmp(pt->childok[i], t) == 0)
				return nil;
		return smprint("unknown type: %s can't hold %s", n->type, t);
	}
	return nil;
}

static int
commonprop(char *s)
{
	return strcmp(s, "tag") == 0 || strcmp(s, "on") == 0 || strcmp(s, "context") == 0;
}

char*
settype(Node *n, char *t)
{
	Type *ty;
	char *e;
	int i;

	if((ty = typebyname(t)) == nil)
		return smprint("unknown type: %s", t);
	if(strcmp(n->type, t) == 0)
		return nil;
	if(n->fixed)
		return smprint("type is fixed: %s", n->type);
	if(n->parent == nil)
		return smprint("type is fixed: a window's root");
	if((e = canhold(n->parent, t, n->name)) != nil)
		return e;
	if(ty->child == 0 && n->nkid > 0)
		return smprint("unknown type: %s holds no children", t);
	free(n->type);
	n->type = estrdup(t);
	for(i = 0; i < n->nprop; ){
		if(commonprop(n->prop[i].name)
		|| (n->prop[i].name[0] == '@' && nodeword(n, n->prop[i].val) != nil)
		|| pdesc(ty, n->prop[i].name) != nil)
			i++;
		else
			rmprop(n, n->prop[i].name);
	}
	return nil;
}

/*
 * Windows.
 */
Win*
newwin(char *name)
{
	Win *w;

	w = emalloc(sizeof *w);
	memset(w, 0, sizeof *w);
	w->name = estrdup(name);
	w->title = estrdup("");
	w->w = 640;
	w->h = 480;
	w->pos = estrdup("default");
	w->kind = estrdup("normal");
	w->titlebar = estrdup("system");
	w->resizable = 1;
	w->state = estrdup("normal");
	w->ask = -1;
	w->root = newnode(nil, "root", "div");
	w->root->win = w;
	w->over = newnode(nil, "over", "@over");
	w->over->parent = w->root;
	w->over->win = w;
	app->win = erealloc(app->win, (app->nwin+1)*sizeof(Win*));
	app->win[app->nwin++] = w;
	return w;
}

static void
freewin(Win *w)
{
	freenode(w->root);
	freenode(w->over);
	free(w->name);
	free(w->title);
	free(w->pos);
	free(w->kind);
	free(w->titlebar);
	free(w->state);
	free(w);
}

void
delwin(Win *w)
{
	int i, j;

	for(i = j = 0; i < app->nwin; i++)
		if(app->win[i] != w)
			app->win[j++] = app->win[i];
	app->nwin = j;
	freewin(w);
}

/*
 * Snapshots, for all-or-nothing ctl writes and batches.
 */
static Node*
nodecopy(Node *n, Node *parent, Win *w)
{
	Node *c;
	int i;

	c = emalloc(sizeof *c);
	*c = *n;
	c->parent = parent;
	c->win = w;
	c->name = estrdup(n->name);
	c->type = estrdup(n->type);
	c->style = estrdup(n->style);
	c->bounds = estrdup(n->bounds);
	c->prop = emalloc((n->nprop+1)*sizeof(Prop));
	for(i = 0; i < n->nprop; i++){
		c->prop[i].name = estrdup(n->prop[i].name);
		c->prop[i].val = estrdup(n->prop[i].val);
		c->prop[i].bare = n->prop[i].bare;
	}
	c->ord = emalloc((n->nord+1)*sizeof(int));
	memmove(c->ord, n->ord, n->nord*sizeof(int));
	c->kid = emalloc((n->nkid+1)*sizeof(Node*));
	for(i = 0; i < n->nkid; i++)
		c->kid[i] = nodecopy(n->kid[i], c, w);
	return c;
}

App*
appcopy(App *a)
{
	App *c;
	Win *w, *ow;
	int i;

	c = emalloc(sizeof *c);
	*c = *a;
	c->win = emalloc((a->nwin+1)*sizeof(Win*));
	for(i = 0; i < a->nwin; i++){
		ow = a->win[i];
		w = emalloc(sizeof *w);
		*w = *ow;
		w->name = estrdup(ow->name);
		w->title = estrdup(ow->title);
		w->pos = estrdup(ow->pos);
		w->kind = estrdup(ow->kind);
		w->titlebar = estrdup(ow->titlebar);
		w->state = estrdup(ow->state);
		w->root = nodecopy(ow->root, nil, w);
		w->over = nodecopy(ow->over, w->root, w);
		c->win[i] = w;
	}
	for(i = 0; i < Ntok; i++)
		c->tok[i] = estrdup(a->tok[i]);
	c->pref = estrdup(a->pref);
	c->themename = estrdup(a->themename);
	c->keys = estrdup(a->keys);
	c->bar = estrdup(a->bar);
	c->menu = emalloc((a->nmenu+1)*sizeof(Menu));
	for(i = 0; i < a->nmenu; i++){
		c->menu[i].name = estrdup(a->menu[i].name);
		c->menu[i].text = estrdup(a->menu[i].text);
	}
	return c;
}

void
appfree(App *a)
{
	int i;

	for(i = 0; i < a->nwin; i++)
		freewin(a->win[i]);
	free(a->win);
	for(i = 0; i < Ntok; i++)
		free(a->tok[i]);
	free(a->pref);
	free(a->themename);
	free(a->keys);
	free(a->bar);
	for(i = 0; i < a->nmenu; i++){
		free(a->menu[i].name);
		free(a->menu[i].text);
	}
	free(a->menu);
	free(a);
}

char*
effmode(void)
{
	if(strcmp(app->pref, "system") == 0)
		return mode;
	return app->pref;
}

void
initapp(void)
{
	int i;
	char *s;

	for(i = 0; catalog[i] != nil; i += 2){
		types = erealloc(types, (ntypes+1)*sizeof(Type));
		memset(&types[ntypes], 0, sizeof(Type));
		types[ntypes].name = catalog[i];
		types[ntypes].spec = catalog[i+1];
		parsespec(&types[ntypes]);
		ntypes++;
	}
	app = emalloc(sizeof *app);
	memset(app, 0, sizeof *app);
	for(i = 0; i < Ntok; i++)
		app->tok[i] = estrdup(tokdefault[i]);
	app->pref = estrdup("system");
	app->themename = estrdup("default");
	app->keys = estrdup("");
	app->bar = estrdup("");
	app->menu = emalloc(sizeof(Menu));
	newwin("main");
	s = getenv("uxkitmode");
	if(s != nil && (strcmp(s, "dark") == 0 || strcmp(s, "light") == 0))
		mode = s;
}

/*
 * Documents.
 */

/* a value as one token: quoted if it must be */
char*
quoteval(char *v)
{
	char *s, *p;
	int n;

	if(v[0] != 0 && strpbrk(v, " \t'") == nil && strncmp(v, "<<", 2) != 0
	&& strcmp(v, ":") != 0 && v[0] != '#')
		return estrdup(v);
	n = 3;
	for(p = v; *p; p++)
		n += *p == '\'' ? 2 : 1;
	s = emalloc(n);
	p = s;
	*p++ = '\'';
	for(; *v; v++){
		if(*v == '\'')
			*p++ = '\'';
		*p++ = *v;
	}
	*p++ = '\'';
	*p = 0;
	return s;
}

static int
isdefault(Node *n, Prop *p)
{
	Pdesc *d;

	if(p->val[0] == 0)
		return 1;
	d = pdesc(typebyname(n->type), p->name);
	if(d == nil)
		return 0;
	if(strcmp(d->form, "bool") == 0 && strcmp(p->val, "off") == 0)
		return 1;
	if(d->dflt != nil && strcmp(d->dflt, p->val) == 0)
		return 1;
	return 0;
}

static int
hasline(char *body, char *w)
{
	char *p;
	int l;

	l = strlen(w);
	for(p = body; p != nil; p = strchr(p, '\n')){
		if(*p == '\n')
			p++;
		if(strncmp(p, w, l) == 0 && (p[l] == '\n' || p[l] == 0))
			return 1;
	}
	return 0;
}

static void
printnode(Fmt *f, Node *n, int depth)
{
	int i, j, k, nhere;
	char *q, word[16], **body, **bw;
	Node *c;

	for(i = 0; i < depth; i++)
		fmtprint(f, "  ");
	fmtprint(f, "%s %s", n->type, n->parent == nil ? "root" : n->name);
	for(i = 0; i < n->nprop; i++)
		if(n->prop[i].bare)
			fmtprint(f, "  %s", n->prop[i].name[0] == '@' ? n->prop[i].val : n->prop[i].name);
	body = nil;
	bw = nil;
	nhere = 0;
	for(i = 0; i < n->nprop; i++){
		if(n->prop[i].bare || isdefault(n, &n->prop[i]))
			continue;
		if(strcmp(n->prop[i].name, "value") == 0 && issecret(n))
			continue;
		if(strchr(n->prop[i].val, '\n') != nil){
			for(k = nhere+1; ; k++){
				if(k == 1)
					strcpy(word, "END");
				else
					snprint(word, sizeof word, "END%d", k);
				if(!hasline(n->prop[i].val, word))
					break;
			}
			fmtprint(f, "  %s=<<%s", n->prop[i].name, word);
			body = erealloc(body, (nhere+1)*sizeof(char*));
			bw = erealloc(bw, (nhere+1)*sizeof(char*));
			body[nhere] = n->prop[i].val;
			bw[nhere] = estrdup(word);
			nhere++;
			continue;
		}
		q = quoteval(n->prop[i].val);
		fmtprint(f, "  %s=%s", n->prop[i].name, q);
		free(q);
	}
	if(n->style[0] != 0)
		fmtprint(f, "  : %s", n->style);
	fmtprint(f, "\n");
	for(i = 0; i < nhere; i++){
		fmtprint(f, "%s\n%s\n", body[i], bw[i]);
		free(bw[i]);
	}
	free(body);
	free(bw);
	for(i = 0; i < n->nord; i++)
		for(j = 0; j < n->nkid; j++)
			if(n->kid[j]->id == n->ord[i])
				printnode(f, n->kid[j], depth+1);
	for(j = 0; j < n->nkid; j++){
		c = n->kid[j];
		for(i = 0; i < n->nord; i++)
			if(n->ord[i] == c->id)
				break;
		if(i == n->nord)
			printnode(f, c, depth+1);
	}
}

char*
printtree(Node *n)
{
	Fmt f;

	fmtstrinit(&f);
	printnode(&f, n, 0);
	return fmtstrflush(&f);
}

int
issecret(Node *n)
{
	char *s;

	s = getprop(n, "secret");
	return s != nil && strcmp(s, "on") == 0;
}

/* value is always a file; other props when the descriptor says so */
int
isfileprop(Pdesc *p)
{
	return p->file || strcmp(p->name, "value") == 0;
}

/*
 * The trace: what applied and what failed, recent history first.
 */
enum {
	Ntrace = 2000,
};
static char	*tr[Ntrace];
static int	ntr;

void
trace(char *fmt, ...)
{
	va_list arg;
	char *s;

	va_start(arg, fmt);
	s = vsmprint(fmt, arg);
	va_end(arg);
	if(ntr == Ntrace){
		free(tr[0]);
		memmove(tr, tr+1, (Ntrace-1)*sizeof(char*));
		ntr--;
	}
	tr[ntr++] = s;
}

char*
tracetext(void)
{
	Fmt f;
	int i;

	fmtstrinit(&f);
	for(i = 0; i < ntr; i++)
		fmtprint(&f, "%s\n", tr[i]);
	return fmtstrflush(&f);
}
