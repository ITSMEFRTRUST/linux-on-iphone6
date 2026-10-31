/* iPhone backend: simpledrm via raw DRM ioctls (dumb buffer + DIRTYFB damage clips; the fbdev
 * emulation in /dev/fb0 defers every update by 50 ms), evdev multi-touch (type B slots: every
 * finger is reported, the kernel's single-pointer emulation only shows the oldest one),
 * gpio-keys, drv260x haptics. */
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "orchard.h"

#define MAX_DEV 8
#define BITS(n) (((n) + 63) / 64)
#define TEST(b, a) (((a)[(b) / 64] >> ((b) % 64)) & 1)

#define SLOTS 10
static struct dev {
	int fd;
	char sysfs[64];		/* /sys/class/input/eventN/device/ (touch: its "inhibited" switch) */
	bool touch, mt, dropped;
	struct input_absinfo ax, ay;
	int slot, tx, ty;	/* tx/ty/tdown: single-touch fallback */
	bool tdown;
	struct { int id, x, y; } s[SLOTS];
} dev[MAX_DEV];
static int ndev, ff_fd = -1, ff_id = -1;
static void touch_init(void);
static void touch_uninhibit_now(void);

/* /tmp/orchard.cmd: remote control for testing from the PC (bin/orc):
 *   tap X Y | drag X1 Y1 X2 Y2 MS | key home|power | keydown / keyup home|power (one half: real-key timing)
 *   | shot (→ /tmp/orchard.raw, BGRX 750x1334) | state (→ /tmp/orchard.state, for tools/disp/stress.sh) */
#define CMD_FIFO "/tmp/orchard.cmd"
static int cmd_fd = -1;
static struct { bool on; uint32_t t0, ms; int x1, y1, x2, y2; } drag;

static uint32_t tick(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void open_inputs(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	while (d && (e = readdir(d)) && ndev < MAX_DEV) {
		char p[300];
		uint64_t ev[BITS(EV_MAX + 1)] = { 0 };
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		int fd = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
		if (TEST(EV_FF, ev) && ff_fd < 0) {
			ff_fd = fd;
			continue;
		}
		struct dev *v = &dev[ndev++];
		uint64_t abs[BITS(ABS_MAX + 1)] = { 0 };
		v->fd = fd;
		snprintf(v->sysfs, sizeof(v->sysfs), "/sys/class/input/%.16s/device/", e->d_name);
		v->touch = TEST(EV_ABS, ev);
		if (!v->touch)
			continue;
		ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
		v->mt = TEST(ABS_MT_SLOT, abs) && TEST(ABS_MT_POSITION_X, abs) && TEST(ABS_MT_TRACKING_ID, abs);
		ioctl(fd, EVIOCGABS(v->mt ? ABS_MT_POSITION_X : ABS_X), &v->ax);
		ioctl(fd, EVIOCGABS(v->mt ? ABS_MT_POSITION_Y : ABS_Y), &v->ay);
		for (int k = 0; k < SLOTS; k++)
			v->s[k].id = -1;
		if (v->mt) {
			struct input_absinfo sl;
			if (!ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &sl))
				v->slot = sl.value;
		}
	}
	if (d)
		closedir(d);
}

void hal_haptic(int ms)
{
	struct ff_effect fx = {
		.type = FF_RUMBLE,
		.id = ff_id,
		.u.rumble.strong_magnitude = 0xc000,
		.replay.length = ms < 12 ? 12 : ms,
	};
	struct input_event play = { .type = EV_FF, .value = 1 };
	if (ff_fd < 0 || ioctl(ff_fd, EVIOCSFF, &fx) < 0)
		return;
	ff_id = fx.id;
	play.code = ff_id;
	if (write(ff_fd, &play, sizeof(play)) < 0)
		ff_fd = -1;
}

/* ---------- display ----------
 * simpledrm: one DRM dumb buffer, LVGL renders straight into it, DIRTYFB per frame (the kernel copies the
 * damage into vram). adp-t7000 (this project's display-pipe driver, patches/ + tools/disp): LVGL renders into a
 * cached shadow; two dumb buffers in (write-combined) vram take turns on screen: each frame the damage not
 * yet in the back buffer is copied over (this frame's + what the other buffer got since) and the back
 * buffer is page-flipped at vblank — tear-free. A frame that comes while a flip is still pending waits for
 * it (≤ one vblank), so it never draws into the buffer being scanned. Each flip carries a sequence number
 * (user_data) and only its own FLIP_COMPLETE ends the wait, so a late event can't pair with the next flip; a
 * frame whose wait times out (50 ms) is held back (the shadow has it) and presented from hal_run's loop
 * once the event arrives — or after 1 s without one, when the flip is taken as lost.
 * adp-t7000 with one buffer (ORCHARD_NOFLIP=1, or no room for a second): LVGL still renders into the cached
 * shadow and each frame's damage is copied to the vram buffer (never drawing into write-combined memory). */

#define MAX_CLIPS 64
static int drm_fd = -1;
static uint32_t drm_conn;	/* the connector (its DPMS property: hal_display) */
static bool drm_adp;
static uint32_t drm_fb;
static struct drm_clip_rect clips[MAX_CLIPS];
static int nclips;
static lv_draw_buf_t drm_buf;
static struct {
	bool on, pending, deferred;
	bool single;			/* adp, one buffer: shadow → mem[0] copies, no flips */
	int back;			/* the buffer not on screen */
	int busy;			/* PAGE_FLIP refusals in a row */
	uint64_t seq, sent;		/* user_data of the pending flip, when it was queued (us) */
	uint32_t crtc, fb[2], pitch;
	uint8_t *mem[2], *shadow;
	struct drm_clip_rect stale[2][MAX_CLIPS];	/* per buffer: shadow areas it doesn't have yet */
	int nstale[2];			/* > MAX_CLIPS = the whole screen */
} flip;

/* ORCHARD_MOTION=1 in the environment: one log line per burst of animation frames (frame count,
 * fps, render + flush ms, share of the screen redrawn, delay after the input that started it) */
static struct { bool on, active; int n, verbose; uint64_t t_refr, t_first, t_last, t_input, flush, f_sum, f_max, fl_sum, px_sum, drawn; } mo;
static uint64_t frame_px;

static uint64_t us(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000000ull + t.tv_nsec / 1000;
}

static void motion_event(lv_event_t *e)
{
	uint64_t now = us();
	if (lv_event_get_code(e) == LV_EVENT_REFR_START) {
		if (mo.active && now - mo.t_last > 80000) {
			printf("motion: %d frames in %llu ms = %llu fps | frame avg %.1f max %.1f ms (flush %.1f) | "
			       "redrawn %llu%% of the screen | first frame %lld ms after input\n",
			       mo.n, (unsigned long long)(mo.t_last - mo.t_first) / 1000,
			       (unsigned long long)(mo.n * 1000000ull / (mo.t_last - mo.t_first + 1)),
			       mo.f_sum / 1000.0 / mo.n, mo.f_max / 1000.0, mo.fl_sum / 1000.0 / mo.n,
			       (unsigned long long)(mo.px_sum * 100 / mo.n / (SCR_W * SCR_H)),
			       (long long)(mo.t_first - mo.t_input) / 1000);
			fflush(stdout);
			mo.active = false;
		}
		mo.t_refr = now;
		return;
	}
	if (!frame_px)
		return;
	if (!mo.active) {
		mo = (typeof(mo)){ .on = true, .active = true, .verbose = mo.verbose, .drawn = mo.drawn, .t_first = mo.t_refr, .t_refr = mo.t_refr,
				   .t_input = mo.t_input, .flush = mo.flush };
	}
	uint64_t f = now - mo.t_refr;
	mo.drawn += f;
	if (mo.verbose)
		printf("  frame +%llu ms: %.1f ms, %llu%%\n", (unsigned long long)(mo.t_refr - mo.t_first) / 1000, f / 1000.0,
		       (unsigned long long)(frame_px * 100 / (SCR_W * SCR_H)));
	mo.n++;
	mo.f_sum += f;
	mo.f_max = f > mo.f_max ? f : mo.f_max;
	mo.fl_sum += mo.flush;
	mo.px_sum += frame_px;
	mo.t_last = now;
	mo.flush = 0;
	frame_px = 0;
}

static bool clip_in(const struct drm_clip_rect *a, const struct drm_clip_rect *o)	/* a inside o */
{
	return a->x1 >= o->x1 && a->y1 >= o->y1 && a->x2 <= o->x2 && a->y2 <= o->y2;
}

/* a buffer two frames behind gets the last frame's damage and this one's: the same area twice when one picture
 * keeps changing (the camera viewfinder, a video: 3 MB copied twice into vram per frame). An area already listed
 * (or inside one) is copied once */
static void stale_add(int b, const struct drm_clip_rect *r)
{
	for (int i = 0; i < flip.nstale[b] && i < MAX_CLIPS; i++) {
		if (clip_in(r, &flip.stale[b][i]))
			return;
		if (clip_in(&flip.stale[b][i], r)) {
			flip.stale[b][i] = *r;
			return;
		}
	}
	if (flip.nstale[b] < MAX_CLIPS)
		flip.stale[b][flip.nstale[b]] = *r;
	flip.nstale[b]++;
}

static void flip_events(void)	/* read the events waiting on drm_fd (call when it is readable) */
{
	char buf[256];
	ssize_t n = read(drm_fd, buf, sizeof(buf));
	for (ssize_t o = 0; o + (ssize_t)sizeof(struct drm_event) <= n;) {
		struct drm_event *ev = (struct drm_event *)(buf + o);
		if (ev->length < sizeof(*ev) || o + (ssize_t)ev->length > n)
			break;
		if (ev->type == DRM_EVENT_FLIP_COMPLETE && ev->length >= sizeof(struct drm_event_vblank) &&
		    ((struct drm_event_vblank *)ev)->user_data == flip.seq)
			flip.pending = false;	/* ours; an older (timed-out) flip's event is ignored */
		o += ev->length;
	}
}

static bool drm_readable(int ms)
{
	struct pollfd p = { .fd = drm_fd, .events = POLLIN };
	return poll(&p, 1, ms) > 0 && (p.revents & POLLIN);
}

/* the pending flip's FLIP_COMPLETE; false = not within ms (a late copy could draw into the scanned buffer) */
static bool flip_wait(int ms)
{
	while (flip.pending) {
		if (!drm_readable(ms))
			return false;
		flip_events();
	}
	return true;
}

static void flip_copy(uint8_t *dst, const struct drm_clip_rect *r)
{
	int x1 = r->x1 < 0 ? 0 : r->x1, y1 = r->y1 < 0 ? 0 : r->y1;
	int x2 = r->x2 > SCR_W ? SCR_W : r->x2, y2 = r->y2 > SCR_H ? SCR_H : r->y2;
	for (int y = y1; y < y2; y++)
		memcpy(dst + (size_t)y * flip.pitch + x1 * 4, flip.shadow + (size_t)y * flip.pitch + x1 * 4,
		       (size_t)(x2 - x1) * 4);
}

static void flip_present(void)
{
	static const struct drm_clip_rect all = { 0, 0, SCR_W, SCR_H };
	flip.deferred = false;
	while (drm_readable(0))	/* anything still queued (stale completions) */
		flip_events();
	int b = flip.back;
	if (flip.nstale[b] > MAX_CLIPS)
		flip_copy(flip.mem[b], &all);
	else
		for (int i = 0; i < flip.nstale[b]; i++)
			flip_copy(flip.mem[b], &flip.stale[b][i]);
	flip.nstale[b] = 0;
	struct drm_mode_crtc_page_flip pf = { .crtc_id = flip.crtc, .fb_id = flip.fb[b],
					      .flags = DRM_MODE_PAGE_FLIP_EVENT, .user_data = flip.seq + 1 };
	if (ioctl(drm_fd, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0) {
		flip.seq++;
		flip.pending = true;
		flip.sent = us();
		flip.back = !b;
		flip.busy = 0;
	} else {	/* refused (busy / gone): the other buffer gets this frame too, shown at the next flip */
		flip.nstale[!b] = MAX_CLIPS + 1;
		flip.deferred = ++flip.busy < 50;	/* busy: hal_run tries again shortly */
	}
}

/* the pending flip never completed (1 s): take it as lost; which buffer is on screen is unknown */
static void flip_lost(void)
{
	static int nlost;
	if (nlost++ < 5)
		fprintf(stderr, "orchard: page flip %llu: no FLIP_COMPLETE after 1 s\n", (unsigned long long)flip.seq);
	flip.pending = false;
	flip.nstale[0] = flip.nstale[1] = MAX_CLIPS + 1;
}

static void flip_frame(void)
{
	for (int i = 0; i < nclips; i++) {
		stale_add(0, &clips[i]);
		stale_add(1, &clips[i]);
	}
	if (nclips >= MAX_CLIPS) {	/* (hal_damage overflowed: everything) */
		flip.nstale[0] = flip.nstale[1] = MAX_CLIPS + 1;
	}
	nclips = 0;
	if (!flip_wait(50)) {
		if (us() - flip.sent < 1000000) {
			flip.deferred = true;	/* the shadow has it: hal_run presents it when the flip completes */
			return;
		}
		flip_lost();
	}
	flip_present();
}

/* hal_run, every pass: completions that came meanwhile, a held-back frame; → ms the loop may sleep */
static int flip_service(int sleep_ms)
{
	if (!flip.on)
		return sleep_ms;
	while (drm_readable(0))
		flip_events();
	if (!flip.deferred)
		return sleep_ms;
	if (flip.pending && us() - flip.sent >= 1000000)
		flip_lost();
	if (!flip.pending)
		flip_present();
	return flip.deferred && sleep_ms > 8 ? 8 : sleep_ms;
}

static void single_copy(void)	/* adp, one buffer: this frame's damage shadow → vram */
{
	static const struct drm_clip_rect all = { 0, 0, SCR_W, SCR_H };
	if (nclips >= MAX_CLIPS)
		flip_copy(flip.mem[0], &all);
	else
		for (int i = 0; i < nclips; i++)
			flip_copy(flip.mem[0], &clips[i]);
}

static void drm_flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	(void)px;
	frame_px += lv_area_get_size(a);
	if (nclips < MAX_CLIPS)
		clips[nclips++] = (struct drm_clip_rect){ a->x1, a->y1, a->x2 + 1, a->y2 + 1 };
	if (lv_display_flush_is_last(d) && flip.on) {
		uint64_t t = us();
		flip_frame();
		mo.flush += us() - t;
	} else if (lv_display_flush_is_last(d)) {
		if (flip.single)
			single_copy();
		struct drm_mode_fb_dirty_cmd dirty = {
			.fb_id = drm_fb,
			.num_clips = nclips < MAX_CLIPS ? nclips : 0,	/* 0 = whole fb */
			.clips_ptr = (uintptr_t)clips,
		};
		uint64_t t = us();
		ioctl(drm_fd, DRM_IOCTL_MODE_DIRTYFB, &dirty);
		mo.flush += us() - t;
		nclips = 0;
	}
	lv_display_flush_ready(d);
}

void hal_damage(const lv_area_t *a)
{
	if (nclips < MAX_CLIPS)
		clips[nclips++] = (struct drm_clip_rect){ a->x1, a->y1, a->x2 + 1, a->y2 + 1 };
	else
		nclips = MAX_CLIPS;	/* DIRTYFB of the whole buffer */
}

static lv_display_t *drm_init(void)
{
	uint32_t crtcs[4], conns[4], encs[4], fbs[4];
	struct drm_mode_modeinfo modes[16];
	/* the first card that opens; adp-t7000 wins over simpledrm (it removes simpledrm when it loads) */
	int fd = -1;
	bool adp = false;
	for (int i = 0; i < 4 && !adp; i++) {
		char path[32], name[32] = "";
		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		int f = open(path, O_RDWR | O_CLOEXEC);
		if (f < 0)
			continue;
		struct drm_version v = { .name = name, .name_len = sizeof(name) - 1 };
		bool is_adp = !ioctl(f, DRM_IOCTL_VERSION, &v) && !strcmp(name, "adp-t7000");
		if (fd < 0 || is_adp) {
			if (fd >= 0)
				close(fd);
			fd = f, adp = is_adp;
		} else {
			close(f);
		}
	}
	if (fd < 0)
		return NULL;
	bool noflip = getenv("ORCHARD_NOFLIP");

	struct drm_mode_card_res res = { 0 };
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !res.count_crtcs || !res.count_connectors ||
	    res.count_crtcs > 4 || res.count_connectors > 4 || res.count_encoders > 4 || res.count_fbs > 4)
		goto fail;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	res.connector_id_ptr = (uintptr_t)conns;
	res.encoder_id_ptr = (uintptr_t)encs;
	res.fb_id_ptr = (uintptr_t)fbs;
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		goto fail;

	struct drm_mode_get_connector conn = { .connector_id = conns[0] };
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn) || !conn.count_modes || conn.count_modes > 16)
		goto fail;
	conn.modes_ptr = (uintptr_t)modes;
	conn.count_props = conn.count_encoders = 0;
	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &conn))
		goto fail;

	struct drm_mode_create_dumb dumb = { .width = modes[0].hdisplay, .height = modes[0].vdisplay, .bpp = 32 };
	if (ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &dumb))
		goto fail;
	struct drm_mode_fb_cmd fb = {
		.width = dumb.width, .height = dumb.height, .pitch = dumb.pitch,
		.bpp = 32, .depth = 24, .handle = dumb.handle,
	};
	if (ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb))
		goto fail;
	struct drm_mode_map_dumb map = { .handle = dumb.handle };
	if (ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
		goto fail;
	void *mem = mmap(NULL, dumb.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
	if (mem == MAP_FAILED)
		goto fail;

	struct drm_mode_crtc crtc = {
		.crtc_id = crtcs[0], .fb_id = fb.fb_id, .set_connectors_ptr = (uintptr_t)&conns[0],
		.count_connectors = 1, .mode = modes[0], .mode_valid = 1,
	};
	if (ioctl(fd, DRM_IOCTL_MODE_SETCRTC, &crtc))
		goto fail;

	drm_fd = fd;
	drm_fb = fb.fb_id;
	drm_conn = conns[0];
	drm_adp = adp;
	if (adp && !noflip) {	/* second buffer + the cached shadow LVGL renders into */
		struct drm_mode_create_dumb d2 = { .width = dumb.width, .height = dumb.height, .bpp = 32 };
		struct drm_mode_fb_cmd fb2 = { .width = dumb.width, .height = dumb.height, .bpp = 32, .depth = 24 };
		struct drm_mode_map_dumb m2 = { 0 };
		void *mem2 = MAP_FAILED;
		uint8_t *sh = aligned_alloc(64, dumb.size);
		if (sh && !ioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &d2) && d2.pitch == dumb.pitch &&
		    (fb2.pitch = d2.pitch, fb2.handle = d2.handle, !ioctl(fd, DRM_IOCTL_MODE_ADDFB, &fb2)) &&
		    (m2.handle = d2.handle, !ioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m2)) &&
		    (mem2 = mmap(NULL, d2.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m2.offset)) != MAP_FAILED) {
			memset(sh, 0, dumb.size);
			flip.on = true;
			flip.crtc = crtcs[0];
			flip.fb[0] = fb.fb_id, flip.fb[1] = fb2.fb_id;
			flip.mem[0] = mem, flip.mem[1] = mem2;
			flip.pitch = dumb.pitch;
			flip.shadow = sh;
			flip.back = 1;	/* buffer 0 is on screen (SETCRTC) */
			flip.nstale[0] = flip.nstale[1] = MAX_CLIPS + 1;
			mem = sh;
			fprintf(stderr, "orchard: adp-t7000, page flips (2 buffers + shadow)\n");
		} else {
			free(sh);
			fprintf(stderr, "orchard: adp-t7000 but no second buffer: single buffer, may tear\n");
		}
	}
	if (adp && !flip.on) {	/* one buffer: LVGL still renders into a cached shadow, damage is copied over */
		uint8_t *sh = aligned_alloc(64, dumb.size);
		if (sh) {
			memcpy(sh, mem, dumb.size);
			flip.single = true;
			flip.mem[0] = mem;
			flip.pitch = dumb.pitch;
			flip.shadow = sh;
			mem = sh;
			fprintf(stderr, "orchard: adp-t7000, single buffer (shadow + damage copies)\n");
		}
	}
	lv_display_t *d = lv_display_create(dumb.width, dumb.height);
	lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
	lv_draw_buf_init(&drm_buf, dumb.width, dumb.height, LV_COLOR_FORMAT_XRGB8888, dumb.pitch, mem, dumb.size);
	lv_display_set_draw_buffers(d, &drm_buf, NULL);
	lv_display_set_render_mode(d, LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(d, drm_flush);
	return d;
fail:
	close(fd);
	return NULL;
}

lv_display_t *hal_init(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	lv_tick_set_cb(tick);
	/* keep fbcon (and its cursor) from drawing over us */
	int tty = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (tty >= 0)
		ioctl(tty, KDSETMODE, KD_GRAPHICS);
	lv_display_t *d = drm_init();
	if (!d) {
		fprintf(stderr, "orchard: DRM init failed, falling back to /dev/fb0\n");
		d = lv_linux_fbdev_create();
		lv_linux_fbdev_set_file(d, "/dev/fb0");
	}
	if (getenv("ORCHARD_MOTION")) {
		mo.on = true;
		mo.verbose = atoi(getenv("ORCHARD_MOTION")) > 1;
		lv_display_add_event_cb(d, motion_event, LV_EVENT_REFR_START, NULL);
		lv_display_add_event_cb(d, motion_event, LV_EVENT_REFR_READY, NULL);
	}
	open_inputs();
	touch_init();
	touch_uninhibit_now();
	unlink(CMD_FIFO);
	mkfifo(CMD_FIFO, 0600);
	cmd_fd = open(CMD_FIFO, O_RDWR | O_NONBLOCK | O_CLOEXEC);	/* RDWR: never sees EOF */
	return d;
}

/* ---------- touch: calibrated raw → pixel mapping + tap steadying ---------- */

/* Raw panel units → screen pixels, fitted from tap targets (tools/tcal + bin/tcalfit; the panel's
 * absinfo range is wider than the glass, and fingers land below what the eye aims at).
 * Per axis: c0 + c1·u + c2·v + c3·u² + c4·v² + c5·u·v with u, v = raw / absinfo max (0..1).
 * Override: a "TOUCHCAL" line with 12 numbers in $ORCHARD_DATA/touchcal or /etc/orchard/touchcal. */
#define TOUCHCAL_X { 38.8561, 742.0148, -7.8718, 0.0000, 0.0000, 0.0000 }	/* 2026-10-18, 28 taps */
#define TOUCHCAL_Y { -35.4315, -10.6077, 1334.9131, 0.0000, 0.0000, 0.0000 }
static double cal[2][6] = { TOUCHCAL_X, TOUCHCAL_Y };
static bool cal_on = true;

static void cal_load(void)
{
	char p[256];
	const char *d = getenv("ORCHARD_DATA");
	snprintf(p, sizeof(p), "%s/touchcal", d ? d : "/etc/orchard");
	FILE *f = fopen(p, "r");
	if (!f)
		f = fopen("/etc/orchard/touchcal", "r");
	if (!f)
		return;
	double c[12];
	if (fscanf(f, "TOUCHCAL %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &c[0], &c[1], &c[2], &c[3], &c[4],
		   &c[5], &c[6], &c[7], &c[8], &c[9], &c[10], &c[11]) == 12)
		memcpy(cal, c, sizeof(cal));
	fclose(f);
}

static void map_touch(const struct dev *v, int rx, int ry, int *x, int *y)
{
	if (!cal_on) {	/* ORCHARD_TOUCHCAL=0: plain stretch of the absinfo range */
		*x = (rx - v->ax.minimum) * SCR_W / (v->ax.maximum - v->ax.minimum + !(v->ax.maximum - v->ax.minimum));
		*y = (ry - v->ay.minimum) * SCR_H / (v->ay.maximum - v->ay.minimum + !(v->ay.maximum - v->ay.minimum));
	} else {
		double u = (double)rx / (v->ax.maximum ? v->ax.maximum : 1), w = (double)ry / (v->ay.maximum ? v->ay.maximum : 1);
		double t[6] = { 1, u, w, u * u, w * w, u * w }, o[2] = { 0, 0 };
		for (int a = 0; a < 2; a++)
			for (int k = 0; k < 6; k++)
				o[a] += cal[a][k] * t[k];
		*x = (int)lround(o[0]);
		*y = (int)lround(o[1]);
	}
	*x = *x < 0 ? 0 : *x >= SCR_W ? SCR_W - 1 : *x;
	*y = *y < 0 ? 0 : *y >= SCR_H ? SCR_H - 1 : *y;
}

static struct steady { int id, x0, y0, x, y; bool moving; } steady[SLOTS];

static void touch_init(void)
{
	for (int k = 0; k < SLOTS; k++)
		steady[k].id = -1;
	cal_on = !getenv("ORCHARD_TOUCHCAL") || atoi(getenv("ORCHARD_TOUCHCAL"));
	cal_load();
}

/* The panel wobbles by up to ~20 px while a finger rests, most of all as it lands and lifts. Hold
 * each contact at its landing point until it has really moved (a tap stays a tap, never a tiny
 * drag), then follow it with light smoothing. */
#define STEADY_R 10	/* px a finger must travel before it counts as moving */

static void steady_pos(int k, int id, int *x, int *y)
{
	struct steady *s = &steady[k];
	if (s->id != id) {
		*s = (struct steady){ id, *x, *y, *x, *y, false };
		return;
	}
	if (!s->moving) {
		int dx = *x - s->x0, dy = *y - s->y0;
		if (dx * dx + dy * dy < STEADY_R * STEADY_R) {
			*x = s->x0;
			*y = s->y0;
			return;
		}
		s->moving = true;
	}
	s->x += (*x - s->x) * 2 / 3;	/* light smoothing: ~half a frame of lag */
	s->y += (*y - s->y) * 2 / 3;
	*x = s->x;
	*y = s->y;
}

/* after SYN_DROPPED: re-read every slot from the kernel */
static void mt_resync(struct dev *v)
{
	struct { uint32_t code; int32_t val[SLOTS]; } q;
	static const int codes[] = { ABS_MT_TRACKING_ID, ABS_MT_POSITION_X, ABS_MT_POSITION_Y };
	for (int c = 0; c < 3; c++) {
		memset(&q, 0, sizeof(q));
		q.code = codes[c];
		if (ioctl(v->fd, EVIOCGMTSLOTS(sizeof(q)), &q) < 0)
			return;
		for (int k = 0; k < SLOTS; k++) {
			if (c == 0)
				v->s[k].id = q.val[k];
			else if (c == 1)
				v->s[k].x = q.val[k];
			else
				v->s[k].y = q.val[k];
		}
	}
	struct input_absinfo sl;
	if (!ioctl(v->fd, EVIOCGABS(ABS_MT_SLOT), &sl))
		v->slot = sl.value;
}

/* every finger the slots hold now → the touch router */
static void mt_report(struct dev *v)
{
	mo.t_input = us();
	struct touch t[SLOTS];
	int n = 0;
	for (int k = 0; k < SLOTS; k++) {
		if (v->s[k].id < 0) {
			steady[k].id = -1;
			continue;
		}
		int x, y;
		map_touch(v, v->s[k].x, v->s[k].y, &x, &y);
		steady_pos(k, v->s[k].id, &x, &y);
		t[n++] = (struct touch){ v->s[k].id, x, y };
	}
	input_touches(t, n);
}

static void mt_event(struct dev *v, const struct input_event *ev)
{
	if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
		v->dropped = true;
		return;
	}
	if (v->dropped) {	/* discard up to the next report, then ask the kernel */
		if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
			v->dropped = false;
			mt_resync(v);
		} else {
			return;
		}
	}
	if (ev->type == EV_ABS) {
		int sl = v->slot >= 0 && v->slot < SLOTS ? v->slot : -1;
		switch (ev->code) {
		case ABS_MT_SLOT: v->slot = ev->value; break;
		case ABS_MT_TRACKING_ID: if (sl >= 0) v->s[sl].id = ev->value; break;
		case ABS_MT_POSITION_X: if (sl >= 0) v->s[sl].x = ev->value; break;
		case ABS_MT_POSITION_Y: if (sl >= 0) v->s[sl].y = ev->value; break;
		}
	} else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
		mt_report(v);
	}
}

/* " @<CLOCK_MONOTONIC s>": the same clock as the dmesg / netconsole stamps, to line log lines up with them */
static const char *ts(void)
{
	static char b[32];
	uint64_t t = us();
	snprintf(b, sizeof(b), " @%llu.%03llu", (unsigned long long)(t / 1000000), (unsigned long long)(t / 1000 % 1000));
	return b;
}

static void read_dev(int i)
{
	struct dev *v = &dev[i];
	struct input_event ev;
	while (read(v->fd, &ev, sizeof(ev)) == sizeof(ev)) {
		if (!v->touch) {
			/* every hardware key event in the log (a few lines per press): a press that never shows up here
			 * never left the kernel */
			if (ev.type == EV_SYN && ev.code == SYN_DROPPED)
				fprintf(stderr, "orchard: keys: SYN_DROPPED%s\n", ts());
			if (ev.type == EV_KEY && ev.value != 2) {
				fprintf(stderr, "orchard: key %d %s%s%s\n", ev.code, ev.value ? "down" : "up",
					shell_screen_off() ? power_dark() ? " (dark)" : " (screen off)" : "", ts());
				mo.t_input = us();
				input_key(ev.code, ev.value);
			}
		} else if (v->mt) {
			mt_event(v, &ev);
		} else if (ev.type == EV_ABS && ev.code == ABS_X) {
			v->tx = ev.value;
		} else if (ev.type == EV_ABS && ev.code == ABS_Y) {
			v->ty = ev.value;
		} else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
			v->tdown = ev.value;
		} else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
			int x, y;
			map_touch(v, v->tx, v->ty, &x, &y);
			if (v->tdown)
				steady_pos(0, 0, &x, &y);
			else
				steady[0].id = -1;
			input_feed(x, y, v->tdown);
		}
	}
}

/* Screen off: the touch panel is inhibited (powered down where the driver can) and isn't read at all (a phone in
 * a pocket reports touches all the time; they would only be thrown away), so the main loop sleeps until a key or
 * a timer. On: what queued up is dropped and the
 * slots are read back from the kernel, so fingers lifted meanwhile are released and one resting now counts. */
static bool touch_off;
static void touch_inhibit(bool on);
static bool inh_write(const struct dev *v, bool on);

/* startup: an Orchard that went away while the screen was off (deploy, crash) left the touch panel inhibited, and
 * an open of an inhibited device never starts it — so before the first read, always 0 (synchronous: if that
 * boots the controller, it is part of startup) */
static void touch_uninhibit_now(void)
{
	for (int i = 0; i < ndev; i++)
		if (dev[i].touch && dev[i].mt && !inh_write(&dev[i], false) && errno != ENOENT)
			fprintf(stderr, "orchard: %sinhibited = 0 at start: %s\n", dev[i].sysfs, strerror(errno));
}

void hal_screen(bool on)
{
	if (touch_off == !on)
		return;
	touch_off = !on;
	touch_inhibit(!on);
	if (!on)
		return;
	for (int i = 0; i < ndev; i++) {
		struct dev *v = &dev[i];
		struct input_event ev;
		if (!v->touch)
			continue;
		while (read(v->fd, &ev, sizeof(ev)) == sizeof(ev))
			;
		if (v->mt) {
			v->dropped = false;
			mt_resync(v);
			mt_report(v);
		} else if (v->tdown) {
			int x, y;
			map_touch(v, v->tx, v->ty, &x, &y);
			v->tdown = false;
			steady[0].id = -1;
			input_feed(x, y, false);
		}
	}
}

/* Touch power: the input core's "inhibited" switch (a driver with open/close powers the panel down; on an
 * older kernel the write fails and nothing changes). Written by a helper thread: switching it back on boots the
 * touch controller, which may take a while, and the wake must not wait for that. */
static pthread_mutex_t inh_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t inh_cv = PTHREAD_COND_INITIALIZER;
static int inh_want = -1;

/* one touch device's switch; false = the write failed (errno). Reading it back catches a kernel that took the 0
 * but left the device inhibited because the driver's open failed. */
static bool inh_write(const struct dev *v, bool on)
{
	char p[96], back[4] = "";
	snprintf(p, sizeof(p), "%sinhibited", v->sysfs);
	int fd = open(p, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return false;
	bool ok = write(fd, on ? "1" : "0", 1) == 1;
	int e = errno;
	if (ok && lseek(fd, 0, SEEK_SET) == 0 && read(fd, back, sizeof(back) - 1) > 0 && back[0] != (on ? '1' : '0'))
		ok = false, e = EIO;
	close(fd);
	errno = e;
	return ok;
}

static void *inh_thread(void *arg)
{
	(void)arg;
	int done = -1;
	for (;;) {
		pthread_mutex_lock(&inh_mu);
		while (inh_want == done)
			pthread_cond_wait(&inh_cv, &inh_mu);
		int w = done = inh_want;
		pthread_mutex_unlock(&inh_mu);
		for (int i = 0; i < ndev; i++) {
			if (!dev[i].touch || !dev[i].mt)
				continue;
			char p[96];
			snprintf(p, sizeof(p), "%sinhibited", dev[i].sysfs);
			if (w) {	/* off: one try (failing = touch just stays powered) */
				static int warned;
				if (!inh_write(&dev[i], true) && !warned++)
					fprintf(stderr, "orchard: %s = 1: %s (touch stays powered while dark)\n", p, strerror(errno));
				continue;
			}
			/* on: the driver boots the touch controller here and may fail (it tries twice itself): try 3 times,
			 * 500 ms apart, unless the screen went off again meanwhile */
			for (int k = 1;; k++) {
				if (inh_write(&dev[i], false)) {
					if (k > 1)
						fprintf(stderr, "orchard: %s = 0: ok on try %d\n", p, k);
					break;
				}
				if (errno == ENOENT)
					break;	/* kernel without the switch: nothing was inhibited */
				fprintf(stderr, "orchard: %s = 0 (try %d): %s\n", p, k, strerror(errno));
				if (k == 3) {
					fprintf(stderr, "orchard: TOUCH STILL INHIBITED after 3 tries: touch is dead until the "
							"screen goes off and on again (Power twice)\n");
					break;
				}
				usleep(500000);
				pthread_mutex_lock(&inh_mu);
				bool changed = inh_want != w;
				pthread_mutex_unlock(&inh_mu);
				if (changed)
					break;
			}
		}
		fflush(stderr);
	}
	return NULL;
}

static void touch_inhibit(bool on)
{
	static bool started;
	pthread_mutex_lock(&inh_mu);
	inh_want = on;
	pthread_cond_signal(&inh_cv);
	pthread_mutex_unlock(&inh_mu);
	if (!started) {
		started = true;
		pthread_t t;
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&a, 64 << 10);
		pthread_create(&t, &a, inh_thread, NULL);
		pthread_attr_destroy(&a);
	}
}

/* Display pipe power (adp-t7000's connector "DPMS": off ≈ 25 ms, on ≈ 250 ms; off also sleeps the panel with
 * the driver's panel_sleep). While off page flips fail, so power.c only switches it off once the UI sleeps and on
 * before the wake redraw. A kernel without it: the property is missing / the ioctl fails, the pipe stays on. */
static uint32_t dpms_prop;
static bool dpms_off;
static bool disp_bad;	/* DPMS on went through, but the kernel says the screen isn't on (display_state "bad") */
static uint64_t disp_bad_at;	/* when the on (or the last off + on) was reported bad */

static bool dpms_find(void)
{
	if (dpms_prop)
		return true;
	uint32_t ids[32];
	uint64_t vals[32];
	struct drm_mode_obj_get_properties op = { .obj_id = drm_conn, .obj_type = DRM_MODE_OBJECT_CONNECTOR };
	if (ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
		return false;
	op.count_props = op.count_props > 32 ? 32 : op.count_props;
	op.props_ptr = (uintptr_t)ids;
	op.prop_values_ptr = (uintptr_t)vals;
	if (ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_GETPROPERTIES, &op))
		return false;
	for (uint32_t i = 0; i < op.count_props; i++) {
		struct drm_mode_get_property p = { .prop_id = ids[i] };
		if (!ioctl(drm_fd, DRM_IOCTL_MODE_GETPROPERTY, &p) && !strcmp(p.name, "DPMS"))
			return dpms_prop = ids[i], true;
	}
	return false;
}

/* adp-t7000's display_state (the card's device dir): 1 = "ok", 0 = "bad" (CRTC on, but the layer or the panel
 * didn't come back; the kernel keeps retrying), -1 = no such file (older driver: trust the ioctl) */
static int display_state(char *buf, size_t n)
{
	struct stat st;
	char p[96];
	buf[0] = 0;
	if (fstat(drm_fd, &st))
		return -1;
	snprintf(p, sizeof(p), "/sys/dev/char/%u:%u/device/display_state", major(st.st_rdev), minor(st.st_rdev));
	int fd = open(p, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	ssize_t r = read(fd, buf, n - 1);
	close(fd);
	if (r <= 0)
		return -1;
	buf[r] = 0;
	buf[strcspn(buf, "\n")] = 0;
	return strncmp(buf, "bad", 3) != 0;
}

static bool dpms_set(bool on)
{
	struct drm_mode_obj_set_property sp = { .value = on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF, .prop_id = dpms_prop,
						.obj_id = drm_conn, .obj_type = DRM_MODE_OBJECT_CONNECTOR };
	for (int k = 1; ioctl(drm_fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &sp); k++) {
		fprintf(stderr, "orchard: DPMS %s (try %d): %s%s\n", on ? "on" : "off", k, strerror(errno), ts());
		if (!on || k == 3)	/* off failed: still on, carry on as before; on failed: still off (power.c retries) */
			return false;
		usleep(20000);
	}
	dpms_off = !on;
	return true;
}

/* false for "on" = the screen isn't showing anything yet: power.c keeps the backlight off and calls again every
 * 500 ms (every 30 s after 10 s of that). A DPMS on the kernel reports bad is re-checked on those calls (the kernel
 * retries by itself) and, still bad ≥ 2 s after the last try, switched off and on again. */
bool hal_display(bool on)
{
	char st[192];
	if (drm_fd < 0 || !drm_adp || !dpms_find())
		return true;	/* nothing to switch */
	if (!on)
		disp_bad = false;
	if (on && !dpms_off && disp_bad) {	/* a retry after an on the kernel reported bad */
		if (display_state(st, sizeof(st)) != 0) {
			disp_bad = false;
			fprintf(stderr, "orchard: display on now (%s)%s\n", st, ts());
			return true;
		}
		if (us() - disp_bad_at < 2000000)
			return false;
		fprintf(stderr, "orchard: display still not on (%s): DPMS off + on%s\n", st, ts());
		if (!dpms_set(false))
			return false;
	} else if (dpms_off == !on) {
		return true;	/* already there */
	}
	if (!on && flip.on) {	/* the last flip lands first (its event would never come) */
		flip_wait(50);
		flip.pending = flip.deferred = false;
	}
	uint64_t t = us();
	if (!dpms_set(on))
		return false;
	if (on) {	/* whatever vram holds is stale: both buffers get the whole next frame */
		flip.pending = flip.deferred = false;
		flip.nstale[0] = flip.nstale[1] = MAX_CLIPS + 1;
		nclips = MAX_CLIPS;
	}
	int ok = on ? display_state(st, sizeof(st)) : 1;
	fprintf(stderr, "orchard: display %s (%llu ms)%s%s%s\n", on ? "on" : "off", (unsigned long long)(us() - t) / 1000,
		ok ? "" : " BUT the kernel says: ", ok ? "" : st, ts());
	if (!ok) {
		disp_bad = true;
		disp_bad_at = us();
		return false;
	}
	return true;
}

/* /tmp/orchard.state for tools/disp/stress.sh: what Orchard believes, next to what the kernel says */
static void write_state(void)
{
	char st[192];
	int k = drm_fd >= 0 && drm_adp ? display_state(st, sizeof(st)) : -1;
	FILE *f = fopen("/tmp/orchard.state.tmp", "w");
	if (!f)
		return;
	fprintf(f, "screen %s dark %d dpms %s disp_bad %d touch %s backlight %d kernel %s\n",
		shell_screen_off() ? "off" : "on", power_dark(), dpms_off ? "off" : "on", disp_bad,
		touch_off ? "off" : "on", sys_backlight_get(), k < 0 ? "-" : st);
	fclose(f);
	rename("/tmp/orchard.state.tmp", "/tmp/orchard.state");
}

static void do_cmd(char *line)
{
	int a, b, c, d, ms;
	char key[16];
	if (sscanf(line, "tap %d %d", &a, &b) == 2) {
		drag = (typeof(drag)){ true, tick(), 60, a, b, a, b };
	} else if (sscanf(line, "drag %d %d %d %d %d", &a, &b, &c, &d, &ms) == 5) {
		drag = (typeof(drag)){ true, tick(), ms > 0 ? ms : 1, a, b, c, d };
	} else if (!strcmp(line, "shot")) {
		lv_draw_buf_t *b = lv_display_get_buf_active(lv_display_get_default());
		FILE *f = fopen("/tmp/orchard.raw.tmp", "wb");
		for (int y = 0; f && b && y < SCR_H; y++)
			fwrite(b->data + y * b->header.stride, 4, SCR_W, f);
		if (f) {
			fclose(f);
			rename("/tmp/orchard.raw.tmp", "/tmp/orchard.raw");
		}
	} else if (!strcmp(line, "state")) {
		write_state();
	} else if (!strncmp(line, "key", 3) && sscanf(line + strcspn(line, " "), "%15s", key) == 1) {
		/* key X = press + release at once; keydown X / keyup X = one of them (real-key timing for stress.sh) */
		int code = !strcmp(key, "home") ? KEY_HOMEPAGE : !strcmp(key, "power") ? KEY_POWER : atoi(key);
		bool down = strncmp(line, "keyup", 5) != 0, up = strncmp(line, "keydown", 7) != 0;
		mo.t_input = us();
		if (down)
			input_key(code, true);
		if (up)
			input_key(code, false);
	}
}

static void read_cmds(void)
{
	static char buf[512];
	static int len;
	ssize_t n;
	while (cmd_fd >= 0 && (n = read(cmd_fd, buf + len, sizeof(buf) - 1 - len)) > 0) {
		len += n;
		buf[len] = 0;
		char *nl;
		while ((nl = strchr(buf, '\n'))) {
			*nl = 0;
			do_cmd(buf);
			len -= nl + 1 - buf;
			memmove(buf, nl + 1, len + 1);
		}
		if (len == sizeof(buf) - 1)
			len = 0;
	}
}

/* synthetic touch for tap/drag commands, interpolated in real time */
static void run_drag(void)
{
	if (!drag.on)
		return;
	uint32_t t = tick() - drag.t0;
	mo.t_input = us();
	if (t >= drag.ms) {
		input_feed(drag.x2, drag.y2, true);
		input_feed(drag.x2, drag.y2, false);
		drag.on = false;
		return;
	}
	input_feed(drag.x1 + (drag.x2 - drag.x1) * (int)t / (int)drag.ms,
		   drag.y1 + (drag.y2 - drag.y1) * (int)t / (int)drag.ms, true);
}

/* hal_wake: an eventfd in hal_run's poll set (made on the first hal_wake_set, on the UI thread) */
static int wake_fd = -1;
static void (*wake_cb)(void);

void hal_wake_set(void (*cb)(void))
{
	if (wake_fd < 0)
		wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	wake_cb = cb;
}

void hal_wake(void)
{
	uint64_t one = 1;
	if (wake_fd >= 0 && write(wake_fd, &one, sizeof(one)) < 0) {
		/* (EAGAIN: the counter is full of wakes nobody read yet — one is enough) */
	}
}

void hal_run(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	struct pollfd p[MAX_DEV + 3];
	for (int i = 0; i < ndev; i++)
		p[i] = (struct pollfd){ .fd = dev[i].fd, .events = POLLIN };
	p[ndev] = (struct pollfd){ .fd = cmd_fd, .events = POLLIN };
	p[ndev + 1] = (struct pollfd){ .fd = flip.on ? drm_fd : -1, .events = POLLIN };	/* flip completions */
	for (;;) {
		uint64_t t0 = mo.on ? us() : 0;
		power_wakeups++;
		uint32_t idle = lv_timer_handler();
		if (mo.on) {	/* ORCHARD_MOTION: work outside drawing that holds up the UI (building a page, a slow timer) */
			uint64_t busy = us() - t0 - mo.drawn;
			if (busy > 25000) {
				printf("stall: %llu ms of work in one pass (+%llu ms drawing)\n", (unsigned long long)busy / 1000,
				       (unsigned long long)mo.drawn / 1000);
				fflush(stdout);
			}
			mo.drawn = 0;
		}
		/* sleep until the next LVGL timer is due (input wakes the poll): a fixed 10 ms cap meant 100
		 * wake-ups a second with nothing to do. Dark (power.c): only the slow background timers are left,
		 * and touch isn't polled */
		uint32_t cap = power_dark() ? 60000 : 1000;
		for (int i = 0; i < ndev; i++)
			p[i].fd = dev[i].touch && touch_off ? -1 : dev[i].fd;
		p[ndev + 2] = (struct pollfd){ .fd = wake_fd, .events = POLLIN };	/* (-1 until a hal_wake_set) */
		poll(p, ndev + 3, flip_service(drag.on ? 5 : idle > cap ? (int)cap : (int)idle));
		flip_service(0);
		uint64_t wakes;
		if ((p[ndev + 2].revents & POLLIN) && read(wake_fd, &wakes, sizeof(wakes)) == sizeof(wakes) && wake_cb)
			wake_cb();
		for (int i = 0; i < ndev; i++)
			if (!(dev[i].touch && touch_off))
				read_dev(i);
		read_cmds();
		run_drag();
	}
}
