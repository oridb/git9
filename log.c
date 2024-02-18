#include <u.h>
#include <libc.h>
#include "git.h"

typedef struct Pfilt Pfilt;
struct Pfilt {
	char	*elt;
	int	show;
	Pfilt	*sub;
	int	nsub;
};

Biobuf	*out;
char	*queryexpr;
char	*commitid;
int	shortlog;
int	msgcount = -1;

Objset	done;
Objq	objq;
Pfilt	*pathfilt;

void
filteradd(Pfilt *pf, char *path)
{
	char *p, *e;
	int i;

	if((e = strchr(path, '/')) != nil)
		p = smprint("%.*s", (int)(e - path), path);
	else
		p = strdup(path);

	while(e != nil && *e == '/')
		e++;
	for(i = 0; i < pf->nsub; i++){
		if(strcmp(pf->sub[i].elt, p) == 0){
			pf->sub[i].show = pf->sub[i].show || (e == nil);
			if(e != nil)
				filteradd(&pf->sub[i], e);
			free(p);
			return;
		}
	}
	pf->sub = earealloc(pf->sub, pf->nsub+1, sizeof(Pfilt));
	pf->sub[pf->nsub].elt = p;
	pf->sub[pf->nsub].show = (e == nil);
	pf->sub[pf->nsub].nsub = 0;
	pf->sub[pf->nsub].sub = nil;
	if(e != nil)
		filteradd(&pf->sub[pf->nsub], e);
	pf->nsub++;
}

Hash
lookup(Pfilt *pf, Object *o)
{
	int i;

	for(i = 0; i < o->tree->nent; i++)
		if(strcmp(o->tree->ent[i].name, pf->elt) == 0)
			return o->tree->ent[i].h;
	return Zhash;
}

int
matchesfilter1(Pfilt *pf, Object *t, Object *pt)
{
	Object *a, *b;
	Hash ha, hb;
	int i, r;

	if(pf->show)
		return 1;
	if(t->type != pt->type)
		return 1;
	if(t->type != GTree)
		return 0;

	for(i = 0; i < pf->nsub; i++){
		ha = lookup(&pf->sub[i], t);
		hb = lookup(&pf->sub[i], pt);
		if(hasheq(&ha, &hb))
			continue;
		if(hasheq(&ha, &Zhash) || hasheq(&hb, &Zhash))
			return 1;
		if((a = readobject(ha)) == nil)
			sysfatal("read %H: %r", ha);
		if((b = readobject(hb)) == nil)
			sysfatal("read %H: %r", hb);
		r = matchesfilter1(&pf->sub[i], a, b);
		unref(a);
		unref(b);
		if(r)
			return 1;
	}
	return 0;
}

int
matchesfilter(Object *o)
{
	Object *t, *p, *pt;
	int i, r;

	assert(o->type == GCommit);
	if(pathfilt == nil)
		return 1;
	if((t = readobject(o->commit->tree)) == nil)
		sysfatal("read %H: %r", o->commit->tree);
	for(i = 0; i < o->commit->nparent; i++){
		if((p = readobject(o->commit->parent[i])) == nil)
			sysfatal("read %H: %r", o->commit->parent[i]);
		if((pt = readobject(p->commit->tree)) == nil)
			sysfatal("read %H: %r", o->commit->tree);
		r = matchesfilter1(pathfilt, t, pt);
		unref(p);
		unref(pt);
		if(r)
			return 1;
	}
	return o->commit->nparent == 0;
}


static char*
nextline(char *p, char *e)
{
	for(; p != e; p++)
		if(*p == '\n')
			break;
	return p;
}

static int
show(Object *o)
{
	Tm tm;
	char *p, *q, *e;

	assert(o->type == GCommit);
	if(shortlog){
		p = o->commit->msg;
		e = p + o->commit->nmsg;
		q = nextline(p, e);
		Bprint(out, "%H ", o->hash);
		Bwrite(out, p, q - p);
		Bputc(out, '\n');
	}else{
		tmtime(&tm, o->commit->mtime, tzload("local"));
		Bprint(out, "Hash:\t%H\n", o->hash);
		Bprint(out, "Author:\t%s\n", o->commit->author);
		if(o->commit->committer != nil
		&& strcmp(o->commit->author, o->commit->committer) != 0)
			Bprint(out, "Committer:\t%s\n", o->commit->committer);
		Bprint(out, "Date:\t%τ\n", tmfmt(&tm, "WW MMM D hh:mm:ss z YYYY"));
		Bprint(out, "\n");
		p = o->commit->msg;
		e = p + o->commit->nmsg;
		for(; p != e; p = q){
			q = nextline(p, e);
			Bputc(out, '\t');
			Bwrite(out, p, q - p);
			Bputc(out, '\n');
			if(q != e)
				q++;
		}
		Bprint(out, "\n");
	}
	Bflush(out);
	return 1;
}

static void
showquery(char *q)
{
	Object *o;
	Hash *h;
	int n, i;

	if((n = resolverefs(&h, q)) == -1)
		sysfatal("resolve: %r");
	for(i = 0; i < n && (msgcount == -1 || msgcount > 0); i++){
		if((o = readobject(h[i])) == nil)
			sysfatal("read %H: %r", h[i]);
		if(matchesfilter(o)){
			show(o);
			if(msgcount != -1)
				msgcount--;
		}
		unref(o);
	}
	exits(nil);
}

static void
showcommits(char *c)
{
	Object *o, *p;
	Qelt e;
	int i;
	Hash h;

	if(c == nil)
		c = "HEAD";
	if(resolveref(&h, c) == -1)
		sysfatal("resolve %s: %r", c);
	if((o = readobject(h)) == nil)
		sysfatal("load %H: %r", h);
	if(o->type != GCommit)
		sysfatal("%s: not a commit", c);
	qinit(&objq);
	osinit(&done);
	qput(&objq, o, 0);
	while(qpop(&objq, &e) && (msgcount == -1 || msgcount > 0)){
		if(matchesfilter(e.o)){
			show(e.o);
			if(msgcount != -1)
				msgcount--;
		}
		for(i = 0; i < e.o->commit->nparent; i++){
			if(oshas(&done, e.o->commit->parent[i]))
				continue;
			if((p = readobject(e.o->commit->parent[i])) == nil)
				sysfatal("load %H: %r", o->commit->parent[i]);
			osadd(&done, p);
			qput(&objq, p, 0);
		}
		unref(e.o);
	}
}

static void
usage(void)
{
	fprint(2, "usage: %s [-s] [-e expr | -c commit] files..\n", argv0);
	exits("usage");
}
	
void
main(int argc, char **argv)
{
	char path[1024], repo[1024], *p, *r;
	int i, nrel, nrepo;

	ARGBEGIN{
	case 'e':
		queryexpr = EARGF(usage());
		break;
	case 'c':
		commitid = EARGF(usage());
		break;
	case 's':
		shortlog++;
		break;
	case 'n':
		msgcount = atoi(EARGF(usage()));
		break;
	default:
		usage();
		break;
	}ARGEND;

	if(findrepo(repo, sizeof(repo), &nrel) == -1)
		sysfatal("find root: %r");
	nrepo = strlen(repo);
	if(argc != 0){
		if(getwd(path, sizeof(path)) == nil)
			sysfatal("getwd: %r");
		if(strncmp(path, repo, nrepo) != 0)
			sysfatal("path shifted??");
		p = path + nrepo;
		pathfilt = emalloc(sizeof(Pfilt));
		for(i = 0; i < argc; i++){
			if(*argv[i] == '/'){
				if(strncmp(argv[i], repo, nrepo) != 0)
					continue;
				r = smprint("./%s", argv[i]+nrepo);
			}else
				r = smprint("./%s/%s", p, argv[i]);
			cleanname(r);
			filteradd(pathfilt, r);
			free(r);
		}
	}
	if(chdir(repo) == -1)
		sysfatal("chdir: %r");

	gitinit();
	tmfmtinstall();
	out = Bfdopen(1, OWRITE);
	if(queryexpr != nil)
		showquery(queryexpr);
	else
		showcommits(commitid);
	Bterm(out);
	exits(nil);
}
