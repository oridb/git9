#include <u.h>
#include <libc.h>
#include "git.h"

#define NCACHE 256
#define TDIR ".git/index9/tracked"
#define RDIR ".git/index9/removed"
#define HDIR "/mnt/git/HEAD/tree"
typedef struct Cache	Cache;
typedef struct Wres	Wres;
struct Cache {
	Dir*	cache;
	int	n;
	int	max;
};

struct Wres {
	char	**path;
	int	npath;
	int	pathsz;
};

enum {
	Rflg	= 1 << 0,
	Mflg	= 1 << 1,
	Aflg	= 1 << 2,
	Tflg	= 1 << 3,
};

Cache seencache[NCACHE];
int quiet;
int printflg;
char *rstr = "R ";
char *tstr = "T ";
char *mstr = "M ";
char *astr = "A ";

int
seen(Dir *dir)
{
	Dir *dp;
	int i;
	Cache *c;

	c = &seencache[dir->qid.path&(NCACHE-1)];
	dp = c->cache;
	for(i=0; i<c->n; i++, dp++)
		if(dir->qid.path == dp->qid.path &&
		   dir->type == dp->type &&
		   dir->dev == dp->dev)
			return 1;
	if(c->n == c->max){
		if (c->max == 0)
			c->max = 8;
		else
			c->max += c->max/2;
		c->cache = realloc(c->cache, c->max*sizeof(Dir));
		if(c->cache == nil)
			sysfatal("realloc: %r");
	}
	c->cache[c->n++] = *dir;
	return 0;
}

int
readpaths(Wres *r, char *pfx, char *dir)
{
	char *f, *sub, *full, *sep;
	Dir *d;
	int fd, ret, i, n;

	ret = -1;
	sep = "";
	if(dir[0] != 0)
		sep = "/";
	if((full = smprint("%s/%s", pfx, dir)) == nil)
		sysfatal("smprint: %r");
	if((fd = open(full, OREAD)) < 0)
		goto error;
	while((n = dirread(fd, &d)) > 0){
		for(i = 0; i < n; i++){
			if(seen(&d[i]))
				continue;
			if(d[i].qid.type & QTDIR){
				if((sub = smprint("%s%s%s", dir, sep, d[i].name)) == nil)
					sysfatal("smprint: %r");
				if(readpaths(r, pfx, sub) == -1){
					free(sub);
					goto error;
				}
				free(sub);
			}else{
				if(r->npath == r->pathsz){
					r->pathsz = 2*r->pathsz + 1;
					r->path = erealloc(r->path, r->pathsz * sizeof(char*));
				}
				if((f = smprint("%s%s%s", dir, sep, d[i].name)) == nil)
					sysfatal("smprint: %r");
				r->path[r->npath++] = f;
			}
		}
	}
	ret = r->npath;
error:
	close(fd);
	free(full);
	free(d);
	return ret;
}

int
cmp(void *pa, void *pb)
{
	return strcmp(*(char **)pa, *(char **)pb);
}

void
dedup(Wres *r)
{
	int i, o;

	if(r->npath <= 1)
		return;
	o = 0;
	qsort(r->path, r->npath, sizeof(r->path[0]), cmp);
	for(i = 1; i < r->npath; i++)
		if(strcmp(r->path[o], r->path[i]) != 0)
			r->path[++o] = r->path[i];
	r->npath = o + 1;
}

static void
findroot(void)
{
	char path[256], buf[256], *p;

	if(access("/mnt/git/ctl", AEXIST) != 0)
		sysfatal("no running git/fs");
	if((getwd(path, sizeof(path))) == nil)
		sysfatal("could not get wd: %r");
	while((p = strrchr(path, '/')) != nil){
		snprint(buf, sizeof(buf), "%s/.git", path);
		if(access(buf, AEXIST) == 0){
			chdir(path);
			return;
		}
		*p = '\0';
	}
	sysfatal("not a git repository");
}

int
sameqid(char *f, char *qf)
{
	char indexqid[64], fileqid[64], *p;
	Dir *d;
	int fd, n;

	if((fd = open(qf, OREAD)) == -1)
		return -1;
	if((n = readn(fd, indexqid, sizeof(indexqid) - 1)) == -1)
		return -1;
	indexqid[n] = 0;
	close(fd);
	if((p = strpbrk(indexqid, "  \t\n\r")) != nil)
		*p = 0;

	if((d = dirstat(f)) == nil)
		return -1;
	snprint(fileqid, sizeof(fileqid), "%ullx.%uld.%.2uhhx",
	    d->qid.path, d->qid.vers, d->qid.type);
	if(strcmp(indexqid, fileqid) == 0)
		return 1;
	return 0;
}

int
samedata(char *pa, char *pb)
{
	char ba[32*1024], bb[32*1024];
	int fa, fb, na, nb, same;

	same = 0;
	fa = open(pa, OREAD);
	fb = open(pb, OREAD);
	if(fa == -1 || fb == -1){
		goto mismatch;
	}
	while(1){
		if((na = readn(fa, ba, sizeof(ba))) == -1)
			goto mismatch;
		if((nb = readn(fb, bb, sizeof(bb))) == -1)
			goto mismatch;
		if(na != nb)
			goto mismatch;
		if(na == 0)
			break;
		if(memcmp(ba, bb, na) != 0)
			goto mismatch;
	}
	same = 1;
mismatch:
	if(fa != -1)
		close(fa);
	if(fb != -1)
		close(fb);
	return same;
}

void
usage(void)
{
	fprint(2, "usage: %s [-qbc] [-f filt]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char rmpath[256], tpath[256], bpath[256], buf[8];
	char *p, *e;
	int i, dirty;
	Wres r;

	ARGBEGIN{
	case 'q':
		quiet++;
		break;
	case 'c':
		rstr = "";
		tstr = "";
		mstr = "";
		astr = "";
		break;
	case 'f':
		for(p = EARGF(usage()); *p; p++)
			switch(*p){
			case 'T':	printflg |= Tflg;	break;
			case 'A':	printflg |= Aflg;	break;
			case 'M':	printflg |= Mflg;	break;
			case 'R':	printflg |= Rflg;	break;
			default:	usage();		break;
		}
		break;
	default:
		usage();
	}ARGEND

	findroot();
	dirty = 0;
	r.path = nil;
	r.npath = 0;
	r.pathsz = 0;
	if(access("/mnt/git/ctl", AEXIST) != 0)
		sysfatal("git/fs does not seem to be running");
	if(printflg == 0)
		printflg = Tflg | Aflg | Mflg | Rflg;
	if(access(TDIR, AEXIST) == 0 && readpaths(&r, TDIR, "") == -1)
		sysfatal("read tracked: %r");
	if(access(RDIR, AEXIST) == 0 && readpaths(&r, RDIR, "") == -1)
		sysfatal("read removed: %r");
	dedup(&r);

	for(i = 0; i < r.npath; i++){
		p = r.path[i];
		snprint(rmpath, sizeof(rmpath), RDIR"/%s", p);
		snprint(tpath, sizeof(tpath), TDIR"/%s", p);
		snprint(bpath, sizeof(bpath), HDIR"/%s", p);
		if(access(p, AEXIST) != 0 || access(rmpath, AEXIST) == 0){
			dirty |= Mflg;
			if(!quiet && (printflg & Rflg))
				print("%s%s\n", rstr, p);
		}else if(access(bpath, AEXIST) == -1) {
			dirty |= Aflg;
			if(!quiet && (printflg & Aflg))
				print("%s%s\n", astr, p);
		}else if(!sameqid(p, tpath) && !samedata(p, bpath)){
			dirty |= Mflg;
			if(!quiet && (printflg & Mflg))
				print("%s%s\n", mstr, p);
		}else{
			if(!quiet && (printflg & Tflg))
				print("%s%s\n", tstr, p);
		}
	}
	if(!dirty)
		exits(nil);

	p = buf;
	e = buf + sizeof(buf);
	for(i = 0; (1 << i) != Tflg; i++)
		if(dirty & (1 << i))
			p = seprint(p, e, "%c", "DMAT"[i]);
	exits(buf);
}
