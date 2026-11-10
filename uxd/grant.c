/*
 * Rights and grants (ux.md, Rights and Grants).
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <libsec.h>
#include "dat.h"
#include "fns.h"

Grant *grants;
char *grantfile;

static char *rightname[] = {
	"read",
	"sensitive",
	"activate",
	"edit",
	"keys",
	"clipboard",
	"admin",
};

/* "read", "edit", ...: one right, or -1 */
int
rightbit(char *s)
{
	int i;

	for(i = 0; i < nelem(rightname); i++)
		if(strcmp(s, rightname[i]) == 0)
			return 1<<i;
	return -1;
}

/* rights as words joined by sep */
char*
rightstr(int rights, char *sep)
{
	char *s, *t;
	int i;

	s = estrdup9p("");
	for(i = 0; i < nelem(rightname); i++)
		if(rights & (1<<i)){
			t = smprint("%s%s%s", s, *s ? sep : "", rightname[i]);
			free(s);
			s = t;
		}
	return s;
}

/* "activate,edit" -> bits, or -1 */
static int
parserights(char *s)
{
	char *f[16], *p;
	int i, n, b, r;

	p = estrdup9p(s);
	n = getfields(p, f, nelem(f), 1, ",");
	r = 0;
	for(i = 0; i < n; i++){
		if((b = rightbit(f[i])) < 0){
			free(p);
			return -1;
		}
		r |= b;
	}
	free(p);
	return r;
}

/* shell-style match with * only */
int
match(char *pat, char *s)
{
	if(pat == nil)
		return 1;
	for(; *pat; pat++, s++){
		if(*pat == '*'){
			for(;;){
				if(match(pat+1, s))
					return 1;
				if(*s == 0)
					return 0;
				s++;
			}
		}
		if(*pat != *s)
			return 0;
	}
	return *s == 0;
}

/* what a fid attached as may do to app (nil: uxd's own files) */
int
rightsfor(Att *a, App *app)
{
	Grant *g;

	if(a->admin)
		return Rall;
	g = a->g;
	if(g == nil || g->revoked)
		return Rsread;
	if(app != nil && g->app != nil && !match(g->app, app->id) && !match(g->app, app->name))
		return Rsread;
	return Rsread | g->rights;
}

/* the rights an attach has, as the rights file shows them */
int
attrights(Att *a)
{
	if(a->admin)
		return Rall;
	if(a->g == nil || a->g->revoked)
		return Rsread;
	return Rsread | a->g->rights;
}

Grant*
lookgrant(char *token)
{
	Grant *g;

	for(g = grants; g != nil; g = g->next)
		if(!g->revoked && strcmp(g->token, token) == 0)
			return g;
	return nil;
}

static char*
grantline(Grant *g)
{
	char *r, *a[3], *s;
	int i;

	r = rightstr(g->rights, ",");
	a[0] = smprint("rights=%s", r);
	a[1] = smprint("app=%s", g->app ? g->app : "*");
	a[2] = smprint("note=%s", g->note ? g->note : "");
	for(i = 0; i < 3; i++){
		s = qtok(a[i]);
		free(a[i]);
		a[i] = s;
	}
	s = smprint("grant token=%s %s %s %s\n", g->token, a[0], a[1], a[2]);
	for(i = 0; i < 3; i++)
		free(a[i]);
	free(r);
	return s;
}

/* grants/list */
char*
grantlist(void)
{
	Grant *g;
	char *s, *l, *t;

	s = estrdup9p("");
	for(g = grants; g != nil; g = g->next){
		if(g->revoked)
			continue;
		l = grantline(g);
		t = smprint("%s%s", s, l);
		free(s);
		free(l);
		s = t;
	}
	return s;
}

static void
savegrants(void)
{
	char *s, *dir, *p;
	int fd;

	if(grantfile == nil)
		return;
	dir = estrdup9p(grantfile);
	if((p = strrchr(dir, '/')) != nil){
		*p = 0;
		mkdirs(dir);
	}
	free(dir);
	s = grantlist();
	if((fd = create(grantfile, OWRITE|OTRUNC, 0600)) < 0){
		fprint(2, "uxd: create %s: %r\n", grantfile);
		free(s);
		return;
	}
	if(write(fd, s, strlen(s)) != strlen(s))
		fprint(2, "uxd: write %s: %r\n", grantfile);
	close(fd);
	free(s);
}

static Grant*
addgrant(char *token, int rights, char *app, char *note)
{
	Grant *g, **l;

	g = emalloc9p(sizeof *g);
	memset(g, 0, sizeof *g);
	g->ref = 1;
	strecpy(g->token, g->token+sizeof g->token, token);
	g->rights = rights;
	g->app = app && *app && strcmp(app, "*") != 0 ? estrdup9p(app) : nil;
	g->note = note && *note ? estrdup9p(note) : nil;
	for(l = &grants; *l != nil; l = &(*l)->next)
		;
	*l = g;
	return g;
}

static int
istoken(char *s)
{
	int i;

	for(i = 0; i < 32; i++)
		if(!(s[i] >= '0' && s[i] <= '9' || s[i] >= 'a' && s[i] <= 'f'))
			return 0;
	return s[32] == 0;
}

/* parse "grant token=... rights=... [app=...] [note=...]" or without token= */
static char*
parsegrant(char *line, char **token, int *rights, char **app, char **note)
{
	char *f[16], *v;
	int i, n;

	n = tokenize(line, f, nelem(f));
	if(n < 1 || strcmp(f[0], "grant") != 0)
		return "unknown command";
	*rights = -1;
	for(i = 1; i < n; i++){
		if((v = strchr(f[i], '=')) == nil)
			return "bad syntax";
		*v++ = 0;
		if(strcmp(f[i], "rights") == 0){
			if((*rights = parserights(v)) < 0)
				return "bad value: unknown right";
		}else if(strcmp(f[i], "app") == 0)
			*app = v;
		else if(strcmp(f[i], "note") == 0)
			*note = v;
		else if(strcmp(f[i], "token") == 0 && token != nil)
			*token = v;
	}
	if(*rights < 0)
		return "bad syntax: rights= missing";
	return nil;
}

void
loadgrants(void)
{
	char *s, *lines[1024], *token, *app, *note;
	int i, n, rights;

	if(grantfile == nil || (s = readfile(grantfile)) == nil)
		return;
	n = getfields(s, lines, nelem(lines), 1, "\n");
	for(i = 0; i < n; i++){
		token = app = note = nil;
		if(parsegrant(lines[i], &token, &rights, &app, &note) != nil
		|| token == nil || !istoken(token) || lookgrant(token) != nil){
			fprint(2, "uxd: %s: bad line %d\n", grantfile, i+1);
			continue;
		}
		addgrant(token, rights & ~Radmin, app, note);
	}
	free(s);
}

/* grants/new: "grant rights=... [app=...] [note=...]" */
char*
newgrant(char *line, Grant **gp)
{
	uchar rnd[16];
	char token[33], *app, *note, *e;
	int i, rights;

	app = note = nil;
	if((e = parsegrant(line, nil, &rights, &app, &note)) != nil)
		return e;
	if(rights & Radmin)
		return "bad value: admin can't be granted";
	genrandom(rnd, sizeof rnd);
	for(i = 0; i < 16; i++)
		snprint(token+2*i, 3, "%.2ux", rnd[i]);
	*gp = addgrant(token, rights, app, note);
	savegrants();
	return nil;
}

/* grants/ctl: "revoke token" */
char*
grantctl(char *line)
{
	char *f[4];
	Grant *g;
	int n;

	n = tokenize(line, f, nelem(f));
	if(n == 0)
		return nil;
	if(strcmp(f[0], "revoke") != 0)
		return "unknown command";
	if(n != 2)
		return "bad syntax";
	if((g = lookgrant(f[1])) == nil)
		return "not found";
	g->revoked = 1;
	savegrants();
	return nil;
}
