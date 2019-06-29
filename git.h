#include <bio.h>
#include <mp.h>
#include <libsec.h>
#include <flate.h>
#include <regexp.h>

typedef struct Hash	Hash;
typedef struct Cinfo	Cinfo;
typedef struct Tinfo	Tinfo;
typedef struct Object	Object;
typedef struct Objset	Objset;
typedef struct Pack	Pack;
typedef struct Buf	Buf;
typedef struct Dirent	Dirent;
typedef struct Idxent	Idxent;
typedef struct Ols	Ols;

enum {
	/* 5k objects should be enough */
	Cachemax=5*1024,
	Pathmax=512,
	Hashsz=20,

	Nproto	= 16,
	Nport	= 16,
	Nhost	= 256,
	Npath	= 128,
	Nrepo	= 64,
	Nbranch	= 32,
};

typedef enum Type {
	GNone	= 0,
	GCommit	= 1,
	GTree	= 2,
	GBlob	= 3,
	GTag	= 4,
	GOdelta	= 6,
	GRdelta	= 7,
} Type;

enum {
	Cloaded	= 1 << 0,
	Cidx	= 1 << 1,
	Ccache	= 1 << 2,
	Cexist	= 1 << 3,
	Cparsed	= 1 << 5,
};

struct Ols {
	int idx;

	int fd;
	int state;
	int stage;

	Dir *top;
	int ntop;
	int topidx;
	Dir *loose;
	int nloose;
	int looseidx;
	Dir *pack;
	int npack;
	int packidx;
	int nent;
	int entidx;
};

struct Hash {
	uchar h[20];
};

struct Dirent {
	char *name;
	int modref;
	int mode;
	Hash h;
};

struct Object {
	/* Git data */
	Hash	hash;
	Type	type;

	/* Cache */
	int	id;
	int	flag;
	int	refs;
	Object	*next;
	Object	*prev;

	/* For indexing */
	vlong	off;

	/* Everything below here gets cleared */
	char	*all;
	char	*data;
	/* size excludes header */
	vlong	size;

	union {
		Cinfo *commit;
		Tinfo *tree;
	};
};

struct Tinfo {
	/* Tree */
	Dirent	*ent;
	int	nent;
};

struct Cinfo {
	/* Commit */
	Hash	*parent;
	int	nparent;
	Hash	tree;
	char	*author;
	char	*committer;
	char	*msg;
	int	nmsg;
	vlong	ctime;
	vlong	mtime;
};

struct Objset {
	Object	**obj;
	int	nobj;
	int	sz;
};

#define GETBE16(b)\
		((((b)[0] & 0xFFul) <<  8) | \
		 (((b)[1] & 0xFFul) <<  0))

#define GETBE32(b)\
		((((b)[0] & 0xFFul) << 24) | \
		 (((b)[1] & 0xFFul) << 16) | \
		 (((b)[2] & 0xFFul) <<  8) | \
		 (((b)[3] & 0xFFul) <<  0))
#define GETBE64(b)\
		((((b)[0] & 0xFFull) << 56) | \
		 (((b)[1] & 0xFFull) << 48) | \
		 (((b)[2] & 0xFFull) << 40) | \
		 (((b)[3] & 0xFFull) << 32) | \
		 (((b)[4] & 0xFFull) << 24) | \
		 (((b)[5] & 0xFFull) << 16) | \
		 (((b)[6] & 0xFFull) <<  8) | \
		 (((b)[7] & 0xFFull) <<  0))

#define PUTBE16(b, n)\
	do{ \
		(b)[0] = (n) >> 8; \
		(b)[1] = (n) >> 0; \
	} while(0)

#define PUTBE32(b, n)\
	do{ \
		(b)[0] = (n) >> 24; \
		(b)[1] = (n) >> 16; \
		(b)[2] = (n) >> 8; \
		(b)[3] = (n) >> 0; \
	} while(0)

#define PUTBE64(b, n)\
	do{ \
		(b)[0] = (n) >> 56; \
		(b)[1] = (n) >> 48; \
		(b)[2] = (n) >> 40; \
		(b)[3] = (n) >> 32; \
		(b)[4] = (n) >> 24; \
		(b)[5] = (n) >> 16; \
		(b)[6] = (n) >> 8; \
		(b)[7] = (n) >> 0; \
	} while(0)

#define QDIR(qid)	((int)(qid)->path & (0xff))
#define QPATH(id, dt)	(((uvlong)(id) << 8) | ((dt) & 0x7f))
#define isblank(c) \
	(((c) != '\n') && isspace(c))

extern Reprog *authorpat;
extern Objset objcache;
extern Hash Zhash;
extern int chattygit;

#pragma varargck type "H" Hash
#pragma varargck type "T" Type
#pragma varargck type "O" Object*
#pragma varargck type "Q" Qid
int Hfmt(Fmt*);
int Tfmt(Fmt*);
int Ofmt(Fmt*);
int Qfmt(Fmt*);

void gitinit(void);

/* object io */
int	resolverefs(Hash **, char *);
int	resolveref(Hash *, char *);
Object	*readobject(Hash);
void	parseobject(Object *);
int	indexpack(char *, char *, Hash);
int	hasheq(Hash *, Hash *);
Object	*ref(Object *);
void	unref(Object *);
void	cache(Object *);

/* object sets */
void	osinit(Objset *);
void	osadd(Objset *, Object *);
int	oshas(Objset *, Object *);
Object	*osfind(Objset *, Hash);

/* object listing */
Ols	*mkols(void);
int	olsnext(Ols *, Hash *);
void	olsfree(Ols *);

/* util functions */
void	*emalloc(ulong);
void	*erealloc(void *, ulong);
char	*estrdup(char *);
int	slurpdir(char *, Dir **);
int	hparse(Hash *, char *);
int	hassuffix(char *, char *);
int	swapsuffix(char *, int, char *, char *, char *);
char	*strip(char *);
void	die(char *, ...);

/* proto handling */
int	readpkt(int, char*, int);
int	writepkt(int, char*, int);
int	flushpkt(int);
int	parseuri(char *, char *, char *, char *, char *, char *);
int	dialssh(char *, char *, char *, char *);
int	dialgit(char *, char *, char *, char *);
