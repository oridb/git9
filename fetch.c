#include <u.h>
#include <libc.h>

#include "git.h"

Object *indexed;
char *fetchbranch;
char *upstream = "origin";
char *packtmp = ".git/objects/pack/fetch.tmp";

int
resolveremote(Hash *h, char *ref)
{
	char buf[128], *s;
	int r, f;

	ref = strip(ref);
	if((r = hparse(h, ref)) != -1)
		return r;
	/* Slightly special handling: translate remote refs to local ones. */
	if(strcmp(ref, "HEAD") == 0){
		snprint(buf, sizeof(buf), ".git/HEAD");
	}else if(strstr(ref, "refs/heads") == ref){
		ref += strlen("refs/heads");
		snprint(buf, sizeof(buf), ".git/refs/remotes/%s/%s", upstream, ref);
	}else if(strstr(ref, "refs/tags") == ref){
		ref += strlen("refs/tags");
		snprint(buf, sizeof(buf), ".git/refs/tags/%s/%s", upstream, ref);
	}else{
		return -1;
	}

	s = strip(buf);
	if((f = open(s, OREAD)) == -1)
		return -1;
	if(readn(f, buf, sizeof(buf)) >= 40)
		r = hparse(h, buf);
	close(f);

	if(r == -1 && strstr(buf, "ref:") == buf)
		return resolveremote(h, buf + strlen("ref:"));
	
	return r;
}

int
rename(char *pack, char *idx, Hash h)
{
	char name[128];
	Dir st;

	nulldir(&st);
	st.name = name;
	snprint(name, sizeof(name), "%H.pack", h);
	if(access(name, AEXIST) == 0)
		fprint(2, "warning, pack %s already fetched\n", name);
	else if(dirwstat(pack, &st) == -1)
		return -1;
	snprint(name, sizeof(name), "%H.idx", h);
	if(access(name, AEXIST) == 0)
		fprint(2, "warning, pack %s already indexed\n", name);
	else if(dirwstat(idx, &st) == -1)
		return -1;
	return 0;
}

int
checkhash(int fd, vlong sz, Hash *hcomp)
{
	DigestState *st;
	Hash hexpect;
	char buf[65536];
	vlong n, r;
	int nr;
	
	if(sz < 28){
		werrstr("undersize packfile");
		return -1;
	}

	st = nil;
	n = 0;
	while(n != sz - 20){
		nr = sizeof(buf);
		if(sz - n - 20 < sizeof(buf))
			nr = sz - n - 20;
		r = readn(fd, buf, nr);
		if(r != nr)
			return -1;
		st = sha1((uchar*)buf, nr, nil, st);
		n += r;
	}
	sha1(nil, 0, hcomp->h, st);
	if(readn(fd, hexpect.h, sizeof(hexpect.h)) != sizeof(hexpect.h))
		sysfatal("truncated packfile");
	if(!hasheq(hcomp, &hexpect)){
		werrstr("bad hash: %H != %H", *hcomp, hexpect);
		return -1;
	}
	return 0;
}

int
mkoutpath(char *path)
{
	char s[128];
	char *p;
	int fd;

	snprint(s, sizeof(s), "%s", path);
	for(p=strchr(s+1, '/'); p; p=strchr(p+1, '/')){
		*p = 0;
		if(access(s, AEXIST) != 0){
			fd = create(s, OREAD, DMDIR | 0755);
			if(fd == -1)
				return -1;
			close(fd);
		}		
		*p = '/';
	}
	return 0;
}

int
branchmatch(char *br, char *pat)
{
	char name[128];

	if(strstr(pat, "refs/heads") == pat)
		snprint(name, sizeof(name), "%s", pat);
	else if(strstr(pat, "heads"))
		snprint(name, sizeof(name), "refs/%s", pat);
	else
		snprint(name, sizeof(name), "refs/heads/%s", pat);
	return strcmp(br, name) == 0;
}

int
fetchpack(int fd, int pfd, char *packtmp)
{
	char buf[65536];
	char idxtmp[256];
	char *sp[3];
	Hash h, *have, *want;
	int nref, refsz;
	int i, n, req;
	vlong packsz;

	nref = 0;
	refsz = 16;
	have = emalloc(refsz * sizeof(have[0]));
	want = emalloc(refsz * sizeof(want[0]));
	while(1){
		n = readpkt(fd, buf, sizeof(buf));
		if(n == -1)
			return -1;
		if(n == 0)
			break;
		if(strncmp(buf, "ERR ", 4) == 0)
			sysfatal("%s", buf + 4);
		getfields(buf, sp, nelem(sp), 1, " \t\n\r");
		if(strstr(sp[1], "^{}"))
			continue;
		if(fetchbranch && !branchmatch(sp[1], fetchbranch))
			continue;
		if(refsz == nref + 1){
			refsz *= 2;
			have = erealloc(have, refsz * sizeof(have[0]));
			want = erealloc(want, refsz * sizeof(want[0]));
		}
		if(hparse(&want[nref], sp[0]) == -1)
			sysfatal("invalid hash %s", sp[0]);
		if (resolveremote(&have[nref], sp[1]) == -1)
			memset(&have[nref], 0, sizeof(have[nref]));
		print("remote %s %H local %H\n", sp[1], want[nref], have[nref]);
		nref++;
	}

	req = 0;
	for(i = 0; i < nref; i++){
		if(memcmp(have[i].h, want[i].h, sizeof(have[i].h)) == 0)
			continue;
		n = snprint(buf, sizeof(buf), "want %H", want[i]);
		print("want %H\n", want[i]);
		if(writepkt(fd, buf, n) == -1)
			sysfatal("could not send want for %H", want[i]);
		req = 1; 
	}
	flushpkt(fd);
	for(i = 0; i < nref; i++){
		if(memcmp(have[i].h, Zhash.h, sizeof(Zhash.h)) == 0)
			continue;
		n = snprint(buf, sizeof(buf), "have %H\n", have[i]);
		if(writepkt(fd, buf, n + 1) == -1)
			sysfatal("could not send have for %H", have[i]);
	}
	if(!req){
		fprint(2, "up to date\n");
		flushpkt(fd);
	}
	n = snprint(buf, sizeof(buf), "done\n");
	if(writepkt(fd, buf, n) == -1)
		sysfatal("lost connection write");
	if(!req)
		return 0;

	if((n = readpkt(fd, buf, sizeof(buf))) == -1)
		sysfatal("lost connection read");
	buf[n] = 0;

	fprint(2, "fetching...\n");
	packsz = 0;
	while(1){
		n = readn(fd, buf, sizeof buf);
		if(n == 0)
			break;
		if(n == -1 || write(pfd, buf, n) != n)
			sysfatal("could not fetch packfile: %r");
		packsz += n;
	}
	if(seek(pfd, 0, 0) == -1)
		sysfatal("packfile seek: %r");
	if(checkhash(pfd, packsz, &h) == -1)
		sysfatal("corrupt packfile: %r");
	close(pfd);
	n = strlen(packtmp) - strlen(".tmp");
	memcpy(idxtmp, packtmp, n);
	memcpy(idxtmp + n, ".idx", strlen(".idx") + 1);
	if(indexpack(packtmp, idxtmp, h) == -1)
		sysfatal("could not index fetched pack: %r");
	if(rename(packtmp, idxtmp, h) == -1)
		sysfatal("could not rename indexed pack: %r");
	return 0;
}

void
usage(void)
{
	fprint(2, "usage: %s [-b br] remote\n", argv0);
	fprint(2, "\t-b br:	only fetch matching branch 'br'\n");
	fprint(2, "remote:	fetch from this repository\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	char proto[Nproto], host[Nhost], port[Nport];
	char repo[Nrepo], path[Npath];
	int fd, pfd;

	ARGBEGIN{
	case 'b':	fetchbranch=EARGF(usage());	break;
	case 'u':	upstream=EARGF(usage());	break;
	case 'd':	chattygit++;			break;
	default:	usage();			break;
	}ARGEND;

	gitinit();
	if(argc != 1)
		usage();
	fd = -1;

	if(mkoutpath(packtmp) == -1)
		sysfatal("could not create %s: %r", packtmp);
	if((pfd = create(packtmp, ORDWR, 0644)) == -1)
		sysfatal("could not create %s: %r", packtmp);

	if(parseuri(argv[0], proto, host, port, path, repo) == -1)
		sysfatal("bad uri %s", argv[0]);
	if(strcmp(proto, "ssh") == 0 || strcmp(proto, "git+ssh") == 0)
		fd = dialssh(host, port, path, "upload");
	else if(strcmp(proto, "git") == 0)
		fd = dialgit(host, port, path, "upload");
	else if(strcmp(proto, "http") == 0 || strcmp(proto, "git+http") == 0)
		sysfatal("http clone not implemented");
	else
		sysfatal("unknown protocol %s", proto);
	
	if(fd == -1)
		sysfatal("could not dial %s:%s: %r", proto, host);
	if(fetchpack(fd, pfd, packtmp) == -1)
		sysfatal("fetch failed: %r");
	exits(nil);
}
