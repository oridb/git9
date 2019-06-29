#include <u.h>
#include <libc.h>
#include <ctype.h>
#include <fcall.h>
#include <thread.h>
#include <9p.h>

#include "git.h"

typedef struct Ols Ols;

char *Eperm = "permission denied";
char *Eexist = "does not exist";
char *E2long = "path too long";
char *Enodir = "not a directory";
char *Erepo = "unable to read repo";
char *Egreg = "wat";

enum {
	Qroot,
	Qhead,
	Qbranch,
	Qcommit,
	Qcommitmsg,
	Qcommitparent,
	Qcommittree,
	Qcommitdata,
	Qcommithash,
	Qcommitauthor,
	Qobject,
	Qctl,
	Qmax,
	Internal=1<<7,
};

typedef struct Gitaux Gitaux;
struct Gitaux {
	int	 npath;
	Qid	 path[Npath];
	Object  *opath[Npath];
	char 	*refpath;
	int	 qdir;
	vlong	 mtime;
	Object	*obj;

	/* For listing object dir */
	Ols	*ols;
	Object	*olslast;
};

char *qroot[] = {
	"HEAD",
	"branch",
	"object",
	"ctl",
};

char *username;
char *mtpt = "/mnt/git";
char **branches = nil;

static vlong
findbranch(Gitaux *aux, char *path)
{
	int i;

	for(i = 0; branches[i]; i++)
		if(strcmp(path, branches[i]) == 0)
			goto found;
	branches = realloc(branches, sizeof(char *)*(i + 2));
	branches[i] = estrdup(path);
	branches[i + 1] = nil;

found:
	if(aux)
		aux->refpath = estrdup(branches[i]);
	return QPATH(i, Qbranch|Internal);
}

static void
obj2dir(Dir *d, Object *o, long qdir, vlong mtime)
{
	char name[64];

	snprint(name, sizeof(name), "%H", o->hash);
	d->name = estrdup9p(name);
	d->qid.type = QTDIR;
	d->qid.path = QPATH(o->id, qdir);
	d->atime = mtime;
	d->mtime = mtime;
	d->uid = estrdup9p(username);
	d->gid = estrdup9p(username);
	d->muid = estrdup9p(username);
	d->mode = 0755 | DMDIR;
	if(o->type == GBlob || o->type == GTag){
		d->qid.type = 0;
		d->mode = 0644;
		d->length = o->size;
	}

}

static int
rootgen(int i, Dir *d, void *p)
{
	Gitaux *aux;

	aux = p;
	if (i >= nelem(qroot))
		return -1;
	d->mode = 0555 | DMDIR;
	d->name = estrdup9p(qroot[i]);
	d->qid.vers = 0;
	d->qid.type = strcmp(qroot[i], "ctl") == 0 ? 0 : QTDIR;
	d->qid.path = QPATH(i, Qroot);
	d->uid = estrdup9p(username);
	d->gid = estrdup9p(username);
	d->muid = estrdup9p(username);
	d->mtime = aux->mtime;
	return 0;
}

static int
branchgen(int i, Dir *d, void *p)
{
	Gitaux *aux;
	Dir *refs;
	int n;

	aux = p;
	refs = nil;
	d->qid.vers = 0;
	d->qid.type = QTDIR;
	d->qid.path = findbranch(nil, aux->refpath);
	d->mode = 0555 | DMDIR;
	d->uid = estrdup9p(username);
	d->gid = estrdup9p(username);
	d->muid = estrdup9p(username);
	d->mtime = aux->mtime;
	d->atime = aux->mtime;
	if((n = slurpdir(aux->refpath, &refs)) < 0)
		return -1;
	if(i < n){
		d->name = estrdup9p(refs[i].name);
		free(refs);
		return 0;
	}else{
		free(refs);
		return -1;
	}
}

/* FIXME: walk to the appropriate submodule.. */
static Object*
modrefobj(Dirent *e)
{
	Object *m;

	m = emalloc(sizeof(Object));
	m->hash = e->h;
	m->type = GTree;
	m->tree = emalloc(sizeof(Tree));
	m->tree->ent = nil;
	m->tree->nent = 0;
	m->flag |= Cloaded|Cparsed;
	m->off = -1;
	ref(m);
	cache(m);
	return m;
}

static int
gtreegen(int i, Dir *d, void *p)
{
	Gitaux *aux;
	Object *o, *e;

	aux = p;
	e = aux->obj;
	if(i >= e->tree->nent)
		return -1;
	if((o = readobject(e->tree->ent[i].h)) == nil)
		if(e->tree->ent[i].modref)
			o = modrefobj(&e->tree->ent[i]);
		else
			die("could not read object %H: %r", e->tree->ent[i].h, e->hash);
	d->qid.vers = 0;
	d->qid.type = o->type == GTree ? QTDIR : 0;
	d->qid.path = QPATH(o->id, aux->qdir);
	d->mode = e->tree->ent[i].mode;
	d->atime = aux->mtime;
	d->mtime = aux->mtime;
	d->uid = estrdup9p(username);
	d->gid = estrdup9p(username);
	d->muid = estrdup9p(username);
	d->name = estrdup9p(e->tree->ent[i].name);
	d->length = o->size;
	return 0;
}

static int
gcommitgen(int i, Dir *d, void *p)
{
	Object *o;

	o = ((Gitaux*)p)->obj;
	d->uid = estrdup9p(username);
	d->gid = estrdup9p(username);
	d->muid = estrdup9p(username);
	d->mode = 0444;
	d->atime = o->commit->ctime;
	d->mtime = o->commit->ctime;
	d->qid.type = 0;
	d->qid.vers = 0;

	switch(i){
	case 0:
		d->mode = 0555 | DMDIR;
		d->name = estrdup9p("tree");
		d->qid.type = QTDIR;
		d->qid.path = QPATH(o->id, Qcommittree);
		break;
	case 1:
		d->name = estrdup9p("parent");
		d->qid.path = QPATH(o->id, Qcommitparent);
		break;
	case 2:
		d->name = estrdup9p("msg");
		d->qid.path = QPATH(o->id, Qcommitmsg);
		break;
	case 3:
		d->name = estrdup9p("hash");
		d->qid.path = QPATH(o->id, Qcommithash);
		break;
	case 4:
		d->name = estrdup9p("author");
		d->qid.path = QPATH(o->id, Qcommitauthor);
		break;
	default:
		return -1;
	}
	return 0;
}


static int
objgen(int i, Dir *d, void *p)
{
	Gitaux *aux;
	Object *o;
	Ols *ols;
	Hash h;

	aux = p;
	if(!aux->ols)
		aux->ols = mkols();
	ols = aux->ols;
	o = nil;
	/* We tried to sent it, but it didn't fit */
	if(aux->olslast && ols->idx == i + 1){
		obj2dir(d, aux->olslast, Qobject, aux->mtime);
		return 0;
	}
	while(ols->idx <= i){
		if(olsnext(ols, &h) == -1)
			return -1;
		if((o = readobject(h)) == nil)
			return -1;
	}
	if(o != nil){
		obj2dir(d, o, Qobject, aux->mtime);
		unref(aux->olslast);
		aux->olslast = ref(o);
		return 0;
	}
	return -1;
}

static void
objread(Req *r, Gitaux *aux)
{
	Object *o;

	o = aux->obj;
	switch(o->type){
	case GBlob:
		readbuf(r, o->data, o->size);
		break;
	case GTag:
		readbuf(r, o->data, o->size);
		break;
	case GTree:
		dirread9p(r, gtreegen, aux);
		break;
	case GCommit:
		dirread9p(r, gcommitgen, aux);
		break;
	default:
		die("invalid object type %d", o->type);
	}
}

static void
readcommitparent(Req *r, Object *o)
{
	char *buf, *p;
	int i, n;

	n = o->commit->nparent * (40 + 2);
	buf = emalloc(n);
	p = buf;
	for (i = 0; i < o->commit->nparent; i++)
		p += sprint(p, "%H\n", o->commit->parent[i]);
	readbuf(r, buf, n);
	free(buf);
}


static void
gitattach(Req *r)
{
	Gitaux *aux;
	Dir *d;

	if((d = dirstat(".git")) == nil)
		sysfatal("git/fs: %r");
	aux = emalloc(sizeof(Gitaux));
	aux->path[0] = (Qid){Qroot, 0, QTDIR};
	aux->opath[0] = nil;
	aux->npath = 1;
	aux->mtime = d->mtime;
	r->ofcall.qid = (Qid){Qroot, 0, QTDIR};
	r->fid->qid = r->ofcall.qid;
	r->fid->aux = aux;
	respond(r, nil);
}

static char *
objwalk1(Qid *q, Gitaux *aux, char *name, vlong qdir)
{
	Object *o, *w;
	char *e;
	int i;

	w = nil;
	e = nil;
	o = aux->obj;
	if(!o)
		return Eexist;
	if(o->type == GTree){
		q->type = 0;
		for(i = 0; i < o->tree->nent; i++){
			if(strcmp(o->tree->ent[i].name, name) != 0)
				continue;
			w = readobject(o->tree->ent[i].h);
			if(!w && o->tree->ent[i].modref)
				w = modrefobj(&o->tree->ent[i]);
			if(!w)
				die("could not read object for %s", name);
			q->type = (w->type == GTree) ? QTDIR : 0;
			q->path = QPATH(w->id, qdir);
			aux->obj = w;
		}
		if(!w)
			e = Eexist;
	}else if(o->type == GCommit){
		q->type = 0;
		aux->mtime = o->commit->mtime;
		assert(qdir == Qcommit || qdir == Qobject || qdir == Qcommittree || qdir == Qhead);
		if(strcmp(name, "msg") == 0)
			q->path = QPATH(o->id, Qcommitmsg);
		else if(strcmp(name, "parent") == 0 && o->commit->nparent != 0)
			q->path = QPATH(o->id, Qcommitparent);
		else if(strcmp(name, "hash") == 0)
			q->path = QPATH(o->id, Qcommithash);
		else if(strcmp(name, "author") == 0)
			q->path = QPATH(o->id, Qcommitauthor);
		else if(strcmp(name, "tree") == 0){
			q->type = QTDIR;
			q->path = QPATH(o->id, Qcommittree);
			aux->obj = readobject(o->commit->tree);
		}
		else
			e = Eexist;
	}else if(o->type == GTag){
		e = "tag walk unimplemented";
	}
	return e;
}

static Object *
readref(char *pathstr)
{
	char buf[128], path[128], *p, *e;
	Hash h;
	int n, f;

	snprint(path, sizeof(path), "%s", pathstr);
	while(1){
		if((f = open(path, OREAD)) == -1)
			return nil;
		if((n = readn(f, buf, sizeof(buf) - 1)) == -1)
			return nil;
		close(f);
		buf[n] = 0;
		if(strncmp(buf, "ref:", 4) !=  0)
			break;

		p = buf + 4;
		while(isspace(*p))
			p++;
		if((e = strchr(p, '\n')) != nil)
			*e = 0;
		snprint(path, sizeof(path), ".git/%s", p);
	}

	if(hparse(&h, buf) == -1){
		print("failed to parse hash %s\n", buf);
		return nil;
	}

	return readobject(h);
}

static char*
gitwalk1(Fid *fid, char *name, Qid *q)
{
	char path[128];
	Gitaux *aux;
	Object *o;
	char *e;
	Dir *d;
	Hash h;

	e = nil;
	aux = fid->aux;
	q->vers = 0;

	if(strcmp(name, "..") == 0){
		if(aux->npath > 1)
			aux->npath--;
		*q = aux->path[aux->npath - 1];
		o = ref(aux->opath[aux->npath - 1]);
		if(aux->obj)
			unref(aux->obj);
		aux->obj = o;
		fid->qid = *q;
		return nil;
	}
	

	switch(QDIR(&fid->qid)){
	case Qroot:
		if(strcmp(name, "HEAD") == 0){
			*q = (Qid){Qhead, 0, QTDIR};
			aux->obj = readref(".git/HEAD");
		}else if(strcmp(name, "object") == 0){
			*q = (Qid){Qobject, 0, QTDIR};
		}else if(strcmp(name, "branch") == 0){
			*q = (Qid){Qbranch, 0, QTDIR};
			aux->refpath = estrdup(".git/refs/");
		}else if(strcmp(name, "ctl") == 0){
			*q = (Qid){Qctl, 0, 0};
		}else{
			e = Eexist;
		}
		break;
	case Qbranch:
		if(strcmp(aux->refpath, ".git/refs/heads") == 0 && strcmp(name, "HEAD") == 0)
			snprint(path, sizeof(path), ".git/HEAD");
		else
			snprint(path, sizeof(path), "%s/%s", aux->refpath, name);
		q->type = QTDIR;
		d = dirstat(path);
		if(d && d->qid.type == QTDIR)
			q->path = QPATH(findbranch(aux, path), Qbranch);
		else if(d && (aux->obj = readref(path)) != nil)
			q->path = QPATH(aux->obj->id, Qcommit);
		else
			e = Eexist;
		free(d);
		break;
	case Qobject:
		if(aux->obj){
			e = objwalk1(q, aux, name, Qobject);
		}else{
			if(hparse(&h, name) == -1)
				return "invalid object name";
			if((aux->obj = readobject(h)) == nil)
				return "could not read object";
			q->path = QPATH(aux->obj->id, Qobject);
			q->type = (aux->obj->type == GBlob) ? 0 : QTDIR;
			q->vers = 0;
		}
		break;
	case Qhead:
		e = objwalk1(q, aux, name, Qhead);
		break;
	case Qcommit:
		e = objwalk1(q, aux, name, Qcommit);
		break;
	case Qcommittree:
		e = objwalk1(q, aux, name, Qcommittree);
		break;
	case Qcommitparent:
	case Qcommitmsg:
	case Qcommitdata:
	case Qcommithash:
	case Qcommitauthor:
	case Qctl:
		return Enodir;
	default:
		die("walk: bad qid %Q", *q);
	}
	if(aux->npath >= Npath)
		e = E2long;
	if(!e && QDIR(q) >= Qmax){
		print("npath: %d\n", aux->npath);
		print("walking to %llx (name: %s)\n", q->path, name);
		print("walking from %llx\n", fid->qid.path);
		print("QDIR=%d\n", QDIR(&fid->qid));
		if(aux->obj)
			print("obj=%O\n", aux->obj);
		abort();
	}

	aux->path[aux->npath] = *q;
	if(aux->obj)
		aux->opath[aux->npath] = ref(aux->obj);
	aux->npath++;
	fid->qid = *q;
	return e;
}

static char*
gitclone(Fid *o, Fid *n)
{
	Gitaux *aux, *oaux;
	int i;

	oaux = o->aux;
	aux = emalloc(sizeof(Gitaux));
	aux->npath = oaux->npath;
	for(i = 0; i < aux->npath; i++){
		aux->path[i] = oaux->path[i];
		aux->opath[i] = oaux->opath[i];
		if(aux->opath[i])
			ref(aux->opath[i]);
	}
	if(oaux->refpath)
		aux->refpath = strdup(oaux->refpath);
	if(oaux->obj)
		aux->obj = ref(oaux->obj);
	aux->qdir = oaux->qdir;
	aux->mtime = oaux->mtime;
	n->aux = aux;
	return nil;
}

static void
gitdestroyfid(Fid *f)
{
	Gitaux *aux;
	int i;

	if((aux = f->aux) == nil)
		return;
	for(i = 0; i < aux->npath; i++)
		unref(aux->opath[i]);
	free(aux->refpath);
	olsfree(aux->ols);
	unref(aux->obj);
	free(aux);
}

static char *
readctl(Req *r)
{
	char data[512], buf[512], *p;
	int fd, n;
	if((fd = open(".git/HEAD", OREAD)) == -1)
		return Erepo;
	/* empty HEAD is invalid */
	if((n = readn(fd, buf, sizeof(buf) - 1)) <= 0)
		return Erepo;
	close(fd);
	p = buf;
	buf[n] = 0;
	if(strstr(p, "ref: ") == buf)
		p += strlen("ref: ");
	if(strstr(p, "refs/") == p)
		p += strlen("refs/");
	snprint(data, sizeof(data), "branch %s", p);
	readstr(r, data);
	return nil;
}

static void
gitread(Req *r)
{
	char buf[64], *e;
	Gitaux *aux;
	Object *o;
	Qid *q;

	q = &r->fid->qid;
	o = nil;
	e = nil;
	if(aux = r->fid->aux)
		o = aux->obj;

	switch(QDIR(q)){
	case Qroot:
		dirread9p(r, rootgen, aux);
		break;
	case Qbranch:
		if(o)
			objread(r, aux);
		else
			dirread9p(r, branchgen, aux);
		break;
	case Qobject:
		if(o)
			objread(r, aux);
		else
			dirread9p(r, objgen, aux);
		break;
	case Qcommitmsg:
		readbuf(r, o->commit->msg, o->commit->nmsg);
		break;
	case Qcommitparent:
		readcommitparent(r, o);
		break;
	case Qcommithash:
		snprint(buf, sizeof(buf), "%H\n", o->hash);
		readstr(r, buf);
		break;
	case Qcommitauthor:
		readstr(r, o->commit->author);
		break;
	case Qctl:
		e = readctl(r);
		break;
	case Qhead:
		/* Empty repositories have no HEAD */
		if(aux->obj == nil)
			r->ofcall.count = 0;
		else
			objread(r, aux);
		break;
	case Qcommit:
	case Qcommittree:
	case Qcommitdata:
		objread(r, aux);
		break;
	default:
		die("read: bad qid %Q", *q);
	}
	respond(r, e);
}

static void
gitstat(Req *r)
{
	Gitaux *aux;
	Qid *q;

	q = &r->fid->qid;
	aux = r->fid->aux;
	r->d.uid = estrdup9p(username);
	r->d.gid = estrdup9p(username);
	r->d.muid = estrdup9p(username);
	r->d.mtime = aux->mtime;
	r->d.atime = r->d.mtime;
	r->d.qid = r->fid->qid;
	r->d.mode = 0755 | DMDIR;
	if(aux->obj){
		obj2dir(&r->d, aux->obj, QDIR(q), aux->mtime);
	} else {
		switch(QDIR(q)){
		case Qroot:
			r->d.name = estrdup9p("/");
			break;
		case Qhead:
			r->d.name = estrdup9p("HEAD");
			break;
		case Qbranch:
			r->d.name = estrdup9p("branch");
			break;
		case Qobject:
			r->d.name = estrdup9p("object");
			break;
		case Qctl:
			r->d.name = estrdup9p("ctl");
			r->d.mode = 0666;
			break;
		case Qcommit:
			r->d.name = smprint("%H", aux->obj->hash);
			break;
		case Qcommitmsg:
			r->d.name = estrdup9p("msg");
			r->d.mode = 0644;
			break;
		case Qcommittree:
			r->d.name = estrdup9p("tree");
			break;
		case Qcommitparent:
			r->d.name = estrdup9p("info");
			r->d.mode = 0644;
			break;
		case Qcommithash:
			r->d.name = estrdup9p("hash");
			r->d.mode = 0644;
			break;
		default:
			die("stat: bad qid %Q", *q);
		}
	}

	respond(r, nil);
}

Srv gitsrv = {
	.attach=gitattach,
	.walk1=gitwalk1,
	.clone=gitclone,
	.read=gitread,
	.stat=gitstat,
	.destroyfid=gitdestroyfid,
};

void
usage(void)
{
	fprint(2, "usage: %s [-d]\n", argv0);
	fprint(2, "\t-d:	debug\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	gitinit();
	ARGBEGIN{
	case 'd':	chatty9p++;	break;
	default:	usage();	break;
	}ARGEND;
	if(argc != 0)
		usage();

	username = getuser();
	branches = emalloc(sizeof(char*));
	branches[0] = nil;
	postmountsrv(&gitsrv, nil, "/mnt/git", MCREATE);
	exits(nil);
}
