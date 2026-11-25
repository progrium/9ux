/*
 * libux internals.
 *
 * Locking: one QLock, uxlk, guards the whole model. The UI thread holds
 * it whenever it is not waiting for input or running app handlers; the
 * tree (tapi.c) takes it, after the model lock, to drive the drawing
 * layer. Functions whose names
 * start with _ expect it held. Handlers are never run with it held:
 * internal code queues them with _notify() and the UI thread runs them
 * after releasing the lock.
 */

#include "uxstyle.h"

#include "uxglyphs.h"

typedef struct Obj Obj;
typedef struct App App;
typedef struct Ctltype Ctltype;
typedef struct Verb Verb;
typedef struct Item Item;
typedef struct Icon Icon;
typedef struct Note Note;
typedef struct Action Action;
typedef struct Uxstyle Uxstyle;
typedef struct Uxlen Uxlen;
typedef struct Uxpop Uxpop;

/* spike: uitree style words (flex.c) */
enum {
	Lnone,
	Lpx,
	Lpct,
};

struct Uxlen {
	int	kind;
	int	v;
};

enum { Dcol, Drow };
enum { Astretch, Astart, Acenter, Aend };
enum { Jstart, Jcenter, Jend, Jbetween };

struct Uxstyle {
	int	set;
	int	dir;
	int	grow, shrink, basis0;
	int	align, justify;
	int	gap;
	int	pad[4], mar[4];	/* top right bottom left */
	Uxlen	w, h, minw, minh, maxw, maxh;
	int	abs;
	int	inset[4];	/* top right bottom left, with hasinset bits */
	int	hasinset;
	ulong	bg, fg, bordercol;
	int	hasbg, hasfg, hasbordercol;
	int	border;
	int	textsize;	/* 0 xs .. 5 3xl, -1 unset */
	int	bold;	/* -1 unset */
	int	textalign;	/* -1 unset, 0 left, 1 center, 2 right */
	int	truncate;
};

enum {
	Oapp,
	Owin,
	Octl,
	Omenu,
};

/* rights, as in ux.md */
enum {
	Rnone,
	Ractivate,
	Redit,
};

/* window state */
enum {
	Wcurrent	= 1<<0,
	Whidden		= 1<<1,
	Wmodal		= 1<<2,
};

/* icon view drags (Uxctl.drag) */
enum {
	Dnone,
	Dpress,	/* button down on an icon, not moved yet */
	Dmove,	/* moving the selected icons */
	Dband,	/* rubber band selection */
};

/*
 * Set by drag and drop (S9): called while selected icons are being
 * moved and the pointer leaves the icon view. Returning 1 takes the
 * drag over; the icons go back where they were.
 */
extern int	(*_icondragout)(Uxctl*, Mouse*);

/* control state; the low bits are the UX* flags of ux.h */
enum {
	Cenabled	= 1<<8,
	Cvisible	= 1<<9,
	Cdefault	= 1<<10,
};

enum {
	Maxvalue	= 1<<20,
};

struct Obj {
	int	kind;
	ulong	oid;	/* index in the object table; never reused */
	int	dead;
};

struct Verb {
	char	*name;
	int	right;
	int	min, max;	/* argument counts, not counting the verb */
	char*	(*fn)(Uxctl*, int, char**);
};

struct Ctltype {
	char	*name;
	char	**files;	/* type-specific files */
	Verb	*verbs;
	char*	(*read)(Uxctl*, char*);	/* snapshot of a type file */
	char*	(*writevalue)(Uxctl*, char*);	/* value file write, at clunk */
	void	(*draw)(Uxctl*);
	int	(*mouse)(Uxctl*, Mouse*);	/* 1: wants the mouse until release */
	int	(*key)(Uxctl*, Rune);	/* 1: took the key */
	int	focusable;
};

struct Icon {
	char	*id;
	char	*label;
	char	*icon;
	Point	pt;
	int	sel;
	int	open;	/* its window is open: drawn hatched */
};

struct Uxctl {
	Obj;
	Uxwin	*w;
	char	*id;
	Ctltype	*t;
	char	*label;
	int	state;
	Rectangle	r;
	Uxctl	*parent;
	Uxfn	*fn;
	void	*arg;

	/* type data */
	Rune	*text;	/* text-field, label; nil means empty */
	int	ntext;
	int	q0, q1;	/* selection, rune offsets */
	int	org;	/* first rune shown (one-line fields) */
	int	check;	/* checkbox: 0 off, 1 on, 2 mixed */
	char	**items;	/* list */
	int	nitems;
	uchar	*sel;	/* list: per item */
	int	top;	/* list: first item shown */
	Icon	*icons;
	int	nicons;
	Point	origin;	/* scroll-view */
	Rectangle	extent;
	int	bars;	/* scroll-view: UXvbar, UXhbar, UXgrowbox */
	int	header;	/* scroll-view: rows at the top that don't scroll, left of the bar */
	int	sbpart;	/* scroll-view: the part being tracked (Sb* in ctl.c) */
	Point	sborg;	/* scroll-view: the origin when a thumb drag began */
	char	**cols;	/* list: column titles, nil for none */
	int	*colw;	/* list: column widths */
	int	ncols;
	char	**ricons;	/* list: a small icon per item, or nil */
	char	*dir;	/* file-chooser */
	char	*name;
	Uxctl	*inner;	/* file-chooser's list */
	int	pressed;	/* button being tracked */
	int	lastclick;	/* for double clicks */
	int	lastitem;
	int	drag;	/* icon view: Dnone, Dpress, Dmove, Dband */
	Point	dragp;	/* where the button went down */
	Point	dragl;	/* the pointer at the last move */
	Rectangle	band;	/* the rubber band, screen coordinates */
	int	qh;	/* console: the output point; what's after it is typed */
	int	qa;	/* console: where a selection drag began */
	int	*line;	/* console: where each wrapped line starts */
	int	nline;
	int	lfrom;	/* console: wrap again from here */
	int	lw;	/* console: the width it was wrapped to */
	int	raw;	/* console: keys go straight to the app */
	int	follow;	/* console: show the end at the next layout */
	int	vmin, vmax, vstep, val;	/* spike: slider */
	int	row;	/* spike: radios laid out in a row */
	char	*ops;	/* spike: canvas display list */
	Font	*cfont;	/* console: its own font, or nil for the body font */

	/* spike: uitree layout, svg and styled-text */
	Uxstyle	*st;	/* style words, or nil */
	char	*svgsrc;	/* svg: the document */
	void	*svg;	/* svg: parsed (svg.c) */
	Image	*svgimg;	/* svg: drawn at its last size */
	ulong	svgfg;	/* svg: in this currentColor */
	char	*stext;	/* styled-text: the text */
	char	*runs;	/* styled-text: the runs */
};

struct Uxwin {
	Obj;
	int	winid;
	char	*kind;
	char	*title;
	Rectangle	r;	/* screen coordinates, whole window */
	Image	*named;	/* a dialog's rio window image; img is our window inside it */
	Screen	*scr;
	int	state;
	Uxctl	**ctl;
	int	nctl;
	Uxctl	*focus;
	Uxctl	*def;
	Uxctl	*grab;	/* control tracking the mouse */
	Uxfn	*fn;
	void	*arg;

	/* drawing; img is nil when headless */
	Display	*d;
	Image	*img;
	Font	*font;	/* body role */
	Font	*sysfont;	/* system role */
	Font	*smallfont;	/* small role */
	Mousectl	*mc;
	Keyboardctl	*kc;
	int	dirty;
	int	main;	/* the app's own rio window */
	Mouse	lastm;
	int	wctlfd;	/* a dialog's wctl, or -1 */
	int	edge;	/* we draw out to rio's frame: chrome edge, in 9/UX mode */
	int	edgeok;	/* rio took chrome edge for this window */
	Image	*edgeimg;	/* the main window's image when edge: over the border too */
	int	winnamefd;	/* a dialog's winname, or -1 */
	Image	*col[128];	/* solid colours, by value (_color) */
	ulong	colv[128];
	int	ncol;
	Image	*shade;	/* a selected icon's darkening */
	Image	*glyph[32];
	Image	*glyphw[32];	/* the glyphs' white layers */
	Image	*hatch;	/* grey3 every other pixel, for open icons */
	Rectangle	clip;	/* where the control being drawn may draw */
	Uxstyle	*st;	/* spike: a root style: lay the window out by flexbox */
	Uxpop	*pop;	/* spike: a pop-up menu over the window (widget.c) */
	Uxctl	*hover;	/* spike: the control under the pointer, for UXhover */
};

struct Item {
	char	*action;
	char	*label;
	char	*key;
	int	sep;
	int	enabled;
	int	check;
};

struct Uxmenu {
	Obj;
	char	*id;
	char	*title;
	Item	*item;
	int	nitem;
};

struct Action {
	char	*id;
	Uxfn	*fn;
	void	*arg;
	Action	*next;
};


/* a handler call waiting for the lock to be released */
struct Note {
	Uxfn	*fn;
	void	*arg;
	Uxev	ev;
	char	*s1, *s2;	/* strings owned by the note */
	Note	*next;
};

struct App {
	char	*name;
	char	*title;
	char	*id;	/* from uxd, or nil */
	char	*user;
	Obj	**obj;
	int	nobj;
	Uxwin	**win;
	int	nwin;
	Uxmenu	**menu;
	int	nmenu;
	Action	*actions;
	Uxfn	*fn;
	void	*arg;
	Note	*notes, **notetail;
	Channel	*uikick;	/* wake the UI thread */
	int	quit;
	int	headless;
	int	nextwinid;	/* headless window IDs */
	int	menufd;	/* /dev/ux/menubar/ctl, or -1 */
	int	devux;
	Channel	*menuc;	/* actions from the bar: char* lines */
	Uxwin	*front;
	int	hadmain;
	int	regfd;	/* /mnt/ux/register, or -1 */
	int	(*post)(char*);	/* uxpublish: serves the tree instead of libux */
	char	*barid;	/* the app ID the menu bar knows us by */
	int	rebar;	/* register with the bar again */
	int	barreader;
	int	evreader;
	int	riooff;	/* rio's 9/UX mode is off: it paints its own border */
};

extern QLock uxlk;
extern App ux;

/* obj.c */
void	_addobj(Obj*, int);
Obj*	_lookobj(ulong);
void	_delobj(Obj*);
Uxwin*	_findwin(int);
Uxctl*	_findctl(Uxwin*, char*);
Uxmenu*	_findmenu(char*);
int	_validid(char*);
char*	_ctlpath(Uxctl*);
char*	_flags(int, int);
char*	_runestr(Rune*, int);
void*	emalloc(ulong);
char*	estrdup(char*);
void*	erealloc(void*, ulong);

/* ev.c */
void	_notify(Uxfn*, void*, Uxev*);
void	_runnotes(void);
char*	_attr(char*, char*);
char*	_quote(char*);

/* cmd.c */
char*	_runcmd(Obj*, int, char*);
char*	_ctlhelp(Obj*);

/* ctl.c */
extern Ctltype *ctltypes[];
Uxctl*	_newctl(Uxwin*, char*, Ctltype*, char*, Rectangle);
void	_ctlevent(Uxctl*, int, int);
char*	_ctlread(Uxctl*, char*);
char*	_settext(Uxctl*, char*, int);
void	_setfocus(Uxwin*, Uxctl*, int);
void	_activate(Uxctl*, int);
void	_setcheck(Uxctl*, int, int);
void	_setsel(Uxctl*, uchar*, int);
void	_listopen(Uxctl*, int, int);
char*	_listsel(Uxctl*, int);
void	_iconsel(Uxctl*, int);
void	_iconopen(Uxctl*, char*, int);
void	_setorigin(Uxctl*, Point, int);
Point	_clampedorigin(Uxctl*, Point);
int	_thumbat(int, int, int, int, int*);
void	_clamporigin(Uxctl*, int);
void	_chooserload(Uxctl*);

/* win.c */
Uxwin*	_newwin(char*, char*, Rectangle);
void	_delwin(Uxwin*);
void	_winclose(Uxwin*, int);
void	_focusto(int winid);
void	_redraw(Uxwin*);
void	_drawdirty(void);
void	_wininput(Uxwin*, Mouse*);
void	_winkey(Uxwin*, Rune);
void	_winresize(Uxwin*);
void	_front(Uxwin*);
void	_winstate(Uxwin*, int);
int	_dialogwin(Uxwin*, Rectangle);

/* draw.c */
void	_drawctl(Uxctl*);
Rectangle	_abs(Uxctl*);
Rectangle	_local(Uxctl*);
Rectangle	_vis(Uxctl*);
Rectangle	_vislocal(Uxctl*);
void	_scrollparts(Uxctl*, Rectangle, Rectangle*, Rectangle*, Rectangle*, Rectangle*);
int	_listhead(Uxctl*);
Image*	_color(Uxwin*, ulong);

/*
 * A theme role as a colour value, for tables made before the theme is
 * known (flex.c's tokens): alpha 1, the role's index above it. _color
 * and _rgba turn it into the current theme's value.
 */
#define	UXrole(r)	((ulong)(r)<<8 | 0x01)
#define	_rgba(v)	(((v)&0xFF) == 0x01 && ((v)>>8) < UXNrole ? uxtheme[(v)>>8] : (v))
void	_winfonts(Uxwin*);

/* menu.c */
void	_barevent(char*);
void	_menuaction(char*, int, char*);
void	_menuregister(void);
char*	_menuitems(Uxmenu*);
void	_menuwin(Uxwin*, int);
void	_chrome(Uxwin*);
void	_edgeimage(Uxwin*);
void	_riomode(int);
char*	_riotheme(void);
extern void	(*_themehook)(char*);
Rectangle	_riorect(Uxwin*);

/* res.c */
Image*	_resicon(Uxwin*, char*, int);
void	_resdraw(Uxwin*);
void	_resfreewin(Uxwin*);

/* clip.c */
long	_clipput(char*);
char*	_clipget(void);
char*	_tfedit(Uxctl*, char*);	/* ctl.c */

/* cons.c */
extern Ctltype	_constype;
void	_conslayout(Uxctl*);
void	_drawcons(Uxctl*, Rectangle);
char*	_consedit(Uxctl*, char*);

/* drag.c */
int	_dragstart(Uxdrag*, Uxctl*);

/* flex.c, svg.c, stext.c: the spike */
extern Ctltype	_divtype, _svgtype, _stexttype;
/* spike: widget.c */
int	_iswidget(Uxctl*);
Point	_widgetsize(Uxctl*, int);
void	_drawwidget(Uxctl*, Rectangle);
void	_popopen(Uxwin*, Point, char**, int, int, void(*)(Uxwin*, int, void*), void*);
void	_popclose(Uxwin*, int);
void	_popmouse(Uxwin*, Mouse*, int);
void	_drawpop(Uxwin*);
int	_iscanvas(Uxctl*);
void	_drawcanvas(Uxctl*, Rectangle);
void	_canvasmove(Uxctl*, Mouse*);
int	_parsecolor(char*, ulong*);
int	_spikemouse(Uxctl*, Mouse*);
void	_styledefault(Uxstyle*);
char*	_parsestyle(Uxstyle*, char*);
void	_flexlayout(Uxwin*);
Point	_flexnatural(Uxwin*, int);
Font*	_stylefont(Uxwin*, Uxstyle*, int);
Font*	_font(Uxwin*, int, int, int);
Image*	_anycolor(Uxwin*, ulong);
void	_drawdiv(Uxctl*, Rectangle);
Point	_svgsize(Uxctl*);
void	_drawsvg(Uxctl*, Rectangle);
void	_svgfree(Uxctl*);
Point	_stextsize(Uxctl*, int);
void	_drawstext(Uxctl*, Rectangle);

/* reg.c */
void	_register(void);
void	_regwin(int, int);
