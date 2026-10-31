/* Headless backend for scripted screenshots (no window, simulated clock).
 * ./orchard-snap CMD... where CMD is one of:
 *   open APP | tap X Y | drag X1 Y1 X2 Y2 | wait MS | key home|power | hold home|power MS |
 *   type TEXT | press bksp|enter | touch X Y (finger down / moves there) | lift |
 *   notify TITLE BODY | shot FILE.ppm | bench N (redraw the whole screen N times, print ms each) |
 *   spinner (an endless animation on the overlay) |
 *   idle MS (run MS of simulated time the way the phone's main loop does — sleep until the next timer, capped at
 *   1 s, 60 s while dark — and print its wake-ups, frames, the share of the screen drawn and each timer that ran) */
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "orchard.h"
#include "src/misc/lv_timer_private.h"
#include "src/display/lv_display_private.h"

static uint32_t now_ms;
static uint32_t *fb;

static uint32_t tick(void) { return now_ms; }

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
	(void)a;
	(void)px;
	lv_display_flush_ready(d);
}

lv_display_t *hal_init(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	lv_tick_set_cb(tick);
	lv_display_t *d = lv_display_create(SCR_W, SCR_H);
	lv_display_set_color_format(d, LV_COLOR_FORMAT_XRGB8888);
	fb = malloc(SCR_W * SCR_H * 4);
	lv_display_set_buffers(d, fb, NULL, SCR_W * SCR_H * 4, LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(d, flush);
	return d;
}

static void run(int ms)
{
	for (int t = 0; t <= ms; t += 16) {
		now_ms += 16;
		usleep(1500);	/* let the terminal's child process produce output */
		lv_timer_handler();
	}
}

/* listen N: N test lock listeners that print what they're told (lock_listen: added while locked = told at once) */
#define LCB(n) static void lcb##n(bool l) { printf("listener %d: %s\n", n, l ? "locked" : "unlocked"); }
LCB(0) LCB(1) LCB(2) LCB(3) LCB(4) LCB(5) LCB(6) LCB(7) LCB(8) LCB(9)
static void (*const lcbs[])(bool) = { lcb0, lcb1, lcb2, lcb3, lcb4, lcb5, lcb6, lcb7, lcb8, lcb9 };

/* idle MS: the phone's loop (hal_phone.c hal_run) on the simulated clock; which timers wake it and what gets drawn */
static unsigned idle_frames;
static uint64_t idle_px;

static void idle_refr(lv_event_t *e)
{
	lv_display_t *d = lv_event_get_target(e);
	static int verbose = -1;
	if (verbose < 0)
		verbose = getenv("SNAP_INV") != NULL;	/* every redrawn area */
	for (int i = 0; i < d->inv_p; i++)
		if (!d->inv_area_joined[i]) {
			idle_px += lv_area_get_size(&d->inv_areas[i]);
			if (verbose)
				printf("  frame %u: %d,%d - %d,%d\n", idle_frames, (int)d->inv_areas[i].x1, (int)d->inv_areas[i].y1,
				       (int)d->inv_areas[i].x2, (int)d->inv_areas[i].y2);
		}
	idle_frames++;
}

extern char __executable_start;

static void idle_run(int ms)
{
	static bool hooked;
	if (!hooked) {
		hooked = true;
		lv_display_add_event_cb(lv_display_get_default(), idle_refr, LV_EVENT_REFR_START, NULL);
	}
	struct { lv_timer_cb_t cb; unsigned runs; uint32_t period; } tl[64];
	int ntl = 0;
	unsigned wakes = 0, f0 = idle_frames;
	uint64_t px0 = idle_px;
	uint32_t end = now_ms + ms;
	while ((int32_t)(end - now_ms) > 0) {
		lv_timer_t *ts[128];
		uint32_t last[128];
		int n = 0;
		for (lv_timer_t *t = lv_timer_get_next(NULL); t && n < 128; t = lv_timer_get_next(t))
			ts[n] = t, last[n++] = t->last_run;
		uint32_t idle = lv_timer_handler();
		wakes++;
		for (int k = 0; k < n; k++) {	/* the ones that ran (and still exist) */
			bool live = false;
			for (lv_timer_t *t = lv_timer_get_next(NULL); t && !live; t = lv_timer_get_next(t))
				live = t == ts[k];
			if (!live || ts[k]->last_run == last[k])
				continue;
			int j = 0;
			while (j < ntl && tl[j].cb != ts[k]->timer_cb)
				j++;
			if (j == ntl && ntl < 64)
				tl[ntl++] = (typeof(tl[0])){ ts[k]->timer_cb, 0, 0 };
			if (j < 64)
				tl[j].runs++, tl[j].period = ts[k]->period;
		}
		uint32_t cap = power_dark() ? 60000 : 1000, step = idle > cap ? cap : idle;
		now_ms += step ? step : 1;
	}
	printf("idle %d ms%s: %u wake-ups (%.1f/s), %u frames, %.1f%% of a screen drawn\n", ms, power_dark() ? " (dark)" : "",
	       wakes, wakes * 1000.0 / ms, idle_frames - f0, (idle_px - px0) * 100.0 / (SCR_W * SCR_H));
	for (int j = 0; j < ntl; j++)	/* addr2line -f -e orchard-snap 0xOFFSET */
		printf("  timer 0x%lx: %u runs, period %u ms\n", (unsigned long)((char *)(uintptr_t)tl[j].cb - &__executable_start),
		       tl[j].runs, tl[j].period);
}

static void shot(const char *path)
{
	lv_refr_now(NULL);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", SCR_W, SCR_H);
	for (int i = 0; i < SCR_W * SCR_H; i++) {
		unsigned char rgb[3] = { fb[i] >> 16, fb[i] >> 8, fb[i] };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

void hal_run(int argc, char **argv)
{
	run(100);
	for (int i = 1; i < argc; i++) {
		const char *c = argv[i];
		if (!strcmp(c, "tap") && i + 2 < argc) {
			int x = atoi(argv[++i]), y = atoi(argv[++i]);
			input_feed(x, y, true);
			run(80);
			input_feed(x, y, false);
			run(50);
		} else if (!strcmp(c, "drag") && i + 4 < argc) {
			int x1 = atoi(argv[i + 1]), y1 = atoi(argv[i + 2]), x2 = atoi(argv[i + 3]), y2 = atoi(argv[i + 4]);
			i += 4;
			for (int s = 0; s <= 10; s++) {
				input_feed(x1 + (x2 - x1) * s / 10, y1 + (y2 - y1) * s / 10, true);
				run(20);
			}
			input_feed(x2, y2, false);
			run(50);
		} else if (!strcmp(c, "touch") && i + 2 < argc) {	/* step through a drag frame by frame */
			int x = atoi(argv[++i]), y = atoi(argv[++i]);
			input_feed(x, y, true);
			run(0);
		} else if (!strcmp(c, "lift")) {
			input_feed(0, 0, false);
			run(0);
		} else if (!strcmp(c, "wait") && i + 1 < argc) {
			run(atoi(argv[++i]));
		} else if (!strcmp(c, "hold") && i + 2 < argc) {	/* hold home|power MS */
			int code = !strcmp(argv[++i], "home") ? KEY_HOMEPAGE : KEY_POWER;
			input_key(code, true);
			run(atoi(argv[++i]));
			input_key(code, false);
			run(50);
		} else if (!strcmp(c, "key") && i + 1 < argc) {
			int code = !strcmp(argv[++i], "home") ? KEY_HOMEPAGE : KEY_POWER;
			input_key(code, true);
			input_key(code, false);
			run(50);
		} else if (!strcmp(c, "open") && i + 1 < argc) {
			static const app_t *apps[] = { &app_terminal, &app_settings, &app_calc, &app_music,
						       &app_clock, &app_notes, &app_files,
						       &app_web, &app_weather,
						       &app_camera, &app_memos };
			const char *name = argv[++i];
			for (unsigned k = 0; k < sizeof(apps) / sizeof(apps[0]); k++)
				if (!strcasecmp(apps[k]->name, name))
					shell_launch(apps[k]);
			run(400);
		} else if (!strcmp(c, "type") && i + 1 < argc) {
			kbd_type(argv[++i]);
			run(50);
		} else if ((!strcmp(c, "press") && i + 1 < argc) || !strcmp(c, "enter")) {
			const char *k = !strcmp(c, "enter") ? "enter" : argv[++i];
			kbd_press(!strcmp(k, "bksp") ? TK_BKSP : !strcmp(k, "tab") ? TK_TAB : TK_ENTER);
			run(50);
		} else if (!strcmp(c, "notify") && i + 2 < argc) {
			shell_banner(&app_clock, argv[i + 1], argv[i + 2]);
			i += 2;
			run(50);
		} else if (!strcmp(c, "bench") && i + 1 < argc) {	/* render speed (real time, not simulated) */
			int n = atoi(argv[++i]);
			struct timespec t0, t1;
			clock_gettime(CLOCK_MONOTONIC, &t0);
			for (int k = 0; k < n; k++) {
				lv_obj_invalidate(lv_screen_active());
				lv_refr_now(NULL);
			}
			clock_gettime(CLOCK_MONOTONIC, &t1);
			printf("bench: %.2f ms per full-screen frame\n",
			       ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / (n > 0 ? n : 1));
		} else if (!strcmp(c, "spinner")) {	/* an endless animation (power tests: frozen while dark) */
			lv_obj_center(ui_spinner(shell_overlay(), 0, 0));
			run(50);
		} else if (!strcmp(c, "listen") && i + 1 < argc) {
			for (int k = 0, n = atoi(argv[++i]); k < n && k < 10; k++)
				lock_listen(lcbs[k], true);
		} else if (!strcmp(c, "appspinner")) {	/* one in the app, below the lock screen (spinner made while locked) */
			lv_obj_center(ui_spinner(lv_screen_active(), 0, 0));
			run(50);
		} else if (!strcmp(c, "idle") && i + 1 < argc) {
			idle_run(atoi(argv[++i]));
		} else if (!strcmp(c, "shot") && i + 1 < argc) {
			shot(argv[++i]);
		}
	}
}

void hal_haptic(int ms) { (void)ms; }
void hal_damage(const lv_area_t *a) { (void)a; }
void hal_screen(bool on) { (void)on; }
bool hal_display(bool on) { (void)on; return true; }
void hal_wake_set(void (*cb)(void)) { (void)cb; }
void hal_wake(void) {}
