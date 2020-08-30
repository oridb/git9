#include <u.h>
#include <libc.h>
#include <pool.h>

#include "git.h"

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
negotiate(Conn *c, Hash **head, int *nhead, Hash **tail, int *ntail)
{
	char pkt[Pktmax];
	int n, acked;
	Object *o;

	if(showrefs(c) == -1)
		return -1;

	*head = nil;
	*tail = nil;
	*nhead = 0;
	*ntail = 0;
	acked = 0;
	while(1){
		if((n = readpkt(c, pkt, sizeof(pkt))) == -1)
			goto error;
		if(n == 0)
			break;
		if(strncmp(pkt, "want ", 5) == 0){
			*head = erealloc(*head, (*nhead + 1)*sizeof(Hash));
			if(hparse(&(*head)[*nhead], &pkt[5]) == -1){
				fmtpkt(c, "ERR: garbled want\n");
				goto error;
			}
			*nhead += 1;
		}
			
		if(strncmp(pkt, "have ", 5) == 0){
			*tail = erealloc(*tail, (*ntail + 1)*sizeof(Hash));
			if(hparse(&(*tail)[*ntail], &pkt[5]) == -1){
				fmtpkt(c, "ERR: garbled have\n");
				goto error;
			}
			if((o = readobject((*tail)[*ntail])) == nil)
				continue;
			if(!acked)
				if(fmtpkt(c, "ACK %H\r\n", o->hash) == -1)
					goto error;
			unref(o);
			acked = 1;
			*ntail += 1;
		}
	}
	if(!acked)
		fmtpkt(c, "NAK\n");
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

	if(negotiate(c, &head, &nhead, &tail, &ntail) == -1)
		sysfatal("negotiate: %r");
	dprint(1, "finding twixt\n");
	if(findtwixt(head, nhead, tail, ntail, &obj, &nobj) == -1)
		sysfatal("twixt: %r");
	dprint(1, "writing pack\n");
	if(writepack(c->wfd, obj, nobj, &h) == -1)
		sysfatal("send: %r");
	return 0;
}

int
recvpack(Conn *c)
{
	USED(c);
	sysfatal("recvpack: noimpl");
	return -1;
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
	if(strcmp(cmd, "git-fetch-pack") == 0 && allowwrite)
		recvpack(&c);
	else if(strcmp(cmd, "git-upload-pack") == 0)
		servpack(&c);
	else
		sysfatal("unsupported command '%s'", cmd);
	exits(nil);
}
