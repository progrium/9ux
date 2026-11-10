/*
 * Resource map parser.
 *
 * Layout (all big-endian):
 *	header	data offset[4] map offset[4] data length[4] map length[4]
 *	data	per resource: length[4] bytes[length]
 *	map	header copy[16] next map[4] file ref[2] attributes[2]
 *		type list offset[2] name list offset[2]   (from map start)
 *	types	count-1[2], then per type: type[4] count-1[2] ref list offset[2]
 *		(ref list offset is from the type list start)
 *	refs	id[2] name offset[2] attributes[1] data offset[3] handle[4]
 *		(name offset is from the name list start, 0xFFFF for none;
 *		data offset is from the data area start)
 *	names	Pascal strings: length[1] bytes[length], MacRoman
 */
#include "rescompat.h"
#include "res.h"

enum {
	Hdrlen = 16,
	Maphdrlen = 28,
	Typelen = 8,
	Reflen = 12,
};

static u32int
get16(uchar *p)
{
	return (p[0]<<8) | p[1];
}

static u32int
get24(uchar *p)
{
	return ((u32int)p[0]<<16) | (p[1]<<8) | p[2];
}

static u32int
get32(uchar *p)
{
	return ((u32int)p[0]<<24) | ((u32int)p[1]<<16) | (p[2]<<8) | p[3];
}

static int
seterr(char *err, int nerr, char *msg)
{
	if(err != nil && nerr > 0){
		strncpy(err, msg, nerr-1);
		err[nerr-1] = 0;
	}
	return -1;
}

/* does [off, off+len) fit within [0, size)? without overflow */
static int
fits(u32int off, u32int len, u32int size)
{
	return off <= size && len <= size - off;
}

typedef struct Sortref Sortref;
struct Sortref {
	Res	r;
	int	ord;
};

static int
rescmp(void *va, void *vb)
{
	Sortref *a, *b;
	int c;

	a = va;
	b = vb;
	c = memcmp(a->r.type, b->r.type, 4);
	if(c != 0)
		return c;
	if(a->r.id != b->r.id)
		return a->r.id < b->r.id ? -1 : 1;
	return a->ord - b->ord;
}

#ifdef RESHOST
static int
qcmp(const void *a, const void *b)
{
	return rescmp((void*)a, (void*)b);
}
#define rqsort(p, n, sz) qsort(p, n, sz, qcmp)
#else
#define rqsort(p, n, sz) qsort(p, n, sz, rescmp)
#endif

int
resparse(uchar *buf, u32int n, Resfile *rf, char *err, int nerr)
{
	u32int dataoff, mapoff, datalen, maplen, tl, nl, ntypes, total;
	u32int i, j, cnt, refoff, roff, doff, len, noff, plen;
	uchar *map, *t, *ref;
	Sortref *s;
	Res *r;
	int ns, k;

	memset(rf, 0, sizeof *rf);
	if(buf == nil || n < Hdrlen)
		return seterr(err, nerr, "file too short for a resource header");
	dataoff = get32(buf);
	mapoff = get32(buf+4);
	datalen = get32(buf+8);
	maplen = get32(buf+12);
	if(!fits(dataoff, datalen, n))
		return seterr(err, nerr, "data area outside the file");
	if(!fits(mapoff, maplen, n))
		return seterr(err, nerr, "resource map outside the file");
	if(maplen < Maphdrlen)
		return seterr(err, nerr, "resource map too short");
	map = buf + mapoff;
	rf->attr = get16(map+22);
	tl = get16(map+24);
	nl = get16(map+26);
	if(!fits(tl, 2, maplen))
		return seterr(err, nerr, "type list outside the map");
	ntypes = (get16(map+tl) + 1) & 0xFFFF;
	if(!fits(tl+2, ntypes*Typelen, maplen))
		return seterr(err, nerr, "type list overruns the map");

	/* count refs and check every ref list before allocating */
	total = 0;
	for(i = 0; i < ntypes; i++){
		t = map + tl + 2 + i*Typelen;
		cnt = get16(t+4) + 1;
		refoff = get16(t+6);
		if(!fits(tl + refoff, cnt*Reflen, maplen))
			return seterr(err, nerr, "reference list overruns the map");
		total += cnt;
		if(total > Resmaxres)
			return seterr(err, nerr, "too many resources");
	}

	s = nil;
	if(total > 0){
		s = malloc(total * sizeof *s);
		if(s == nil)
			return seterr(err, nerr, "out of memory");
		memset(s, 0, total * sizeof *s);
	}
	ns = 0;
	for(i = 0; i < ntypes; i++){
		t = map + tl + 2 + i*Typelen;
		cnt = get16(t+4) + 1;
		refoff = get16(t+6);
		for(j = 0; j < cnt; j++){
			roff = tl + refoff + j*Reflen;
			ref = map + roff;
			doff = get24(ref+5);
			if(datalen < 4 || doff > datalen - 4){
				rf->nbad++;
				continue;
			}
			len = get32(buf + dataoff + doff);
			if(len > datalen - 4 - doff){
				rf->nbad++;
				continue;
			}
			r = &s[ns].r;
			s[ns].ord = ns;
			memmove(r->type, t, 4);
			r->id = (short)get16(ref);
			r->attr = ref[4];
			r->data = buf + dataoff + doff + 4;
			r->len = len;
			r->name = nil;
			noff = get16(ref+2);
			if(noff != 0xFFFF && fits(nl + noff, 1, maplen)){
				plen = map[nl + noff];
				if(fits(nl + noff + 1, plen, maplen)){
					r->name = macromanstr(map + nl + noff + 1, plen);
					if(r->name == nil){
						for(k = 0; k < ns; k++)
							free(s[k].r.name);
						free(s);
						return seterr(err, nerr, "out of memory");
					}
				}
			}
			ns++;
		}
	}

	if(ns > 0)
		rqsort(s, ns, sizeof *s);
	rf->res = nil;
	if(ns > 0){
		rf->res = malloc(ns * sizeof(Res));
		if(rf->res == nil){
			for(k = 0; k < ns; k++)
				free(s[k].r.name);
			free(s);
			return seterr(err, nerr, "out of memory");
		}
	}
	/* drop repeated (type, id), keeping the first in map order */
	for(k = 0; k < ns; k++){
		if(rf->nres > 0 && memcmp(rf->res[rf->nres-1].type, s[k].r.type, 4) == 0
		&& rf->res[rf->nres-1].id == s[k].r.id){
			free(s[k].r.name);
			rf->ndup++;
			continue;
		}
		rf->res[rf->nres++] = s[k].r;
	}
	free(s);
	return 0;
}

void
resfree(Resfile *rf)
{
	int i;

	for(i = 0; i < rf->nres; i++)
		free(rf->res[i].name);
	free(rf->res);
	memset(rf, 0, sizeof *rf);
}

int
restypeeq(Res *r, char *type)
{
	return strlen(type) == 4 && memcmp(r->type, type, 4) == 0;
}

Res*
reslookupcode(Resfile *rf, uchar type[4], int id)
{
	int lo, hi, m, c;
	Res *r;

	lo = 0;
	hi = rf->nres;
	while(lo < hi){
		m = (lo + hi) / 2;
		r = &rf->res[m];
		c = memcmp(r->type, type, 4);
		if(c == 0)
			c = r->id < id ? -1 : r->id > id;
		if(c == 0)
			return r;
		if(c < 0)
			lo = m + 1;
		else
			hi = m;
	}
	return nil;
}

Res*
reslookup(Resfile *rf, char *type, int id)
{
	if(strlen(type) != 4)
		return nil;
	return reslookupcode(rf, (uchar*)type, id);
}

static int
unhex(int c)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

/* the inverse of resescname; -1 unless it names exactly four bytes */
int
resunescname(char *s, uchar type[4])
{
	int n, hi, lo;

	for(n = 0; n < 4; n++){
		if(*s == 0)
			return -1;
		if(*s == '%'){
			if(s[1] == 0 || (hi = unhex(s[1])) < 0 || (lo = unhex(s[2])) < 0)
				return -1;
			type[n] = hi<<4 | lo;
			s += 3;
		}else
			type[n] = *s++;
	}
	return *s == 0 ? 0 : -1;
}

/* one line a resource: type id size=n attr=0xNN [name=...] */
void
resindex(Resfile *rf, Rbuf *out)
{
	char t[16];
	Res *r;
	int i;

	for(i = 0; i < rf->nres; i++){
		r = &rf->res[i];
		resescname(r->type, t, sizeof t);
		rbufs(out, t);
		rbufc(out, ' ');
		rbufint(out, r->id);
		rbufkvint(out, "size", r->len);
		rbufs(out, " attr=0x");
		rbufhex(out, r->attr, 2);
		if(r->name != nil)
			rbufkv(out, "name", r->name);
		rbufc(out, '\n');
	}
}
