#include <u.h>
#include <libc.h>
#include "git.h"

typedef struct Seen	Seen;
typedef struct Idxed	Idxed;
typedef struct Idxent	Idxent;

#define NCACHE 4096

enum {
	Rflg	= 1 << 0,
	Mflg	= 1 << 1,
	Aflg	= 1 << 2,
	Uflg	= 1 << 3,
	/* everything after this is not an error */
	Tflg	= 1 << 4,
};

struct Seen {
	Dir*	cache;
	int	n;
	int	max;
};

struct Idxed {
	char**	cache;
	int	n;
	int	max;
};

Seen	seentab[NCACHE];
Idxed	idxtab[NCACHE];
char	repopath[1024];
char	wdirpath[1024];
char	*rstr	= "R ";
char	*mstr	= "M ";
char	*astr	= "A ";
char	*ustr	= "U ";
char	*tstr 	= "T ";
char	*bdir = ".git/fs/HEAD/tree";
int	useidx	= 1;
int	nrel;
int	quiet;
int	dirty;
int	printflg;

Idxent	*idx;
int	idxsz;
int	nidx;
int	staleidx;
Idxent	*wdir;
int	wdirsz;
int	nwdir;

int	loadwdir(char*);

int
seen(Dir *dir)
{
	Seen *c;
	Dir *dp;
	int i;

	c = &seentab[dir->qid.path&(NCACHE-1)];
	dp = c->cache;
	for(i=0; i<c->n; i++, dp++)
		if(dir->qid.path == dp->qid.path
		&& dir->qid.type == dp->qid.type
		&& dir->dev == dp->dev)
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
checkedin(Idxent *e, int change)
{
	char *p;
	int r;

	p = smprint("%s/%s", bdir, e->path);
	r = access(p, AEXIST);
	if(r == 0 && change){
		if(e->state != 'R')
			e->state = 'T';
		staleidx = 1;
	}
	free(p);
	return r == 0;
}

int
indexed(char *path, int isdir)
{
	int lo, hi, mid, n, r;
	char *s;

	if(!useidx){
		s = smprint("%s/%s", bdir, path);
		r = access(s, AEXIST);
		free(s);
		return r == 0;
	}
	s = path;
	if(isdir)
		s = smprint("%s/", path);
	r = -1;
	lo = 0;
	hi = nidx-1;
	n = strlen(s);
	while(lo <= hi){
		mid = (hi + lo) / 2;
		if(isdir)
			r = strncmp(s, idx[mid].path, n);
		else
			r = strcmp(s, idx[mid].path);
		if(r < 0)
			hi = mid-1;
		else if(r > 0)
			lo = mid+1;
		else
			break;
	}
	if(isdir)
		free(s);
	return r == 0;
}

int
idxcmp(void *pa, void *pb)
{
	Idxent *a, *b;
	int c;

	a = (Idxent*)pa;
	b = (Idxent*)pb;
	if((c = strcmp(a->path, b->path)) != 0)
		return c;
	/* order is unique */
	return a-> order < b->order ? -1 : 1;
}

/*
 * compares whether the indexed entry 'a'
 * has the same contents and mode as
 * the entry on disk 'b'; if the indexed
 * entry is nil, does a deep comparison
 * of the checked out file and the file
 * checked in.
 */
int
samedata(Idxent *a, Idxent *b)
{
	char *gitpath, ba[IOUNIT], bb[IOUNIT];
	int fa, fb, na, nb, same;
	Dir *da, *db;

	if(a != nil){
		if(a->qid.path == b->qid.path
		&& a->qid.vers == b->qid.vers
		&& a->qid.type == b->qid.type
		&& a->mode == b->mode
		&& a->mode != 0)
			return 1;
	}

	same = 0;
	da = nil;
	db = nil;
	if((gitpath = smprint("%s/%s", bdir, b->path)) == nil)
		sysfatal("smprint: %r");
	fa = open(gitpath, OREAD);
	fb = open(b->path, OREAD);
	if(fa == -1 || fb == -1)
		goto mismatch;
	da = dirfstat(fa);
	db = dirfstat(fb);
	if(da == nil || db == nil)
		goto mismatch;
	if((da->mode&0100) != (db->mode&0100))
		goto mismatch;
	if(da->length != db->length)
		goto mismatch;
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
	if(a != nil){
		a->qid = db->qid;
		a->mode = db->mode;
		staleidx = 1;
	}
	same = 1;

mismatch:
	free(da);
	free(db);
	if(fa != -1)
		close(fa);
	if(fb != -1)
		close(fb);
	return same;
}

int
loadent(char *dir, Dir *d, int fullpath)
{
	char *path;
	int ret, isdir;
	Idxent *e;

	if(fullpath)
		path = strdup(dir);
	else
		path = smprint("%s/%s", dir, d->name);
	if(path == nil)
		sysfatal("smprint: %r");

	cleanname(path);
	if(strncmp(path, ".git/", 5) == 0){
		free(path);
		return 0;
	}
	ret = 0;
	isdir = d->qid.type & QTDIR;
	if((printflg & Uflg) == 0 && !indexed(path, isdir)){
		free(path);
		return 0;
	}
	if(isdir){
		ret = loadwdir(path);
		free(path);
	}else{
		if(nwdir == wdirsz){
			wdirsz += wdirsz/2;
			wdir = erealloc(wdir, wdirsz*sizeof(Idxent));
		}
		e = wdir + nwdir;
		e->path = path;
		e->qid = d->qid;
		e->mode = d->mode;
		e->order = nwdir;
		e->state = 'T';
		nwdir++;
	}
	return ret;
}

int
loadwdir(char *path)
{
	int fd, ret, i, n;
	Dir *d, *e;

	d = nil;
	e = nil;
	ret = -1;
	cleanname(path);
	if(strncmp(path, ".git/", 5) == 0)
		return 0;
	if((fd = open(path, OREAD)) < 0)
		goto error;
	if((e = dirfstat(fd)) == nil)
		sysfatal("fstat: %r");
	if(e->qid.type & QTDIR)
		while((n = dirread(fd, &d)) > 0){
			for(i = 0; i < n; i++)
				if(loadent(path, &d[i], 0) == -1)
					goto error;
			free(d);
		}
	else{
		if(loadent(path, e, 1) == -1)
			goto error;
	}
	ret = 0;
error:
	free(e);
	if(fd != -1)
		close(fd);
	return ret;
}

int
pfxmatch(char *p, char **pfx, int *pfxlen, int npfx)
{
	int i;

	if(p == nil)
		return 0;
	if(npfx == 0)
		return 1;
	for(i = 0; i < npfx; i++){
		if(strncmp(p, pfx[i], pfxlen[i]) != 0)
			continue;
		if(p[pfxlen[i]] == '/' || p[pfxlen[i]] == 0)
			return 1;
		if(strcmp(pfx[i], ".") == 0 || *pfx[i] == 0)
			return 1;
	}
	return 0;
}


char*
reporel(char *s)
{
	char *p;
	int n;

	if(*s == '/')
		s = strdup(s);
	else
		s = smprint("%s/%s", wdirpath, s);
	if(s == nil)
		sysfatal("smprint or memory allocation failed: %r");
	p = cleanname(s);
	n = strlen(repopath);
	if(strncmp(s, repopath, n) != 0)
		sysfatal("path outside repo: %s", s);
	p += n;
	if(*p == '/')
		p++;
	memmove(s, p, strlen(p)+1);
	return s;
}

void
show(Biobuf *o, int flg, char *str, char *path)
{
	dirty |= flg;
	if(!quiet && (printflg & flg))
		Bprint(o, "%s%s\n", str, path);
}

void
usage(void)
{
	fprint(2, "usage: %s [-qbc] [-f filt] [-b base] [paths...]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *p, *e, *ln, *base, **argrel, *parts[4], xbuf[8];
	int i, j, c, line, wfd, *argn;
	Biobuf *f, *o, *w;
	Hash h, hd;
	Dir rn;

	gitinit();
	if(access(".git/fs/ctl", AEXIST) != 0)
		sysfatal("no running git/fs");
	if(getwd(wdirpath, sizeof(wdirpath)) == nil)
		sysfatal("getwd: %r");
	if(findrepo(repopath, sizeof(repopath), &nrel) == -1)
		sysfatal("find root: %r");
	if(chdir(repopath) == -1)
		sysfatal("chdir: %r");

	ARGBEGIN{
	case 'q':
		quiet++;
		break;
	case 'c':
		rstr = "";
		tstr = "";
		mstr = "";
		astr = "";
		ustr = "";
		break;
	case 'f':
		for(p = EARGF(usage()); *p; p++)
			switch(*p){
			case 'T':	printflg |= Tflg;	break;
			case 'A':	printflg |= Aflg;	break;
			case 'M':	printflg |= Mflg;	break;
			case 'R':	printflg |= Rflg;	break;
			case 'U':	printflg |= Uflg;	break;
			default:	usage();		break;
		}
		break;
	case 'b':
		useidx = 0;
		base = EARGF(usage());
		if(resolveref(&h, base) == -1)
			sysfatal("no such ref '%s'", base);
		/* optimization: we're a lot faster when using the index */
		if(resolveref(&hd, "HEAD") == 0 && hasheq(&h, &hd))
			useidx = 1;
		bdir = smprint(".git/fs/object/%H/tree", h);
		break;
	default:
		usage();
	}ARGEND;

	if(printflg == 0)
		printflg = Tflg | Aflg | Mflg | Rflg;

	nidx = 0;
	idxsz = 32;
	idx = emalloc(idxsz*sizeof(Idxent));
	nwdir = 0;
	wdirsz = 32;
	wdir = emalloc(wdirsz*sizeof(Idxent));
	argrel = emalloc(argc*sizeof(char*));
	argn = emalloc(argc*sizeof(int));
	for(i = 0; i < argc; i++){
		argrel[i] = reporel(argv[i]);
		argn[i] = strlen(argrel[i]);
	}
	if((o = Bfdopen(1, OWRITE)) == nil)
		sysfatal("open out: %r");
	if(useidx){
		if((f = Bopen(".git/INDEX9", OREAD)) == nil){
			fprint(2, "open index: %r\n");
			if(access(".git/index9", AEXIST) == 0){
				fprint(2, "index format conversion needed:\n");
				fprint(2, "\tcd %s && git/fs\n", repopath);
				fprint(2, "\t@{cd .git/index9/removed >[2]/dev/null && walk -f | sed 's/^/R NOQID 0 /'} >> .git/INDEX9\n");
				fprint(2, "\t@{cd .git/fs/HEAD/tree && walk -f | sed 's/^/T NOQID 0 /'} >> .git/INDEX9\n");
			}
			exits("noindex");
		}
		line = 0;
		while((ln = Brdstr(f, '\n', 1)) != nil){
			line++;
			/* allow blank lines */
			if(ln[0] == 0 || ln[0] == '\n')
				continue;
			if(getfields(ln, parts, nelem(parts), 0, " \t") != nelem(parts))
				sysfatal(".git/INDEX9:%d: corrupt index", line);
			if(nidx == idxsz){
				idxsz += idxsz/2;
				idx = realloc(idx, idxsz*sizeof(Idxent));
			}
			cleanname(parts[3]);
			if(strncmp(parts[3], ".git/", 5) == 0){
				staleidx = 1;
				free(ln);
				continue;
			}
			idx[nidx].state = *parts[0];
			idx[nidx].qid = parseqid(parts[1]);
			idx[nidx].mode = strtol(parts[2], nil, 8);
			idx[nidx].path = strdup(parts[3]);
			idx[nidx].order = nidx;
			if(idx[nidx].path == nil)
				sysfatal("strdup: memory allocation failed");
			nidx++;
			free(ln);
		}
		qsort(idx, nidx, sizeof(Idxent), idxcmp);
	}

	for(i = 0; i < argc; i++){
		argrel[i] = reporel(argv[i]);
		argn[i] = strlen(argrel[i]);
	}
	if(argc == 0)
		loadwdir(".");
	else for(i = 0; i < argc; i++)
		loadwdir(argrel[i]);
	qsort(wdir, nwdir, sizeof(Idxent), idxcmp);
	for(i = 0; i < argc; i++){
		argrel[i] = reporel(argv[i]);
		argn[i] = strlen(argrel[i]);
	}
	i = 0;
	j = 0;
	while(i < nidx || j < nwdir){
		/* find the last entry we tracked for a path */
		while(i+1 < nidx && strcmp(idx[i].path, idx[i+1].path) == 0){
			staleidx = 1;
			i++;
		}
		while(j+1 < nwdir && strcmp(wdir[j].path, wdir[j+1].path) == 0)
			j++;
		if(i < nidx && !pfxmatch(idx[i].path, argrel, argn, argc)){
			i++;
			continue;
		}
		if(i >= nidx)
			c = 1;
		else if(j >= nwdir)
			c = -1;
		else
			c = strcmp(idx[i].path, wdir[j].path);
		/* exists in both index and on disk */
		if(c == 0){
			if(idx[i].state == 'R'){
				if(checkedin(&idx[i], 0))
					show(o, Rflg, rstr, idx[i].path);
				else{
					idx[i].state = 'U';
					staleidx = 1;
				}
			}else if(idx[i].state == 'A' && !checkedin(&idx[i], 1))
				show(o, Aflg, astr, idx[i].path);
			else if(!samedata(&idx[i], &wdir[j]))
				show(o, Mflg, mstr, idx[i].path);
			else
				show(o, Tflg, tstr, idx[i].path);
			i++;
			j++;
		/* only exists in index */
		}else if(c < 0){
			if(checkedin(&idx[i], 0))
				show(o, Rflg, rstr, idx[i].path);
			i++;
		/* only exists on disk */
		}else{
			if(!useidx && checkedin(&wdir[j], 0)){
				if(samedata(nil, &wdir[j]))
					show(o, Tflg, tstr, wdir[j].path);
				else
					show(o, Mflg, mstr, wdir[j].path);
			}else if(printflg & Uflg && pfxmatch(idx[i].path, argrel, argn, argc))
				show(o, Uflg, ustr, wdir[j].path);
			j++;
		}
	}
	Bterm(o);

	if(useidx && staleidx)
	if((wfd = create(".git/INDEX9.new", OWRITE, 0644)) != -1){
		if((w = Bfdopen(wfd, OWRITE)) == nil){
			close(wfd);
			goto Nope;
		}
		for(i = 0; i < nidx; i++){
			while(i+1 < nidx && strcmp(idx[i].path, idx[i+1].path) == 0)
				i++;
			if(idx[i].state == 'U')
				continue;
			Bprint(w, "%c %Q %o %s\n",
				idx[i].state,
				idx[i].qid, 
				idx[i].mode,
				idx[i].path);
		}
		Bterm(w);
		nulldir(&rn);
		rn.name = "INDEX9";
		if(remove(".git/INDEX9") == -1)
			goto Nope;
		if(dirwstat(".git/INDEX9.new", &rn) == -1)
			sysfatal("rename: %r");
	}

Nope:
	if(!dirty)
		exits(nil);

	p = xbuf;
	e = p + sizeof(xbuf);
	for(i = 0; (1 << i) != Tflg; i++)
		if(dirty & (1 << i))
			p = seprint(p, e, "%c", "RMAUT"[i]);
	exits(xbuf);
}
