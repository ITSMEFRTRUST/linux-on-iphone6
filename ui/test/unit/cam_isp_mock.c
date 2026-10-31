/* A pretend /dev/isp0 for the snap / sdl build (LD_PRELOAD): checks Orchard's side of the v1 contract
 * (BRIEF-CAMERA-STREAM.md) without the phone — ioctl numbers + struct sizes, mmap of nbufs × bufsize, poll + read of
 * struct isp_frame, every buffer RELEASEd, never more than nbufs - 1 held, STOP / close at the end.
 *   cc -shared -fPIC -O1 -Isrc test/unit/cam_isp_mock.c -o /tmp/ispmock.so -ldl -lpthread
 *   LD_PRELOAD=/tmp/ispmock.so ORCHARD_CAMERA=real build-x/snap/orchard-snap open Camera wait 500 shot a.ppm
 * Like the driver (re/CAMERA-STREAM-REVIEW.md S3 / S5): INFO is zeros except `version` and mmap fails (EINVAL) until a
 * camera is STARTed; buffers are sized at the first START after open and a START needing another size (rear 640×480 ↔
 * front 480×360) is EBUSY until the fd is closed (ISPMOCK_SAMESIZE=1: both 640×480); read while stopped = EIO.
 * Preset flags: by default the mock is a driver before them (a flag = EINVAL, as bf66cd142; not a violation, Orchard
 * retries with the bare number); ISPMOCK_FLAGS=1 takes them like 1a3a9e4a1: PHOTO_VIEW = the sizes above, STILL =
 * rear 1280×960 (its own buffer set: never EBUSY), front unchanged. ISPMOCK_UNSTEADY=1: STILL frames alternate dark /
 * bright, so auto-exposure never "settles" (Orchard saves the 60th frame; with the camera turned off meanwhile the
 * first bright one, at most the 40th).
 * ISP_IOC_FOCUS (tap to focus): ENOTTY by default, like a driver before AF (cam_focus.c then stops asking; not a
 * violation); ISPMOCK_FOCUS=1 takes it (logged; the lens is pretend).
 * Frames: NV12 colour bars (white yellow cyan green magenta red blue black, left → right on the sensor) with a white
 * square in the sensor's top-left corner, 30 fps. Summary + any violation on stderr ("ispmock:"). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "isp_uapi.h"

#define NBUFS 4
#define MAXBUF ((1280 * 960 * 3 / 2 + 4095) & ~4095)

static int W, H, STRIDE, BUFSIZE;	/* the started camera's format */
static int alloc_size;		/* buffers sized at the first START since open (0 = none yet) */
static int fd_isp = -1, started, held[NBUFS], nheld, maxheld, frames, releases, bad, reopen_needed;
static unsigned seq, next_buf;
static uint8_t *bufs;
static pthread_t ticker;
static volatile int run;

#define BAD(...) do { fprintf(stderr, "ispmock: VIOLATION: " __VA_ARGS__); fprintf(stderr, "\n"); bad++; } while (0)

static void *tick_main(void *a)
{
	(void)a;
	while (run) {
		usleep(33333);
		uint64_t one = 1;
		if (write(fd_isp, &one, sizeof(one)) < 0)
			break;
	}
	return NULL;
}

static void fill(uint8_t *y, int dy)	/* dy: added to every Y (clamped) */
{
	static const uint8_t bars[8][3] = { { 235, 128, 128 }, { 210, 16, 146 }, { 170, 166, 16 }, { 145, 54, 34 },
					    { 106, 202, 222 }, { 81, 90, 240 }, { 41, 240, 110 }, { 16, 128, 128 } };
	uint8_t *uv = y + STRIDE * H;
	for (int r = 0; r < H; r++)
		for (int c = 0; c < W; c++) {
			const uint8_t *b = bars[c * 8 / W];
			int v = (r < 64 && c < 64 ? 235 : b[0]) + dy;
			y[r * STRIDE + c] = (uint8_t)(v < 16 ? 16 : v > 235 ? 235 : v);
			if (!(r & 1) && !(c & 1)) {
				uv[(r / 2) * STRIDE + c] = r < 64 && c < 64 ? 128 : b[1];
				uv[(r / 2) * STRIDE + c + 1] = r < 64 && c < 64 ? 128 : b[2];
			}
		}
}

int open(const char *path, int flags, ...)
{
	static int (*real)(const char *, int, ...);
	if (!real)
		real = dlsym(RTLD_NEXT, "open");
	va_list ap;
	va_start(ap, flags);
	int mode = flags & O_CREAT ? va_arg(ap, int) : 0;
	va_end(ap);
	if (strcmp(path, "/dev/isp0"))
		return real(path, flags, mode);
	if (fd_isp >= 0) {
		errno = EBUSY;
		return -1;
	}
	fd_isp = eventfd(0, EFD_CLOEXEC | (flags & O_NONBLOCK ? EFD_NONBLOCK : 0));
	if (!bufs)
		bufs = calloc(NBUFS, MAXBUF);
	alloc_size = 0;
	fprintf(stderr, "ispmock: open (flags %#x)\n", flags);
	return fd_isp;
}
int open64(const char *path, int flags, ...) __attribute__((alias("open")));

int ioctl(int fd, unsigned long req, ...)
{
	static int (*real)(int, unsigned long, ...);
	if (!real)
		real = dlsym(RTLD_NEXT, "ioctl");
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (fd != fd_isp || fd < 0)
		return real(fd, req, arg);
	if (req == ISP_IOC_INFO) {
		struct isp_info *i = arg;
		if (sizeof(*i) != 60)
			BAD("struct isp_info is %zu bytes, not 60", sizeof(*i));
		memset(i, 0, sizeof(*i));
		i->version = 1;
		if (started)	/* the format only while streaming */
			*i = (struct isp_info){ 1, W, H, STRIDE, NBUFS, BUFSIZE, 0x8865, "mock-sensor" };
		return 0;
	}
	if (req == ISP_IOC_START) {
		const unsigned a = *(const __u32 *)arg, cam = a & ISP_START_CAMERA_MASK, mode = a & ~ISP_START_CAMERA_MASK;
		if (started)
			BAD("START while started");
		if (cam > 1 || (mode & ~(unsigned)(ISP_START_PHOTO_VIEW | ISP_START_STILL)) ||
		    mode == (ISP_START_PHOTO_VIEW | ISP_START_STILL)) {
			BAD("START %#x", a);
			errno = EINVAL;
			return -1;
		}
		if (mode && !getenv("ISPMOCK_FLAGS")) {
			fprintf(stderr, "ispmock: START %#x: EINVAL (a driver before the preset flags)\n", a);
			errno = EINVAL;
			return -1;
		}
		const bool still = mode == ISP_START_STILL && !cam;
		int w = cam && !getenv("ISPMOCK_SAMESIZE") ? 480 : 640, h = cam && !getenv("ISPMOCK_SAMESIZE") ? 360 : 480;
		if (still)
			w = 1280, h = 960;
		int size = (w * h * 3 / 2 + 4095) & ~4095;
		if (!still && alloc_size && alloc_size != size) {
			fprintf(stderr, "ispmock: START %s: EBUSY (buffers sized for the other camera)\n", cam ? "front" : "rear");
			reopen_needed = 1;
			errno = EBUSY;
			return -1;
		}
		if (!still)
			alloc_size = size;
		W = w, H = h, STRIDE = (w + 63) & ~63, BUFSIZE = (STRIDE * H * 3 / 2 + 4095) & ~4095;
		const bool unsteady = mode == ISP_START_STILL && getenv("ISPMOCK_UNSTEADY");
		for (int k = 0; k < NBUFS; k++)
			fill(bufs + (size_t)k * BUFSIZE, unsteady ? (k & 1 ? 40 : -40) : 0);
		started = 1;
		run = 1;
		pthread_create(&ticker, NULL, tick_main, NULL);
		fprintf(stderr, "ispmock: START %s%s %dx%d\n", cam ? "front" : "rear",
			mode == ISP_START_STILL ? " STILL" : mode ? " PHOTO_VIEW" : "", w, h);
		return 0;
	}
	if (req == ISP_IOC_STOP) {
		if (!started)
			BAD("STOP while stopped");
		started = 0;
		run = 0;
		pthread_join(ticker, NULL);
		fprintf(stderr, "ispmock: STOP after %d frames, %d released, at most %d held\n", frames, releases, maxheld);
		memset(held, 0, sizeof(held));
		nheld = 0;
		return 0;
	}
	if (req == ISP_IOC_FOCUS) {
		const struct isp_focus *fo = arg;
		if (!getenv("ISPMOCK_FOCUS")) {
			fprintf(stderr, "ispmock: FOCUS: ENOTTY (a driver before AF)\n");
			errno = ENOTTY;
			return -1;
		}
		fprintf(stderr, "ispmock: FOCUS mode %u flags %#x window %u,%u %ux%u\n", fo->mode, fo->flags, fo->x, fo->y,
			fo->w, fo->h);
		return 0;
	}
	if (req == ISP_IOC_RELEASE) {
		unsigned idx = *(const __u32 *)arg;
		if (idx >= NBUFS || !held[idx])
			BAD("RELEASE of buffer %u, not held", idx);
		else
			held[idx] = 0, nheld--, releases++;
		return 0;
	}
	BAD("unknown ioctl %#lx (INFO %#lx START %#lx STOP %#lx RELEASE %#lx)", req, (unsigned long)ISP_IOC_INFO,
	    (unsigned long)ISP_IOC_START, (unsigned long)ISP_IOC_STOP, (unsigned long)ISP_IOC_RELEASE);
	errno = ENOTTY;
	return -1;
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	static void *(*real)(void *, size_t, int, int, int, off_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "mmap");
	if (fd != fd_isp || fd < 0)
		return real(addr, len, prot, flags, fd, off);
	if (!started) {
		fprintf(stderr, "ispmock: mmap before START: EINVAL\n");
		errno = EINVAL;
		return MAP_FAILED;
	}
	if (len != (size_t)NBUFS * BUFSIZE || off != 0)
		BAD("mmap of %zu bytes at %ld (want %d)", len, (long)off, NBUFS * BUFSIZE);
	if (prot & PROT_WRITE)
		BAD("mmap with PROT_WRITE (the buffers are read-only)");
	void *p = real(NULL, (size_t)NBUFS * BUFSIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memcpy(p, bufs, (size_t)NBUFS * BUFSIZE);
	mprotect(p, (size_t)NBUFS * BUFSIZE, PROT_READ);
	return p;
}
void *mmap64(void *addr, size_t len, int prot, int flags, int fd, off_t off) __attribute__((alias("mmap")));

ssize_t read(int fd, void *buf, size_t n)
{
	static ssize_t (*real)(int, void *, size_t);
	if (!real)
		real = dlsym(RTLD_NEXT, "read");
	if (fd != fd_isp || fd < 0)
		return real(fd, buf, n);
	if (n != sizeof(struct isp_frame) || sizeof(struct isp_frame) != 24)
		BAD("read of %zu bytes (struct isp_frame = %zu, want 24)", n, sizeof(struct isp_frame));
	uint64_t ticks;
	ssize_t r = real(fd, &ticks, sizeof(ticks));
	if (r < 0)
		return r;
	if (!started) {
		BAD("read while stopped");
		errno = EIO;
		return -1;
	}
	int b = -1;
	for (int i = 0; i < NBUFS && b < 0; i++)
		if (!held[(next_buf + i) % NBUFS])
			b = (int)((next_buf + i) % NBUFS);
	if (b < 0 || nheld + 1 > NBUFS - 1)
		BAD("frame %u: %d buffers held already (max %d)", seq, nheld, NBUFS - 1);
	if (b < 0)
		b = (int)(next_buf % NBUFS);
	next_buf = b + 1;
	held[b] = 1;
	nheld++;
	maxheld = nheld > maxheld ? nheld : maxheld;
	frames++;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	struct isp_frame f = { (unsigned)b, seq++, (uint64_t)t.tv_sec * 1000000000u + t.tv_nsec, STRIDE * H * 3 / 2, 0 };
	memcpy(buf, &f, sizeof(f));
	return sizeof(f);
}

int close(int fd)
{
	static int (*real)(int);
	if (!real)
		real = dlsym(RTLD_NEXT, "close");
	if (fd == fd_isp && fd >= 0) {
		if (started) {	/* close = STOP (the contract) */
			run = 0;
			pthread_join(ticker, NULL);
			started = 0;
		}
		if (reopen_needed)
			fprintf(stderr, "ispmock: closed after EBUSY (reopen path)\n");
		reopen_needed = 0;
		fprintf(stderr, "ispmock: close: %d frames, %d released, at most %d held, %d violations\n", frames, releases,
			maxheld, bad);
		fd_isp = -1;
	}
	return real(fd);
}
