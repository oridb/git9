#include <u.h>
#include <libc.h>
#include <pool.h>

#include "git.h"

typedef struct Objq	Objq;
typedef struct Buf	Buf;
typedef struct Compout	Compout;
typedef struct Update	Update;

struct Buf {
	int off;
	int sz;
	uchar *data;
};

struct Compout {
	int fd;
	DigestState *st;
};

struct Objq {
	Objq *next;
	Object *obj;
};

struct Update {
	char	ref[128];
	Hash	theirs;
	Hash	ours;
};

int sendall;
int force;
char *curbranch = "refs/heads/master";
char *removed[128];
int nremoved;

static int
hwrite(int fd, void *buf, int nbuf, DigestState **st)
{
	if(write(fd, buf, nbuf) != nbuf)
		return -1;
	*st = sha1(buf, nbuf, nil, *st);
	return nbuf;
}

void
pack(Objset *send, Objset *skip, Object *o)
{
	Dirent *e;
	Object *s;
	int i;

	if(oshas(send, o) || oshas(skip, o))
		return;
	osadd(send, o);
	switch(o->type){
	case GCommit:
		if((s = readobject(o->commit->tree)) == nil)
			sysfatal("could not read tree %H: %r", o->hash);
		pack(send, skip, s);
		break;
	case GTree:
		for(i = 0; i < o->tree->nent; i++){
			e = &o->tree->ent[i];
			if ((s = readobject(e->h)) == nil)
				sysfatal("could not read entry %H: %r", e->h);
			pack(send, skip, s);
		}
		break;
	default:
		break;
	}
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
	Compout *o;

	o = p;
	o->st = sha1(buf, n, nil, o->st);
	return write(o->fd, buf, n);
}

int
compress(int fd, void *buf, int sz, DigestState **st)
{
	int r;
	Buf b ={
		.off=0,
		.data=buf,
		.sz=sz,
	};
	Compout o = {
		.fd = fd,
		.st = *st,
	};

	r = deflatezlib(&o, compwrite, &b, compread, 6, 0);
	*st = o.st;
	return r;
}

int
writeobject(int fd, Object *o, DigestState **st)
{
	char hdr[8];
	uvlong sz;
	int i;

	i = 1;
	sz = o->size;
	hdr[0] = o->type << 4;
	hdr[0] |= sz & 0xf;
	if(sz >= (1 << 4)){
		hdr[0] |= 0x80;
		sz >>= 4;
	
		for(i = 1; i < sizeof(hdr); i++){
			hdr[i] = sz & 0x7f;
			if(sz <= 0x7f){
				i++;
				break;
			}
			hdr[i] |= 0x80;
			sz >>= 7;
		}
	}

	if(hwrite(fd, hdr, i, st) != i)
		return -1;
	if(compress(fd, o->data, o->size, st) == -1)
		return -1;
	return 0;
}

int
writepack(int fd, Update *upd, int nupd)
{
	Objset send, skip;
	Object *o, *p;
	Objq *q, *n, *e;
	DigestState *st;
	Update *u;
	char buf[4];
	Hash h;
	int i;

	osinit(&send);
	osinit(&skip);
	for(i = 0; i < nupd; i++){
		u = &upd[i];
		if(hasheq(&u->theirs, &Zhash))
			continue;
		if((o = readobject(u->theirs)) == nil)
			sysfatal("could not read %H", u->theirs);
		pack(&skip, &skip, o);
	}

	q = nil;
	e = nil;
	for(i = 0; i < nupd; i++){
		u = &upd[i];
		if((o = readobject(u->ours)) == nil)
			sysfatal("could not read object %H", u->ours);

		n = emalloc(sizeof(Objq));
		n->obj = o;
		if(!q){
			q = n;
			e = n;
		}else{
			e->next = n;
		}
	}

	for(n = q; n; n = n->next)
		e = n;
	for(; q; q = n){
		o = q->obj;
		if(oshas(&skip, o) || oshas(&send, o))
			goto iter;
		pack(&send, &skip, o);
		for(i = 0; i < o->commit->nparent; i++){
			if((p = readobject(o->commit->parent[i])) == nil)
				sysfatal("could not read parent of %H", o->hash);
			e->next = emalloc(sizeof(Objq));
			e->next->obj = p;
			e = e->next;
		}
iter:
		n = q->next;
		free(q);
	}

	st = nil;
	PUTBE32(buf, send.nobj);
	if(hwrite(fd, "PACK\0\0\0\02", 8, &st) != 8)
		return -1;
	if(hwrite(fd, buf, 4, &st) == -1)
		return -1;
	for(i = 0; i < send.sz; i++){
		if(!send.obj[i])
			continue;
		if(writeobject(fd, send.obj[i], &st) == -1)
			return -1;
	}
	sha1(nil, 0, h.h, st);
	if(write(fd, h.h, sizeof(h.h)) == -1)
		return -1;
	return 0;
}

Update*
findref(Update *u, int nu, char *ref)
{
	int i;

	for(i = 0; i < nu; i++)
		if(strcmp(u[i].ref, ref) == 0)
			return &u[i];
	return nil;
}

int
readours(Update **ret)
{
	Update *u, *r;
	Hash *h;
	int nd, nu, i;
	char *pfx;
	Dir *d;

	nu = 0;
	if(!sendall){
		u = emalloc((nremoved + 1)*sizeof(Update));
		snprint(u[nu].ref, sizeof(u[nu].ref), "%s", curbranch);
		if(resolveref(&u[nu].ours, curbranch) == -1)
			sysfatal("broken branch %s", curbranch);
		nu++;
	}else{
		if((nd = slurpdir(".git/refs/heads", &d)) == -1)
			sysfatal("read branches: %r");
		u = emalloc((nremoved + nd)*sizeof(Update));
		for(i = 0; i < nd; i++){
			snprint(u->ref, sizeof(u->ref), "refs/heads/%s", d[nu].name);
			if(resolveref(&u[nu].ours, u[nu].ref) == -1)
				continue;
			nu++;
		}
	}
	for(i = 0; i < nremoved; i++){
		pfx = "refs/heads/";
		if(strstr(removed[i], "heads/") == removed[i])
			pfx = "refs/";
		if(strstr(removed[i], "refs/heads/") == removed[i])
			pfx = "";
		snprint(u[nu].ref, sizeof(u[nu].ref), "%s%s", pfx, removed[i]);
		h = &u[nu].ours;
		if((r = findref(u, nu, u[nu].ref)) != nil)
			h = &r->ours;
		else
			nu++;
		memcpy(h, &Zhash, sizeof(Hash));
	}

	*ret = u;
	return nu;	
}

int
sendpack(int fd)
{
	int i, n, r, nupd, nsp, updating;
	char buf[65536], *sp[3];
	Update *upd, *u;
	Object *a, *b;

	if((nupd = readours(&upd)) == -1)
		sysfatal("read refs: %r");
	while(1){
		n = readpkt(fd, buf, sizeof(buf));
		if(n == -1)
			return -1;
		if(n == 0)
			break;
		if(strncmp(buf, "ERR ", 4) == 0)
			sysfatal("%s", buf + 4);

		if(getfields(buf, sp, nelem(sp), 1, " \t\n\r") != 2)
			sysfatal("invalid ref line %.*s", utfnlen(buf, n), buf);
		if((u = findref(upd, nupd, sp[1])) == nil)
			continue;
		if(hparse(&u->theirs, sp[0]) == -1)
			sysfatal("invalid hash %s", sp[0]);
		snprint(u->ref, sizeof(u->ref), sp[1]);
	}

	r = 0;
	updating = 0;
	for(i = 0; i < nupd; i++){
		u = &upd[i];
		a = readobject(u->theirs);
		b = readobject(u->ours);
		if((!force || hasheq(&u->theirs, &Zhash)) && (a == nil || ancestor(a, b) != a)){
			fprint(2, "remote has diverged\n");
			werrstr("force needed");
			updating=0;
			r = -1;
			break;
		}
		if(hasheq(&u->ours, &Zhash)){
			print("removed %s\n", u->ref);
			continue;
		}
		if(hasheq(&u->theirs, &u->ours)){
			print("uptodate %s\n", u->ref);
			continue;
		}
		print("update %s %H %H\n", u->ref, u->theirs, u->ours);
		n = snprint(buf, sizeof(buf), "%H %H %s", u->theirs, u->ours, u->ref);

		/*
		 * Workaround for github.
		 *
		 * Github will accept the pack but fail to update the references
		 * if we don't have capabilities advertised. Report-status seems
		 * harmless to add, so we add it.
		 *
		 * Github doesn't advertise any capabilities, so we can't check
		 * for compatibility. We just need to add it blindly.
		 */
		if(i == 0){
			buf[n++] = '\0';
			n += snprint(buf + n, sizeof(buf) - n, " report-status");
		}
		if(writepkt(fd, buf, n) == -1)
			sysfatal("unable to send update pkt");
		updating = 1;
	}
	flushpkt(fd);
	if(updating){
		if(writepack(fd, upd, nupd) == -1)
			return -1;

		/* We asked for a status report, may as well use it. */
		while((n = readpkt(fd, buf, sizeof(buf))) > 0){
 			buf[n] = 0;
			nsp = getfields(buf, sp, nelem(sp), 1, " \t\n\r");
			if(nsp < 2) 
				continue;
			if(nsp < 3)
				sp[2] = "";
			/*
			 * Only report errors; successes will be reported by
			 * surrounding scripts.
			 */
			if(strcmp(sp[0], "unpack") == 0 && strcmp(sp[1], "ok") != 0)
				fprint(2, "unpack %s\n", sp[1]);
			else if(strcmp(sp[0], "ng") == 0)
				fprint(2, "failed update: %s\n", sp[1]);
			else
				continue;
			r = -1;
		}
	}
	return r;
}

void
usage(void)
{
	fprint(2, "usage: %s remote [reponame]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char proto[Nproto], host[Nhost], port[Nport];
	char repo[Nrepo], path[Npath];
	int fd;

	ARGBEGIN{
	default:	usage();	break;
	case 'a':	sendall++;	break;
	case 'd':	chattygit++;	break;
	case 'f':	force++;	break;
	case 'r':
		if(nremoved == nelem(removed))
			sysfatal("too many deleted branches");
		removed[nremoved++] = EARGF(usage());
		break;
	case 'b':
		curbranch = smprint("refs/%s", EARGF(usage()));
		break;
	}ARGEND;

	gitinit();
	if(argc != 1)
		usage();
	fd = -1;
	if(parseuri(argv[0], proto, host, port, path, repo) == -1)
		sysfatal("bad uri %s", argv[0]);
	if(strcmp(proto, "ssh") == 0 || strcmp(proto, "git+ssh") == 0)
		fd = dialssh(host, port, path, "receive");
	else if(strcmp(proto, "git") == 0)
		fd = dialgit(host, port, path, "receive");
	else if(strcmp(proto, "http") == 0 || strcmp(proto, "git+http") == 0)
		sysfatal("http clone not implemented");
	else
		sysfatal("unknown protocol %s", proto);
	
	if(fd == -1)
		sysfatal("could not dial %s:%s: %r", proto, host);
	if(sendpack(fd) == -1)
		sysfatal("send failed: %r");
	exits(nil);
}
