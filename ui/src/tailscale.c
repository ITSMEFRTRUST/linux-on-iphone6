/* Tailscale (VPN): your own tailnet through /usr/sbin/tsup (tools/tsup; binaries + state on the flash in
 * /data/tailscale). Nothing here blocks: `tsup status` runs as a child whose one-line answer is read through a
 * non-blocking pipe, `tsup on|off` as another child, both reaped by one LVGL timer (500 ms). Status is polled
 * only while something shows it on screen: every 3 s with Settings › VPN open, every 10 s for a bound row value.
 * The timer pauses when nothing is running and no VPN UI exists, and slows to 2 s while that UI is off screen.
 * `tsup status` = "tailscale <on|off>: connected ip=100.x | needs-login url=... | starting | stopped | not-installed".
 * Previews (snap/sdl): a fake tailnet, ORCHARD_TS=off|login|none|stopped for the other states (default connected);
 * ORCHARD_TS=real runs the real script (ORCHARD_TSUP = its path, ORCHARD_TS_DIR = its /data/tailscale: a mock). */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "orchard.h"

#define MAXLBL 4

static bool fake, inited;
static enum ts_state st = TS_NONE;
static char ip[48], url[160];
static lv_obj_t *lbl[MAXLBL];
static pid_t st_pid, act_pid;
static int st_fd = -1;
static char st_buf[256];
static int st_len;
static uint32_t polled_at, fk_at;
static bool act_on, polled;
static enum ts_state fk_after;
static lv_timer_t *timer;

/* Settings › VPN */
static lv_obj_t *pg_box, *pg_sw;
static enum ts_state pg_shown = (enum ts_state)-1;
static void page_build(void);

/* binaries + state + off flag (tools/tsup); ORCHARD_TS_DIR for a mock with ORCHARD_TS=real */
static const char *ts_file(const char *name)
{
	static char b[256];
	const char *e = getenv("ORCHARD_TS_DIR");
	snprintf(b, sizeof(b), "%s/%s", e && *e ? e : "/data/tailscale", name);
	return b;
}

static const char *tsup_path(void)
{
	const char *e = getenv("ORCHARD_TSUP");
	return e && *e ? e : "/usr/sbin/tsup";
}

const char *ts_status(void)
{
	switch (st) {
	case TS_NONE: return "Not Installed";
	case TS_OFF: return "Off";
	case TS_STOPPED: return "Not Connected";
	case TS_STARTING: return "Connecting\xe2\x80\xa6";
	case TS_LOGIN: return "Needs Login";
	case TS_ON: return "Connected";
	}
	return "";
}

enum ts_state ts_state(void) { return st; }

static void set_state(enum ts_state s)
{
	st = s;
	for (int i = 0; i < MAXLBL; i++)
		if (lbl[i])
			lv_label_set_text(lbl[i], ts_status());
	if (pg_box && pg_shown != st)
		page_build();
}

/* no child needed: no script / binaries, or the off flag */
static bool quick_state(void)
{
	if (access(tsup_path(), X_OK) || access(ts_file("tailscaled"), X_OK)) {
		set_state(TS_NONE);
		return true;
	}
	if (!access(ts_file("off"), F_OK)) {
		set_state(TS_OFF);
		return true;
	}
	return false;
}

static void parse(char *line)
{
	char *nl = strchr(line, '\n');
	if (nl)
		*nl = 0;
	char *p = strstr(line, ": ");
	if (!p) {
		fprintf(stderr, "tailscale: odd status '%s'\n", line);
		return;
	}
	p += 2;
	enum ts_state ns = TS_STARTING;
	char nip[sizeof(ip)] = "", nurl[sizeof(url)] = "", *q;
	if (!strncmp(p, "connected", 9)) {
		ns = TS_ON;
		if ((q = strstr(p, "ip=")))
			snprintf(nip, sizeof(nip), "%s", q + 3);
	} else if (!strncmp(p, "needs-login", 11)) {
		ns = TS_LOGIN;
		if ((q = strstr(p, "url=")) && strcmp(q + 4, "-"))
			snprintf(nurl, sizeof(nurl), "%s", q + 4);
	} else if (!strncmp(p, "not-installed", 13)) {
		ns = TS_NONE;
	} else if (!strncmp(line, "tailscale off", 13)) {
		ns = TS_OFF;
	} else if (!strncmp(p, "stopped", 7)) {
		ns = TS_STOPPED;
	}
	/* while "tsup on" runs only progress counts (the login link, connected), not the state it started from */
	if (act_pid && act_on && (ns == TS_OFF || ns == TS_STOPPED || ns == TS_NONE))
		return;
	if (pg_box && (strcmp(nip, ip) || strcmp(nurl, url)))
		pg_shown = (enum ts_state)-1;	/* new IP / login link: rebuild the page */
	memcpy(ip, nip, sizeof(ip));
	memcpy(url, nurl, sizeof(url));
	set_state(ns);
}

static pid_t spawn(const char *arg, int *out_fd)
{
	int pfd[2] = { -1, -1 };
	if (out_fd && pipe(pfd))
		return 0;
	pid_t p = fork();
	if (p == 0) {
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(out_fd ? pfd[1] : fd, 1);
		}
		for (int i = 3; i < 1024; i++)
			close(i);
		setsid();
		execl(tsup_path(), "tsup", arg, (char *)NULL);
		_exit(127);
	}
	if (out_fd) {
		close(pfd[1]);
		if (p > 0) {
			fcntl(pfd[0], F_SETFL, O_NONBLOCK);
			*out_fd = pfd[0];
		} else {
			close(pfd[0]);
		}
	}
	return p > 0 ? p : 0;
}

static void poll_status(void)
{
	polled_at = lv_tick_get();
	polled = true;
	if (st_pid || (!(act_pid && act_on) && quick_state()))
		return;
	st_len = 0;
	st_pid = spawn("status", &st_fd);
}

static void reap(void)
{
	int stt;
	if (st_fd >= 0) {
		for (;;) {
			ssize_t n = read(st_fd, st_buf + st_len, sizeof(st_buf) - 1 - st_len);
			if (n > 0) {
				st_len += n;
				if (st_len < (int)sizeof(st_buf) - 1)
					continue;
			}
			if (n < 0 && (errno == EAGAIN || errno == EINTR))
				break;
			close(st_fd);	/* EOF (or full) */
			st_fd = -1;
			st_buf[st_len] = 0;
			if (st_len && !(act_pid && !act_on))	/* "tsup off" owns the state meanwhile */
				parse(st_buf);
			break;
		}
	}
	if (st_pid && waitpid(st_pid, &stt, WNOHANG) == st_pid)
		st_pid = 0;
	if (act_pid && waitpid(act_pid, &stt, WNOHANG) == act_pid) {
		int code = WIFEXITED(stt) ? WEXITSTATUS(stt) : -1;
		fprintf(stderr, "tailscale: tsup %s exited %d\n", act_on ? "on" : "off", code);
		act_pid = 0;
		polled_at = 0;	/* ask at once */
		if (code && act_on)
			ui_alert("Tailscale", "Tailscale didn't start (tsup on failed). Its log is /tmp/tailscale.log.",
				 (const char *[]){ "OK" }, 1, NULL, NULL);
	}
}

/* bound labels; on_screen: only those visible now (Settings open on its root page) */
static int watchers(bool on_screen)
{
	int n = 0;
	for (int i = 0; i < MAXLBL; i++)
		n += lbl[i] != NULL && (!on_screen || lv_obj_is_visible(lbl[i]));
	return n;
}

static void wake(void)
{
	if (timer) {
		lv_timer_set_period(timer, 500);
		lv_timer_resume(timer);
	}
}

static void tick(lv_timer_t *t)
{
	if (fake) {
		if (fk_at && (int32_t)(lv_tick_get() - fk_at) >= 0) {
			fk_at = 0;
			set_state(fk_after);
		}
		if (!fk_at && (shell_locked() || (!pg_box && !watchers(false))))
			lv_timer_pause(t);	/* (locked: lock_changed wakes it) */
		return;
	}
	reap();
	bool seen = !shell_locked();	/* under the lock screen nothing shows the state: no `tailscale status` */
	bool page = seen && pg_box && lv_obj_is_visible(pg_box);
	uint32_t every = page ? 3000 : seen && watchers(true) ? 10000 : 0;
	if (every && !(act_pid && !act_on) && (!polled || !polled_at || lv_tick_elaps(polled_at) >= every))
		poll_status();
	bool busy = st_pid || st_fd >= 0 || act_pid;
	if (busy || every)
		lv_timer_set_period(t, 500);
	else if (pg_box || watchers(false))
		lv_timer_set_period(t, 2000);	/* VPN UI exists but is off screen (a page above it, app hidden) */
	else
		lv_timer_pause(t);	/* wake() when the UI comes back or tsup on/off starts */
}

static void lock_changed(bool locked)
{
	if (!locked && (pg_box || watchers(false)))
		wake();	/* the VPN row / page polls again at once */
}

static void init(void)
{
	if (inited)
		return;
	inited = true;
	lock_listen(lock_changed, true);
	const char *e = getenv("ORCHARD_TS");
#ifdef ORCHARD_PHONE
	fake = false;
#else
	fake = !(e && !strcmp(e, "real"));
#endif
	if (fake) {
		st = !e ? TS_ON : !strcmp(e, "off") ? TS_OFF : !strcmp(e, "login") ? TS_LOGIN : !strcmp(e, "none") ? TS_NONE
		   : !strcmp(e, "stopped") ? TS_STOPPED : TS_ON;
		snprintf(ip, sizeof(ip), "100.101.102.103");
		snprintf(url, sizeof(url), "https://login.tailscale.com/a/1a2b3c4d5e6f7");
	} else {
		quick_state();
		if (st == TS_NONE && !access(tsup_path(), X_OK) && !access(ts_file("tailscaled"), X_OK))
			st = TS_STOPPED;
	}
	timer = lv_timer_create(tick, 500, NULL);
}

void ts_set_power(bool on)
{
	init();
	wake();
	act_on = on;
	if (fake) {
		if (on) {
			fk_after = getenv("ORCHARD_TS") && !strcmp(getenv("ORCHARD_TS"), "login") ? TS_LOGIN : TS_ON;
			fk_at = lv_tick_get() + 1500;
			set_state(TS_STARTING);
		} else {
			fk_at = 0;
			set_state(TS_OFF);
		}
		return;
	}
	if (act_pid)	/* the running one finishes first; its result is polled afterwards */
		return;
	act_pid = spawn(on ? "on" : "off", NULL);
	set_state(on ? TS_STARTING : TS_OFF);
}

static void lbl_deleted(lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target(e);
	for (int i = 0; i < MAXLBL; i++)
		if (lbl[i] == o)
			lbl[i] = NULL;
}

void ts_bind_label(lv_obj_t *label)
{
	init();
	wake();
	if (!label)
		return;
	for (int i = 0; i < MAXLBL; i++)
		if (!lbl[i]) {
			lbl[i] = label;
			lv_label_set_text(label, ts_status());
			lv_obj_add_event_cb(label, lbl_deleted, LV_EVENT_DELETE, NULL);
			polled_at = 0;
			return;
		}
}

/* ================================ Settings › VPN ================================ */

static void sw_cb(lv_event_t *e)
{
	ts_set_power(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	ui_haptic_tap();
}

static void info_row(lv_obj_t *sec, const char *title, const char *value)
{
	lv_obj_t *r = ui_row(sec, NULL, 0, title);
	ui_row_value(r, value);
}

static void page_build(void)
{
	pg_shown = st;
	lv_obj_clean(pg_box);
	pg_sw = NULL;
	const char *foot = st == TS_NONE
		? "Tailscale isn't on this iPhone: it needs /usr/sbin/tsup and the binaries in /data/tailscale."
		: "Connects this iPhone to your tailnet, so it reaches your devices from any Wi-Fi. Nothing on the tailnet "
		  "can connect to the iPhone (shields up).";
	lv_obj_t *s = ui_section(pg_box, NULL, foot);
	lv_obj_t *r = ui_row(s, G_SHIELD, pal.blue, "Tailscale");
	ui_row_subtitle(r, ts_status());
	if (st != TS_NONE)
		pg_sw = ui_row_switch(r, st != TS_OFF, sw_cb, NULL);

	if (st == TS_LOGIN) {
		s = ui_section(pg_box, "Approve this iPhone", "Open the link on a computer signed in to the tailnet. "
			       "The iPhone connects by itself once it is approved.");
		lv_obj_t *l = text(s, url[0] ? url : "Waiting for the login link\xe2\x80\xa6", font(F_MONO, 26), pal.label);
		lv_obj_set_width(l, LV_PCT(100));
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_pad_all(l, 32, 0);
	} else if (st == TS_ON) {
		s = ui_section(pg_box, "This iPhone", NULL);
		info_row(s, "Name", "iphone6");
		info_row(s, "Tailscale IP", ip[0] ? ip : "\xe2\x80\x94");
		info_row(s, "Incoming", "Blocked");
	}
}

static void page_left(void)
{
	pg_box = pg_sw = NULL;
	pg_shown = (enum ts_state)-1;
}

void ts_settings_page(void)
{
	init();
	wake();
	if (pg_box)
		return;
	lv_obj_t *c = nav_push("VPN");
	if (!c)
		return;
	nav_on_leave(page_left);
	pg_box = box(c);
	lv_obj_set_size(pg_box, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(pg_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(pg_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	page_build();
	polled_at = 0;
}
