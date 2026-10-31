/* flipt - DRM page-flip + vblank test client for adp-t7000 (phase 2, step 4). Raw ioctls, no libdrm.
 *
 *   flipt [N]        modeset, then N page flips between two dumb buffers (default 300 = 5 s at 60 Hz),
 *                    each waiting for its flip event; prints flip rate and min/avg/max event interval.
 *                    The screen shows colour bars with a white band moving down: a torn band = not
 *                    vsync-latched, a stutter = missed vblank.
 *   flipt v [N]      N x DRM_IOCTL_WAIT_VBLANK (relative 1): vblank IRQ rate only, no flips
 *
 * Env FLIPT_DEV=/dev/dri/cardN (default card0). Stop Orchard first (it holds the CRTC). */
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fd;
static uint32_t crtc_id, conn_id;
static struct drm_mode_modeinfo mode;
struct buf { uint32_t handle, fb, pitch; uint64_t size; uint32_t *px; int band; } b[2];

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void die(const char *what)
{
	fprintf(stderr, "flipt: %s: %s\n", what, strerror(errno));
	exit(1);
}

static void setup(void)
{
	uint32_t crtcs[4], conns[4], encs[4], fbs[4];
	struct drm_mode_modeinfo modes[16];
	char name[64] = "";
	struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };
	const char *dev = getenv("FLIPT_DEV") ? getenv("FLIPT_DEV") : "/dev/dri/card0";

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		die(dev);
	if (ioctl(fd, DRM_IOCTL_VERSION, &v))
		die("VERSION");
	printf("%s: driver %s\n", dev, name);

	struct drm_mode_card_res res = { 0 };
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs || !res.count_connectors ||
	    res.count_crtcs > 4 || res.count_connectors > 4 || res.count_encoders > 4 || res.count_fbs > 4)
		die("GETRESOURCES");
	res.crtc_id_ptr = (uintptr_t)crtcs, res.connector_id_ptr = (uintptr_t)conns;
	res.encoder_id_ptr = (uintptr_t)encs, res.fb_id_ptr = (uintptr_t)fbs;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		die("GETRESOURCES");
	crtc_id = crtcs[0], conn_id = conns[0];

	struct drm_mode_get_connector c = { .connector_id = conn_id };
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) || !c.count_modes || c.count_modes > 16)
		die("GETCONNECTOR");
	c.modes_ptr = (uintptr_t)modes, c.count_props = c.count_encoders = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c))
		die("GETCONNECTOR");
	mode = modes[0];
	printf("mode %s %ux%u@%u clock %u\n", mode.name, mode.hdisplay, mode.vdisplay, mode.vrefresh, mode.clock);

	for (int i = 0; i < 2; i++) {
		struct drm_mode_create_dumb d = { .width = mode.hdisplay, .height = mode.vdisplay, .bpp = 32 };
		if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d))
			die("CREATE_DUMB");
		struct drm_mode_fb_cmd f = { .width = d.width, .height = d.height, .pitch = d.pitch,
					     .bpp = 32, .depth = 24, .handle = d.handle };
		if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &f))
			die("ADDFB");
		struct drm_mode_map_dumb m = { .handle = d.handle };
		if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m))
			die("MAP_DUMB");
		void *p = mmap(NULL, d.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
		if (p == MAP_FAILED)
			die("mmap");
		b[i] = (struct buf){ d.handle, f.fb_id, d.pitch, d.size, p, -1 };
		printf("buffer %d: fb %u pitch %u size %llu\n", i, f.fb_id, d.pitch, (unsigned long long)d.size);
		static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00,
						  0xff00ff, 0xff0000, 0x0000ff, 0x202020 };
		for (unsigned y = 0; y < mode.vdisplay; y++)
			for (unsigned x = 0; x < mode.hdisplay; x++)
				b[i].px[y * (d.pitch / 4) + x] = bars[(x * 8 / mode.hdisplay + i * 4) % 8];
	}

	struct drm_mode_crtc sc = { .crtc_id = crtc_id, .fb_id = b[0].fb, .set_connectors_ptr = (uintptr_t)&conn_id,
				    .count_connectors = 1, .mode = mode, .mode_valid = 1 };
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc))
		die("SETCRTC");
}

/* move the white band in buffer i to row y (restore the bars where it was) */
static void band(int i, int y)
{
	struct buf *f = &b[i];
	int s = f->pitch / 4, h = 24;
	static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x202020 };
	if (f->band >= 0)
		for (int r = f->band; r < f->band + h && r < mode.vdisplay; r++)
			for (int x = 0; x < mode.hdisplay; x++)
				f->px[r * s + x] = bars[(x * 8 / mode.hdisplay + i * 4) % 8];
	for (int r = y; r < y + h && r < mode.vdisplay; r++)
		for (int x = 0; x < mode.hdisplay; x++)
			f->px[r * s + x] = 0xffffff;
	f->band = y;
}

static int wait_event(double *ts, unsigned *seq)
{
	struct pollfd p = { fd, POLLIN, 0 };
	if (poll(&p, 1, 200) <= 0)
		return -1;
	char ev[1024];
	int n = read(fd, ev, sizeof(ev));
	for (int o = 0; o + (int)sizeof(struct drm_event) <= n;) {
		struct drm_event *e = (void *)(ev + o);
		if (e->type == DRM_EVENT_FLIP_COMPLETE) {
			struct drm_event_vblank *v = (void *)e;
			*ts = v->tv_sec + v->tv_usec / 1e6;
			*seq = v->sequence;
			return 0;
		}
		o += e->length;
	}
	return -1;
}

static void stats(const char *what, double *t, int n, int timeouts)
{
	double mn = 1e9, mx = 0, sum = 0;
	for (int i = 1; i < n; i++) {
		double d = (t[i] - t[i - 1]) * 1000;
		mn = d < mn ? d : mn, mx = d > mx ? d : mx, sum += d;
	}
	if (n > 1)
		printf("%s: %d events, %.1f /s, interval min %.2f avg %.2f max %.2f ms, timeouts %d\n", what, n,
		       (n - 1) / (t[n - 1] - t[0]), mn, sum / (n - 1), mx, timeouts);
	else
		printf("%s: no events (timeouts %d)\n", what, timeouts);
}

int main(int argc, char **argv)
{
	int vbl = argc > 1 && !strcmp(argv[1], "v");
	int n = argc > 1 + vbl ? atoi(argv[1 + vbl]) : 300;
	double *t = calloc(n + 1, sizeof(double));
	int got = 0, timeouts = 0;

	setup();
	if (vbl) {
		for (int i = 0; i < n; i++) {
			union drm_wait_vblank w = { .request = { .type = _DRM_VBLANK_RELATIVE, .sequence = 1 } };
			if (ioctl(fd, DRM_IOCTL_WAIT_VBLANK, &w)) {
				timeouts++;
				continue;
			}
			t[got++] = w.reply.tval_sec + w.reply.tval_usec / 1e6;
		}
		stats("WAIT_VBLANK", t, got, timeouts);
		return 0;
	}

	unsigned seq0 = 0, seq = 0, skipped = 0;
	double t0 = now();
	for (int i = 0; i < n; i++) {
		int back = (i + 1) & 1;
		band(back, (i * 6) % (mode.vdisplay - 24));
		struct drm_mode_crtc_page_flip pf = { .crtc_id = crtc_id, .fb_id = b[back].fb,
						      .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = i };
		if (ioctl(fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf)) {
			fprintf(stderr, "PAGE_FLIP %d: %s\n", i, strerror(errno));
			timeouts++;
			continue;
		}
		double ts;
		unsigned s;
		if (wait_event(&ts, &s)) {
			timeouts++;
			continue;
		}
		if (got && s != seq + 1)
			skipped += s - seq - 1;
		if (!got)
			seq0 = s;
		seq = s;
		t[got++] = ts;
	}
	stats("PAGE_FLIP", t, got, timeouts);
	printf("vblank sequence %u -> %u, %u vblanks skipped, wall %.2f s\n", seq0, seq, skipped, now() - t0);
	return timeouts ? 2 : 0;
}
