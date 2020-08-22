#include <u.h>
#include <libc.h>
#include <pool.h>

#include "git.h"

#define TMPPATH(suff) (".git/objects/pack/repack."suff)

typedef struct Objmeta	Objmeta;
typedef struct Objq	Objq;
typedef struct Buf	Buf;
typedef struct Compout	Compout;

struct Objmeta {
	int	type;
	char	*path;
	vlong	mtime;
	Hash	hash;

	Object	*obj;
	Object	*base;
	Delta	*delta;
	int	ndelta;
};

struct Objq {
	Objq	*next;
	Object	*obj;
};

struct Buf {
	int off;
	int sz;
	uchar *data;
};

struct Compout {
	Biobuf *bfd;
	DigestState *st;
};

void
usage(void)
{
	fprint(2, "usage: %s [-d]\n", argv0);
	exits("usage");
}

static int
dsortcmp(void *pa, void *pb)
{
	Objmeta *a, *b;
	int cmp;

	a = pa;
	b = pb;
	if(a->type != b->type)
		return a->type - b->type;
	cmp = strcmp(a->path, b->path);
	if(cmp != 0)
		return cmp;
	if(a->mtime != b->mtime)
		return a->mtime - b->mtime;
	return memcmp(a->hash.h, b->hash.h, sizeof(a->hash.h));
}

static int
timecmp(void *pa, void *pb)
{
	Objmeta *a, *b;

	a = pa;
	b = pb;
	return b->mtime - a->mtime;
}

static void
addmeta(Objmeta **m, int *nm, int type, Hash h, char *path, vlong mtime)
{
	*m = erealloc(*m, (*nm + 1)*sizeof(Objmeta));
	memset(&(*m)[*nm], 0, sizeof(Objmeta));
	(*m)[*nm].type = type;
	(*m)[*nm].path = path;
	(*m)[*nm].mtime = mtime;
	(*m)[*nm].hash = h;
	*nm += 1;
}

static int
loadtree(Objmeta **m, int *nm, Hash tree, char *dpath, vlong mtime, Objset *has)
{
	Object *t, *o;
	Dirent *e;
	char *p;
	int i, k;

	if(oshas(has, tree))
		return 0;
	if((t = readobject(tree)) == nil)
		return -1;
//	osadd(has, t);
	addmeta(m, nm, t->type, t->hash, dpath, mtime);
	for(i = 0; i < t->tree->nent; i++){
		e = &t->tree->ent[i];
		if(oshas(has, e->h))
			continue;
		if(e->ismod)
			continue;
		k = (e->mode & DMDIR) ? GTree : GBlob;
		o = clearedobject(e->h, k);
		p = smprint("%s/%s", dpath, e->name);
		addmeta(m, nm, k, o->hash, p, mtime);
//		if(k == GBlob)
//			osadd(has, o);
		if(k == GTree && loadtree(m, nm, e->h, p, mtime, has) == -1)
			return -1;
		osadd(has, o);
	}
	unref(t);
	return 0;
}

static int
loadcommit(Objmeta **m, int *nm, Hash h, Objset *has)
{
	Object *c, *p;
	Objq *q, *e, *t, *n;
	int i;

	if((c = readobject(h)) == nil)
		return -1;
	if(c->type != GCommit)
		sysfatal("object %H not commit", c->hash);
	q = emalloc(sizeof(Objq));
	e = q;
	q->next = nil;
	q->obj = c;
	for(; q != nil; q = n){
		c = q->obj;
		if(oshas(has, c->hash))
			goto nextiter;
		osadd(has, c);
		for(i = 0; i < c->commit->nparent; i++){
			if((p = readobject(c->commit->parent[i])) == nil)
				return -1;
			t = emalloc(sizeof(Objq));
			t->next = nil;
			t->obj = p;
			e->next = t;
			e = t;
		}
		addmeta(m, nm, c->type, c->hash, estrdup(""), c->commit->ctime);
		if(loadtree(m, nm, c->commit->tree, "", c->commit->ctime, has) == -1)
			goto error;
nextiter:
		n = q->next;
		unref(q->obj);
		free(q);
	}
	return 0;
error:
	for(; q != nil; q = n) {
		n = q->next;
		free(q);
	}
	return -1;
}

static int
readmeta(Objmeta **m, Hash *roots, int nroots, Hash * /*leaves */, int /*nleaves*/)
{
	Objset has;
	int i, nm;

	*m = nil;
	nm = 0;
	osinit(&has);
	for(i = 0; i < nroots; i++)
		if(loadcommit(m, &nm, roots[i], &has) == -1){
			free(*m);
			return -1;
		}
	return nm;
}

static int
deltasz(Delta *d, int nd)
{
	int i, sz;
	sz = 32;
	for(i = 0; i < nd; i++)
		sz += d[i].cpy ? 7 : d[i].len + 1;
	return sz;
}

static void
pickdeltas(Objmeta *meta, int nmeta)
{
	Objmeta *m, *p;
	Object *a, *b;
	Delta *d;
	int i, nd, sz, best;

	qsort(meta, nmeta, sizeof(Objmeta), dsortcmp);
	for(i = 0; i < nmeta; i++){
		m = &meta[i];
		p = meta;
		if(i > 10)
			p = m - 10;
		if((a = readobject(m->hash)) == nil)
			sysfatal("missing object %H", m->hash);
		best = a->size;
		m->base = nil;
		m->delta = nil;
		m->ndelta = 0;
		for(; p != m; p++){
			if((b = readobject(p->hash)) == nil)
				sysfatal("missing object %H", p->hash);
			d = deltify(a->data, a->size, b->data, b->size, &nd);
			sz = deltasz(d, nd);
			if(sz + 32 < best){
				free(m->delta);
				best = sz;
				m->base = b;
				m->delta = d;
				m->ndelta = nd;
			}else
				free(d);
			unref(b);
		}
		unref(a);
	}
}

static int
hwrite(Biobuf *b, void *buf, int len, DigestState **st)
{
	*st = sha1(buf, len, nil, *st);
	return Bwrite(b, buf, len);
}

int
compread(void *p, void *dst, int n)
{
	Buf *b;

	b = p;
	if(n > b->sz - b->off)
		n = b->sz - b->off;
	memcpy(dst, b->data + b->off, n);
	b->off += n;
	return n;
}

int
compwrite(void *p, void *buf, int n)
{
	return hwrite(((Compout *)p)->bfd, buf, n, &((Compout*)p)->st);
}

int
hcompress(Biobuf *bfd, void *buf, int sz, DigestState **st)
{
	int r;
	Buf b ={
		.off=0,
		.data=buf,
		.sz=sz,
	};
	Compout o = {
		.bfd = bfd,
		.st = *st,
	};

	r = deflatezlib(&o, compwrite, &b, compread, 6, 0);
	*st = o.st;
	return r;
}

void
append(char **p, int *len, int *sz, void *seg, int nseg)
{
	if(*len + nseg >= *sz){
		while(*len + nseg >= *sz)
			*sz += *sz/2;
		*p = erealloc(*p, *sz);
	}
	memcpy(*p + *len, seg, nseg);
	*len += nseg;
}

int
encodedelta(Objmeta *m, Object *o, Object *b, void **pp)
{
	char *p, *bp, buf[16];
	int len, sz, n, i, j;
	Delta *d;

	sz = 128;
	len = 0;
	p = emalloc(sz);

	/* base object size */
	buf[0] = b->size & 0x7f;
	n = b->size >> 7;
	for(i = 1; n > 0; i++){
		buf[i - 1] |= 0x80;
		buf[i] = n & 0x7f;
		n >>= 7;
	}
	append(&p, &len, &sz, buf, i);

	/* target object size */
	buf[0] = o->size & 0x7f;
	n = o->size >> 7;
	for(i = 1; n > 0; i++){
		buf[i - 1] |= 0x80;
		buf[i] = n & 0x7f;
		n >>= 7;
	}
	append(&p, &len, &sz, buf, i);
	for(j = 0; j < m->ndelta; j++){
		d = &m->delta[j];
		if(d->cpy){
			n = d->off;
			bp = buf + 1;
			buf[0] = 0x81;
			buf[1] = 0x00;
			for(i = 0; i < sizeof(buf); i++) {
				buf[0] |= 1<<i;
				*bp++ = n & 0xff;
				n >>= 8;
				if(n == 0)
					break;
			}

			n = d->len;
			if(n != 0x10000) {
				buf[0] |= 0x1<<4;
				for(i = 0; i < sizeof(buf)-4 && n > 0; i++){
					buf[0] |= 1<<(i + 4);
					*bp++ = n & 0xff;
					n >>= 8;
				}
			}
			append(&p, &len, &sz, buf, bp - buf);
		}else{
			n = 0;
			while(n != d->len){
				buf[0] = (d->len - n < 127) ? d->len - n : 127;
				append(&p, &len, &sz, buf, 1);
				append(&p, &len, &sz, o->data + d->off + n, buf[0]);
				n += buf[0];
			}
		}
	}
	*pp = p;
	return len;
}

static int
writepack(int fd, Objmeta *meta, int nmeta, Hash *h)
{
	int i, j, n, x, res, pcnt, len, ret;
	DigestState *st;
	Biobuf *bfd;
	Objmeta *m;
	Object *o;
	char *p, buf[16];

	st = nil;
	ret = -1;
	pcnt = 0;
	if((fd = dup(fd, -1)) == -1)
		return -1;
	if((bfd = Bfdopen(fd, OWRITE)) == nil)
		return -1;
	if(hwrite(bfd, "PACK", 4, &st) == -1)
		return -1;
	PUTBE32(buf, 2);
	if(hwrite(bfd, buf, 4, &st) == -1)
		return -1;
	PUTBE32(buf, nmeta);
	if(hwrite(bfd, buf, 4, &st) == -1)
		return -1;
	qsort(meta, nmeta, sizeof(Objmeta), timecmp);
	fprint(2, "deltifying %d objects:   0%%", nmeta);
	for(i = 0; i < nmeta; i++){
		m = &meta[i];
		x = (i*100) / nmeta;
		if(x > pcnt){
			pcnt = x;
			if(pcnt%10 == 0)
				fprint(2, "\b\b\b\b%3d%%", pcnt);
		}
		if((o = readobject(m->hash)) == nil)
			return -1;
		if(m->delta == nil){
			len = o->size;
			buf[0] = o->type << 4;
			buf[0] |= len & 0xf;
			len >>= 4;
			for(j = 1; len != 0; j++){
				assert(j < sizeof(buf));
				buf[j-1] |= 0x80;
				buf[j] = len & 0x7f;
				len >>= 7;
			}
			hwrite(bfd, buf, j, &st);
			if(hcompress(bfd, o->data, o->size, &st) == -1)
				goto error;
		}else{
			n = encodedelta(m, o, m->base, &p);
			len = n;
			buf[0] = GRdelta << 4;
			buf[0] |= len & 0xf;
			len >>= 4;
			for(j = 1; len != 0; j++){
				assert(j < sizeof(buf));
				buf[j-1] |= 0x80;
				buf[j] = len & 0x7f;
				len >>= 7;
			}
			hwrite(bfd, buf, j, &st);
			hwrite(bfd, m->base->hash.h, sizeof(m->base->hash.h), &st);
			res = hcompress(bfd, p, n, &st);
			free(p);
			if(res == -1)
				goto error;
		}
		unref(o);
	}
	fprint(2, "\b\b\b\b100%%\n");
	sha1(nil, 0, h->h, st);
	if(Bwrite(bfd, h->h, sizeof(h->h)) == -1)
		goto error;
	ret = 0;
error:
	if(Bterm(bfd) == -1)
		return -1;
	return ret;
}

int
cleanup(Hash h)
{
	char newpfx[42], dpath[256], fpath[256];
	int i, j, nd;
	Dir *d;

	snprint(newpfx, sizeof(newpfx), "%H.", h);
	for(i = 0; i < 256; i++){
		snprint(dpath, sizeof(dpath), ".git/objects/%02x", i);
		if((nd = slurpdir(dpath, &d)) == -1)
			continue;
		for(j = 0; j < nd; j++){
			snprint(fpath, sizeof(fpath), ".git/objects/%02x/%s", i, d[j].name);
			remove(fpath);
		}
		remove(dpath);
		free(d);
	}
	snprint(dpath, sizeof(dpath), ".git/objects/pack");
	if((nd = slurpdir(dpath, &d)) == -1)
		return -1;
	for(i = 0; i < nd; i++){
		if(strncmp(d[i].name, newpfx, strlen(newpfx)) == 0)
			continue;
		snprint(fpath, sizeof(fpath), ".git/objects/pack/%s", d[i].name);
		remove(fpath);
	}
	return 0;
}

void
main(int argc, char **argv)
{
	int fd, nmeta, nrefs;
	Objmeta *meta;
	Hash *refs, h;
	char path[128];
	Dir rn;

	ARGBEGIN{
	case 'd':
		debug++;
		break;
	default:
		usage();
	}ARGEND;

	gitinit();
	refs = nil;
	if((nrefs = listrefs(&refs)) == -1)
		sysfatal("load refs: %r");
	if((nmeta = readmeta(&meta, refs, nrefs, nil, 0)) == -1)
		sysfatal("read object metadata: %r");
	pickdeltas(meta, nmeta);
	if((fd = create(TMPPATH("pack.tmp"), OWRITE, 0644)) == -1)
		sysfatal("open %s: %r", TMPPATH("pack.tmp"));
	if(writepack(fd, meta, nmeta, &h) == -1)
		sysfatal("writepack: %r");
	if(indexpack(TMPPATH("pack.tmp"), TMPPATH("idx.tmp"), h) == -1)
		sysfatal("indexpack: %r");
	close(fd);

	nulldir(&rn);
	rn.name = path;
	snprint(path, sizeof(path), "%H.pack", h);
	if(dirwstat(TMPPATH("pack.tmp"), &rn) == -1)
		sysfatal("rename pack: %r");
	snprint(path, sizeof(path), "%H.idx", h);
	if(dirwstat(TMPPATH("idx.tmp"), &rn) == -1)
		sysfatal("rename pack: %r");
	if(cleanup(h) == -1)
		sysfatal("cleanup: %r");
	exits(nil);
}
