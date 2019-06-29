#include <u.h>
#include <libc.h>

#include "git.h"

int fullpath;

void
usage(void)
{
	fprint(2, "usage: %s [-p]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	int i, j, n;
	Hash *h;

	ARGBEGIN{
	case 'p':	fullpath++;	break;
	default:	usage();	break;
	}ARGEND;

	gitinit();
	for(i = 0; i < argc; i++){
		if((n = resolverefs(&h, argv[i])) == -1)
			sysfatal("resolve %s: %r", argv[i]);
		for(j = 0; j < n; j++)
			if(fullpath)
				print("/mnt/git/object/%H\n", h[j]);
			else
				print("%H\n", h[j]);
	}
}
