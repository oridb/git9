#include <u.h>
#include <libc.h>
#include "git.h"

typedef struct Objbuf Objbuf;
struct Objbuf {
	int off;
	char *hdr;
	int nhdr;
	char *dat;
	int ndat;
};
enum {
	Maxparents = 16,
};

static int
bwrite(void *p, void *buf, int nbuf)
{
	return Bwrite(p, buf, nbuf);
}

static int
objbytes(void *p, void *buf, int nbuf)
{
	Objbuf *b;
	int r, n, o;
	char *s;

	b = p;
	n = 0;
	if(b->off < b->nhdr){
		r = b->nhdr - b->off;
		r = (nbuf < r) ? nbuf : r;
		memcpy(buf, b->hdr, r);
		b->off += r;
		nbuf -= r;
		n += r;
	}
	if(b->off < b->ndat + b->nhdr){
		s = buf;
		o = b->off - b->nhdr;
		r = b->ndat - o;
		r = (nbuf < r) ? nbuf : r;
		memcpy(s + n, b->dat + o, r);
		b->off += r;
		n += r;
	}
	return n;
}

void
writeobj(Hash *h, char *hdr, int nhdr, char *dat, int ndat)
{
	Objbuf b = {.off=0, .hdr=hdr, .nhdr=nhdr, .dat=dat, .ndat=ndat};
	char s[64], o[256];
	SHA1state *st;
	Biobuf *f;
	int fd;

	st = sha1((uchar*)hdr, nhdr, nil, nil);
	st = sha1((uchar*)dat, ndat, nil, st);
	sha1(nil, 0, h->h, st);
	snprint(s, sizeof(s), "%H", *h);
	fd = create(".git/objects", OREAD, DMDIR|0755);
	close(fd);
	snprint(o, sizeof(o), ".git/objects/%c%c", s[0], s[1]);
	fd = create(o, OREAD, DMDIR | 0755);
	close(fd);
	snprint(o, sizeof(o), ".git/objects/%c%c/%s", s[0], s[1], s + 2);
	if(readobject(*h) == nil){
		if((f = Bopen(o, OWRITE)) == nil)
			sysfatal("could not open %s: %r", o);
		if(deflatezlib(f, bwrite, &b, objbytes, 9, 0) == -1)
			sysfatal("could not write %s: %r", o);
		Bterm(f);
	}
}

int
gitmode(int m)
{
	return (m & 0777) | ((m & DMDIR) ? 0040000 : 0100000);
}

void
blobify(char *path, vlong size, Hash *bh)
{
	char h[64], *d;
	int f, nh;

	nh = snprint(h, sizeof(h), "%T %lld", GBlob, size) + 1;
	if((f = open(path, OREAD)) == -1)
		sysfatal("could not open %s: %r", path);
	d = emalloc(size);
	if(readn(f, d, size) != size)
		sysfatal("could not read blob %s: %r", path);
	writeobj(bh, h, nh, d, size);
	close(f);
	free(d);
}

int
tracked(char *path)
{
	Dir *d;
	char ipath[256];

	/* Explicitly removed. */
	snprint(ipath, sizeof(ipath), ".git/index9/removed/%s", path);
	if(strstr(cleanname(ipath), ".git/index9/removed") != ipath)
		sysfatal("path %s leaves index", ipath);
	d = dirstat(ipath);
	if(d != nil && d->qid.type != QTDIR){
		free(d);
		return 0;
	}

	/* Explicitly added. */
	snprint(ipath, sizeof(ipath), ".git/index9/tracked/%s", path);
	if(strstr(cleanname(ipath), ".git/index9/tracked") != ipath)
		sysfatal("path %s leaves index", ipath);
	if(access(ipath, AEXIST) == 0)
		return 1;

	return 0;
}

int
dircmp(void *pa, void *pb)
{
	char aname[256], bname[256], c;
	Dir *a, *b;

	a = pa;
	b = pb;
	/*
	 * If the files have the same name, they're equal.
	 * Otherwise, If they're trees, they sort as thoug
	 * there was a trailing slash.
	 *
	 * Wat.
	 */
	if(strcmp(a->name, b->name) == 0){
		snprint(aname, sizeof(aname), "%s", a->name);
		snprint(bname, sizeof(bname), "%s", b->name);
	}else{
		c = (a->qid.type & QTDIR) ? '/' : 0;
		snprint(aname, sizeof(aname), "%s%c", a->name, c);
		c = (b->qid.type & QTDIR) ? '/' : 0;
		snprint(bname, sizeof(bname), "%s%c", b->name, c);
	}

	return strcmp(aname, bname);
}

int
treeify(char *path, Hash *th)
{
	char *t, h[64], l[256], ep[256];
	int nd, nl, nt, nh, i, s;
	Hash eh;
	Dir *d;
		
	if((nd = slurpdir(path, &d)) == -1)
		sysfatal("could not read %s", path);
	if(nd == 0)
		return 0;

	t = nil;
	nt = 0;
	qsort(d, nd, sizeof(Dir), dircmp);
	for(i = 0; i < nd; i++){
		snprint(ep, sizeof(ep), "%s/%s", path, d[i].name);
		if(strcmp(d[i].name, ".git") == 0)
			continue;
		if(!tracked(ep))
			continue;
		if((d[i].qid.type & QTDIR) == 0)
			blobify(ep, d[i].length, &eh);
		else if(treeify(ep, &eh) == 0)
			continue;

		nl = snprint(l, sizeof(l), "%o %s", gitmode(d[i].mode), d[i].name);
		s = nt + nl + sizeof(eh.h) + 1;
		t = realloc(t, s);
		memcpy(t + nt, l, nl + 1);
		memcpy(t + nt + nl + 1, eh.h, sizeof(eh.h));
		nt = s;
	}
	free(d);
	nh = snprint(h, sizeof(h), "%T %d", GTree, nt) + 1;
	if(nh >= sizeof(h))
		sysfatal("overlong header");
	writeobj(th, h, nh, t, nt);
	free(t);
	return nd;
}


void
mkcommit(Hash *c, char *msg, char *name, char *email, Hash *parents, int nparents, Hash tree)
{
	char *s, h[64];
	int ns, nh, i;
	Fmt f;

	fmtstrinit(&f);
	fmtprint(&f, "tree %H\n", tree);
	for(i = 0; i < nparents; i++)
		fmtprint(&f, "parent %H\n", parents[i]);
	fmtprint(&f, "author %s <%s> %lld +0000\n", name, email, (vlong)time(nil));
	fmtprint(&f, "committer %s <%s> %lld +0000\n", name, email, (vlong)time(nil));
	fmtprint(&f, "\n");
	fmtprint(&f, "%s", msg);
	s = fmtstrflush(&f);

	ns = strlen(s);
	nh = snprint(h, sizeof(h), "%T %d", GCommit, ns) + 1;
	writeobj(c, h, nh, s, ns);
	free(s);
}

void
usage(void)
{
	fprint(2, "usage: git/commit -n name -e email -m message -d dir");
	exits("usage");
}

void
main(int argc, char **argv)
{
	Hash c, t, parents[Maxparents];
	char *msg, *name, *email;
	int r, nparents;


	msg = nil;
	name = nil;
	email = nil;
	nparents = 0;
	gitinit();
	ARGBEGIN{
	case 'm':	msg = EARGF(usage());	break;
	case 'n':	name = EARGF(usage());	break;
	case 'e':	email = EARGF(usage());	break;
	case 'p':
		if(nparents >= Maxparents)
			sysfatal("too many parents");
		if(resolveref(&parents[nparents++], EARGF(usage())) == -1)
			sysfatal("invalid parent: %r");
		break;
	}ARGEND;

	if(!msg) sysfatal("missing message");
	if(!name) sysfatal("missing name");
	if(!email) sysfatal("missing email");
	if(!msg || !name)
		usage();

	gitinit();
	if(access(".git", AEXIST) != 0)
		sysfatal("could not find git repo: %r");
	r = treeify(".", &t);
	if(r == -1)
		sysfatal("could not commit: %r\n");
	if(r == 0)
		sysfatal("empty commit: aborting");
	mkcommit(&c, msg, name, email, parents, nparents, t);
	print("%H\n", c);
	exits(nil);
}
