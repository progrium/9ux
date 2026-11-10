/*
 * dcore: Desktop's core, everything in Desktop that isn't drawing.
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

typedef struct Dcattr Dcattr;
typedef struct Dcrec Dcrec;
typedef struct Dcmeta Dcmeta;
typedef struct Dccaps Dccaps;
typedef struct Dcvol Dcvol;
typedef struct Dcws Dcws;
typedef struct Dcfolder Dcfolder;
typedef struct Dcchange Dcchange;
typedef struct Dcinfo Dcinfo;
typedef struct Dctrashitem Dctrashitem;

/* metadata records (meta.c), as uxmeta serves them: ndb text */
struct Dcattr
{
	char	*attr;
	char	*val;
};

struct Dcrec
{
	Dcattr	*a;	/* a[0] is kind=key */
	int	na;
	Dcrec	*next;
};

struct Dcmeta
{
	char	*root;	/* where uxmeta is mounted, usually /mnt/meta */
	vlong	gen;	/* generation of the last query */
};

Dcmeta*	dcmetaopen(char *root);
void	dcmetaclose(Dcmeta*);
Dcrec*	dcmetaquery(Dcmeta*, char *fmt, ...);
int	dcmetactl(Dcmeta*, char *fmt, ...);
vlong	dcmetagen(Dcmeta*);
Dcrec*	dcparserecs(char *text, vlong *gen);
char*	dcrecget(Dcrec*, char *attr);
char*	dcreckey(Dcrec*);
void	dcrecfree(Dcrec*);

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
int	dcerrclass(char *err);
char*	dcerrname(int class);

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
struct Dccaps
{
	char	c[Nkind];
	char	why[Nkind][ERRMAX];	/* the server's error when c is Cno */
};
char*	dccapname(int kind);
char*	dccapval(int val);
void	dccapnote(Dccaps*, int kind, int ok, char *err);
int	dcprobe(Dcvol*, char *dir);

/* volumes and the workspace (vol.c) */
enum
{
	Vunknown,
	Vonline,
	Voffline,
};
struct Dcvol
{
	char	*name;
	char	*path;		/* mount point in the session namespace */
	char	*icon;
	char	*addr;		/* dial string Desktop attaches, or nil */
	char	*srv;		/* /srv file Desktop attaches, or nil */
	char	*spec;		/* attach name */
	int	desktop;
	int	x, y;
	int	state;
	char	err[ERRMAX];
	int	attached;	/* Desktop mounted it */
	int	haveid;		/* identity of the root when last online */
	ushort	type;
	uint	dev;
	Qid	qid;
	Dccaps	caps;
	int	seen;		/* still in the registry (dcwsload) */
};

struct Dcws
{
	char	*name;		/* workspace name, carried by launches */
	Dcmeta	*meta;		/* nil: no uxmeta; volumes added by hand */
	Dcvol	**vol;
	int	nvol;
	vlong	gen;		/* meta generation volumes were loaded at */
	int	(*progress)(void *arg, char *path, vlong bytes);	/* nonzero cancels */
	void	*progarg;
};

Dcws*	dcwsopen(char *name, Dcmeta*);
void	dcwsclose(Dcws*);
int	dcwsload(Dcws*);
Dcvol*	dcwsaddvol(Dcws*, char *name, char *path);
Dcvol*	dcvolbyname(Dcws*, char *name);
Dcvol*	dcvolof(Dcws*, char *path);
char*	dcvolstate(int state);
int	dcvolcheck(Dcvol*);
int	dcvolattach(Dcvol*);
int	dcvoldetach(Dcvol*);
char*	dcresolve(Dcws*, char *arg);

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
struct Dcfolder
{
	char	*path;
	Dir	*d;		/* entries, sorted by name */
	long	n;
	Dir	*self;		/* the directory's own stat */
	ulong	scans;
	char	err[ERRMAX];
};
struct Dcchange
{
	int	op;
	char	*name;
	char	*old;		/* Cren: the previous name */
};

Dcfolder*	dcfolderopen(char *path);
void	dcfolderfree(Dcfolder*);
int	dcrescan(Dcfolder*, Dcchange **chg);
void	dcchangefree(Dcchange*, int n);
char*	dcchangename(int op);
Dir*	dcfind(Dcfolder*, char *name);
Dir**	dcsorted(Dcfolder*, int how);
int	dcsortby(char *name);

/* file operations (ops.c) */
enum
{
	Fcopydel	= 1<<0,	/* allow copy and delete where rename can't */
	Frecursive	= 1<<1,	/* delete folders with what's in them */
};
int	dcrename(Dcws*, char *path, char *newname, int flags);
int	dcmove(Dcws*, char *path, char *dstdir, int flags);
char*	dccopy(Dcws*, char *path, char *dstdir, char *newname);
char*	dcduplicate(Dcws*, char *path);
int	dcdelete(Dcws*, char *path, int flags);
char*	dcmkdir(Dcws*, char *dir, char *name);
int	dcchmod(Dcws*, char *path, ulong mode);
int	dcvalidname(char *name);
char*	dcjoin(char *dir, char *name);
char*	dcdirname(char *path);
char*	dcbasename(char *path);

/* Get Info (info.c) */
struct Dcinfo
{
	char	*path;
	char	*name;
	char	*where;		/* the folder it's in */
	Dcvol	*vol;
	Dir	*d;
	char	*kind;		/* folder, volume, application, a type's desc, or document */
	Dcrec	*assoc;		/* uxmeta's resolve record, or nil */
	int	writable;
	vlong	size;		/* bytes; for a folder, of everything in it when deep */
	long	items;		/* for a folder: entries in it, or below it when deep */
	int	complete;	/* deep count finished within its limit */
};
Dcinfo*	dcinfo(Dcws*, char *path, int deep);
void	dcinfofree(Dcinfo*);

/* opening and launching (launch.c) */
enum
{
	Ofolder,	/* a folder: Desktop opens it itself */
	Oplumbed,	/* sent to the plumber */
	Olaunched,
};
int	dcopen(Dcws*, char *path, char **dst);
int	dcopenwith(Dcws*, char *path, char *app);
int	dclaunch(Dcws*, char *app, char *dir, char **argv);

/* Trash, one per volume (trash.c) */
struct Dctrashitem
{
	char	*path;		/* where it is now, in the Trash */
	char	*name;
	char	*origin;	/* where it came from; nil if unknown */
	long	date;		/* when it was trashed */
	Dcvol	*vol;
};
char*	dctrash(Dcws*, char *path, int flags);	/* returns where it went */
long	dctrashlist(Dcws*, Dcvol*, Dctrashitem**);	/* oldest first */
void	dctrashfree(Dctrashitem*, long);
char*	dcputback(Dcws*, char *item);		/* returns where it went back to */
int	dcemptytrash(Dcws*, Dcvol*);
char*	dctrashlimit(Dcws*, Dcvol*);	/* a sentence for the UI, malloc'd */
char*	dctrashdir(Dcvol*);
int	dcintrash(Dcws*, char *path);
