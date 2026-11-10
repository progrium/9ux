/*
 * Desktop, the app: the desktop and folder windows on libux, over the
 * core library in dcore.h. See README.md (The Desktop app).
 *
 * libux runs our handlers on its UI thread. They only copy the event
 * and send it to Desktop's own proc (main.c), which does everything
 * else: file system calls, uxmeta queries and libux updates. So a slow
 * or vanished file server stalls Desktop's proc, never the drawing.
 * Desktop's state below is touched only by that proc.
 */

typedef struct Dev Dev;
typedef struct Item Item;
typedef struct View View;
typedef struct Alert Alert;

/* View.kind */
enum
{
	Kdesk,
	Kfolder,
	Kinfo,
	Ktrash,		/* the Trash window: every volume's Trash */
};

/* View.mode: how a folder window shows its items */
enum
{
	Micons,
	Mlist,
};

/* an event from libux, copied for Desktop's proc */
struct Dev
{
	int	type;
	int	via;
	Uxwin	*win;
	Uxctl	*ctl;
	char	*action;
	char	*item;
	char	*text;
	int	n;
	Point	pt;
	View	*v;	/* the view the handler was registered for */
	Alert	*a;	/* or the alert */

	/* UXdrop of our own drag: where its files were, and where they go */
	char	**paths;
	int	npaths;
	char	*dest;	/* a folder, "trash", or nil: nowhere */
};

/* one icon in a view: a folder entry, or a volume on the desktop */
struct Item
{
	char	*id;	/* icon item ID in the icon view: i1, i2, ... */
	char	*name;	/* entry name, or volume name */
	char	*path;
	char	*icon;
	int	dir;
	Point	pt;	/* the icon's centre in the view */
	int	placed;	/* pt came from uxmeta (or the user), not auto placement */
	Dcvol	*vol;	/* desktop volumes */
	int	offline;
	int	trash;	/* the desktop's Trash */
};

struct View
{
	int	kind;
	Uxwin	*w;
	char	*path;		/* the folder (or the file, for Get Info); nil for the desktop */
	char	*title;
	Rectangle	r;	/* last known window rectangle, screen coordinates */
	Point	size;		/* last known drawable size */

	/* desktop and folder windows */
	Uxctl	*icons;		/* icon view (Micons, and the desktop) */
	Uxctl	*list;		/* list (Mlist) */
	Uxctl	*scroll;	/* folder and Trash windows: the scroll view with both bars */
	Uxctl	*status;	/* a label: items, selection, read-only, offline */
	Uxctl	*space;	/* a label at the right of the info bar: the size of what's there */
	int	look;	/* the bars and info bar folderlayout last set, 0 for none yet */
	Uxctl	*edit;		/* the rename field, while renaming */
	Item	*editing;
	int	mode;
	int	sort;
	Dcfolder	*f;
	Dcvol	*vol;
	Item	**item;
	int	nitem;
	int	nextid;
	char	**rows;		/* Mlist: item for each list row, by ID */
	int	nrows;
	char	err[ERRMAX];	/* why the folder can't be read, or "" */

	/* Get Info */
	Uxctl	*name;
	Qid	qid;		/* the file when last shown */
	long	mtime;
	int	gone;

	View	*next;
};

/* a modal question or error */
struct Alert
{
	Uxwin	*w;
	void	(*fn)(Alert*, int choice);	/* choice: 0 cancel, 1 default, 2 the other button */
	char	*path;		/* what it's about */
	char	*arg;
	View	*v;
	Alert	*next;
};

/*
 * itemlk guards each view's item list and each item's path, which
 * libux's UI thread reads while starting a drag or taking a drop.
 * Desktop's proc holds it only while changing them, never across a
 * libux call.
 */
extern Lock	itemlk;

/* main.c */
extern Dcws	*ws;
extern Dcmeta	*meta;
extern View	*views;
extern View	*desk;
extern View	*front;
extern int	nodesk;
void	setfront(View*);
void	updatemenus(void);
void	viewhandler(Uxev*, void*);
void	alerthandler(Uxev*, void*);

/* view.c */
View*	deskopen(void);
void	deskload(void);
void	deskcheck(void);
View*	folderopen(char *path, int remember);
View*	viewbypath(char *path);
View*	viewbywin(Uxwin*);
void	viewclose(View*, int remember);
void	viewlayout(View*);
void	viewrescan(View*);
void	viewsetmode(View*, int mode);
void	viewsetsort(View*, int sort);
void	viewcleanup(View*);
void	viewstatus(View*);
void	folderchrome(View*);
void	folderlayout(View*);
Point	viewport(View*);
Rectangle	viewcontent(View*);
void	viewevent(View*, Dev*);
void	openit(View*, Item*);
int	viewsel(View*, Item ***items);
void	viewselectall(View*);
void	viewselectname(View*, char *name);
Item*	itembyid(View*, char *id);
Item*	itembyname(View*, char *name);
void	itemrename(View*, Item*, char *newname);
void	renamestart(View*);
void	renameend(View*, int commit);
void	metachange(char *line);
char*	viewlabel(Item*);
void	itemsetpath(Item*, char*);
void	viewdrop(View*, Dev*);
void	dragsetup(View*);
void	droplookup(View*, Uxev*, Dev*);

/* trashwin.c */
View*	trashopen(void);
void	trashrefresh(View*);
void	trashcheck(void);
void	trashitems(View*);
void	trashstatus(View*);
void	totrash(View*, Item*);
void	putback(View*, Item*);
void	emptytrash(void);
Point	trashpt(View*);

/* info.c */
View*	infoopen(char *path);
void	infoevent(View*, Dev*);
void	inforefresh(View*);

/* alert.c */
Alert*	alert(char *msg, char *def, char *other, void (*fn)(Alert*, int));
void	alertevent(Alert*, Dev*);
Alert*	alertbywin(Uxwin*);
void	alerterr(char *what);

/* layout.c */
Point	cellpt(int col, int row);
Point	freecell(View*);
void	savewin(View*);
void	saveitem(View*, Item*);
void	savevol(Item*);
void	forgetwin(View*);
Rectangle	winrect(char *path);
int	winmode(char *path, int *sort);
Dcrec*	itemrecs(char *dir);
Rectangle	cascade(void);

/* util (layout.c) */
void*	demalloc(ulong);
char*	destrdup(char*);
