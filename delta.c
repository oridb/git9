#include <u.h>
#include <libc.h>

#include "git.h"

typedef struct Dblock	Dblock;
typedef struct Delta	Delta;
typedef struct Dtab	Dtab;

enum {
	Blksz	= 127,
	Hshift	= 113,
	Hlast	= 1692137473L,
};

struct Dblock {
	uchar	*buf;
	int	len;
	int	off;
	u64int	rhash;
};

struct Dtab {
	Dblock	*b;
	int	nb;
	int	sz;
};

static void
addblk(Dtab *dt, void *buf, int len, int off, u64int rh)
{
	int i, sz, probe;
	Dblock *db;

	probe = rh % dt->sz;
	while(dt->b[probe].buf != nil){
		if(len == dt->b[probe].len && memcmp(buf, dt->b[probe].buf, len) == 0)
			return;
		probe = (probe + 1) % dt->sz;
	}
	assert(dt->b[probe].buf == nil);
	dt->b[probe].buf = buf;
	dt->b[probe].len = len;
	dt->b[probe].off = off;
	dt->b[probe].rhash = rh;
	dt->nb++;
	if(dt->sz < 2*dt->nb){
		sz = dt->sz;
		db = dt->b;
		dt->sz *= 2;
		dt->nb = 0;
		dt->b = emalloc(dt->sz * sizeof(Dblock));
		for(i = 0; i < sz; i++)
			if(db[i].buf != nil)
				addblk(dt, db[i].buf, db[i].len, db[i].off, db[i].rhash);
		free(db);
	}		
}

static Dblock*
findrough(Dtab *dt, u64int rh)
{
	int probe;

	for(probe = rh % dt->sz; dt->b[probe].buf != nil; probe = (probe + 1) % dt->sz)
		if(dt->b[probe].rhash == rh)
			return &dt->b[probe];
	return nil;
}

static int
nextblk(uchar *s, uchar *e, u64int *rh)
{
	uchar *p;
	int i;

	p = s;
	*rh = 0;
	for(i = 0; i < Blksz; i++){
		if(p == e)
			break;
		/* FIXME: better hash */
		*rh *= Hshift;
		*rh += *p++;
	}
	return p - s;
}

static int
sameblk(Dblock *b, uchar *s, uchar *e)
{
	if(b->len != e - s)
		return 0;
	return memcmp(b->buf, s, b->len) == 0;
}

static int
emitdelta(Delta **pd, int *nd, int cpy, int off, int len)
{
	Delta *d;

	if(len == 0)
		return 0;
	*nd += 1;
	*pd = erealloc(*pd, *nd * sizeof(Delta));
	d = &(*pd)[*nd - 1];
	d->cpy = cpy;
	d->off = off;
	d->len = len;
	return len;
}


Delta*
deltify(void *targ, int ntarg, void *base, int nbase, int *pnd)
{
	Dblock *k;
	Delta *d;
	Dtab dt;
	uchar *l, *s, *e, *eb, *bp, *tp;
	int i, nd, nb;
	u64int rh;

	bp = base;
	tp = targ;
	s = bp;
	e = bp;
	dt.nb = 0;
	dt.sz = 128;
	dt.b = emalloc(dt.sz*sizeof(Dblock));
	while(e != bp + nbase){
		e += nextblk(s, bp + nbase, &rh);
		addblk(&dt, s, e - s, s - bp, rh);
		s = e;
	}

	l = targ;
	s = targ;
	e = targ;
	d = nil;
	nd = 0;
	e += nextblk(s, tp + ntarg, &rh);
	while(1){
		if((k = findrough(&dt, rh)) != nil){
			if(sameblk(k, s, e)){
				nb = k->len;
				eb = k->buf + k->len;
				/* stretch the block as far as it'll go */
				for(i = 0; i < (1<<24) - Blksz; i++){
					if(e == tp + ntarg || eb == bp + nbase)
						break;
					if(*e != *eb)
						break;
					nb++;
					eb++;
					e++;
				}
				emitdelta(&d, &nd, 0, l - tp, s - l);
				emitdelta(&d, &nd, 1, k->off, nb);
				s = e;
				l = e;
				e += nextblk(s, tp + ntarg, &rh);
				continue;
			}
		}
		if(e == tp + ntarg)
			break;
		/* got a better hash? apply within! */
		rh -= *s++ * Hlast;
		rh *= Hshift;
		rh += *e++;
	}
	emitdelta(&d, &nd, 0, l - tp, tp + ntarg - l);
	*pnd = nd;
	free(dt.b);
	return d;
}
