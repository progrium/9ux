/*
 * libFuzzer target: parse a resource file and run every decoder on
 * every resource. Built on the host only (see ../Makefile).
 */
#include "../rescompat.h"
#include "../res.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

/* encode a decoded text form back (what rescomp -d does) */
static void
reencode(Res *r, Rbuf *text)
{
	char *copy, *lines[256];
	u32int k;
	int n, eline;
	Rbuf enc;

	copy = malloc(text->n + 1);
	if(copy == NULL)
		return;
	if(text->n > 0)
		memcpy(copy, text->p, text->n);
	copy[text->n] = 0;
	n = 0;
	lines[n++] = copy;
	for(k = 0; k < text->n && n < 256; k++)
		if(copy[k] == '\n'){
			copy[k] = 0;
			lines[n++] = copy + k + 1;
		}
	if(n > 0 && *lines[n-1] == 0)
		n--;
	rbufinit(&enc);
	restextenc(r->type, lines, n, &enc, NULL, 0, &eline);
	rbuffree(&enc);
	free(copy);
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	char err[128];
	uchar *buf;
	Resfile rf;
	Rbuf b;
	Res *r;
	int i;

	if(size > 1<<20)
		return 0;
	/* a private copy, so reads past the end are caught */
	buf = malloc(size ? size : 1);
	if(buf == NULL)
		return 0;
	memcpy(buf, data, size);
	{
		/* the same bytes as an image(6) file, as rescomp reads them */
		int w, h;
		u32int *px;
		Rbuf ib;
		if(resreadimage(buf, size, &w, &h, &px, err, sizeof err) == 0){
			rbufinit(&ib);
			if(w == 32 && h == 32)
				resiconenc((uchar*)"icl8", w, h, px, px, &ib, err, sizeof err);
			rbuffree(&ib);
			free(px);
		}
	}
	if(resparse(buf, size, &rf, err, sizeof err) < 0){
		free(buf);
		return 0;
	}
	rbufinit(&b);
	resindex(&rf, &b);
	rbuffree(&b);
	for(i = 0; i < rf.nres; i++){
		r = &rf.res[i];
		if(reslookupcode(&rf, r->type, r->id) != r)
			abort();
		if(istexttype(r) && restext(r, &b, err, sizeof err) == 0)
			reencode(r, &b);
		rbuffree(&b);
		if(isicontype(r))
			resimage(&rf, r, &b, err, sizeof err);
		rbuffree(&b);
		if(restypeeq(r, "CURS"))
			rescursor(r, &b, err, sizeof err);
		rbuffree(&b);
		resicon(&rf, r->id, Iconlarge, &b, err, sizeof err);
		rbuffree(&b);
		resicon(&rf, r->id, Iconsmall, &b, err, sizeof err);
		rbuffree(&b);
	}
	resfree(&rf);
	free(buf);
	return 0;
}
