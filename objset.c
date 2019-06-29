#include <u.h>
#include <libc.h>
#include <pool.h>

#include "git.h"

void
osinit(Objset *s)
{
	s->sz = 16;
	s->nobj = 0;
	s->obj = emalloc(s->sz * sizeof(Hash));
}

void
osfree(Objset *s)
{
	free(s->obj);
}

void
osadd(Objset *s, Object *o)
{
	u32int probe;
	Object **obj;
	int i, sz;

	probe = GETBE32(o->hash.h) % s->sz;
	while(s->obj[probe]){
		if(hasheq(&s->obj[probe]->hash, &o->hash))
			return;
		probe = (probe + 1) % s->sz;
	}
	assert(s->obj[probe] == nil);
	s->obj[probe] = o;
	s->nobj++;
	if(s->sz < 2*s->nobj){
		sz = s->sz;
		obj = s->obj;

		s->sz *= 2;
		s->nobj = 0;
		s->obj = emalloc(s->sz * sizeof(Hash));
		for(i = 0; i < sz; i++)
			if(obj[i])
				osadd(s, obj[i]);
		free(obj);
	}
}

Object*
osfind(Objset *s, Hash h)
{
	u32int probe;

	for(probe = GETBE32(h.h) % s->sz; s->obj[probe]; probe = (probe + 1) % s->sz)
		if(hasheq(&s->obj[probe]->hash, &h))
			return s->obj[probe]; 
	return 0;
}

int
oshas(Objset *s, Object *o)
{
	return osfind(s, o->hash) != nil;
}
