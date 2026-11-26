# Host build of libux's terminal emulator (vt.c), for its tests (GNU make).
# Inside 9front or h9, use mk instead (see mkfile).
#
#	make test            build test/vthost with sanitizers and run it

CC ?= cc
CFLAGS ?= -O1 -g -Wall -Wextra -Wno-sign-compare -Wno-unused-parameter
SAN ?= -fsanitize=address,undefined -fno-sanitize-recover=all
O = host

all: $(O)/vthost

$(O):
	mkdir -p $(O)

$(O)/vthost: test/vthost.c vt.c vt.h | $(O)
	$(CC) -DVTHOST $(CFLAGS) $(SAN) -o $@ test/vthost.c vt.c

test: $(O)/vthost
	$(O)/vthost

clean:
	rm -rf $(O)
