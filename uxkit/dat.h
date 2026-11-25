/*
 * uxkit: a uitree host (spec draft 0.2) for 9/UX.
 *
 * It serves uitree's file protocol over 9P: the app directory, windows
 * as node trees, the ctl grammar, documents, events, requests, batches,
 * theme, keys, menus, clip and types/. This is the headless form the
 * conformance suite drives: nothing is drawn, and time is a fake clock
 * that /debug/inject's tick moves.
 */

#define emalloc	kemalloc
#define erealloc	kerealloc
#define estrdup	kestrdup

typedef struct Prop Prop;
typedef struct Node Node;
typedef struct Win Win;
typedef struct Menu Menu;
typedef struct App App;
typedef struct Pdesc Pdesc;
typedef struct Type Type;
typedef struct Tok Tok;
typedef struct Line Line;
typedef struct Ev Ev;
typedef struct Sub Sub;
typedef struct Rq Rq;
typedef struct Pend Pend;

/* a prop, a bare word (flag or bool that is on), or a kind word (name "@kind") */
struct Prop {
	char	*name;
	char	*val;
	int	bare;
};

struct Node {
	int	id;
	char	*name;
	char	*type;
	Node	*parent;
	Node	**kid;
	int	nkid;
	int	*ord;	/* the order file: child ids */
	int	nord;
	Prop	*prop;
	int	nprop;
	char	*style;
	char	*bounds;
	int	fixed;	/* type can't change: a frame was drawn */
	Win	*win;
};

struct Win {
	char	*name;
	Node	*root;
	char	*title;
	int	w, h;
	int	minw, minh, maxw, maxh;
	char	*pos;
	char	*kind;
	char	*titlebar;
	int	resizable;
	char	*state;
	int	shown;
	int	claimed;
	int	focus;	/* node id, or 0 */
	int	ask;	/* -1: the app's; 0: none; 1: close */
};

struct Menu {
	char	*name;
	char	*text;
};

enum {
	Aclose = 1,
	Aquit = 2,
	Aopen = 4,
};

enum {
	Ntok = 22,
};

struct App {
	Win	**win;
	int	nwin;
	char	*tok[Ntok];
	char	*pref;
	char	*themename;
	char	*keys;
	char	*bar;
	Menu	*menu;
	int	nmenu;
	int	ask;
	int	nextid;
};

/* a type's descriptor, parsed from its spec */
struct Pdesc {
	char	*name;
	char	*form;
	int	file;
	int	readonly;
	char	*dflt;
	char	*min;
	char	*max;
};

struct Type {
	char	*name;
	char	*spec;
	Pdesc	*p;
	int	np;
	char	**word;	/* bare words */
	char	**wkind;	/* each word's kind: flag, size, variant... */
	int	nword;
	int	child;	/* 0: none, 1: any, 2: the list */
	char	**childok;
	int	nchildok;
};

/* a token of a ctl line */
struct Tok {
	char	*s;
	int	quoted;
	int	here;
};

/* a line of a ctl write or document: its tokens and indent */
struct Line {
	Tok	*t;
	int	n;
	int	indent;
};

/* a queued event line */
struct Ev {
	char	*line;
	char	*name;
	char	*win;
	int	node;
	int	state;
	Ev	*next;
};

enum {
	Sapp,
	Swin,
	Snode,
};

struct Rq {
	Req	*r;
	Rq	*next;
};

struct Sub {
	int	kind;
	char	*win;
	int	node;
	Ev	*head;
	Ev	*tail;
	int	nq;
	int	lost;
	char	*cur;	/* the line being read */
	long	curoff;
	Rq	*rq;
	int	gone;
	int	eof;
	Sub	*next;
};

/* an event made while a ctl write runs, sent if it succeeds */
struct Pend {
	char	*name;
	char	*key;
	char	*path;
	char	*attrs;
	char	*win;
	int	*anc;
	int	nanc;
	int	state;
	int	seq;
	Pend	*next;
};

/* model.c */
extern App	*app;
extern Type	*types;
extern int	ntypes;
extern char	*tokname[Ntok];
extern char	*statenames[];
extern char	*stylewords[];
extern char	*mode;	/* the system's appearance */
extern char	*clip;
extern long	nclip;
extern int	now;
extern int	quitting;

void	initapp(void);
App*	appcopy(App*);
void	appfree(App*);
Node*	nodebyid(int);
Win*	winbyname(char*);
Node*	newnode(Node*, char*, char*);
void	delnode(Node*);
Node*	kidbyname(Node*, char*);
char*	relpath(Node*);
char*	uipath(Node*);
Type*	typebyname(char*);
Pdesc*	pdesc(Type*, char*);
char*	getprop(Node*, char*);
void	setprop(Node*, char*, char*, int);
void	rmprop(Node*, char*);
char*	checkstyle(char*);
char*	checkcolor(char*);
char*	checkon(char*);
char*	checkstroke(char*);
char*	checkvalue(Node*, Pdesc*, char*);
int	isnodefile(char*, int);
char*	printtree(Node*);
char*	quoteval(char*);
char*	settype(Node*, char*);
char*	canhold(Node*, char*);
int	ancestors(Node*, int**);
char*	effmode(void);
Win*	newwin(char*);
void	delwin(Win*);
char*	nodeword(Node*, char*);
void*	emalloc(ulong);
void*	erealloc(void*, ulong);
char*	estrdup(char*);
char*	trim1(char*, long);

/* cmd.c */
int	lex(char*, Line**, char**);
void	freelines(Line*, int);
char*	ctlrun(Node*, char*);
char*	injectline(char*);
char*	setwinprop(Win*, char*, char*);
char*	settree(Node*, char*);
char*	settokens(char*);
char*	setkeys(char*);
char*	setmenu(char*, char*);
char*	setfocus(Win*, char*, int);
char*	resolve(Node*, char*, Node**);
void	closewin(Win*, int);
void	tick(int);
void	doquit(void);

/* ev.c */
extern Sub	*subs;
extern int	seq;
extern int	holding;
Sub*	newsub(int, char*, int);
void	freesub(Sub*);
int	emit(char*, char*, char*, char*, char*, int*, int, int);
int	emitnode(char*, Win*, Node*, char*, int);
void	emiterr(char*, char*, Node*, Win*, char*);
void	hold(void);
void	release(int);
void	sweep(void);
void	subread(Sub*, Req*);
void	subflush(Req*);
void	servesubs(void);
int	anysub(void);
char*	codeof(char*);
void	startresponder(void);
extern int	srvpid;

/* fs.c */
void	lockmodel(void);
void	unlockmodel(void);

/* cmd.c, for ui.c */
void	closerequest(Win*, char*);
void	quitrequest(char*);

/* ui.c */
extern int	drawing;
void	uisync(void);
void	uistart(void);
void	uirun(void);
char*	uibounds(Node*);
