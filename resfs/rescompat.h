/*
 * The resource parser and decoders build on 9front and on the host.
 * On 9front this pulls in <u.h> and <libc.h>; host builds define
 * RESHOST and get the same names from the C library.
 */
#ifdef RESHOST
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef uint8_t u8int;
typedef uint16_t u16int;
typedef uint32_t u32int;
typedef long long vlong;
#define nil ((void*)0)
#define USED(x) ((void)(x))
#define snprint snprintf
#define OREAD O_RDONLY
#define OWRITE O_WRONLY
#define create(f, m, p) open(f, O_WRONLY|O_CREAT|O_TRUNC, p)
#define exits(s) exit((s) == nil ? 0 : 1)
#else
#include <u.h>
#include <libc.h>
#endif
