#include <u.h>
#include <libc.h>
#include <ctype.h>

#include "git.h"

Reprog *authorpat;
Hash Zhash;

int
hasheq(Hash *a, Hash *b)
{
	return memcmp(a->h, b->h, sizeof(a->h)) == 0;
}

static int
charval(int c, int *err)
{
	if(c >= '0' && c <= '9')
		return c - '0';
	if(c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if(c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	*err = 1;
	return -1;
}

void *
emalloc(ulong n)
{
	void *v;
	
	v = mallocz(n, 1);
	if(v == nil)
		sysfatal("malloc: %r");
	setmalloctag(v, getcallerpc(&n));
	return v;
}

void *
erealloc(void *p, ulong n)
{
	void *v;
	
	v = realloc(p, n);
	if(v == nil)
		sysfatal("realloc: %r");
	setmalloctag(v, getcallerpc(&n));
	return v;
}

char*
estrdup(char *s)
{
	s = strdup(s);
	if(s == nil)
		sysfatal("strdup: %r");
	setmalloctag(s, getcallerpc(&s));
	return s;
}

int
Hfmt(Fmt *fmt)
{
	Hash h;
	int i, n, l;
	char c0, c1;

	l = 0;
	h = va_arg(fmt->args, Hash);
	for(i = 0; i < sizeof h.h; i++){
		n = (h.h[i] >> 4) & 0xf;
		c0 = (n >= 10) ? n-10 + 'a' : n + '0';
		n = h.h[i] & 0xf;
		c1 = (n >= 10) ? n-10 + 'a' : n + '0';
		l += fmtprint(fmt, "%c%c", c0, c1);
	}
	return l;
}

int
Tfmt(Fmt *fmt)
{
	Type t;
	int l;

	t = va_arg(fmt->args, Type);
	switch(t){
	case GNone:	l = fmtprint(fmt, "none");	break;
	case GCommit:	l = fmtprint(fmt, "commit");	break;
	case GTree:	l = fmtprint(fmt, "tree");	break;
	case GBlob:	l = fmtprint(fmt, "blob");	break;
	case GTag:	l = fmtprint(fmt, "tag");	break;
	case GOdelta:	l = fmtprint(fmt, "odelta");	break;
	case GRdelta:	l = fmtprint(fmt, "gdelta");	break;
	default:	l = fmtprint(fmt, "?%d?", t);	break;
	}
	return l;
}

int
Ofmt(Fmt *fmt)
{
	Object *o;
	int l;

	o = va_arg(fmt->args, Object *);
	print("== %H (%T) ==\n", o->hash, o->type);
	switch(o->type){
	case GTree:
		l = fmtprint(fmt, "tree\n");
		break;
	case GBlob:
		l = fmtprint(fmt, "blob %s\n", o->data);
		break;
	case GCommit:
		l = fmtprint(fmt, "commit\n");
		break;
	case GTag:
		l = fmtprint(fmt, "tag\n");
		break;
	default:
		l = fmtprint(fmt, "invalid: %d\n", o->type);
		break;
	}
	return l;
}

int
Qfmt(Fmt *fmt)
{
	Qid q;

	q = va_arg(fmt->args, Qid);
	return fmtprint(fmt, "Qid{path=0x%llx(dir:%d,obj:%lld), vers=%ld, type=%d}",
	    q.path, QDIR(&q), (q.path >> 8), q.vers, q.type);
}

void
gitinit(void)
{
	fmtinstall('H', Hfmt);
	fmtinstall('T', Tfmt);
	fmtinstall('O', Ofmt);
	fmtinstall('Q', Qfmt);
	inflateinit();
	deflateinit();
	authorpat = regcomp("[\t ]*(.*)[\t ]+([0-9]+)[\t ]+([\\-+]?[0-9]+)");
	osinit(&objcache);
}

int
hparse(Hash *h, char *b)
{
	int i, err;

	err = 0;
	for(i = 0; i < sizeof(h->h); i++){
		err = 0;
		h->h[i] = 0;
		h->h[i] |= ((charval(b[2*i], &err) & 0xf) << 4);
		h->h[i] |= ((charval(b[2*i+1], &err)& 0xf) << 0);
		if(err){
			werrstr("invalid hash");
			return -1;
		}
	}
	return 0;
}

int
slurpdir(char *p, Dir **d)
{
	int r, f;

	if((f = open(p, OREAD)) == -1)
		return -1;
	r = dirreadall(f, d);
	close(f);
	return r;
}	

int
hassuffix(char *base, char *suf)
{
	int nb, ns;

	nb = strlen(base);
	ns = strlen(suf);
	if(ns <= nb && strcmp(base + (nb - ns), suf) == 0)
		return 1;
	return 0;
}

int
swapsuffix(char *dst, int dstsz, char *base, char *oldsuf, char *suf)
{
	int bl, ol, sl, l;

	bl = strlen(base);
	ol = strlen(oldsuf);
	sl = strlen(suf);
	l = bl + sl - ol;
	if(l + 1 > dstsz || ol > bl)
		return -1;
	memmove(dst, base, bl - ol);
	memmove(dst + bl - ol, suf, sl);
	dst[l] = 0;
	return l;
}

char *
strip(char *s)
{
	char *e;

	while(isspace(*s))
		s++;
	e = s + strlen(s);
	while(e > s && isspace(*--e))
		*e = 0;
	return s;
}

void
die(char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprint(2, fmt, ap);
	va_end(ap);
	abort();
}
