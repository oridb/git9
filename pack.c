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


static u32int
crc32(u32int crc, char *b, int nb)
{
	static u32int crctab[256] = {
		0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f, 0xe963a535, 
		0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988, 0x09b64c2b, 0x7eb17cbd, 
		0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de, 0x1adad47d, 
		0x6ddde4eb, 0xf4d4b551, 0x83d385c7, 0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 
		0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 
		0xa2677172, 0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c, 
		0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59, 0x26d930ac, 
		0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423, 0xcfba9599, 0xb8bda50f, 
		0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924, 0x2f6f7c87, 0x58684c11, 0xc1611dab, 
		0xb6662d3d, 0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 
		0x9fbfe4a5, 0xe8b8d433, 0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 
		0x086d3d2d, 0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e, 
		0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 
		0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65, 0x4db26158, 0x3ab551ce, 
		0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 
		0x346ed9fc, 0xad678846, 0xda60b8d0, 0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 
		0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 
		0xce61e49f, 0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81, 
		0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a, 0xead54739, 
		0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8, 
		0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1, 0xf00f9344, 0x8708a3d2, 0x1e01f268, 
		0x6906c2fe, 0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 
		0x10da7a5a, 0x67dd4acc, 0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 
		0xa1d1937e, 0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b, 
		0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55, 0x316e8eef, 
		0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236, 0xcc0c7795, 0xbb0b4703, 
		0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 
		0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d, 0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 
		0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 
		0x0cb61b38, 0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242, 
		0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777, 0x88085ae6, 
		0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45, 
		0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2, 0xa7672661, 0xd06016f7, 0x4969474d, 
		0x3e6e77db, 0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 
		0x47b2cf7f, 0x30b5ffe9, 0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 
		0xcdd70693, 0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94, 
		0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
	};
	int i;

	crc ^=  0xFFFFFFFF;
	for(i = 0; i < nb; i++)
		crc = (crc >> 8) ^ crctab[(crc ^ b[i]) & 0xFF];
	return crc ^ 0xFFFFFFFF;
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
	o->len = Boffset(f) - o->off;
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
	o->len = Boffset(f) - o->off;
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
		o->len = Boffset(f) - o->off;
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

static u32int
objectcrc(Biobuf *f, Object *o)
{
	char buf[8096];
	int n, r;

	o->crc = 0;
	Bseek(f, o->off, 0);
	for(n = o->len; n > 0; n -= r){
		r = Bread(f, buf, n > sizeof(buf) ? sizeof(buf) : n);
		if(r == -1)
			return -1;
		if(r == 0)
			return 0;
		o->crc = crc32(o->crc, buf, r);
	}
	return 0;
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
			if(objectcrc(f, o) == -1)
				return -1;
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
	if(hwrite(f, "\xfftOc\x00\x00\x00\x02", 8, &st) != 8)
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
		PUTBE32(buf, objects[i]->crc);
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
