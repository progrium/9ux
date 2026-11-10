/*
 * fcore: Finder's core, everything in Finder that isn't drawing.
 *
 * Volumes come from an explicit registry (uxmeta's volume records),
 * never from guessing at the namespace. Folders are directories, read
 * and compared by rescanning. Operations ask the file server and report
 * what it actually did: an operation the server doesn't support fails
 * with "not supported", and the copy-and-delete fallback only runs when
 * the caller asks for it (Fcopydel).
 *
 * Errors are set with werrstr and start with one of the phrases in
 * README.md (Errors), so callers match on the phrase, not the detail.
 */

typedef struct Fcattr Fcattr;
typedef struct Fcrec Fcrec;
typedef struct Fcmeta Fcmeta;
typedef struct Fccaps Fccaps;
typedef struct Fcvol Fcvol;
typedef struct Fcws Fcws;
typedef struct Fcfolder Fcfolder;
typedef struct Fcchange Fcchange;
typedef struct Fcinfo Fcinfo;

/* metadata records (meta.c), as uxmeta serves them: ndb text */
struct Fcattr
{
	char	*attr;
	char	*val;
};

struct Fcrec
{
	Fcattr	*a;	/* a[0] is kind=key */
	int	na;
	Fcrec	*next;
};

struct Fcmeta
{
	char	*root;	/* where uxmeta is mounted, usually /mnt/meta */
	vlong	gen;	/* generation of the last query */
};

Fcmeta*	fcmetaopen(char *root);
void	fcmetaclose(Fcmeta*);
Fcrec*	fcmetaquery(Fcmeta*, char *fmt, ...);
int	fcmetactl(Fcmeta*, char *fmt, ...);
vlong	fcmetagen(Fcmeta*);
Fcrec*	fcparserecs(char *text, vlong *gen);
char*	fcrecget(Fcrec*, char *attr);
char*	fcreckey(Fcrec*);
void	fcrecfree(Fcrec*);

/* errors (caps.c) */
enum
{
	Eother,
	Eperm,		/* permission denied: the server supports it, the user may not */
	Enotsup,	/* the server doesn't do this at all */
	Egone,		/* the server or its connection went away */
	Enotfound,
	Eexist,
};
int	fcerrclass(char *err);
char*	fcerrname(int class);

/* what a volume's server supports, learned by probing or by use */
enum
{
	Cunknown,
	Cyes,
	Cno,
};
enum
{
	Kread,
	Kcreate,
	Kmkdir,
	Kwrite,
	Kremove,
	Krename,
	Kchmod,
	Kmtime,
	Nkind,
};
struct Fccaps
{
	char	c[Nkind];
	char	why[Nkind][ERRMAX];	/* the server's error when c is Cno */
};
char*	fccapname(int kind);
char*	fccapval(int val);
void	fccapnote(Fccaps*, int kind, int ok, char *err);
int	fcprobe(Fcvol*, char *dir);

/* volumes and the workspace (vol.c) */
enum
{
	Vunknown,
	Vonline,
	Voffline,
};
struct Fcvol
{
	char	*name;
	char	*path;		/* mount point in the session namespace */
	char	*icon;
	char	*addr;		/* dial string Finder attaches, or nil */
	char	*srv;		/* /srv file Finder attaches, or nil */
	char	*spec;		/* attach name */
	int	desktop;
	int	x, y;
	int	state;
	char	err[ERRMAX];
	int	attached;	/* Finder mounted it */
	int	haveid;		/* identity of the root when last online */
	ushort	type;
	uint	dev;
	Qid	qid;
	Fccaps	caps;
	int	seen;		/* still in the registry (fcwsload) */
};

struct Fcws
{
	char	*name;		/* workspace name, carried by launches */
	Fcmeta	*meta;		/* nil: no uxmeta; volumes added by hand */
	Fcvol	**vol;
	int	nvol;
	vlong	gen;		/* meta generation volumes were loaded at */
	int	(*progress)(void *arg, char *path, vlong bytes);	/* nonzero cancels */
	void	*progarg;
};

Fcws*	fcwsopen(char *name, Fcmeta*);
void	fcwsclose(Fcws*);
int	fcwsload(Fcws*);
Fcvol*	fcwsaddvol(Fcws*, char *name, char *path);
Fcvol*	fcvolbyname(Fcws*, char *name);
Fcvol*	fcvolof(Fcws*, char *path);
char*	fcvolstate(int state);
int	fcvolcheck(Fcvol*);
int	fcvolattach(Fcvol*);
int	fcvoldetach(Fcvol*);
char*	fcresolve(Fcws*, char *arg);

/* folders (folder.c) */
enum
{
	Cadd,
	Cdel,
	Cmod,
	Cren,
};
enum
{
	Sname,
	Skind,
	Sdate,
	Ssize,
};
struct Fcfolder
{
	char	*path;
	Dir	*d;		/* entries, sorted by name */
	long	n;
	Dir	*self;		/* the directory's own stat */
	ulong	scans;
	char	err[ERRMAX];
};
struct Fcchange
{
	int	op;
	char	*name;
	char	*old;		/* Cren: the previous name */
};

Fcfolder*	fcfolderopen(char *path);
void	fcfolderfree(Fcfolder*);
int	fcrescan(Fcfolder*, Fcchange **chg);
void	fcchangefree(Fcchange*, int n);
char*	fcchangename(int op);
Dir*	fcfind(Fcfolder*, char *name);
Dir**	fcsorted(Fcfolder*, int how);
int	fcsortby(char *name);

/* file operations (ops.c) */
enum
{
	Fcopydel	= 1<<0,	/* allow copy and delete where rename can't */
	Frecursive	= 1<<1,	/* delete folders with what's in them */
};
int	fcrename(Fcws*, char *path, char *newname, int flags);
int	fcmove(Fcws*, char *path, char *dstdir, int flags);
char*	fccopy(Fcws*, char *path, char *dstdir, char *newname);
char*	fcduplicate(Fcws*, char *path);
int	fcdelete(Fcws*, char *path, int flags);
char*	fcmkdir(Fcws*, char *dir, char *name);
int	fcchmod(Fcws*, char *path, ulong mode);
int	fcvalidname(char *name);
char*	fcjoin(char *dir, char *name);
char*	fcdirname(char *path);
char*	fcbasename(char *path);

/* Get Info (info.c) */
struct Fcinfo
{
	char	*path;
	char	*name;
	char	*where;		/* the folder it's in */
	Fcvol	*vol;
	Dir	*d;
	char	*kind;		/* folder, volume, application, a type's desc, or document */
	Fcrec	*assoc;		/* uxmeta's resolve record, or nil */
	int	writable;
	vlong	size;		/* bytes; for a folder, of everything in it when deep */
	long	items;		/* for a folder: entries in it, or below it when deep */
	int	complete;	/* deep count finished within its limit */
};
Fcinfo*	fcinfo(Fcws*, char *path, int deep);
void	fcinfofree(Fcinfo*);

/* opening and launching (launch.c) */
enum
{
	Ofolder,	/* a folder: Finder opens it itself */
	Oplumbed,	/* sent to the plumber */
	Olaunched,
};
int	fcopen(Fcws*, char *path, char **dst);
int	fcopenwith(Fcws*, char *path, char *app);
int	fclaunch(Fcws*, char *app, char *dir, char **argv);
