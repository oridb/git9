#include <u.h>
#include <libc.h>

#include "git.h"

int chattygit;

int
readpkt(int fd, char *buf, int nbuf)
{
	char len[5];
	char *e;
	int n;

	if(readn(fd, len, 4) == -1)
		return -1;
	len[4] = 0;
	n = strtol(len, &e, 16);
	if(n == 0){
		if(chattygit)
			fprint(2, "readpkt: 0000\n");
		return 0;
	}
	if(e != len + 4 || n <= 4)
		sysfatal("invalid packet line length");
	n  -= 4;
	if(n >= nbuf)
		sysfatal("buffer too small");
	if(readn(fd, buf, n) != n)
		return -1;
	buf[n] = 0;
	if(chattygit)
		fprint(2, "readpkt: %s:\t%.*s\n", len, nbuf, buf);
	return n;
}

int
writepkt(int fd, char *buf, int nbuf)
{
	char len[5];


	snprint(len, sizeof(len), "%04x", nbuf + 4);
	if(write(fd, len, 4) != 4)
		return -1;
	if(write(fd, buf, nbuf) != nbuf)
		return -1;
	if(chattygit){
		fprint(2, "writepkt: %s:\t", len);
		write(2, buf, nbuf);
		write(2, "\n", 1);
	}
	return 0;
}

int
flushpkt(int fd)
{
	if(chattygit)
		fprint(2, "writepkt: 0000\n");
	return write(fd, "0000", 4);
}

static void
grab(char *dst, int n, char *p, char *e)
{
	int l;

	l = e - p;
	if(l >= n)
		sysfatal("overlong component");
	memcpy(dst, p, l);
	dst[l + 1] = 0;

}

int
parseuri(char *uri, char *proto, char *host, char *port, char *path, char *repo)
{
	char *s, *p, *q;
	int n;

	p = strstr(uri, "://");
	if(!p){
		werrstr("missing protocol");
		return -1;
	}
	grab(proto, Nproto, uri, p);
	s = p + 3;

	p = strstr(s, "/");
	if(!p || strlen(p) == 1){
		werrstr("missing path");
		return -1;
	}
	q = memchr(s, ':', p - s);
	if(q){
		grab(host, Nhost, s, q);
		grab(port, Nport, q + 1, p);
	}else{
		grab(host, Nhost, s, p);
		snprint(port, Nport, "9418");
	}
	
	snprint(path, Npath, "%s", p);
	p = strrchr(p, '/') + 1;
	if(!p || strlen(p) == 0){
		werrstr("missing repository in uri");
		return -1;
	}
	n = strlen(p);
	if(hassuffix(p, ".git"))
		n -= 4;
	grab(repo, Nrepo, p, p + n);
	return 0;
}

int
dialssh(char *host, char *, char *path, char *direction)
{
	int pid, pfd[2];
	char cmd[64];

	if(pipe(pfd) == -1)
		sysfatal("unable to open pipe: %r");
	pid = fork();
	if(pid == -1)
		sysfatal("unable to fork");
	if(pid == 0){
		close(pfd[1]);
		dup(pfd[0], 0);
		dup(pfd[0], 1);
		snprint(cmd, sizeof(cmd), "git-%s-pack", direction);
		execl("/bin/ssh", "ssh", host, cmd, path, nil);
	}else{
		close(pfd[0]);
		return pfd[1];
	}
	return -1;
}

int
dialgit(char *host, char *port, char *path, char *direction)
{
	char *ds, cmd[128];
	int fd, l;

	ds = netmkaddr(host, "tcp", port);
	fd = dial(ds, nil, nil, nil);
	if(fd == -1)
		return -1;
	l = snprint(cmd, sizeof(cmd), "git-%s-pack %s\n", direction, path);
	if(writepkt(fd, cmd, l + 1) == -1){
		print("failed to write message\n");
		close(fd);
		return -1;
	}
	return fd;
}
