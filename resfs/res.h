/*
 * Classic Macintosh resource files: parsing and decoding.
 * Include "rescompat.h" first.
 *
 * Every function here treats its input as untrusted. Lengths,
 * offsets, counts and dimensions are checked before they are used,
 * and nothing is allocated from a size read out of the file without
 * checking it against the bytes actually present.
 */

typedef struct Res Res;
typedef struct Resfile Resfile;
typedef struct Rbuf Rbuf;

enum {
	Resmaxfile = 64*1024*1024,	/* largest file resfs will load */
	Resmaxres = 65536,		/* refs per file; more is refused */

	/* resource attribute bits */
	Rsysref = 0x80,
	Rsysheap = 0x40,
	Rpurgeable = 0x20,
	Rlocked = 0x10,
	Rprotected = 0x08,
	Rpreload = 0x04,
	Rchanged = 0x02,
};

struct Res {
	uchar	type[4];	/* four-character type code, as stored */
	int	id;		/* signed 16-bit resource ID */
	char	*name;		/* UTF-8, or nil when the resource has none */
	int	attr;		/* attribute byte */
	uchar	*data;		/* points into the buffer given to resparse */
	u32int	len;
};

struct Resfile {
	Res	*res;		/* sorted by type, then ID; (type, id) is unique */
	int	nres;
	int	ndup;		/* refs dropped because (type, id) repeated */
	int	nbad;		/* refs dropped because their data was out of bounds */
	int	attr;		/* resource map attributes */
};

/* A growable output buffer. On allocation failure err is set and
 * further appends do nothing. */
struct Rbuf {
	uchar	*p;
	u32int	n;
	u32int	cap;
	int	err;
};

/* res.c */
int	resparse(uchar *buf, u32int n, Resfile *rf, char *err, int nerr);
void	resfree(Resfile *rf);
Res	*reslookup(Resfile *rf, char *type, int id);
int	restypeeq(Res *r, char *type);
Res	*reslookupcode(Resfile *rf, uchar type[4], int id);
int	resunescname(char *s, uchar type[4]);
void	resindex(Resfile *rf, Rbuf *out);

/* resload.c */
uchar	*resreadfile(char *path, u32int *n, char *err, int nerr);

/* macroman.c */
int	macromanrune(int c);
void	macroman2utf(Rbuf *b, uchar *s, u32int n);
char	*macromanstr(uchar *s, u32int n);

/* rbuf.c */
void	rbufinit(Rbuf *b);
void	rbuffree(Rbuf *b);
void	rbufput(Rbuf *b, void *p, u32int n);
void	rbufc(Rbuf *b, int c);
void	rbufs(Rbuf *b, char *s);
void	rbufint(Rbuf *b, long v);
void	rbufhex(Rbuf *b, u32int v, int ndig);
void	rbufrune(Rbuf *b, long r);
void	rbufquote(Rbuf *b, char *s);
void	rbufkv(Rbuf *b, char *key, char *val);
void	rbufkvint(Rbuf *b, char *key, long v);
void	rbufkvbool(Rbuf *b, char *key, int v);
void	rbufuint(Rbuf *b, u32int v);
void	rbufesc(Rbuf *b, char *s);
int	resescname(uchar type[4], char *out, int nout);

/* icon.c */
enum {
	Iconlarge = 32,
	Iconsmall = 16,
};
int	isicontype(Res *r);
int	resimage(Resfile *rf, Res *r, Rbuf *out, char *err, int nerr);
int	resicon(Resfile *rf, int id, int size, Rbuf *out, char *err, int nerr);
int	rescursor(Res *r, Rbuf *out, char *err, int nerr);
int	rescursorimage(Res *r, Rbuf *out, char *err, int nerr);
extern u32int macpal4[16];
extern u32int macpal8[256];

/* text.c */
int	istexttype(Res *r);
int	restext(Res *r, Rbuf *out, char *err, int nerr);
