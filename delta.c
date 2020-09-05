#include <u.h>
#include <libc.h>

#include "git.h"

typedef struct Dblock	Dblock;
typedef struct Delta	Delta;
typedef struct Dset	Dset;
typedef struct Objmeta	Objmeta;
typedef struct Deltaset	Deltaset;

enum {
	Blksz	= 127,
	Hshift	= 113,
	Hlast	= 1692137473L,
};

struct Dblock {
	uchar	*p;
	uchar	*s;
	uchar	*e;
	u64int	rhash;
	Hash	chash;
};

struct Objmeta {
	Object	*obj;
	char	*name;
	vlong	time;
	Delta	*delta;
	int	ndelta;
};

struct Deltaset {
	Objset	skip;
	Objset	send;
	Objmeta	*meta;
	int	nmeta;
	int	metasz;
};

static u64int
addh(u64int h, uchar v)
{
	return h + v;
}

static int
blkcmp(void *pa, void *pb)
{
	Dblock *a, *b;

	a = (Dblock*)pa;
	b = (Dblock*)pb;
	if(b->rhash == a->rhash)
		return 0;
	return (a->rhash > b->rhash) ? -1 : 1;
}

static void
initblk(Dblock *b, uchar *p, uchar *s, uchar *e, u64int rh)
{
	b->p = p;
	b->s = s;
	b->e = e;
	b->rhash = rh;
	sha1(s, e - s, b->chash.h, nil);
}

static Dblock*
findrough(Dblock *b, int nb, u64int rh)
{
	int mid, lo, hi;

	lo = 0;
	hi = nb;
	while(lo <= hi){
		mid = (lo + hi)/2;
		if(b[mid].rhash == rh)
			return &b[mid];
		else if(b[mid].rhash > rh)
			lo = mid + 1;
		else
			hi = mid - 1;
	}
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
sameblk(Dblock *b, Hash h, uchar *s, uchar *e)
{
	int n;

	n = b->e - b->s;
	if(n != e - s)
		return 0;
	return hasheq(&b->chash, &h) && memcmp(b->s, s, n) == 0;
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
	Hash h;
	Dblock *b, *k;
	Delta *d;
	uchar *l, *s, *e, *eb, *bp, *tp;
	int i, nb, nd;
	u64int rh;

	bp = base;
	tp = targ;
	s = bp;
	e = bp;
	nb = (nbase + Blksz - 1) / Blksz;
	b = emalloc(nb*sizeof(Dblock));
	for(i = 0; i < nb; i++){
		e += nextblk(s, bp + nbase, &rh);
		initblk(&b[i], bp, s, e, rh);
		s = e;
	}
	qsort(b, nb, sizeof(*b), blkcmp);

	l = targ;
	s = targ;
	e = targ;
	d = nil;
	nd = 0;
	e += nextblk(s, tp + ntarg, &rh);
	while(1){
		if((k = findrough(b, nb, rh)) != nil){
			sha1(s, e - s, h.h, nil);
			if(sameblk(k, h, s, e)){
				eb = k->e;
				/* stretch the block as far as it'll go */
				for(i = 0; i < (1<<24) - Blksz; i++){
					if(e == tp + ntarg || eb == bp + nbase)
						break;
					if(*e != *eb)
						break;
					eb++;
					e++;
				}
				emitdelta(&d, &nd, 0, l - tp, s - l);
				emitdelta(&d, &nd, 1, k->s - k->p, eb - k->s);
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
	free(b);
	return d;
}
