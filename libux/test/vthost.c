/*
 * vt.c's tests, on the host (make test in libux): what programs write,
 * and what the primary and alternate screens hold after it.
 */
#include "../vt.h"
#include <assert.h>

/* UTF-8, for the host */
int
chartorune(Rune *r, char *s)
{
	uchar *u;
	int n, i;
	Rune c;

	u = (uchar*)s;
	if(u[0] < 0x80){ *r = u[0]; return 1; }
	if((u[0] & 0xE0) == 0xC0){ n = 2; c = u[0] & 0x1F; }
	else if((u[0] & 0xF0) == 0xE0){ n = 3; c = u[0] & 0x0F; }
	else if((u[0] & 0xF8) == 0xF0){ n = 4; c = u[0] & 0x07; }
	else { *r = Runeerror; return 1; }
	for(i = 1; i < n; i++){
		if((u[i] & 0xC0) != 0x80){ *r = Runeerror; return 1; }
		c = c<<6 | (u[i] & 0x3F);
	}
	*r = c;
	return n;
}

int
runetochar(char *s, Rune *rp)
{
	Rune r;

	r = *rp;
	if(r < 0x80){ s[0] = r; return 1; }
	if(r < 0x800){ s[0] = 0xC0 | r>>6; s[1] = 0x80 | (r & 0x3F); return 2; }
	if(r < 0x10000){ s[0] = 0xE0 | r>>12; s[1] = 0x80 | (r>>6 & 0x3F); s[2] = 0x80 | (r & 0x3F); return 3; }
	s[0] = 0xF0 | r>>18; s[1] = 0x80 | (r>>12 & 0x3F); s[2] = 0x80 | (r>>6 & 0x3F); s[3] = 0x80 | (r & 0x3F);
	return 4;
}

int
fullrune(char *s, int n)
{
	uchar c;

	if(n <= 0)
		return 0;
	c = s[0];
	if(c < 0x80) return 1;
	if((c & 0xE0) == 0xC0) return n >= 2;
	if((c & 0xF0) == 0xE0) return n >= 3;
	if((c & 0xF8) == 0xF0) return n >= 4;
	return 1;
}

/* a console's text: output, then what's typed after qh */

static Vbuf b;
static Vt *vt;
static char replies[1024];
static char title[256];
static int failed;

static void
breplace(Vbuf *v, int q0, int q1, Rune *r, Vattr *a, int n)
{
	int d, i;

	assert(0 <= q0 && q0 <= q1 && q1 <= v->ntext);
	d = n - (q1-q0);
	v->text = realloc(v->text, (v->ntext + (d > 0 ? d : 0) + 1)*sizeof(Rune));
	v->attr = realloc(v->attr, (v->ntext + (d > 0 ? d : 0) + 1)*sizeof(Vattr));
	memmove(v->text+q0+n, v->text+q1, (v->ntext-q1)*sizeof(Rune));
	memmove(v->attr+q0+n, v->attr+q1, (v->ntext-q1)*sizeof(Vattr));
	for(i = 0; i < n; i++){
		v->text[q0+i] = r[i];
		if(a != nil)
			v->attr[q0+i] = a[i];
		else
			memset(&v->attr[q0+i], 0, sizeof(Vattr));
	}
	v->ntext += d;
#define Move(q) if(q >= q1) q += d; else if(q > q0) q = q0
	Move(v->qh);
	Move(v->qc);
	if(v->qs >= 0){
		Move(v->qs);
	}
#undef Move
}

static void
vreply(Vt *v, char *s, int n)
{
	USED(v);
	strncat(replies, s, n);
}

static void
vtitle(Vt *v, char *s)
{
	USED(v);
	snprint(title, sizeof title, "%s", s);
}

static void
setup(int rows, int cols)
{
	vtfree(vt);
	free(b.text);
	free(b.attr);
	memset(&b, 0, sizeof b);
	b.replace = breplace;
	vt = vtnew(rows, cols, &b);
	vt->reply = vreply;
	vt->settitle = vtitle;
	vt->fgrgb = 0x000000;
	vt->bgrgb = 0xFFFFFF;
	replies[0] = 0;
	title[0] = 0;
}

static void
w(char *s)
{
	vtwrite(vt, s, strlen(s));
}

/* the primary screen's output as UTF-8, with ^ at the cursor */
static char*
text(void)
{
	static char s[65536];
	char *p;
	int i;

	p = s;
	for(i = 0; i <= b.qh; i++){
		if(i == b.qc)
			*p++ = '^';
		if(i < b.qh)
			p += runetochar(p, &b.text[i]);
	}
	*p = 0;
	return s;
}

#define check(name, got, want) do{ \
	char *_g = (got), *_w = (want); \
	if(strcmp(_g, _w) != 0){ \
		fprintf(stderr, "FAIL %s:%d %s\n  got  %s\n  want %s\n", __FILE__, __LINE__, name, _g, _w); \
		failed++; \
	} \
}while(0)

#define checkint(name, got, want) do{ \
	long _g = (got), _w = (want); \
	if(_g != _w){ \
		fprintf(stderr, "FAIL %s:%d %s: got %ld want %ld\n", __FILE__, __LINE__, name, _g, _w); \
		failed++; \
	} \
}while(0)

static char*
screen(void)
{
	static char s[65536];
	char *t;

	t = vtscreen(vt);
	snprint(s, sizeof s, "%s", t);
	free(t);
	return s;
}

static void
testplain(void)
{
	setup(24, 80);
	w("hello\nworld\n");
	check("plain lines", text(), "hello\nworld\n^");
	w("tab\there");
	check("tab at the end stays a tab", text(), "hello\nworld\ntab\there^");
	setup(24, 80);
	vtwrite(vt, "a\0b", 3);
	check("NUL dropped", text(), "ab^");
	w("\033[31mred\033[0m");
	check("SGR leaves no text", text(), "abred^");
	checkint("red fg", b.attr[2].fg, VTpal|1);
	checkint("after reset", b.attr[1].fg, 0);
	/* a UTF-8 rune split across writes */
	setup(24, 80);
	vtwrite(vt, "\xe2\x94", 2);
	vtwrite(vt, "\x80!", 2);
	check("split rune", text(), "─!^");
	/* unknown sequences never show */
	setup(24, 80);
	w("x\033[?9999h\033]8;;http://a\033\\y\033P+q544e\033\\z\033[>4;1m");
	check("swallowed", text(), "xyz^");
}

static void
testlineedit(void)
{
	setup(24, 80);
	w("progress 10%\rprogress 50%\rdone");
	check("CR overwrites", text(), "done^ress 50%");
	w("\033[K");
	check("EL 0", text(), "done^");
	setup(24, 80);
	w("$ helo\b\b\033[Kllo");
	check("readline-style fix", text(), "$ hello^");
	w("\033[3D");
	check("CUB", text(), "$ he^llo");
	w("\033[2P");
	check("DCH", text(), "$ he^o");
	w("\033[1@");
	check("ICH", text(), "$ he^ o");
	w("\033[10G");
	check("CHA past the end pads", text(), "$ he o   ^");
	w("\033[2K");
	check("EL 2 keeps the column", text(), "         ^");
	setup(24, 80);
	w("abc\r\n");
	check("CR LF", text(), "abc\n^");
	w("one\ntwo\033[Athree");
	check("CUU then write", text(), "abc\nonethree^\ntwo");
	setup(24, 80);
	w("x\033[1;31mB\033[22;39m\033[48;5;196mC\033[48;2;1;2;3mD\033[38:2::4:5:6mE");
	checkint("bold", b.attr[1].fl & Vbold, Vbold);
	checkint("red", b.attr[1].fg, VTpal|1);
	checkint("bold off", b.attr[2].fl & Vbold, 0);
	checkint("256 bg", b.attr[2].bg, VTpal|196);
	checkint("rgb bg", b.attr[3].bg, VTrgb|0x010203);
	checkint("rgb fg, colon form", b.attr[4].fg, VTrgb|0x040506);
}

static void
testclear(void)
{
	setup(3, 80);
	w("1\n2\n3\n4\n5");
	w("\033[H");
	check("home is the screen's top", text(), "1\n2\n^3\n4\n5");
	w("\033[2J\033[3J");
	check("clear(1)", text(), "^");
	setup(3, 80);
	w("1\n2\n3\n4\n5\033[2;1H\033[J");
	check("ED 0 from row 2", text(), "1\n2\n3\n^");
	setup(24, 80);
	w("\033[5;3Hx");
	check("CUP below the text makes lines", text(), "\n\n\n\n  x^");
	setup(24, 80);
	w("a\033[6n");
	check("DSR", replies, "\033[1;2R");
}

static void
testalt(void)
{
	setup(4, 10);
	w("shell$ ");
	w("\033[?1049h\033[H\033[2J");
	checkint("alt on", vt->alt, 1);
	w("\033[1;1Htop\033[4;1Hbottom\033[2;3H\033[7mX");
	check("grid", screen(), "top\n  X\n\nbottom\n");
	checkint("inverse", vt->grid[1*10+2].a.fl & Vinverse, Vinverse);
	check("primary untouched", text(), "shell$ ^");
	w("\033[?1049l");
	checkint("alt off", vt->alt, 0);
	w("back");
	check("primary again", text(), "shell$ back^");

	/* autowrap and scrolling */
	setup(3, 5);
	w("\033[?1049h");
	w("abcdefgh");
	check("wrap", screen(), "abcde\nfgh\n\n");
	w("\r\n1\r\n2\r\n3");
	check("scroll", screen(), "1\n2\n3\n");
	/* the scroll region */
	setup(4, 5);
	w("\033[?1049h1\r\n2\r\n3\r\n4\033[2;3r\033[3;1H\nX");
	check("region scrolls alone", screen(), "1\n3\nX\n4\n");
	w("\033[2;1H\033[L");
	check("IL in the region", screen(), "1\n\n3\n4\n");
	w("\033[M");
	check("DL in the region", screen(), "1\n3\n\n4\n");
	/* line drawing */
	setup(2, 5);
	w("\033[?1049h\033(0lqk\033(Bx");
	check("DEC graphics", screen(), "┌─┐x\n\n");
	/* wide characters take two cells */
	setup(2, 6);
	w("\033[?1049h日本\033[2;1Hab");
	check("wide", screen(), "日本\nab\n");
	checkint("cursor after", vt->cx, 2);
	/* erasing in the background colour */
	setup(2, 4);
	w("\033[?1049h\033[44m\033[2J");
	checkint("bce", vt->grid[0].a.bg, VTpal|4);
	/* insert and delete characters */
	setup(1, 6);
	w("\033[?1049habcdef\033[1;2H\033[2@");
	check("ICH grid", screen(), "a  bcd\n");
	w("\033[3P");
	check("DCH grid", screen(), "acd\n");
}

static void
testreplies(void)
{
	setup(24, 80);
	w("\033[c");
	check("DA1", replies, "\033[?62;22c");
	replies[0] = 0;
	w("\033[>c");
	check("DA2", replies, "\033[>0;95;0c");
	replies[0] = 0;
	w("\033]11;?\007");
	check("OSC 11", replies, "\033]11;rgb:ffff/ffff/ffff\033\\");
	replies[0] = 0;
	w("\033[18t");
	check("size", replies, "\033[8;24;80t");
	replies[0] = 0;
	w("\033[?2004h\033[?2004$p");
	check("DECRQM", replies, "\033[?2004;1$y");
	w("\033]0;vim: notes\007");
	check("title", title, "vim: notes");
	w("\033]2;st\033\\after");
	check("title ST", title, "st");
	check("after title", text(), "after^");
}

static void
testkeys(void)
{
	char k[16];

	setup(24, 80);
	vtkey(vt, Kup, k, sizeof k);
	check("up", k, "\033[A");
	w("\033[?1h");
	vtkey(vt, Kup, k, sizeof k);
	check("up, application", k, "\033OA");
	vtkey(vt, Kpgdown, k, sizeof k);
	check("page down", k, "\033[6~");
	vtkey(vt, KF|1, k, sizeof k);
	check("F1", k, "\033OP");
	checkint("a private key we don't send", vtkey(vt, KF|0x20, k, sizeof k), 0);
	vtkey(vt, 0x00E9, k, sizeof k);
	check("é", k, "é");

	checkint("no mouse unless asked", vtmouse(vt, 0, VMpress, 1, 2, k, sizeof k), 0);
	w("\033[?1000h\033[?1006h");
	vtmouse(vt, 0, VMpress, 4, 2, k, sizeof k);
	check("SGR press", k, "\033[<0;5;3M");
	vtmouse(vt, 0, VMrelease, 4, 2, k, sizeof k);
	check("SGR release", k, "\033[<0;5;3m");
	vtmouse(vt, 64, VMpress, 0, 0, k, sizeof k);
	check("wheel", k, "\033[<64;1;1M");
}

static void
testresize(void)
{
	setup(3, 4);
	w("\033[?1049habcd\r\nef\r\ngh");
	vtresize(vt, 2, 6);
	check("keeps the bottom rows", screen(), "ef\ngh\n");
	checkint("cursor row", vt->cy, 1);
	setup(3, 4);
	w("\033[?1049h\033[2;3r");
	vtresize(vt, 5, 5);
	checkint("region resets", vt->bot, 4);
}

/* random bytes must never break it */
static void
testfuzz(void)
{
	char s[256];
	unsigned x;
	int i, j;

	x = 1;
	for(i = 0; i < 20000; i++){
		if(i % 500 == 0)
			setup(1 + i%7, 1 + i%13);
		for(j = 0; j < (int)sizeof s; j++){
			x = x*1103515245 + 12345;
			s[j] = (x>>16) & 0xFF;
			/* bias toward escape sequences */
			if((x>>8) % 5 == 0)
				s[j] = "\033[;?0123456789HJKmrhl\r\n\b\t"[(x>>3) % 26];
		}
		vtwrite(vt, s, sizeof s);
		if(i % 97 == 0)
			vtresize(vt, 1 + (x>>4)%9, 1 + (x>>9)%17);
		assert(b.qc >= 0 && b.qc <= b.qh && b.qh <= b.ntext);
		assert(vt->cx >= 0 && vt->cx < vt->cols && vt->cy >= 0 && vt->cy < vt->rows);
		if(b.ntext > 20000){
			setup(5, 9);
		}
	}
}

int
main(void)
{
	testplain();
	testlineedit();
	testclear();
	testalt();
	testreplies();
	testkeys();
	testresize();
	testfuzz();
	vtfree(vt);
	free(b.text);
	free(b.attr);
	if(failed){
		fprintf(stderr, "%d failed\n", failed);
		return 1;
	}
	printf("vt: ok\n");
	return 0;
}
