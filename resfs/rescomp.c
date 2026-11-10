/*
 * rescomp: compile resource source text into a resource file, or
 * decompile a resource file into source.
 *
 *	rescomp [-o out.rsrc] file.res
 *	rescomp -d file.rsrc
 *
 * The source format is described in docs/protocols/res.md
 * ("Resource source"). Its bodies are the text forms resfs serves,
 * so `rescomp -d` then `rescomp` gives back the same resources.
 */
#include "rescompat.h"
#include "res.h"

enum {
	Maxtok = 16,
	Maxbody = 65536,
};

typedef struct Src Src;
struct Src {
	char	*file;
	char	*dir;	/* for relative paths, with a trailing / or empty */
	char	**line;
	int	nline;
	Res	*res;
	int	nres;
	int	cap;
};

static char *argv0name = "rescomp";

static void
die(char *msg)
{
	char buf[512];

	snprint(buf, sizeof buf, "%s: %s\n", argv0name, msg);
	if(write(2, buf, strlen(buf)) < 0)
		exits("error");
	exits("error");
}

static void
srcerr(Src *s, int line, char *msg, char *detail)
{
	char buf[512];

	if(detail != nil)
		snprint(buf, sizeof buf, "%s:%d: %s: %s", s->file, line, msg, detail);
	else
		snprint(buf, sizeof buf, "%s:%d: %s", s->file, line, msg);
	die(buf);
}

static void*
emalloc(u32int n)
{
	void *p;

	p = malloc(n ? n : 1);
	if(p == nil)
		die("out of memory");
	memset(p, 0, n ? n : 1);
	return p;
}

static char*
estrdup(char *s)
{
	char *p;

	p = emalloc(strlen(s) + 1);
	strcpy(p, s);
	return p;
}

static uchar*
readfile(char *path, u32int *n)
{
	char err[128], buf[512];
	uchar *b;

	b = resreadfile(path, n, err, sizeof err);
	if(b == nil){
		snprint(buf, sizeof buf, "%s: %s", path, err);
		die(buf);
	}
	return b;
}

/* split the file into NUL-terminated lines, dropping a trailing CR */
static void
splitlines(Src *s, uchar *buf, u32int n)
{
	u32int i, start, len;
	char **nl;
	int cap;

	cap = 64;
	s->line = emalloc(cap * sizeof *s->line);
	s->nline = 0;
	start = 0;
	for(i = 0; i <= n; i++){
		if(i < n && buf[i] != '\n')
			continue;
		if(i == n && start == n)
			break;
		if(s->nline == cap){
			cap *= 2;
			nl = emalloc(cap * sizeof *nl);
			memmove(nl, s->line, s->nline * sizeof *nl);
			free(s->line);
			s->line = nl;
		}
		len = i - start;
		if(len > 0 && buf[start + len - 1] == '\r')
			len--;
		if(memchr(buf + start, 0, len) != nil)
			srcerr(s, s->nline+1, "NUL byte in source", nil);
		s->line[s->nline] = emalloc(len + 1);
		memmove(s->line[s->nline], buf + start, len);
		s->nline++;
		start = i+1;
	}
}

static Res*
newres(Src *s, uchar type[4], int id, int line)
{
	Res *r, *nr;
	int i;

	for(i = 0; i < s->nres; i++)
		if(memcmp(s->res[i].type, type, 4) == 0 && s->res[i].id == id)
			srcerr(s, line, "the same type and ID twice", nil);
	if(s->nres == s->cap){
		s->cap = s->cap ? 2*s->cap : 32;
		nr = emalloc(s->cap * sizeof *nr);
		if(s->nres > 0)
			memmove(nr, s->res, s->nres * sizeof *nr);
		free(s->res);
		s->res = nr;
	}
	r = &s->res[s->nres++];
	memset(r, 0, sizeof *r);
	memmove(r->type, type, 4);
	r->id = id;
	return r;
}

static void
setdata(Res *r, Rbuf *b)
{
	r->data = b->p;
	r->len = b->n;
	rbufinit(b);
}

static char*
path(Src *s, char *p)
{
	char *q;

	if(p[0] == '/' || s->dir[0] == 0)
		return estrdup(p);
	q = emalloc(strlen(s->dir) + strlen(p) + 1);
	strcpy(q, s->dir);
	strcat(q, p);
	return q;
}

static int
parseid(char *t, int *id)
{
	long v;
	int neg;

	neg = *t == '-';
	if(neg)
		t++;
	if(*t == 0)
		return -1;
	for(v = 0; *t; t++){
		if(*t < '0' || *t > '9' || v > 40000)
			return -1;
		v = v*10 + *t - '0';
	}
	if(neg)
		v = -v;
	if(v < -32768 || v > 32767)
		return -1;
	*id = v;
	return 0;
}

static int
parseattr(char *s, int *attr)
{
	static char *names[] = {
		nil, "changed", "preload", "protected",
		"locked", "purgeable", "sysheap", "sysref",
	};
	char *p;
	int i;

	*attr = 0;
	if(s == nil || *s == 0)
		return 0;
	for(;;){
		p = strchr(s, ',');
		if(p != nil)
			*p = 0;
		for(i = 1; i < 8; i++)
			if(strcmp(s, names[i]) == 0)
				break;
		if(i == 8)
			return -1;
		*attr |= 1<<i;
		if(p == nil)
			return 0;
		s = p+1;
	}
}

static void
setname(Src *s, Res *r, char **tok, int ntok, int line)
{
	char *v;

	v = resattr(tok, ntok, "name");
	if(v == nil)
		return;
	if(resunesc(v) < 0)
		srcerr(s, line, "bad value: bad escape in", "name");
	r->name = estrdup(v);
}

static u32int*
loadimage(Src *s, char *p, int line, int *w, int *h)
{
	char err[128], *full;
	uchar *buf;
	u32int n, *px;

	full = path(s, p);
	buf = readfile(full, &n);
	if(resreadimage(buf, n, w, h, &px, err, sizeof err) < 0)
		srcerr(s, line, full, err);
	free(buf);
	free(full);
	return px;
}

static void
icondata(Src *s, uchar type[4], char *img, char *bw, int line, Rbuf *out)
{
	char err[128];
	u32int *px, *bpx;
	int w, h, bw_, bh;

	px = loadimage(s, img, line, &w, &h);
	bpx = nil;
	if(bw != nil){
		bpx = loadimage(s, bw, line, &bw_, &bh);
		if(bw_ != w || bh != h)
			srcerr(s, line, "bad value: bw image size differs from image", nil);
	}
	if(resiconenc(type, w, h, px, bpx, out, err, sizeof err) < 0)
		srcerr(s, line, err, img);
	free(px);
	free(bpx);
}

static void
hexdata(Src *s, char *hex, int line, Rbuf *out)
{
	int h, l;

	for(; *hex; hex += 2){
		h = reshexval(hex[0]);
		l = h < 0 ? -1 : reshexval(hex[1]);
		if(h < 0 || l < 0)
			srcerr(s, line, "bad value", "hex");
		rbufc(out, h<<4 | l);
	}
}

/* resource TYPE ID [attrs]: returns the next line to read */
static int
resource(Src *s, int ln, char **tok, int ntok)
{
	char err[128], buf[64], *v, *img, *bw;
	uchar type[4];
	int id, attr, end, eline, hx, hy, w, h;
	u32int n, *px;
	Rbuf b;
	Res *r;

	if(ntok < 3 || resunescname(tok[1], type) < 0)
		srcerr(s, ln+1, "bad syntax", "resource TYPE ID [attr=...]");
	if(parseid(tok[2], &id) < 0)
		srcerr(s, ln+1, "bad value: resource ID", tok[2]);
	if(parseattr(resattr(tok, ntok, "attr"), &attr) < 0)
		srcerr(s, ln+1, "bad value", "attr");
	r = newres(s, type, id, ln+1);
	r->attr = attr;
	setname(s, r, tok, ntok, ln+1);
	rbufinit(&b);
	if((v = resattr(tok, ntok, "hex")) != nil)
		hexdata(s, v, ln+1, &b);
	else if((v = resattr(tok, ntok, "file")) != nil){
		v = path(s, v);
		b.p = readfile(v, &n);
		b.n = b.cap = n;
		free(v);
	}else if((img = resattr(tok, ntok, "image")) != nil){
		bw = resattr(tok, ntok, "bw");
		if(memcmp(type, "CURS", 4) == 0){
			v = resattr(tok, ntok, "hotspot");
			hx = hy = 0;
			if(v != nil){
				if(strlen(v) >= sizeof buf)
					srcerr(s, ln+1, "bad value", "hotspot");
				strcpy(buf, v);
				v = strchr(buf, ',');
				if(v == nil)
					srcerr(s, ln+1, "bad value", "hotspot");
				*v++ = 0;
				if(parseid(buf, &hx) < 0 || parseid(v, &hy) < 0)
					srcerr(s, ln+1, "bad value", "hotspot");
			}
			px = loadimage(s, img, ln+1, &w, &h);
			if(rescursenc(w, h, px, hx, hy, &b, err, sizeof err) < 0)
				srcerr(s, ln+1, err, img);
			free(px);
		}else
			icondata(s, type, img, bw, ln+1, &b);
	}else{
		/* a text form, up to a line that is just "end" */
		for(end = ln+1; end < s->nline && strcmp(s->line[end], "end") != 0; end++)
			;
		if(end == s->nline)
			srcerr(s, ln+1, "bad syntax: no end for this resource", nil);
		if(!hasencoder(type))
			srcerr(s, ln+1, "bad syntax: this type has no text form; give hex=, file= or image=", nil);
		if(restextenc(type, s->line + ln + 1, end - ln - 1, &b, err, sizeof err, &eline) < 0)
			srcerr(s, eline > 0 ? ln + 1 + eline : ln + 1, err, nil);
		ln = end;
	}
	if(b.err)
		die("out of memory");
	setdata(r, &b);
	return ln + 1;
}

/* icon ID [name=] [large=] [large1=] [small=] [small1=] */
static void
iconfamily(Src *s, int ln, char **tok, int ntok)
{
	static char *large[] = {"ICN#", "icl4", "icl8"};
	static char *small[] = {"ics#", "ics4", "ics8"};
	char **types, *img, *bw;
	int id, i, k;
	Rbuf b;
	Res *r;

	if(ntok < 2 || parseid(tok[1], &id) < 0)
		srcerr(s, ln, "bad syntax", "icon ID [large=image] [small=image]");
	if(resattr(tok, ntok, "large") == nil && resattr(tok, ntok, "small") == nil)
		srcerr(s, ln, "bad syntax: icon needs large= or small=", nil);
	for(k = 0; k < 2; k++){
		img = resattr(tok, ntok, k == 0 ? "large" : "small");
		bw = resattr(tok, ntok, k == 0 ? "large1" : "small1");
		types = k == 0 ? large : small;
		if(img == nil)
			continue;
		for(i = 0; i < 3; i++){
			r = newres(s, (uchar*)types[i], id, ln);
			if(i == 0 && k == 0)
				setname(s, r, tok, ntok, ln);
			rbufinit(&b);
			icondata(s, r->type, img, i == 0 ? bw : nil, ln, &b);
			setdata(r, &b);
		}
	}
}

static void
compile(char *file, char *outfile)
{
	char err[128], *tok[Maxtok], *p, *line;
	uchar *buf;
	u32int n;
	int ln, ntok, fd;
	long w;
	Src s;
	Rbuf out;

	memset(&s, 0, sizeof s);
	s.file = file;
	s.dir = estrdup(file);
	p = strrchr(s.dir, '/');
	if(p != nil)
		p[1] = 0;
	else
		s.dir[0] = 0;
	buf = readfile(file, &n);
	splitlines(&s, buf, n);
	free(buf);
	for(ln = 0; ln < s.nline; ){
		line = estrdup(s.line[ln]);
		for(p = line; *p == ' ' || *p == '\t'; p++)
			;
		if(*p == 0 || *p == '#'){
			free(line);
			ln++;
			continue;
		}
		ntok = restokenize(line, tok, Maxtok);
		if(ntok < 0)
			srcerr(&s, ln+1, "bad syntax", "unterminated quote or too many tokens");
		if(strcmp(tok[0], "resource") == 0)
			ln = resource(&s, ln, tok, ntok);
		else if(strcmp(tok[0], "icon") == 0){
			iconfamily(&s, ln+1, tok, ntok);
			ln++;
		}else
			srcerr(&s, ln+1, "unknown command", tok[0]);
		free(line);
	}
	rbufinit(&out);
	if(reswrite(s.res, s.nres, 0, &out, err, sizeof err) < 0)
		die(err);
	if(outfile == nil){
		fd = 1;
	}else{
		fd = create(outfile, OWRITE, 0666);
		if(fd < 0){
			snprint(err, sizeof err, "cannot create %s", outfile);
			die(err);
		}
	}
	for(n = 0; n < out.n; n += w){
		w = write(fd, out.p + n, out.n - n);
		if(w <= 0)
			die("write error");
	}
	if(fd != 1)
		close(fd);
}

/* does the text form of r encode back to exactly r's bytes? */
static int
roundtrips(Res *r, Rbuf *text)
{
	char *copy, **lines;
	int n, i, ok, eline;
	u32int k;
	Rbuf enc;

	copy = emalloc(text->n + 1);
	memmove(copy, text->p, text->n);
	copy[text->n] = 0;
	n = 0;
	for(k = 0; k < text->n; k++)
		if(copy[k] == '\n')
			n++;
	lines = emalloc((n + 1) * sizeof *lines);
	lines[0] = copy;
	for(i = 1, k = 0; k < text->n; k++)
		if(copy[k] == '\n'){
			copy[k] = 0;
			if(i <= n)
				lines[i++] = copy + k + 1;
		}
	ok = 0;
	for(i = 0; i < n; i++)
		if(strcmp(lines[i], "end") == 0)
			goto out;
	rbufinit(&enc);
	if(restextenc(r->type, lines, n, &enc, nil, 0, &eline) == 0
	&& enc.n == r->len && memcmp(enc.p, r->data, r->len) == 0)
		ok = 1;
	rbuffree(&enc);
out:
	free(lines);
	free(copy);
	return ok;
}

static void
decompile(char *file)
{
	static char *attrnames[] = {
		nil, "changed", "preload", "protected",
		"locked", "purgeable", "sysheap", "sysref",
	};
	char err[128], t[16];
	uchar *buf;
	u32int n, k;
	int i, j, first;
	long w;
	Resfile rf;
	Rbuf out, text;
	Res *r;

	buf = readfile(file, &n);
	if(resparse(buf, n, &rf, err, sizeof err) < 0)
		die(err);
	rbufinit(&out);
	rbufs(&out, "# decompiled by rescomp -d\n");
	for(i = 0; i < rf.nres; i++){
		r = &rf.res[i];
		resescname(r->type, t, sizeof t);
		rbufs(&out, "resource ");
		rbufs(&out, t);
		rbufc(&out, ' ');
		rbufint(&out, r->id);
		if(r->name != nil)
			rbufkv(&out, "name", r->name);
		if((r->attr & 0xFE) != 0){
			rbufs(&out, " attr=");
			first = 1;
			for(j = 7; j >= 1; j--)
				if(r->attr & (1<<j)){
					if(!first)
						rbufc(&out, ',');
					first = 0;
					rbufs(&out, attrnames[j]);
				}
		}
		rbufinit(&text);
		if(istexttype(r) && restext(r, &text, nil, 0) == 0 && roundtrips(r, &text)){
			rbufc(&out, '\n');
			rbufput(&out, text.p, text.n);
			rbufs(&out, "end\n");
		}else{
			rbufs(&out, " hex=");
			for(k = 0; k < r->len; k++)
				rbufhex(&out, r->data[k], 2);
			rbufc(&out, '\n');
		}
		rbuffree(&text);
	}
	if(out.err)
		die("out of memory");
	for(k = 0; k < out.n; k += w){
		w = write(1, out.p + k, out.n - k);
		if(w <= 0)
			die("write error");
	}
}

static void
usage(void)
{
	die("usage: rescomp [-o out.rsrc] file.res | rescomp -d file.rsrc");
}

#ifdef RESHOST
int
#else
void
#endif
main(int argc, char **argv)
{
	char *out;
	int dec, i;

	out = nil;
	dec = 0;
	for(i = 1; i < argc && argv[i][0] == '-' && argv[i][1] != 0; i++){
		if(strcmp(argv[i], "-d") == 0)
			dec = 1;
		else if(strcmp(argv[i], "-o") == 0 && i+1 < argc)
			out = argv[++i];
		else
			usage();
	}
	if(i != argc - 1 || (dec && out != nil))
		usage();
	if(dec)
		decompile(argv[i]);
	else
		compile(argv[i], out);
	exits(nil);
}
