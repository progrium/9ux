/*
 * Growable output buffers and the text conventions in
 * docs/protocols/res.md: escaping, quoting, key=value tokens.
 */
#include "rescompat.h"
#include "res.h"

enum {
	Rbufmax = 64*1024*1024,
};

void
rbufinit(Rbuf *b)
{
	memset(b, 0, sizeof *b);
}

void
rbuffree(Rbuf *b)
{
	free(b->p);
	memset(b, 0, sizeof *b);
}

static int
grow(Rbuf *b, u32int n)
{
	u32int ncap;
	uchar *p;

	if(b->err)
		return -1;
	if(n > Rbufmax || b->n > Rbufmax - n){
		b->err = 1;
		return -1;
	}
	if(b->n + n <= b->cap)
		return 0;
	ncap = b->cap ? b->cap : 64;
	while(ncap < b->n + n)
		ncap *= 2;
	p = realloc(b->p, ncap);
	if(p == nil){
		b->err = 1;
		return -1;
	}
	b->p = p;
	b->cap = ncap;
	return 0;
}

void
rbufput(Rbuf *b, void *p, u32int n)
{
	if(n == 0 || grow(b, n) < 0)
		return;
	memmove(b->p + b->n, p, n);
	b->n += n;
}

void
rbufc(Rbuf *b, int c)
{
	uchar u;

	u = c;
	rbufput(b, &u, 1);
}

void
rbufs(Rbuf *b, char *s)
{
	rbufput(b, s, strlen(s));
}

void
rbufint(Rbuf *b, long v)
{
	char tmp[24];
	int i;
	unsigned long u;

	i = sizeof tmp;
	u = v < 0 ? -(unsigned long)v : (unsigned long)v;
	do{
		tmp[--i] = '0' + u%10;
		u /= 10;
	}while(u != 0);
	if(v < 0)
		tmp[--i] = '-';
	rbufput(b, tmp+i, sizeof tmp - i);
}

void
rbufhex(Rbuf *b, u32int v, int ndig)
{
	static char hex[] = "0123456789abcdef";
	char tmp[8];
	int i;

	if(ndig < 1 || ndig > 8)
		ndig = 8;
	for(i = ndig-1; i >= 0; i--){
		tmp[i] = hex[v & 0xF];
		v >>= 4;
	}
	rbufput(b, tmp, ndig);
}

void
rbufrune(Rbuf *b, long r)
{
	uchar s[4];

	if(r < 0 || r > 0x10FFFF || (r >= 0xD800 && r <= 0xDFFF))
		r = 0xFFFD;
	if(r < 0x80){
		s[0] = r;
		rbufput(b, s, 1);
	}else if(r < 0x800){
		s[0] = 0xC0 | (r>>6);
		s[1] = 0x80 | (r & 0x3F);
		rbufput(b, s, 2);
	}else if(r < 0x10000){
		s[0] = 0xE0 | (r>>12);
		s[1] = 0x80 | ((r>>6) & 0x3F);
		s[2] = 0x80 | (r & 0x3F);
		rbufput(b, s, 3);
	}else{
		s[0] = 0xF0 | (r>>18);
		s[1] = 0x80 | ((r>>12) & 0x3F);
		s[2] = 0x80 | ((r>>6) & 0x3F);
		s[3] = 0x80 | (r & 0x3F);
		rbufput(b, s, 4);
	}
}

/*
 * Escape a UTF-8 string so it fits on one line:
 * backslash, newline (from CR or LF), tab, and other control
 * characters become \\ \n \t \xHH.
 */
void
rbufesc(Rbuf *b, char *s)
{
	uchar c;

	for(; *s; s++){
		c = *s;
		switch(c){
		case '\\':
			rbufs(b, "\\\\");
			break;
		case '\r':
		case '\n':
			rbufs(b, "\\n");
			break;
		case '\t':
			rbufs(b, "\\t");
			break;
		default:
			if(c < 0x20 || c == 0x7F){
				rbufs(b, "\\x");
				rbufhex(b, c, 2);
			}else
				rbufc(b, c);
		}
	}
}

/* quote(2) rules: single quotes, with a quote doubled, when needed */
void
rbufquote(Rbuf *b, char *s)
{
	char *p;
	int need;

	need = *s == 0;
	for(p = s; *p; p++)
		if((uchar)*p <= ' ' || *p == '\'' || *p == '=' || *p == '#')
			need = 1;
	if(!need){
		rbufs(b, s);
		return;
	}
	rbufc(b, '\'');
	for(p = s; *p; p++){
		if(*p == '\'')
			rbufc(b, '\'');
		rbufc(b, *p);
	}
	rbufc(b, '\'');
}

/* " key=value", value escaped then quoted */
void
rbufkv(Rbuf *b, char *key, char *val)
{
	Rbuf e;

	rbufinit(&e);
	rbufesc(&e, val);
	rbufc(&e, 0);
	rbufc(b, ' ');
	rbufs(b, key);
	rbufc(b, '=');
	if(e.err)
		b->err = 1;
	else
		rbufquote(b, (char*)e.p);
	rbuffree(&e);
}

void
rbufkvint(Rbuf *b, char *key, long v)
{
	rbufc(b, ' ');
	rbufs(b, key);
	rbufc(b, '=');
	rbufint(b, v);
}

/*
 * A type code as a file name: bytes outside printable ASCII,
 * and '/' and '%', become %XX. "STR " is "STR%20".
 */
int
resescname(uchar type[4], char *out, int nout)
{
	static char hex[] = "0123456789ABCDEF";
	int i, n;
	uchar c;

	n = 0;
	for(i = 0; i < 4; i++){
		c = type[i];
		if(c > 0x20 && c < 0x7F && c != '/' && c != '%'){
			if(n+1 >= nout)
				return -1;
			out[n++] = c;
		}else{
			if(n+3 >= nout)
				return -1;
			out[n++] = '%';
			out[n++] = hex[c>>4];
			out[n++] = hex[c&0xF];
		}
	}
	out[n] = 0;
	return n;
}
