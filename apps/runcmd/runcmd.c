/*
 * runcmd: Run Command, an rc shell in a 9/UX window, after A/UX's
 * CommandShell. The shell's text is a libux console in a scroll view
 * that fills the window: the body font instead of rio's fixed one, a
 * classic scroll bar and grow box, and 9/UX chrome.
 *
 * rc runs in its own namespace and note group with this program's
 * /dev/cons and /dev/consctl mounted before /dev, as vt(1) does, so
 * what it and its children read and write on the console goes through
 * the window. Delete interrupts it; when it exits, so does the window.
 *
 *	runcmd [-f font] [command ...]
 *
 * With a command, rc runs it (rc -c) instead of reading commands. The
 * font is the body font (Lucida Sans) unless -f gives another; rc gets
 * it as $font.
 */
#include <u.h>
#include <libc.h>
#include <draw.h>
#include <thread.h>
#include <fcall.h>
#include <9p.h>
#include <ux.h>

static Uxwin *win, *dlg;
static Uxctl *scroll, *text;
static char **cmdargv;
static char *fontname = "/lib/font/bit/lucidasans/unicode.7.font";	/* the body role, assets/fonts.ndb */
static int rcpid = -1;
static Channel *pidc;
static Channel *inputc;	/* char*: a line typed, or "" for end of file */
static Channel *readc;	/* Req*: a read of /dev/cons */
static Channel *flushc;	/* Req*: a flush */

/* the shell's console: /dev/cons and /dev/consctl */

static File *devcons, *devconsctl;

typedef struct Partial Partial;
struct Partial {
	int	n;
	char	s[UTFmax];
};

/*
 * Reads of /dev/cons wait here for what's typed: a read takes what's
 * left of the current line, or as much of it as fits. An empty line
 * (^D with nothing typed) answers one read with nothing: end of file.
 */
static void
consreader(void*)
{
	Req *r, *fr;
	char *s, *p;
	long n, m;

	threadsetname("consreader");
	r = nil;
	s = p = nil;
	for(;;){
		Alt a[] = {
			{ flushc, &fr, CHANRCV },
			{ readc, &r, r == nil ? CHANRCV : CHANNOP },
			{ inputc, &s, s == nil ? CHANRCV : CHANNOP },
			{ nil, nil, CHANEND },
		};
		if(alt(a) == 0){
			if(r != nil && fr->oldreq == r){
				respond(r, "interrupted");
				r = nil;
			}
			respond(fr, nil);
			continue;
		}
		if(r == nil || s == nil)
			continue;
		if(p == nil)
			p = s;
		n = strlen(p);
		m = r->ifcall.count;
		if(n > m){
			/* end the read at a rune's start, unless it's shorter than one */
			while(m > 0 && (p[m] & 0xC0) == 0x80)
				m--;
			n = m > 0 ? m : r->ifcall.count;
		}
		memmove(r->ofcall.data, p, n);
		r->ofcall.count = n;
		respond(r, nil);
		r = nil;
		p += n;
		if(*p == 0){
			free(s);
			s = p = nil;
		}
	}
}

static void
fsread(Req *r)
{
	if(r->fid->file == devcons){
		sendp(readc, r);
		return;
	}
	respond(r, "not implemented");
}

static void
fswrite(Req *r)
{
	Partial *u;
	char *b;
	long n, k, i;

	if(r->fid->file == devcons){
		if((u = r->fid->aux) == nil)
			u = r->fid->aux = emalloc9p(sizeof *u);
		/* a rune split across writes is kept for the next one */
		n = u->n + r->ifcall.count;
		b = emalloc9p(n);
		memmove(b, u->s, u->n);
		memmove(b+u->n, r->ifcall.data, r->ifcall.count);
		k = n;
		for(i = n-1; i >= 0 && i > n-UTFmax; i--)
			if((b[i] & 0xC0) != 0x80){
				if(!fullrune(b+i, n-i))
					k = i;
				break;
			}
		u->n = n-k;
		memmove(u->s, b+k, u->n);
		uxconswrite(text, b, k);
		free(b);
		r->ofcall.count = r->ifcall.count;
		respond(r, nil);
		return;
	}
	if(r->fid->file == devconsctl){
		n = r->ifcall.count;
		b = r->ifcall.data;
		if(n >= 5 && strncmp(b, "rawon", 5) == 0)
			uxconsraw(text, 1);
		else if(n >= 6 && strncmp(b, "rawoff", 6) == 0)
			uxconsraw(text, 0);
		r->ofcall.count = n;
		respond(r, nil);
		return;
	}
	respond(r, "not implemented");
}

static void
fsflush(Req *r)
{
	sendp(flushc, r);
}

static void
fsdestroyfid(Fid *f)
{
	/* closing consctl ends raw mode, as with rio */
	if(f->file == devconsctl && f->omode >= 0)
		uxconsraw(text, 0);
	free(f->aux);
	f->aux = nil;
}

static Srv fs = {
	.read = fsread,
	.write = fswrite,
	.flush = fsflush,
	.destroyfid = fsdestroyfid,
};

static void
fsproc(void*)
{
	threadsetname("consfs");
	threadsrv(&fs);
}

/* the console's file server, on one end of a pipe; returns the other end */
static int
startfs(void)
{
	int p[2];

	fs.tree = alloctree(nil, nil, DMDIR|0555, nil);
	devcons = createfile(fs.tree->root, "cons", nil, 0666, nil);
	devconsctl = createfile(fs.tree->root, "consctl", nil, 0666, nil);
	if(devcons == nil || devconsctl == nil)
		sysfatal("console files: %r");
	if(pipe(p) < 0)
		sysfatal("pipe: %r");
	fs.infd = fs.outfd = p[1];
	readc = chancreate(sizeof(Req*), 8);
	flushc = chancreate(sizeof(Req*), 8);
	inputc = chancreate(sizeof(char*), 256);
	proccreate(consreader, nil, 16*1024);
	proccreate(fsproc, nil, 32*1024);
	return p[0];
}

/* the shell: its own namespace, file descriptors and note group */

static int fsfd;

static void
runrc(void*)
{
	char *cmd, **a;
	int fd;

	threadsetname("rc");
	if(mount(fsfd, -1, "/dev", MBEFORE, "") < 0)
		sysfatal("mount console: %r");
	for(fd = 0; fd < 100; fd++)
		close(fd);
	open("/dev/cons", OREAD);
	open("/dev/cons", OWRITE);
	dup(1, 2);
	putenv("termprog", "runcmd");
	/* so mc(1), lc and the like lay out columns for the console's font */
	putenv("font", fontname);
	cmd = nil;
	for(a = cmdargv; *a != nil; a++){
		if(cmd == nil)
			cmd = smprint("%q", *a);
		else
			cmd = smprint("%s %q", cmd, *a);
	}
	if(cmd != nil)
		procexecl(pidc, "/bin/rc", "rc", "-c", cmd, nil);
	else
		procexecl(pidc, "/bin/rc", "rc", "-i", nil);
	fprint(2, "runcmd: can't run rc: %r\n");
	threadexits("exec");
}

/* the window goes when the shell does */
static void
waiter(void*)
{
	Channel *wc;
	Waitmsg *w;

	threadsetname("waiter");
	wc = threadwaitchan();
	while((w = recvp(wc)) != nil){
		if(w->pid == rcpid){
			free(w);
			rcpid = -1;
			uxquit();
			return;
		}
		free(w);
	}
}

static void
interrupt(void)
{
	if(rcpid > 0)
		postnote(PNGROUP, rcpid, "interrupt");
}

static void
quit(void)
{
	if(rcpid > 0)
		postnote(PNGROUP, rcpid, "hangup");
	uxquit();
}

/* New Window: another Run Command, in a new rio window beside this one */
static void
newwin(void)
{
	char *prog, dir[512];
	Rectangle r;

	prog = argv0;
	if(prog[0] != '/' && getwd(dir, sizeof dir) != nil)
		prog = smprint("%s/%s", dir, prog);
	else
		prog = strdup(prog);
	r = rectaddpt(uxwinrect(win), Pt(24, 24));
	switch(rfork(RFPROC|RFFDG|RFNOTEG|RFNOWAIT)){
	case -1:
		break;
	case 0:
		execl("/bin/window", "window", "-r",
			smprint("%d", r.min.x), smprint("%d", r.min.y),
			smprint("%d", r.max.x), smprint("%d", r.max.y),
			prog, nil);
		_exits("exec");
	}
	free(prog);
}

static void
dlgev(Uxev *e, void*)
{
	if(dlg == nil || e->win != dlg)
		return;
	switch(e->type){
	case UXactivate:
	case UXdefault:
	case UXcancel:
	case UXclose:
		uxwinclose(dlg);
		dlg = nil;
	}
}

static void
about(void)
{
	if(dlg != nil)
		return;
	dlg = uxresalert(128, dlgev, nil);
}

static void
layout(void)
{
	Point sz;

	sz = uxwinsize(win);
	/* the scroll view's frame is the window's: its bar runs down the right edge */
	uxsetrect(scroll, Rect(-1, -1, sz.x+1, sz.y+1));
}

static void
action(char *a)
{
	if(strcmp(a, "quit") == 0 || strcmp(a, "close") == 0)
		quit();
	else if(strcmp(a, "new-window") == 0)
		newwin();
	else if(strcmp(a, "about-run-command") == 0)
		about();
	else if(strcmp(a, "cut") == 0 || strcmp(a, "copy") == 0 || strcmp(a, "paste") == 0)
		uxedit(a);
	else if(strcmp(a, "select-all") == 0)
		uxedit("selectall");
	else if(strcmp(a, "interrupt") == 0)
		interrupt();
	else if(strcmp(a, "send-end-of-file") == 0)
		nbsendp(inputc, strdup(""));
	else if(strcmp(a, "clear-window") == 0)
		uxconsclear(text);
}

static void
appev(Uxev *e, void*)
{
	if(e->type == UXquit)
		quit();
	else if(e->type == UXaction)
		action(e->action);
}

static void
winev(Uxev *e, void *arg)
{
	USED(arg);
	switch(e->type){
	case UXresize:
		layout();
		break;
	case UXclose:
		quit();
		break;
	}
}

static void
textev(Uxev *e, void*)
{
	switch(e->type){
	case UXsubmit:
		/* what's typed goes to the shell's next read */
		if(nbsendp(inputc, strdup(e->text ? e->text : "")) == 0)
			fprint(2, "runcmd: input dropped\n");
		break;
	case UXinterrupt:
		interrupt();
		break;
	}
}

static void
usage(void)
{
	fprint(2, "usage: %s [-f font] [command ...]\n", argv0);
	threadexitsall("usage");
}

void
threadmain(int argc, char *argv[])
{
	Point sz;

	ARGBEGIN{
	case 'f':
		fontname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	cmdargv = argv;

	uxinit("runcmd", "Run Command");
	uxapphandler(appev, nil);
	if(uxres(nil) < 0)
		sysfatal("resources: %r");
	if(uxresmenu(128) == nil || uxresmenu(129) == nil || uxresmenu(130) == nil)
		sysfatal("menus: %r");

	win = uxwindow("window", "Run Command", Rect(0, 0, 560, 360));
	uxwinhandler(win, winev, nil);
	sz = uxwinsize(win);
	scroll = uxscrollview(win, "scroll", Rect(-1, -1, sz.x+1, sz.y+1));
	uxscrollbars(scroll, UXvbar|UXgrowbox, 0);
	text = uxconsole(win, "text", Rect(0, 0, sz.x, sz.y));
	uxsetparent(text, scroll);
	uxhandler(text, textev, nil);
	if(uxconsfont(text, fontname) < 0)
		sysfatal("can't open font %s: %r", fontname);
	uxfocus(text);

	fsfd = startfs();
	pidc = chancreate(sizeof(ulong), 0);
	proccreate(waiter, nil, 8*1024);
	procrfork(runrc, nil, 16*1024, RFNAMEG|RFFDG|RFENVG|RFNOTEG);
	rcpid = recvul(pidc);
	close(fsfd);
	if(rcpid == ~0)
		sysfatal("can't run rc");

	uxmain();
	threadexitsall(nil);
}
