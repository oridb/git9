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
typedef struct Crumb Crumb;

struct Crumb {
	char	*name;
	Object	*obj;
	Qid	qid;
	int	mode;
	vlong	mtime;
};

struct Gitaux {
	int	ncrumb;
	Crumb	*crumb;
	char	*refpath;
	int	qdir;

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

static Crumb*
curcrumb(Gitaux *aux)
{
	return &aux->crumb[aux->ncrumb - 1];
}

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
obj2dir(Dir *d, Object *o, Crumb *c, char *name, long qdir)
{
	d->qid.type = QTDIR;
	d->qid.path = QPATH(o->id, qdir);
	d->atime = c->mtime;
	d->mtime = c->mtime;
	d->mode = c->mode;
	d->name = estrdup9p(name);
	d->uid = estrdup9p(username);
	d->gid = estrdup9p(username);
	d->muid = estrdup9p(username);
	if(o->type == GBlob || o->type == GTag){
		d->qid.type = 0;
		d->mode &= 0777;
		d->length = o->size;
	}

}

static int
rootgen(int i, Dir *d, void *p)
{
	Crumb *c;

	c = curcrumb(p);
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
	d->mtime = c->mtime;
	return 0;
}

static int
branchgen(int i, Dir *d, void *p)
{
	Gitaux *aux;
	Dir *refs;
	Crumb *c;
	int n;

	aux = p;
	c = curcrumb(aux);
	refs = nil;
	d->qid.vers = 0;
	d->qid.type = QTDIR;
	d->qid.path = findbranch(nil, aux->refpath);
	d->mode = 0555 | DMDIR;
	d->uid = estrdup9p(username);
	d->gid = estrdup9p(username);
	d->muid = estrdup9p(username);
	d->mtime = c->mtime;
	d->atime = c->mtime;
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
	Object *o, *e;
	Gitaux *aux;
	Crumb *c;

	aux = p;
	c = curcrumb(aux);
	e = c->obj;
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
	d->atime = c->mtime;
	d->mtime = c->mtime;
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

	o = curcrumb(p)->obj;
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
	Crumb *c;
	char name[64];
	Ols *ols;
	Hash h;

	aux = p;
	c = curcrumb(aux);
	if(!aux->ols)
		aux->ols = mkols();
	ols = aux->ols;
	o = nil;
	/* We tried to sent it, but it didn't fit */
	if(aux->olslast && ols->idx == i + 1){
		snprint(name, sizeof(name), "%H", aux->olslast->hash);
		obj2dir(d, aux->olslast, c, name, Qobject);
		return 0;
	}
	while(ols->idx <= i){
		if(olsnext(ols, &h) == -1)
			return -1;
		if((o = readobject(h)) == nil)
			return -1;
	}
	if(o != nil){
		snprint(name, sizeof(name), "%H", o->hash);
		obj2dir(d, o, c, name, Qobject);
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

	o = curcrumb(aux)->obj;
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
	aux->crumb = emalloc(sizeof(Crumb));
	aux->crumb[0].qid = (Qid){Qroot, 0, QTDIR};
	aux->crumb[0].obj = nil;
	aux->crumb[0].mode = DMDIR | 0555;
	aux->crumb[0].mtime = d->mtime;
	aux->crumb[0].name = estrdup("/");
	aux->ncrumb = 1;
	r->ofcall.qid = (Qid){Qroot, 0, QTDIR};
	r->fid->qid = r->ofcall.qid;
	r->fid->aux = aux;
	respond(r, nil);
}

static char *
objwalk1(Qid *q, Object *o, Crumb *c, char *name, vlong qdir)
{
	Object *w;
	char *e;
	int i;

	w = nil;
	e = nil;
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
			c->mode = o->tree->ent[i].mode;
			c->obj = w;
		}
		if(!w)
			e = Eexist;
	}else if(o->type == GCommit){
		q->type = 0;
		c->mtime = o->commit->mtime;
		c->mode = 0444;
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
			unref(c->obj);
			c->obj = readobject(o->commit->tree);
			c->mode = DMDIR | 0555;
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
	Crumb *c, *o;
	char *e;
	Dir *d;
	Hash h;

	e = nil;
	aux = fid->aux;
	
	q->vers = 0;

	if(strcmp(name, "..") == 0){
		if(aux->ncrumb > 1){
			c = &aux->crumb[aux->ncrumb - 1];
			free(c->name);
			unref(c->obj);
			aux->ncrumb--;
		}
		c = &aux->crumb[aux->ncrumb - 1];
		*q = c->qid;
		fid->qid = *q;
		return nil;
	}
	
	aux->crumb = realloc(aux->crumb, (aux->ncrumb + 1) * sizeof(Crumb));
	c = &aux->crumb[aux->ncrumb];
	o = &aux->crumb[aux->ncrumb - 1];
	memset(c, 0, sizeof(Crumb));
	c->mode = o->mode;
	c->mtime = o->mtime;
	if(o->obj)
		c->obj = ref(o->obj);
	aux->ncrumb++;
	
	switch(QDIR(&fid->qid)){
	case Qroot:
		if(strcmp(name, "HEAD") == 0){
			*q = (Qid){Qhead, 0, QTDIR};
			c->mode = DMDIR | 0555;
			c->obj = readref(".git/HEAD");
		}else if(strcmp(name, "object") == 0){
			*q = (Qid){Qobject, 0, QTDIR};
			c->mode = DMDIR | 0555;
		}else if(strcmp(name, "branch") == 0){
			*q = (Qid){Qbranch, 0, QTDIR};
			aux->refpath = estrdup(".git/refs/");
			c->mode = DMDIR | 0555;
		}else if(strcmp(name, "ctl") == 0){
			*q = (Qid){Qctl, 0, 0};
			c->mode = 0644;
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
		else if(d && (c->obj = readref(path)) != nil)
			q->path = QPATH(c->obj->id, Qcommit);
		else
			e = Eexist;
		free(d);
		break;
	case Qobject:
		if(c->obj){
			e = objwalk1(q, o->obj, c, name, Qobject);
		}else{
			if(hparse(&h, name) == -1)
				return "invalid object name";
			if((c->obj = readobject(h)) == nil)
				return "could not read object";
			c->mode = (c->obj->type == GBlob) ? 0444 : QTDIR | 0555;
			q->path = QPATH(c->obj->id, Qobject);
			q->type = (c->obj->type == GBlob) ? 0 : QTDIR;
			q->vers = 0;
		}
		break;
	case Qhead:
		e = objwalk1(q, o->obj, c, name, Qhead);
		break;
	case Qcommit:
		e = objwalk1(q, o->obj, c, name, Qcommit);
		break;
	case Qcommittree:
		e = objwalk1(q, o->obj, c, name, Qcommittree);
		break;
	case Qcommitparent:
	case Qcommitmsg:
	case Qcommitdata:
	case Qcommithash:
	case Qcommitauthor:
	case Qctl:
		return Enodir;
	default:
		return Egreg;
	}

	c->name = estrdup(name);
	c->qid = *q;
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
	aux->ncrumb = oaux->ncrumb;
	aux->crumb = emalloc(oaux->ncrumb * sizeof(Crumb));
	for(i = 0; i < aux->ncrumb; i++){
		aux->crumb[i] = oaux->crumb[i];
		aux->crumb[i].name = estrdup(oaux->crumb[i].name);
		if(aux->crumb[i].obj)
			aux->crumb[i].obj = ref(oaux->crumb[i].obj);
	}
	if(oaux->refpath)
		aux->refpath = strdup(oaux->refpath);
	aux->qdir = oaux->qdir;
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
	for(i = 0; i < aux->ncrumb; i++){
		if(aux->crumb[i].obj)
			unref(aux->crumb[i].obj);
		free(aux->crumb[i].name);
	}
	olsfree(aux->ols);
	free(aux->refpath);
	free(aux->crumb);
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

	aux = r->fid->aux;
	q = &r->fid->qid;
	o = curcrumb(aux)->obj;
	e = nil;

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
		if(o == nil)
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
		e = Egreg;
	}
	respond(r, e);
}

static void
gitstat(Req *r)
{
	Gitaux *aux;
	Crumb *c;
	Qid *q;

	aux = r->fid->aux;
	q = &r->fid->qid;
	c = curcrumb(aux);
	r->d.uid = estrdup9p(username);
	r->d.gid = estrdup9p(username);
	r->d.muid = estrdup9p(username);
	r->d.qid = r->fid->qid;
	r->d.mtime = c->mtime;
	r->d.atime = c->mtime;
	r->d.mode = c->mode;
	if(c->obj)
		obj2dir(&r->d, c->obj, c, c->name, QDIR(q));
	else
		r->d.name = estrdup9p(c->name);
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
