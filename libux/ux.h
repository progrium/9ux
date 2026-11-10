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
	UXselect,	/* selection changed */
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
	char	*text;		/* new name, chosen path */
	Point	pt;
};

typedef void Uxfn(Uxev*, void*);

/* text field and general control flags */
enum {
	UXmultiline	= 1<<0,
	UXsecret	= 1<<1,
	UXsensitive	= 1<<2,
	UXreadonly	= 1<<3,
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

/* lists */
void	uxsetitems(Uxctl*, char **items, int n);
int	uxgetsel(Uxctl*, int **sel);	/* malloc'd array */

/* icon views */
void	uxiconadd(Uxctl*, char *itemid, char *label, char *icon, Point pt);
void	uxicondel(Uxctl*, char *itemid);
void	uxiconclear(Uxctl*);
void	uxiconset(Uxctl*, char *itemid, char *label, char *icon, Point pt);	/* nil label or icon: unchanged */
int	uxicongetsel(Uxctl*, char ***ids);	/* malloc'd array of malloc'd IDs */
void	uxiconselect(Uxctl*, char **ids, int n);

/* scroll views */
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
