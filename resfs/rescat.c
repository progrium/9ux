/*
 * rescat: print what resfs would serve, without 9P.
 *
 *	rescat file                    the index
 *	rescat file type id [form]     raw (default), text, image, cursor
 *	rescat file icon id [size]     image (32x32, default) or small (16x16)
 *
 * type is the escaped name resfs uses (STR%20 for 'STR ').
 * Used by the tests on the host and in h9, and handy for inspecting
 * imported files.
 */
#include "rescompat.h"
#include "res.h"

static char *argv0name = "rescat";

static void
fatal(char *msg, char *arg)
{
	char buf[256];
	int n;

	n = snprint(buf, sizeof buf, "%s: %s%s%s\n", argv0name, arg ? arg : "", arg ? ": " : "", msg);
	if(n > 0 && write(2, buf, strlen(buf)) < 0)
		n = 0;
	exits("error");
}

static void
out(Rbuf *b)
{
	u32int off;
	long w;

	for(off = 0; off < b->n; off += w){
		w = write(1, b->p+off, b->n-off);
		if(w <= 0)
			fatal("write error", nil);
	}
}

static void
finish(Rbuf *b, Resfile *rf, uchar *buf)
{
	out(b);
	rbuffree(b);
	resfree(rf);
	free(buf);
	exits(nil);
}

#ifdef RESHOST
int
#else
void
#endif
main(int argc, char **argv)
{
	char err[128], *form;
	uchar *buf, type[4];
	u32int n;
	Resfile rf;
	Rbuf b;
	Res *r;
	int id, rv;

	if(argc != 2 && argc != 4 && argc != 5)
		fatal("usage: rescat file [type id [form]]", nil);
	buf = resreadfile(argv[1], &n, err, sizeof err);
	if(buf == nil)
		fatal(err, argv[1]);
	if(resparse(buf, n, &rf, err, sizeof err) < 0)
		fatal(err, argv[1]);
	rbufinit(&b);
	if(argc == 2){
		resindex(&rf, &b);
		finish(&b, &rf, buf);
	}
	id = atoi(argv[3]);
	form = argc == 5 ? argv[4] : nil;
	if(strcmp(argv[2], "icon") == 0){
		if(form == nil || strcmp(form, "image") == 0)
			rv = resicon(&rf, id, Iconlarge, &b, err, sizeof err);
		else if(strcmp(form, "small") == 0)
			rv = resicon(&rf, id, Iconsmall, &b, err, sizeof err);
		else
			fatal("unknown icon form", form);
		if(rv < 0)
			fatal(err, nil);
		finish(&b, &rf, buf);
	}
	if(resunescname(argv[2], type) < 0)
		fatal("bad type name", argv[2]);
	r = reslookupcode(&rf, type, id);
	if(r == nil)
		fatal("no such resource", nil);
	if(form == nil || strcmp(form, "raw") == 0){
		rbufput(&b, r->data, r->len);
		rv = 0;
	}else if(strcmp(form, "text") == 0)
		rv = restext(r, &b, err, sizeof err);
	else if(strcmp(form, "image") == 0)
		rv = resimage(&rf, r, &b, err, sizeof err);
	else if(strcmp(form, "cursor") == 0)
		rv = restypeeq(r, "CURS") ? rescursor(r, &b, err, sizeof err) : -1;
	else
		fatal("unknown form", form);
	if(rv < 0)
		fatal(restypeeq(r, "CURS") || strcmp(form, "cursor") != 0 ? err : "not a cursor", nil);
	finish(&b, &rf, buf);
}
