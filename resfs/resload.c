/*
 * Reading a whole resource file into memory, up to Resmaxfile.
 */
#include "rescompat.h"
#include "res.h"

static void*
seterrp(char *err, int nerr, char *msg)
{
	if(err != nil && nerr > 0){
		strncpy(err, msg, nerr-1);
		err[nerr-1] = 0;
	}
	return nil;
}

uchar*
resreadfile(char *path, u32int *np, char *err, int nerr)
{
	uchar *buf, *nb;
	u32int n, cap;
	int fd;
	long r;

	fd = open(path, OREAD);
	if(fd < 0)
		return seterrp(err, nerr, "cannot open");
	n = 0;
	cap = 8192;
	buf = malloc(cap);
	if(buf == nil){
		close(fd);
		return seterrp(err, nerr, "out of memory");
	}
	for(;;){
		if(n == cap){
			if(cap >= Resmaxfile){
				free(buf);
				close(fd);
				return seterrp(err, nerr, "file too large");
			}
			cap *= 2;
			nb = realloc(buf, cap);
			if(nb == nil){
				free(buf);
				close(fd);
				return seterrp(err, nerr, "out of memory");
			}
			buf = nb;
		}
		r = read(fd, buf+n, cap-n);
		if(r < 0){
			free(buf);
			close(fd);
			return seterrp(err, nerr, "read error");
		}
		if(r == 0)
			break;
		n += r;
	}
	close(fd);
	*np = n;
	return buf;
}
