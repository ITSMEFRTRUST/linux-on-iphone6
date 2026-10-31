/* Voice Memos: record from the iPhone 6's mics, list, play back.
 *
 * Capture = the codec's ASP on ALSA hw:0,2 (2 ch S16_LE 48 kHz: TX1 = ADC1A = bottom mic, TX2 = ADC2A = rear by the
 * cs42l67 driver's defaults). One recorder thread runs `arecord` (busybox-less phone: /usr/bin/aplay started as
 * "arecord") into a pipe, keeps one channel and writes a mono 16-bit 48 kHz WAV under $ORCHARD_DATA/Recordings/
 * (first as ".rec-<name>.wav", renamed when it is finished; a leftover one from a crash is recovered on the next
 * scan). The rear / front mics need the n61-mic3 module's controls (`ADC1A Capture Volume` tells it): rear = TX2 on
 * ADC2A, front = `ASP TX2 Mux` → ADC2B for the recording (put back to ADC2A after). The device is checked first
 * (/proc/asound/card0/pcm2c/sub0/status): in use = an alert naming who has it, nothing starts.
 * Playback = `aplay` on the ALSA default PCM (the speaker's plug on hw:0,0, or BlueALSA while Bluetooth audio is
 * connected — bt.c rewrites the default), fed raw samples by a player thread from the chosen spot; pause = aplay
 * stops (the spot is kept), seek = a new aplay from there. Nothing ever plays by itself: tapping a recording only
 * selects it. Music pauses when a recording or playback starts (one sound at a time).
 * Home / app closed = the recording stops and is saved, playback pauses. Locking (Power, auto-lock) pauses playback
 * only: like iOS the recording goes on with the screen dark (its thread and arecord don't need the UI; Orchard
 * never suspends the SoC) and shows its progress again on wake. A recording holds auto-lock off while the app is
 * in front, not on the Lock Screen.
 * Previews (sdl / snap; ORCHARD_MEMOS=fake on the phone): a synthetic voice paced by LVGL's clock, no ALSA;
 * ORCHARD_MEMOS=busy = the "in use" alert, =nomic = no capture device.
 * Space: a recording won't start below MIN_START_MB free in Recordings/; one running stops and is saved below
 * MIN_KEEP_MB (checked once a second of audio). ORCHARD_MEMOS_FREE_MB="150,40" pretends (one value per check).
 * Pref memos_mic: 0 bottom (default), 1 rear, 2 front. Pure parts: memo.c (test/unit/memo_test.c).
 */
#include "orchard.h"
#include "memo.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#define CAP_DEV "hw:0,2"
/* test knobs (a PC harness with stand-in programs): ORCHARD_APLAY, ORCHARD_AMIXER, ORCHARD_CAP_STATUS */
static const char *env_or(const char *k, const char *def)
{
	const char *e = getenv(k);
	return e && *e ? e : def;
}
#define APLAY env_or("ORCHARD_APLAY", "/usr/bin/aplay")
#define AMIXER env_or("ORCHARD_AMIXER", "/usr/bin/amixer")
#define CAP_STATUS env_or("ORCHARD_CAP_STATUS", "/proc/asound/card0/pcm2c/sub0/status")
#define MIN_START_MB 100	/* free space a recording needs to start */
#define MIN_KEEP_MB 50		/* below this a running one stops and is saved (/data stays usable) */
#define REC_PIPE (1 << 20)	/* arecord → recorder pipe: ~5.5 s of 2 ch 48 kHz rides out a flash stall */
#define RT_PRIO 10	/* like Music's mpg123: SCHED_RR under the IRQ threads */

#define SIDE 32
#define W_CELL (SCR_W - 2 * SIDE)
#define MAXM 150	/* rows built (newest first) */
#define NBARS 62
#define BAR_W 6
#define BAR_GAP 5
#define WAVE_H 170
#define REC_MS 100	/* UI tick while recording (meter + timer: small areas only) */
#define PLAY_MS 250
#define G_MIC "\xef\x84\xb0"
#define G_STOP "\xef\x81\x8d"
#define G_WAVE "\xf3\xb1\x91\xbd"	/* md-waveform */
#define G_REW15 "\xf3\xb1\xa5\x86"	/* md-rewind-15 */
#define G_FF15 "\xf3\xb1\xa4\xba"	/* md-fast-forward-15 */

enum { MIC_BOTTOM, MIC_REAR, MIC_FRONT };
static const char *const mic_names[] = { "Bottom", "Rear", "Front" };

/* ---------- process helpers (worker threads only) ---------- */

static long mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static bool fake_mode(void)
{
	const char *e = getenv("ORCHARD_MEMOS");
#ifdef ORCHARD_PHONE
	return e && *e && strcmp(e, "real");
#else
	return !e || strcmp(e, "real");
#endif
}

static const char *fake_kind(void)
{
	const char *e = getenv("ORCHARD_MEMOS");
	return e ? e : "fake";
}

/* the clock the fake devices are paced by: LVGL's (simulated in snap), else the real one */
static long now_ms(bool fake) { return fake ? (long)lv_tick_get() : mono_ms(); }

/* fork + exec with stdin / stdout / stderr on the given fds (-1 = /dev/null); argv prepared by the caller. */
static pid_t spawn(const char *path, char *const argv[], int in, int out, int err, bool rt)
{
	int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
	pid_t pid = fork();
	if (pid == 0) {
		if (rt) {	/* sound must not wait behind a busy UI (musl's sched_setscheduler is a stub) */
			struct sched_param sp = { .sched_priority = RT_PRIO };
			if (syscall(SYS_sched_setscheduler, 0, SCHED_RR, &sp))
				(void)setpriority(PRIO_PROCESS, 0, -11);
		}
		dup2(in >= 0 ? in : nul, 0);
		dup2(out >= 0 ? out : nul, 1);
		dup2(err >= 0 ? err : nul, 2);
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		execv(path, argv);
		_exit(127);
	}
	if (nul >= 0)
		close(nul);
	return pid;
}

static int wait_exit(pid_t pid)
{
	int st;
	while (waitpid(pid, &st, 0) < 0)
		if (errno != EINTR)
			return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

/* waitpid with a deadline, then SIGKILL */
static int reap(pid_t pid, int ms)
{
	for (int i = 0; i < ms / 10; i++) {
		int st;
		pid_t r = waitpid(pid, &st, WNOHANG);
		if (r == pid)
			return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
		if (r < 0 && errno != EINTR)
			return -1;
		usleep(10000);
	}
	kill(pid, SIGKILL);
	return wait_exit(pid);
}

static int amixer(const char *ctl, const char *val)
{
	char name[96];
	snprintf(name, sizeof(name), "name=%s", ctl);
	char *argv[] = { "amixer", "-q", "-c", "0", val ? "cset" : "cget", name, (char *)val, NULL };
	pid_t pid = spawn(AMIXER, argv, -1, -1, -1, false);
	return pid < 0 ? -1 : wait_exit(pid);
}

/* keep the last part of a child's stderr (one line) */
static int err_take(char *dst, size_t n, int fd)	/* → how many "overrun!!!" lines arecord printed */
{
	char b[512];
	ssize_t r;
	int xruns = 0;
	while ((r = read(fd, b, sizeof(b) - 1)) > 0) {
		b[r] = 0;
		for (const char *q = b; (q = strstr(q, "overrun!!!")); q++)
			xruns++;
		size_t have = strlen(dst);
		if (have + r >= n) {	/* keep the newest */
			size_t drop = have + r - n + 1;
			if (drop >= have)
				dst[0] = 0;
			else
				memmove(dst, dst + drop, have - drop + 1);
		}
		strncat(dst, b, n - strlen(dst) - 1);
	}
	return xruns;
}

static void err_tidy(char *s)
{
	char *nl;
	while ((nl = strrchr(s, '\n')) && !nl[1])
		*nl = 0;
	if ((nl = strrchr(s, '\n')))
		memmove(s, nl + 1, strlen(nl + 1) + 1);
}

/* ---------- mic probe: which controls the codec driver has (once, off the UI thread) ---------- */

static volatile int mics_ok = -1;	/* -1 not known yet, 0 bottom only, 1 rear + front too */

static void *probe_main(void *arg)
{
	(void)arg;
	mics_ok = amixer("ADC1A Capture Volume", NULL) == 0;	/* n61-mic2/3: the fixed mux labels + gains */
	return NULL;
}

static void probe_start(void)
{
	if (mics_ok >= 0)
		return;
	if (fake_mode()) {
		mics_ok = 1;
		return;
	}
	static bool asked;	/* (until it answers, only the bottom mic is offered) */
	if (asked)
		return;
	asked = true;
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &at, probe_main, NULL))
		mics_ok = 0;
	pthread_attr_destroy(&at);
}

/* ---------- recorder thread ---------- */

enum { RS_IDLE, RS_RUN, RS_DONE, RS_FAIL };
static struct {
	pthread_mutex_t mu;
	int st;
	bool stop;
	uint32_t frames;	/* recorded so far */
	int peak;		/* largest |sample| since the UI last looked */
	bool got_peak;
	char err[200];		/* RS_FAIL: why; RS_DONE: a warning (saved, but cut short) */
	char path[512], tmp[512], name[128];
	int mic;
	bool fake;
} R = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* hw:0,2 free? else why not (who has it) */
static bool cap_free(char *why, size_t n)
{
	if (R.fake) {
		const char *k = fake_kind();
		if (!strcmp(k, "busy")) {
			snprintf(why, n, "The microphone is in use by arecord (pid 812).");
			return false;
		}
		if (!strcmp(k, "nomic")) {
			snprintf(why, n, "This kernel has no microphone device (ALSA " CAP_DEV ").");
			return false;
		}
		return true;
	}
	FILE *f = fopen(CAP_STATUS, "r");
	if (!f) {
		snprintf(why, n, "This kernel has no microphone device (ALSA " CAP_DEV ").");
		return false;
	}
	char line[128];
	bool closed = false;
	int pid = 0;
	while (fgets(line, sizeof(line), f)) {
		if (!strncmp(line, "closed", 6))
			closed = true;
		sscanf(line, "owner_pid : %d", &pid);
	}
	fclose(f);
	if (closed)
		return true;
	char comm[64] = "", p[64];
	snprintf(p, sizeof(p), "/proc/%d/comm", pid);
	if (pid > 0 && (f = fopen(p, "r"))) {
		if (fgets(comm, sizeof(comm), f))
			comm[strcspn(comm, "\n")] = 0;
		fclose(f);
	}
	if (comm[0])
		snprintf(why, n, "The microphone is in use by %s (pid %d).", comm, pid);
	else
		snprintf(why, n, "The microphone is in use by another program.");
	return false;
}

/* a synthetic voice: syllables of a buzzy 140 Hz tone, phrases with pauses */
static int16_t fake_sample(uint32_t i)
{
	double t = i / (double)MEMO_RATE;
	double syl = sin(2 * M_PI * 3.1 * t), phrase = sin(2 * M_PI * 0.23 * t);
	double env = syl > 0 ? syl * syl : 0;
	env *= phrase > -0.3 ? 0.35 + 0.65 * (phrase + 0.3) / 1.3 : 0.02;
	double v = sin(2 * M_PI * 140 * t) + 0.5 * sin(2 * M_PI * 280 * t) + 0.25 * sin(2 * M_PI * 560 * t);
	return (int16_t)(v * env * 7000 + ((i * 1103515245u + 12345u) >> 20 & 63) - 32);
}

static long free_mb(void);

/* rec_main's result, published only once the mixer is back (a new recording may start right after) */
static int rec_res;
static char rec_msg[200];
static void rec_fail(const char *why, bool keep)
{
	snprintf(rec_msg, sizeof(rec_msg), "%s", why);
	rec_res = keep ? RS_DONE : RS_FAIL;
}

static void *rec_main(void *arg)
{
	(void)arg;
	char why[200] = "", errtxt[400] = "";
	int pick = R.mic == MIC_BOTTOM ? 0 : 1;
	bool mixed = false;
	uint32_t frames = 0;
	rec_res = RS_FAIL;
	rec_msg[0] = 0;
	if (!cap_free(why, sizeof(why))) {
		rec_fail(why, false);
		goto restore;
	}
	if (!R.fake && R.mic != MIC_BOTTOM) {
		if (amixer("ASP TX2 Mux", R.mic == MIC_FRONT ? "ADC2B" : "ADC2A")) {
			rec_fail("Couldn't switch the codec to that microphone (amixer).", false);
			goto restore;
		}
		mixed = R.mic == MIC_FRONT;
	}
	FILE *f = fopen(R.tmp, "wb");
	if (!f) {
		snprintf(why, sizeof(why), "Couldn't create the file: %s.", strerror(errno));
		rec_fail(why, false);
		goto restore;
	}
	static char fbuf[1 << 16];
	setvbuf(f, fbuf, _IOFBF, sizeof(fbuf));
	uint8_t hdr[MEMO_HDR];
	memo_wav_header(hdr, MEMO_RATE, 1, 0);
	fwrite(hdr, 1, sizeof(hdr), f);

	pid_t pid = -1;
	int out = -1, efd = -1, psz = 0, fulls = 0, xruns = 0;
	bool full = false;
	if (!R.fake) {
		int po[2], pe[2];
		if (pipe2(po, O_CLOEXEC) || pipe2(pe, O_CLOEXEC)) {
			fclose(f);
			unlink(R.tmp);
			rec_fail("Couldn't start the recorder (pipe).", false);
			goto restore;
		}
		char *argv[] = { "arecord", "-q", "-D", CAP_DEV, "-c", "2", "-f", "S16_LE", "-r", "48000", "-t", "raw",
				 NULL };
		pid = access(APLAY, X_OK) ? -1 : spawn(APLAY, argv, -1, po[1], pe[1], true);
		close(po[1]);
		close(pe[1]);
		out = po[0];
		efd = pe[0];
		psz = fcntl(out, F_SETPIPE_SZ, REC_PIPE);
		if (psz < 0) {
			fprintf(stderr, "memos: pipe to %d KiB failed (%s), 64 KiB it is\n", REC_PIPE >> 10, strerror(errno));
			psz = fcntl(out, F_GETPIPE_SZ);
		}
		fcntl(efd, F_SETFL, O_NONBLOCK);
		if (pid < 0) {
			close(out);
			close(efd);
			fclose(f);
			unlink(R.tmp);
			rec_fail("Can't record: aplay (alsa-utils) is missing on the phone.", false);
			goto restore;
		}
	}

	static int16_t in[2 * 2048], mono[2048];
	size_t have = 0;	/* bytes in `in` (a partial frame stays for the next read) */
	long t0 = now_ms(R.fake);
	bool stopping = false, wfail = false, eof = false;
	long stop_at = 0;
	uint32_t space_at = 0;	/* frames when free space is next checked (once a second of audio) */
	while (!eof) {
		bool stop;
		pthread_mutex_lock(&R.mu);
		stop = R.stop;
		pthread_mutex_unlock(&R.mu);
		if (!stopping && frames >= space_at) {
			space_at = frames + MEMO_RATE;
			long mb = free_mb();
			if (mb >= 0 && mb < MIN_KEEP_MB) {
				snprintf(why, sizeof(why),
					 "The phone is almost out of space (%ld MB free), so the recording stopped. What was "
					 "recorded is saved.", mb);
				stop = true;
			}
		}
		if (stop && !stopping) {
			stopping = true;
			stop_at = mono_ms();
			if (pid > 0)
				kill(pid, SIGINT);	/* arecord stops cleanly; what it still has comes out first */
			else
				break;
		}
		int n = 0;	/* frames this pass */
		if (R.fake) {
			long due = (now_ms(true) - t0) * (MEMO_RATE / 1000);
			if ((long)frames >= due) {
				usleep(5000);
				continue;
			}
			n = due - frames > 2048 ? 2048 : (int)(due - frames);
			for (int i = 0; i < n; i++)
				in[i * 2] = in[i * 2 + 1] = fake_sample(frames + i);
		} else {
			struct pollfd pf[2] = { { out, POLLIN, 0 }, { efd, POLLIN, 0 } };
			if (poll(pf, 2, 100) < 0 && errno != EINTR)
				break;
			if (pf[1].revents)
				xruns += err_take(errtxt, sizeof(errtxt), efd);
			int q;	/* a full pipe = arecord blocked on its write = ALSA overruns soon */
			bool now_full = psz > 0 && !ioctl(out, FIONREAD, &q) && q >= psz;
			fulls += now_full && !full;
			full = now_full;
			if (stopping && mono_ms() - stop_at > 1500)
				break;	/* it doesn't stop: reap() kills it */
			if (!frames && !stopping && mono_ms() - t0 > 4000) {
				snprintf(why, sizeof(why), "No sound came from the microphone (ALSA " CAP_DEV ").");
				break;
			}
			if (!(pf[0].revents & (POLLIN | POLLHUP | POLLERR)))
				continue;
			ssize_t r = read(out, (char *)in + have, sizeof(in) - have);
			if (r < 0 && errno == EINTR)
				continue;
			if (r <= 0) {
				eof = true;
				r = 0;
			}
			have += r;
			n = have / 4;
		}
		if (n > 0) {
			int pk = memo_pick(in, n, 2, pick, mono);
			if (!wfail && fwrite(mono, 2, n, f) != (size_t)n) {
				snprintf(why, sizeof(why), "The recording stopped early: %s.", strerror(errno));
				wfail = true;
			}
			if (!R.fake) {
				size_t used = (size_t)n * 4;
				memmove(in, (char *)in + used, have - used);
				have -= used;
			}
			if (wfail) {
				pthread_mutex_lock(&R.mu);
				R.stop = true;
				pthread_mutex_unlock(&R.mu);
				continue;
			}
			frames += n;
			pthread_mutex_lock(&R.mu);
			R.frames = frames;
			if (pk > R.peak)
				R.peak = pk;
			R.got_peak = true;
			pthread_mutex_unlock(&R.mu);
		}
	}
	if (pid > 0) {
		kill(pid, SIGINT);
		int st = reap(pid, 1000);
		xruns += err_take(errtxt, sizeof(errtxt), efd);
		fprintf(stderr, "memos: recorded %.1f s, pipe %d KiB, full %d time(s), arecord overruns %d\n",
			frames / (double)MEMO_RATE, psz >> 10, fulls, xruns);
		close(out);
		close(efd);
		if (!frames && !why[0]) {
			err_tidy(errtxt);
			if (strstr(errtxt, "busy"))
				snprintf(why, sizeof(why), "The microphone is in use by another program.");
			else if (errtxt[0])
				snprintf(why, sizeof(why), "The recorder failed: %s", errtxt);
			else
				snprintf(why, sizeof(why), "The recorder failed (exit %d).", st);
		} else if (!stopping && !why[0]) {
			snprintf(why, sizeof(why), "The microphone stopped by itself; what came before is saved.");
		}
	}
	/* finish the file: real sizes, on the flash, then its real name */
	memo_wav_header(hdr, MEMO_RATE, 1, frames * 2);
	bool ok = frames > 0 && fflush(f) == 0 && fseek(f, 0, SEEK_SET) == 0 && fwrite(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
		  fflush(f) == 0;
	if (ok)
		fsync(fileno(f));
	fclose(f);
	if (!ok || rename(R.tmp, R.path)) {
		unlink(R.tmp);
		rec_fail(why[0] ? why : "Nothing was recorded.", false);
	} else {
		rec_fail(why, true);
	}
restore:
	if (mixed)
		amixer("ASP TX2 Mux", "ADC2A");	/* back to the driver's default: TX2 = rear */
	pthread_mutex_lock(&R.mu);
	R.frames = frames;
	snprintf(R.err, sizeof(R.err), "%s", rec_msg);
	R.st = rec_res;
	pthread_mutex_unlock(&R.mu);
	return NULL;
}

/* ---------- player thread ---------- */

enum { PS_IDLE, PS_RUN, PS_END, PS_FAIL, PS_STOPPED };
static struct {
	pthread_mutex_t mu;
	int st;
	bool stop;
	bool fake;
	char path[512];
	uint32_t start_ms, len_ms;
	uint64_t written;	/* frames handed to aplay */
	uint32_t rate;
	long t0;		/* when the first samples went out (now_ms) */
	char err[200];
	unsigned gen;
} P = { .mu = PTHREAD_MUTEX_INITIALIZER };
static pthread_mutex_t play_dev = PTHREAD_MUTEX_INITIALIZER;	/* one aplay at a time: a seek's new one waits
								 * for the old one to be gone (else hw:0,0 is busy) */

static void *play_main(void *arg)
{
	unsigned gen = (unsigned)(uintptr_t)arg;
	char why[200] = "", errtxt[400] = "";
	uint8_t hb[4096];
	memo_wav_t w;
	int fd = open(P.path, O_RDONLY | O_CLOEXEC);
	struct stat sb;
	ssize_t hn = fd >= 0 ? read(fd, hb, sizeof(hb)) : -1;
	bool fake = P.fake;
	int result = PS_END;
	pid_t pid = -1;
	int sock = -1, efd = -1;
	char *buf = malloc(9600);
	pthread_mutex_lock(&play_dev);
	if (!buf || hn <= 0 || fstat(fd, &sb) || !memo_wav_parse(hb, hn, sb.st_size, &w)) {
		snprintf(why, sizeof(why), "This recording can't be read.");
		result = PS_FAIL;
		goto out;
	}
	uint32_t fb = w.channels * 2;
	uint64_t total = w.data_bytes / fb, pos = (uint64_t)P.start_ms * w.rate / 1000;
	if (pos >= total)
		pos = 0;
	lseek(fd, w.data_off + pos * fb, SEEK_SET);
	pthread_mutex_lock(&P.mu);
	P.rate = w.rate;
	pthread_mutex_unlock(&P.mu);
	if (!fake) {
		int sv[2], pe[2];
		if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) || pipe2(pe, O_CLOEXEC)) {
			snprintf(why, sizeof(why), "Couldn't start the player.");
			result = PS_FAIL;
			goto out;
		}
		int sz = 8192;	/* little queued in the socket: pause / seek answer at once */
		setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
		char rate[16], ch[8];
		snprintf(rate, sizeof(rate), "%u", w.rate);
		snprintf(ch, sizeof(ch), "%d", w.channels);
		char *argv[] = { "aplay", "-q", "-t", "raw", "-f", "S16_LE", "-r", rate, "-c", ch, "-B", "200000", NULL };
		pid = access(APLAY, X_OK) ? -1 : spawn(APLAY, argv, sv[1], -1, pe[1], true);
		close(sv[1]);
		close(pe[1]);
		sock = sv[0];
		efd = pe[0];
		fcntl(efd, F_SETFL, O_NONBLOCK);
		if (pid < 0) {
			snprintf(why, sizeof(why), "Can't play: aplay (alsa-utils) is missing on the phone.");
			result = PS_FAIL;
			goto out;
		}
	}
	size_t blen = 0, boff = 0;
	uint64_t sent = 0;	/* frames */
	long t0 = 0;
	for (;;) {
		pthread_mutex_lock(&P.mu);
		bool stop = P.stop || P.gen != gen;
		pthread_mutex_unlock(&P.mu);
		if (stop) {
			result = PS_STOPPED;
			break;
		}
		if (boff == blen) {
			uint64_t left = (total - pos - sent) * fb;
			size_t want = left < 9600 ? (size_t)left : 9600 / fb * fb;
			ssize_t r = want ? read(fd, buf, want) : 0;
			if (r <= 0)
				break;	/* the end: aplay plays out what it has */
			blen = r - r % fb;
			boff = 0;
			if (!blen)
				break;
		}
		if (fake) {	/* paced by the (simulated) clock, nothing played */
			long now = now_ms(true);
			if (!t0)
				t0 = now;
			uint64_t due = (uint64_t)(now - t0) * w.rate / 1000 + w.rate / 5;
			if (sent >= due) {
				usleep(5000);
				continue;
			}
			sent += (blen - boff) / fb;
			boff = blen;
		} else {
			struct pollfd pf[2] = { { sock, POLLOUT, 0 }, { efd, POLLIN, 0 } };
			if (poll(pf, 2, 50) < 0 && errno != EINTR)
				break;
			if (pf[1].revents & POLLIN)
				err_take(errtxt, sizeof(errtxt), efd);
			if (pf[0].revents & (POLLERR | POLLHUP)) {
				result = PS_FAIL;
				break;
			}
			if (!(pf[0].revents & POLLOUT))
				continue;
			ssize_t r = send(sock, buf + boff, blen - boff, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (r < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				result = PS_FAIL;
				break;
			}
			if (!t0)
				t0 = now_ms(false);
			boff += r;
			sent += r / fb;	/* (whole frames: the buffer holds whole frames, a split one counts next time) */
		}
		pthread_mutex_lock(&P.mu);
		if (P.gen == gen) {
			P.written = pos + sent;
			P.t0 = t0;
		}
		pthread_mutex_unlock(&P.mu);
	}
	if (result == PS_END && !fake) {	/* let aplay finish what it has queued */
		shutdown(sock, SHUT_WR);
		for (;;) {
			int st;
			pid_t r = waitpid(pid, &st, WNOHANG);
			if (r == pid) {
				pid = -1;
				if (!WIFEXITED(st) || WEXITSTATUS(st))
					result = PS_FAIL;
				break;
			}
			pthread_mutex_lock(&P.mu);
			bool stop = P.stop || P.gen != gen;
			pthread_mutex_unlock(&P.mu);
			if (stop) {
				result = PS_STOPPED;
				break;
			}
			usleep(20000);
		}
	} else if (result == PS_END && fake) {
		while (now_ms(true) - t0 < (long)(sent * 1000 / w.rate)) {
			pthread_mutex_lock(&P.mu);
			bool stop = P.stop || P.gen != gen;
			pthread_mutex_unlock(&P.mu);
			if (stop) {
				result = PS_STOPPED;
				break;
			}
			usleep(5000);
		}
	}
out:
	if (pid > 0) {
		kill(pid, SIGTERM);
		reap(pid, 500);
	}
	if (efd >= 0) {
		err_take(errtxt, sizeof(errtxt), efd);
		close(efd);
	}
	if (sock >= 0)
		close(sock);
	if (fd >= 0)
		close(fd);
	pthread_mutex_unlock(&play_dev);
	free(buf);
	if (result == PS_FAIL && !why[0]) {
		err_tidy(errtxt);
		if (strstr(errtxt, "busy"))
			snprintf(why, sizeof(why), "The speaker is in use by another program.");
		else if (errtxt[0])
			snprintf(why, sizeof(why), "%s", errtxt);
		else
			snprintf(why, sizeof(why), "The player stopped.");
	}
	pthread_mutex_lock(&P.mu);
	if (P.gen == gen) {
		P.st = result;
		snprintf(P.err, sizeof(P.err), "%s", why);
	}
	pthread_mutex_unlock(&P.mu);
	return NULL;
}

static bool thread_go(void *(*fn)(void *), void *arg)
{
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	bool ok = pthread_create(&t, &at, fn, arg) == 0;
	pthread_attr_destroy(&at);
	return ok;
}

/* ---------- the recordings ---------- */

struct memo {
	char name[128];	/* file name without ".wav" */
	time_t when;
	long when_ns;	/* (two saved within a second still sort newest first) */
	uint32_t ms;
};
static struct memo *M;
static int nm;
static char sel_name[128];	/* the selected one (by name: survives a rescan) */
static uint32_t sel_pos;	/* its playhead while paused */

static const char *rec_dir(void)
{
	static char d[400];
	if (!d[0]) {
		snprintf(d, sizeof(d), "%s/Recordings", data_dir());
		mkdir(d, 0755);
	}
	return d;
}

/* MB free for recordings, -1 = unknown (never blocks). ORCHARD_MEMOS_FREE_MB="a,b,..." = pretend, one value per
 * call, the last one sticks (test knob) */
static long free_mb(void)
{
	const char *t = getenv("ORCHARD_MEMOS_FREE_MB");
	if (t && *t) {
		static int calls;
		const char *v = t;
		for (int i = 0; i < calls && strchr(v, ','); i++)
			v = strchr(v, ',') + 1;
		calls++;
		return atol(v);
	}
	struct statvfs s;
	if (statvfs(rec_dir(), &s))
		return -1;
	return (long)((unsigned long long)s.f_bavail * s.f_frsize >> 20);
}

static void memo_path(char *buf, size_t n, const char *name)
{
	snprintf(buf, n, "%s/%s.wav", rec_dir(), name);
}

static int by_when(const void *a, const void *b)
{
	const struct memo *x = a, *y = b;
	if (x->when != y->when)
		return x->when < y->when ? 1 : -1;
	if (x->when_ns != y->when_ns)
		return x->when_ns < y->when_ns ? 1 : -1;
	return strcmp(x->name, y->name);
}

static bool recording_busy(void);

/* read the folder (newest first); a ".rec-*" left by a crash becomes a recording again */
static void scan(void)
{
	DIR *d = opendir(rec_dir());
	nm = 0;
	if (!M)
		M = calloc(MAXM * 4, sizeof(*M));
	if (!d || !M) {
		if (d)
			closedir(d);
		return;
	}
	struct dirent *e;
	while ((e = readdir(d)) && nm < MAXM * 4) {
		const char *n = e->d_name;
		size_t len = strlen(n);
		if (len < 5 || strcmp(n + len - 4, ".wav"))
			continue;
		char p[600];
		if (!strncmp(n, ".rec-", 5)) {
			if (recording_busy())
				continue;
			char q[600];
			snprintf(p, sizeof(p), "%s/%s", rec_dir(), n);
			snprintf(q, sizeof(q), "%s/%s", rec_dir(), n + 5);
			if (access(q, F_OK) == 0 || rename(p, q))
				continue;
			n += 5;
			len -= 5;
		}
		if (n[0] == '.' || len - 4 >= sizeof(M[0].name))
			continue;
		snprintf(p, sizeof(p), "%s/%s", rec_dir(), n);
		int fd = open(p, O_RDONLY | O_CLOEXEC);
		if (fd < 0)
			continue;
		uint8_t h[512];
		struct stat sb;
		ssize_t r = read(fd, h, sizeof(h));
		memo_wav_t w;
		bool ok = r > 0 && !fstat(fd, &sb) && memo_wav_parse(h, r, sb.st_size, &w);
		close(fd);
		if (!ok)
			continue;
		struct memo *m = &M[nm++];
		snprintf(m->name, sizeof(m->name), "%.*s", (int)(len - 4), n);
		m->when = sb.st_mtim.tv_sec;
		m->when_ns = sb.st_mtim.tv_nsec;
		m->ms = memo_wav_ms(&w);
	}
	closedir(d);
	qsort(M, nm, sizeof(*M), by_when);
	if (nm > MAXM)
		nm = MAXM;
}

static int find_memo(const char *name)
{
	for (int i = 0; i < nm; i++)
		if (!strcmp(M[i].name, name))
			return i;
	return -1;
}

/* a free name: "Oct 21, 10:45 PM", then "Oct 21, 10:45 PM 2" ... */
static void unique_name(char *out, size_t n, const char *base)
{
	char p[600], tmp[600];
	for (int k = 1; k < 1000; k++) {
		if (k == 1)
			snprintf(out, n, "%s", base);
		else
			snprintf(out, n, "%s %d", base, k);
		memo_path(p, sizeof(p), out);
		snprintf(tmp, sizeof(tmp), "%s/.rec-%s.wav", rec_dir(), out);
		if (access(p, F_OK) && access(tmp, F_OK))
			return;
	}
}

/* ---------- UI ---------- */

static lv_obj_t *root, *content, *list, *empty, *sheet, *rec_btn, *rec_dot, *mic_btn;
static lv_obj_t *pl_box, *pl_name, *pl_when, *pl_slider, *pl_el, *pl_rem, *pl_play, *pl_rew, *pl_ff, *pl_trash;
static lv_obj_t *rc_box, *rc_name, *rc_time, *rc_wave, *rc_mic;
static lv_obj_t *rows[MAXM];
static lv_timer_t *tm;
static uint8_t bars[NBARS];
static int bar_head;	/* next slot (the oldest) */
static bool dragging;
static char shown_el[16], shown_rem[16], shown_timer[24];
static int shown_slider = -1;

enum { U_IDLE, U_REC, U_SAVING };
static int ustate;	/* the record button's state */
static bool playing;	/* the UI's idea: a player thread for sel_name is on */

static bool recording_busy(void)
{
	pthread_mutex_lock(&R.mu);
	bool b = R.st == RS_RUN;
	pthread_mutex_unlock(&R.mu);
	return b;
}

static int cur_mic(void)
{
	int m = prefs_int("memos_mic", MIC_BOTTOM);
	if (m < MIC_BOTTOM || m > MIC_FRONT || mics_ok != 1)
		m = MIC_BOTTOM;
	return m;
}

static void set_text(lv_obj_t *l, char *shown, size_t n, const char *s)
{
	if (!l || !strcmp(shown, s))
		return;
	snprintf(shown, n, "%s", s);
	lv_label_set_text(l, s);
}

static void tick_pace(void);

/* player position now (ms) */
static uint32_t play_pos(void)
{
	pthread_mutex_lock(&P.mu);
	uint64_t written = P.written;
	long t0 = P.t0;
	uint32_t rate = P.rate ? P.rate : MEMO_RATE, start = P.start_ms;
	bool fake = P.fake;
	pthread_mutex_unlock(&P.mu);
	if (!t0)
		return start;
	uint64_t wr_ms = written * 1000 / rate;
	uint64_t at = start + (uint64_t)(now_ms(fake) - t0);
	return (uint32_t)(at < wr_ms ? at : wr_ms);
}

static void player_stop(void)
{
	if (!playing)
		return;
	sel_pos = play_pos();
	pthread_mutex_lock(&P.mu);
	P.stop = true;
	P.gen++;	/* whatever that thread still reports is ignored */
	P.st = PS_IDLE;
	pthread_mutex_unlock(&P.mu);
	playing = false;
}

static void player_start(uint32_t from)
{
	int i = find_memo(sel_name);
	if (i < 0)
		return;
	player_stop();
	if (music_playing())	/* one sound at a time */
		music_toggle();
	if (from + 300 >= M[i].ms)
		from = 0;
	pthread_mutex_lock(&P.mu);
	P.stop = false;
	P.gen++;
	P.st = PS_RUN;
	P.fake = fake_mode();
	memo_path(P.path, sizeof(P.path), sel_name);
	P.start_ms = from;
	P.len_ms = M[i].ms;
	P.written = (uint64_t)from * MEMO_RATE / 1000;
	P.rate = MEMO_RATE;
	P.t0 = 0;
	P.err[0] = 0;
	unsigned gen = P.gen;
	pthread_mutex_unlock(&P.mu);
	if (!thread_go(play_main, (void *)(uintptr_t)gen)) {
		pthread_mutex_lock(&P.mu);
		P.st = PS_IDLE;
		pthread_mutex_unlock(&P.mu);
		return;
	}
	playing = true;
	sel_pos = from;
	power_busy();
}

static void player_show(void);
static void sheet_layout(void);
static void list_build(void);

static void rows_mark(void)
{
	for (int i = 0; i < nm && i < MAXM; i++) {
		if (!rows[i])
			continue;
		lv_obj_t *col = lv_obj_get_user_data(rows[i]);
		if (!col)
			continue;
		lv_obj_t *t = lv_obj_get_child(col, 0);
		bool on = !strcmp(M[i].name, sel_name);
		lv_color_t want = C(on ? pal.tint : pal.label);
		if (!lv_color_eq(lv_obj_get_style_text_color(t, 0), want))
			lv_obj_set_style_text_color(t, want, 0);
	}
}

static void select_memo(const char *name)
{
	if (strcmp(name, sel_name)) {
		player_stop();
		snprintf(sel_name, sizeof(sel_name), "%s", name);
		sel_pos = 0;
	}
	rows_mark();
	sheet_layout();
	player_show();
}

/* ---- delete / rename ---- */

static char act_name[128];	/* the recording a menu / alert / rename page is about */

static void delete_now(int idx, void *ud)
{
	(void)ud;
	if (idx != 1)
		return;
	if (!strcmp(act_name, sel_name)) {
		player_stop();
		sel_name[0] = 0;
	}
	char p[600];
	memo_path(p, sizeof(p), act_name);
	if (unlink(p) && errno != ENOENT) {
		char m[200];
		snprintf(m, sizeof(m), "%s", strerror(errno));
		ui_alert("Couldn't Delete", m, (const char *[]){ "OK" }, 1, NULL, NULL);
	}
	if (!root)
		return;
	scan();
	list_build();
	sheet_layout();
}

static void ask_delete(const char *name)
{
	ui_swipe_close();
	if (name != act_name)
		snprintf(act_name, sizeof(act_name), "%s", name);
	char m[220];
	snprintf(m, sizeof(m), "\"%s\" will be deleted. This can't be undone.", name);
	ui_alert("Delete Recording?", m, (const char *[]){ "Cancel", "!Delete" }, 2, delete_now, NULL);
}

static lv_obj_t *rn_ta;

static void rn_leave(void)
{
	if (!rn_ta)
		return;
	char t[128];
	snprintf(t, sizeof(t), "%s", lv_textarea_get_text(rn_ta));
	rn_ta = NULL;
	if (!memo_clean_name(t, sizeof(t)) || !strcmp(t, act_name))
		return;
	char from[600], to[600];
	memo_path(from, sizeof(from), act_name);
	memo_path(to, sizeof(to), t);
	if (!access(to, F_OK)) {
		ui_alert("Name Taken", "Another recording already has that name.", (const char *[]){ "OK" }, 1, NULL, NULL);
		return;
	}
	bool was_sel = !strcmp(act_name, sel_name);
	if (was_sel)
		player_stop();
	if (rename(from, to)) {
		char m[200];
		snprintf(m, sizeof(m), "%s", strerror(errno));
		ui_alert("Couldn't Rename", m, (const char *[]){ "OK" }, 1, NULL, NULL);
		return;
	}
	if (was_sel)
		snprintf(sel_name, sizeof(sel_name), "%s", t);
	if (!root)
		return;
	scan();
	list_build();
	sheet_layout();
	player_show();
}

static void rn_done(lv_event_t *e)
{
	(void)e;
	if (rn_ta)
		nav_pop();	/* rn_leave renames */
}

static void rename_page(const char *name)
{
	if (name != act_name)
		snprintf(act_name, sizeof(act_name), "%s", name);
	lv_obj_t *c = nav_push("Rename");
	if (!c)
		return;
	lv_obj_t *gap = box(c);
	lv_obj_set_size(gap, 1, 36);
	rn_ta = ui_text_field(c, W_CELL, "Name");
	lv_obj_set_style_pad_hor(lv_obj_get_parent(rn_ta), 32, 0);
	lv_textarea_set_text(rn_ta, name);
	lv_textarea_set_max_length(rn_ta, 100);
	lv_obj_add_event_cb(rn_ta, rn_done, LV_EVENT_READY, NULL);
	nav_button("Done", rn_done, NULL);
	nav_on_leave(rn_leave);
	kbd_attach(rn_ta, KBD_TEXT);	/* typing straight away, like iOS */
}

/* ---- list ---- */

static int row_idx(lv_event_t *e) { return (int)(intptr_t)lv_event_get_user_data(e); }

static void row_tap(lv_event_t *e)
{
	int i = row_idx(e);
	if (i < nm && ustate == U_IDLE)
		select_memo(M[i].name);	/* selects only: nothing plays until ▶ */
}

static void row_swipe(lv_event_t *e)
{
	int i = row_idx(e);
	if (i < nm)
		ask_delete(M[i].name);
}

enum { MN_PLAY = 1, MN_RENAME, MN_DELETE };
static void row_menu_pick(int id)
{
	if (find_memo(act_name) < 0)
		return;
	if (id == MN_PLAY) {
		select_memo(act_name);
		player_start(sel_pos);
		player_show();
		tick_pace();
	} else if (id == MN_RENAME) {
		rename_page(act_name);
	} else if (id == MN_DELETE) {
		ask_delete(act_name);
	}
}

static void row_hold(lv_event_t *e)
{
	int i = row_idx(e);
	if (i >= nm || ustate != U_IDLE)
		return;
	snprintf(act_name, sizeof(act_name), "%s", M[i].name);
	static const ui_menu_item_t it[] = {
		{ MN_PLAY, 0, G_PLAY, "Play" },
		{ MN_RENAME, 0, G_PEN, "Rename" },
		{ MN_DELETE, MI_RED | MI_GROUP, G_TRASH, "Delete" },
	};
	ui_row_menu(lv_event_get_target(e), it, 3, row_menu_pick);
}

static void list_build(void)
{
	if (!list)
		return;
	lv_obj_clean(list);
	memset(rows, 0, sizeof(rows));
	if (!nm) {
		lv_obj_remove_flag(empty, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_add_flag(empty, LV_OBJ_FLAG_HIDDEN);
	lv_obj_t *sec = ui_section(list, NULL, nm >= MAXM ? "Only the newest 150 are shown." : NULL);
	time_t now = time(NULL);
	bool h24 = prefs_int("clock24", 0);
	for (int i = 0; i < nm; i++) {
		char when[48], dur[16];
		memo_fmt_when(when, sizeof(when), M[i].when, now, h24);
		memo_fmt_dur(dur, sizeof(dur), M[i].ms);
		lv_obj_t *r = ui_row(sec, NULL, 0, M[i].name);
		ui_row_subtitle(r, when);
		ui_row_value(r, dur);
		ui_row_tap(r, row_tap, (void *)(intptr_t)i);
		ui_row_hold(r, row_hold, (void *)(intptr_t)i);
		ui_row_swipe(r, G_TRASH, "Delete", pal.red, row_swipe, (void *)(intptr_t)i);
		rows[i] = r;
	}
	if (find_memo(sel_name) < 0) {
		player_stop();
		sel_name[0] = 0;
	}
	rows_mark();
}

/* ---- the player card ---- */

static void times_show(uint32_t pos, uint32_t len)
{
	char a[16], b[20];
	memo_fmt_dur(a, sizeof(a), pos);
	memo_fmt_dur(b + 1, sizeof(b) - 1, len > pos ? len - pos : 0);
	b[0] = '-';
	set_text(pl_el, shown_el, sizeof(shown_el), a);
	set_text(pl_rem, shown_rem, sizeof(shown_rem), b);
}

static void player_show(void)
{
	int i = find_memo(sel_name);
	if (!pl_box || i < 0)
		return;
	if (strcmp(lv_label_get_text(pl_name), M[i].name))
		lv_label_set_text(pl_name, M[i].name);
	char when[48];
	memo_fmt_when(when, sizeof(when), M[i].when, time(NULL), prefs_int("clock24", 0));
	if (strcmp(lv_label_get_text(pl_when), when))
		lv_label_set_text(pl_when, when);
	uint32_t pos = playing ? play_pos() : sel_pos, len = M[i].ms;
	if (pos > len)
		pos = len;
	if (!dragging) {
		int v = len ? (int)((uint64_t)pos * 1000 / len) : 0;
		if (v != shown_slider) {
			lv_slider_set_value(pl_slider, v, LV_ANIM_OFF);
			shown_slider = v;
		}
		times_show(pos, len);
	}
	lv_obj_t *g = lv_obj_get_child(pl_play, 0);
	ui_icon_set(g, playing ? G_PAUSE : G_PLAY);
}

static void play_cb(lv_event_t *e)
{
	(void)e;
	if (!sel_name[0] || ustate != U_IDLE)
		return;
	if (playing)
		player_stop();
	else
		player_start(sel_pos);
	player_show();
	tick_pace();
}

static void skip(int dms)
{
	int i = find_memo(sel_name);
	if (i < 0 || ustate != U_IDLE)
		return;
	long p = (long)(playing ? play_pos() : sel_pos) + dms;
	if (p < 0)
		p = 0;
	if (p > (long)M[i].ms)
		p = M[i].ms;
	if (playing) {
		if (p >= (long)M[i].ms) {
			player_stop();
			sel_pos = 0;
		} else {
			player_start((uint32_t)p);
		}
	} else {
		sel_pos = (uint32_t)p;
	}
	player_show();
}

static void rew_cb(lv_event_t *e) { (void)e; skip(-15000); }
static void ff_cb(lv_event_t *e) { (void)e; skip(15000); }

static void trash_cb(lv_event_t *e)
{
	(void)e;
	if (sel_name[0] && ustate == U_IDLE)
		ask_delete(sel_name);
}

static void seek_ev(lv_event_t *e)
{
	lv_event_code_t c = lv_event_get_code(e);
	int i = find_memo(sel_name);
	if (i < 0)
		return;
	uint32_t len = M[i].ms, to = (uint32_t)((uint64_t)lv_slider_get_value(pl_slider) * len / 1000);
	if (c == LV_EVENT_PRESSED) {
		dragging = true;
	} else if (c == LV_EVENT_VALUE_CHANGED) {
		if (dragging)
			times_show(to, len);
	} else if (dragging) {	/* released / press lost */
		dragging = false;
		shown_slider = -1;
		if (playing)
			player_start(to);
		else
			sel_pos = to;
		player_show();
	}
}

/* ---- recording ---- */

static void dot_shape(void *o, int32_t v)	/* 0 = round dot, 256 = the stop square */
{
	int s = 108 - (108 - 52) * v / 256, r = 54 - (54 - 12) * v / 256;
	lv_obj_set_size(o, s, s);
	lv_obj_set_style_radius(o, r, 0);
	lv_obj_center(o);
}

static void dot_to(bool square)
{
	if (rec_dot)
		motion_anim(rec_dot, dot_shape, square ? 0 : 256, square ? 256 : 0, MOTION_SMALL, NULL);
}

static void wave_draw(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_obj_t *o = lv_event_get_target(e);
	lv_area_t a;
	lv_obj_get_coords(o, &a);
	int h = lv_area_get_height(&a), w = lv_area_get_width(&a), cy = a.y1 + h / 2;
	int x0 = a.x1 + (w - (NBARS * (BAR_W + BAR_GAP) - BAR_GAP)) / 2;
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_opa = LV_OPA_COVER;
	d.radius = BAR_W / 2;
	lv_area_t mid = { a.x1, cy, a.x2, cy };	/* the zero line */
	d.bg_color = C(pal.separator);
	lv_draw_rect(layer, &d, &mid);
	for (int i = 0; i < NBARS; i++) {
		int lv = bars[(bar_head + i) % NBARS];
		int bh = BAR_W + lv * (h - BAR_W) / 100;
		lv_area_t r = { x0 + i * (BAR_W + BAR_GAP), cy - bh / 2, x0 + i * (BAR_W + BAR_GAP) + BAR_W - 1,
				cy - bh / 2 + bh - 1 };
		d.bg_color = C(lv ? pal.red : pal.label3);
		lv_draw_rect(layer, &d, &r);
	}
}

static void rec_start(void)
{
	if (ustate != U_IDLE || recording_busy())
		return;
	long mb = free_mb();
	if (mb >= 0 && mb < MIN_START_MB) {
		char m[160];
		snprintf(m, sizeof(m), "Only %ld MB is free on the phone. Recording needs at least %d MB; delete some "
			 "recordings or other files first.", mb, MIN_START_MB);
		ui_alert("Not Enough Storage", m, (const char *[]){ "OK" }, 1, NULL, NULL);
		return;
	}
	player_stop();
	if (music_playing())
		music_toggle();
	char base[128], name[128];
	memo_default_name(base, sizeof(base), time(NULL), prefs_int("clock24", 0));
	unique_name(name, sizeof(name), base);
	pthread_mutex_lock(&R.mu);
	R.st = RS_RUN;
	R.stop = false;
	R.frames = 0;
	R.peak = 0;
	R.got_peak = false;
	R.err[0] = 0;
	R.mic = cur_mic();
	R.fake = fake_mode();
	snprintf(R.name, sizeof(R.name), "%s", name);
	memo_path(R.path, sizeof(R.path), name);
	snprintf(R.tmp, sizeof(R.tmp), "%s/.rec-%s.wav", rec_dir(), name);
	pthread_mutex_unlock(&R.mu);
	if (!thread_go(rec_main, NULL)) {
		pthread_mutex_lock(&R.mu);
		R.st = RS_IDLE;
		pthread_mutex_unlock(&R.mu);
		return;
	}
	memset(bars, 0, sizeof(bars));
	bar_head = 0;
	shown_timer[0] = 0;
	ustate = U_REC;
	ui_haptic_tap();
	power_busy();
	power_awake_for("voicememo", 3000);
	if (rc_name)
		lv_label_set_text(rc_name, name);
	if (rc_mic) {
		char m[48];
		snprintf(m, sizeof(m), "%s Microphone", mic_names[R.mic]);
		lv_label_set_text(rc_mic, m);
	}
	dot_to(true);
	sheet_layout();
	tick_pace();
}

static void rec_stop(void)
{
	if (ustate != U_REC)
		return;
	pthread_mutex_lock(&R.mu);
	R.stop = true;
	pthread_mutex_unlock(&R.mu);
	ustate = U_SAVING;
	dot_to(false);
	if (rec_btn)
		lv_obj_set_style_opa(rec_btn, LV_OPA_50, 0);
}

static void rec_cb(lv_event_t *e)
{
	(void)e;
	if (ustate == U_IDLE)
		rec_start();
	else if (ustate == U_REC)
		rec_stop();
}

/* the recorder finished (or failed): back to idle, the new one selected (not playing) */
static void rec_finished(void)
{
	pthread_mutex_lock(&R.mu);
	int st = R.st;
	char err[200], name[128];
	snprintf(err, sizeof(err), "%s", R.err);
	snprintf(name, sizeof(name), "%s", R.name);
	R.st = RS_IDLE;
	pthread_mutex_unlock(&R.mu);
	bool was = ustate != U_IDLE;
	ustate = U_IDLE;
	power_keep_awake("voicememo", false);	/* the auto-lock countdown starts now */
	if (!root)
		return;
	if (was)
		dot_to(false);
	if (rec_btn)
		lv_obj_set_style_opa(rec_btn, LV_OPA_COVER, 0);
	scan();
	if (st == RS_DONE && find_memo(name) >= 0) {
		snprintf(sel_name, sizeof(sel_name), "%s", name);
		sel_pos = 0;
		playing = false;
	}
	list_build();
	sheet_layout();
	player_show();
	if (st == RS_FAIL)
		ui_alert("Can't Record", err[0] ? err : "The recording failed.", (const char *[]){ "OK" }, 1, NULL, NULL);
	else if (err[0])
		ui_alert("Recording Saved", err, (const char *[]){ "OK" }, 1, NULL, NULL);
}

/* ---- the microphone menu ---- */

static void mic_pick(int id)
{
	if (id >= 10 && id <= 12 && ustate == U_IDLE)
		prefs_set_int("memos_mic", id - 10);
}

static void mic_cb(lv_event_t *e)
{
	(void)e;
	if (ustate != U_IDLE)
		return;
	int m = cur_mic();
	ui_menu_item_t it[5];
	int n = 0;
	it[n++] = (ui_menu_item_t){ 0, MI_HEAD, NULL, "Microphone" };
	for (int i = 0; i < 3; i++) {
		if (i && mics_ok != 1)
			break;
		it[n++] = (ui_menu_item_t){ 10 + i, MI_CHECK | (m == i ? MI_ON : 0), NULL, mic_names[i] };
	}
	if (mics_ok != 1)
		it[n++] = (ui_menu_item_t){ 0, MI_HEAD | MI_GROUP, NULL, "Rear + front need the newer audio driver" };
	ui_menu(it, n, SCR_W - SIDE, STATUS_H + NAV_H, STATUS_H, mic_pick);
}

/* ---- layout: the sheet at the bottom grows for the player card / the recording ---- */

#define BTN_AREA 236
#define PL_H 300
#define RC_H 420

static int sheet_h;

static void sheet_layout(void)
{
	if (!sheet)
		return;
	bool rec = ustate != U_IDLE, pl = !rec && find_memo(sel_name) >= 0;
	int h = BTN_AREA + (rec ? RC_H : pl ? PL_H : 0);
	if (lv_obj_has_flag(rc_box, LV_OBJ_FLAG_HIDDEN) == rec)
		rec ? lv_obj_remove_flag(rc_box, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(rc_box, LV_OBJ_FLAG_HIDDEN);
	if (lv_obj_has_flag(pl_box, LV_OBJ_FLAG_HIDDEN) == pl)
		pl ? lv_obj_remove_flag(pl_box, LV_OBJ_FLAG_HIDDEN) : lv_obj_add_flag(pl_box, LV_OBJ_FLAG_HIDDEN);
	if (h != sheet_h) {
		sheet_h = h;
		lv_obj_set_y(sheet, SCR_H - h);
		lv_obj_set_height(sheet, h + 60);	/* the rounded bottom corners stay off screen */
		lv_obj_set_y(rec_btn, h - BOTTOM_PAD - 40 - 136);
		lv_obj_set_style_pad_bottom(content, h + 24, 0);
	}
	lv_obj_set_style_opa(mic_btn, rec ? LV_OPA_40 : LV_OPA_COVER, 0);
}

static lv_obj_t *ctl_btn(lv_obj_t *p, const char *glyph, int size, int ink, lv_event_cb_t cb)
{
	lv_obj_t *b = ui_icon_button(p, glyph, size, pal.cell, pal.label);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_t *g = lv_obj_get_child(b, 0);
	lv_obj_set_style_text_font(g, ui_icon_font(glyph, ink), 0);
	ui_glyph_fit(g);
	lv_obj_center(g);
	lv_obj_set_ext_click_area(b, 12);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

static lv_obj_t *lbl(lv_obj_t *p, const char *s, lv_font_t *f, uint32_t col, int x, int y, int w)
{
	lv_obj_t *l = text(p, s, f, col);
	lv_obj_set_pos(l, x, y);
	if (w) {
		lv_obj_set_width(l, w);
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	}
	return l;
}

static void build_sheet(lv_obj_t *page)
{
	sheet = box(page);
	sheet_h = 0;
	lv_obj_add_flag(sheet, LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_set_size(sheet, SCR_W, BTN_AREA + 60);
	lv_obj_set_pos(sheet, 0, SCR_H - BTN_AREA);
	lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(sheet, C(pal.cell), 0);
	lv_obj_set_style_radius(sheet, 44, 0);
	lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);	/* taps on it don't reach the list under it */

	/* the player card: name, when, scrubber, -15 / play / +15, trash */
	pl_box = box(sheet);
	lv_obj_set_size(pl_box, SCR_W, PL_H);
	lv_obj_set_pos(pl_box, 0, 0);
	lv_obj_add_flag(pl_box, LV_OBJ_FLAG_HIDDEN);
	pl_name = lbl(pl_box, "", font(F_TEXT_SEMI, 36), pal.label, SIDE + 8, 34, W_CELL - 16);
	pl_when = lbl(pl_box, "", font(F_TEXT, 26), pal.label2, SIDE + 8, 84, W_CELL - 16);
	pl_slider = ui_slider(pl_box, W_CELL - 16, 0, 1000, 0);
	lv_obj_set_pos(pl_slider, SIDE + 8, 146);
	lv_obj_set_style_bg_color(pl_slider, C(mix(pal.label2, pal.cell, 55)), 0);
	lv_obj_set_style_bg_color(pl_slider, C(pal.label2), LV_PART_INDICATOR);
	lv_obj_set_style_pad_all(pl_slider, 8, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(pl_slider, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_bg_color(pl_slider, C(pal.label), LV_PART_KNOB);
	lv_obj_remove_flag(pl_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(pl_slider, seek_ev, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(pl_slider, seek_ev, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(pl_slider, seek_ev, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(pl_slider, seek_ev, LV_EVENT_PRESS_LOST, NULL);
	pl_el = lbl(pl_box, "0:00", font(F_TEXT_MEDIUM, 24), pal.label2, SIDE + 8, 172, 0);
	pl_rem = lbl(pl_box, "-0:00", font(F_TEXT_MEDIUM, 24), pal.label2, SCR_W - SIDE - 8 - 200, 172, 200);
	lv_obj_set_style_text_align(pl_rem, LV_TEXT_ALIGN_RIGHT, 0);
	shown_el[0] = shown_rem[0] = 0;
	shown_slider = -1;
	int cy = 252;
	pl_rew = ctl_btn(pl_box, G_REW15, 96, 46, rew_cb);
	lv_obj_set_pos(pl_rew, SCR_W / 2 - 150 - 48, cy - 48);
	pl_play = ctl_btn(pl_box, G_PLAY, 110, 54, play_cb);
	lv_obj_set_pos(pl_play, SCR_W / 2 - 55, cy - 55);
	pl_ff = ctl_btn(pl_box, G_FF15, 96, 46, ff_cb);
	lv_obj_set_pos(pl_ff, SCR_W / 2 + 150 - 48, cy - 48);
	pl_trash = ctl_btn(pl_box, G_TRASH, 84, 34, trash_cb);
	lv_obj_set_pos(pl_trash, SCR_W - SIDE - 84, cy - 42);
	lv_obj_t *tg = lv_obj_get_child(pl_trash, 0);
	lv_obj_set_style_text_color(tg, C(pal.tint), 0);

	/* the recording: its name, the timer, the level bars */
	rc_box = box(sheet);
	lv_obj_set_size(rc_box, SCR_W, RC_H);
	lv_obj_set_pos(rc_box, 0, 0);
	lv_obj_add_flag(rc_box, LV_OBJ_FLAG_HIDDEN);
	rc_name = lbl(rc_box, "", font(F_TEXT_SEMI, 36), pal.label, SIDE, 40, W_CELL);
	lv_obj_set_style_text_align(rc_name, LV_TEXT_ALIGN_CENTER, 0);
	rc_mic = lbl(rc_box, "", font(F_TEXT, 26), pal.label2, SIDE, 90, W_CELL);
	lv_obj_set_style_text_align(rc_mic, LV_TEXT_ALIGN_CENTER, 0);
	rc_wave = box(rc_box);
	lv_obj_set_size(rc_wave, W_CELL, WAVE_H);
	lv_obj_set_pos(rc_wave, SIDE, 146);
	lv_obj_add_event_cb(rc_wave, wave_draw, LV_EVENT_DRAW_MAIN, NULL);
	rc_time = lbl(rc_box, "00:00.00", font(F_MONO, 64), pal.label, SIDE, 330, W_CELL);
	lv_obj_set_style_text_align(rc_time, LV_TEXT_ALIGN_CENTER, 0);

	/* the record button: a ring with the red dot (a red square while recording) */
	rec_btn = box(sheet);
	lv_obj_set_size(rec_btn, 136, 136);
	lv_obj_set_pos(rec_btn, (SCR_W - 136) / 2, BTN_AREA - BOTTOM_PAD - 40 - 136);
	lv_obj_set_style_radius(rec_btn, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(rec_btn, 7, 0);
	lv_obj_set_style_border_color(rec_btn, C(mix(pal.label, pal.cell, 25)), 0);
	lv_obj_add_flag(rec_btn, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(rec_btn, 16);
	lv_obj_add_event_cb(rec_btn, rec_cb, LV_EVENT_CLICKED, NULL);
	rec_dot = box(rec_btn);
	lv_obj_remove_flag(rec_dot, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_opa(rec_dot, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(rec_dot, C(pal.red), 0);
	lv_obj_set_style_color_filter_dsc(rec_btn, &lv_color_filter_shade, LV_STATE_PRESSED);
	lv_obj_set_style_color_filter_opa(rec_btn, LV_OPA_30, LV_STATE_PRESSED);
	dot_shape(rec_dot, ustate == U_IDLE ? 0 : 256);
	if (ustate == U_SAVING)
		lv_obj_set_style_opa(rec_btn, LV_OPA_50, 0);
}

static void build_empty(lv_obj_t *c)
{
	empty = box(c);
	lv_obj_set_size(empty, W_CELL, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(empty, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(empty, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(empty, 190, 0);
	lv_obj_set_style_pad_row(empty, 18, 0);
	ui_icon(empty, G_WAVE, ui_icon_font(G_WAVE, 120), pal.label3);
	lv_obj_t *t = text(empty, "No Recordings", font(F_TEXT_SEMI, 44), pal.label);
	lv_obj_set_style_pad_top(t, 12, 0);
	lv_obj_t *s = text(empty, "Tap the record button to start a voice memo.", font(F_TEXT, 30), pal.label2);
	lv_obj_set_width(s, W_CELL - 80);
	lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
}

/* ---- the timer: recorder + player progress (paused while neither runs) ---- */

static void tick(lv_timer_t *t)
{
	(void)t;
	if (!root)
		return;
	pthread_mutex_lock(&R.mu);
	int rst = R.st;
	uint32_t frames = R.frames;
	int peak = R.peak;
	bool got = R.got_peak;
	R.peak = 0;
	R.got_peak = false;
	pthread_mutex_unlock(&R.mu);
	if (rst == RS_DONE || rst == RS_FAIL) {
		rec_finished();
	} else if (rst == RS_RUN) {
		static uint32_t lease_at;
		if (!lock_visible() && lv_tick_elaps(lease_at) > 1000) {	/* recording isn't idle: no auto-lock meanwhile
										   (the Lock Screen goes dark as usual) */
			lease_at = lv_tick_get();
			power_awake_for("voicememo", 3000);
		}
		if (ustate == U_REC) {
			char b[24];
			memo_fmt_timer(b, sizeof(b), (uint32_t)((uint64_t)frames * 1000 / MEMO_RATE));
			set_text(rc_time, shown_timer, sizeof(shown_timer), b);
			bars[bar_head] = got ? memo_level(peak) : 0;
			bar_head = (bar_head + 1) % NBARS;
			lv_obj_invalidate(rc_wave);
		}
	}
	if (playing) {
		pthread_mutex_lock(&P.mu);
		int pst = P.st;
		char err[200];
		snprintf(err, sizeof(err), "%s", P.err);
		if (pst != PS_RUN)
			P.st = PS_IDLE;
		pthread_mutex_unlock(&P.mu);
		if (pst == PS_END) {	/* back to the start, like iOS */
			playing = false;
			sel_pos = 0;
		} else if (pst == PS_FAIL) {
			playing = false;
			sel_pos = 0;
			ui_alert("Can't Play", err, (const char *[]){ "OK" }, 1, NULL, NULL);
		} else if (pst != PS_RUN) {
			playing = false;
		}
		player_show();
	}
	tick_pace();
}

static void tick_pace(void)
{
	if (!tm)
		return;
	bool rec = ustate != U_IDLE || recording_busy();
	uint32_t want = rec ? REC_MS : PLAY_MS;
	if (!rec && !playing) {
		lv_timer_pause(tm);
		return;
	}
	if (lv_timer_get_paused(tm))
		lv_timer_resume(tm);
	lv_timer_set_period(tm, want);
}

/* the lock screen came up: pause playback; a recording goes on (iOS) */
static void lock_changed(bool locked)
{
	if (!locked)
		return;
	if (playing) {
		player_stop();
		player_show();
	}
	tick_pace();
}

static void memos_open(lv_obj_t *r)
{
	root = r;
	probe_start();
	content = nav_root(root, "All Recordings");
	lv_obj_t *page = lv_obj_get_parent(content);
	mic_btn = nav_button(G_MIC, mic_cb, NULL);
	list = box(content);
	lv_obj_set_size(list, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	build_empty(content);
	pthread_mutex_lock(&R.mu);
	int rst = R.st;
	pthread_mutex_unlock(&R.mu);
	ustate = rst == RS_RUN ? U_SAVING : U_IDLE;	/* (closing stopped it; it may still be saving) */
	build_sheet(page);
	scan();
	list_build();
	sheet_layout();
	player_show();
	tm = lv_timer_create(tick, PLAY_MS, NULL);
	lock_listen(lock_changed, true);
	if (rst == RS_DONE || rst == RS_FAIL)	/* it finished while the app was closed */
		rec_finished();
	tick_pace();
}

static void memos_close(void)
{
	rec_stop();	/* Home / app closed: the recording is saved by its thread */
	player_stop();
	rn_ta = NULL;
	lock_listen(lock_changed, false);
	if (tm)
		lv_timer_delete(tm);
	tm = NULL;
	if (ustate == U_IDLE)
		power_keep_awake("voicememo", false);
	root = content = list = empty = sheet = rec_btn = rec_dot = mic_btn = NULL;
	pl_box = pl_name = pl_when = pl_slider = pl_el = pl_rem = pl_play = pl_rew = pl_ff = pl_trash = NULL;
	rc_box = rc_name = rc_time = rc_wave = rc_mic = NULL;
	memset(rows, 0, sizeof(rows));
	dragging = false;
}

const app_t app_memos = { .name = "Voice Memos", .glyph = G_MIC, .icon = IC_DARK, .open = memos_open,
			  .close = memos_close };
