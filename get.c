#include <u.h>
#include <libc.h>

#include "git.h"

char *fetchbranch;
char *upstream = "origin";
int listonly;

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

	r = -1;
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
	char buf[Pktmax];
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
			fd = create(s, OREAD, DMDIR | 0775);
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

char *
matchcap(char *s, char *cap, int full)
{
	if(strncmp(s, cap, strlen(cap)) == 0)
		if(!full || strlen(s) == strlen(cap))
			return s + strlen(cap);
	return nil;
}

void
handlecaps(char *caps)
{
	char *p, *n, *c, *r;

	for(p = caps; p != nil; p = n){
		n = strchr(p, ' ');
		if(n != nil)
			*n++ = 0;
		if((c = matchcap(p, "symref=", 0)) != nil){
			if((r = strchr(c, ':')) != nil){
				*r++ = '\0';
				print("symref %s %s\n", c, r);
			}
		}
	}
}

void
fail(char *pack, char *idx, char *msg, ...)
{
	char buf[ERRMAX];
	va_list ap;

	va_start(ap, msg);
	snprint(buf, sizeof(buf), msg, ap);
	va_end(ap);

	remove(pack);
	remove(idx);
	fprint(2, "%s", buf);
	exits(buf);
}

int
fetchpack(Conn *c)
{
	char buf[Pktmax], *sp[3], *ep;
	char *packtmp, *idxtmp, **ref;
	Hash h, *have, *want;
	int nref, refsz, first;
	int i, n, l, req, pfd;
	vlong packsz;
	Objset hadobj;
	Object *o;

	nref = 0;
	refsz = 16;
	first = 1;
	have = eamalloc(refsz, sizeof(have[0]));
	want = eamalloc(refsz, sizeof(want[0]));
	ref = eamalloc(refsz, sizeof(ref[0]));
	while(1){
		n = readpkt(c, buf, sizeof(buf));
		if(n == -1)
			return -1;
		if(n == 0)
			break;
		if(strncmp(buf, "ERR ", 4) == 0)
			sysfatal("%s", buf + 4);

		if(first && n > strlen(buf))
			handlecaps(buf + strlen(buf) + 1);
		first = 0;

		getfields(buf, sp, nelem(sp), 1, " \t\n\r");
		if(strstr(sp[1], "^{}"))
			continue;
		if(fetchbranch && !branchmatch(sp[1], fetchbranch))
			continue;
		if(refsz == nref + 1){
			refsz *= 2;
			have = earealloc(have, refsz, sizeof(have[0]));
			want = earealloc(want, refsz, sizeof(want[0]));
			ref = earealloc(ref, refsz, sizeof(ref[0]));
		}
		if(hparse(&want[nref], sp[0]) == -1)
			sysfatal("invalid hash %s", sp[0]);
		if (resolveremote(&have[nref], sp[1]) == -1)
			memset(&have[nref], 0, sizeof(have[nref]));
		ref[nref] = estrdup(sp[1]);
		nref++;
	}
	if(listonly){
		flushpkt(c);
		goto showrefs;
	}

	if(writephase(c) == -1)
		sysfatal("write: %r");
	req = 0;
	for(i = 0; i < nref; i++){
		if(hasheq(&have[i], &want[i]))
			continue;
		if((o = readobject(want[i])) != nil){
			unref(o);
			continue;
		}
		n = snprint(buf, sizeof(buf), "want %H\n", want[i]);
		if(writepkt(c, buf, n) == -1)
			sysfatal("could not send want for %H", want[i]);
		req = 1;
	}
	flushpkt(c);
	osinit(&hadobj);
	for(i = 0; i < nref; i++){
		if(hasheq(&have[i], &Zhash) || oshas(&hadobj, have[i]))
			continue;
		if((o = readobject(have[i])) == nil)
			sysfatal("missing object we should have: %H", have[i]);
		osadd(&hadobj, o);
		unref(o);	
		n = snprint(buf, sizeof(buf), "have %H\n", have[i]);
		if(writepkt(c, buf, n + 1) == -1)
			sysfatal("could not send have for %H", have[i]);
	}
	osclear(&hadobj);
	if(!req)
		flushpkt(c);

	n = snprint(buf, sizeof(buf), "done\n");
	if(writepkt(c, buf, n) == -1)
		sysfatal("write: %r");
	if(!req)
		goto showrefs;
	if(readphase(c) == -1)
		sysfatal("read: %r");
	if((n = readpkt(c, buf, sizeof(buf))) == -1)
		sysfatal("read: %r");
	buf[n] = 0;

	if((packtmp = smprint(".git/objects/pack/fetch.%d.pack", getpid())) == nil)
		sysfatal("smprint: %r");
	if((idxtmp = smprint(".git/objects/pack/fetch.%d.idx", getpid())) == nil)
		sysfatal("smprint: %r");
	if(mkoutpath(packtmp) == -1)
		sysfatal("could not create %s: %r", packtmp);
	if((pfd = create(packtmp, ORDWR, 0664)) == -1)
		sysfatal("could not create %s: %r", packtmp);

	fprint(2, "fetching...\n");
	/*
	 * Work around torvalds git bug: we get duplicate have lines
	 * somtimes, even though the protocol is supposed to start the
	 * pack file immediately.
	 *
	 * Skip ahead until we read 'PACK' off the wire
	 */
	while(1){
		if(readn(c->rfd, buf, 4) != 4)
			sysfatal("fetch packfile: short read");
		buf[4] = 0;
		if(strncmp(buf, "PACK", 4) == 0)
			break;
		l = strtol(buf, &ep, 16);
		if(l == 0 || ep != buf + 4)
			sysfatal("fetch packfile: junk pktline");
		if(readn(c->rfd, buf, l) != l)
			sysfatal("fetch packfile: short read");
	}
	if(write(pfd, "PACK", 4) != 4)
		sysfatal("write pack header: %r");
	packsz = 4;
	while(1){
		n = read(c->rfd, buf, sizeof buf);
		if(n == 0)
			break;
		if(n == -1 || write(pfd, buf, n) != n)
			sysfatal("fetch packfile: %r");
		packsz += n;
	}

	closeconn(c);
	if(seek(pfd, 0, 0) == -1)
		fail(packtmp, idxtmp, "packfile seek: %r");
	if(checkhash(pfd, packsz, &h) == -1)
		fail(packtmp, idxtmp, "corrupt packfile: %r");
	close(pfd);
	if(indexpack(packtmp, idxtmp, h) == -1)
		fail(packtmp, idxtmp, "could not index fetched pack: %r");
	if(rename(packtmp, idxtmp, h) == -1)
		fail(packtmp, idxtmp, "could not rename indexed pack: %r");

showrefs:
	for(i = 0; i < nref; i++){
		print("remote %s %H local %H\n", ref[i], want[i], have[i]);
		free(ref[i]);
	}
	free(ref);
	free(want);
	free(have);
	return 0;
}

void
usage(void)
{
	fprint(2, "usage: %s [-dl] [-b br] [-u upstream] remote\n", argv0);
	fprint(2, "\t-b br:	only fetch matching branch 'br'\n");
	fprint(2, "remote:	fetch from this repository\n");
	exits("usage");
}

void
main(int argc, char **argv)
{
	Conn c;

	ARGBEGIN{
	case 'b':	fetchbranch=EARGF(usage());	break;
	case 'u':	upstream=EARGF(usage());	break;
	case 'd':	chattygit++;			break;
	case 'l':	listonly++;			break;
	default:	usage();			break;
	}ARGEND;

	gitinit();
	if(argc != 1)
		usage();

	if(gitconnect(&c, argv[0], "upload") == -1)
		sysfatal("could not dial %s: %r", argv[0]);
	if(fetchpack(&c) == -1)
		sysfatal("fetch failed: %r");
	closeconn(&c);
	exits(nil);
}
