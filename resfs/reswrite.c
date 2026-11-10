/*
 * Writing a resource file: the inverse of resparse.
 */
#include "rescompat.h"
#include "res.h"

static int
seterr(char *err, int nerr, char *msg)
{
	if(err != nil && nerr > 0){
		strncpy(err, msg, nerr-1);
		err[nerr-1] = 0;
	}
	return -1;
}

static int
cmp(Res *a, Res *b)
{
	int c;

	c = memcmp(a->type, b->type, 4);
	if(c != 0)
		return c;
	return a->id < b->id ? -1 : a->id > b->id;
}

#ifdef RESHOST
static int
qcmp(const void *a, const void *b)
{
	return cmp(*(Res**)a, *(Res**)b);
}
#else
static int
qcmp(void *a, void *b)
{
	return cmp(*(Res**)a, *(Res**)b);
}
#endif

static void
put16(Rbuf *b, u32int v)
{
	rbufc(b, v>>8);
	rbufc(b, v);
}

static void
put32(Rbuf *b, u32int v)
{
	put16(b, v>>16);
	put16(b, v);
}

/*
 * Lay out res[0..n) as a resource file in out: the header, 240 bytes
 * of zeros (the usual data offset of 256), the data, then the map.
 * Names are UTF-8 and are written as MacRoman.
 */
int
reswrite(Res *res, int n, int mapattr, Rbuf *out, char *err, int nerr)
{
	Res **s;
	Rbuf data, names, refs, types;
	u32int doff, ntypes, mapoff, maplen, tl, nl, start;
	int i, j, r;
	uchar z[240];

	if(n < 0 || n > Resmaxres)
		return seterr(err, nerr, "too many resources");
	s = malloc((n > 0 ? n : 1) * sizeof *s);
	if(s == nil)
		return seterr(err, nerr, "out of memory");
	for(i = 0; i < n; i++)
		s[i] = &res[i];
	if(n > 0)
		qsort(s, n, sizeof *s, qcmp);
	for(i = 1; i < n; i++)
		if(cmp(s[i-1], s[i]) == 0){
			free(s);
			return seterr(err, nerr, "the same type and ID twice");
		}

	rbufinit(&data);
	rbufinit(&names);
	rbufinit(&refs);
	rbufinit(&types);
	r = -1;
	ntypes = 0;
	for(i = 0; i < n; i = j){
		for(j = i; j < n && memcmp(s[j]->type, s[i]->type, 4) == 0; j++)
			;
		ntypes++;
	}
	start = 2 + 8*ntypes;	/* ref lists start after the type list */
	if(n > 0)
		put16(&types, ntypes - 1);
	else
		put16(&types, 0xFFFF);
	for(i = 0; i < n; i = j){
		for(j = i; j < n && memcmp(s[j]->type, s[i]->type, 4) == 0; j++)
			;
		if(start + refs.n > 0xFFFF){
			seterr(err, nerr, "resource map too large");
			goto out;
		}
		rbufput(&types, s[i]->type, 4);
		put16(&types, j - i - 1);
		put16(&types, start + refs.n);
		for(; i < j; i++){
			doff = data.n;
			if(doff > 0xFFFFFF){
				seterr(err, nerr, "data area over 16 MB");
				goto out;
			}
			put32(&data, s[i]->len);
			rbufput(&data, s[i]->data, s[i]->len);
			put16(&refs, s[i]->id & 0xFFFF);
			if(s[i]->name != nil){
				if(names.n >= 0xFFFF){
					seterr(err, nerr, "name list too large");
					goto out;
				}
				put16(&refs, names.n);
				rbufc(&names, 0);
				start = names.n;
				if(utf2macroman(&names, s[i]->name, 0) < 0){
					seterr(err, nerr, "a name is not representable in MacRoman");
					goto out;
				}
				if(names.n - start > 255){
					seterr(err, nerr, "a name is over 255 bytes");
					goto out;
				}
				if(!names.err)
					names.p[start-1] = names.n - start;
				start = 2 + 8*ntypes;
			}else
				put16(&refs, 0xFFFF);
			rbufc(&refs, s[i]->attr);
			rbufc(&refs, doff>>16);
			rbufc(&refs, doff>>8);
			rbufc(&refs, doff);
			put32(&refs, 0);
		}
	}
	tl = 28;
	nl = tl + types.n + refs.n;
	if(nl > 0xFFFF){
		seterr(err, nerr, "resource map too large");
		goto out;
	}
	if(data.err || names.err || refs.err || types.err){
		seterr(err, nerr, "out of memory");
		goto out;
	}
	mapoff = 256 + data.n;
	maplen = nl + names.n;
	put32(out, 256);
	put32(out, mapoff);
	put32(out, data.n);
	put32(out, maplen);
	memset(z, 0, sizeof z);
	rbufput(out, z, sizeof z);
	rbufput(out, data.p, data.n);
	start = out->n;
	rbufput(out, z, 16);	/* the header copy, filled in below */
	put32(out, 0);		/* next map */
	put16(out, 0);		/* file ref */
	put16(out, mapattr);
	put16(out, tl);
	put16(out, nl);
	rbufput(out, types.p, types.n);
	rbufput(out, refs.p, refs.n);
	rbufput(out, names.p, names.n);
	if(out->err){
		seterr(err, nerr, "out of memory");
		goto out;
	}
	memmove(out->p + start, out->p, 16);
	r = 0;
out:
	rbuffree(&data);
	rbuffree(&names);
	rbuffree(&refs);
	rbuffree(&types);
	free(s);
	return r;
}
