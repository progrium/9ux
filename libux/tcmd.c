/*
 * The ctl grammar: tokens, here-values, the core verbs, window verbs,
 * app verbs, requests, batches, documents, theme, keys, menus, and
 * /debug/inject.
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include "tree.h"

typedef struct Request Request;
struct Request {
	int	seq;
	int	kind;
	char	*win;
	int	due;
	Request	*next;
};

static Request	*requests;
static App	*batch;
static int	batchstart;

/*
 * Lexing: Plan 9 tokenize, plus here-values (prop=<<WORD) and indents.
 */
static int
addtok(Line *l, char *s, int quoted, int here)
{
	l->t = erealloc(l->t, (l->n+1)*sizeof(Tok));
	l->t[l->n].s = s;
	l->t[l->n].quoted = quoted;
	l->t[l->n].here = here;
	return l->n++;
}

void
freelines(Line *l, int n)
{
	int i, j;

	for(i = 0; i < n; i++){
		for(j = 0; j < l[i].n; j++)
			free(l[i].t[j].s);
		free(l[i].t);
		free(l[i].raw);
	}
	free(l);
}

/* split text into lines of tokens; returns the count, or -1 and *err */
int
lex(char *text, Line **lines, char **err)
{
	char **phys, *p, *e, *buf, *s, *word, *pre, *body;
	int nphys, i, j, k, n, q, have, quoted, nl, hi, len;
	Line *L, *cur;
	Fmt f;

	*lines = nil;
	*err = nil;
	buf = estrdup(text);
	phys = nil;
	nphys = 0;
	for(p = buf; ; p = e+1){
		e = strchr(p, '\n');
		if(e != nil)
			*e = 0;
		if(e != nil || *p != 0){
			phys = erealloc(phys, (nphys+1)*sizeof(char*));
			phys[nphys++] = p;
		}
		if(e == nil)
			break;
	}
	L = nil;
	nl = 0;
	for(i = 0; i < nphys; i++){
		p = phys[i];
		k = 0;
		while(p[k] == ' ' || p[k] == '\t')
			k++;
		if(p[k] == 0 || p[k] == '#')
			continue;
		L = erealloc(L, (nl+1)*sizeof(Line));
		cur = &L[nl++];
		memset(cur, 0, sizeof *cur);
		cur->indent = k;
		p += k;
		cur->raw = estrdup(p);
		hi = i;	/* the last physical line used */
		while(*p){
			while(*p == ' ' || *p == '\t')
				p++;
			if(*p == 0)
				break;
			s = emalloc(strlen(p)+1);
			n = 0;
			q = 0;
			quoted = 0;
			have = 0;
			for(; *p; p++){
				if(q){
					if(*p == '\''){
						if(p[1] == '\''){
							s[n++] = '\'';
							p++;
						}else
							q = 0;
					}else
						s[n++] = *p;
					continue;
				}
				if(*p == ' ' || *p == '\t')
					break;
				if(*p == '\''){
					q = 1;
					quoted = 1;
					have = 1;
					continue;
				}
				s[n++] = *p;
				have = 1;
			}
			s[n] = 0;
			if(q){
				free(s);
				*err = estrdup("bad syntax: unterminated quote");
				goto bad;
			}
			USED(have);
			/* a here-value: <<WORD or prop=<<WORD */
			if(!quoted && (word = strstr(s, "<<")) != nil
			&& (word == s || (word[-1] == '=' && strchr(s, '=') == word-1)) && word[2] != 0){
				pre = emalloc(word-s+1);
				memmove(pre, s, word-s);
				pre[word-s] = 0;
				word += 2;
				fmtstrinit(&f);
				for(j = hi+1; j < nphys; j++){
					if(strcmp(phys[j], word) == 0)
						break;
					if(j > hi+1)
						fmtprint(&f, "\n");
					fmtprint(&f, "%s", phys[j]);
				}
				body = fmtstrflush(&f);
				if(j >= nphys){
					*err = smprint("bad syntax: no %s line", word);
					free(body);
					free(pre);
					free(s);
					goto bad;
				}
				hi = j;
				len = strlen(pre)+strlen(body)+1;
				free(s);
				s = emalloc(len);
				snprint(s, len, "%s%s", pre, body);
				free(pre);
				free(body);
				addtok(cur, s, 0, 1);
				continue;
			}
			addtok(cur, s, quoted, 0);
		}
		i = hi;
	}
	free(phys);
	free(buf);
	*lines = L;
	return nl;
bad:
	free(phys);
	free(buf);
	freelines(L, nl);
	return -1;
}

/*
 * Paths.
 */
static int
countnamed(Node *n, char *name, Node **found)
{
	int i, c;

	c = 0;
	for(i = 0; i < n->nkid; i++){
		if(strcmp(n->kid[i]->name, name) == 0){
			*found = n->kid[i];
			c++;
		}
		c += countnamed(n->kid[i], name, found);
	}
	return c;
}

char*
resolve(Node *base, char *path, Node **np)
{
	char *buf, *f[128];
	int i, n;
	Node *x, *found;

	*np = nil;
	buf = estrdup(path);
	n = getfields(buf, f, nelem(f), 1, "/");
	x = base;
	for(i = 0; i < n; i++){
		if(f[i][0] == 0 || strcmp(f[i], ".") == 0)
			continue;
		if(strcmp(f[i], "..") == 0){
			if(x->parent != nil)
				x = x->parent;
			continue;
		}
		if(f[i][0] == '@'){
			found = nil;
			switch(countnamed(x, f[i]+1, &found)){
			case 0:
				free(buf);
				return smprint("not found: %s", path);
			case 1:
				x = found;
				continue;
			default:
				path = smprint("in use: ambiguous %s", f[i]);
				free(buf);
				return path;
			}
		}
		if((x = walkkid(x, f[i])) == nil){
			free(buf);
			return smprint("not found: %s", path);
		}
	}
	free(buf);
	*np = x;
	return nil;
}

/* a path on the app ctl: win/NAME/... */
static char*
resolveapp(char *path, Node **np)
{
	char *s, *p, *e;
	Win *w;

	*np = nil;
	if(strncmp(path, "win/", 4) != 0)
		return smprint("not found: %s", path);
	s = estrdup(path+4);
	p = strchr(s, '/');
	if(p != nil)
		*p++ = 0;
	w = winbyname(s);
	if(w == nil){
		free(s);
		return smprint("not found: %s", path);
	}
	e = resolve(w->root, p != nil ? p : ".", np);
	free(s);
	return e;
}

static char*
resolvein(Node *base, char *path, Node **np)
{
	if(base == nil)
		return resolveapp(path, np);
	return resolve(base, path, np);
}

static Win*
winof(Node *n)
{
	while(n->parent != nil)
		n = n->parent;
	return n->win;
}

/*
 * Windows.
 */
static int
pair(char *v, int *a, int *b)
{
	char *e;

	*a = strtol(v, &e, 10);
	if(e == v || *e != ',')
		return -1;
	v = e+1;
	*b = strtol(v, &e, 10);
	if(e == v || *e != 0)
		return -1;
	return 0;
}

static char *winprops[] = {
	"title", "size", "min", "max", "pos", "kind", "titlebar", "resizable", "focus", nil,
};

static int
iswinprop(char *s)
{
	int i;

	for(i = 0; winprops[i] != nil; i++)
		if(strcmp(winprops[i], s) == 0)
			return 1;
	return 0;
}

static int
oneof(char *v, char *a, char *b, char *c, char *d)
{
	return (a != nil && strcmp(v, a) == 0) || (b != nil && strcmp(v, b) == 0)
		|| (c != nil && strcmp(v, c) == 0) || (d != nil && strcmp(v, d) == 0);
}

char*
setwinprop(Win *w, char *name, char *v)
{
	int a, b;

	if(strcmp(name, "title") == 0){
		free(w->title);
		w->title = estrdup(v);
	}else if(strcmp(name, "size") == 0 || strcmp(name, "min") == 0 || strcmp(name, "max") == 0){
		if(pair(v, &a, &b) < 0 || a < 0 || b < 0)
			return smprint("bad value: %s=%s", name, v);
		if(name[0] == 's'){
			w->w = a;
			w->h = b;
		}else if(name[1] == 'i'){
			w->minw = a;
			w->minh = b;
		}else{
			w->maxw = a;
			w->maxh = b;
		}
	}else if(strcmp(name, "pos") == 0){
		if(pair(v, &a, &b) < 0 && !oneof(v, "center", "default", "cascade", "mouse"))
			return smprint("bad value: pos=%s", v);
		free(w->pos);
		w->pos = estrdup(v);
	}else if(strcmp(name, "kind") == 0){
		if(!oneof(v, "normal", "dialog", "panel", "popup"))
			return smprint("bad value: kind=%s", v);
		free(w->kind);
		w->kind = estrdup(v);
	}else if(strcmp(name, "titlebar") == 0){
		if(!oneof(v, "system", "custom", "none", nil))
			return smprint("bad value: titlebar=%s", v);
		free(w->titlebar);
		w->titlebar = estrdup(v);
	}else if(strcmp(name, "resizable") == 0){
		if(!oneof(v, "on", "off", nil, nil))
			return smprint("bad value: resizable=%s", v);
		w->resizable = strcmp(v, "on") == 0;
	}else if(strcmp(name, "focus") == 0)
		return setfocus(w, v, 1);
	else
		return smprint("not found: window prop %s", name);
	return nil;
}

char*
setfocus(Win *w, char *path, int ev)
{
	Node *n;
	char *e;

	if(path[0] == 0){
		w->focus = 0;
		return nil;
	}
	if((e = resolve(w->root, path, &n)) != nil)
		return e;
	if(w->focus == n->id)
		return nil;
	w->focus = n->id;
	if(ev)
		emitnode("focus", w, n, "via=ctl", 1);
	return nil;
}

static void
dropreqs(char *win)
{
	Request **l, *r;

	for(l = &requests; (r = *l) != nil; ){
		if(r->win != nil && win != nil && strcmp(r->win, win) == 0){
			*l = r->next;
			free(r->win);
			free(r);
			continue;
		}
		l = &r->next;
	}
}

void
closewin(Win *w, int)
{
	dropreqs(w->name);
	delwin(w);
}

void
doquit(void)
{
	quitting = 1;
	servesubs();
}

/* libux's C API: a quit or close asked for goes to the app's handler (tapi.c) */
void	(*quithook)(char*);
void	(*closehook)(Win*, char*);
void	(*activatehook)(Win*);

/* a close or quit the user or client asked for: a request if the app asked */
static void
request(int kind, Win *w, char *via)
{
	int asked, s;
	char *attrs;
	Request *r;

	if(kind == Aquit && quithook != nil){
		quithook(via);
		return;
	}
	if(kind == Aclose && w->capi && closehook != nil){
		closehook(w, via);
		return;
	}
	if(kind == Aclose)
		asked = w->ask == 1 || (w->ask == -1 && (app->ask & Aclose));
	else
		asked = (app->ask & Aquit) != 0;
	if(!asked || !anysub()){
		if(kind == Aclose)
			closewin(w, 0);
		else
			doquit();
		return;
	}
	attrs = smprint("via=%s", via);
	if(kind == Aclose)
		s = emitnode("close", w, w->root, attrs, 0);
	else
		s = emit("quit", "-", "-", attrs, nil, nil, 0, 0);
	free(attrs);
	r = emalloc(sizeof *r);
	r->seq = s;
	r->kind = kind;
	r->win = w != nil ? estrdup(w->name) : nil;
	r->due = now + 5;
	r->next = requests;
	requests = r;
}

void
closerequest(Win *w, char *via)
{
	request(Aclose, w, via);
}

void
quitrequest(char *via)
{
	request(Aquit, nil, via);
}

/* answer seq=N: keep, or the request's own default */
static char*
answer(Win *w, int keep, int kind, char *arg)
{
	Request **l, *r;
	int s;

	if(arg == nil || strncmp(arg, "seq=", 4) != 0)
		return smprint("bad value: an answer needs seq=");
	s = atoi(arg+4);
	for(l = &requests; (r = *l) != nil; l = &r->next){
		if(r->seq != s)
			continue;
		if(!keep && r->kind != kind)
			continue;
		if(w != nil && (r->win == nil || strcmp(r->win, w->name) != 0))
			continue;
		if(w == nil && r->kind != Aquit)
			continue;
		*l = r->next;
		if(!keep){
			if(r->kind == Aclose && (w = winbyname(r->win)) != nil)
				closewin(w, 0);
			else if(r->kind == Aquit)
				doquit();
		}
		free(r->win);
		free(r);
		return nil;
	}
	return smprint("not found: no request seq=%d", s);
}

void
tick(int secs)
{
	Request **l, *r;
	Win *w;

	now += secs;
	if(batch != nil && now >= batchstart + 2){
		appfree(batch);
		batch = nil;
		trace("batch timeout");
	}
	for(l = &requests; (r = *l) != nil; ){
		if(r->due > now){
			l = &r->next;
			continue;
		}
		*l = r->next;
		if(r->kind == Aclose && (w = winbyname(r->win)) != nil)
			closewin(w, 0);
		else if(r->kind == Aquit)
			doquit();
		free(r->win);
		free(r);
	}
}

/*
 * Props, bare words and style on a node, from a command's tokens.
 */
static int	creating;
static void	echovalue(Node*, Pdesc*, char*);
static int	hasitem(char*, char*);
static char*	checkkids(Node*, char*);

char*
setnodeprop(Node *n, char *name, char *v)
{
	Pdesc *p;
	Win *w;
	char *e;

	w = n->parent == nil ? n->win : nil;
	if(w != nil && iswinprop(name))
		return setwinprop(w, name, v);
	if(strcmp(name, "style") == 0){
		if((e = checkstyle(v)) != nil)
			return e;
		free(n->style);
		n->style = estrdup(v);
		return nil;
	}
	if(strcmp(name, "tag") == 0 || strcmp(name, "context") == 0){
		setprop(n, name, v, 0);
		return nil;
	}
	if(strcmp(name, "on") == 0){
		if((e = checkon(v)) != nil)
			return e;
		setprop(n, name, v, 0);
		return nil;
	}
	p = pdesc(typebyname(n->type), name);
	if(p == nil)
		return smprint("not found: %s has no prop %s", n->type, name);
	if((e = checkvalue(n, p, v)) != nil)
		return e;
	if(strcmp(name, "value") == 0 && pdesc(typebyname(n->type), "items") != nil
	&& strcmp(p->form, "word") == 0 && getprop(n, "items") != nil && getprop(n, "items")[0] != 0
	&& !hasitem(getprop(n, "items"), v))
		return smprint("not found: no item %s", v);
	if(strcmp(name, "value") == 0 && strcmp(p->form, "text") == 0
	&& (e = checkkids(n, v)) != nil)
		return e;
	if(strcmp(p->form, "bool") == 0)
		setprop(n, name, v, strcmp(v, "on") == 0);
	else
		setprop(n, name, v, 0);
	if(strcmp(name, "value") == 0 && !creating)
		echovalue(n, p, v);
	return nil;
}

/*
 * A text value on a type whose children are listed (accordion's open
 * items) names those children, each one.
 */
static char*
checkkids(Node *n, char *v)
{
	Type *t;
	char *s, *f[64];
	int i, nf;
	char *e;

	t = typebyname(n->type);
	if(t == nil || t->child != 2 || n->nkid == 0)
		return nil;
	s = estrdup(v);
	nf = tokenize(s, f, nelem(f));
	e = nil;
	for(i = 0; i < nf; i++)
		if(kidbyname(n, f[i]) == nil){
			e = smprint("not found: no child %s", f[i]);
			break;
		}
	free(s);
	return e;
}

/* a client's write to a value is echoed as a change with via=ctl */
static void
echovalue(Node *n, Pdesc *p, char *v)
{
	char *a, *q;
	Node *r;

	for(r = n; r->parent != nil; r = r->parent)
		;
	if(strcmp(p->form, "text") == 0 || strcmp(p->form, "bytes") == 0 || strcmp(p->form, "data") == 0
	|| issecret(n))
		a = estrdup("via=ctl");
	else{
		q = quoteval(v);
		a = smprint("value=%s via=ctl", q);
		free(q);
	}
	emitnode("change", r->win, n, a, 1);
	free(a);
}

/* is id the first field of a line of items? */
static int
hasitem(char *items, char *id)
{
	char *buf, *line, *nl, *f[4], *tab;
	int found;

	if(items == nil)
		return 0;
	found = 0;
	buf = estrdup(items);
	for(line = buf; line != nil && *line && !found; line = nl){
		nl = strchr(line, '\n');
		if(nl != nil)
			*nl++ = 0;
		if((tab = strchr(line, '\t')) != nil){
			*tab = 0;
			found = strcmp(line, id) == 0;
		}else if(tokenize(line, f, nelem(f)) > 0)
			found = strcmp(f[0], id) == 0;
	}
	free(buf);
	return found;
}

static char*
setbare(Node *n, char *w)
{
	char *k, *s;

	k = nodeword(n, w);
	if(k == nil)
		return smprint("bad value: %s takes no word %s", n->type, w);
	if(strcmp(k, "flag") == 0)
		setprop(n, w, "on", 1);
	else{
		s = smprint("@%s", k);
		setprop(n, s, w, 1);
		free(s);
	}
	return nil;
}

static char*
applyargs(Node *n, Tok *t, int nt)
{
	int i, st;
	char *e, *v, *name;
	Fmt f;

	st = 0;
	fmtstrinit(&f);
	for(i = 0; i < nt; i++){
		if(st){
			fmtprint(&f, "%s%s", st > 1 ? " " : "", t[i].s);
			st++;
			continue;
		}
		if(!t[i].quoted && strcmp(t[i].s, ":") == 0){
			st = 1;
			continue;
		}
		if((v = strchr(t[i].s, '=')) != nil){
			name = emalloc(v-t[i].s+1);
			memmove(name, t[i].s, v-t[i].s);
			name[v-t[i].s] = 0;
			e = setnodeprop(n, name, v+1);
			free(name);
		}else
			e = setbare(n, t[i].s);
		if(e != nil){
			free(fmtstrflush(&f));
			return e;
		}
	}
	v = fmtstrflush(&f);
	if(st){
		if((e = checkstyle(v)) != nil){
			free(v);
			return e;
		}
		free(n->style);
		n->style = v;
	}else
		free(v);
	return nil;
}

static char*
checkname(Node *parent, char *s)
{
	USED(parent);
	if(s[0] == 0 || strcmp(s, ".") == 0 || strcmp(s, "..") == 0 || s[0] == '@'
	|| strchr(s, '/') != nil || strpbrk(s, " \t\n") != nil)
		return smprint("bad value: name %s", s);
	if(isnodefile(s, 0))
		return smprint("in use: %s is a node file", s);
	return nil;
}

/* new TYPE PATH args */
static char*
cmdnew(Node *base, Tok *t, int nt)
{
	char *path, *slash, *e, *name;
	Node *parent, *n;
	Pdesc *p;

	if(nt < 2)
		return smprint("bad syntax: new TYPE PATH");
	if(typebyname(t[0].s) == nil)
		return smprint("unknown type: %s", t[0].s);
	path = estrdup(t[1].s);
	slash = strrchr(path, '/');
	if(slash != nil){
		*slash = 0;
		name = slash+1;
		e = resolvein(base, path, &parent);
	}else{
		name = path;
		if(base == nil)
			e = smprint("not found: %s", path);
		else{
			e = nil;
			parent = base;
		}
	}
	if(e == nil)
		e = checkname(parent, name);
	if(e == nil && kidbyname(parent, name) != nil)
		e = smprint("in use: %s", t[1].s);
	if(e == nil){
		p = pdesc(typebyname(parent->type), name);
		USED(p);
		e = canhold(parent, t[0].s, name);
	}
	if(e != nil){
		free(path);
		return e;
	}
	n = newnode(parent, name, t[0].s);
	free(path);
	creating++;
	e = applyargs(n, t+2, nt-2);
	creating--;
	return e;
}

/* the action IDs that keys and menus name */
static int
knownaction(char *id)
{
	char *buf, *line, *nl, *f[8];
	int i, n, found;

	found = 0;
	buf = estrdup(app->keys);
	for(line = buf; line != nil && *line && !found; line = nl){
		nl = strchr(line, '\n');
		if(nl != nil)
			*nl++ = 0;
		n = tokenize(line, f, nelem(f));
		if(n >= 2 && strcmp(f[1], id) == 0)
			found = 1;
	}
	free(buf);
	for(i = 0; i < app->nmenu && !found; i++){
		buf = estrdup(app->menu[i].text);
		for(line = buf; line != nil && *line && !found; line = nl){
			nl = strchr(line, '\n');
			if(nl != nil)
				*nl++ = 0;
			n = tokenize(line, f, nelem(f));
			if(n >= 2 && (strcmp(f[0], "item") == 0 || strcmp(f[0], "sub") == 0) && strcmp(f[1], id) == 0)
				found = 1;
		}
		free(buf);
	}
	return found;
}

static char*
setaction(Tok *t, int nt)
{
	int i;
	char *v;

	if(!knownaction(t[0].s))
		return smprint("not found: action %s", t[0].s);
	for(i = 1; i < nt; i++){
		v = strchr(t[i].s, '=');
		if(v == nil)
			return smprint("bad value: %s", t[i].s);
		v++;
		if(strncmp(t[i].s, "enabled=", 8) == 0){
			if(!oneof(v, "on", "off", nil, nil))
				return smprint("bad value: %s", t[i].s);
		}else if(strncmp(t[i].s, "check=", 6) == 0){
			if(!oneof(v, "on", "off", "mixed", nil))
				return smprint("bad value: %s", t[i].s);
		}else
			return smprint("not found: %s", t[i].s);
	}
	return nil;
}

/*
 * Documents: replace a node's subtree, keeping children whose name and
 * type match.
 */
static void
clearnode(Node *n)
{
	int i;

	/* value is the node's state, and a rewrite keeps it */
	for(i = 0; i < n->nprop; ){
		if(strcmp(n->prop[i].name, "value") == 0)
			i++;
		else
			rmprop(n, n->prop[i].name);
	}
	free(n->style);
	n->style = estrdup("");
}

static char*
docnode(Node *n, Line *L, int nl, int *i)
{
	Line *me, *c;
	int base, *keep, nkeep, j, k;
	Node *x;
	char *e;

	me = &L[*i];
	base = me->indent;
	(*i)++;
	if(strcmp(n->type, me->t[0].s) != 0 && (e = settype(n, me->t[0].s)) != nil)
		return e;
	clearnode(n);
	creating++;
	e = applyargs(n, me->t+2, me->n-2);
	creating--;
	if(e != nil)
		return e;
	keep = nil;
	nkeep = 0;
	while(*i < nl && L[*i].indent > base){
		c = &L[*i];
		if(c->indent != base+2){
			free(keep);
			return smprint("bad syntax: a child is indented two more than its parent");
		}
		if(c->n < 2){
			free(keep);
			return smprint("bad syntax: a document line is TYPE NAME");
		}
		if(typebyname(c->t[0].s) == nil){
			free(keep);
			return smprint("unknown type: %s", c->t[0].s);
		}
		x = kidbyname(n, c->t[1].s);
		if(x != nil && strcmp(x->type, c->t[0].s) != 0){
			delnode(x);
			x = nil;
		}
		if(x == nil){
			if((e = checkname(n, c->t[1].s)) != nil || (e = canhold(n, c->t[0].s, c->t[1].s)) != nil){
				free(keep);
				return e;
			}
			x = newnode(n, c->t[1].s, c->t[0].s);
		}
		for(k = 0; k < nkeep; k++)
			if(keep[k] == x->id){
				free(keep);
				return smprint("in use: %s", x->name);
			}
		keep = erealloc(keep, (nkeep+1)*sizeof(int));
		keep[nkeep++] = x->id;
		if((e = docnode(x, L, nl, i)) != nil){
			free(keep);
			return e;
		}
	}
	for(j = 0; j < n->nkid; ){
		for(k = 0; k < nkeep; k++)
			if(n->kid[j]->id == keep[k])
				break;
		if(k == nkeep)
			delnode(n->kid[j]);
		else
			j++;
	}
	free(n->ord);
	n->ord = keep != nil ? keep : emalloc(sizeof(int));
	n->nord = nkeep;
	return nil;
}

static char*
applydoc(Node *n, char *text)
{
	Line *L;
	int nl, i;
	char *e;

	nl = lex(text, &L, &e);
	if(nl < 0)
		return e;
	if(nl == 0){
		freelines(L, nl);
		return smprint("bad syntax: an empty document");
	}
	if(L[0].n < 2){
		freelines(L, nl);
		return smprint("bad syntax: a document line is TYPE NAME");
	}
	i = 0;
	e = docnode(n, L, nl, &i);
	if(e == nil && i < nl)
		e = smprint("bad syntax: more than one top node");
	freelines(L, nl);
	return e;
}

/* a tree file write: all or nothing */
char*
settree(Node *n, char *text)
{
	App *snap;
	char *e;
	int id;

	id = n->id;
	snap = appcopy(app);
	hold();
	e = applydoc(n, text);
	if(e != nil){
		appfree(app);
		app = snap;
		release(0);
		return e;
	}
	USED(id);
	appfree(snap);
	release(1);
	return nil;
}

/*
 * Commands.
 */
static char*
winverb(Win *w, Tok *t, int nt)
{
	char *v, *e;

	v = t[0].s;
	if(strcmp(v, "show") == 0){
		w->shown = 1;
		return nil;
	}
	if(strcmp(v, "activate") == 0){
		w->shown = 1;
		if(w->capi && activatehook != nil)
			activatehook(w);
		return nil;
	}
	if(strcmp(v, "state") == 0){
		if(nt < 2 || !(oneof(t[1].s, "normal", "min", "max", "full") || strcmp(t[1].s, "hidden") == 0))
			return smprint("bad value: state %s", nt > 1 ? t[1].s : "");
		free(w->state);
		w->state = estrdup(t[1].s);
		if(strcmp(t[1].s, "hidden") != 0)
			w->shown = 1;
		v = smprint("value=%s via=ctl", t[1].s);
		emitnode("state", w, w->root, v, 1);
		free(v);
		return nil;
	}
	if(strcmp(v, "notify") == 0){
		if(nt < 2)
			return smprint("bad value: notify TEXT");
		if(nt > 2){
			if(nt > 3 || strncmp(t[2].s, "type=", 5) != 0)
				return smprint("bad value: %s", t[2].s);
			if(!oneof(t[2].s+5, "info", "success", "warning", "error"))
				return smprint("bad value: %s", t[2].s);
		}
		return nil;
	}
	if(strcmp(v, "close") == 0){
		if(nt > 1)
			return answer(w, 0, Aclose, t[1].s);
		request(Aclose, w, "ctl");
		return nil;
	}
	if(strcmp(v, "keep") == 0)
		return answer(w, 1, Aclose, nt > 1 ? t[1].s : nil);
	if(strcmp(v, "destroy") == 0){
		closewin(w, 1);
		return nil;
	}
	if(strcmp(v, "ask") == 0){
		if(nt != 2 || !oneof(t[1].s, "close", "none", nil, nil))
			return smprint("bad value: ask %s", nt > 1 ? t[1].s : "");
		w->ask = strcmp(t[1].s, "close") == 0;
		return nil;
	}
	e = smprint("unknown command: %s", v);
	return e;
}

static char*
appverb(Tok *t, int nt)
{
	char *v, *e, *eq, *name, *buf;
	int i, m, fd;
	Win *w;

	v = t[0].s;
	if(strcmp(v, "window") == 0){
		if(nt < 2)
			return smprint("bad syntax: window NAME");
		if(strchr(t[1].s, '/') != nil || t[1].s[0] == 0 || t[1].s[0] == '.')
			return smprint("bad value: window name %s", t[1].s);
		w = winbyname(t[1].s);
		if(w != nil){
			if(strcmp(w->name, "main") != 0 || w->claimed)
				return smprint("in use: window %s", t[1].s);
		}else
			w = newwin(t[1].s);
		w->claimed = 1;
		for(i = 2; i < nt; i++){
			eq = strchr(t[i].s, '=');
			if(eq == nil)
				return smprint("bad value: %s", t[i].s);
			name = emalloc(eq-t[i].s+1);
			memmove(name, t[i].s, eq-t[i].s);
			name[eq-t[i].s] = 0;
			e = setwinprop(w, name, eq+1);
			free(name);
			if(e != nil)
				return e;
		}
		return nil;
	}
	if(strcmp(v, "quit") == 0){
		if(nt > 1)
			return answer(nil, 0, Aquit, t[1].s);
		request(Aquit, nil, "ctl");
		return nil;
	}
	if(strcmp(v, "keep") == 0)
		return answer(nil, 1, Aquit, nt > 1 ? t[1].s : nil);
	if(strcmp(v, "ask") == 0){
		m = 0;
		for(i = 1; i < nt; i++){
			if(strcmp(t[i].s, "close") == 0)
				m |= Aclose;
			else if(strcmp(t[i].s, "quit") == 0)
				m |= Aquit;
			else if(strcmp(t[i].s, "open") == 0)
				m |= Aopen;
			else if(strcmp(t[i].s, "none") != 0)
				return smprint("bad value: ask %s", t[i].s);
		}
		app->ask = m;
		return nil;
	}
	if(strcmp(v, "begin") == 0){
		if(batch != nil)
			return smprint("in use: a batch is open");
		batch = appcopy(app);
		batchstart = now;
		return nil;
	}
	if(strcmp(v, "commit") == 0 || strcmp(v, "abort") == 0){
		if(batch == nil)
			return smprint("no batch");
		if(v[0] == 'a'){
			appfree(app);
			app = batch;
		}else
			appfree(batch);
		batch = nil;
		return nil;
	}
	if(strcmp(v, "theme") == 0){
		if(nt == 3 && strcmp(t[1].s, "load") == 0){
			if((fd = open(t[2].s, OREAD)) < 0)
				return smprint("not found: %s", t[2].s);
			buf = emalloc(65536);
			m = readn(fd, buf, 65535);
			close(fd);
			buf[m < 0 ? 0 : m] = 0;
			e = settokens(buf);
			free(buf);
			return e;
		}
		if(nt == 2){
			if(strcmp(t[1].s, "default") != 0)
				return smprint("not found: theme %s", t[1].s);
			return nil;
		}
		return smprint("bad syntax: theme load FILE");
	}
	if(strcmp(v, "set") == 0 && nt >= 2 && strncmp(t[1].s, "win/", 4) != 0)
		return setaction(t+1, nt-1);
	return smprint("unknown command: %s", v);
}

/* libux's C API does the verbs of the controls it draws (tapi.c) */
char*	(*verbhook)(Node*, Tok*, int);

/* a verb the node's descriptor lists; (char*)-1 if it lists none by that name */
static char*
typeverb(Node *n, Tok *t, int nt)
{
	Type *ty;
	char *d;
	Node *r;
	int i;

	USED(nt);
	ty = typebyname(n->type);
	if(ty == nil)
		return (char*)-1;
	for(i = 0; i < ty->nverb; i++){
		if(strcmp(ty->verb[i], t[0].s) != 0)
			continue;
		d = getprop(n, "disabled");
		if(d != nil && strcmp(d, "on") == 0)
			return smprint("disabled: %s", n->name);
		if(verbhook != nil && (d = verbhook(n, t, nt)) != (char*)-1)
			return d;
		/* an overlay's open and close show and hide it */
		if(ty->overlay && (strcmp(t[0].s, "open") == 0 || strcmp(t[0].s, "close") == 0)){
			setprop(n, "open", strcmp(t[0].s, "open") == 0 ? "on" : "off", strcmp(t[0].s, "open") == 0);
			uisync();
			return nil;
		}
		if(strcmp(ty->vclass[i], "activate") == 0){
			for(r = n; r->parent != nil; r = r->parent)
				;
			emitnode(t[0].s, r->win, n, "via=ctl", 0);
		}
		return nil;
	}
	return (char*)-1;
}

static char*
cmd(Node *base, Line *l)
{
	Tok *t;
	int nt;
	Node *n;
	char *e, *v;

	t = l->t;
	nt = l->n;
	v = t[0].s;
	if(base != nil && strcmp(base->type, "canvas") == 0
	&& (e = canvasverb(base, t, nt)) != (char*)-1)
		return e;
	if(strcmp(v, "set") == 0 && (base != nil || (nt >= 2 && strncmp(t[1].s, "win/", 4) == 0))){
		if(nt < 2)
			return smprint("bad syntax: set PATH");
		if((e = resolvein(base, t[1].s, &n)) != nil)
			return e;
		return applyargs(n, t+2, nt-2);
	}
	if(strcmp(v, "new") == 0)
		return cmdnew(base, t+1, nt-1);
	if(strcmp(v, "delete") == 0){
		if(nt != 2)
			return smprint("bad syntax: delete PATH");
		if((e = resolvein(base, t[1].s, &n)) != nil)
			return e;
		if(n->parent == nil || strcmp(n->type, "@over") == 0)
			return smprint("bad value: a window's root");
		delnode(n);
		return nil;
	}
	if(strcmp(v, "tree") == 0){
		if(nt != 3 || !t[2].here)
			return smprint("bad syntax: tree PATH <<END");
		if((e = resolvein(base, t[1].s, &n)) != nil)
			return e;
		return applydoc(n, t[2].s);
	}
	if(base == nil)
		return appverb(t, nt);
	if((e = typeverb(base, t, nt)) != (char*)-1)
		return e;
	if(base->parent == nil && base->win != nil)
		return winverb(base->win, t, nt);
	return smprint("unknown command: %s", v);
}

/* a line for the trace, with a secret node's value hidden */
static void
tracecmd(Node *base, Line *l, char *upath)
{
	char *path, *s, *q, *p, *e;
	Node *n;
	int secret, i;
	Fmt f;

	secret = 0;
	path = nil;
	if(l->n >= 3 && strcmp(l->t[0].s, "new") == 0)
		path = l->t[2].s;
	else if(l->n >= 2 && strcmp(l->t[0].s, "set") == 0)
		path = l->t[1].s;
	if(path != nil && resolvein(base, path, &n) == nil && issecret(n))
		secret = 1;
	for(i = 0; i < l->n; i++)
		if(!l->t[i].quoted && strcmp(l->t[i].s, "secret") == 0)
			secret = 1;
	s = l->raw;
	if(secret){
		fmtstrinit(&f);
		for(p = s; *p; ){
			if(strncmp(p, "value=", 6) == 0 && (p == s || p[-1] == ' ' || p[-1] == '\t')){
				fmtprint(&f, "value=<secret>");
				p += 6;
				if(*p == '\''){
					for(p++; *p; p++)
						if(*p == '\''){
							if(p[1] == '\'')
								p++;
							else{
								p++;
								break;
							}
						}
				}else
					while(*p && *p != ' ' && *p != '\t')
						p++;
				continue;
			}
			fmtrune(&f, *p++);
		}
		s = fmtstrflush(&f);
	}else
		s = estrdup(s);
	if((e = strchr(s, '\n')) != nil)
		*e = 0;
	q = quoteval(s);
	trace("ctl path=%s cmd=%s", upath, q);
	free(q);
	free(s);
}

/* a ctl write: every line applies, or none does */
char*
ctlrun(Node *base, char *text, char *upath)
{
	Line *L;
	int nl, i, id;
	char *e;
	App *snap;

	nl = lex(text, &L, &e);
	if(nl < 0)
		return e;
	id = base != nil ? base->id : 0;
	snap = appcopy(app);
	hold();
	e = nil;
	for(i = 0; i < nl && e == nil; i++){
		if(id != 0 && (base = nodebyid(id)) == nil){
			e = smprint("gone");
			break;
		}
		e = cmd(base, &L[i]);
	}
	if(e == nil)
		for(i = 0; i < nl; i++)
			tracecmd(id != 0 ? nodebyid(id) : nil, &L[i], upath);
	freelines(L, nl);
	if(e != nil){
		appfree(app);
		app = snap;
		release(0);
		return e;
	}
	appfree(snap);
	release(1);
	return nil;
}

/*
 * Theme, keys and menus.
 */
char*
settokens(char *text)
{
	char *buf, *line, *nl, *f[8], *e;
	char **val;
	int n, i, k;

	buf = estrdup(text);
	val = emalloc(Ntok*sizeof(char*));
	memset(val, 0, Ntok*sizeof(char*));
	e = nil;
	for(line = buf; line != nil && *line && e == nil; line = nl){
		nl = strchr(line, '\n');
		if(nl != nil)
			*nl++ = 0;
		n = tokenize(line, f, nelem(f));
		if(n == 0 || f[0][0] == '#')
			continue;
		if(n != 2){
			e = smprint("bad syntax: NAME VALUE");
			break;
		}
		for(k = 0; k < Ntok; k++)
			if(strcmp(tokname[k], f[0]) == 0)
				break;
		if(k == Ntok){
			e = smprint("not found: token %s", f[0]);
			break;
		}
		if(strcmp(f[0], "radius") == 0){
			if(strtod(f[1], &line), *line != 0)
				e = smprint("bad value: radius %s", f[1]);
		}else if(checkcolor(f[1]) != nil)
			e = smprint("bad value: %s %s", f[0], f[1]);
		if(e == nil){
			free(val[k]);
			val[k] = estrdup(f[1]);
		}
	}
	if(e == nil)
		for(i = 0; i < Ntok; i++)
			if(val[i] != nil){
				free(app->tok[i]);
				app->tok[i] = val[i];
				val[i] = nil;
			}
	for(i = 0; i < Ntok; i++)
		free(val[i]);
	free(val);
	free(buf);
	return e;
}

char*
setkeys(char *text)
{
	char *buf, *line, *nl, *f[16], *e;
	int n, i;

	buf = estrdup(text);
	e = nil;
	for(line = buf; line != nil && *line && e == nil; line = nl){
		nl = strchr(line, '\n');
		if(nl != nil)
			*nl++ = 0;
		n = tokenize(line, f, nelem(f));
		if(n == 0 || f[0][0] == '#')
			continue;
		if(n < 2){
			e = smprint("bad value: STROKE ID");
			break;
		}
		if((e = checkstroke(f[0])) != nil)
			break;
		for(i = 2; i < n; i++)
			if(strncmp(f[i], "context=", 8) != 0 && strcmp(f[i], "global") != 0){
				e = smprint("bad value: %s", f[i]);
				break;
			}
	}
	free(buf);
	if(e != nil)
		return e;
	free(app->keys);
	app->keys = estrdup(text);
	return nil;
}

static char *roles[] = {
	"undo", "redo", "cut", "copy", "paste", "paste-and-match-style", "delete",
	"select-all", "quit", "about", "preferences", "settings", "hide",
	"hide-others", "unhide", "show-all", "minimize", "zoom", "close",
	"fullscreen", "toggle-fullscreen", "front", "services", "window", "help",
	"reload", "zoom-in", "zoom-out", "reset-zoom", "new", "open", "save",
	"save-as", "print", "find", nil,
};

char*
setmenu(char *name, char *text)
{
	char *buf, *line, *nl, *f[32], *e, *v;
	int n, i, k;

	buf = estrdup(text);
	e = nil;
	for(line = buf; line != nil && *line && e == nil; line = nl){
		nl = strchr(line, '\n');
		if(nl != nil)
			*nl++ = 0;
		n = tokenize(line, f, nelem(f));
		if(n == 0 || f[0][0] == '#')
			continue;
		if(strcmp(f[0], "sep") == 0)
			continue;
		if(strcmp(f[0], "item") != 0 && strcmp(f[0], "sub") != 0){
			e = smprint("bad value: %s", f[0]);
			break;
		}
		if(n < 2){
			e = smprint("bad value: %s needs an ID", f[0]);
			break;
		}
		for(i = 2; i < n && e == nil; i++){
			v = strchr(f[i], '=');
			if(v == nil){
				e = smprint("bad value: %s", f[i]);
				break;
			}
			v++;
			if(strncmp(f[i], "role=", 5) == 0){
				for(k = 0; roles[k] != nil; k++)
					if(strcmp(roles[k], v) == 0)
						break;
				if(roles[k] == nil)
					e = smprint("bad value: role %s", v);
			}else if(strncmp(f[i], "enabled=", 8) == 0){
				if(!oneof(v, "on", "off", nil, nil))
					e = smprint("bad value: %s", f[i]);
			}else if(strncmp(f[i], "check=", 6) == 0){
				if(!oneof(v, "on", "off", "mixed", nil))
					e = smprint("bad value: %s", f[i]);
			}else if(strncmp(f[i], "key=", 4) == 0)
				e = checkstroke(v);
			else if(strncmp(f[i], "label=", 6) != 0 && strncmp(f[i], "menu=", 5) != 0)
				e = smprint("bad value: %s", f[i]);
		}
	}
	free(buf);
	if(e != nil)
		return e;
	for(i = 0; i < app->nmenu; i++)
		if(strcmp(app->menu[i].name, name) == 0){
			free(app->menu[i].text);
			app->menu[i].text = estrdup(text);
			return nil;
		}
	app->menu = erealloc(app->menu, (app->nmenu+1)*sizeof(Menu));
	app->menu[app->nmenu].name = estrdup(name);
	app->menu[app->nmenu].text = estrdup(text);
	app->nmenu++;
	return nil;
}

/*
 * /debug/inject.
 */
static char*
attr(Tok *t, int nt, char *name)
{
	int i, l;

	l = strlen(name);
	for(i = 0; i < nt; i++)
		if(strncmp(t[i].s, name, l) == 0 && t[i].s[l] == '=')
			return t[i].s+l+1;
	return nil;
}

static char*
injwin(Tok *t, int nt, Win **wp, Node **np, int needwin)
{
	char *s;

	*wp = nil;
	*np = nil;
	s = attr(t, nt, "win");
	if(s == nil)
		return needwin ? smprint("bad value: needs win=") : nil;
	if((*wp = winbyname(s)) == nil)
		return smprint("not found: window %s", s);
	s = attr(t, nt, "node");
	return resolve((*wp)->root, s != nil ? s : ".", np);
}

static int
isstate(char *name)
{
	int i;

	for(i = 0; statenames[i] != nil; i++)
		if(strcmp(statenames[i], name) == 0)
			return 1;
	return 0;
}

static char*
injevent(Tok *t, int nt)
{
	char *name, *via, *key, *e, *v, *q, *path;
	Win *w;
	Node *n;
	int i, st, *anc, nanc;
	Fmt f;

	if(nt < 2)
		return smprint("bad syntax: event NAME");
	name = t[1].s;
	if((e = injwin(t+2, nt-2, &w, &n, 0)) != nil)
		return e;
	via = attr(t+2, nt-2, "via");
	if(strcmp(name, "close") == 0 && w != nil && (n == nil || n == w->root)){
		request(Aclose, w, via != nil ? via : "user");
		return nil;
	}
	if(strcmp(name, "quit") == 0){
		request(Aquit, nil, via != nil ? via : "user");
		return nil;
	}
	st = isstate(name);
	fmtstrinit(&f);
	for(i = 2; i < nt; i++){
		v = strchr(t[i].s, '=');
		if(v == nil)
			continue;
		if(strncmp(t[i].s, "win=", 4) == 0 || strncmp(t[i].s, "node=", 5) == 0
		|| strncmp(t[i].s, "key=", 4) == 0 || strncmp(t[i].s, "via=", 4) == 0)
			continue;
		q = quoteval(v+1);
		fmtprint(&f, "%s%.*s=%s", f.nfmt ? " " : "", (int)(v-t[i].s), t[i].s, q);
		free(q);
	}
	if(via != nil || st)
		fmtprint(&f, "%svia=%s", f.nfmt ? " " : "", via != nil ? via : "user");
	v = fmtstrflush(&f);
	key = attr(t+2, nt-2, "key");
	if(key != nil || n == nil){
		anc = nil;
		nanc = 0;
		if(n != nil){
			nanc = ancestors(n, &anc);
			path = relpath(n);
		}else
			path = estrdup("-");
		emit(name, key != nil ? key : "-", path, v, w != nil ? w->name : nil, anc, nanc, st);
		free(path);
		free(anc);
	}else
		emitnode(name, w, n, v, st);
	free(v);
	return nil;
}

static void
fix(Node *n)
{
	int i;

	n->fixed = 1;
	for(i = 0; i < n->nkid; i++)
		fix(n->kid[i]);
}

static char*
injkey(Tok *t, int nt)
{
	char *stroke, *buf, *line, *nl, *f[16], *ctx, *id, *path;
	char **chain;
	int nchain, best, rank, i, k, n;
	Win *w;
	Node *fn, *x;

	if((w = winbyname(attr(t, nt, "win") != nil ? attr(t, nt, "win") : "")) == nil)
		return smprint("not found: window");
	stroke = attr(t, nt, "stroke");
	if(stroke == nil || checkstroke(stroke) != nil)
		return smprint("bad value: stroke");
	fn = nodebyid(w->focus);
	chain = nil;
	nchain = 0;
	for(x = fn; x != nil; x = x->parent)
		if((ctx = getprop(x, "context")) != nil && ctx[0] != 0){
			chain = erealloc(chain, (nchain+1)*sizeof(char*));
			chain[nchain++] = ctx;
		}
	best = -1;
	id = nil;
	buf = estrdup(app->keys);
	for(line = buf; line != nil && *line; line = nl){
		nl = strchr(line, '\n');
		if(nl != nil)
			*nl++ = 0;
		n = tokenize(line, f, nelem(f));
		if(n < 2 || strcmp(f[0], stroke) != 0)
			continue;
		rank = 1000;
		for(i = 2; i < n; i++)
			if(strncmp(f[i], "context=", 8) == 0){
				for(k = 0; k < nchain; k++)
					if(strcmp(chain[k], f[i]+8) == 0)
						break;
				rank = k < nchain ? k : -1;
			}
		if(rank < 0)
			continue;
		if(best < 0 || rank < best){
			best = rank;
			free(id);
			id = estrdup(f[1]);
		}
	}
	free(buf);
	free(chain);
	if(id != nil){
		path = fn != nil ? relpath(fn) : estrdup(".");
		emit("action", id, path, "via=key", w->name, nil, 0, 0);
		free(path);
		free(id);
	}
	return nil;
}

char*
injectline(char *line)
{
	Line *L;
	int nl, nt, i, k;
	Tok *t;
	char *e, *v, *old, *file;
	Win *w;
	Node *n;

	nl = lex(line, &L, &e);
	if(nl < 0)
		return e;
	if(nl == 0){
		freelines(L, nl);
		return nil;
	}
	t = L[0].t;
	nt = L[0].n;
	e = nil;
	if(strcmp(t[0].s, "event") == 0)
		e = injevent(t, nt);
	else if(strcmp(t[0].s, "mirror") == 0){
		file = attr(t+1, nt-1, "file");
		v = attr(t+1, nt-1, "value");
		if(file == nil || v == nil)
			e = smprint("bad syntax: mirror file= value=");
		else if((e = injwin(t+1, nt-1, &w, &n, 0)) == nil){
			if(w == nil)
				e = smprint("bad value: needs win=");
			else if(attr(t+1, nt-1, "node") == nil && strcmp(file, "focus") == 0)
				e = setfocus(w, v, 0);
			else if(attr(t+1, nt-1, "node") == nil && strcmp(file, "state") == 0){
				free(w->state);
				w->state = estrdup(v);
			}else if(strcmp(file, "style") == 0){
				free(n->style);
				n->style = estrdup(v);
			}else
				setprop(n, file, v, 0);
		}
	}else if(strcmp(t[0].s, "bounds") == 0){
		v = attr(t+1, nt-1, "value");
		if((e = injwin(t+1, nt-1, &w, &n, 1)) == nil){
			free(n->bounds);
			n->bounds = estrdup(v != nil ? v : "");
		}
	}else if(strcmp(t[0].s, "frame") == 0){
		v = attr(t+1, nt-1, "win");
		for(i = 0; i < app->nwin; i++)
			if(v == nil || strcmp(app->win[i]->name, v) == 0)
				fix(app->win[i]->root);
	}else if(strcmp(t[0].s, "tick") == 0){
		if(nt < 2)
			e = smprint("bad syntax: tick SECONDS");
		else
			tick(atoi(t[1].s));
	}else if(strcmp(t[0].s, "key") == 0)
		e = injkey(t+1, nt-1);
	else if(strcmp(t[0].s, "menu") == 0){
		v = attr(t+1, nt-1, "item");
		old = attr(t+1, nt-1, "win");
		if(v == nil)
			e = smprint("bad syntax: menu item=ID [win=NAME]");
		else if(old != nil && winbyname(old) == nil)
			e = smprint("not found: window %s", old);
		else
			emit("action", v, "-", "via=menu", old, nil, 0, 0);
	}else if(strcmp(t[0].s, "appearance") == 0){
		v = attr(t+1, nt-1, "mode");
		if(v == nil || !oneof(v, "light", "dark", nil, nil))
			e = smprint("bad value: appearance mode=light|dark");
		else{
			old = effmode();
			k = strcmp(old, v);
			old = estrdup(old);
			mode = strcmp(v, "dark") == 0 ? "dark" : "light";
			if(strcmp(old, effmode()) != 0){
				v = smprint("mode=%s via=system", effmode());
				emit("theme", "-", "-", v, nil, nil, 0, 1);
				free(v);
			}
			USED(k);
			free(old);
		}
	}else if(strcmp(t[0].s, "drag") == 0)
		e = smprint("unsupported: no transfer layer");
	else
		e = smprint("unknown command: %s", t[0].s);
	freelines(L, nl);
	return e;
}
