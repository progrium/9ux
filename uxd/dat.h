/*
 * uxd: the UI registry and broker (docs/protocols/ux.md).
 *
 * Concurrency model. lib9p's server procs (one per posted service) do
 * nothing but hand each request to the main proc over reqchan. Every
 * request then runs as its own thread in the main proc, so all of
 * uxd's state is touched by one proc and needs no locks: threads only
 * switch when one blocks on a channel. Talking to an app's server goes
 * through a Conn, whose reader and writer procs do the blocking I/O,
 * so a stalled app stalls only the requests waiting on it.
 */

typedef struct App App;
typedef struct Att Att;
typedef struct Conn Conn;
typedef struct Fx Fx;
typedef struct Grant Grant;
typedef struct Help Help;
typedef struct Msg Msg;
typedef struct Op Op;
typedef struct Reg Reg;
typedef struct Rpc Rpc;
typedef struct Sub Sub;

enum {
	Msize = 8192+IOHDRSZ,
	Stack = 32*1024,
	Qmax = 256,		/* messages queued per events subscription */
	Maxvalue = 1024,	/* largest value carried in an event */
};

/* rights (ux.md, Rights) */
enum {
	Rsread		= 1<<0,	/* Rread is taken by fcall.h */
	Rsensitive	= 1<<1,
	Ractivate	= 1<<2,
	Redit		= 1<<3,
	Rkeys		= 1<<4,
	Rclipboard	= 1<<5,
	Radmin		= 1<<6,
	Rall		= (1<<7)-1,
};

/* uxd's own files; everything inside an app's tree is Qpass */
enum {
	Qroot,
	Qversion,
	Qrights,
	Qregister,
	Qwindows,
	Qevents,
	Qapps,
	Qgrants,
	Qgnew,
	Qglist,
	Qgctl,
	Qpass,
};

/* a message from a Conn's reader or writer proc */
enum {
	Mrx,		/* a reply from the app */
	Mhangup,	/* the reader stopped: the app hung up */
	Mwdone,		/* the writer stopped */
};
struct Msg {
	int	kind;
	Conn	*c;
	Fcall	f;
	uchar	*buf;
};

/* an RPC to an app in flight */
struct Rpc {
	Conn	*c;
	int	tag;
	Channel	*ch;		/* nil: nobody waits; free on reply */
	Msg	*rx;
	char	*err;
	int	done;
	Rpc	*flush;		/* the Tflush sent for this one */
	Rpc	*old;		/* for a Tflush: what it flushes */
	int	oldtag;
};

/* a 9P client connection to one app's server */
struct Conn {
	int	fd;
	int	ref;
	int	dead;		/* the reader has stopped */
	int	closing;	/* we are done with it */
	int	wdead;		/* a write failed (writer proc only) */
	int	readerid;
	Channel	*wc;		/* uchar* messages for the writer; nil stops it */
	Rpc	*tags[256];
	Rpc	*vrpc;		/* the Tversion in flight (NOTAG) */
	ulong	nextfid;
	App	*app;
};

/* the state of one request being served */
struct Op {
	Req	*r;
	Rpc	*rpc;		/* the app RPC it waits on, if any */
	void	(*cancel)(Op*);	/* undoes a local wait, if any */
	void	*waiton;	/* what cancel needs */
	int	flushed;
	void	*freeme;	/* freed after the reply */
	Msg	*msg;		/* freed after the reply */
	char	err[ERRMAX];
};

/* how a fid attached: what the client may do */
struct Att {
	int	ref;
	int	admin;		/* attached to the session service */
	Grant	*g;		/* the grant whose token was the aname */
};

struct Grant {
	Grant	*next;
	int	ref;
	char	token[33];
	int	rights;
	char	*app;		/* pattern, or nil for every app */
	char	*note;
	int	revoked;
};

/* the verb -> right table of one ctl file, from its help */
struct Help {
	Help	*next;
	char	*path;
	int	n;
	char	**verb;
	int	*right;		/* -1: refuse */
};

/* one events subscription on uxd's own events file */
struct Sub {
	Sub	*next;
	char	*q[Qmax];
	int	nq;
	Req	**rq;		/* blocked reads */
	int	nrq;
};

/* a registration: one open of register */
struct Reg {
	App	*app;
	int	idsent;
	int	ended;
	Req	**rq;		/* reads waiting for the end */
	int	nrq;
};

struct App {
	App	*next;
	int	ref;
	int	gone;
	int	serial;
	char	*id;
	char	*name;
	char	*title;
	Conn	*c;
	ulong	rootfid;
	Qid	rootqid;
	Reg	*reg;
	int	*win;
	int	nwin;
	Help	*help;
};

/* per-fid state */
struct Fx {
	int	q;
	Att	*att;
	Qid	qid;
	int	open;
	int	omode;

	/* Qpass */
	App	*app;
	long	afid;		/* fid on app->c, -1 if none */
	char	*path;		/* below the app's root; "" at the root */
	int	depth;
	int	isevents;
	int	redact;		/* events: strip sensitive values */
	char	*pend;		/* redacted events not yet read */
	long	npend;

	/* snapshot of a local state file */
	char	*buf;
	long	nbuf;

	Sub	*sub;		/* Qevents */
	Reg	*reg;		/* Qregister */
	Grant	*newg;		/* Qgnew, after the grant command */
};

extern Channel	*rxchan;
extern App	*apps;
extern Grant	*grants;
extern char	*user;
extern char	*grantfile;
extern ulong	starttime;
