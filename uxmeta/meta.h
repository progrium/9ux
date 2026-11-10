/*
 * uxmeta: layered ndb metadata.  See docs/protocols/meta.md.
 *
 * Nothing here locks: the caller (uxmeta's 9P server) serializes every
 * call under one lock.
 */

typedef struct Tup Tup;
typedef struct Rec Rec;
typedef struct Chunk Chunk;
typedef struct Nfile Nfile;
typedef struct Layer Layer;
typedef struct Ent Ent;
typedef struct View View;
typedef struct Change Change;
typedef struct Meta Meta;

enum {
	Maxfile = 16*1024*1024,	/* largest layer file read */
	Nhash = 1024,
	Attrmax = 31,		/* ndb's limit (Ndbalen-1) */
};

/* one attr=value; val nil for a bare attribute */
struct Tup {
	char	*attr;
	char	*val;
};

/* a record: t[0] is the kind and key */
struct Rec {
	Tup	*t;
	int	nt;
};

/* a piece of a file: verbatim text, a record, or both (unchanged record) */
struct Chunk {
	char	*text;	/* verbatim text; nil once rec is changed */
	Rec	*rec;	/* nil for comments and blank lines */
};

struct Nfile {
	Chunk	*c;
	int	nc;
};

enum {	/* layer classes, lowest precedence first */
	Lsystem,
	Lapps,
	Luser,
	Lworkspace,
};

struct Layer {
	char	*name;	/* system/NAME, apps/APP, user, workspace */
	char	*path;
	int	class;
	int	writable;
	int	present;
	Nfile	f;
	char	*text;	/* contents last read or written */
	Qid	qid;
	vlong	length;
	ulong	mtime;
	char	*err;	/* read error, or ignored kinds */
	int	dirty;	/* changed in memory, not yet written */
	int	seen;	/* scratch for rescans */
};

/* an entry of the merged view */
struct Ent {
	char	*kind;
	char	*key;
	Rec	*r;	/* merged record; nil if removed */
	Ent	*next;
};

struct View {
	Ent	*h[Nhash];
	int	n;
};

enum { Cset, Cdel };
struct Change {
	int	op;
	char	*kind;
	char	*key;
};

struct Meta {
	char	*sysdir;	/* system layer: sysdir/ *.ndb */
	char	*appsdir;	/* apps layer: appsdir/ * /app.ndb */
	char	*userfile;
	char	*wsfile;
	char	*genfile;	/* nil: don't persist gen */
	char	*plumbfile;	/* nil: don't generate rules */
	char	*userplumbing;	/* with reloadplumb: written to plumbrules */
	char	*plumbrulesfile;	/* /mnt/plumb/rules */
	int	reloadplumb;
	void	(*notify)(Meta*, Change*, int);	/* after each commit that changed something */
	Layer	**l;
	int	nl;
	View	*v;
	uvlong	gen;
	char	*rules;	/* generated plumbing rules */
	Change	*ch;	/* changes from the last commit */
	int	nch;
};

/* ndb.c */
int	ndbparse(char*, Nfile*);
void	nfilefree(Nfile*);
char*	nfiletext(Nfile*);
int	recfmt(Fmt*, Rec*);
int	recfmtw(Fmt*, Rec*, int);
char*	rectext(Rec*);
Rec*	recnew(char*, char*);
Rec*	recdup(Rec*);
void	recfree(Rec*);
char*	recget(Rec*, char*);
void	recadd(Rec*, char*, char*);
int	recdel(Rec*, char*);
void	recset(Rec*, char*, char**, int);
int	receq(Rec*, Rec*);
int	validattr(char*);
int	validval(char*);
Chunk*	nfilefind(Nfile*, char*, char*);
void	nfileappend(Nfile*, Rec*);
void	nfiledel(Nfile*, Chunk*);
void*	emalloc(ulong);
void*	erealloc(void*, ulong);
char*	estrdup(char*);

/* layer.c */
int	layeradmits(Layer*, char*);
int	layerload(Layer*);
int	layersave(Layer*);
int	layerchanged(Layer*);
int	metarescan(Meta*);
Layer*	metalayer(Meta*, char*);
Layer*	metadefault(Meta*, char*);
char*	metalayers(Meta*);
int	metalock(char*);
int	atomicwrite(char*, char*);
char*	readfile(char*, int*);
void	metarecover(char*);
int	mkdirs(char*);
void	metaloadgen(Meta*);
void	metasavegen(Meta*);

/* view.c */
View*	viewbuild(Meta*);
void	viewfree(View*);
Ent*	viewlook(View*, char*, char*);
Ent**	viewsorted(View*, int*);
int	viewdiff(View*, View*, Change**);
void	changesfree(Change*, int);
int	metacommit(Meta*);
char*	metaquery(Meta*, char*);
char*	metadb(Meta*);
Rec*	metaresolve(Meta*, char*);
int	lowerhas(Meta*, Layer*, char*, char*);

/* txn.c */
enum {	/* rights */
	Rro = 1,
	Rlay = 2,
	Rrw = 4,
};
char*	metactl(Meta*, char*, int);
int	metarefresh(Meta*);

/* plumb.c */
char*	plumbrules(Meta*);
int	metaplumb(Meta*);
