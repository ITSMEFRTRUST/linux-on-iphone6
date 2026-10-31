/* Power: what the shell does so the battery lasts.
 *
 * Screen off ("dark"). shell.c turns the backlight off and puts the black blackout up; GRACE_MS later — the black
 * frame is on the panel, every animation has landed — the UI goes to sleep: LVGL's
 * invalidation is off, the refresh + animation timers stay paused (spinners freeze), and every LVGL timer is
 * paused except the background ones
 * registered with power_keep() (alarms, music playback, Bluetooth audio routing, Wi-Fi, network jobs,
 * the Terminal's session), which drop to their dark period. hal_phone.c's main loop then sleeps in poll()
 * until the next of those is due or a key comes; touch isn't read at all (hal_screen). Wake (Power / Home key, an
 * alarm) = the paused timers resume, one full redraw. mpg123 is its own process: music keeps playing.
 * Once asleep the display pipe goes off (hal_display: adp-t7000's DPMS, ~25 ms; on again ≈ 250 ms before the wake
 * redraw; a kernel without it keeps the pipe on, its vblank IRQ masked after drm's offdelay), the touch panel is
 * inhibited from the moment the screen goes off (hal_screen), the CPU tops out at DARK_KHZ and Wi-Fi listens to
 * every 3rd DTIM beacon (dtim_dark).
 *
 * Auto-lock: pref autolock (s, 0 = never, default 30). DIM_MS before it the backlight drops to a third; a touch or
 * key brings it back. Woken but still on the Lock Screen: off after LOCKSCREEN_S. Something that isn't idle
 * (a ringing alarm) calls lv_display_trigger_activity(); a lasting reason is held with power_keep_awake() /
 * power_awake_for(): a playing video (lease refreshed every second; released on pause / end, and the
 * countdown starts then), Terminal output in front, the stopwatch / timer on screen while charging. Typing: the
 * 30 s after the last key while the keyboard is up (kbd.c). Music playing keeps nothing awake (iOS). A touch on a dimmed screen only brightens it (not delivered).
 *
 * CPU: schedutil, with the minimum raised to the top while a finger is down and BOOST_MS after the last touch or
 * key (and while a video plays: power_busy) — what "performance while the screen is on" was for (taps and
 * scrolls never start on a 300 MHz core) without idling at 1.4 GHz the rest of the time. Screen off: plain
 * schedutil. ORCHARD_CPU=perf = the old way (performance while on).
 *
 * Low Power Mode (pref lowpower, Settings › Battery + Control Center): CPU capped at LPM_KHZ, Wi-Fi power save on,
 * the charger checked every 10 s instead of 2 s, Wi-Fi's signal every 30 s, Auto-Lock 30 s, the home clock
 * without its second hand, a yellow battery. Like iOS it turns itself off once the charger has taken the battery
 * to 80 %, and the 20 % alert offers it.
 *
 * Usage since unplugged (Settings › Battery): screen on / off time, % used, average drain → time left.
 *
 * Wi-Fi Sleep (prefs wifisleep + wifisleep_min, Settings › Battery): Wi-Fi asleep once the screen has been dark that
 * long and nothing needs the network, back at once when the screen comes on (see "Wi-Fi Sleep" below).
 *
 * ORCHARD_POWERLOG=1: screen changes (+ what was paused / kept) and once a minute the main loop's wake-ups,
 * frames, share of the screen drawn, Orchard's own CPU time and the battery, in /tmp/orchard.log. */
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <linux/input-event-codes.h>
#include "orchard.h"
#include "net.h"
#include "power_calc.h"
#include "src/misc/lv_timer_private.h"
#include "src/display/lv_display_private.h"

#define GRACE_MS 1200		/* screen off → UI asleep */
#define DIM_MS 5000		/* dimmed this long before auto-lock */
#define LOCKSCREEN_S 10		/* the Lock Screen alone (woken, not unlocked) goes off after this */
#define BOOST_MS 1000		/* CPU at its top this long after the last touch / key */
#define DARK_KHZ 600000		/* CPU cap while the screen is off (music decoding needs far less) */
#define LPM_KHZ 840000		/* CPU cap in Low Power Mode */
#define LPM_OFF_PCT 80		/* Low Power Mode ends at this charge (on the charger) */
#define MAX_KEEP 16
#define MAX_SWEPT 256

unsigned power_wakeups;

static bool off, dark, logging, dimmed;
static int dim_from;	/* the backlight before dimming */
static bool boosted, perf_mode, lpm;
static uint32_t boost_end;
static lv_timer_t *boost_t, *disp_t;
static bool display_ok = true;	/* the pipe is on (DPMS on worked) */
static int backlight_pending;	/* level waiting for the pipe (a failed DPMS on) */
static lv_timer_t *grace;
static struct keep { lv_timer_t *t; uint32_t dark_ms, on_ms; const char *name; } keeps[MAX_KEEP];
static int nkeeps;
/* paused by us while dark (pointers only compared, never followed blindly; with the callback + user data, so a timer
 * freed and made again at the same address meanwhile isn't taken for it) */
static struct swept { lv_timer_t *t; lv_timer_cb_t cb; void *ud; } swept[MAX_SWEPT];
static int nswept;
static uint32_t off_at, dark_at, last_key_at;
static int last_key;
static unsigned frames;
static uint64_t drawn_px;
static void ws_wake(const char *why);

/* ---------- log ---------- */

/* screen transitions, always in orchard.log (a few lines per press), stamped like hal_phone.c's display lines */
static void tlog(const char *what)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	fprintf(stderr, "orchard: %s @%lld.%03ld\n", what, (long long)t.tv_sec, t.tv_nsec / 1000000);
}

static void plog(const char *fmt, ...)
{
	if (!logging)
		return;
	va_list ap;
	va_start(ap, fmt);
	uint32_t t = lv_tick_get();
	printf("power: [%u.%01u] ", t / 1000, t % 1000 / 100);	/* (uptime-ish: Orchard's tick, s) */
	vprintf(fmt, ap);
	printf("\n");
	fflush(stdout);
	va_end(ap);
}

static long self_cpu_ms(void)	/* Orchard's own user + system time */
{
	FILE *f = fopen("/proc/self/stat", "r");
	char buf[512];
	long ut = 0, st = 0;
	if (f && fgets(buf, sizeof(buf), f)) {
		char *p = strrchr(buf, ')');
		/* after "pid (comm)": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
		if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &ut, &st) != 2)
			ut = st = 0;
	}
	if (f)
		fclose(f);
	long hz = sysconf(_SC_CLK_TCK);
	return (ut + st) * 1000 / (hz > 0 ? hz : 100);
}

static void minute_log(void)
{
	static uint32_t at;
	static unsigned w0, f0;
	static uint64_t px0;
	static long cpu0;
	uint32_t now = lv_tick_get();
	if (!logging || (at && lv_tick_elaps(at) < 60000))
		return;
	long cpu = self_cpu_ms();
	if (at) {
		uint32_t ms = now - at;
		plog("%s %u s: %u wake-ups (%.1f/s), %u frames, %llu%% of a screen drawn, cpu %ld ms (%.2f%%), battery %d%%%s",
		     dark ? "dark" : off ? "off" : "on", ms / 1000, power_wakeups - w0, (power_wakeups - w0) * 1000.0 / ms,
		     frames - f0, (unsigned long long)((drawn_px - px0) * 100 / (SCR_W * SCR_H)), cpu - cpu0,
		     (cpu - cpu0) * 100.0 / ms, sys_battery_pct(), sys_charger_online() ? " (charger)" : "");
	}
	at = now;
	w0 = power_wakeups, f0 = frames, px0 = drawn_px, cpu0 = cpu;
}

/* frames + area drawn (what LVGL is about to redraw) */
static void refr_start(lv_event_t *e)
{
	lv_display_t *d = lv_event_get_target(e);
	for (int i = 0; i < d->inv_p; i++)
		if (!d->inv_area_joined[i])
			drawn_px += lv_area_get_size(&d->inv_areas[i]);
	frames++;
}

/* ---------- CPU ---------- */

static void cpu_apply(void)
{
	if (perf_mode && !off && !lpm)
		sys_cpufreq(true, 0, 0);
	else
		sys_cpufreq(false, boosted && !off ? -1 : 0, off ? DARK_KHZ : lpm ? LPM_KHZ : 0);
}

static void boost_for(uint32_t ms)
{
	if (off || perf_mode || !boost_t)
		return;
	uint32_t now = lv_tick_get();
	if (!boosted || (int32_t)(now + ms - boost_end) > 0)
		boost_end = now + ms;
	if (!boosted) {
		boosted = true;
		cpu_apply();
	}
	lv_timer_set_period(boost_t, boost_end - now);
	lv_timer_reset(boost_t);
	lv_timer_resume(boost_t);
}

static void boost_cb(lv_timer_t *t)
{
	int32_t left = (int32_t)(boost_end - lv_tick_get());
	if (left > 0 && !off) {
		lv_timer_set_period(t, left);
		return;
	}
	lv_timer_pause(t);
	boosted = false;
	cpu_apply();
}

void power_busy(void) { boost_for(2 * BOOST_MS); }

/* how often the sampler thread reads the charger / gauge (s) */
static void pace(void) { sys_sampler_pace(dark ? 60 : lpm ? 10 : 2, dark || lpm ? 60 : 30, false); }

/* ---------- Wi-Fi beacon listening ---------- */

/* While dark the radio wakes for every 3rd DTIM beacon instead of every one (brcmfmac's runtime bcn_li_dtim, from
 * the n61-power kernel; missing on older kernels): fewer radio wake-ups, broadcasts / incoming pings a bit later.
 * The value it had comes back on wake. */
#define BCN_LI_DTIM "/sys/module/brcmfmac/parameters/bcn_li_dtim"
#define DARK_DTIM "3"

static void dtim_dark(bool on)
{
#ifdef ORCHARD_PHONE
	static char dtim_saved[16];
	if (on) {
		FILE *f = fopen(BCN_LI_DTIM, "r");
		if (!f)
			return;
		if (!fgets(dtim_saved, sizeof(dtim_saved), f))
			dtim_saved[0] = 0;
		fclose(f);
		dtim_saved[strcspn(dtim_saved, "\n")] = 0;
	} else if (!dtim_saved[0]) {
		return;
	}
	FILE *f = fopen(BCN_LI_DTIM, "w");
	if (f) {
		fputs(on ? DARK_DTIM : dtim_saved, f);
		if (fclose(f))
			plog("bcn_li_dtim: write failed");
		else
			plog("bcn_li_dtim %s", on ? DARK_DTIM " (dark)" : dtim_saved);
	}
	if (!on)
		dtim_saved[0] = 0;
#else
	(void)on;
#endif
}

/* ---------- Low Power Mode ---------- */

/* Wi-Fi power save (brcmfmac PM_FAST: the radio dozes between beacons); usually on already (wifiup sets it, the
 * kernel's CFG80211_DEFAULT_PS asks for it), Low Power Mode makes sure. iw runs detached (double fork). */
static void wifi_powersave(void)
{
#ifdef ORCHARD_PHONE
	if (access("/usr/sbin/iw", X_OK))
		return;
	pid_t p = fork();
	if (p == 0) {
		if (fork() == 0) {
			execl("/usr/sbin/iw", "iw", "dev", "wlan0", "set", "power_save", "on", (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	if (p > 0)
		waitpid(p, NULL, 0);
#endif
}

bool power_lpm(void) { return lpm; }

void power_set_lpm(bool on)
{
	if (on == lpm)
		return;
	lpm = on;
	prefs_set_int("lowpower", on);
	cpu_apply();
	pace();
	if (on)
		wifi_powersave();
	status_refresh();	/* the yellow battery */
	cc_tick();
	home_tick();	/* the clock's second hand */
	plog("Low Power Mode %s", on ? "on" : "off");
}

/* ---------- usage since unplugged ---------- */

static struct { bool valid; uint32_t last; long on_ms, off_ms; int pct0; } use;

static void usage_tick(void)
{
	uint32_t now = lv_tick_get();
	int pct = sys_battery_pct();
	if (sys_charger_online() || pct < 0) {
		use.valid = false;
		return;
	}
	if (!use.valid) {
		use = (typeof(use)){ .valid = true, .last = now, .pct0 = pct };
		return;
	}
	uint32_t dt = now - use.last;
	use.last = now;
	if (off)
		use.off_ms += dt;
	else
		use.on_ms += dt;
	if (pct > use.pct0)	/* the gauge settled upwards after unplugging */
		use.pct0 = pct;
}

void power_usage(power_usage_t *u)
{
	usage_tick();
	int pct = sys_battery_pct();
	*u = (power_usage_t){ .valid = use.valid, .pct = pct };
	if (!use.valid)
		return;
	u->on_secs = use.on_ms / 1000;
	u->off_secs = use.off_ms / 1000;
	u->secs = u->on_secs + u->off_secs;
	u->used_pct = use.pct0 - pct;
	u->rate_x10 = power_rate_x10(u->used_pct, u->secs);
}

/* ---------- the UI asleep ---------- */

static bool alive(lv_timer_t *t)
{
	for (lv_timer_t *x = lv_timer_get_next(NULL); x; x = lv_timer_get_next(x))
		if (x == t)
			return true;
	return false;
}

static struct keep *kept(lv_timer_t *t)
{
	for (int i = 0; i < nkeeps; i++)
		if (keeps[i].t == t)
			return &keeps[i];
	return NULL;
}

void power_keep(lv_timer_t *t, uint32_t dark_ms, const char *name)
{
	if (!t || kept(t) || nkeeps >= MAX_KEEP)
		return;
	keeps[nkeeps++] = (struct keep){ t, dark_ms, t->period, name };
	if (dark && dark_ms) {	/* registered while dark (a timer made lazily) */
		keeps[nkeeps - 1].on_ms = t->period;
		lv_timer_set_period(t, dark_ms);
	}
}

/* pause every running timer that isn't kept (again every dark tick: timers made meanwhile) */
static int sweep(void)
{
	lv_timer_t *anim = lv_anim_get_timer();	/* pauses itself with no animation left */
	int n = 0;
	for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) {
		if (t == anim || t == grace || t == boost_t || t == disp_t || kept(t) || lv_timer_get_paused(t) || nswept >= MAX_SWEPT)
			continue;
		lv_timer_pause(t);
		swept[nswept++] = (struct swept){ t, t->timer_cb, t->user_data };
		n++;
	}
	return n;
}

/* LVGL resumes the animation timer whenever an animation starts (a spinner is endless): while dark it stays
 * paused — this hook runs on every lv_timer_resume / set_period */
static void timer_resumed(void *ud)
{
	(void)ud;
	lv_timer_t *anim = lv_anim_get_timer();
	if (dark && anim && !lv_timer_get_paused(anim))
		lv_timer_pause(anim);
}

/* LVGL resumes the refresh timer on every layout change: while dark it stays paused */
static void refr_request(lv_event_t *e)
{
	(void)e;
	if (dark)
		lv_timer_pause(lv_display_get_refr_timer(NULL));
}

static void sleep_ui(void)
{
	dark = true;
	dark_at = lv_tick_get();
	lv_display_enable_invalidation(NULL, false);
	lv_timer_pause(lv_display_get_refr_timer(NULL));
	lv_timer_pause(lv_anim_get_timer());	/* animations freeze where they are (they landed in the grace; spinners don't) */
	int n = sweep();
	hal_display(false);
	dtim_dark(true);
	char buf[400];
	int len = 0;
	for (int i = 0; i < nkeeps; i++) {
		struct keep *k = &keeps[i];
		if (!alive(k->t))
			continue;
		k->on_ms = k->t->period;
		if (k->dark_ms)
			lv_timer_set_period(k->t, k->dark_ms);
		if (len < (int)sizeof(buf) - 40)
			len += snprintf(buf + len, sizeof(buf) - len, " %s=%u%s", k->name, k->t->period,
					lv_timer_get_paused(k->t) ? "(idle)" : "");
	}
	buf[len] = 0;
	pace();
	plog("dark: UI asleep, %d timers paused; kept (ms):%s", n, buf);
}

/* DPMS on failed at wake: try again every 500 ms; the backlight stays off until it works (never lit over a black,
 * frozen pipe). Capped: after DISP_FAST_MS (≈ 5 of hal_display's off + on cycles) once every DISP_SLOW_MS, logged
 * once; the next wake (Power) starts fast again. */
#define DISP_FAST_MS 10000
#define DISP_SLOW_MS 30000
static uint32_t disp_since;	/* first failed on of this wake */
static bool disp_slow;

static void disp_retry(lv_timer_t *t);

static void disp_retry_start(void)
{
	if (!disp_t)
		disp_t = lv_timer_create(disp_retry, 500, NULL);
	disp_since = lv_tick_get();
	disp_slow = false;
	lv_timer_set_period(disp_t, 500);
	lv_timer_resume(disp_t);
	lv_timer_reset(disp_t);
}

static void disp_retry(lv_timer_t *t)
{
	if (off) {
		lv_timer_pause(t);
		return;
	}
	if (!(display_ok = hal_display(true))) {
		if (!disp_slow && lv_tick_elaps(disp_since) >= DISP_FAST_MS) {
			disp_slow = true;
			lv_timer_set_period(t, DISP_SLOW_MS);
			fprintf(stderr, "orchard: display still not on after %d s of retries: now one every %d s (Power "
				"off + on starts over)\n", DISP_FAST_MS / 1000, DISP_SLOW_MS / 1000);
		}
		return;
	}
	lv_timer_pause(t);
	plog("display back on after a retry");
	lv_obj_invalidate(lv_screen_active());
	if (backlight_pending)
		sys_backlight_set(backlight_pending);
	backlight_pending = 0;
}

void power_backlight_on(int level)
{
	if (display_ok) {
		sys_backlight_set(level);
		return;
	}
	backlight_pending = level;
}

static void wake_ui(void)
{
	display_ok = hal_display(true);	/* (blocks ≈ 250 ms: the pipe + panel come back) */
	if (!display_ok) {
		plog("display didn't come on: retrying, backlight held off");
		disp_retry_start();
	}
	dark = false;
	int n = 0;
	for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
		for (int i = 0; i < nswept; i++)
			if (swept[i].t == t && swept[i].cb == t->timer_cb && swept[i].ud == t->user_data) {
				lv_timer_resume(t);
				swept[i].t = NULL;	/* listed twice (freed + made again meanwhile): resumed once */
				n++;
				break;
			}
	nswept = 0;
	/* kept timers: the period they had back (dark_ms ones), and all run on the next pass, so the ones that pace
	 * themselves (Terminal, Wi-Fi, clock, music, Bluetooth) pick their screen-on pace at once */
	for (int i = 0; i < nkeeps; i++) {
		if (!alive(keeps[i].t))
			continue;
		if (keeps[i].dark_ms)
			lv_timer_set_period(keeps[i].t, keeps[i].on_ms);
		lv_timer_ready(keeps[i].t);
	}
	dtim_dark(false);
	if (lv_anim_count_running())
		lv_timer_resume(lv_anim_get_timer());
	lv_display_enable_invalidation(NULL, true);
	lv_obj_invalidate(lv_screen_active());	/* the whole display: nothing was invalidated while dark */
	lv_timer_resume(lv_display_get_refr_timer(NULL));
	lv_timer_ready(lv_display_get_refr_timer(NULL));
	plog("wake: %u s dark, %d timers resumed", lv_tick_elaps(dark_at) / 1000, n);
}

static void grace_cb(lv_timer_t *t)
{
	(void)t;
	grace = NULL;	/* one-shot: deleted after this */
	if (off && !dark)
		sleep_ui();
}

void power_screen(bool o)
{
	if (o == off)
		return;
	usage_tick();	/* the time so far counts for the old state */
	off = o;
	if (o)	/* a finger still down at screen-off: its release (at wake) must not be a click */
		for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
			if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER && lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED)
				lv_indev_wait_release(i);
	hal_screen(!o);
	if (o) {
		off_at = lv_tick_get();
		backlight_pending = 0;
		boosted = false;
		if (boost_t)
			lv_timer_pause(boost_t);
		cpu_apply();
		if (!grace) {
			grace = lv_timer_create(grace_cb, GRACE_MS, NULL);
			lv_timer_set_repeat_count(grace, 1);
		}
		plog("screen off (%s)", last_key == KEY_POWER && lv_tick_elaps(last_key_at) < 300 ? "power key" : "auto-lock / Lock button");
		tlog("screen off");
	} else {
		if (grace) {
			lv_timer_delete(grace);
			grace = NULL;
		}
		ws_wake("screen on");	/* first: it reconnects while the display comes back */
		if (dark)
			wake_ui();
		else if (!display_ok && disp_t)	/* back on within the grace while a retry was pending */
			lv_timer_resume(disp_t);
		cpu_apply();
		boost_for(2 * BOOST_MS);	/* the full redraw + whatever the key leads to */
		sys_sampler_pace(lpm ? 10 : 2, lpm ? 60 : 30, true);	/* fresh battery % for the lock screen */
		bool key = last_key && lv_tick_elaps(last_key_at) < 300;
		tlog("screen on");
		plog("screen on after %u s off (%s)", lv_tick_elaps(off_at) / 1000,
		     !key ? "alarm / alert" : last_key == KEY_POWER ? "power key" : last_key == KEY_HOMEPAGE ? "home key" : "key");
	}
}

bool power_dark(void) { return dark; }

static void undim(void)
{
	if (!dimmed)
		return;
	dimmed = false;
	sys_backlight_set(dim_from);
}

int power_backlight(void)
{
	if (dimmed) {
		dimmed = false;	/* the caller sets the backlight itself (screen off) */
		return dim_from;
	}
	return sys_backlight_get();
}

bool power_input(int key)
{
	bool was = dimmed && !off;
	if (!off) {
		undim();
		boost_for(BOOST_MS);
	}
	if (key) {
		last_key = key;
		last_key_at = lv_tick_get();
	}
	if (was)
		plog("undim (%s)", key ? "key" : "touch, not delivered");
	return was;
}

/* ---------- awake reasons ---------- */

#define MAX_AWAKE 8
static struct awake { const char *token; uint32_t until; bool held; } awake[MAX_AWAKE];

static struct awake *slot_in(struct awake *v, int n, const char *token, bool make)
{
	struct awake *free_slot = NULL;
	for (int i = 0; i < n; i++) {
		if (v[i].token && !strcmp(v[i].token, token))
			return &v[i];
		if (!v[i].token && !free_slot)
			free_slot = &v[i];
	}
	if (make && free_slot)
		free_slot->token = token;
	return make ? free_slot : NULL;
}

static struct awake *awake_slot(const char *token, bool make) { return slot_in(awake, MAX_AWAKE, token, make); }

static void awake_drop(struct awake *a, const char *why)
{
	plog("awake -%s (%s)", a->token, why);
	a->token = NULL;
	a->held = false;
	lv_display_trigger_activity(NULL);	/* the auto-lock countdown starts now */
}

void power_keep_awake(const char *token, bool on)
{
	struct awake *a = awake_slot(token, on);
	if (!a)
		return;
	if (!on) {
		awake_drop(a, "released");
		return;
	}
	if (!a->held && !a->until)
		plog("awake +%s", token);
	a->held = true;
	undim();
}

void power_awake_for(const char *token, uint32_t ms)
{
	struct awake *a = awake_slot(token, true);
	if (!a)
		return;
	uint32_t until = (lv_tick_get() + ms) | 1;
	if (!a->held && !a->until)
		plog("awake +%s (%u ms lease)", token, ms);
	if (!a->until || (int32_t)(until - a->until) > 0)
		a->until = until;
	undim();
}

/* any reason held (leases that ran out are dropped here) */
static bool awake_any(void)
{
	bool any = false;
	for (int i = 0; i < MAX_AWAKE; i++) {
		struct awake *a = &awake[i];
		if (!a->token)
			continue;
		if (!a->held && a->until && (int32_t)(lv_tick_get() - a->until) >= 0) {
			awake_drop(a, "lease ran out");
			continue;
		}
		any = true;
	}
	return any;
}

/* ---------- Wi-Fi Sleep ---------- */

/* Associated Wi-Fi is what is left running while dark (POWER-AUDIT: +45-50 mA with power save before the dark DTIM-3
 * listening, Tailscale's keepalives +15-20 of that; suspend would save only 3-10, re/SUSPEND.md). So once the screen has been dark for the
 * pref's minutes Wi-Fi goes to sleep (wifi_sleep: disconnected + wlan0 down, the user's switch untouched), and the
 * screen coming on wakes it at once (before the display: the reconnect runs while the pipe comes back). Checked from
 * power_tick, every 30 s while dark — no timer of its own; the delay counts from screen-off.
 * Not while something needs the network: music playing (songs stream in as it goes, also with the screen off, where
 * it holds nothing else), a net.c request (one made while it sleeps wakes it: power_net_for("fetch"), see net.c),
 * a hold (power_net_hold / power_net_for), the charger (power is free there, and a phone on its
 * cable stays reachable on Wi-Fi), or a download / remote session in a process — curl, wget, ssh, scp, sftp, rsync, apk, git (the Terminal's, the console's; /proc is read only
 * when it is about to sleep). One of these turning up while it sleeps wakes it (the charger at the next tick, a hold
 * at once). Wi-Fi off / starting / joining: nothing to do. Tailscale rides along: without a default route tailscaled
 * has nothing to send and stays quiet, and it rebinds by itself when Wi-Fi is back (pausing it would save nothing
 * measurable and could leave MagicDNS hung if Orchard died meanwhile).
 * Tests: ORCHARD_WIFISLEEP_S=<s> = that delay (0 = off), ORCHARD_WIFISLEEP=charger = also on the charger (a phone
 * measured on its cable, tools/pa_meas.sh). ORCHARD_POWERLOG shows every decision. */
#define MAX_NETHOLD 8
static struct awake nethold[MAX_NETHOLD];
static bool ws_slept;		/* we put Wi-Fi to sleep */
static char ws_held[24];	/* the last reason logged (once per reason) */

static int wifisleep_min(void)
{
	if (!prefs_int("wifisleep", 0))
		return 0;
	int m = prefs_int("wifisleep_min", 5);
	return m == 1 || m == 15 ? m : 5;
}

static uint32_t ws_delay_ms(void)
{
	const char *e = getenv("ORCHARD_WIFISLEEP_S");
	if (e && *e)
		return (uint32_t)LV_MAX(atoi(e), 0) * 1000u;
	return (uint32_t)wifisleep_min() * 60000u;
}

static void ws_wake(const char *why)
{
	if (!ws_slept)
		return;
	ws_slept = false;
	ws_held[0] = 0;
	if (wifi_sleep(false))
		plog("wifi sleep: waking Wi-Fi (%s)", why);
}

void power_net_hold(const char *token, bool on)
{
	struct awake *a = slot_in(nethold, MAX_NETHOLD, token, on);
	if (!a)
		return;
	if (!on) {
		plog("net -%s (released)", token);
		a->token = NULL;
		a->held = false;
		a->until = 0;
		return;
	}
	if (!a->held && !a->until)
		plog("net +%s", token);
	a->held = true;
	ws_wake(token);
}

void power_net_for(const char *token, uint32_t ms)
{
	struct awake *a = slot_in(nethold, MAX_NETHOLD, token, true);
	if (!a)
		return;
	uint32_t until = (lv_tick_get() + ms) | 1;
	if (!a->held && !a->until)
		plog("net +%s (%u ms lease)", token, ms);
	if (!a->until || (int32_t)(until - a->until) > 0)
		a->until = until;
	ws_wake(token);
}

static const char *nethold_any(void)
{
	for (int i = 0; i < MAX_NETHOLD; i++) {
		struct awake *a = &nethold[i];
		if (!a->token)
			continue;
		if (!a->held && a->until && (int32_t)(lv_tick_get() - a->until) >= 0) {
			plog("net -%s (lease ran out)", a->token);
			a->token = NULL;
			a->until = 0;
			continue;
		}
		return a->token;
	}
	return NULL;
}

/* what keeps Wi-Fi awake (cheap checks; the process scan is proc_net_user), NULL = nothing */
static const char *wifisleep_hold(void)
{
	static bool charger_too, checked;
	if (!checked) {
		const char *e = getenv("ORCHARD_WIFISLEEP");
		charger_too = e && !strcmp(e, "charger");
		checked = true;
	}
	if (!charger_too && sys_charger_online())
		return "charger";
	if (music_playing())
		return "music";
	if (net_active() > 0)
		return "net";
	return nethold_any();
}

/* a process that is moving data over the network: its name (comm, or a script's name for sh), NULL = none */
static const char *proc_net_user(void)
{
#ifdef ORCHARD_PHONE
	static const char *names[] = { "curl", "wget", "ssh", "scp", "sftp", "rsync", "apk", "git" };
	static char found[20];
	DIR *d = opendir("/proc");
	if (!d)
		return NULL;
	const char *hit = NULL;
	struct dirent *de;
	char path[64], buf[256];
	while (!hit && (de = readdir(d))) {
		if (de->d_name[0] < '0' || de->d_name[0] > '9')
			continue;
		snprintf(path, sizeof(path), "/proc/%s/comm", de->d_name);
		FILE *f = fopen(path, "r");
		if (!f)
			continue;
		size_t n = fread(buf, 1, sizeof(buf) - 1, f);
		fclose(f);
		buf[n] = 0;
		buf[strcspn(buf, "\n")] = 0;
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !hit; i++)
			if (!strcmp(buf, names[i]))
				hit = names[i];
	}
	closedir(d);
	if (hit)
		snprintf(found, sizeof(found), "%s", hit);
	return hit ? found : NULL;
#else
	return NULL;	/* (the preview's PC runs git and ssh all day) */
#endif
}

/* power_tick while dark */
static void ws_check(void)
{
	if (ws_slept) {
		if (!wifi_asleep()) {	/* woken another way (Wi-Fi switched off / on: its own business) */
			ws_slept = false;
			return;
		}
		const char *h = wifisleep_hold();
		if (h)
			ws_wake(h);
		return;
	}
	uint32_t delay = ws_delay_ms();
	if (!delay || lv_tick_elaps(off_at) < delay || wifi_state() != WIFI_ON || wifi_asleep())
		return;
	const char *h = wifisleep_hold();
	if (!h)
		h = proc_net_user();
	if (h) {
		if (strcmp(h, ws_held))
			plog("wifi sleep: due, kept awake by %s", h);
		snprintf(ws_held, sizeof(ws_held), "%s", h);
		return;
	}
	ws_held[0] = 0;
	if (wifi_sleep(true)) {
		ws_slept = true;
		plog("wifi sleep: Wi-Fi asleep after %u s dark", lv_tick_elaps(off_at) / 1000);
	}
}

/* shell tick: every second while on, every 30 s while dark (its power_keep) */
bool power_tick(void)
{
	minute_log();
	usage_tick();
	if (lpm && sys_charger_online() && sys_battery_pct() >= LPM_OFF_PCT)
		power_set_lpm(false);	/* like iOS: charged enough */
	if (dark) {
		sweep();	/* timers made since (by the kept ones) */
		ws_check();
		return false;
	}
	if (off)
		return false;
	if (awake_any()) {	/* not idle: as if just touched */
		lv_display_trigger_activity(NULL);
		undim();
		return false;
	}
	int secs = power_autolock();
	if (secs > 0 && lock_visible())
		secs = LV_MIN(secs, LOCKSCREEN_S);
	if (secs <= 0) {
		undim();
		return false;
	}
	uint32_t idle = lv_display_get_inactive_time(NULL), at = secs * 1000u, dim_at = at - LV_MIN(DIM_MS, at / 3);
	if (idle >= at)
		return true;	/* shell.c turns it off (power_backlight() hands back the undimmed level) */
	if (idle < dim_at) {
		undim();	/* activity that wasn't a touch (an alarm, a video) */
	} else if (!dimmed) {
		int max = sys_backlight_max(), lo = LV_MAX(max / 40, 20);
		dim_from = sys_backlight_get();
		dimmed = true;
		if (dim_from / 3 > lo)
			sys_backlight_set(dim_from / 3);
		else if (dim_from > lo)
			sys_backlight_set(lo);
		plog("dim (%d → %d), auto-lock in %u s", dim_from, sys_backlight_get(), (at - idle + 999) / 1000);
	}
	return false;
}

int power_autolock(void)
{
	int s = prefs_int("autolock", 30);
	return lpm && (s == 0 || s > 30) ? 30 : s;	/* Low Power Mode: 30 s at most (iOS) */
}

void power_init(void)
{
	logging = getenv("ORCHARD_POWERLOG") && atoi(getenv("ORCHARD_POWERLOG"));
	perf_mode = getenv("ORCHARD_CPU") && !strcmp(getenv("ORCHARD_CPU"), "perf");
	boost_t = lv_timer_create(boost_cb, BOOST_MS, NULL);
	lv_timer_pause(boost_t);
	cpu_apply();
	sys_cpufreq_on_exit();
	lv_display_t *d = lv_display_get_default();
	lv_display_add_event_cb(d, refr_request, LV_EVENT_REFR_REQUEST, NULL);
	lv_timer_handler_set_resume_cb(timer_resumed, NULL);
	lv_display_add_event_cb(d, refr_start, LV_EVENT_REFR_START, NULL);
	lpm = prefs_int("lowpower", 0);
	pace();
	cpu_apply();
	if (lpm)
		wifi_powersave();
	plog("log on (ORCHARD_POWERLOG): screen changes + a line a minute");
}
