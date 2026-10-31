/* The camera worker (cam.h): one long-lived thread owns the source (cam_src.c) — opens it, STARTs the camera the UI
 * asked for, waits for frames (poll with a 100 ms timeout, so a change of plan is seen quickly), converts each one
 * into the back buffer of three (cam_conv.c) and gives the ISP buffer straight back; the UI's timer takes the
 * newest one. A sensor faster than the viewfinder (the rear IMX237 preset streams ~120 fps) is paced to ~30 fps (every Nth frame): the
 * frames in between go straight back unconverted. A front photo is saved as the mirrored viewfinder shows it unless
 * Mirror Front Camera is off (cam_dev_mirror; iOS's setting, on by default here). Off = STOP + close (the driver powers the sensor down) and the thread sleeps on a condition variable.
 * A call stuck in the driver only ever stalls this thread: the UI shows "starting" meanwhile.
 * Log: a summary line when the viewfinder stops (frames, fps, convert ms, the thread's CPU), every 5 s with
 * ORCHARD_CAMERA_STATS=1. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cam.h"

#define START_TIMEOUT_MS 5000	/* no first frame by then: an error (Try Again) */
#define VIEW_PERIOD_MS (1000.0 / 30)	/* viewfinder pacing: frames converted per second ≈ 30..52 (see the worker) */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static bool started;	/* the thread */
static struct {
	bool on;
	int camera, dw, dh;
	unsigned gen;		/* bumped by every request: the worker re-plans */
	unsigned offs;		/* bumped whenever the UI turns the camera off: a still under way hurries up */
	enum cam_state state;
	char why[160];
	cam_info_t info;
	uint32_t *buf[3];
	int bw, bh;
	int front, ready, back;
	bool capture;		/* a photo was asked for and isn't taken yet */
	bool stills;		/* the viewfinder's driver has the presets: a photo is a full-resolution still */
	bool still;		/* such a still is under way: from the tap until the viewfinder's next frame (busy) */
	bool mirror;		/* front photos mirrored like the viewfinder (cam_dev_mirror) */
	/* photo result */
	int res;
	char res_path[256], res_why[160];
	uint32_t res_thumb[CAM_THUMB * CAM_THUMB];
	bool res_has_thumb;
	void (*notify)(void);	/* cam_dev_notify */
} S = { .front = -1, .ready = -1, .back = 0, .mirror = true };

static double ms_now(clockid_t c)
{
	struct timespec t;
	clock_gettime(c, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void set_state(enum cam_state st, const char *why)
{
	S.state = st;
	snprintf(S.why, sizeof(S.why), "%s", why ? why : "");
}

static void free_bufs(void)
{
	for (int i = 0; i < 3; i++) {
		free(S.buf[i]);
		S.buf[i] = NULL;
	}
	S.bw = S.bh = 0;
	S.front = S.ready = -1;
	S.back = 0;
}

static int third(int a, int b)
{
	for (int i = 0; i < 3; i++)
		if (i != a && i != b)
			return i;
	return 0;
}

/* ---------- photos: a copy of the frame, saved on a thread of its own ---------- */

typedef struct shot {
	uint8_t *nv12;
	int w, h, stride;
	cam_orient_t o;
} shot_t;

static void *save_main(void *arg)
{
	shot_t *p = arg;
	static pthread_mutex_t num_mu = PTHREAD_MUTEX_INITIALIZER;
	static int next_num;
	pthread_mutex_lock(&num_mu);
	if (!next_num)
		next_num = cam_photos_next_number();
	int num = next_num++;
	pthread_mutex_unlock(&num_mu);
	char path[256] = "", why[160] = "";
	/* the thumbnail first, from the frame itself */
	const int tw = CAM_THUMB, th = CAM_THUMB;
	uint32_t *thumb = malloc(sizeof(uint32_t) * tw * th);
	cam_conv_t c;
	if (thumb && cam_conv_init(&c, p->w, p->h, p->stride, p->o, tw, th)) {
		cam_convert(&c, p->nv12, p->nv12 + (size_t)p->stride * p->h, thumb, tw);
		cam_conv_free(&c);
	} else {
		free(thumb);
		thumb = NULL;
	}
	double t0 = ms_now(CLOCK_MONOTONIC);
	bool ok = cam_save_photo(path, sizeof(path), num, p->nv12, p->w, p->h, p->stride, p->o, why, sizeof(why));
	if (ok && thumb)
		cam_thumb_write(path, thumb);
	fprintf(stderr, "cam: photo %s %s (%.0f ms)%s%s\n", path, ok ? "saved" : "failed", ms_now(CLOCK_MONOTONIC) - t0,
		ok ? "" : ": ", ok ? "" : why);
	pthread_mutex_lock(&mu);
	S.res = ok ? 1 : -1;
	snprintf(S.res_path, sizeof(S.res_path), "%s", path);
	snprintf(S.res_why, sizeof(S.res_why), "%s", why);
	S.res_has_thumb = ok && thumb;
	if (S.res_has_thumb)
		memcpy(S.res_thumb, thumb, sizeof(S.res_thumb));
	pthread_mutex_unlock(&mu);
	free(thumb);
	free(p->nv12);	/* (the frame's copy: 12 MB at 8 MP) */
	free(p);
	return NULL;
}

static void save_frame(const cam_info_t *in, const cam_frame_t *f, cam_orient_t o)
{
	size_t ylen = (size_t)in->stride * in->h;
	shot_t *p = malloc(sizeof(*p));
	uint8_t *copy = p ? malloc(ylen * 3 / 2) : NULL;
	pthread_t t;
	if (copy) {
		memcpy(copy, f->y, ylen);
		memcpy(copy + ylen, f->uv, ylen / 2);
		*p = (shot_t){ copy, in->w, in->h, in->stride, o };
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&t, &a, save_main, p) == 0) {
			pthread_attr_destroy(&a);
			return;
		}
		pthread_attr_destroy(&a);
	}
	free(copy);
	free(p);
	pthread_mutex_lock(&mu);
	S.res = -1;
	snprintf(S.res_why, sizeof(S.res_why), "Out of memory.");
	pthread_mutex_unlock(&mu);
}

/* ---------- the worker ---------- */

typedef struct stats {
	unsigned frames, skipped;	/* converted / given straight back (pacing) */
	double conv_sum, conv_max, t0, cpu0, printed;
} stats_t;

static void stats_reset(stats_t *st)
{
	memset(st, 0, sizeof(*st));
	st->t0 = st->printed = ms_now(CLOCK_MONOTONIC);
	st->cpu0 = ms_now(CLOCK_THREAD_CPUTIME_ID);
}

static void stats_line(const stats_t *st, const char *what, const cam_info_t *in, int dw, int dh)
{
	double wall = ms_now(CLOCK_MONOTONIC) - st->t0, cpu = ms_now(CLOCK_THREAD_CPUTIME_ID) - st->cpu0;
	if (!st->frames || wall <= 0)
		return;
	fprintf(stderr, "cam: %s %dx%d → %dx%d: %u frames, %.1f fps (sensor %.1f fps), convert %.2f ms avg / %.2f max, "
		"camera thread %.0f %% of a core\n", what, in->w, in->h, dw, dh, st->frames, st->frames * 1e3 / wall,
		(st->frames + st->skipped) * 1e3 / wall, st->conv_sum / st->frames, st->conv_max, cpu * 100 / wall);
}

/* mean Y of a sparse grid over the centre (AE settling) */
static int frame_mean(const cam_info_t *in, const cam_frame_t *f)
{
	long sum = 0, n = 0;
	for (int y = in->h / 4; y < in->h * 3 / 4; y += 16)
		for (int x = in->w / 4; x < in->w * 3 / 4; x += 16, n++)
			sum += f->y[(size_t)y * in->stride + x];
	return n ? (int)(sum / n) : 0;
}

/* the camera was turned off (app closed, screen off, locked, switcher, viewer) since offs was read */
static bool turned_off(unsigned offs)
{
	pthread_mutex_lock(&mu);
	bool off = !S.on || S.offs != offs;
	pthread_mutex_unlock(&mu);
	return off;
}

/*
 * The shutter, with a driver that has the presets (cam_src_has_presets; without them a photo is simply the next
 * viewfinder frame): the viewfinder preset is binned (rear 1632x1224); the photo comes from the sensor's
 * full-resolution preset (rear 3264x2448, 8 MP). STOP, START(STILL), let auto-exposure settle: the fw starts the new
 * preset's AE from dark (hw: mean Y 16-19 for the first ~20 full-res frames of a lit scene, 85 at frame 30), so a
 * frame counts only once its brightness is within 2 % of the frame before AND at least 80 % of the viewfinder's last
 * one (view_y; a dark scene, view_y < 28, takes any steady frame). At least STILL_SKIP frames, at most STILL_MAX
 * (then the last one is saved anyway); the caller then re-STARTs the viewfinder, which keeps showing its last frame
 * meanwhile. Once START(STILL) is sent the photo is always finished — the user got the blink for it — even when the
 * camera is turned off meanwhile (Home, lock, switcher, the last-photo thumbnail): then the first bright-enough
 * frame will do (no steadiness: dark start frames are still skipped) and STILL_OFF_MAX caps it, so the sensor
 * stays on a little longer, not until STILL_MAX. A flip changes nothing: that photo is finished first.
 * false = the source failed (why); no photo within the frames is a failed photo, not a failed camera.
 */
#define STILL_SKIP 3
#define STILL_MAX 60
#define STILL_OFF_MAX 40	/* frames, when the camera was turned off during the still (AE: ~20-30 from dark) */
#define STILL_FRAME_MS 1500	/* no frame for this long: no photo */
static bool take_still(cam_src_t *src, int camera, cam_orient_t so, int view_y, unsigned offs, char *why, size_t n)
{
	double t0 = ms_now(CLOCK_MONOTONIC);
	cam_src_stop(src);
	if (!cam_src_start(src, camera, CAM_MODE_STILL, why, n))
		return false;
	cam_info_t si = *cam_src_info(src);
	int prev = -1, k, off_at = -1;	/* off_at: the frame the camera was first seen turned off at */
	bool saved = false;
	for (k = 0; k < STILL_MAX && !saved; k++) {
		cam_frame_t f;
		if (cam_src_next(src, &f, STILL_FRAME_MS, why, n) <= 0)
			break;
		if (off_at < 0 && turned_off(offs))
			off_at = k;
		const int m = frame_mean(&si, &f), last = off_at < 0 ? STILL_MAX - 1 : STILL_OFF_MAX - 1;
		const bool bright = view_y < 28 || m * 5 >= view_y * 4;
		const bool steady = prev >= 0 && abs(m - prev) * 50 <= prev + 1 && bright;
		const bool will_do = off_at >= 0 && view_y >= 28 && bright;	/* (off: no waiting for steady) */
		if (k >= STILL_SKIP && (steady || will_do || k >= last)) {
			so.rot = cam_default_orient(si.w, si.h, camera).rot;
			save_frame(&si, &f, so);
			saved = true;
		}
		prev = m;
		cam_src_release(src, &f);
	}
	char off_note[64] = "";
	if (off_at >= 0)
		snprintf(off_note, sizeof(off_note), " (camera turned off at frame %d: finished anyway)", off_at);
	fprintf(stderr, "cam: still %dx%d after %d frame(s), %.0f ms, mean Y %d (viewfinder %d)%s%s\n", si.w, si.h, k,
		ms_now(CLOCK_MONOTONIC) - t0, prev, view_y, saved ? "" : " (no photo)", off_note);
	if (!saved) {
		pthread_mutex_lock(&mu);
		S.res = -1;
		snprintf(S.res_why, sizeof(S.res_why), "The camera didn't deliver a photo.");
		pthread_mutex_unlock(&mu);
	}
	cam_src_stop(src);
	return true;
}

static void *worker(void *arg)
{
	(void)arg;
	cam_src_t *src = NULL;
	cam_conv_t conv = { 0 };
	bool conv_ok = false, failed = false;
	int live = -1;	/* the camera STARTed, -1 = none */
	unsigned gen = 0;
	double start_at = 0, shown_at = 0;	/* shown_at: the last converted frame's timestamp (pacing) */
	int view_y = 0;	/* mean Y of the last viewfinder frame (the still's exposure target) */
	bool after_still = false;	/* the next viewfinder START follows a photo: focus held again (cam_focus) */
	stats_t st = { 0 };
	cam_info_t in = { 0 };
	const bool stats_env = getenv("ORCHARD_CAMERA_STATS") != NULL;
	char why[160];
	pthread_mutex_lock(&mu);
	for (;;) {
		if (S.gen != gen) {
			gen = S.gen;
			failed = false;
		}
		if (!S.on || failed) {	/* off, or an error waiting for Try Again: let the sensor power down */
			S.capture = S.still = false;
			if (src) {
				pthread_mutex_unlock(&mu);
				if (live >= 0) {
					cam_src_stop(src);
					stats_line(&st, "stopped", &in, conv.dw, conv.dh);
				}
				cam_src_close(src);
				pthread_mutex_lock(&mu);
				src = NULL;
				live = -1;
				after_still = false;	/* (closed: the driver forgot the focus too) */
				continue;	/* (the plan may have changed meanwhile) */
			}
			if (conv_ok)
				cam_conv_free(&conv);
			conv_ok = false;
			if (!S.on) {
				free_bufs();
				set_state(CAM_OFF, NULL);
			}
			pthread_cond_wait(&cv, &mu);
			continue;
		}
		const int want = S.camera, dw = S.dw, dh = S.dh;
		const bool capture = S.capture, mirror = S.mirror;
		const unsigned offs = S.offs;
		pthread_mutex_unlock(&mu);

		if (!src) {
			pthread_mutex_lock(&mu);
			set_state(CAM_STARTING, NULL);
			pthread_mutex_unlock(&mu);
			src = cam_src_open(why, sizeof(why));
			if (!src) {
				pthread_mutex_lock(&mu);
				set_state(CAM_ERROR, why[0] ? why : "The camera couldn't be opened.");
				failed = true;
				continue;
			}
			in = *cam_src_info(src);
		}
		if (live != want) {
			if (live >= 0) {
				cam_src_stop(src);
				stats_line(&st, "switched", &in, conv.dw, conv.dh);
			}
			live = -1;
			pthread_mutex_lock(&mu);
			set_state(CAM_STARTING, NULL);
			S.info = in;
			pthread_mutex_unlock(&mu);
			if (!cam_src_start(src, want, CAM_MODE_VIEW, why, sizeof(why))) {
				pthread_mutex_lock(&mu);
				set_state(CAM_ERROR, why);
				failed = true;
				continue;
			}
			live = want;
			if (after_still)
				cam_focus_restarted(live);
			after_still = false;
			in = *cam_src_info(src);	/* (the size this camera streams) */
			pthread_mutex_lock(&mu);
			S.info = in;
			S.stills = cam_src_has_presets(src);
			pthread_mutex_unlock(&mu);
			start_at = ms_now(CLOCK_MONOTONIC);
			shown_at = 0;
			stats_reset(&st);
		}
		cam_orient_t o = cam_default_orient(in.w, in.h, live);
		if (!conv_ok || conv.dw != dw || conv.dh != dh || conv.o.rot != o.rot || conv.o.mirror != o.mirror ||
		    conv.sw != in.w || conv.sh != in.h || conv.stride != in.stride) {
			if (conv_ok)
				cam_conv_free(&conv);
			conv_ok = cam_conv_init(&conv, in.w, in.h, in.stride, o, dw, dh);
			if (!conv_ok) {
				pthread_mutex_lock(&mu);
				set_state(CAM_ERROR, "The camera's frame size isn't supported.");
				failed = true;
				continue;
			}
		}
		pthread_mutex_lock(&mu);
		if (S.bw != dw || S.bh != dh) {	/* (the UI isn't showing any of them: cam_dev_run's rule) */
			free_bufs();
			for (int i = 0; i < 3; i++)
				S.buf[i] = malloc(sizeof(uint32_t) * dw * dh);
			if (!S.buf[0] || !S.buf[1] || !S.buf[2]) {
				free_bufs();
				set_state(CAM_ERROR, "Not enough memory for the viewfinder.");
				failed = true;
				continue;
			}
			S.bw = dw, S.bh = dh;
		}
		uint32_t *dst = S.buf[S.back];
		pthread_mutex_unlock(&mu);

		if (capture && cam_src_has_presets(src)) {	/* full-resolution photo, then the viewfinder again */
			cam_orient_t so = cam_default_orient(in.w, in.h, live);
			so.mirror = so.mirror && mirror;	/* (the front camera's viewfinder is a mirror) */
			stats_line(&st, "photo", &in, conv.dw, conv.dh);
			bool ok = take_still(src, live, so, view_y, offs, why, sizeof(why));
			pthread_mutex_lock(&mu);
			S.capture = false;	/* (S.still stays until the viewfinder's first frame: the shutter stays grey) */
			if (!ok) {
				set_state(CAM_ERROR, why);
				failed = true;
			}
			live = -1;	/* re-START the viewfinder preset at the top of the loop */
			after_still = ok;
			continue;
		}
		cam_focus_service(src, live, &in, o, dw, dh);	/* tap to focus (cam_focus.c) */
		cam_frame_t f;
		int r = cam_src_next(src, &f, 100, why, sizeof(why));
		if (r <= 0) {
			pthread_mutex_lock(&mu);
			bool late = r == 0 && S.state == CAM_STARTING && ms_now(CLOCK_MONOTONIC) - start_at > START_TIMEOUT_MS;
			if (r < 0 || late) {
				if (late)
					snprintf(why, sizeof(why), "No picture from the camera (%s) after %d s.", in.sensor,
						 START_TIMEOUT_MS / 1000);
				set_state(CAM_ERROR, why);
				failed = true;
			}
			continue;
		}
		double t0 = ms_now(CLOCK_MONOTONIC);
		/* pacing by count, not by gap: the source's frame period (a running average of its timestamps) says how
		 * many frames make one viewfinder frame — every one up to ~52 fps, every 2nd up to ~105, … The old rule (a
		 * frame < 28 ms after the last shown one goes back) dropped a jittery ~31 fps stream's frames at random (24
		 * of 31 shown, each drop a 64 ms hold) and halved a 50 fps one to 25 (a 2-3-2-3 vblank judder) */
		static double period;	/* ms (worker thread only) */
		static int since;	/* frames given back since the last one shown */
		double ft = f.ts_ns ? f.ts_ns / 1e6 : t0;
		if (!shown_at)	/* (a new START) */
			period = 0, since = 0;
		else if (ft > shown_at && ft - shown_at < 500)
			period = period ? period * 0.9 + (ft - shown_at) * 0.1 : ft - shown_at;
		shown_at = ft;
		int every = period > 0 ? (int)(VIEW_PERIOD_MS / period + 0.25) : 1;
		if (!capture && ++since < every) {	/* faster than the viewfinder needs */
			cam_src_release(src, &f);
			st.skipped++;
			pthread_mutex_lock(&mu);
			continue;
		}
		since = 0;
		cam_convert(&conv, f.y, f.uv, dst, dw);
		view_y = frame_mean(&in, &f);	/* the still's exposure target */
		double ms = ms_now(CLOCK_MONOTONIC) - t0;
		if (capture) {	/* front: as the viewfinder shows it, or un-mirrored (Mirror Front Camera off) */
			cam_orient_t so = o;
			so.mirror = o.mirror && mirror;
			save_frame(&in, &f, so);
		}
		cam_src_release(src, &f);
		st.frames++;
		st.conv_sum += ms;
		st.conv_max = ms > st.conv_max ? ms : st.conv_max;
		if (stats_env && t0 - st.printed > 5000) {
			stats_line(&st, "running", &in, dw, dh);
			st.printed = t0;
		}
		pthread_mutex_lock(&mu);
		if (capture)
			S.capture = false;
		if (S.bw == dw && S.bh == dh && S.buf[S.back] == dst) {
			if (!S.capture)
				S.still = false;	/* the viewfinder is back after a still: the shutter takes taps again */
			if (S.ready >= 0) {	/* the UI hasn't taken the last one: it is dropped */
				int t = S.ready;
				S.ready = S.back;
				S.back = t;
			} else {
				S.ready = S.back;
				S.back = third(S.front, S.ready);
			}
			if (S.state != CAM_RUNNING)
				set_state(CAM_RUNNING, NULL);
			if (S.notify)	/* the UI picks it up now, not at its timer's next tick */
				S.notify();
		}
	}
	return NULL;
}

/* ---------- the UI's side (never blocks: the worker takes the lock only around bookkeeping) ---------- */

void cam_dev_run(bool on, int camera, int dw, int dh)
{
	pthread_mutex_lock(&mu);
	if (on != S.on || (on && (camera != S.camera || dw != S.dw || dh != S.dh))) {
		S.on = on;
		if (on)
			S.camera = camera, S.dw = dw, S.dh = dh;
		S.gen++;
		if (!S.on)	/* the UI drops what it shows now; nothing of it is handed out again. A tap the worker
				 * hasn't picked up yet is dropped (not taken on the next run); a still under way finishes */
			S.front = S.ready = -1, S.offs++, S.capture = false;
		else	/* (not the last run's error or state while the worker catches up) */
			set_state(CAM_STARTING, NULL);
		if (on && !started) {
			pthread_t t;
			pthread_attr_t a;
			pthread_attr_init(&a);
			pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
			started = pthread_create(&t, &a, worker, NULL) == 0;
			pthread_attr_destroy(&a);
			if (!started)
				set_state(CAM_ERROR, "Couldn't start the camera thread.");
		}
		pthread_cond_signal(&cv);
	}
	pthread_mutex_unlock(&mu);
}

void cam_dev_retry(void)
{
	pthread_mutex_lock(&mu);
	S.gen++;
	if (S.state == CAM_ERROR)
		set_state(CAM_STARTING, NULL);
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
}

enum cam_state cam_dev_state(char *why, size_t n, cam_info_t *info)
{
	pthread_mutex_lock(&mu);
	enum cam_state st = S.state;
	if (why && n)
		snprintf(why, n, "%s", S.why);
	if (info)
		*info = S.info;
	pthread_mutex_unlock(&mu);
	return st;
}

const uint32_t *cam_dev_frame(void)
{
	const uint32_t *p = NULL;
	pthread_mutex_lock(&mu);
	if (S.on && S.ready >= 0 && S.buf[S.ready]) {
		S.front = S.ready;
		S.ready = -1;
		p = S.buf[S.front];
	}
	pthread_mutex_unlock(&mu);
	return p;
}

bool cam_dev_capture(void)
{
	pthread_mutex_lock(&mu);
	const bool ok = S.on && S.state == CAM_RUNNING && !S.capture && !S.still;
	if (ok)
		S.capture = true, S.still = S.stills;
	pthread_mutex_unlock(&mu);
	return ok;
}

bool cam_dev_busy(void)
{
	pthread_mutex_lock(&mu);
	const bool busy = S.on && S.still;
	pthread_mutex_unlock(&mu);
	return busy;
}

int cam_dev_capture_result(char *path, size_t pn, uint32_t *thumb, char *why, size_t wn)
{
	pthread_mutex_lock(&mu);
	int r = S.res;
	if (r) {
		snprintf(path, pn, "%s", S.res_path);
		snprintf(why, wn, "%s", S.res_why);
		if (thumb && r > 0) {
			if (S.res_has_thumb)
				memcpy(thumb, S.res_thumb, sizeof(S.res_thumb));
			else
				memset(thumb, 0, sizeof(S.res_thumb));
		}
		S.res = 0;
	}
	pthread_mutex_unlock(&mu);
	return r;
}

void cam_dev_mirror(bool on)
{
	pthread_mutex_lock(&mu);
	S.mirror = on;
	pthread_mutex_unlock(&mu);
}

void cam_dev_notify(void (*fn)(void))
{
	pthread_mutex_lock(&mu);
	S.notify = fn;
	pthread_mutex_unlock(&mu);
}
