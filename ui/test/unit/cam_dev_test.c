/* cam_dev.c's shutter against a stub source (cam_src_* + cam_save_* here; cam_conv.c real): a driver without the
 * presets = the next frame is the photo (no STOP / START(STILL)); with them a full-resolution still, the shutter busy
 * from the tap until the viewfinder's next frame and every tap meanwhile refused. The camera turned off during a
 * still (Home 300 ms after the tap, an off → on bounce) still saves it — a bright frame, not a dark AE start one —
 * then powers down, capped at STILL_OFF_MAX frames; a flip finishes it too; a scene that never settles saves the
 * last of STILL_MAX frames; a tap the worker hadn't picked up when the camera went off isn't taken after it comes
 * back. Plus the failure paths (START(STILL) / viewfinder re-START fail, source error, no frames, the driver
 * downgrading): none leaves the shutter busy. Usage: cam_dev_test */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "cam.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---------- the stub source (called on the camera thread only; the test reads it under mu) ---------- */

#define VW 160
#define VH 120
#define SW 320
#define SH 240
#define NB 4
#define VIEW_Y 120		/* the viewfinder's brightness (mean Y) */

enum still_y {
	Y_STEADY,		/* VIEW_Y from the first frame */
	Y_ALT,			/* 60 / 180 alternating: never steady */
	Y_RAMP,			/* AE from dark: 20 + 8 per frame up to VIEW_Y (80 % of it at frame 10) */
	Y_DARK,			/* 20 for ever: never bright enough */
};

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	bool presets;		/* the driver has the flags */
	enum still_y still_y;
	int period_ms;		/* between frames */
	/* failures to inject (counts) */
	int fail_still_start, fail_view_start, err_still, nofr_still;
	bool downgrade_on_still;	/* START(STILL) finds the driver without the flags (EINVAL → bare) */
	/* counters / state */
	int opens, closes, starts_view, starts_still, stops, still_frames, saves, held;
	int start_cam;		/* camera of the last START */
	bool is_open, running, still;
	unsigned nexts;		/* cam_src_next calls so far (bumped on entry) */
	int saved_w, saved_h, saved_y;
	bool saved_mirror;
	int restarts, restart_cam;	/* cam_focus_restarted calls, its camera */
	double closed_at, saved_at;
} B = { .period_ms = 10 };

struct cam_src {
	cam_info_t info;
	uint8_t *buf;
	uint32_t seq;
	int k;			/* frames since START */
};

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

cam_src_t *cam_src_open(char *why, size_t n)
{
	(void)why, (void)n;
	cam_src_t *s = calloc(1, sizeof(*s));
	s->buf = malloc((size_t)SW * SH * 3 / 2 * NB);
	pthread_mutex_lock(&mu);
	B.opens++, B.is_open = true;
	pthread_mutex_unlock(&mu);
	return s;
}

const cam_info_t *cam_src_info(cam_src_t *s) { return &s->info; }

bool cam_src_start(cam_src_t *s, int camera, enum cam_mode mode, char *why, size_t n)
{
	pthread_mutex_lock(&mu);
	if (mode == CAM_MODE_STILL)
		B.starts_still++;
	else
		B.starts_view++;
	int *fail = mode == CAM_MODE_STILL ? &B.fail_still_start : &B.fail_view_start;
	if (*fail) {
		(*fail)--;
		pthread_mutex_unlock(&mu);
		snprintf(why, n, "%s START failed (test)", mode == CAM_MODE_STILL ? "still" : "viewfinder");
		return false;
	}
	if (mode == CAM_MODE_STILL && B.downgrade_on_still)
		B.presets = false;
	const bool still = mode == CAM_MODE_STILL && B.presets;
	B.running = true, B.still = still, B.start_cam = camera;
	pthread_mutex_unlock(&mu);
	s->info = (cam_info_t){ still ? SW : VW, still ? SH : VH, still ? SW : VW, NB, "stub" };
	s->k = 0;
	return true;
}

void cam_src_stop(cam_src_t *s)
{
	(void)s;
	pthread_mutex_lock(&mu);
	B.stops++, B.running = false;
	pthread_mutex_unlock(&mu);
}

void cam_focus_service(cam_src_t *s, int camera, const cam_info_t *in, cam_orient_t o, int dw, int dh)
{
	(void)s, (void)camera, (void)in, (void)o, (void)dw, (void)dh;	/* tap to focus: cam_focus.c, not under test here */
}

void cam_focus_restarted(int camera)
{
	pthread_mutex_lock(&mu);
	B.restarts++, B.restart_cam = camera;
	pthread_mutex_unlock(&mu);
}

bool cam_src_has_presets(cam_src_t *s)
{
	(void)s;
	pthread_mutex_lock(&mu);
	bool p = B.presets;
	pthread_mutex_unlock(&mu);
	return p;
}

int cam_src_next(cam_src_t *s, cam_frame_t *f, int timeout_ms, char *why, size_t n)
{
	pthread_mutex_lock(&mu);
	const bool run = B.running, still = B.still;
	const enum still_y sy = B.still_y;
	const int period = B.period_ms;
	if (still && B.err_still) {
		B.err_still--;
		pthread_mutex_unlock(&mu);
		snprintf(why, n, "source error (test)");
		return -1;
	}
	const bool nofr = still && B.nofr_still;
	B.nexts++;
	pthread_mutex_unlock(&mu);
	if (!run || nofr || period > timeout_ms) {
		usleep((useconds_t)timeout_ms * 1000);
		return 0;
	}
	usleep((useconds_t)period * 1000);
	const int w = s->info.w, h = s->info.h, idx = (int)(s->seq % NB), k = s->k++;
	int y0 = VIEW_Y;
	if (still)
		y0 = sy == Y_ALT ? (k & 1 ? 60 : 180) : sy == Y_RAMP ? (20 + 8 * k < VIEW_Y ? 20 + 8 * k : VIEW_Y)
			: sy == Y_DARK ? 20 : VIEW_Y;
	uint8_t *y = s->buf + (size_t)idx * SW * SH * 3 / 2;
	memset(y, y0, (size_t)w * h);
	memset(y + (size_t)w * h, 128, (size_t)w * h / 2);
	f->index = idx;
	f->seq = s->seq++;
	f->ts_ns = (uint64_t)(now_ms() * 1e6);
	f->y = y;
	f->uv = y + (size_t)w * h;
	pthread_mutex_lock(&mu);
	B.held++;
	if (still)
		B.still_frames++;
	pthread_mutex_unlock(&mu);
	return 1;
}

void cam_src_release(cam_src_t *s, const cam_frame_t *f)
{
	(void)s, (void)f;
	pthread_mutex_lock(&mu);
	B.held--;
	pthread_mutex_unlock(&mu);
}

void cam_src_close(cam_src_t *s)
{
	if (!s)
		return;
	free(s->buf);
	free(s);
	pthread_mutex_lock(&mu);
	B.closes++, B.is_open = B.running = false, B.closed_at = now_ms();
	pthread_mutex_unlock(&mu);
}

/* ---------- cam_save stubs ---------- */

bool cam_save_photo(char *path, size_t pn, int num, const uint8_t *nv12, int w, int h, int stride, cam_orient_t o,
		    char *why, size_t n)
{
	(void)o, (void)why, (void)n;
	snprintf(path, pn, "IMG_%04d.JPG", num);
	pthread_mutex_lock(&mu);
	B.saves++, B.saved_w = w, B.saved_h = h, B.saved_y = nv12[(size_t)(h / 2) * stride + w / 2];
	B.saved_mirror = o.mirror;
	B.saved_at = now_ms();
	pthread_mutex_unlock(&mu);
	return true;
}
bool cam_thumb_write(const char *photo, const uint32_t *px) { (void)photo, (void)px; return true; }
int cam_photos_next_number(void) { return 1; }

/* ---------- helpers ---------- */

#define GET(field) ({ pthread_mutex_lock(&mu); __typeof__(B.field) v_ = B.field; pthread_mutex_unlock(&mu); v_; })
#define SET(field, v) do { pthread_mutex_lock(&mu); B.field = (v); pthread_mutex_unlock(&mu); } while (0)

/* wait up to ms for cond (polled every 2 ms); false = timed out */
#define WAIT_FOR(cond, ms) ({ double t_ = now_ms() + (ms); bool ok_; \
	while (!(ok_ = (cond)) && now_ms() < t_) usleep(2000); ok_; })

static uint32_t thumb[CAM_THUMB * CAM_THUMB];
static char res_why[160];

static int result(int ms)	/* the photo's outcome within ms: 1 / -1 / 0 */
{
	char path[256];
	int r = 0;
	res_why[0] = 0;
	WAIT_FOR((r = cam_dev_capture_result(path, sizeof(path), thumb, res_why, sizeof(res_why))) != 0, ms);
	return r;
}

static enum cam_state state(void) { return cam_dev_state(NULL, 0, NULL); }

static void cam_on(int camera)
{
	cam_dev_run(true, camera, 64, 64);
	CHECK(WAIT_FOR(state() == CAM_RUNNING, 2000), "viewfinder not running");
	while (cam_dev_frame())	/* (what the UI does: takes frames) */
		;
}

static void cam_off(void)
{
	cam_dev_run(false, 0, 64, 64);
	CHECK(WAIT_FOR(state() == CAM_OFF && !GET(is_open), 3000), "camera not off");
}

static void setup(bool presets, enum still_y sy, int period_ms)
{
	SET(presets, presets);
	SET(still_y, sy);
	SET(period_ms, period_ms);
}

static void busy_cleared(const char *what)
{
	CHECK(WAIT_FOR(!cam_dev_busy(), 4000), "%s: shutter stuck busy (state %d)", what, state());
}

/* ---------- tests ---------- */

/* no presets (an older driver, the fake source): the next frame, at viewfinder size, no STOP / START(STILL) */
static void test_instant(void)
{
	setup(false, Y_STEADY, 10);
	cam_on(0);
	const int stops = GET(stops), stills = GET(starts_still), saves = GET(saves);
	CHECK(cam_dev_capture(), "instant: capture refused");
	CHECK(!cam_dev_busy(), "instant: busy (the shutter would flicker grey)");
	CHECK(!cam_dev_capture(), "instant: a second tap before the frame was taken accepted (same photo)");
	CHECK(result(2000) == 1, "instant: no photo");
	CHECK(GET(saves) == saves + 1 && GET(saved_w) == VW && GET(saved_h) == VH, "instant: saved %d at %dx%d",
	      GET(saves) - saves, GET(saved_w), GET(saved_h));
	CHECK(GET(stops) == stops && GET(starts_still) == stills, "instant: %d STOP(s), %d START(STILL)",
	      GET(stops) - stops, GET(starts_still) - stills);
	CHECK(WAIT_FOR(cam_dev_capture(), 500), "instant: second photo refused");
	CHECK(result(2000) == 1, "instant: no second photo");
	cam_off();
}

/* presets: a full-resolution still; busy from the tap until the viewfinder's next frame, taps meanwhile refused */
static void test_still(void)
{
	setup(true, Y_STEADY, 10);
	cam_on(0);
	const int saves = GET(saves), stills = GET(starts_still), views = GET(starts_view);
	CHECK(cam_dev_capture(), "still: capture refused");
	CHECK(cam_dev_busy(), "still: not busy right after the tap");
	CHECK(!cam_dev_capture(), "still: tap during the still accepted");
	bool refused = true;
	while (cam_dev_busy() && GET(starts_still) == stills)	/* (taps until it starts: all refused) */
		refused &= !cam_dev_capture(), usleep(1000);
	CHECK(WAIT_FOR(GET(saves) == saves + 1, 2000), "still: not saved");
	while (cam_dev_busy() && GET(starts_view) == views)	/* (taps after the save, before the viewfinder) */
		refused &= !cam_dev_capture(), usleep(1000);
	CHECK(refused, "still: a tap during the still was accepted");
	CHECK(GET(saved_w) == SW && GET(saved_h) == SH, "still: saved at %dx%d", GET(saved_w), GET(saved_h));
	CHECK(GET(starts_still) == stills + 1, "still: %d START(STILL)", GET(starts_still) - stills);
	CHECK(result(2000) == 1, "still: no result");
	CHECK(WAIT_FOR(!cam_dev_busy(), 2000), "still: busy after the viewfinder came back");
	CHECK(GET(starts_view) == views + 1 && state() == CAM_RUNNING,
	      "still: viewfinder not back (%d START(VIEW))", GET(starts_view) - views);
	CHECK(cam_dev_capture(), "still: shutter refused after the viewfinder came back");
	CHECK(result(3000) == 1, "still: no second photo");
	CHECK(WAIT_FOR(!cam_dev_busy(), 2000), "still: busy after the second photo");
	cam_off();
}

/* tap, then Home (or lock, switcher, the thumbnail's viewer) 300 ms later, AE still coming up from dark: the photo
 * the user got the blink for is saved — a bright frame, without waiting for steady — then the sensor powers down */
static void test_off_mid_still(void)
{
	setup(true, Y_RAMP, 30);
	cam_on(0);
	const int saves = GET(saves), f0 = GET(still_frames);
	CHECK(cam_dev_capture(), "off mid-still: capture refused");
	usleep(300000);
	const int k_off = GET(still_frames) - f0;
	const double t0 = now_ms();
	cam_dev_run(false, 0, 64, 64);	/* (Home) */
	CHECK(result(3000) == 1, "off mid-still: no photo (%s)", res_why);
	CHECK(GET(saves) == saves + 1 && GET(saved_w) == SW, "off mid-still: %d photo(s), %d wide", GET(saves) - saves,
	      GET(saved_w));
	CHECK(GET(saved_y) * 5 >= VIEW_Y * 4, "off mid-still: a dark frame saved (Y %d)", GET(saved_y));
	CHECK(WAIT_FOR(!GET(is_open), 1500), "off mid-still: the sensor stayed on");
	const int frames = GET(still_frames) - f0;
	CHECK(frames <= 12, "off mid-still: %d still frames (bright at 10: no waiting for steady once off)", frames);
	CHECK(GET(closed_at) - GET(saved_at) < 300, "off mid-still: closed %.0f ms after the save",
	      GET(closed_at) - GET(saved_at));
	CHECK(state() == CAM_OFF && !cam_dev_busy(), "off mid-still: not off / busy");
	printf("cam_dev: Home %d frame(s) into a still: saved after %d frame(s), sensor off %.0f ms after Home\n", k_off,
	       frames, GET(closed_at) - t0);
	/* back on: photos work again */
	setup(true, Y_STEADY, 10);
	cam_on(0);
	CHECK(cam_dev_capture() && result(3000) == 1, "off mid-still: no photo after the camera came back");
	cam_off();
}

/* off during a still that never gets bright: saved at STILL_OFF_MAX frames, not STILL_MAX */
static void test_off_cap(void)
{
	setup(true, Y_DARK, 5);
	cam_on(0);
	const int f0 = GET(still_frames), saves = GET(saves);
	CHECK(cam_dev_capture(), "off cap: capture refused");
	CHECK(WAIT_FOR(GET(still_frames) >= f0 + 3, 2000), "off cap: the still didn't start");
	cam_dev_run(false, 0, 64, 64);
	CHECK(result(3000) == 1 && GET(saves) == saves + 1, "off cap: no photo");
	CHECK(WAIT_FOR(!GET(is_open), 1500), "off cap: the sensor stayed on");
	CHECK(GET(still_frames) - f0 == 40, "off cap: %d still frames, want 40", GET(still_frames) - f0);
	CHECK(state() == CAM_OFF && !cam_dev_busy(), "off cap: not off / busy");
}

/* never settles, camera on: the last of STILL_MAX frames is saved (the timeout saves, it doesn't fail) */
static void test_timeout_saves(void)
{
	setup(true, Y_ALT, 2);
	cam_on(0);
	const int f0 = GET(still_frames), saves = GET(saves);
	CHECK(cam_dev_capture(), "timeout: capture refused");
	CHECK(result(5000) == 1, "timeout: no photo");
	CHECK(GET(saves) == saves + 1 && GET(saved_w) == SW, "timeout: saved %d at %d wide", GET(saves) - saves,
	      GET(saved_w));
	CHECK(GET(still_frames) - f0 == 60, "timeout: %d still frames, want 60", GET(still_frames) - f0);
	cam_off();
}

/* Mirror Front Camera: a front photo is saved mirrored like the viewfinder (default), un-mirrored when it is off;
 * a rear photo never is. Both paths (instant, full-resolution still) */
static void test_mirror(void)
{
	for (int presets = 0; presets < 2; presets++) {
		setup(presets, Y_STEADY, 10);
		for (int cam = 0; cam < 2; cam++)
			for (int on = 1; on >= 0; on--) {
				cam_on(cam);
				cam_dev_mirror(on);
				const int saves = GET(saves);
				CHECK(WAIT_FOR(cam_dev_capture(), 1000), "mirror: capture refused");
				CHECK(result(3000) == 1 && GET(saves) == saves + 1, "mirror: no photo");
				CHECK(GET(saved_mirror) == (cam == 1 && on), "mirror: presets %d camera %d setting %d: saved %s",
				      presets, cam, on, GET(saved_mirror) ? "mirrored" : "un-mirrored");
				busy_cleared("mirror");
				cam_off();
			}
	}
	cam_dev_mirror(true);
}

/* after a full-resolution still the viewfinder's re-START tells cam_focus (the tapped / locked lens is held again);
 * a plain START (open, flip) doesn't */
static void test_focus_restart(void)
{
	setup(true, Y_STEADY, 10);
	const int r0 = GET(restarts);
	cam_on(0);
	CHECK(GET(restarts) == r0, "focus: a plain START counted as the photo's re-START");
	CHECK(cam_dev_capture(), "focus: capture refused");
	CHECK(result(3000) == 1, "focus: no photo");
	busy_cleared("focus");
	CHECK(GET(restarts) == r0 + 1 && GET(restart_cam) == 0, "focus: %d re-START notice(s) after the photo",
	      GET(restarts) - r0);
	cam_off();
	cam_on(0);
	CHECK(GET(restarts) == r0 + 1, "focus: a re-START notice after close + open");
	cam_off();
}

/* a flip during a still finishes that photo (from the camera it was taken with), then switches */
static void test_flip(void)
{
	setup(true, Y_ALT, 5);	/* 60 frames ≈ 0.3 s */
	cam_on(0);
	const int f0 = GET(still_frames), saves = GET(saves);
	CHECK(cam_dev_capture(), "flip: capture refused");
	CHECK(WAIT_FOR(GET(still_frames) >= f0 + 3, 2000), "flip: the still didn't start");
	cam_dev_run(true, 1, 64, 64);
	CHECK(result(3000) == 1 && GET(saves) == saves + 1, "flip: the photo was lost");
	CHECK(GET(still_frames) - f0 == 60, "flip: the still was cut short (%d frames)", GET(still_frames) - f0);
	CHECK(WAIT_FOR(GET(start_cam) == 1 && state() == CAM_RUNNING, 2000),
	      "flip: the front camera didn't start after the photo");
	CHECK(WAIT_FOR(!cam_dev_busy(), 1000), "flip: busy after the switch");
	cam_off();
}

/* a tap the worker hasn't picked up yet (it is waiting for the next frame), then a quick off → on: no photo after
 * the camera is back (both paths) */
static void test_stale_tap(bool presets)
{
	setup(presets, Y_STEADY, 60);
	cam_on(0);
	const int saves = GET(saves), stills = GET(starts_still);
	const unsigned n0 = GET(nexts);	/* (the worker has just started waiting 60 ms for a frame: ≤ 2 ms ago) */
	CHECK(WAIT_FOR(GET(nexts) != n0, 1000), "stale tap: worker never waits for a frame");
	CHECK(cam_dev_capture(), "stale tap: capture refused");	/* (microseconds: well inside that wait) */
	cam_dev_run(false, 0, 64, 64);
	cam_dev_run(true, 0, 64, 64);
	CHECK(WAIT_FOR(state() == CAM_RUNNING, 2000), "stale tap: not running after the bounce");
	usleep(300000);
	CHECK(GET(saves) == saves && GET(starts_still) == stills && result(100) == 0,
	      "stale tap (%s): taken after the camera came back (%d photo(s), %d still(s))",
	      presets ? "still" : "instant", GET(saves) - saves, GET(starts_still) - stills);
	busy_cleared("stale tap");
	CHECK(cam_dev_capture() && result(3000) == 1, "stale tap: next photo not taken");
	busy_cleared("stale tap, next photo");
	cam_off();
}

/* the reviewer's (review6) paths: nothing leaves the shutter busy */
static void test_failures(void)
{
	char why[160];
	/* START(STILL) fails: the camera errors (pre-existing behaviour), busy clears, Try Again works */
	setup(true, Y_STEADY, 10);
	cam_on(0);
	SET(fail_still_start, 1);
	CHECK(cam_dev_capture(), "x1: capture refused");
	busy_cleared("x1: START(STILL) failed");
	CHECK(WAIT_FOR(cam_dev_state(why, sizeof(why), NULL) == CAM_ERROR, 2000), "x1: no error");
	cam_dev_retry();
	CHECK(WAIT_FOR(state() == CAM_RUNNING, 2000), "x1: Try Again didn't restart");
	cam_off();
	/* a source error mid-still: a failed photo (alert), the viewfinder comes back */
	cam_on(0);
	SET(err_still, 1);
	CHECK(cam_dev_capture(), "x2: capture refused");
	CHECK(result(3000) == -1, "x2: no alert for the failed photo");
	busy_cleared("x2: source error mid-still");
	CHECK(WAIT_FOR(state() == CAM_RUNNING, 2000), "x2: viewfinder not back");
	cam_off();
	/* no still frames at all: alert after STILL_FRAME_MS */
	cam_on(0);
	SET(nofr_still, 1);
	double t0 = now_ms();
	CHECK(cam_dev_capture(), "x3: capture refused");
	CHECK(result(4000) == -1, "x3: no alert");
	CHECK(now_ms() - t0 >= 1400, "x3: gave up after %.0f ms", now_ms() - t0);
	SET(nofr_still, 0);
	busy_cleared("x3: no still frames");
	cam_off();
	/* the viewfinder's re-START after the still fails: photo saved, camera errors, busy clears */
	cam_on(0);
	int saves = GET(saves);
	SET(fail_view_start, 1);
	CHECK(cam_dev_capture(), "x4: capture refused");
	busy_cleared("x4: viewfinder re-START failed");
	CHECK(result(3000) == 1 && GET(saves) == saves + 1, "x4: photo lost");
	CHECK(WAIT_FOR(state() == CAM_ERROR, 2000), "x4: no error");
	cam_dev_retry();
	CHECK(WAIT_FOR(state() == CAM_RUNNING, 2000), "x4: Try Again didn't restart");
	cam_off();
	/* off → on bounce mid-still: the photo is saved, the viewfinder comes back */
	setup(true, Y_ALT, 30);
	cam_on(0);
	saves = GET(saves);
	int f0 = GET(still_frames);
	CHECK(cam_dev_capture(), "x5: capture refused");
	CHECK(WAIT_FOR(GET(still_frames) >= f0 + 3, 2000), "x5: the still didn't start");
	cam_dev_run(false, 0, 64, 64);
	cam_dev_run(true, 0, 64, 64);
	CHECK(result(3000) == 1 && GET(saves) == saves + 1, "x5: bounce lost the photo");
	busy_cleared("x5: bounce");
	CHECK(WAIT_FOR(state() == CAM_RUNNING, 2000), "x5: viewfinder not back");
	cam_off();
	/* START(STILL) finds the driver without the flags: that photo at the default size, then the instant path */
	setup(true, Y_STEADY, 10);
	cam_on(0);
	SET(downgrade_on_still, true);
	CHECK(cam_dev_capture(), "x6: capture refused");
	CHECK(result(3000) == 1, "x6: no photo");
	busy_cleared("x6: downgrade");
	CHECK(WAIT_FOR(cam_dev_capture(), 1000), "x6: second photo refused");
	CHECK(!cam_dev_busy(), "x6: busy after the downgrade (should be the instant path)");
	CHECK(result(2000) == 1, "x6: no second photo");
	SET(downgrade_on_still, false);
	cam_off();
}

int main(void)
{
	test_instant();
	test_still();
	test_off_mid_still();
	test_off_cap();
	test_timeout_saves();
	test_flip();
	test_mirror();
	test_focus_restart();
	test_stale_tap(false);
	test_stale_tap(true);
	test_failures();
	CHECK(GET(held) == 0, "%d frame(s) never released", GET(held));
	CHECK(GET(opens) == GET(closes), "%d opens, %d closes", GET(opens), GET(closes));
	if (fails)
		fprintf(stderr, "cam_dev_test: %d failure(s)\n", fails);
	else
		printf("cam_dev_test ok\n");
	return fails != 0;
}
