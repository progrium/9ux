/* app.c */
int	isname(char*);
App*	lookapp(char*);
App*	winowner(int);
void	appincref(App*);
void	appdecref(App*);
void	unregister(App*);
void	apphungup(App*);
void	regclosed(Reg*);
char*	regwrite(Reg*, char*, Op*);
void	regread(Reg*, Op*);
char*	winlist(void);

/* conn.c */
Conn*	newconn(int);
void	closeconn(Conn*);
void	dispatch(Msg*);
void	freemsg(Msg*);
Msg*	rpc(Conn*, Fcall*, Op*);
void	flushrpc(Op*);
void	rpcasync(Conn*, Fcall*);
ulong	newfid(Conn*);
void	tclunk(Conn*, ulong);
Msg*	twalk(Conn*, ulong, ulong, int, char**, Op*);
Msg*	topen(Conn*, ulong, int, Op*);
Msg*	tread(Conn*, ulong, vlong, ulong, Op*);
Msg*	twrite(Conn*, ulong, vlong, ulong, char*, Op*);
Msg*	tstat(Conn*, ulong, Op*);
char*	readapp(Conn*, ulong, char*, Op*);

/* fs.c */
Qid	passqid(App*, Qid);
void	freefx(Fx*);
void	fsattach(Op*, int);
void	fswalk(Op*);
void	fsopen(Op*);
void	fsread(Op*);
void	fswrite(Op*);
void	fsstat(Op*);
void	fsflush(Op*);

/* grant.c */
int	rightbit(char*);
char*	rightstr(int, char*);
int	match(char*, char*);
int	rightsfor(Att*, App*);
int	attrights(Att*);
Grant*	lookgrant(char*);
char*	grantlist(void);
void	loadgrants(void);
char*	newgrant(char*, Grant**);
char*	grantctl(char*);

/* sub.c */
void	postevent(char*, ...);
Sub*	newsub(void);
void	subread(Sub*, Op*);
void	freesub(Sub*);

/* uxd.c */
void	reply(Op*, char*);
char*	readfile(char*);
void	mkdirs(char*);
char*	qtok(char*);

#pragma varargck argpos postevent 1
