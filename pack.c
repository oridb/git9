#include <u.h>
#include <libc.h>
#include <ctype.h>

#include "git.h"

typedef struct Buf Buf;

struct Buf {
	int len;
	int sz;
	char *data;
};

static int	readpacked(Biobuf *, Object *, int);
static Object	*readidxobject(Biobuf *, Hash, int);

Objset objcache;
Object *lruhead;
Object *lrutail;
int	ncache;

static void
clear(Object *o)
{
	if(!o)
		return;

	assert(o->refs == 0);
	assert((o->flag & Ccache) == 0);
	assert(o->flag & Cloaded);
	switch(o->type){
	case GCommit:
		if(!o->commit)
			break;
		free(o->commit->parent);
		free(o->commit->author);
		free(o->commit->committer);
		free(o->commit);
		o->commit = nil;
		break;
	case GTree:
		if(!o->tree)
			break;
		free(o->tree->ent);
		free(o->tree);
		o->tree = nil;
		break;
	default:
		break;
	}

	free(o->all);
	o->all = nil;
	o->data = nil;
	o->flag &= ~Cloaded;
}

void
unref(Object *o)
{
	if(!o)
		return;
	o->refs--;
	if(!o->refs)
		clear(o);
}

Object*
ref(Object *o)
{
	o->refs++;
	return o;
}

void
cache(Object *o)
{
	Object *p;

	if(o == lruhead)
		return;
	if(o == lrutail)
		lrutail = lrutail->prev;
	if(!(o->flag & Cexist)){
		osadd(&objcache, o);
		o->id = objcache.nobj;
		o->flag |= Cexist;
	}
	if(o->prev)
		o->prev->next = o->next;
	if(o->next)
		o->next->prev = o->prev;
	if(lrutail == o){
		lrutail = o->prev;
		lrutail->next = nil;
	}else if(!lrutail)
		lrutail = o;
	if(lruhead)
		lruhead->prev = o;
	o->next = lruhead;
	o->prev = nil;
	lruhead = o;

	if(!(o->flag & Ccache)){
		o->flag |= Ccache;
		ref(o);
		ncache++;
	}
	while(ncache > Cachemax){
		p = lrutail;
		lrutail = p->prev;
		lrutail->next = nil;
		p->flag &= ~Ccache;
		p->prev = nil;
		p->next = nil;
		unref(p);
		ncache--;
	}		
}

int
bappend(void *p, void *src, int len)
{
	Buf *b = p;
	char *n;

	while(b->len + len >= b->sz){
		b->sz = b->sz*2 + 64;
		n = realloc(b->data, b->sz);
		if(n == nil)
			return -1;
		b->data = n;
	}
	memmove(b->data + b->len, src, len);
	b->len += len;
	return len;
}

int
breadc(void *p)
{
	return Bgetc(p);
}

int
bdecompress(Buf *d, Biobuf *b, vlong *csz)
{
	vlong o;

	o = Boffset(b);
	if(inflatezlib(d, bappend, b, breadc) == -1){
		free(d->data);
		return -1;
	}
	if (csz)
		*csz = Boffset(b) - o;
	return d->len;
}

int
decompress(void **p, Biobuf *b, vlong *csz)
{
	Buf d = {.len=0, .data=nil, .sz=0};

	if(bdecompress(&d, b, csz) == -1){
		free(d.data);
		return -1;
	}
	*p = d.data;
	return d.len;
}

static int
preadbe32(Biobuf *b, int *v, vlong off)
{
	char buf[4];
	
	if(Bseek(b, off, 0) == -1)
		return -1;
	if(Bread(b, buf, sizeof(buf)) == -1)
		return -1;
	*v = GETBE32(buf);

	return 0;
}
static int
preadbe64(Biobuf *b, vlong *v, vlong off)
{
	char buf[8];
	
	if(Bseek(b, off, 0) == -1)
		return -1;
	if(Bread(b, buf, sizeof(buf)) == -1)
		return -1;
	*v = GETBE64(buf);
	return 0;
}

int
readvint(char *p, char **pp)
{
	int i, n, c;
	
	i = 0;
	n = 0;
	do {
		c = *p++;
		n |= (c & 0x7f) << i;
		i += 7;
	} while (c & 0x80);
	*pp = p;

	return n;
}

static int
hashsearch(Hash *hlist, int nent, Hash h)
{
	int hi, lo, mid, d;

	lo = 0;
	hi = nent;
	while(lo < hi){
		mid = (lo + hi)/2;
		d = memcmp(hlist[mid].h, h.h, sizeof h.h);
		if(d < 0)
			lo = mid + 1;
		else if(d > 0)
			hi = mid;
		else
			return mid;
	}
	return -1;
}

static int
applydelta(Object *dst, Object *base, char *d, int nd)
{
	char *r, *b, *ed, *er;
	int n, nr, c;
	vlong o, l;

	ed = d + nd;
	b = base->data;
	n = readvint(d, &d);
	if(n != base->size){
		werrstr("mismatched source size");
		return -1;
	}

	nr = readvint(d, &d);
	r = emalloc(nr + 64);
	n = snprint(r, 64, "%T %d", base->type, nr) + 1;
	dst->all = r;
	dst->type = base->type;
	dst->data = r + n;
	dst->size = nr;
	er = dst->data + nr;
	r = dst->data;

	while(1){
		if(d == ed)
			break;
		c = *d++;
		if(!c){
			werrstr("bad delta encoding");
			return -1;
		}
		/* copy from base */
		if(c & 0x80){
			o = 0;
			l = 0;
			/* Offset in base */
			if(c & 0x01) o |= (*d++ <<  0) & 0x000000ff;
			if(c & 0x02) o |= (*d++ <<  8) & 0x0000ff00;
			if(c & 0x04) o |= (*d++ << 16) & 0x00ff0000;
			if(c & 0x08) o |= (*d++ << 24) & 0xff000000;

			/* Length to copy */
			if(c & 0x10) l |= (*d++ <<  0) & 0x0000ff;
			if(c & 0x20) l |= (*d++ <<  8) & 0x00ff00;
			if(c & 0x40) l |= (*d++ << 16) & 0xff0000;
			if(l == 0) l = 0x10000;

			assert(o + l <= base->size);
			memmove(r, b + o, l);
			r += l;
		/* inline data */
		}else{
			memmove(r, d, c);
			d += c;
			r += c;
		}

	}
	if(r != er){
		werrstr("truncated delta (%zd)", er - r);
		return -1;
	}

	return nr;
}

static int
readrdelta(Biobuf *f, Object *o, int nd, int flag)
{
	Object *b;
	Hash h;
	char *d;
	int n;

	d = nil;
	if(Bread(f, h.h, sizeof(h.h)) != sizeof(h.h))
		goto error;
	if(hasheq(&o->hash, &h))
		goto error;
	if((n = decompress(&d, f, nil)) == -1)
		goto error;
	if(d == nil || n != nd)
		goto error;
	if((b = readidxobject(f, h, flag)) == nil)
		goto error;
	if(applydelta(o, b, d, n) == -1)
		goto error;
	free(d);
	return 0;
error:
	free(d);
	return -1;
}

static int
readodelta(Biobuf *f, Object *o, vlong nd, vlong p, int flag)
{
	Object b;
	char *d;
	vlong r;
	int c, n;

	r = 0;
	d = nil;
	while(1){
		if((c = Bgetc(f)) == -1)
			goto error;
		r |= c & 0x7f;
		if (!(c & 0x80))
			break;
		r++;
		r <<= 7;
	}while(c & 0x80);

	if(r > p){
		werrstr("junk offset -%lld (from %lld)", r, p);
		goto error;
	}
	if((n = decompress(&d, f, nil)) == -1)
		goto error;
	if(d == nil || n != nd)
		goto error;
	if(Bseek(f, p - r, 0) == -1)
		goto error;
	if(readpacked(f, &b, flag) == -1)
		goto error;
	if(applydelta(o, &b, d, nd) == -1)
		goto error;
	free(d);
	return 0;
error:
	free(d);
	return -1;
}

static int
readpacked(Biobuf *f, Object *o, int flag)
{
	int c, s, n;
	vlong l, p;
	Type t;
	Buf b;

	p = Boffset(f);
	c = Bgetc(f);
	if(c == -1)
		return -1;
	l = c & 0xf;
	s = 4;
	t = (c >> 4) & 0x7;
	if(!t){
		werrstr("unknown type for byte %x", c);
		return -1;
	}
	while(c & 0x80){
		if((c = Bgetc(f)) == -1)
			return -1;
		l |= (c & 0x7f) << s;
		s += 7;
	}

	switch(t){
	default:
		werrstr("invalid object at %lld", Boffset(f));
		return -1;
	case GCommit:
	case GTree:
	case GTag:
	case GBlob:
		b.sz = 64 + l;

		b.data = emalloc(b.sz);
		n = snprint(b.data, 64, "%T %lld", t, l) + 1;
		b.len = n;
		if(bdecompress(&b, f, nil) == -1){
			free(b.data);
			return -1;
		}
		o->type = t;
		o->all = b.data;
		o->data = b.data + n;
		o->size = b.len - n;
		break;
	case GOdelta:
		if(readodelta(f, o, l, p, flag) == -1)
			return -1;
		break;
	case GRdelta:
		if(readrdelta(f, o, l, flag) == -1)
			return -1;
		break;
	}
	o->flag |= Cloaded|flag;
	return 0;
}

static int
readloose(Biobuf *f, Object *o, int flag)
{
	struct { char *tag; int type; } *p, types[] = {
		{"blob", GBlob},
		{"tree", GTree},
		{"commit", GCommit},
		{"tag", GTag},
		{nil},
	};
	char *d, *s, *e;
	vlong sz, n;
	int l;

	n = decompress(&d, f, nil);
	if(n == -1)
		return -1;

	s = d;
	o->type = GNone;
	for(p = types; p->tag; p++){
		l = strlen(p->tag);
		if(strncmp(s, p->tag, l) == 0){
			s += l;
			o->type = p->type;
			while(!isspace(*s))
				s++;
			break;
		}
	}
	if(o->type == GNone){
		free(o->data);
		return -1;
	}
	sz = strtol(s, &e, 0);
	if(e == s || *e++ != 0){
		werrstr("malformed object header");
		goto error;
	}
	if(sz != n - (e - d)){
		werrstr("mismatched sizes");
		goto error;
	}
	o->size = sz;
	o->data = e;
	o->all = d;
	o->flag |= Cloaded|flag;
	return 0;

error:
	free(d);
	return -1;
}

vlong
searchindex(Biobuf *f, Hash h)
{
	int lo, hi, idx, i, nent;
	vlong o, oo;
	Hash hh;

	o = 8;
	/*
	 * Read the fanout table. The fanout table
	 * contains 256 entries, corresponsding to
	 * the first byte of the hash. Each entry
	 * is a 4 byte big endian integer, containing
	 * the total number of entries with a leading
	 * byte <= the table index, allowing us to
	 * rapidly do a binary search on them.
	 */
	if (h.h[0] == 0){
		lo = 0;
		if(preadbe32(f, &hi, o) == -1)
			goto err;
	} else {
		o += h.h[0]*4 - 4;
		if(preadbe32(f, &lo, o + 0) == -1)
			goto err;
		if(preadbe32(f, &hi, o + 4) == -1)
			goto err;
	}
	if(hi == lo)
		goto notfound;
	if(preadbe32(f, &nent, 8 + 255*4) == -1)
		goto err;

	/*
	 * Now that we know the range of hashes that the
	 * entry may exist in, read them in so we can do
	 * a bsearch.
	 */
	idx = -1;
	Bseek(f, Hashsz*lo + 8 + 256*4, 0);
	for(i = 0; i < hi - lo; i++){
		if(Bread(f, hh.h, sizeof(hh.h)) == -1)
			goto err;
		if(hasheq(&hh, &h))
			idx = lo + i;
	}
	if(idx == -1)
		goto notfound;


	/*
	 * We found the entry. If it's 32 bits, then we
	 * can just return the oset, otherwise the 32
	 * bit entry contains the oset to the 64 bit
	 * entry.
	 */
	oo = 8;			/* Header */
	oo += 256*4;		/* Fanout table */
	oo += Hashsz*nent;	/* Hashes */
	oo += 4*nent;		/* Checksums */
	oo += 4*idx;		/* Offset offset */
	if(preadbe32(f, &i, oo) == -1)
		goto err;
	o = i & 0xffffffff;
	if(o & (1ull << 31)){
		o &= 0x7fffffff;
		if(preadbe64(f, &o, o) == -1)
			goto err;
	}
	return o;

err:
	fprint(2, "unable to read packfile: %r\n");
	return -1;
notfound:
	werrstr("not present: %H", h);
	return -1;		
}

/*
 * Scans for non-empty word, copying it into buf.
 * Strips off word, leading, and trailing space
 * from input.
 * 
 * Returns -1 on empty string or error, leaving
 * input unmodified.
 */
static int
scanword(char **str, int *nstr, char *buf, int nbuf)
{
	char *p;
	int n, r;

	r = -1;
	p = *str;
	n = *nstr;
	while(n && isblank(*p)){
		n--;
		p++;
	}

	for(; n && *p && !isspace(*p); p++, n--){
		r = 0;
		*buf++ = *p;
		nbuf--;
		if(nbuf == 0)
			return -1;
	}
	while(n && isblank(*p)){
		n--;
		p++;
	}
	*buf = 0;
	*str = p;
	*nstr = n;
	return r;
}

static void
nextline(char **str, int *nstr)
{
	char *s;

	if((s = strchr(*str, '\n')) != nil){
		*nstr -= s - *str + 1;
		*str = s + 1;
	}
}

static int
parseauthor(char **str, int *nstr, char **name, vlong *time)
{
	char buf[128];
	Resub m[4];
	char *p;
	int n, nm;

	if((p = strchr(*str, '\n')) == nil)
		sysfatal("malformed author line");
	n = p - *str;
	if(n >= sizeof(buf))
		sysfatal("overlong author line");
	memset(m, 0, sizeof(m));
	snprint(buf, n + 1, *str);
	*str = p;
	*nstr -= n;
	
	if(!regexec(authorpat, buf, m, nelem(m)))
		sysfatal("invalid author line %s", buf);
	nm = m[1].ep - m[1].sp;
	*name = emalloc(nm + 1);
	memcpy(*name, m[1].sp, nm);
	buf[nm] = 0;
	
	nm = m[2].ep - m[2].sp;
	memcpy(buf, m[2].sp, nm);
	buf[nm] = 0;
	*time = atoll(buf);
	return 0;
}

static void
parsecommit(Object *o)
{
	char *p, *t, buf[128];
	int np;

	p = o->data;
	np = o->size;
	o->commit = emalloc(sizeof(Cinfo));
	while(1){
		if(scanword(&p, &np, buf, sizeof(buf)) == -1)
			break;
		if(strcmp(buf, "tree") == 0){
			if(scanword(&p, &np, buf, sizeof(buf)) == -1)
				sysfatal("invalid commit: tree missing");
			if(hparse(&o->commit->tree, buf) == -1)
				sysfatal("invalid commit: garbled tree");
		}else if(strcmp(buf, "parent") == 0){
			if(scanword(&p, &np, buf, sizeof(buf)) == -1)
				sysfatal("invalid commit: missing parent");
			o->commit->parent = realloc(o->commit->parent, ++o->commit->nparent * sizeof(Hash));
			if(!o->commit->parent)
				sysfatal("unable to malloc: %r");
			if(hparse(&o->commit->parent[o->commit->nparent - 1], buf) == -1)
				sysfatal("invalid commit: garbled parent");
		}else if(strcmp(buf, "author") == 0){
			parseauthor(&p, &np, &o->commit->author, &o->commit->mtime);
		}else if(strcmp(buf, "committer") == 0){
			parseauthor(&p, &np, &o->commit->committer, &o->commit->ctime);
		}else if(strcmp(buf, "gpgsig") == 0){
			/* just drop it */
			if((t = strstr(p, "-----END PGP SIGNATURE-----")) == nil)
				sysfatal("malformed gpg signature");
			np -= t - p;
			p = t;
		}
		nextline(&p, &np);
	}
	while (np && isspace(*p)) {
		p++;
		np--;
	}
	o->commit->msg = p;
	o->commit->nmsg = np;
}

static void
parsetree(Object *o)
{
	char *p, buf[256];
	int np, nn, m;
	Dirent *t;

	p = o->data;
	np = o->size;
	o->tree = emalloc(sizeof(Tinfo));
	while(np > 0){
		if(scanword(&p, &np, buf, sizeof(buf)) == -1)
			break;
		o->tree->ent = erealloc(o->tree->ent, ++o->tree->nent * sizeof(Dirent));
		t = &o->tree->ent[o->tree->nent - 1];
		memset(t, 0, sizeof(Dirent));
		m = strtol(buf, nil, 8);
		/* FIXME: symlinks and other BS */
		if(m == 0160000){
			print("setting mode to link...\n");
			t->mode |= DMDIR;
			t->modref = 1;
		}
		t->mode = m & 0777;
		if(m & 0040000)
			t->mode |= DMDIR;
		t->name = p;
		nn = strlen(p) + 1;
		p += nn;
		np -= nn;
		if(np < sizeof(t->h.h))
			sysfatal("malformed tree %H, remaining %d (%s)", o->hash, np, p);
		memcpy(t->h.h, p, sizeof(t->h.h));
		p += sizeof(t->h.h);
		np -= sizeof(t->h.h);
	}
}

static void
parsetag(Object *)
{
}

void
parseobject(Object *o)
{
	if(o->flag & Cparsed)
		return;
	switch(o->type){
	case GTree:	parsetree(o);	break;
	case GCommit:	parsecommit(o);	break;
	case GTag:	parsetag(o);	break;
	default:	break;
	}
	o->flag |= Cparsed;
}

static Object*
readidxobject(Biobuf *idx, Hash h, int flag)
{
	char path[Pathmax];
	char hbuf[41];
	Biobuf *f;
	Object *obj;
	int l, i, n;
	vlong o;
	Dir *d;

	USED(idx);
	if((obj = osfind(&objcache, h)) != nil){
		if(obj->flag & Cloaded)
			return obj;
		if(obj->flag & Cidx){
			assert(idx != nil);
			o = Boffset(idx);
			if(Bseek(idx, obj->off, 0) == -1)
				sysfatal("could not seek to object offset");
			if(readpacked(idx, obj, flag) == -1)
				sysfatal("could not reload object %H", obj->hash);
			if(Bseek(idx, o, 0) == -1)
				sysfatal("could not restore offset");
			cache(obj);
			return obj;
		}
	}

	d = nil;
	obj = emalloc(sizeof(Object));
	obj->id = objcache.nobj + 1;
	obj->hash = h;

	snprint(hbuf, sizeof(hbuf), "%H", h);
	snprint(path, sizeof(path), ".git/objects/%c%c/%s", hbuf[0], hbuf[1], hbuf + 2);
	if((f = Bopen(path, OREAD)) != nil){
		if(readloose(f, obj, flag) == -1)
			goto error;
		Bterm(f);
		parseobject(obj);
		cache(obj);
		return obj;
	}

	if ((n = slurpdir(".git/objects/pack", &d)) == -1)
		goto error;
	o = -1;
	for(i = 0; i < n; i++){
		l = strlen(d[i].name);
		if(l > 4 && strcmp(d[i].name + l - 4, ".idx") != 0)
			continue;
		snprint(path, sizeof(path), ".git/objects/pack/%s", d[i].name);
		if((f = Bopen(path, OREAD)) == nil)
			continue;
		o = searchindex(f, h);
		Bterm(f);
		if(o == -1)
			continue;
		break;
	}

	if (o == -1)
		goto error;

	if((n = snprint(path, sizeof(path), "%s", path)) >= sizeof(path) - 4)
		goto error;
	memcpy(path + n - 4, ".pack", 6);
	if((f = Bopen(path, OREAD)) == nil)
		goto error;
	if(Bseek(f, o, 0) == -1)
		goto error;
	if(readpacked(f, obj, flag) == -1)
		goto error;
	Bterm(f);
	parseobject(obj);
	cache(obj);
	return obj;
error:
	free(d);
	free(obj);
	return nil;
}

Object*
readobject(Hash h)
{
	Object *o;

	o = readidxobject(nil, h, 0);
	if(o)
		ref(o);
	return o;
}

int
objcmp(void *pa, void *pb)
{
	Object *a, *b;

	a = *(Object**)pa;
	b = *(Object**)pb;
	return memcmp(a->hash.h, b->hash.h, sizeof(a->hash.h));
}

static int
hwrite(Biobuf *b, void *buf, int len, DigestState **st)
{
	*st = sha1(buf, len, nil, *st);
	return Bwrite(b, buf, len);
}

int
indexpack(char *pack, char *idx, Hash ph)
{
	char hdr[4*3], buf[8];
	int nobj, nvalid, nbig, n, i, step;
	Object *o, **objects;
	DigestState *st;
	char *valid;
	Biobuf *f;
	Hash h;
	int c;

	if((f = Bopen(pack, OREAD)) == nil)
		return -1;
	if(Bread(f, hdr, sizeof(hdr)) != sizeof(hdr)){
		werrstr("short read on header");
		return -1;
	}
	if(memcmp(hdr, "PACK\0\0\0\2", 8) != 0){
		werrstr("invalid header");
		return -1;
	}

	nvalid = 0;
	nobj = GETBE32(hdr + 8);
	objects = calloc(nobj, sizeof(Object*));
	valid = calloc(nobj, sizeof(char));
	step = nobj/100;
	if(!step)
		step++;
	while(nvalid != nobj){
		fprint(2, "indexing (%d/%d):", nvalid, nobj);
		n = 0;
		for(i = 0; i < nobj; i++){
			if(valid[i]){
				n++;
				continue;
			}
			if(i % step == 0)
				fprint(2, ".");
			if(!objects[i]){
				o = emalloc(sizeof(Object));
				o->off = Boffset(f);
				objects[i] = o;
			}
			o = objects[i];
			Bseek(f, o->off, 0);
			if (readpacked(f, o, Cidx) == 0){
				sha1((uchar*)o->all, o->size + strlen(o->all) + 1, o->hash.h, nil);
				cache(o);
				valid[i] = 1;
				n++;
			}
		}
		fprint(2, "\n");
		if(n == nvalid){
			sysfatal("fix point reached too early: %d/%d: %r", nvalid, nobj);
			goto error;
		}
		nvalid = n;
	}
	Bterm(f);

	st = nil;
	qsort(objects, nobj, sizeof(Object*), objcmp);
	if((f = Bopen(idx, OWRITE)) == nil)
		return -1;
	if(Bwrite(f, "\xfftOc\x00\x00\x00\x02", 8) != 8)
		goto error;
	/* fanout table */
	c = 0;
	for(i = 0; i < 256; i++){
		while(c < nobj && (objects[c]->hash.h[0] & 0xff) <= i)
			c++;
		PUTBE32(buf, c);
		hwrite(f, buf, 4, &st);
	}
	for(i = 0; i < nobj; i++){
		o = objects[i];
		hwrite(f, o->hash.h, sizeof(o->hash.h), &st);
	}

	/* fuck it, pointless */
	for(i = 0; i < nobj; i++){
		PUTBE32(buf, 42);
		hwrite(f, buf, 4, &st);
	}

	nbig = 0;
	for(i = 0; i < nobj; i++){
		if(objects[i]->off <= (1ull<<31))
			PUTBE32(buf, objects[i]->off);
		else
			PUTBE32(buf, (1ull << 31) | nbig++);
		hwrite(f, buf, 4, &st);
	}
	for(i = 0; i < nobj; i++){
		if(objects[i]->off > (1ull<<31)){
			PUTBE64(buf, objects[i]->off);
			hwrite(f, buf, 8, &st);
		}
	}
	hwrite(f, ph.h, sizeof(ph.h), &st);
	sha1(nil, 0, h.h, st);
	Bwrite(f, h.h, sizeof(h.h));

	free(objects);
	free(valid);
	Bterm(f);
	return 0;

error:
	free(objects);
	free(valid);
	Bterm(f);
	return -1;
}
