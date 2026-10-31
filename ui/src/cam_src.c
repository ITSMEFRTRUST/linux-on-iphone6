/* Camera frame sources (cam.h): the A8 ISP's /dev/isp0 (isp_uapi.h, BRIEF-CAMERA-STREAM.md v1) or a fake one
 * that makes animated NV12 test frames in the same buffers-index-release shape, so the whole app path runs
 * without the hardware. ORCHARD_CAMERA = real | fake | none (driver not loaded) | nostream (loaded without
 * stream=1) | fail (START fails) | stall (no frames); default real on the phone, fake in sdl / snap.
 * ORCHARD_CAMERA_SIZE=WxH = the fake sensor's size (default 960x540). All calls block: camera thread only. */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "cam.h"
#include "isp_uapi.h"

#define ISP_DEV "/dev/isp0"
#define ISP_MODULE "/sys/module/apple_isp"
#define FAKE_BUFS 4

struct cam_src {
	bool fake;
	cam_info_t info;
	/* real */
	int fd;
	unsigned stale;		/* frames replaced by a newer one already waiting (real_next) */
	uint8_t *map;
	size_t maplen, bufsize;
	bool no_flags;		/* the driver took a bare camera number but not the preset flags (until closed) */
	/* fake */
	const char *mode;
	uint8_t *bufs;
	bool held[FAKE_BUFS];
	int camera;
	bool running;
	uint32_t seq;
	struct timespec due;
};

static const char *mode(void)
{
	const char *e = getenv("ORCHARD_CAMERA");
	if (e && *e)
		return e;
#ifdef ORCHARD_PHONE
	return "real";
#else
	return "fake";
#endif
}

bool cam_src_fake(void) { return strcmp(mode(), "real") != 0; }

__attribute__((format(printf, 3, 4))) static void say(char *why, size_t n, const char *fmt, ...)
{
	if (!why || !n)
		return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(why, n, fmt, ap);
	va_end(ap);
}

static bool module_loaded(void)
{
	struct stat st;
	return stat(ISP_MODULE, &st) == 0;
}

/* ---------- /dev/isp0 ---------- */

static void real_unmap(cam_src_t *s)
{
	if (s->map)
		munmap(s->map, s->maplen);
	s->map = NULL;
	s->maplen = 0;
}

/* the driver fills INFO (and allows mmap) only once a camera is STARTed: before that only `version` is set.
 * So: open = version check; START = INFO + format check + a fresh mapping; STOP = unmap */
static bool real_map(cam_src_t *s, char *why, size_t n)
{
	struct isp_info ii;
	memset(&ii, 0, sizeof(ii));
	real_unmap(s);
	if (ioctl(s->fd, ISP_IOC_INFO, &ii) < 0) {
		say(why, n, "The camera driver didn't answer (INFO: %s).", strerror(errno));
		return false;
	}
	size_t need = (size_t)ii.stride * ii.height * 3 / 2;
	if (ii.width < 2 || ii.height < 2 || (ii.width | ii.height) & 1 || ii.stride < ii.width || ii.nbufs < 2 ||
	    ii.nbufs > 64 || ii.bufsize < need) {
		say(why, n, "The camera driver reported an odd frame format (%ux%u, stride %u, %u × %u bytes).",
		    ii.width, ii.height, ii.stride, ii.nbufs, ii.bufsize);
		return false;
	}
	size_t len = (size_t)ii.nbufs * ii.bufsize;
	void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, s->fd, 0);
	if (m == MAP_FAILED) {
		say(why, n, "Can't map the camera's buffers: %s.", strerror(errno));
		return false;
	}
	s->map = m;
	s->maplen = len;
	s->bufsize = ii.bufsize;
	s->info.w = (int)ii.width, s->info.h = (int)ii.height, s->info.stride = (int)ii.stride;
	s->info.nbufs = (int)ii.nbufs;
	ii.sensor_name[sizeof(ii.sensor_name) - 1] = 0;
	snprintf(s->info.sensor, sizeof(s->info.sensor), "%s", ii.sensor_name[0] ? ii.sensor_name : "ISP");
	return true;
}

/* open + version check (open boots the ISP when it is off: 1-2 s, on the camera thread) */
static int real_open_fd(char *why, size_t n)
{
	int fd = open(ISP_DEV, O_RDWR | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0) {
		int e = errno;
		if (e == ENOENT)
			say(why, n, module_loaded() ? "The camera driver is loaded without streaming (apple-isp stream=1)."
						     : "The camera driver isn't loaded (apple-isp.ko).");
		else if (e == EBUSY)
			say(why, n, "The camera is in use by another program.");
		else
			say(why, n, "Can't open " ISP_DEV ": %s.", strerror(e));
		return -1;
	}
	struct isp_info ii;
	memset(&ii, 0, sizeof(ii));
	if (ioctl(fd, ISP_IOC_INFO, &ii) < 0) {
		say(why, n, "The camera driver didn't answer (INFO: %s).", strerror(errno));
		close(fd);
		return -1;
	}
	if (ii.version != ISP_UAPI_VERSION) {
		say(why, n, "The camera driver speaks interface v%u; Orchard knows v1.", ii.version);
		close(fd);
		return -1;
	}
	return fd;
}

static cam_src_t *real_open(char *why, size_t n)
{
	int fd = real_open_fd(why, n);
	if (fd < 0)
		return NULL;
	cam_src_t *s = calloc(1, sizeof(*s));
	if (!s) {
		close(fd);
		return NULL;
	}
	s->fd = fd;
	snprintf(s->info.sensor, sizeof(s->info.sensor), "ISP");
	return s;
}

static int real_next(cam_src_t *s, cam_frame_t *f, int timeout_ms, char *why, size_t n)
{
	struct pollfd p = { .fd = s->fd, .events = POLLIN };
	int r = poll(&p, 1, timeout_ms);
	if (r < 0)
		return errno == EINTR ? 0 : (say(why, n, "Camera poll failed: %s.", strerror(errno)), -1);
	if (r == 0)
		return 0;
	if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		say(why, n, "The camera stopped (driver error).");
		return -1;
	}
	struct isp_frame fr;
	ssize_t got = read(s->fd, &fr, sizeof(fr));
	if (got < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : (say(why, n, "Camera read failed: %s.", strerror(errno)), -1);
	if (got != (ssize_t)sizeof(fr) || fr.index >= (unsigned)s->info.nbufs) {
		say(why, n, "The camera driver sent a broken frame record.");
		return -1;
	}
	if (fr.bytesused && fr.bytesused < (size_t)s->info.stride * s->info.h * 3 / 2) {	/* a short frame: skip it */
		__u32 idx = fr.index;
		ioctl(s->fd, ISP_IOC_RELEASE, &idx);
		return 0;
	}
	/* newest frame wins: the driver queues frames oldest first, so after a late pickup (this thread was held up)
	 * the next read would be the stale one; one already waiting replaces this one, which goes straight back */
	struct pollfd q = { .fd = s->fd, .events = POLLIN };
	struct isp_frame nx;
	for (int k = 1; k < s->info.nbufs && poll(&q, 1, 0) == 1 && (q.revents & POLLIN) &&	/* (at most a queue's worth) */
	     read(s->fd, &nx, sizeof(nx)) == (ssize_t)sizeof(nx) && nx.index < (unsigned)s->info.nbufs; k++) {
		__u32 idx = nx.index;
		if (nx.bytesused && nx.bytesused < (size_t)s->info.stride * s->info.h * 3 / 2) {	/* a short one: */
			ioctl(s->fd, ISP_IOC_RELEASE, &idx);	/* it goes back, the good one stays */
			continue;
		}
		idx = fr.index;
		ioctl(s->fd, ISP_IOC_RELEASE, &idx);
		fr = nx;
		s->stale++;
	}
	f->index = (int)fr.index;
	f->seq = fr.seq;
	f->ts_ns = fr.timestamp_ns;
	f->y = s->map + (size_t)fr.index * s->bufsize;
	f->uv = f->y + (size_t)s->info.stride * s->info.h;
	return 1;
}

/* ---------- the fake sensor ---------- */

/* 5×7 glyphs for the test card's text */
static const struct { char c; uint8_t row[7]; } glyphs[] = {
	{ '0', { 14, 17, 19, 21, 25, 17, 14 } }, { '1', { 4, 12, 4, 4, 4, 4, 14 } },
	{ '2', { 14, 17, 1, 2, 4, 8, 31 } }, { '3', { 31, 2, 4, 2, 1, 17, 14 } },
	{ '4', { 2, 6, 10, 18, 31, 2, 2 } }, { '5', { 31, 16, 30, 1, 1, 17, 14 } },
	{ '6', { 6, 8, 16, 30, 17, 17, 14 } }, { '7', { 31, 1, 2, 4, 8, 8, 8 } },
	{ '8', { 14, 17, 17, 14, 17, 17, 14 } }, { '9', { 14, 17, 17, 15, 1, 2, 12 } },
	{ 'A', { 14, 17, 17, 31, 17, 17, 17 } }, { 'C', { 14, 17, 16, 16, 16, 17, 14 } },
	{ 'E', { 31, 16, 16, 30, 16, 16, 31 } }, { 'F', { 31, 16, 16, 30, 16, 16, 16 } },
	{ 'K', { 17, 18, 20, 24, 20, 18, 17 } }, { 'M', { 17, 27, 21, 21, 17, 17, 17 } },
	{ 'N', { 17, 17, 25, 21, 19, 17, 17 } }, { 'O', { 14, 17, 17, 17, 17, 17, 14 } },
	{ 'R', { 30, 17, 17, 30, 20, 18, 17 } }, { 'T', { 31, 4, 4, 4, 4, 4, 4 } },
	{ '#', { 10, 10, 31, 10, 31, 10, 10 } },
};

/* text at portrait (x, y), sc px per dot, Y = lum, its chroma neutral — written through the orientation the app
 * will use, so it reads upright in the viewfinder (and in saved photos) */
static void fake_text(cam_src_t *s, uint8_t *y, uint8_t *uv, cam_orient_t o, int x, int yy, int sc, const char *t)
{
	const int W = s->info.w, H = s->info.h, S = s->info.stride;
	const int pw = o.rot % 180 ? H : W, ph = o.rot % 180 ? W : H;
	for (; *t; t++, x += 6 * sc) {
		const uint8_t *g = NULL;
		for (unsigned i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++)
			if (glyphs[i].c == *t)
				g = glyphs[i].row;
		if (!g)
			continue;
		for (int r = 0; r < 7 * sc; r++)
			for (int c = 0; c < 5 * sc; c++) {
				int px = x + c, py = yy + r, sx, sy;
				if (!(g[r / sc] >> (4 - c / sc) & 1) || px < 0 || py < 0 || px >= pw || py >= ph)
					continue;
				cam_orient_src_xy(o, W, H, px, py, &sx, &sy);
				y[sy * S + sx] = 235;
				uv[(sy / 2) * S + (sx & ~1)] = 128;
				uv[(sy / 2) * S + (sx & ~1) + 1] = 128;
			}
	}
}

static int tri(int v) { v &= 511; return v < 256 ? v : 511 - v; }

/* an animated test card: diagonal light bands drifting one way, hues drifting the other, the camera's name and a
 * frame counter */
static void fake_frame(cam_src_t *s, uint8_t *y, uint32_t seq)
{
	const int W = s->info.w, H = s->info.h, S = s->info.stride, t = (int)seq;
	uint8_t *uv = y + (size_t)S * H;
	for (int r = 0; r < H; r++) {
		uint8_t *d = y + (size_t)r * S;
		for (int c = 0; c < W; c++)
			d[c] = (uint8_t)(40 + tri((c + r) * 512 / (W + H) * 2 + t * 6) * 170 / 255);
	}
	for (int r = 0; r < H / 2; r++) {
		uint8_t *d = uv + (size_t)r * S;
		for (int c = 0; c < W / 2; c++) {
			d[2 * c] = (uint8_t)(64 + tri(c * 1024 / W - t * 4) / 2);
			d[2 * c + 1] = (uint8_t)(64 + tri(r * 1024 / H + 128 + t * 3) / 2);
		}
	}
	cam_orient_t o = cam_default_orient(W, H, s->camera);
	char line[32];
	fake_text(s, y, uv, o, 40, 180, 8, "FAKE");
	fake_text(s, y, uv, o, 40, 260, 8, "CAMERA");
	fake_text(s, y, uv, o, 40, 360, 5, s->camera ? "FRONT" : "REAR");
	snprintf(line, sizeof(line), "#%u", (unsigned)seq);
	fake_text(s, y, uv, o, 40, 420, 5, line);
}

static cam_src_t *fake_open(char *why, size_t n)
{
	const char *m = mode();
	if (!strcmp(m, "none")) {
		say(why, n, "The camera driver isn't loaded (apple-isp.ko).");
		return NULL;
	}
	if (!strcmp(m, "nostream")) {
		say(why, n, "The camera driver is loaded without streaming (apple-isp stream=1).");
		return NULL;
	}
	cam_src_t *s = calloc(1, sizeof(*s));
	if (!s)
		return NULL;
	int w = 960, h = 540;
	const char *e = getenv("ORCHARD_CAMERA_SIZE");
	if (e && sscanf(e, "%dx%d", &w, &h) == 2 && (w < 16 || h < 16 || w > 4096 || h > 4096))
		w = 960, h = 540;
	s->fake = true;
	s->mode = m;
	s->info.w = w & ~1, s->info.h = h & ~1, s->info.stride = (s->info.w + 63) & ~63, s->info.nbufs = FAKE_BUFS;
	snprintf(s->info.sensor, sizeof(s->info.sensor), "fake %dx%d", s->info.w, s->info.h);
	s->bufsize = ((size_t)s->info.stride * s->info.h * 3 / 2 + 4095) & ~(size_t)4095;
	s->bufs = malloc(s->bufsize * FAKE_BUFS);
	if (!s->bufs) {
		free(s);
		return NULL;
	}
	return s;
}

static void ts_add(struct timespec *t, long ns)
{
	t->tv_nsec += ns;
	while (t->tv_nsec >= 1000000000) {
		t->tv_nsec -= 1000000000;
		t->tv_sec++;
	}
}

static int fake_next(cam_src_t *s, cam_frame_t *f, int timeout_ms, char *why, size_t n)
{
	if (!s->running || !strcmp(s->mode, "stall")) {
		usleep((useconds_t)timeout_ms * 1000);
		return 0;
	}
#ifdef ORCHARD_SNAP
	const long period = 3000000;	/* snap's clock is simulated (16 ms steps ≈ 1.5 ms real): keep up with it */
#else
	const long period = 33333333;	/* 30 fps */
#endif
	struct timespec now, limit;
	clock_gettime(CLOCK_MONOTONIC, &now);
	limit = now;
	ts_add(&limit, (long)timeout_ms * 1000000);
	if (s->due.tv_sec == 0)
		s->due = now;
	if (s->due.tv_sec > limit.tv_sec || (s->due.tv_sec == limit.tv_sec && s->due.tv_nsec > limit.tv_nsec)) {
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &limit, NULL);
		return 0;
	}
	clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &s->due, NULL);
	ts_add(&s->due, period);
	if (s->due.tv_sec < now.tv_sec)	/* fell far behind (debugger): don't burst */
		s->due = now;
	int b = -1;
	for (int i = 0; i < FAKE_BUFS && b < 0; i++)
		if (!s->held[(s->seq + i) % FAKE_BUFS])
			b = (int)((s->seq + i) % FAKE_BUFS);
	if (b < 0) {
		say(why, n, "Every camera buffer is held (not released).");
		return -1;
	}
	s->held[b] = true;
	f->index = b;
	f->seq = s->seq++;
	f->ts_ns = (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
	f->y = s->bufs + (size_t)b * s->bufsize;
	f->uv = f->y + (size_t)s->info.stride * s->info.h;
	fake_frame(s, (uint8_t *)f->y, f->seq);
	return 1;
}

/* ---------- the interface ---------- */

cam_src_t *cam_src_open(char *why, size_t n)
{
	if (why && n)
		*why = 0;
	return cam_src_fake() ? fake_open(why, n) : real_open(why, n);
}

const cam_info_t *cam_src_info(cam_src_t *s) { return &s->info; }

bool cam_src_start(cam_src_t *s, int camera, enum cam_mode mode, char *why, size_t n)
{
	if (s->fake) {
		if (!strcmp(s->mode, "fail")) {
			say(why, n, "The camera didn't start (START: %s).", strerror(EIO));
			return false;
		}
		s->camera = camera;
		s->running = true;
		s->due.tv_sec = 0;
		return true;
	}
	__u32 cam = (__u32)camera | (s->no_flags ? 0 : mode == CAM_MODE_STILL ? ISP_START_STILL : ISP_START_PHOTO_VIEW);
	real_unmap(s);
	int r = ioctl(s->fd, ISP_IOC_START, &cam);
	if (r < 0 && errno == EINVAL && !s->no_flags) {	/* a driver before the preset flags? */
		cam = (__u32)camera;
		r = ioctl(s->fd, ISP_IOC_START, &cam);
		/* the bare number got past the argument check: it was the flags. Only for this open (the next
		 * cam_src_open asks again: a reloaded driver may have them); a bare EINVAL too = not the flags */
		if (r == 0 || errno != EINVAL) {
			int e = errno;
			s->no_flags = true;
			fprintf(stderr, "cam: driver without preset flags: streaming its default preset (photo = next frame)\n");
			errno = e;
		}
	}
	if (r < 0 && errno == EBUSY) {
		/* the buffers were sized for the other camera: only a fresh open re-sizes them (the driver re-boots the
		 * ISP on the last close + open, ~1-2 s) */
		fprintf(stderr, "cam: START %s: busy (other camera's buffers), reopening %s\n", camera ? "front" : "rear",
			ISP_DEV);
		close(s->fd);
		s->fd = real_open_fd(why, n);
		if (s->fd < 0)
			return false;
		r = ioctl(s->fd, ISP_IOC_START, &cam);
	}
	if (r < 0) {
		say(why, n, "The %s camera didn't start (START: %s).", camera ? "front" : "rear", strerror(errno));
		return false;
	}
	if (!real_map(s, why, n)) {	/* (this camera's size) */
		ioctl(s->fd, ISP_IOC_STOP);
		return false;
	}
	return true;
}

bool cam_src_has_presets(cam_src_t *s) { return !s->fake && !s->no_flags; }

void cam_src_stop(cam_src_t *s)
{
	if (s->fake) {
		s->running = false;
		memset(s->held, 0, sizeof(s->held));
		return;
	}
	ioctl(s->fd, ISP_IOC_STOP);
	real_unmap(s);	/* (the next START maps its own buffers) */
}

int cam_src_next(cam_src_t *s, cam_frame_t *f, int timeout_ms, char *why, size_t n)
{
	return s->fake ? fake_next(s, f, timeout_ms, why, n) : real_next(s, f, timeout_ms, why, n);
}

void cam_src_release(cam_src_t *s, const cam_frame_t *f)
{
	if (s->fake) {
		if (f->index >= 0 && f->index < FAKE_BUFS)
			s->held[f->index] = false;
		return;
	}
	__u32 idx = (__u32)f->index;
	ioctl(s->fd, ISP_IOC_RELEASE, &idx);
}

int cam_src_focus(cam_src_t *s, int mode, const int win[4], bool ae, int *lens)
{
	*lens = -1;
	if (s->fake)
		return 0;	/* (nothing to focus; the UI still shows its square) */
	struct isp_focus f = { .mode = (__u32)mode, .flags = ae ? ISP_FOCUS_F_AE : 0 };
	if (win)
		f.x = (__u16)win[0], f.y = (__u16)win[1], f.w = (__u16)win[2], f.h = (__u16)win[3];
	if (ioctl(s->fd, ISP_IOC_FOCUS, &f) < 0)
		return errno;
	*lens = f.position;
	return 0;
}

void cam_src_close(cam_src_t *s)
{
	if (!s)
		return;
	if (s->fake) {
		free(s->bufs);
	} else {
		if (s->stale)
			fprintf(stderr, "cam: %u late frame(s) replaced by a newer one\n", s->stale);
		real_unmap(s);
		if (s->fd >= 0)
			close(s->fd);	/* = STOP; the last close powers the sensor down */
	}
	free(s);
}
