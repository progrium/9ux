/*
 * Shared code for the 9/UX protocol fakes: event subscriptions,
 * message formatting and command parsing, following
 * docs/protocols/README.md.
 */

typedef struct Sub Sub;
typedef struct Msg Msg;
typedef struct Cmd Cmd;

enum {
	Maxtok = 64,
	Maxmsg = 8192,
};

/* One open of an events file: its own bounded queue. */
struct Sub {
	char	*topic;		/* which events it wants */
	char	**q;		/* queued messages, each ending in \n */
	int	nq;
	int	max;		/* queue bound */
	int	gone;		/* `gone` queued; EOF after the queue drains */
	Req	**wait;		/* blocked reads, oldest first */
	int	nwait;
	Sub	*next;
};

/* A message under construction: "verb tok tok attr=val ..." */
struct Msg {
	Fmt	f;
	int	started;
};

/* A parsed command line. */
struct Cmd {
	char	*buf;
	char	*tok[Maxtok];
	int	ntok;		/* all tokens */
	int	npos;		/* leading tokens without '=' (verb included) */
};

extern int	qmax;		/* default queue bound */
extern vlong	seq;		/* server's sequence counter */

/* subscriptions */
Sub*	subopen(char *topic);
void	subclose(Sub*);
void	subread(Sub*, Req*);
int	subflush(Req *old);
void	emit(char **topics, int ntopic, char *msg);
void	emit1(char *topic, char *msg);
void	emit1nl(Sub*, char *msg);
void	subgone(char *topic);

/* messages: mbegin(&m, "focus"); mattr(&m, "win", "%d", 7); s = mend(&m); */
void	mbegin(Msg*, char *verb);
void	mtok(Msg*, char *fmt, ...);
void	mattr(Msg*, char *name, char *fmt, ...);
char*	mend(Msg*);
char*	tokq(char*);		/* quoted only if needed; malloced */
char*	attrq(char *name, char *val);	/* tokq(name=val); malloced */		/* malloced, no newline */

/* commands */
int	cmdparse(Cmd*, char *line);	/* 0: blank line */
void	cmdfree(Cmd*);
char*	cmdattr(Cmd*, char *name);	/* nil if absent */
int	cmdattrok(Cmd*, char **names);	/* all attrs named in the nil-terminated list */
int	isid(char*);
int	isbool(char*);
int	parserect(char*, Rectangle*);
char*	fmtrect(Rectangle);

/* splitting a write into lines; calls fn per line, stops at first error */
char*	eachline(char *data, long n, char *(*fn)(char*, void*), void *arg);

void	fakeinit(void);
