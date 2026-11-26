/*
 * vt: an xterm-compatible terminal (TERM=xterm-256color) for the
 * console control. It parses what a program writes, escape sequences
 * and all, and keeps two screens:
 *
 *  - The primary screen is the console's own text (a Vbuf the owner
 *    keeps), so plain output still flows, wraps and scrolls back as
 *    before. Its rows are the text's last lines and it has no width:
 *    the console wraps lines to fit. Colours and other attributes go
 *    with each rune; carriage return, backspace, cursor motion and
 *    erasing edit the text in place, so prompts, progress bars and
 *    line editors work.
 *  - The alternate screen (?1049h and the like, what vi, less and top
 *    use) is a grid of rows×cols cells the owner draws in a fixed-width
 *    font.
 *
 * Replies to queries (cursor position, device attributes, colours) go
 * back through the reply function, as if typed. vtkey and vtmouse
 * encode keys and the mouse as xterm sends them.
 *
 * This file builds on 9front and on the host (VTHOST: see Makefile).
 */

#ifdef VTHOST
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef uint32_t u32int;
typedef uint32_t Rune;
#define nil ((void*)0)
#define USED(x) ((void)(x))
#define snprint snprintf
#define nelem(a) (sizeof(a)/sizeof((a)[0]))
enum {
	UTFmax	= 4,
	Runeerror	= 0xFFFD,
	KF	= 0xF000,
	Spec	= 0xF800,
	Kview	= Spec|0x00,
	Khome	= KF|0x0D,
	Kup	= KF|0x0E,
	Kdown	= Kview,
	Kpgup	= KF|0x0F,
	Kleft	= KF|0x11,
	Kright	= KF|0x12,
	Kpgdown	= KF|0x13,
	Kins	= KF|0x14,
	Kend	= KF|0x18,
	Kbs	= 0x08,
	Kdel	= 0x7f,
	Kesc	= 0x1b,
};
int	chartorune(Rune*, char*);
int	runetochar(char*, Rune*);
int	fullrune(char*, int);
#endif

#ifndef VTTYPES
typedef struct Vattr Vattr;
typedef struct Vbuf Vbuf;
typedef struct Vt Vt;
#endif
typedef struct Vcell Vcell;

enum {
	/* colours: 0 is the default, else one of these with its value */
	VTpal	= 0x01000000,	/* | index into the 256-colour palette */
	VTrgb	= 0x02000000,	/* | 0xRRGGBB */

	/* Vattr.fl */
	Vbold	= 1<<0,
	Vfaint	= 1<<1,
	Vitalic	= 1<<2,
	Vuline	= 1<<3,
	Vblink	= 1<<4,
	Vinverse	= 1<<5,
	Vhidden	= 1<<6,
	Vstrike	= 1<<7,
	Vwide	= 1<<8,	/* cell: a double-width character's left half */
	Vcont	= 1<<9,	/* cell: its right half */

	/* vtmouse kinds */
	VMpress	= 0,
	VMrelease,
	VMmotion,
};

struct Vattr {
	u32int	fg;
	u32int	bg;
	ushort	fl;
};

struct Vcell {
	Rune	r;
	Vattr	a;
};

/*
 * The primary screen's text, kept by the owner: runes and their
 * attributes, the output point qh (what's after it isn't output: a
 * console keeps what's typed there), and the cursor qc, before or at
 * qh. replace swaps runes q0..q1 for the n in r (attributes a, or the
 * default if nil); it must update text, attr, ntext and qh, and move
 * qc and qs (offsets at or after q1 by the change in length, offsets
 * inside q0..q1 to q0), as it moves its own.
 */
struct Vbuf {
	Rune	*text;
	Vattr	*attr;
	int	ntext;
	int	qh;
	int	qc;	/* the cursor */
	int	qs;	/* the saved cursor (DECSC), or -1 */
	void	(*replace)(Vbuf*, int q0, int q1, Rune *r, Vattr *a, int n);
	void	*aux;
};

struct Vt {
	int	rows;
	int	cols;
	Vbuf	*buf;

	/* the alternate screen */
	int	alt;	/* it's showing */
	Vcell	*grid;	/* rows*cols */
	int	cx, cy;
	int	wrapnext;	/* the last column was written: wrap before the next */
	int	top, bot;	/* scroll region, rows top..bot inclusive */
	uchar	*tabs;	/* cols: a tab stop at each 1 */
	int	gdirty;	/* the grid changed since the owner last drew it */

	/* modes */
	Vattr	a;	/* for what's written next */
	int	insert;	/* IRM */
	int	autowrap;	/* DECAWM */
	int	origin;	/* DECOM */
	int	cursor;	/* DECTCEM: the cursor shows */
	int	appcursor;	/* DECCKM */
	int	appkeypad;	/* DECKPAM */
	int	mouse;	/* 0, or 1000, 1002, 1003: which events to report */
	int	sgrmouse;	/* 1006: report them as CSI < ... M */
	int	bpaste;	/* 2004: bracket what's pasted */
	int	focus;	/* 1004 */
	int	cs[4];	/* G0-G3: 'B' ASCII or '0' DEC line drawing */
	int	gl;	/* which of G0-G3 is in use */
	Rune	last;	/* the last character written, for REP */
	int	cleared;	/* ED 2 just erased the primary screen, the cursor this row on it; else -1 */

	struct {
		int	x, y;
		Vattr	a;
		int	origin;
		int	cs[4];
		int	gl;
		int	set;
	} sv[2];	/* DECSC: [0] primary, [1] alternate */

	/* the parser */
	int	state;
	int	arg[32];
	char	sep[32];	/* what came before arg[i]: ';' or ':' */
	int	narg;
	int	priv;	/* CSI's private marker: '?', '>', '<', '=', or 0 */
	int	inter;	/* the intermediate byte, or 0 */
	char	*str;	/* OSC (or DCS) text */
	int	nstr;
	int	strkind;	/* ']' OSC, 'P' DCS, '_' APC, '^' PM, 'X' SOS */
	char	u[UTFmax];	/* a rune split across writes */
	int	nu;
	Rune	*pr;	/* printable runes waiting to go on the primary screen */
	Vattr	*pa;
	int	npr;
	int	maxpr;

	/* to the owner */
	void	(*reply)(Vt*, char*, int);	/* as if typed */
	void	(*settitle)(Vt*, char*);
	void	*aux;
	u32int	fgrgb;	/* the default colours, for OSC 10 and 11 queries */
	u32int	bgrgb;
	int	bell;	/* rang since the owner last cleared it */
};

Vt*	vtnew(int rows, int cols, Vbuf*);
void	vtfree(Vt*);
void	vtwrite(Vt*, char*, long);
void	vtresize(Vt*, int rows, int cols);
void	vtreset(Vt*);
int	vtkey(Vt*, Rune, char*, int);
int	vtmouse(Vt*, int but, int kind, int col, int row, char*, int);
int	vtwcwidth(Rune);
u32int	vtrgb(u32int col, u32int def);
char*	vtscreen(Vt*);
