/* sysfs/procfs readers + the few knobs the shell controls (backlight, haptics). */
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include "orchard.h"

#define CHARGER "/sys/class/power_supply/sn2400-charger/"
#define BATTERY "/sys/class/power_supply/battery/"	/* the bq27540 gauge (apple_swi_battery) */

bool sys_haptics_enabled = true;

static long read_long(const char *path, long def)
{
	FILE *f = fopen(path, "r");
	long v;
	if (!f)
		return def;
	if (fscanf(f, "%ld", &v) != 1)
		v = def;
	fclose(f);
	return v;
}

static const char *backlight_dir(void)
{
	static char dir[256];
	static bool done;
#ifndef ORCHARD_PHONE
	done = true;	/* the desktop preview / snapshot tool keeps its hands off the PC */
#endif
	if (!done) {
		DIR *d = opendir("/sys/class/backlight");
		struct dirent *e;
		done = true;
		while (d && (e = readdir(d)))
			if (e->d_name[0] != '.') {
				snprintf(dir, sizeof(dir), "/sys/class/backlight/%s/", e->d_name);
				break;
			}
		if (d)
			closedir(d);
	}
	return dir[0] ? dir : NULL;
}

static int fake_backlight = 1024;	/* hosts without a backlight */

int sys_backlight_max(void)
{
	char p[300];
	if (!backlight_dir())
		return 2047;
	snprintf(p, sizeof(p), "%smax_brightness", backlight_dir());
	return read_long(p, 2047);
}

int sys_backlight_get(void)
{
	char p[300];
	if (!backlight_dir())
		return fake_backlight;
	snprintf(p, sizeof(p), "%sbrightness", backlight_dir());
	return read_long(p, 0);
}

void sys_backlight_set(int v)
{
	char p[300];
	FILE *f;
	if (!backlight_dir()) {
		fake_backlight = v;
		return;
	}
	snprintf(p, sizeof(p), "%sbrightness", backlight_dir());
	if ((f = fopen(p, "w"))) {
		fprintf(f, "%d\n", v);
		fclose(f);
	}
}

/* The charger's online / current_now go through its ADC (~11 ms per read) and the battery gauge
 * talks HDQ over a slow single wire (~15 ms per register), both blocked in the kernel. Read on the
 * UI thread (status bar, lock screen, several times a second) that was a 30-55 ms hitch every
 * second, so a sampler thread reads them and the UI only gets the last values: the charger every
 * 2 s, the battery every 30 s (and right away when the charger comes or goes) — power.c paces it
 * (screen off: both once a minute). current_now of either is only sampled while someone asked for it
 * in the last few seconds (then every 2 s, the battery details every 4 s). */
static volatile int pace_ch = 2, pace_bat = 30;
static volatile bool pace_kick;
static pthread_mutex_t pace_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pace_cv = PTHREAD_COND_INITIALIZER;	/* phone: re-made on CLOCK_MONOTONIC before use */

#ifdef ORCHARD_PHONE
static volatile int ch_online = -1, ch_ma;
static volatile uint32_t ch_ma_asked, bat_asked;
static volatile int bat_pct = -1, bat_ma, bat_mv, bat_temp, bat_cycles, bat_full, bat_design, bat_mw, bat_charge;

static void battery_read(bool detail)
{
	int p = read_long(BATTERY "capacity", -1);
	if (p < 0) {
		bat_pct = -1;	/* no gauge (older kernel): callers fall back to "unknown" */
		return;
	}
	bat_pct = p > 100 ? 100 : p;
	bat_mv = read_long(BATTERY "voltage_now", 0) / 1000;
	if (detail) {	/* Settings › Battery is open */
		bat_ma = read_long(BATTERY "current_now", 0) / 1000;
		bat_temp = read_long(BATTERY "temp", 0);
		bat_cycles = read_long(BATTERY "cycle_count", 0);
		bat_full = read_long(BATTERY "charge_full", 0) / 1000;
		bat_design = read_long(BATTERY "charge_full_design", 0) / 1000;
		bat_mw = read_long(BATTERY "power_avg", 0) / 1000;
		bat_charge = read_long(BATTERY "charge_now", 0) / 1000;
	}
}

static uint64_t mono_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000ull + t.tv_nsec / 1000000;
}

static void *charger_thread(void *arg)
{
	(void)arg;
	uint64_t next_ch = 0, next_bat = 0;
	for (;;) {
		uint64_t now = mono_ms();
		bool detail = bat_asked && lv_tick_elaps(bat_asked) < 5000;
		pthread_mutex_lock(&pace_mu);
		bool kick = pace_kick;
		pace_kick = false;
		pthread_mutex_unlock(&pace_mu);
		bool ch_due = kick || now >= next_ch, bat_due = kick || now >= next_bat;
		if (ch_due) {
			int was = ch_online;
			ch_online = read_long(CHARGER "online", 0) == 1;
			bat_due |= was != ch_online;
			if (ch_ma_asked && lv_tick_elaps(ch_ma_asked) < 5000)
				ch_ma = read_long(CHARGER "current_now", 0) / 1000;
			next_ch = now + (detail ? 2 : pace_ch) * 1000ull;
		}
		if (bat_due) {
			battery_read(detail);
			next_bat = now + (detail ? 4 : pace_bat) * 1000ull;
		}
		uint64_t until = next_ch < next_bat ? next_ch : next_bat;
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		uint64_t wait = until > now ? until - now : 0;
		ts.tv_sec += wait / 1000;
		ts.tv_nsec += (wait % 1000) * 1000000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec++;
			ts.tv_nsec -= 1000000000;
		}
		pthread_mutex_lock(&pace_mu);
		while (!pace_kick && pthread_cond_timedwait(&pace_cv, &pace_mu, &ts) == 0)
			;
		pthread_mutex_unlock(&pace_mu);	/* (a kick: both are read at the top) */
	}
	return NULL;
}

static void sampler_start(void)
{
	static bool started;
	if (started)
		return;
	started = true;
	ch_online = read_long(CHARGER "online", 0) == 1;	/* first call: one read now, the thread keeps it fresh */
	pthread_condattr_t ca;
	pthread_condattr_init(&ca);
	pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
	pthread_cond_init(&pace_cv, &ca);
	pthread_condattr_destroy(&ca);
	pthread_t t;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&a, 64 << 10);
	pthread_create(&t, &a, charger_thread, NULL);
	pthread_attr_destroy(&a);
}

bool sys_charger_online(void)
{
	sampler_start();
	return ch_online == 1;
}

int sys_charger_ma(void)
{
	if (!ch_ma_asked || lv_tick_elaps(ch_ma_asked) >= 5000)
		ch_ma = read_long(CHARGER "current_now", 0) / 1000;	/* first ask after a while: fresh */
	ch_ma_asked = lv_tick_get() | 1;
	return ch_ma;
}

int sys_battery_pct(void)
{
	sampler_start();
	return bat_pct;
}

void sys_battery_info(sys_battery_t *b)
{
	sampler_start();
	bool fresh = !bat_asked || lv_tick_elaps(bat_asked) >= 5000;
	bat_asked = lv_tick_get() | 1;	/* the thread reads the details while this keeps being asked */
	if (fresh)
		sys_sampler_pace(pace_ch, pace_bat, true);	/* it may be in a long sleep: details now */
	b->pct = bat_pct;
	b->ma = bat_ma;
	b->mv = bat_mv;
	b->temp_dc = bat_temp;
	b->cycles = bat_cycles;
	b->full_mah = bat_full;
	b->design_mah = bat_design;
	b->power_mw = bat_mw;
	b->charge_mah = bat_charge;
}
#else
bool sys_charger_online(void) { return read_long(CHARGER "online", 0) == 1; }
int sys_charger_ma(void) { return read_long(CHARGER "current_now", 0) / 1000; }
/* desktop preview / snapshots: a plausible battery so the status bar and Settings can be checked */
int sys_battery_pct(void) { return 76; }
void sys_battery_info(sys_battery_t *b)
{
	*b = (sys_battery_t){ .pct = 76, .ma = -212, .mv = 3912, .temp_dc = 284, .cycles = 230, .full_mah = 1715,
			      .design_mah = 1751, .power_mw = -829, .charge_mah = 1290 };
}
#endif

void sys_sampler_pace(int charger_s, int battery_s, bool kick)
{
#ifdef ORCHARD_PHONE
	sampler_start();
#endif
	bool changed = charger_s != pace_ch || battery_s != pace_bat;
	pace_ch = charger_s > 0 ? charger_s : 1;
	pace_bat = battery_s > 0 ? battery_s : 1;
	if (!changed && !kick)
		return;
	pthread_mutex_lock(&pace_mu);
	pace_kick = true;	/* the thread starts over with the new pace (and reads both now) */
	pthread_cond_signal(&pace_cv);
	pthread_mutex_unlock(&pace_mu);
}

int sys_charger_limit_ma(void) { return read_long(CHARGER "input_current_limit", 0) / 1000; }

long sys_uptime_s(void) { return read_long("/proc/uptime", 0); }

void sys_mem_mb(int *total, int *avail)
{
	FILE *f = fopen("/proc/meminfo", "r");
	char line[128];
	long v;
	*total = *avail = 0;
	while (f && fgets(line, sizeof(line), f)) {
		if (sscanf(line, "MemTotal: %ld", &v) == 1)
			*total = v / 1024;
		else if (sscanf(line, "MemAvailable: %ld", &v) == 1)
			*avail = v / 1024;
	}
	if (f)
		fclose(f);
}

const char *sys_kernel(void)
{
	static struct utsname u;
	if (!u.release[0]) {
		uname(&u);
		char *g = strstr(u.release, "-g");	/* drop the git describe suffix */
		if (g)
			*g = 0;
	}
	return u.release;
}

void sys_haptic(int ms)
{
	if (sys_haptics_enabled)
		hal_haptic(ms);
}

int sys_cpu_count(void)
{
	long n = sysconf(_SC_NPROCESSORS_ONLN);
	return n > 0 ? n : 1;
}

/* cpufreq (power.c decides): performance, or schedutil between two limits. A write to scaling_min/max_freq
 * waits for the frequency change it causes, and the governor's for its restart, so a helper thread does the
 * writing (latest wish wins) and only what differs from the policy's present value is written. */
#ifdef ORCHARD_PHONE
#define CPUFREQ "/sys/devices/system/cpu/cpufreq/"
static struct { bool perf; int min, max; unsigned gen; } cf_want;
static pthread_mutex_t cf_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cf_cv = PTHREAD_COND_INITIALIZER;

static void put_if(const char *dir, const char *file, const char *v)
{
	char p[160], cur[32] = "";
	snprintf(p, sizeof(p), "%s%s", dir, file);
	FILE *f = fopen(p, "r");
	if (f) {
		if (!fgets(cur, sizeof(cur), f))
			cur[0] = 0;
		fclose(f);
	}
	cur[strcspn(cur, "\n")] = 0;
	if (!strcmp(cur, v) || !(f = fopen(p, "w")))
		return;
	fputs(v, f);
	fclose(f);
}

static void *cf_thread(void *arg)
{
	(void)arg;
	unsigned done = 0;
	for (;;) {
		pthread_mutex_lock(&cf_mu);
		while (cf_want.gen == done)
			pthread_cond_wait(&cf_cv, &cf_mu);
		typeof(cf_want) w = cf_want;
		done = w.gen;
		pthread_mutex_unlock(&cf_mu);
		DIR *d = opendir(CPUFREQ);
		struct dirent *e;
		while (d && (e = readdir(d))) {
			if (strncmp(e->d_name, "policy", 6))
				continue;
			char dir[96], p[160], v[16];
			snprintf(dir, sizeof(dir), CPUFREQ "%s/", e->d_name);
			snprintf(p, sizeof(p), "%scpuinfo_min_freq", dir);
			int lo = read_long(p, 0);
			snprintf(p, sizeof(p), "%scpuinfo_max_freq", dir);
			int hi = read_long(p, 0);
			if (lo <= 0 || hi < lo)
				continue;
			int max = w.max > 0 ? LV_CLAMP(lo, w.max, hi) : hi;
			int min = w.min < 0 ? max : w.min > 0 ? LV_CLAMP(lo, w.min, max) : lo;
			put_if(dir, "scaling_governor", w.perf ? "performance" : "schedutil");
			snprintf(v, sizeof(v), "%d", max);
			put_if(dir, "scaling_max_freq", v);
			snprintf(v, sizeof(v), "%d", w.perf ? max : min);
			put_if(dir, "scaling_min_freq", v);
		}
		if (d)
			closedir(d);
	}
	return NULL;
}
#endif

void sys_cpufreq(bool performance, int min_khz, int max_khz)
{
#ifdef ORCHARD_PHONE
	static bool started;
	pthread_mutex_lock(&cf_mu);
	if (cf_want.gen && cf_want.perf == performance && cf_want.min == min_khz && cf_want.max == max_khz) {
		pthread_mutex_unlock(&cf_mu);
		return;
	}
	cf_want.perf = performance, cf_want.min = min_khz, cf_want.max = max_khz;
	cf_want.gen++;
	pthread_cond_signal(&cf_cv);
	pthread_mutex_unlock(&cf_mu);
	if (!started) {
		started = true;
		pthread_t t;
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&a, 64 << 10);
		pthread_create(&t, &a, cf_thread, NULL);
		pthread_attr_destroy(&a);
	}
#else
	(void)performance, (void)min_khz, (void)max_khz;
#endif
}

/* Orchard stopped (SIGTERM / SIGINT from orc deploy, or exit) while boosted or dark: plain schedutil over the whole
 * range again, so the phone isn't left at 1.4 GHz or capped at 600 MHz. The handler only uses open / write /
 * close on strings made beforehand (async-signal-safe), then dies the default way. */
#ifdef ORCHARD_PHONE
#define CF_MAXPOL 4
static struct { char dir[96], lo[16], hi[16]; } cf_pol[CF_MAXPOL];
static int cf_npol;

static void put_raw(const char *dir, const char *file, const char *v)
{
	char p[160];
	size_t n = strlen(dir), m = strlen(file);
	if (n + m >= sizeof(p))
		return;
	memcpy(p, dir, n);
	memcpy(p + n, file, m + 1);
	int fd = open(p, O_WRONLY | O_CLOEXEC);
	if (fd >= 0) {
		ssize_t r = write(fd, v, strlen(v));
		(void)r;
		close(fd);
	}
}

static void cf_reset(void)
{
	for (int i = 0; i < cf_npol; i++) {
		put_raw(cf_pol[i].dir, "scaling_governor", "schedutil");
		put_raw(cf_pol[i].dir, "scaling_max_freq", cf_pol[i].hi);
		put_raw(cf_pol[i].dir, "scaling_min_freq", cf_pol[i].lo);
	}
}

static void cf_signal(int sig)
{
	cf_reset();
	signal(sig, SIG_DFL);
	raise(sig);
}
#endif

void sys_cpufreq_on_exit(void)
{
#ifdef ORCHARD_PHONE
	DIR *d = opendir(CPUFREQ);
	struct dirent *e;
	while (d && (e = readdir(d)) && cf_npol < CF_MAXPOL) {
		if (strncmp(e->d_name, "policy", 6))
			continue;
		char p[160];
		snprintf(cf_pol[cf_npol].dir, sizeof(cf_pol[0].dir), CPUFREQ "%s/", e->d_name);
		snprintf(p, sizeof(p), "%scpuinfo_min_freq", cf_pol[cf_npol].dir);
		snprintf(cf_pol[cf_npol].lo, sizeof(cf_pol[0].lo), "%ld", read_long(p, 0));
		snprintf(p, sizeof(p), "%scpuinfo_max_freq", cf_pol[cf_npol].dir);
		snprintf(cf_pol[cf_npol].hi, sizeof(cf_pol[0].hi), "%ld", read_long(p, 0));
		if (strcmp(cf_pol[cf_npol].lo, "0") && strcmp(cf_pol[cf_npol].hi, "0"))
			cf_npol++;
	}
	if (d)
		closedir(d);
	atexit(cf_reset);
	signal(SIGTERM, cf_signal);
	signal(SIGINT, cf_signal);
#endif
}

/* the old switch: performance while the screen is on, schedutil while it is off */
void sys_cpu_perf(bool on) { sys_cpufreq(on, 0, 0); }

int sys_cpu_mhz(int cpu)
{
	char p[96];
	snprintf(p, sizeof(p), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
	return read_long(p, 0) / 1000;
}

int sys_cpu_load(void)
{
	static long long last_busy, last_total;
	long long v[8] = { 0 }, busy, total = 0;
	FILE *f = fopen("/proc/stat", "r");
	if (!f)
		return 0;
	int n = fscanf(f, "cpu %lld %lld %lld %lld %lld %lld %lld %lld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
	fclose(f);
	if (n < 4)
		return 0;
	for (int i = 0; i < 8; i++)
		total += v[i];
	busy = total - v[3] - v[4];
	int pct = total > last_total ? (int)((busy - last_busy) * 100 / (total - last_total)) : 0;
	last_busy = busy;
	last_total = total;
	return pct < 0 ? 0 : pct > 100 ? 100 : pct;
}

bool sys_usbnet_up(void)
{
	return read_long("/sys/class/net/usb0/carrier", 0) == 1;
}

/* volume through ALSA's amixer (installed into the ramdisk by bin/audioboot): the speaker's DAC, or the
 * BlueALSA mixer of the Bluetooth device the sound goes to (bt.c → sys_volume_route) */
#define AMIXER "/usr/bin/amixer"
#define VOL_CTL "'Speaker DAC'"
static int vol_cache = -1;
static char vol_target[200] = VOL_CTL;	/* amixer arguments around the sget/sset verb */
static char vol_dev[64];

#ifdef ORCHARD_PHONE
bool sys_volume_available(void) { return access(AMIXER, X_OK) == 0; }
#else
bool sys_volume_available(void) { return false; }
#endif

void sys_volume_route(const char *dev, const char *ctl, int pct)
{
	if (dev && ctl && *ctl && !strchr(dev, '\'') && !strchr(ctl, '\'')) {
		snprintf(vol_dev, sizeof(vol_dev), "-D '%s' ", dev);
		snprintf(vol_target, sizeof(vol_target), "'%s'", ctl);
	} else {
		vol_dev[0] = 0;
		snprintf(vol_target, sizeof(vol_target), VOL_CTL);
	}
	vol_cache = pct >= 0 && dev ? pct : -1;
}

int sys_volume_get(void)
{
	if (vol_cache >= 0)
		return vol_cache;
	char cmd[300];
	snprintf(cmd, sizeof(cmd), AMIXER " %ssget %s 2>/dev/null", vol_dev, vol_target);
	FILE *p = sys_volume_available() ? popen(cmd, "r") : NULL;
	char line[200];
	int pct = 50;
	while (p && fgets(line, sizeof(line), p)) {
		char *b = strchr(line, '[');
		if (b && strchr(b, '%')) {
			pct = atoi(b + 1);
			break;
		}
	}
	if (p)
		pclose(p);
	return vol_cache = pct;
}

void sys_volume_set(int pct)
{
	char cmd[320];
	pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
	vol_cache = pct;
	if (!sys_volume_available())
		return;
	snprintf(cmd, sizeof(cmd), AMIXER " -q %ssset %s %d%% >/dev/null 2>&1 &", vol_dev, vol_target, pct);
	if (system(cmd) < 0)
		vol_cache = -1;
}

void sys_reboot(void)
{
#ifdef ORCHARD_PHONE
	sync();
	reboot(RB_AUTOBOOT);
#endif
}

/* /data is the only disk: unmount it (or at least make it read-only) so the ext4 journal is clean */
void sys_poweroff(void)
{
#ifdef ORCHARD_PHONE
	sync();
	if (umount("/data") && mount(NULL, "/data", NULL, MS_REMOUNT | MS_RDONLY, NULL))
		umount2("/data", MNT_DETACH);
	sync();
	reboot(RB_POWER_OFF);
#endif
}

void sys_clock_format(char *buf, int n, bool with_ampm)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	if (prefs_int("clock24", 0)) {
		strftime(buf, n, "%H:%M", &tm);
		return;
	}
	strftime(buf, n, with_ampm ? "%I:%M %p" : "%I:%M", &tm);	/* musl has no %l */
	if (buf[0] == '0')
		memmove(buf, buf + 1, strlen(buf));
}

/* drop an incomplete UTF-8 sequence at the end (a string cut to a byte limit) */
void utf8_trim(char *s)
{
	int n = strlen(s), i = n;
	while (i > 0 && ((unsigned char)s[i - 1] & 0xc0) == 0x80)
		i--;	/* continuation bytes back to the lead byte */
	if (i == 0)
		return;
	unsigned char c = s[i - 1];
	int need = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
	if (n - (i - 1) < need)
		s[i - 1] = 0;
}
