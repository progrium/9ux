/*
 * A stand-in for libFuzzer where it isn't installed (gcc, or clang
 * without compiler-rt): runs the target on each file named, then, with
 * -n count, on that many random mutations of them. Build it with
 * -fsanitize=address,undefined so a bad read stops the run.
 *
 *	driver [-n count] [-s seed] file...
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

typedef struct { uint8_t *p; size_t n; } Seed;

static uint32_t rng;

static uint32_t
next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void
mutate(uint8_t *p, size_t *n, size_t cap)
{
	int k, i;
	size_t at, len;
	static const uint8_t magic[] = {0x00, 0xFF, 0x7F, 0x80, 0x01, 0xFE};

	for(k = 1 + next() % 8; k > 0; k--){
		if(*n == 0)
			return;
		at = next() % *n;
		switch(next() % 6){
		case 0:	/* flip a bit */
			p[at] ^= 1 << (next() % 8);
			break;
		case 1:	/* interesting byte */
			p[at] = magic[next() % sizeof magic];
			break;
		case 2:	/* random byte run */
			for(i = 0; i < 4 && at+i < *n; i++)
				p[at+i] = next();
			break;
		case 3:	/* truncate */
			*n = at;
			break;
		case 4:	/* copy a run within the file */
			len = next() % 16;
			if(len < *n){
				size_t from = next() % (*n - len);
				if(at + len <= *n)
					memmove(p+at, p+from, len);
			}
			break;
		case 5:	/* big-endian 16-bit edge value */
			if(at+1 < *n){
				uint16_t v = (next() & 1) ? 0xFFFF : (uint16_t)(next() % 0x200);
				p[at] = v >> 8;
				p[at+1] = v;
			}
			break;
		}
	}
	(void)cap;
}

int
main(int argc, char **argv)
{
	long count, i;
	Seed *seeds;
	int nseed, a;
	FILE *f;
	uint8_t *buf;
	size_t n, cap;

	count = 0;
	rng = 0x9a9a9a9a;
	seeds = calloc(argc, sizeof *seeds);
	nseed = 0;
	for(a = 1; a < argc; a++){
		if(strcmp(argv[a], "-n") == 0 && a+1 < argc){
			count = atol(argv[++a]);
			continue;
		}
		if(strcmp(argv[a], "-s") == 0 && a+1 < argc){
			rng = strtoul(argv[++a], NULL, 0) | 1;
			continue;
		}
		f = fopen(argv[a], "rb");
		if(f == NULL){
			perror(argv[a]);
			return 1;
		}
		seeds[nseed].p = malloc(1<<20);
		seeds[nseed].n = fread(seeds[nseed].p, 1, 1<<20, f);
		fclose(f);
		LLVMFuzzerTestOneInput(seeds[nseed].p, seeds[nseed].n);
		nseed++;
	}
	if(nseed == 0 || count == 0){
		for(a = 0; a < nseed; a++)
			free(seeds[a].p);
		free(seeds);
		return 0;
	}
	cap = 1<<20;
	buf = malloc(cap);
	for(i = 0; i < count; i++){
		Seed *s = &seeds[next() % nseed];
		n = s->n;
		memcpy(buf, s->p, n);
		mutate(buf, &n, cap);
		LLVMFuzzerTestOneInput(buf, n);
	}
	printf("%ld inputs, %d seeds, no crashes\n", count, nseed);
	free(buf);
	for(a = 0; a < nseed; a++)
		free(seeds[a].p);
	free(seeds);
	return 0;
}
