/* l2p - runs on iOS 12 (checkra1n, root). Creates a preallocated, zero-filled,
 * contiguous-if-possible file and prints its physical extents on the container
 * partition (disk0s1 = Linux asp0n1p1), so Linux can put ext4 in exactly those blocks.
 *   l2p create <path> <MiB>    preallocate + zero-fill + fullfsync, then print extents
 *   l2p map <path>             print extents only
 *   l2p fill <path> <byte>     overwrite the whole file in place with <byte>, fullfsync
 *   l2p lock <path>            chflags schg, falling back to uchg
 *   l2p raw <dev> <off> <len>  copy raw bytes of <dev> (e.g. /dev/rdisk0s1) to stdout, 4 KiB aligned
 * Output lines: "EXT <file_off> <dev_off> <len>" (bytes), then "TOTAL <n_extents> <bytes>".
 * Built with hand-declared prototypes + the SDK's libSystem.tbd link stub (not shipped; see build.sh). */
typedef long long off_t;
typedef unsigned long size_t;
typedef long ssize_t;

int open(const char *, int, ...);
int close(int);
int fcntl(int, int, ...);
int ftruncate(int, off_t);
ssize_t write(int, const void *, size_t);
ssize_t pread(int, void *, size_t, off_t);
ssize_t pwrite(int, const void *, size_t, off_t);
int fstat(int, void *);
int chflags(const char *, unsigned int);
int printf(const char *, ...);
int dprintf(int, const char *, ...);
int *__error(void);
char *strerror(int);
long long strtoll(const char *, char **, int);
int strcmp(const char *, const char *);
void *calloc(size_t, size_t);

#define O_RDONLY    0x0000
#define O_RDWR      0x0002
#define O_CREAT     0x0200
#define O_EXCL      0x0800
#define F_PREALLOCATE   42
#define F_NOCACHE       48
#define F_FULLFSYNC     51
#define F_LOG2PHYS_EXT  65
#define F_ALLOCATECONTIG 0x2
#define F_ALLOCATEALL    0x4
#define F_PEOFPOSMODE    3
#define UF_IMMUTABLE    0x00000002  /* uchg */
#define SF_IMMUTABLE    0x00020000  /* schg */

struct fstore {
	unsigned int fst_flags;
	int fst_posmode;
	off_t fst_offset;
	off_t fst_length;
	off_t fst_bytesalloc;
};

#pragma pack(push, 4)
struct log2phys {
	unsigned int l2p_flags;
	off_t l2p_contigbytes;
	off_t l2p_devoffset;
};
#pragma pack(pop)

_Static_assert(sizeof(struct log2phys) == 20, "log2phys must be packed to 20 bytes");

static int fail(const char *what)
{
	dprintf(2, "l2p: %s: %s\n", what, strerror(*__error()));
	return 1;
}

static int map(int fd, off_t size)
{
	off_t off = 0, total = 0;
	int n = 0;

	while (off < size) {
		struct log2phys l = { 0, size - off, off };

		if (fcntl(fd, F_LOG2PHYS_EXT, &l) < 0)
			return fail("F_LOG2PHYS_EXT");
		if (l.l2p_contigbytes <= 0) {
			dprintf(2, "l2p: zero-length extent at %lld\n", off);
			return 1;
		}
		printf("EXT %lld %lld %lld\n", off, l.l2p_devoffset, l.l2p_contigbytes);
		off += l.l2p_contigbytes;
		total += l.l2p_contigbytes;
		n++;
	}
	printf("TOTAL %d %lld\n", n, total);
	return 0;
}

static off_t file_size(int fd)
{
	/* struct stat on arm64 darwin: st_size at offset 96 */
	unsigned char st[144];

	if (fstat(fd, st) < 0)
		return -1;
	return *(off_t *)(st + 96);
}

int main(int argc, char **argv)
{
	int fd;

	if (argc >= 4 && !strcmp(argv[1], "create")) {
		off_t size = strtoll(argv[3], 0, 10) << 20;
		struct fstore fs = { F_ALLOCATECONTIG | F_ALLOCATEALL, F_PEOFPOSMODE, 0, size, 0 };
		const size_t chunk = 1 << 20;
		char *buf = calloc(1, chunk);
		off_t done;

		if (size <= 0 || !buf)
			return 2;
		fd = open(argv[2], O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd < 0)
			return fail("open");
		if (fcntl(fd, F_PREALLOCATE, &fs) < 0) {
			dprintf(2, "l2p: contiguous prealloc failed (%s), trying non-contiguous\n",
				strerror(*__error()));
			fs.fst_flags = F_ALLOCATEALL;
			if (fcntl(fd, F_PREALLOCATE, &fs) < 0)
				return fail("F_PREALLOCATE");
		}
		printf("PREALLOC %lld\n", fs.fst_bytesalloc);
		if (ftruncate(fd, size) < 0)
			return fail("ftruncate");
		fcntl(fd, F_NOCACHE, 1);
		for (done = 0; done < size; done += chunk)
			if (write(fd, buf, chunk) != (ssize_t)chunk)
				return fail("write");
		if (fcntl(fd, F_FULLFSYNC) < 0)
			return fail("F_FULLFSYNC");
		return map(fd, size);
	}
	if (argc >= 3 && !strcmp(argv[1], "map")) {
		off_t size;

		fd = open(argv[2], O_RDWR);
		if (fd < 0)
			return fail("open");
		size = file_size(fd);
		printf("SIZE %lld\n", size);
		return map(fd, size);
	}
	if (argc >= 4 && !strcmp(argv[1], "fill")) {
		const size_t chunk = 1 << 20;
		char *buf = calloc(1, chunk);
		int v = (int)strtoll(argv[3], 0, 0);
		off_t size, done;
		size_t i;

		if (!buf)
			return 2;
		for (i = 0; i < chunk; i++)
			buf[i] = (char)v;
		fd = open(argv[2], O_RDWR);
		if (fd < 0)
			return fail("open");
		size = file_size(fd);
		fcntl(fd, F_NOCACHE, 1);
		for (done = 0; done < size; done += chunk)
			if (pwrite(fd, buf, chunk, done) != (ssize_t)chunk)
				return fail("pwrite");
		if (fcntl(fd, F_FULLFSYNC) < 0)
			return fail("F_FULLFSYNC");
		return map(fd, size);
	}
	if (argc >= 3 && !strcmp(argv[1], "lock")) {
		if (chflags(argv[2], SF_IMMUTABLE) == 0) {
			printf("schg\n");
			return 0;
		}
		if (chflags(argv[2], UF_IMMUTABLE) < 0)
			return fail("chflags");
		printf("uchg\n");
		return 0;
	}
	if (argc >= 5 && !strcmp(argv[1], "raw")) {
		off_t off = strtoll(argv[3], 0, 10), len = strtoll(argv[4], 0, 10);
		const size_t chunk = 1 << 20;
		char *buf = calloc(1, chunk);

		if ((off | len) & 4095 || len <= 0 || !buf)
			return 2;
		fd = open(argv[2], O_RDONLY);
		if (fd < 0)
			return fail("open");
		while (len > 0) {
			size_t n = len < (off_t)chunk ? (size_t)len : chunk;

			if (pread(fd, buf, n, off) != (ssize_t)n)
				return fail("pread");
			if (write(1, buf, n) != (ssize_t)n)
				return fail("write");
			off += n;
			len -= n;
		}
		return 0;
	}
	dprintf(2, "usage: l2p create <path> <MiB> | map <path> | fill <path> <byte> | lock <path> | raw <dev> <off> <len>\n");
	return 2;
}
