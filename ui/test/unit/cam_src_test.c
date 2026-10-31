/* cam_src.c against a pretend /dev/isp0 (open / ioctl / mmap / poll / read / close shimmed for that file only): the
 * preset flags and their fallback. A driver without the flags (EINVAL) gets bare camera numbers from then on — but
 * only until the source is closed: the next cam_src_open asks again (a reloaded driver may have them); an EINVAL that
 * a bare number gets too is not the flags (no downgrade); cam_src_has_presets says which one it is.
 * Usage: cam_src_test */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "isp_uapi.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---------- the pretend driver ---------- */

enum drv { DRV_OLD, DRV_NEW, DRV_NOPRESET };	/* before the flags / with them / no usable preset (all EINVAL) */
static enum drv drv;
static int busy_once;		/* the next START: EBUSY (buffers sized for the other camera) */
#define FD 4242
static int opens, closes, fd_open, started, w, h;
static unsigned starts[16];	/* START arguments since the last log_reset */
static int nstarts;

static void log_reset(void) { nstarts = 0; }

static int m_open(const char *path, int flags, ...)
{
	(void)flags;
	if (strcmp(path, "/dev/isp0") || fd_open) {
		errno = fd_open ? EBUSY : ENOENT;
		return -1;
	}
	opens++, fd_open = 1, started = 0;
	return FD;
}

static int m_close(int fd)
{
	if (fd == FD && fd_open)
		closes++, fd_open = 0, started = 0;
	return 0;
}

static int m_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	if (fd != FD || !fd_open) {
		errno = EBADF;
		return -1;
	}
	if (req == ISP_IOC_INFO) {
		struct isp_info *i = arg;
		memset(i, 0, sizeof(*i));
		i->version = ISP_UAPI_VERSION;
		if (started)
			*i = (struct isp_info){ ISP_UAPI_VERSION, (__u32)w, (__u32)h, (__u32)((w + 63) & ~63), 4,
						(__u32)(((w + 63) & ~63) * h * 3 / 2 + 4095) & ~4095u, 0x237, "pretend" };
		return 0;
	}
	if (req == ISP_IOC_START) {
		const unsigned a = *(const __u32 *)arg, cam = a & ISP_START_CAMERA_MASK, mode = a & ~ISP_START_CAMERA_MASK;
		if (nstarts < 16)
			starts[nstarts++] = a;
		if (drv == DRV_NOPRESET || cam > 1 || (drv == DRV_OLD && mode) ||
		    (mode & ~(unsigned)(ISP_START_PHOTO_VIEW | ISP_START_STILL))) {
			errno = EINVAL;
			return -1;
		}
		if (busy_once || started) {
			busy_once = 0;
			errno = EBUSY;
			return -1;
		}
		started = 1;
		w = mode & ISP_START_STILL ? 1280 : mode & ISP_START_PHOTO_VIEW ? 640 : 320;
		h = w * 3 / 4;
		return 0;
	}
	if (req == ISP_IOC_STOP) {
		started = 0;
		return 0;
	}
	if (req == ISP_IOC_RELEASE)
		return 0;
	errno = ENOTTY;
	return -1;
}

static void *m_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	(void)addr, (void)prot, (void)flags, (void)off;
	if (fd != FD || !started) {
		errno = EINVAL;
		return MAP_FAILED;
	}
	return calloc(1, len);
}

static int m_munmap(void *p, size_t len)
{
	(void)len;
	free(p);
	return 0;
}

static int m_poll(struct pollfd *p, nfds_t n, int timeout)
{
	(void)n, (void)timeout;
	p->revents = started ? POLLIN : 0;
	return started;
}

static unsigned seq;	/* the next frame record's seq */
static unsigned short_seq = ~0u;	/* this one comes short (bytesused too small) */

static ssize_t m_read(int fd, void *buf, size_t n)
{
	if (fd != FD || !started || n != sizeof(struct isp_frame)) {
		errno = EIO;
		return -1;
	}
	struct isp_frame f = { seq % 4, seq, 1, (__u32)(((w + 63) & ~63) * h * 3 / 2), 0 };
	if (f.seq == short_seq)
		f.bytesused = 64;
	seq++;
	memcpy(buf, &f, sizeof(f));
	return sizeof(f);
}

#define open m_open
#define close m_close
#define ioctl m_ioctl
#define mmap m_mmap
#define munmap m_munmap
#define poll m_poll
#define read m_read
#include "cam_src.c"
#undef open
#undef close
#undef ioctl
#undef mmap
#undef munmap
#undef poll
#undef read

/* ---------- tests ---------- */

static void expect_starts(int line, const char *what, int n, const unsigned *want)
{
	bool same = nstarts == n;
	for (int i = 0; same && i < n; i++)
		same = starts[i] == want[i];
	if (!same) {
		fails++;
		fprintf(stderr, "FAIL %s:%d: %s: START args", __FILE__, line, what);
		for (int i = 0; i < nstarts; i++)
			fprintf(stderr, " %#x", starts[i]);
		fprintf(stderr, ", want");
		for (int i = 0; i < n; i++)
			fprintf(stderr, " %#x", want[i]);
		fputc('\n', stderr);
	}
	log_reset();
}
#define STARTS(what, ...) do { const unsigned w_[] = { __VA_ARGS__ }; \
	expect_starts(__LINE__, what, (int)(sizeof(w_) / sizeof(w_[0])), w_); } while (0)

int main(void)
{
	char why[160];
	setenv("ORCHARD_CAMERA", "real", 1);

	/* a driver before the flags: VIEW = EINVAL, then the bare number; from then on bare only (STILL too) */
	drv = DRV_OLD;
	cam_src_t *s = cam_src_open(why, sizeof(why));
	CHECK(s, "open: %s", why);
	if (!s)
		return 1;
	CHECK(cam_src_start(s, 0, CAM_MODE_VIEW, why, sizeof(why)), "old driver VIEW: %s", why);
	STARTS("old driver VIEW", ISP_START_PHOTO_VIEW, 0);
	CHECK(!cam_src_has_presets(s), "old driver: has presets");
	CHECK(cam_src_info(s)->w == 320, "old driver: %d wide", cam_src_info(s)->w);
	cam_src_stop(s);
	CHECK(cam_src_start(s, 0, CAM_MODE_STILL, why, sizeof(why)), "old driver STILL: %s", why);
	STARTS("old driver STILL (same open: no flags again)", 0);
	cam_src_stop(s);
	CHECK(cam_src_start(s, 1, CAM_MODE_VIEW, why, sizeof(why)), "old driver front: %s", why);
	STARTS("old driver front", 1);
	cam_src_close(s);

	/* the driver was reloaded with the flags: the next open asks again (no process-lifetime downgrade) */
	drv = DRV_NEW;
	s = cam_src_open(why, sizeof(why));
	CHECK(s && cam_src_start(s, 0, CAM_MODE_VIEW, why, sizeof(why)), "new driver VIEW: %s", why);
	STARTS("new driver VIEW", ISP_START_PHOTO_VIEW);
	CHECK(cam_src_has_presets(s), "new driver: no presets");
	CHECK(cam_src_info(s)->w == 640, "new driver VIEW: %d wide", cam_src_info(s)->w);
	cam_src_stop(s);
	CHECK(cam_src_start(s, 0, CAM_MODE_STILL, why, sizeof(why)), "new driver STILL: %s", why);
	STARTS("new driver STILL", ISP_START_STILL);
	CHECK(cam_src_info(s)->w == 1280, "new driver STILL: %d wide", cam_src_info(s)->w);
	{
		cam_frame_t f;
		CHECK(cam_src_next(s, &f, 100, why, sizeof(why)) == 1, "frame: %s", why);
		/* newest frame wins (the pretend driver always has more waiting): up to nbufs - 1 = 3 newer ones are
		 * read after the first; a short one among them goes back and never replaces a good one */
		unsigned s0 = seq;
		short_seq = s0 + 3;
		CHECK(cam_src_next(s, &f, 100, why, sizeof(why)) == 1, "drain: %s", why);
		CHECK(f.seq == s0 + 2 && seq == s0 + 4, "drain with a short last one: got seq %u (want %u), read %u",
		      f.seq, s0 + 2, seq - s0);
		short_seq = s0 + 4 + 1;	/* the middle one this time */
		CHECK(cam_src_next(s, &f, 100, why, sizeof(why)) == 1 && f.seq == s0 + 4 + 3, "drain with a short middle one: seq %u",
		      f.seq);
		short_seq = ~0u;
	}
	cam_src_stop(s);
	CHECK(cam_src_start(s, 1, CAM_MODE_VIEW, why, sizeof(why)), "new driver front: %s", why);
	STARTS("new driver front", 1 | ISP_START_PHOTO_VIEW);
	cam_src_close(s);

	/* an old driver again after that (e.g. the old .ko loaded back): probed afresh, downgraded again */
	drv = DRV_OLD;
	s = cam_src_open(why, sizeof(why));
	CHECK(s && cam_src_start(s, 0, CAM_MODE_VIEW, why, sizeof(why)), "old again: %s", why);
	STARTS("old driver again", ISP_START_PHOTO_VIEW, 0);
	CHECK(!cam_src_has_presets(s), "old driver again: has presets");
	cam_src_close(s);

	/* EINVAL for the bare number too (no usable preset): not the flags — no downgrade, the error is reported */
	drv = DRV_NOPRESET;
	s = cam_src_open(why, sizeof(why));
	CHECK(s && !cam_src_start(s, 0, CAM_MODE_VIEW, why, sizeof(why)), "no preset: START succeeded");
	CHECK(strstr(why, "Invalid argument"), "no preset: why = %s", why);
	STARTS("no preset", ISP_START_PHOTO_VIEW, 0);
	CHECK(cam_src_has_presets(s), "no preset: downgraded on an EINVAL that wasn't the flags");
	CHECK(!cam_src_start(s, 0, CAM_MODE_STILL, why, sizeof(why)), "no preset STILL: START succeeded");
	STARTS("no preset, again: the flags are still tried", ISP_START_STILL, 0);
	drv = DRV_NEW;	/* (the cause went away: the flags work on the same open) */
	CHECK(cam_src_start(s, 0, CAM_MODE_VIEW, why, sizeof(why)) && cam_src_has_presets(s), "after no preset: %s", why);
	STARTS("after no preset", ISP_START_PHOTO_VIEW);
	cam_src_close(s);

	/* old driver whose bare START is EBUSY (buffers for the other camera): the flags were the EINVAL all the
	 * same, then close + reopen + START, still bare */
	drv = DRV_OLD;
	s = cam_src_open(why, sizeof(why));
	int o0 = opens;
	busy_once = 1;
	CHECK(s && cam_src_start(s, 1, CAM_MODE_VIEW, why, sizeof(why)), "old driver EBUSY: %s", why);
	STARTS("old driver EBUSY", 1 | ISP_START_PHOTO_VIEW, 1, 1);
	CHECK(opens == o0 + 1 && !cam_src_has_presets(s), "old driver EBUSY: %d reopen(s), presets %d", opens - o0,
	      cam_src_has_presets(s));
	cam_src_close(s);
	CHECK(opens == closes && !fd_open, "%d opens, %d closes", opens, closes);

	/* the fake source never has presets (a photo = the next frame) */
	setenv("ORCHARD_CAMERA", "fake", 1);
	s = cam_src_open(why, sizeof(why));
	CHECK(s && cam_src_start(s, 0, CAM_MODE_VIEW, why, sizeof(why)) && !cam_src_has_presets(s), "fake: presets");
	cam_src_close(s);

	if (fails)
		fprintf(stderr, "cam_src_test: %d failure(s)\n", fails);
	else
		printf("cam_src_test ok\n");
	return fails != 0;
}
