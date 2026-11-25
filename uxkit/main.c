/*
 * uxkit: a uitree host. It serves $ui (libux's tree), runs the client,
 * and exits with its status; with -d, libux draws the tree.
 *
 *	uxkit [-dD] [-m mtpt] [-n name] [-s srvname] cmd [arg...]
 */
#include <u.h>
#include <libc.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>
#include <tree.h>

static char *mtpt = "/mnt/ui";
static char *srvname;

static void
usage(void)
{
	fprint(2, "usage: uxkit [-dD] [-m mtpt] [-n name] [-s srvname] cmd [arg...]\n");
	threadexitsall("usage");
}

/* run the client, and exit with its status */
static void
client(void *v)
{
	char **argv, *cmd;
	int pid;
	Waitmsg *w;

	argv = v;
	/*
	 * Everything the child needs is made before the fork. The child gets
	 * a copy of the heap taken while the server procs may be halfway
	 * through changing it (their malloc lock held, a free list being
	 * relinked), so it mustn't malloc: it would hang or find the heap
	 * corrupt. That was uxkit's rare hang and heap crash at start.
	 */
	cmd = argv[0];
	if(cmd[0] != '/' && strncmp(cmd, "./", 2) != 0 && strncmp(cmd, "../", 3) != 0)
		cmd = smprint("/bin/%s", cmd);
	switch(pid = fork()){
	case -1:
		sysfatal("fork: %r");
	case 0:
		exec(cmd, argv);
		fprint(2, "uxkit: exec %s: %r\n", cmd);
		_exits("exec failed");
	}
	while((w = wait()) != nil){
		if(w->pid == pid)
			threadexitsall(w->msg[0] != 0 ? w->msg : nil);
		free(w);
	}
	threadexitsall("no client");
}

int mainstacksize = 64*1024;

static char *appname;

/*
 * The app's name at uxd, [a-z][a-z0-9-]*: the client's, or its
 * script's when the client is a shell (rc tools.rc is tools).
 */
static char*
nameof1(char **argv)
{
	char *s, *p, *q;

	s = argv[0];
	if((p = strrchr(s, '/')) != nil)
		s = p+1;
	if((strcmp(s, "rc") == 0 || strcmp(s, "sh") == 0) && argv[1] != nil){
		s = argv[1];
		if((p = strrchr(s, '/')) != nil)
			s = p+1;
	}
	s = estrdup(s);
	if((p = strchr(s, '.')) != nil)
		*p = 0;
	for(p = q = s; *p; p++){
		if(*p >= 'A' && *p <= 'Z')
			*p += 'a'-'A';
		if(*p >= 'a' && *p <= 'z' || *p >= '0' && *p <= '9' && q > s || *p == '-' && q > s)
			*q++ = *p;
		else if(q > s)
			*q++ = '-';
	}
	*q = 0;
	if(*s == 0){
		free(s);
		s = estrdup("uitree");
	}
	return s;
}

/*
 * uxkitdebug=1 keeps a proc that fails an assertion (the heap's among
 * them) alive and asleep, so acid can read its stack.
 */
extern void (*__assert)(char*);

static void
stayassert(char *s)
{
	fprint(2, "uxkit: assertion failed: %s, pid %d\n", s, getpid());
	for(;;)
		sleep(1000);
}

void
threadmain(int argc, char **argv)
{
	char *pe;

	if((pe = getenv("uxkitdebug")) != nil){
		__assert = stayassert;
		free(pe);
	}
	ARGBEGIN{
	case 'd':
		drawing = 1;
		break;
	case 'D':
		chatty9p++;
		break;
	case 'm':
		mtpt = EARGF(usage());
		break;
	case 'n':
		appname = EARGF(usage());
		break;
	case 's':
		srvname = EARGF(usage());
		break;
	default:
		usage();
	}ARGEND
	if(argc < 1)
		usage();
	if(appname == nil)
		appname = nameof1(argv);
	initapp();
	if(drawing)
		uistart(appname, treepost);
	treeserve(srvname, mtpt);
	putenv("ui", mtpt);
	if(!drawing)
		client(argv);
	proccreate(client, argv, 64*1024);
	uirun();
	threadexitsall(nil);
}

