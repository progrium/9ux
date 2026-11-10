# Host build of the resfs parser, for tests and fuzzing (GNU make).
# Inside 9front or h9, use mk instead (see mkfile).
#
#	make test            build rescat with sanitizers, run the golden tests
#	make fuzz            libFuzzer (clang with compiler-rt): FUZZTIME seconds
#	make fuzz-driver     no libFuzzer: random mutations through the driver
#	make fixtures        regenerate test/fixtures and test/golden

CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter
SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=all
O = host
LIBSRC = res.c rbuf.c macroman.c icon.c text.c resload.c token.c encode.c image.c reswrite.c
HDR = rescompat.h res.h
FUZZTIME ?= 60
FUZZRUNS ?= 200000

all: $(O)/rescat $(O)/rescomp

$(O):
	mkdir -p $(O)

$(O)/rescat: rescat.c $(LIBSRC) $(HDR) | $(O)
	$(CC) -DRESHOST $(CFLAGS) $(SAN) -o $@ rescat.c $(LIBSRC)

$(O)/rescomp: rescomp.c $(LIBSRC) $(HDR) | $(O)
	$(CC) -DRESHOST $(CFLAGS) $(SAN) -o $@ rescomp.c $(LIBSRC)

test: $(O)/rescat $(O)/rescomp
	test/run.sh $(O)/rescat
	test/comp.sh $(O)/rescat $(O)/rescomp

$(O)/fuzzres: fuzz/fuzzres.c $(LIBSRC) $(HDR) | $(O)
	clang -DRESHOST $(CFLAGS) -fsanitize=fuzzer,address,undefined -o $@ fuzz/fuzzres.c $(LIBSRC)

fuzz: $(O)/fuzzres
	mkdir -p $(O)/corpus
	$(O)/fuzzres -max_total_time=$(FUZZTIME) -max_len=65536 $(O)/corpus test/fixtures fuzz/seeds

$(O)/fuzzdriver: fuzz/fuzzres.c fuzz/driver.c $(LIBSRC) $(HDR) | $(O)
	$(CC) -DRESHOST $(CFLAGS) $(SAN) -o $@ fuzz/fuzzres.c fuzz/driver.c $(LIBSRC)

fuzz-driver: $(O)/fuzzdriver
	$(O)/fuzzdriver -n $(FUZZRUNS) test/fixtures/* test/fixtures/._* fuzz/seeds/*

fixtures:
	python3 test/mkfixtures.py

clean:
	rm -rf $(O)

.PHONY: all test fuzz fuzz-driver fixtures clean
