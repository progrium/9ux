/*
 * Reading the text conventions back: tokens with quote(2) rules,
 * unescaping, attributes, and UTF-8 to MacRoman.
 */
#include "rescompat.h"
#include "res.h"

/*
 * Split a line into tokens in place, tokenize(2) style: spaces and
 * tabs separate; single quotes group, with '' a literal quote, and
 * may appear anywhere in a token. Returns the count, or -1 on an
 * unterminated quote or more than max tokens.
 */
int
restokenize(char *s, char **tok, int max)
{
	char *w;
	int n, q;

	n = 0;
	for(;;){
		while(*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
			s++;
		if(*s == 0)
			return n;
		if(n >= max)
			return -1;
		tok[n++] = w = s;
		q = 0;
		for(; *s; s++){
			if(*s == '\''){
				if(q && s[1] == '\''){
					*w++ = '\'';
					s++;
					continue;
				}
				q = !q;
				continue;
			}
			if(!q && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n'))
				break;
			*w++ = *s;
		}
		if(q)
			return -1;
		if(*s != 0)
			s++;
		*w = 0;
	}
}

int
reshexval(int c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* undo rbufesc in place; -1 on a bad escape */
int
resunesc(char *s)
{
	char *w;
	int h, l;

	for(w = s; *s; s++){
		if(*s != '\\'){
			*w++ = *s;
			continue;
		}
		switch(*++s){
		case '\\':
			*w++ = '\\';
			break;
		case 'n':
			*w++ = '\r';	/* the Mac line break */
			break;
		case 't':
			*w++ = '\t';
			break;
		case 'x':
			if((h = reshexval(s[1])) < 0 || (l = reshexval(s[2])) < 0)
				return -1;
			h = h<<4 | l;
			if(h == 0 || h >= 0x80)
				return -1;	/* ASCII only: other bytes aren't UTF-8 */
			*w++ = h;
			s += 2;
			break;
		default:
			return -1;
		}
	}
	*w = 0;
	return 0;
}

/* the value of attribute key among tok[0..n), or nil */
char*
resattr(char **tok, int n, char *key)
{
	int i, k;

	k = strlen(key);
	for(i = 0; i < n; i++)
		if(strncmp(tok[i], key, k) == 0 && tok[i][k] == '=')
			return tok[i] + k + 1;
	return nil;
}

/* one rune from UTF-8; returns bytes used, 0 at end, -1 if invalid */
int
resutfrune(char *s, long *r)
{
	uchar *p;
	long c;
	int n, i;

	p = (uchar*)s;
	if(p[0] == 0)
		return 0;
	if(p[0] < 0x80){
		*r = p[0];
		return 1;
	}
	if((p[0] & 0xE0) == 0xC0){
		n = 2;
		c = p[0] & 0x1F;
	}else if((p[0] & 0xF0) == 0xE0){
		n = 3;
		c = p[0] & 0x0F;
	}else if((p[0] & 0xF8) == 0xF0){
		n = 4;
		c = p[0] & 0x07;
	}else
		return -1;
	for(i = 1; i < n; i++){
		if((p[i] & 0xC0) != 0x80)
			return -1;
		c = c<<6 | (p[i] & 0x3F);
	}
	if((n == 2 && c < 0x80) || (n == 3 && c < 0x800) || (n == 4 && c < 0x10000))
		return -1;
	*r = c;
	return n;
}

/*
 * UTF-8 to MacRoman. glyphs maps the system font's control glyphs
 * (command, check, diamond, apple) back to 0x11-0x14, as the text
 * forms write them. -1 if a character has no MacRoman byte.
 */
int
utf2macroman(Rbuf *b, char *s, int glyphs)
{
	long r;
	int n, c;

	while((n = resutfrune(s, &r)) > 0){
		s += n;
		c = -1;
		if(glyphs){
			switch(r){
			case 0x2318:
				c = 0x11;
				break;
			case 0x2713:
				c = 0x12;
				break;
			case 0x25C6:
				c = 0x13;
				break;
			case 0xF8FF:
				c = 0x14;
				break;
			}
		}
		if(c < 0){
			if(r < 0x80)
				c = r;
			else
				for(c = 0x80; c < 0x100; c++)
					if(macromanrune(c) == r)
						break;
		}
		if(c < 0 || c >= 0x100)
			return -1;
		rbufc(b, c);
	}
	return n < 0 ? -1 : 0;
}
