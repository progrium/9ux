/*
 * libux: the 9/UX Toolbox.
 *
 * An app made with libux serves its controls as a 9P tree (see
 * docs/protocols/ux.md) while drawing them in rio windows. All state
 * lives in one model guarded by libux; the UI thread (the one that
 * calls uxmain) changes it and runs handlers, a separate 9P proc reads
 * it, and nothing a 9P client does can block the UI thread.
 *
 * Every function here may be called from any thread or proc of the
 * app. Handlers run on the UI thread with no libux lock held, so they
 * may call anything here.
 */

typedef struct Uxwin Uxwin;
typedef struct Uxctl Uxctl;
typedef struct Uxmenu Uxmenu;
typedef struct Uxev Uxev;

#pragma incomplete Uxwin
#pragma incomplete Uxctl
#pragma incomplete Uxmenu

/* Uxev.type */
enum {
	UXactivate = 1,	/* button pressed */
	UXvalue,	/* value changed (text, checkbox) */
	UXselection,	/* selection changed */
	UXsubmit,	/* Enter in a one-line field */
	UXopen,		/* list or icon items opened */
	UXrename,	/* icon item renamed (Uxev.item, Uxev.text) */
	UXmove,		/* icon item moved (Uxev.item, Uxev.pt) */
	UXscroll,	/* scroll view origin changed */
	UXaction,	/* menu action (Uxev.action) */
	UXclose,	/* window close asked for (close box or ctl) */
	UXdefault,	/* dialog default (Enter, or ctl default) */
	UXcancel,	/* dialog cancel (Esc, or ctl cancel); chooser cancelled */
	UXchoose,	/* file chooser chose Uxev.text */
	UXquit,		/* app quit asked for */
	UXresize,	/* window moved or resized (Uxev.win); see uxwinrect */
	UXfront,	/* window became the app's current window */
	UXdrop,		/* dropped on a drop target (Uxev.text type, .action op, .pt, .drag) */
	UXdragend,	/* our drag ended: Uxev.text the op the target did, nil if cancelled */
};

/* how an event came about; Uxev.via */
enum {
	UXui,		/* a person using the window */
	UXkey,		/* a keyboard shortcut */
	UXmenubar,	/* the menu bar */
	UXctl,		/* a 9P command */
};

struct Uxev {
	int	type;
	int	via;
	Uxwin	*win;		/* nil for app-wide events */
	Uxctl	*ctl;		/* nil unless a control's event */
	char	*action;	/* UXaction */
	char	*item;		/* icon item id */
	int	n;		/* list item number */
	char	*text;		/* new name, chosen path; drop type */
	Point	pt;
	int	drag;		/* UXdrop, UXdragend: the drag's id in /dev/ux/drag */
};

typedef void Uxfn(Uxev*, void*);

/* text field and general control flags */
enum {
	UXmultiline	= 1<<0,
	UXsecret	= 1<<1,
	UXsensitive	= 1<<2,
	UXreadonly	= 1<<3,
	UXsmall	= 1<<4,	/* label: the small font in grey, as in a window's info bar */
	UXright	= 1<<5,	/* label: right-aligned */
};

/* app */
void	uxinit(char *name, char *title);
void	uxmain(void);		/* runs the UI; returns when the app quits */
void	uxquit(void);		/* ends uxmain */
void	uxapphandler(Uxfn*, void*);	/* UXquit, and actions with no handler */
char*	uxappid(void);		/* malloc'd; uxd's ID, or name-pid */
void	uxsettitle(char*);

/* windows: the first is the rio window the app started in */
Uxwin*	uxwindow(char *kind, char *title, Rectangle r);
void	uxwinhandler(Uxwin*, Uxfn*, void*);
void	uxwinsettitle(Uxwin*, char*);
void	uxwinclose(Uxwin*);
void	uxwinfront(Uxwin*);	/* raise, show and focus it */
int	uxwinid(Uxwin*);
Rectangle	uxwinrect(Uxwin*);	/* the whole window, screen coordinates */
Point	uxwinsize(Uxwin*);	/* the drawable area: controls lie in 0,0 to this */
void	uxdefault(Uxwin*, Uxctl*);	/* the default button */
Uxwin*	uxdialog(char *title, Rectangle r);	/* a modal dialog window */

/* controls; r is window-local */
Uxctl*	uxbutton(Uxwin*, char *id, char *label, Rectangle r);
Uxctl*	uxtextfield(Uxwin*, char *id, char *label, Rectangle r, int flags);
Uxctl*	uxcheckbox(Uxwin*, char *id, char *label, Rectangle r);
Uxctl*	uxlabel(Uxwin*, char *id, char *text, Rectangle r);
void	uxlabelstyle(Uxctl*, int flags);	/* UXsmall, UXright */
Uxctl*	uxgroup(Uxwin*, char *id, char *label, Rectangle r);
Uxctl*	uxlist(Uxwin*, char *id, Rectangle r);
Uxctl*	uxiconview(Uxwin*, char *id, Rectangle r);
Uxctl*	uxscrollview(Uxwin*, char *id, Rectangle r);
Uxctl*	uxfilechooser(Uxwin*, char *id, char *dir, Rectangle r);
void	uxdelete(Uxctl*);
void	uxhandler(Uxctl*, Uxfn*, void*);
void	uxsetparent(Uxctl*, Uxctl*);
void	uxsetrect(Uxctl*, Rectangle);	/* window-local */

char*	uxctlid(Uxctl*);	/* not a copy; stable for the control's life */
char*	uxgetvalue(Uxctl*);	/* malloc'd: text, label value, on/off/mixed, chooser name */
void	uxsetvalue(Uxctl*, char*);
void	uxsetlabel(Uxctl*, char*);
void	uxenable(Uxctl*, int);
void	uxshow(Uxctl*, int);
void	uxfocus(Uxctl*);

/*
 * lists: an item may hold tab-separated columns; with columns set, the
 * list draws a header with their titles and widths, and no frame
 * inside a scroll view (the folder window look)
 */
void	uxsetitems(Uxctl*, char **items, int n);
void	uxsetcolumns(Uxctl*, char **titles, int *widths, int n);
void	uxsetitemicons(Uxctl*, char **icons, int n);	/* a 16 px icon per item */
int	uxgetsel(Uxctl*, int **sel);	/* malloc'd array */

/* icon views */
void	uxiconadd(Uxctl*, char *itemid, char *label, char *icon, Point pt);
void	uxicondel(Uxctl*, char *itemid);
void	uxiconclear(Uxctl*);
void	uxiconset(Uxctl*, char *itemid, char *label, char *icon, Point pt);	/* nil label or icon: unchanged */
int	uxicongetsel(Uxctl*, char ***ids);	/* malloc'd array of malloc'd IDs */
void	uxiconselect(Uxctl*, char **ids, int n);
void	uxiconopen(Uxctl*, char *itemid, int open);	/* its window is open: hatch it */

/*
 * scroll views: children are laid out in content coordinates (the
 * window-local place they'd have with the origin at 0,0) and drawn
 * moved by the origin, clipped to the view. uxscrollbars picks the bars
 * (shown whether or not there's anything to scroll, as in a classic
 * folder window; vertical only by default) and a header: that many
 * pixels at the top stay put, left of the vertical bar, for the app's
 * own controls. A view whose rectangle is its window's, outset by 1,
 * puts the bars against the window's frame.
 */
enum {
	UXvbar	= 1<<0,
	UXhbar	= 1<<1,
	UXgrowbox	= 1<<2,	/* the grow box, which resizes the window */
};
void	uxscrollbars(Uxctl*, int bars, int header);
Rectangle	uxscrollport(Uxctl*);	/* the visible content, window-local */
void	uxsetextent(Uxctl*, Rectangle);
void	uxsetorigin(Uxctl*, Point);
Point	uxgetorigin(Uxctl*);

/* file chooser */
char*	uxchooserdir(Uxctl*);		/* malloc'd */

/* menus, shown in rio's menu bar through /dev/ux and served under menus/ */
Uxmenu*	uxmenu(char *id, char *title);
void	uxitem(Uxmenu*, char *action, char *label, char *key);
void	uxsep(Uxmenu*, char *id);
void	uxitemenable(char *action, int);
void	uxitemcheck(char *action, int);
void	uxaction(char *action, Uxfn*, void*);

/*
 * resources, read through resfs (docs/protocols/res.md): the app's
 * resource file, then the system's (/lib/ux/system.rsrc). Icon names
 * in icon views may name an icon family or give its resource ID.
 */
int	uxres(char *file);	/* a resource file or resfs directory; nil: the default */
char*	uxresstr(int id, int n);	/* malloc'd: string n of STR# id, or STR  id if n is 0 */
Uxmenu*	uxresmenu(int id);	/* the menu in MENU id */
Uxwin*	uxresdialog(int id, Uxfn*, void*);	/* DLOG id and its DITL; fn gets every event */
Uxwin*	uxresalert(int id, Uxfn*, void*);	/* ALRT id and its DITL */
void	uxresparam(int n, char *s);	/* ^n in dialog text, n 0 to 3 */
int	uxresitem(Uxctl*);	/* the DITL item number a control came from, or 0 */
Uxctl*	uxresctl(Uxwin*, int n);	/* the control made from DITL item n, or nil */

/*
 * drag and drop, through rio's /dev/ux/drag (drag.c). A source builds a
 * Uxdrag and starts it while a mouse button is down; the control then
 * gets UXdragend. A drop target gets UXdrop, and fetches the payload
 * from its handler; libux tells rio the drop is done when the handler
 * returns. Payloads always travel through rio, never as paths.
 */
typedef struct Uxdrag Uxdrag;
typedef struct Uxdropfile Uxdropfile;
#pragma incomplete Uxdrag

struct Uxdropfile {
	char	*name;		/* a name, never a path */
	vlong	size;		/* -1 if unknown */
	int	dir;
	ulong	mode;		/* permission bits */
};

typedef void Uxdragfn(Uxdrag*, char **items, int nitems, void *arg);

Uxdrag*	uxdragnew(char *ops, char *label);	/* ops: "copy,move"; nil: copy */
void	uxdragops(Uxdrag*, char *ops);
void	uxdragdata(Uxdrag*, char *type, void *data, long n);
void	uxdragfile(Uxdrag*, char *path);	/* offered as type "file" */
int	uxdragstart(Uxdrag*, Uxctl *src);	/* -1 and errstr on failure; frees the Uxdrag either way */

/*
 * Icon views start drags themselves when selected icons leave the view:
 * the offer is "ux.icon" (the items), plus whatever fn adds, such as
 * uxdragfile for each item's file. fn runs inside libux: it may only
 * call uxdragdata, uxdragfile and uxdragops.
 */
void	uxicondrag(Uxctl *iconview, Uxdragfn *fn, void *arg);

void	uxdroptypes(Uxctl*, char *types, char *ops);	/* on an icon view, UXdrop's Uxev.item is the icon under the pointer */	/* "file,text", "copy"; nil types: no longer a target */
char*	uxdropdata(Uxev*, char *type, int item, long *n);	/* malloc'd, NUL-terminated; item -1: the whole type */
int	uxdropfiles(Uxev*, Uxdropfile**);	/* the "file" list; returns how many */
void	uxdropfilesfree(Uxdropfile*, int);
int	uxdropsave(Uxev*, int item, char *path, ulong mode);	/* file item into a new file */
void	uxdropresult(Uxev*, char *op, char *err);	/* what the drop did, if not the op offered */
int	uxdragfromself(Uxev*);	/* the drop came from this app */
char**	uxdragpaths(Uxev*, int *n);	/* and if so, its files' paths (malloc'd) */

/*
 * The clipboard (clip.c): clipd's /mnt/clip when it is mounted (several
 * types per copy; docs/protocols/clip.md), rio's /dev/snarf otherwise
 * (text only). Either way, text copied here is what legacy programs
 * paste, and what they copy is what uxclipget("text") returns.
 */
char*	uxclipget(char *type, long *n);	/* malloc'd, NUL-terminated; nil if not held */
int	uxclipput(char *type, void *data, long n);	/* the clipboard becomes this one type */
int	uxclipputn(char **types, char **data, long *n, int ntypes);	/* several types of one copy */
char**	uxcliptypes(int *n);	/* the types held now, richest first as copied; malloc'd */
int	uxedit(char *cmd);	/* cut, copy, paste, selectall on the front window's focused text field */
