#include <u.h>
#include <libc.h>
#include <pool.h>
#include <ctype.h>

#include "git.h"

#define Packtmp ".git/objects/pack/recv.pack.tmp"
#define Idxtmp ".git/objects/pack/recv.idx.tmp"

char *pathpfx = "";
int allowwrite;

int
fmtpkt(Conn *c, char *fmt, ...)
{
	char pkt[Pktmax];
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprint(pkt, sizeof(pkt), fmt, ap);
	n = writepkt(c, pkt, n);
	va_end(ap);
	return n;
}

int
showrefs(Conn *c)
{
	int i, ret, nrefs;
	Hash head, *refs;
	char **names;

	ret = -1;
	nrefs = 0;
	if(resolveref(&head, "HEAD") != -1) {
		if(fmtpkt(c, "%H HEAD", head) == -1)
			goto error;
	}
	if((nrefs = listrefs(&refs, &names)) == -1)
		sysfatal("listrefs: %r");
	for(i = 0; i < nrefs; i++)
		if(fmtpkt(c, "%H refs/%s\n", refs[i], names[i]) == -1)
			goto error;
	if(flushpkt(c) == -1)
		goto error;
	ret = 0;
error:
	for(i = 0; i < nrefs; i++)
		free(names[i]);
	free(names);
	free(refs);
	return ret;
}

int
servnegotiate(Conn *c, Hash **head, int *nhead, Hash **tail, int *ntail)
{
	char pkt[Pktmax];
	int n, acked;
	Object *o;
	Hash h;

	if(showrefs(c) == -1)
		return -1;

	*head = nil;
	*tail = nil;
	*nhead = 0;
	*ntail = 0;
	while(1){
		if((n = readpkt(c, pkt, sizeof(pkt))) == -1)
			goto error;
		if(n == 0)
			break;
		if(strncmp(pkt, "want ", 5) != 0){
			fmtpkt(c, "ERR  protocol garble %s\n", pkt);
			goto error;
		}
		if(hparse(&h, &pkt[5]) == -1){
			fmtpkt(c, "ERR  garbled want\n");
			goto error;
		}
		if((o = readobject(h)) == nil){
			fmtpkt(c, "ERR requested nonexistent object");
			goto error;
		}
		unref(o);
		*head = erealloc(*head, (*nhead + 1)*sizeof(Hash));
		(*head)[*nhead] = h;	
		*nhead += 1;
	}

	acked = 0;
	while(1){
		if((n = readpkt(c, pkt, sizeof(pkt))) == -1)
			goto error;
		if(strcmp(pkt, "done") == 0 || strcmp(pkt, "done\n") == 0)
			break;
		if(n == 0){
			if(!acked && fmtpkt(c, "NAK\n") == -1)
					goto error;
		}
		if(strncmp(pkt, "have ", 5) != 0){
			fmtpkt(c, "ERR  protocol garble %s\n", pkt);
			goto error;
		}
		if(hparse(&h, &pkt[5]) == -1){
			fmtpkt(c, "ERR  garbled have\n");
			goto error;
		}
		if((o = readobject(h)) == nil)
			continue;
		if(!acked){
			if(fmtpkt(c, "ACK %H\n", h) == -1)
				goto error;
			acked = 1;
		}
		unref(o);
		*tail = erealloc(*tail, (*ntail + 1)*sizeof(Hash));
		(*tail)[*ntail] = h;	
		*ntail += 1;
	}
	if(!acked && fmtpkt(c, "NAK\n") == -1)
		goto error;
	return 0;
error:
	free(*head);
	free(*tail);
	return -1;
}

int
servpack(Conn *c)
{
	Hash *head, *tail, h;
	Object **obj;
	int nhead, ntail, nobj;

	dprint(1, "negotiating pack\n");
	if(servnegotiate(c, &head, &nhead, &tail, &ntail) == -1)
		sysfatal("negotiate: %r");
	dprint(1, "finding twixt\n");
	if(findtwixt(head, nhead, tail, ntail, &obj, &nobj) == -1)
		sysfatal("twixt: %r");
	fprint(2, "DPRINT!\n");
	dprint(1, "writing pack\n");
	if(nobj > 0 && writepack(c->wfd, obj, nobj, &h) == -1)
		sysfatal("send: %r");
	return 0;
}

int
validref(char *s)
{
	if(strncmp(s, "refs/", 5) != 0)
		return 0;
	for(; *s != '\0'; s++)
		if(!isalnum(*s) && strchr("/-_.", *s) == nil)
			return 0;
	return 1;
}

int
recvnegotiate(Conn *c, Hash **cur, Hash **upd, char ***ref, int *nupd)
{
	char pkt[Pktmax], *sp[4];
	Hash old, new;
	int n, i;

	if(showrefs(c) == -1)
		return -1;
	*cur = nil;
	*upd = nil;
	*ref = nil;
	*nupd = 0;
	while(1){
		if((n = readpkt(c, pkt, sizeof(pkt))) == -1)
			goto error;
		if(n == 0)
			break;
		if(getfields(pkt, sp, nelem(sp), 1, " \t\n\r") != 3){
			fmtpkt(c, "ERR  protocol garble %s\n", pkt);
			goto error;
		}
		if(hparse(&old, sp[0]) == -1){
			fmtpkt(c, "ERR bad old hash %s\n", sp[0]);
			goto error;
		}
		if(hparse(&new, sp[1]) == -1){
			fmtpkt(c, "ERR bad new hash %s\n", sp[1]);
			goto error;
		}
		if(!validref(sp[2])){
			fmtpkt(c, "ERR invalid ref %s\n", sp[2]);
			goto error;
		}
		*cur = erealloc(*cur, (*nupd + 1)*sizeof(Hash));
		*upd = erealloc(*upd, (*nupd + 1)*sizeof(Hash));
		*ref = erealloc(*ref, (*nupd + 1)*sizeof(Hash));
		(*cur)[*nupd] = old;
		(*upd)[*nupd] = new;
		(*ref)[*nupd] = estrdup(sp[2]);
		*nupd += 1;
	}		
	return 0;
error:
	free(*cur);
	free(*upd);
	for(i = 0; i < *nupd; i++)
		free((*ref)[i]);
	free(*ref);
	return -1;
}

int
rename(char *pack, char *idx, Hash h)
{
	char name[128], path[196];
	Dir st;

	nulldir(&st);
	st.name = name;
	snprint(name, sizeof(name), "%H.pack", h);
	snprint(path, sizeof(path), ".git/objects/pack/%s", name);
	if(access(path, AEXIST) == 0)
		fprint(2, "warning, pack %s already pushed\n", name);
	else if(dirwstat(pack, &st) == -1)
		return -1;
	snprint(name, sizeof(name), "%H.idx", h);
	snprint(path, sizeof(path), ".git/objects/pack/%s", name);
	if(access(path, AEXIST) == 0)
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
	if(seek(fd, 0, 0) == -1)
		sysfatal("packfile seek: %r");
	while(n != sz - 20){
		nr = sizeof(buf);
		if(sz - n - 20 < sizeof(buf))
			nr = sz - n - 20;
		r = readn(fd, buf, nr);
		if(r != nr){
			werrstr("short read");
			return -1;
		}
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
updatepack(Conn *c)
{
	char buf[Pktmax];
	int n, pfd, packsz;
	Hash h;

	if((pfd = create(Packtmp, ORDWR, 0644)) == -1)
		return -1;
	packsz = 0;
	while(1){
		n = read(c->rfd, buf, sizeof(buf));
		if(n == 0)
			break;
		if(n == -1 || write(pfd, buf, n) != n)
			return -1;
		packsz += n;
	}
	if(checkhash(pfd, packsz, &h) == -1){
		dprint(1, "hash mismatch\n");
		goto error1;
	}
	if(indexpack(Packtmp, Idxtmp, h) == -1){
		dprint(1, "indexing failed\n");
		goto error1;
	}
	if(rename(Packtmp, Idxtmp, h) == -1){
		dprint(1, "rename failed: %r\n");
		goto error2;
	}
	return 0;

error2://	remove(Idxtmp);
error1://	remove(Packtmp);
	return -1;
}	

int
lockrepo(void)
{
	int fd, i;

	for(i = 0; i < 10; i++) {
		if((fd = create(".git/index9/lock", ORDWR|OEXCL, 0644))!= -1)
			return fd;
		sleep(250);
	}
	return -1;
}

int
updaterefs(Conn *c, Hash *cur, Hash *upd, char **ref, int nupd)
{
	char refpath[512];
	int i, fd, ret, lockfd;
	Hash h;

	ret = -1;
	if((lockfd = lockrepo()) == -1){
		fmtpkt(c, "ERR repo locked\n");
		return -1;
	}
	for(i = 0; i < nupd; i++){
		if(resolveref(&h, ref[i]) == 0 && !hasheq(&h, &cur[i])){
			fmtpkt(c, "ERR old ref changed: %s", ref[i]);
			goto error;
		}
		if(snprint(refpath, sizeof(refpath), ".git/%s", ref[i]) == sizeof(refpath)){
			fmtpkt(c, "ERR ref path too long: %s", ref[i]);
			goto error;
		}
		if((fd = create(refpath, OWRITE, 0644)) == -1){
			fmtpkt(c, "ERR open ref: %r");
			goto error;
		}
		if(fprint(fd, "%H", upd[i]) == -1){
			close(fd);
			fmtpkt(c, "ERR upate ref: %r");
			goto error;
		}
		close(fd);
	}
		
	ret = 0;
error:
	close(lockfd);
	remove(".git/index9/lock");
	return ret;
}

int
recvpack(Conn *c)
{
	Hash *cur, *upd;
	char **ref;
	int nupd;

	if(recvnegotiate(c, &cur, &upd, &ref, &nupd) == -1)
		sysfatal("negotiate refs: %r");
	if(nupd != 0 && updatepack(c) == 0)
		sysfatal("update pack: %r");
	if(nupd != 0 && updaterefs(c, cur, upd, ref, nupd) == -1)
		sysfatal("update refs: %r");
	return 0;
}

char*
parsecmd(char *buf, char *cmd, int ncmd)
{
	int i;
	char *p;

	for(p = buf, i = 0; *p && i < ncmd - 1; i++, p++){
		if(*p == ' ' || *p == '\t'){
			cmd[i] = 0;
			break;
		}
		cmd[i] = *p;
	}
	while(*p == ' ' || *p == '\t')
		p++;
	return p;
}

void
usage(void)
{
	fprint(2, "usage: %s [-dw]\n", argv0);
	exits("usage");
}

void
main(int argc, char **argv)
{
	char *p, cmd[32], buf[512], path[512];
	Conn c;

	ARGBEGIN{
	case 'd':
		debug++;
		chattygit++;
		break;
	case 'r':
		pathpfx = EARGF(usage());
		if(*pathpfx != '/')
			sysfatal("path prefix must begin with '/'");
		break;
	case 'w':
		allowwrite++;
		break;
	default:
		usage();
		break;
	}ARGEND;

	gitinit();
	initconn(&c, 0, 1);

	if(readpkt(&c, buf, sizeof(buf)) == -1)
		sysfatal("readpkt: %r");
	p = parsecmd(buf, cmd, sizeof(cmd));
	if(snprint(path, sizeof(path), "%s/%s", pathpfx, p) == sizeof(path))
		sysfatal("%s: path too long\n", p);
	if(chdir(path) == -1)
		sysfatal("cd %s: %r", p);
	if(access(".git", AREAD) == -1)
		sysfatal("no git repository");
	if(strcmp(cmd, "git-receive-pack") == 0 && allowwrite)
		recvpack(&c);
	else if(strcmp(cmd, "git-upload-pack") == 0)
		servpack(&c);
	else
		sysfatal("unsupported command '%s'", cmd);
	exits(nil);
}
