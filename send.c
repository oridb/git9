#include <u.h>
#include <libc.h>

#include "git.h"

typedef struct Capset	Capset;

struct Capset {
	int	sideband;
	int	sideband64k;
	int	report;
};

int sendall;
int force;
int nbranch;
char **branch;
char *removed[128];
int nremoved;
int npacked;
int nsent;

int
findref(char **r, int nr, char *ref)
{
	int i;

	for(i = 0; i < nr; i++)
		if(strcmp(r[i], ref) == 0)
			return i;
	return -1;
}

int
readours(Hash **tailp, char ***refp)
{
	int nu, i, idx;
	char *r, *pfx, **ref;
	Hash *tail;

	if(sendall)
		return listrefs(tailp, refp);
	nu = 0;
	tail = eamalloc((nremoved + nbranch), sizeof(Hash));
	ref = eamalloc((nremoved + nbranch), sizeof(char*));
	for(i = 0; i < nbranch; i++){
		ref[nu] = estrdup(branch[i]);
		if(resolveref(&tail[nu], branch[i]) == -1)
			sysfatal("broken branch %s", branch[i]);
		nu++;
	}
	for(i = 0; i < nremoved; i++){
		pfx = "refs/heads/";
		if(strstr(removed[i], "heads/") == removed[i])
			pfx = "refs/";
		if(strstr(removed[i], "refs/heads/") == removed[i])
			pfx = "";
		if((r = smprint("%s%s", pfx, removed[i])) == nil)
			sysfatal("smprint: %r");
		if((idx = findref(ref, nu, r)) == -1)
			idx = nu++;
		assert(idx < nremoved + nbranch);
		memcpy(&tail[idx], &Zhash, sizeof(Hash));
		free(r);
	}
	dprint(1, "nu: %d\n", nu);
	for(i = 0; i < nu; i++)
		dprint(1, "update: %H %s\n", tail[i], ref[i]);
	*tailp = tail;
	*refp = ref;
	return nu;	
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
parsecaps(char *caps, Capset *cs)
{
	char *p, *n;

	for(p = caps; p != nil; p = n){
		n = strchr(p, ' ');
		if(n != nil)
			*n++ = 0;
		if(matchcap(p, "report-status", 1) != nil)
			cs->report = 1;
		if(matchcap(p, "side-band", 1) != nil)
			cs->sideband = 1;
		if(matchcap(p, "side-band-64k", 1) != nil)
			cs->sideband64k = 1;
	}
}

int
sendpack(Conn *c)
{
	int i, n, idx, nupd, nsp, send, first;
	char buf[Pktmax], *sp[3];
	Hash h, *theirs, *ours;
	Object *a, *b, *p;
	char **refs;
	Capset cs;

	first = 1;
	memset(&cs, 0, sizeof(Capset));
	nupd = readours(&ours, &refs);
	theirs = eamalloc(nupd, sizeof(Hash));
	while(1){
		n = readpkt(c, buf, sizeof(buf));
		if(n == -1)
			return -1;
		if(n == 0)
			break;
		if(first && n > strlen(buf))
			parsecaps(buf + strlen(buf) + 1, &cs);
		first = 0;
		if(strncmp(buf, "ERR ", 4) == 0)
			sysfatal("%s", buf + 4);

		if(getfields(buf, sp, nelem(sp), 1, " \t\r\n") != 2)
			sysfatal("invalid ref line %.*s", utfnlen(buf, n), buf);
		if((idx = findref(refs, nupd, sp[1])) == -1)
			continue;
		if(hparse(&theirs[idx], sp[0]) == -1)
			sysfatal("invalid hash %s", sp[0]);
	}

	if(writephase(c) == -1)
		return -1;
	send = 0;
	if(force)
		send=1;
	for(i = 0; i < nupd; i++){
		a = readobject(theirs[i]);
		b = hasheq(&ours[i], &Zhash) ? nil : readobject(ours[i]);
		p = nil;
		if(a != nil && b != nil)
			p = ancestor(a, b);
		if(!force && !hasheq(&theirs[i], &Zhash) && (a == nil || p != a)){
			fprint(2, "remote has diverged\n");
			werrstr("force needed");
			flushpkt(c);
			return -1;
		}
		unref(a);
		unref(b);
		unref(p);
		if(hasheq(&theirs[i], &ours[i])){
			print("uptodate %s\n", refs[i]);
			continue;
		}
		print("update %s %H %H\n", refs[i], theirs[i], ours[i]);
		n = snprint(buf, sizeof(buf), "%H %H %s", theirs[i], ours[i], refs[i]);

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
		if(i == 0 && cs.report){
			buf[n++] = '\0';
			n += snprint(buf + n, sizeof(buf) - n, " report-status");
		}
		if(writepkt(c, buf, n) == -1)
			sysfatal("unable to send update pkt");
		send = 1;
	}
	flushpkt(c);
	if(!send){
		fprint(2, "nothing to send\n");
		return 0;
	}

	if(writepack(c->wfd, ours, nupd, theirs, nupd, &h) == -1)
		return -1;
	if(!cs.report)
		return 0;

	if(readphase(c) == -1)
		return -1;
	/* We asked for a status report, may as well use it. */
	while((n = readpkt(c, buf, sizeof(buf))) > 0){
 		buf[n] = 0;
		if(chattygit)
			fprint(2, "done sending pack, status %s\n", buf);
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
		return -1;
	}
	return 0;
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
	char *br;
	Conn c;

	ARGBEGIN{
	default:
		usage();
		break;
	case 'd':
		chattygit++;
		break;
	case 'f':
		force++;
		break;
	case 'r':
		if(nremoved == nelem(removed))
			sysfatal("too many deleted branches");
		removed[nremoved++] = EARGF(usage());
		break;
	case 'a':
		sendall++;
		break;
	case 'b':
		br = EARGF(usage());
		if(strncmp(br, "refs/heads/", strlen("refs/heads/")) == 0)
			br = smprint("%s", br);
		else if(strncmp(br, "heads/", strlen("heads/")) == 0)
			br = smprint("refs/%s", br);
		else
			br = smprint("refs/heads/%s", br);
		branch = erealloc(branch, (nbranch + 1)*sizeof(char*));
		branch[nbranch] = br;
		nbranch++;
		break;
	}ARGEND;

	gitinit();
	if(argc != 1)
		usage();
	if(gitconnect(&c, argv[0], "receive") == -1)
		sysfatal("git connect: %s: %r", argv[0]);
	if(sendpack(&c) == -1)
		sysfatal("send failed: %r");
	closeconn(&c);
	exits(nil);
}
