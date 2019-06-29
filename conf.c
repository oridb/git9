#include <u.h>
#include <libc.h>
#include <ctype.h>

#include "git.h"

static int
showconf(char *cfg, char *sect, char *key)
{
	char *ln, *p;
	Biobuf *f;
	int foundsect, nsect, nkey;

	if((f = Bopen(cfg, OREAD)) == nil)
		return 0;

	nsect = sect ? strlen(sect) : 0;
	nkey = strlen(key);
	foundsect = (sect == nil);
	while((ln = Brdstr(f, '\n', 1)) != nil){
		p = strip(ln);
		if(*p == '[' && sect){
			foundsect = strncmp(sect, ln, nsect) == 0;
		}else if(foundsect && strncmp(p, key, nkey) == 0){
			p = strip(p + nkey);
			if(*p != '=')
				continue;
			p = strip(p + 1);
			print("%s\n", p);
			free(ln);
			return 1;
		}
		free(ln);
	}
	return 0;
}

static void
showroot(void)
{
	char path[256], buf[256], *p;

	if((getwd(path, sizeof(path))) == nil)
		sysfatal("could not get wd: %r");
	while((p = strrchr(path, '/')) != nil){
		snprint(buf, sizeof(buf), "%s/.git", path);
		if(access(buf, AEXIST) == 0){
			print("%s\n", path);
			return;
		}
		*p = '\0';
	}
	sysfatal("not a git repository");
}


void
usage(void)
{
	fprint(2, "usage: %s [-f file] [-r] keys..\n", argv0);
	fprint(2, "\t-f:	use file 'file' (default: .git/config)\n");
	fprint(2, "\t r:	print repository root\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *file[32], *p, *s;
	int i, j, nfile, findroot;

	nfile = 0;
	findroot = 0;
	ARGBEGIN{
	case 'f':	file[nfile++]=EARGF(usage());	break;
	case 'r':	findroot++;			break;
	default:	usage();			break;
	}ARGEND;

	if(findroot)
		showroot();
	if(nfile == 0){
		file[nfile++] = ".git/config";
		if((p = getenv("home")) != nil)
			file[nfile++] = smprint("%s/lib/git/config", p);
	}

	for(i = 0; i < argc; i++){
		if((p = strchr(argv[i], '.')) == nil){
			s = nil;
			p = argv[i];
		}else{
			*p = 0;
			p++;
			s = smprint("[%s]", argv[i]);
		}
		for(j = 0; j < nfile; j++)
			if(showconf(file[j], s, p))
				break;
	}
	exits(nil);
}
