/*
 * libFuzzer target: parse a resource file and run every decoder on
 * every resource. Built on the host only (see ../Makefile).
 */
#include "../rescompat.h"
#include "../res.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

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
		if(istexttype(r))
			restext(r, &b, err, sizeof err);
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
