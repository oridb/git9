#include <u.h>
#include <libc.h>

#include "git.h"

#pragma	varargck	type	"P"	void

int fullpath;
int changes;
char *path[128];
int npath;

int
Pfmt(Fmt *f)
{
	int i, n;

	n = 0;
	for(i = 0; i < npath; i++)
		n += fmtprint(f, "%s/", path[i]);
	return n;
}

void
difftrees(Hash ah, Hash bh)
{
	Dirent *ap, *bp, *ae, *be;
	Object *a, *b;
	int c;

	if((a = readobject(ah)) == nil)
		sysfatal("bad hash %H", ah);
	if((b = readobject(bh)) == nil)
		sysfatal("bad hash %H", bh);
	if(a->type != b->type)
		return;
	switch(a->type){
	case GCommit:
		difftrees(a->commit->tree, b->commit->tree);
		break;
	case GTree:
		ap = a->tree->ent;
		ae = ap + a->tree->nent;
		bp = b->tree->ent;
		be = bp + b->tree->nent;
		while(ap != ae && bp != be){
			c = strcmp(ap->name, bp->name);
			if(c == 0){
				if(ap->mode != bp->mode || !hasheq(&ap->h, &bp->h)) {
					if(!(ap->mode & DMDIR) || !(bp->mode & DMDIR))
						print("~ %P%s\n", ap->name);
					if(ap->mode & DMDIR || bp->mode & DMDIR){
						if(npath >= nelem(path))
						sysfatal("path too deep");
						path[npath++] = ap->name;
						difftrees(ap->h, bp->h);
						npath--;
					}
				}
				ap++;
				bp++;
			}else if(c < 0){
				print("- %P%s\n", ap->name);
				ap++;
			}else if(c > 0){
				print("+ %P%s\n", bp->name);
				bp++;
			}
		}
		break;
	}
}


void
usage(void)
{
	fprint(2, "usage: %s [-pc] query\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	int i, j, n;
	Hash *h;
	char *p, *e;
	char query[2048];

	ARGBEGIN{
	case 'p':	fullpath++;	break;
	case 'c':	changes++;	break;
	default:	usage();	break;
	}ARGEND;

	gitinit();
	fmtinstall('P', Pfmt);
	p = query;
	e = query + nelem(query);
	for(i = 0; i < argc; i++)
		p = seprint(p, e, "%s ", argv[i]);
	if((n = resolverefs(&h, query)) == -1)
		sysfatal("resolve %s: %r", argv[i]);
	if(changes){
		if(n != 2)
			sysfatal("diff: need 2 commits, got %d", n);
		difftrees(h[0], h[1]);
	}else{
		p = (fullpath ? "/mnt/git/object/" : "");
		for(j = 0; j < n; j++)
			print("%s%H\n", p, h[j]);
	}
	exits(nil);
}

