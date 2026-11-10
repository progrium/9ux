/*
 * 9/UX additions to rio: /dev/ux (docs/protocols/devux.md).
 * Everything here runs in rio's main proc, among its threads, so the
 * state below needs no locks: threads only switch at channel operations.
 */

typedef struct Uxapp Uxapp;
typedef struct Uxfid Uxfid;
typedef struct Uxitem Uxitem;
typedef struct Uxmenu Uxmenu;
typedef struct Uxsub Uxsub;

enum
{
	/* event streams */
	Sux,		/* /dev/ux/events */
	Sdesk,		/* /dev/ux/desktop/events */
	Smenu,		/* /dev/ux/menubar/events */
	Nstream,

	Maxqueue	= 256,	/* messages per subscription */

	/* check= */
	Chkoff = 0,
	Chkon,
	Chkmixed,

	/* shortcut modifiers, besides Command */
	Mshift	= 1<<0,
	Malt	= 1<<1,
	Mctl	= 1<<2,
};

struct Uxsub
{
	int		kind;
	char		*q[Maxqueue];
	int		nq;
	Channel	*wake;	/* chan(int)[1] */
	int		dead;
	int		ref;
	Uxsub	*next;
};

struct Uxitem
{
	char		*action;
	char		*label;
	char		*key;	/* as given; nil for none */
	Rune		krune;
	int		kmods;
	int		enabled;
	int		check;
	int		sep;
	char		*sub;
	Uxitem	*next;
};

struct Uxmenu
{
	char		*id;
	char		*title;
	int		win;		/* window override; 0 for app-wide */
	int		top;
	Uxitem	*items;
	Uxmenu	*next;
};

struct Uxapp
{
	char		*id;
	char		*title;
	int		*wins;
	int		nwins;
	Uxmenu	*menus;
	int		batch;	/* inside begin ... commit */
	Uxmenu	*saved;	/* state at begin, for abort */
	char		*savedtitle;
	Uxapp	*next;
};

struct Uxfid
{
	char		*snap;	/* state file snapshot, built at open */
	int		nsnap;
	Uxsub	*sub;
	Uxapp	*app;	/* menubar/ctl registration session */
	int		claim;	/* holds the desktop claim */
};

/* ux.c */
extern int	uxmode;		/* 9/UX presentation and interception on */
extern Rune	uxcmdkey;	/* Command key rune; 0 for none */
extern int	uxbarh;		/* height of the menu bar strip */
extern Uxapp	*uxapps;

void		uxinit(int);
void		uxchanged(void);
void		uxpost(int, char*, ...);
int		uxisdesktop(Window*);
int		uxdesktopid(void);
Rectangle	uxdeskrect(void);
void		uxresized(void);
int		uxrefuse(Window*, Window*, int);
Window*	uxwin(int);
void		fmtattr(Fmt*, char*, char*);
char*	uxlabel(Window*);

/* wind.c */
void		wdeskbottom(void);

/* uxfs.c */
int		uxisqid(int);
int		uxwalk1(char*, Qid*, Dirtab**);
Dirtab*	uxdirtab(int);
void		uxopen(Xfid*);
void		uxread(Xfid*);
void		uxwrite(Xfid*);
void		uxclose(Xfid*);

/* uxmenu.c */
char*	uxmenuctl(Uxfid*, Window*, char*);
void		uxmenuclose(Uxfid*);
char*	uxmenusnap(int*);
Uxapp*	uxappof(int);
Uxapp*	uxfindapp(char*);
void		uxappdelwin(int);
char*	uxappid(int);
int		uxforeground(Uxapp**);
void		uxshortcut(Rune, int, int);

/* uxbar.c */
void		uxbarinit(void);
void		uxbarfree(void);
void		uxbardraw(void);
int		uxbarhit(Point);
void		uxbarmouse(void);
void		uxbarontop(void);
int		uxkbd(char*);

/* uxstyle.c */
#include "../assets/gen/uxstyle.h"
extern Font	*uxsysfont;
extern Image	*uxmark;
void		uxstyleinit(void);
Image*	uxcolor(ulong);
void		uxglyph(Image*, Point, char*, Image*);
int		uxglyphwidth(char*);
int		uxglyphheight(char*);
