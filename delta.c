#include <u.h>
#include <libc.h>

#include "git.h"

enum {
	K	= 3,
	Bconst	= 42,
	Bmask	= 0x7f,
	Bshift	= 7,
	Hshift	= 113,
	Hlast	= 1692137473L,
};

static void
addblk(Dtab *dt, void *buf, int len, int off, u64int rh)
{
	int i, sz, probe;
	Dblock *db;

	rh >>= Bshift;
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
		dt->b = eamalloc(dt->sz, sizeof(Dblock));
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

	rh >>= Bshift;
	for(probe = rh % dt->sz; dt->b[probe].buf != nil; probe = (probe + 1) % dt->sz)
		if(dt->b[probe].rhash == rh)
			return &dt->b[probe];
	return nil;
}

static int
nextblk(uchar *s, uchar *e, u64int *ph)
{
	u64int rh;
	uchar *p;
	int i;

	p = s;
	rh = 0;
	for(i = 0; (rh & Bmask) != Bconst; i++){
		if(p == e)
			break;
		/* FIXME: better hash */
		rh *= Hshift;
		rh += *p++ + K;
	}
	*ph = rh;
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

void
dtinit(Dtab *dt, void *base, int nbase)
{
	uchar *bp, *s, *e;
	u64int rh;
	
	bp = base;
	s = bp;
	e = bp;
	rh = 0;
	dt->nb = 0;
	dt->sz = 128;
	dt->b = eamalloc(dt->sz, sizeof(Dblock));
	while(e != bp + nbase){
		e += nextblk(s, bp + nbase, &rh);
		addblk(dt, s, e - s, s - bp, rh);
		s = e;
	}
}

void
dtclear(Dtab *dt)
{
	free(dt->b);
}

Delta*
deltify(void *targ, int ntarg, Dtab *dt, int *pnd)
{
	Dblock *k;
	Delta *d;
	uchar *l, *s, *e, *eb, *tp;
	int i, nd, nb;
	u64int rh;


	tp = targ;
	l = targ;
	s = targ;
	e = targ;
	d = nil;
	nd = 0;
	rh = 0;
	e += nextblk(s, tp + ntarg, &rh);
	while(1){
		if((rh & Bmask) == Bconst && (k = findrough(dt, rh)) != nil){
			if(sameblk(k, s, e)){
				nb = k->len;
				eb = k->buf + k->len;
				/* stretch the block: 1<<24 is the max packfiles support. */
				for(i = 0; i < (1<<24) - nb; i++){
					if(e == tp + ntarg || eb == dt->base + dt->nbase)
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
		rh += *e++ + K;
	}
	emitdelta(&d, &nd, 0, l - tp, tp + ntarg - l);
	*pnd = nd;
	return d;
}
