/* Music: MP3s stored on the phone (data_dir()/Music, /root/Music, /tmp/music), plus an optional remote
 * library through a relay that speaks a small plain-HTTP API (no relay ships with this project; with none
 * configured Music is a local player). Relays, tried in order by the ping: one over the USB link (fast, while
 * plugged in), then one over Wi-Fi. The one that answered serves everything until a request
 * finds it gone (→ ping again at once) or the next ping (every minute) finds the PC back. Network jobs run in three worker threads (jobs in, results out, drained
 * on the UI thread by the engine timer; one thread only does pings + songs, so a stalled list or
 * cover never holds those up); playback is one mpg123 in remote mode (simulated in the snap/sdl
 * previews). Playback + queue live in statics and keep going with the app closed. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "orchard.h"

void lv_image_cache_drop(const void *src);	/* LVGL, only in its private headers */

#define RELAY ""	/* optional relay over USB (e.g. "172.16.43.2:8650"): ORCHARD_MUSICD=host:port, else pref music_relay */
#define RELAY2 ""	/* optional relay over Wi-Fi: ORCHARD_MUSICD2, else pref music_relay2; empty / "off" = none */
#ifndef MPG123
#define MPG123 "/usr/bin/mpg123"
#endif
#ifndef SONG_DIR
#define SONG_DIR "/tmp/orchard-music"
#endif
#define MP_RT_PRIO 10		/* mpg123: SCHED_RR, under the kernel's IRQ threads (50) */
#define SONG_CACHE_MAX (120L << 20)
#define MAX_TRACKS 1500
#define MAX_ROWS 150		/* rows built per page ("Show More" for the rest) */
#define MAX_SHELVES 8
#define MAX_LISTS 64
#define SIDE 32
#define ROW_H 112
#define THUMB 88
#define CARD 300
#define NP_ART 560
#define NP_X ((SCR_W - NP_ART) / 2)
#define NP_TOP 108
#define MINI_H 112
#define TABBAR_TOP (SCR_H - BOTTOM_PAD - 6 - 124)	/* ui_tabbar() geometry */
#define MINI_Y (TABBAR_TOP - 12 - MINI_H)

#define GM_NOTE "\xf3\xb0\x8e\x87"	/* md-music_note */
#define GM_HOME "\xf3\xb0\x8b\x9c"
#define GM_LIBRARY "\xf3\xb0\x8c\xb3"	/* md-music_box_multiple */
#define GM_SEARCH "\xf3\xb0\x8d\x89"
#define GM_SHUFFLE "\xef\x81\xb4"
#define GM_REPEAT "\xf3\xb0\x91\x96"
#define GM_REPEAT1 "\xf3\xb0\x91\x98"
#define GM_HEART "\xef\x80\x84"
#define GM_PHONE "\xf3\xb0\x84\x9c"
#define GM_PLAYLIST "\xf3\xb0\xb2\xb8"
#define GM_QUEUE "\xf3\xb0\x90\x91"	/* md-playlist_play */
#define GM_CABLE "\xf3\xb1\x8e\x94"
#define GM_RETRY "\xef\x80\xa1"

/* ---------- tracks ---------- */

typedef struct track {
	char id[24];		/* relay video id, "" = MP3 on the phone */
	char title[120], artist[96];
	int secs;
	const char *path;	/* MP3 on the phone (interned, never freed) */
	int ord;		/* queue order before shuffling */
} track_t;

typedef struct tlist {
	track_t *v;
	int n, cap;
} tlist_t;

/* copy without cutting a UTF-8 sequence in half */
static void ucopy(char *dst, int cap, const char *src, int n)
{
	if (n < 0)
		n = strlen(src);
	if (n > cap - 1) {
		n = cap - 1;
		while (n > 0 && ((unsigned char)src[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(dst, src, n);
	dst[n] = 0;
}

static void tl_add(tlist_t *l, const track_t *t)
{
	if (l->n >= MAX_TRACKS)
		return;
	if (l->n == l->cap) {
		int cap = l->cap ? l->cap * 2 : 32;
		track_t *v = realloc(l->v, cap * sizeof(*v));
		if (!v)
			return;
		l->v = v;
		l->cap = cap;
	}
	l->v[l->n++] = *t;
}

static void tl_free(tlist_t *l)
{
	free(l->v);
	*l = (tlist_t){ 0 };
}

static char *next_line(char **s)
{
	char *l = *s;
	if (!l || !*l)
		return NULL;
	char *e = strchr(l, '\n');
	*s = e ? e + 1 : l + strlen(l);
	if (e)
		*e = 0;
	size_t n = strlen(l);
	if (n && l[n - 1] == '\r')
		l[n - 1] = 0;
	return l;
}

static int split_tabs(char *l, char **f, int max)
{
	int n = 0;
	while (l && n < max) {
		f[n++] = l;
		l = strchr(l, '\t');
		if (l)
			*l++ = 0;
	}
	return n;
}

/* id  title  artists  album  seconds */
static bool parse_track(char *line, track_t *t)
{
	char *f[5];
	int n = split_tabs(line, f, 5);
	if (n < 2 || !*f[0] || strlen(f[0]) >= sizeof(t->id))
		return false;
	memset(t, 0, sizeof(*t));
	strcpy(t->id, f[0]);
	ucopy(t->title, sizeof(t->title), f[1], -1);
	if (n > 2)
		ucopy(t->artist, sizeof(t->artist), f[2], -1);
	if (n > 4)
		t->secs = atoi(f[4]);
	return true;
}

static void parse_tracks(char *body, tlist_t *l)
{
	track_t t;
	char *s = body, *ln;
	while ((ln = next_line(&s)))
		if (parse_track(ln, &t))
			tl_add(l, &t);
}

static void fmt_time(char *b, int n, int s)
{
	if (s < 0)
		s = 0;
	if (s >= 3600)
		snprintf(b, n, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(b, n, "%d:%02d", s / 60, s % 60);
}

/* ---------- relay client: tiny HTTP/1.0 GET in three worker threads ---------- */

enum { J_PING, J_HOME, J_LISTS, J_LIST, J_SEARCH, J_RADIO, J_PLAYED, J_ART, J_SONG, J_SCAN };
enum { LANES = 3, SONG_LANE = 2 };	/* worker threads; SONG_LANE only takes pings and songs */

typedef struct job {
	struct job *next;
	int kind, gen, px;
	char id[96];		/* video / playlist id */
	char path[600];		/* request path */
	int status;		/* HTTP status; 0 = relay unreachable, -1 = transfer broke / timed out */
	char *body;
	int len;
	tlist_t tl;		/* J_SCAN result */
} job_t;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static job_t *todo, *finished;
static char pins[2][24];	/* under mu: the current + next song, which the cache trim never deletes */
static char local_dirs[3][300];
enum { MAX_RELAYS = 2 };
static struct { char host[64], port[8]; bool wifi; } relays[MAX_RELAYS];	/* wifi: the RELAY2 slot */
static int nrelays, relay_cur;	/* relay_cur: the one requests go to (under mu: set by the ping worker) */

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

static int relay_now(void)
{
	pthread_mutex_lock(&mu);
	int r = relay_cur;
	pthread_mutex_unlock(&mu);
	return r;
}

static int relay_connect(int r, int ms)
{
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV };
	struct addrinfo *ai;
	if (getaddrinfo(relays[r].host, relays[r].port, &hints, &ai))
		return -1;
	int s = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (s >= 0 && connect(s, ai->ai_addr, ai->ai_addrlen) < 0) {
		struct pollfd p = { .fd = s, .events = POLLOUT };
		int err = errno;
		socklen_t el = sizeof(err);
		if (err != EINPROGRESS || poll(&p, 1, ms) != 1 || getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) ||
		    err) {
			close(s);
			s = -1;
		}
	}
	freeaddrinfo(ai);
	return s;
}

/* where the body goes: a 200 answer into the file (if any), anything else into memory */
struct sink {
	int fd, status, max, len, cap;
	char *buf;
	bool fail;
};

static void sink_put(struct sink *k, const char *d, int n)
{
	if (k->status == 200 && k->fd >= 0) {
		while (n > 0) {
			ssize_t w = write(k->fd, d, n);
			if (w < 0 && errno == EINTR)
				continue;
			if (w <= 0) {
				k->fail = true;
				return;
			}
			d += w;
			n -= w;
		}
		return;
	}
	if (k->len + n > k->max) {
		if (k->status == 200)
			k->fail = true;
		n = k->max - k->len;
	}
	if (n <= 0)
		return;
	if (k->len + n + 1 > k->cap) {
		int cap = k->cap ? k->cap : 4096;
		while (cap < k->len + n + 1)
			cap *= 2;
		char *b = realloc(k->buf, cap);
		if (!b) {
			k->fail = true;
			return;
		}
		k->buf = b;
		k->cap = cap;
	}
	memcpy(k->buf + k->len, d, n);
	k->len += n;
	k->buf[k->len] = 0;
}

/* GET a path from relay r (connect_ms = how long the TCP connect may take). Returns the HTTP status
 * (body in *body, or written to fd for a 200), 0 when the relay can't be reached, -1 when the
 * transfer broke or timed out.
 * wait_s = how long the answer may take to start; idle_s = the longest pause between chunks of
 * the body after that (0: wait_s is the deadline for the whole transfer). */
static int http_get_at(int r, int connect_ms, const char *path, int wait_s, int idle_s, int fd, int max, char **body,
		       int *blen)
{
	*body = NULL;
	*blen = 0;
	int s = relay_connect(r, connect_ms);
	if (s < 0)
		return 0;
	char req[800];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s:%s\r\nConnection: close\r\n\r\n", path,
			 relays[r].host, relays[r].port);
	if (n >= (int)sizeof(req) || send(s, req, n, MSG_NOSIGNAL) != n) {
		close(s);
		return 0;
	}
	struct sink k = { .fd = fd, .status = -1, .max = max };
	struct pollfd p = { .fd = s, .events = POLLIN };
	char hdr[2048], tmp[16384];
	int hl = 0;
	long clen = -1, got = 0, deadline = now_ms() + wait_s * 1000L;
	bool in_body = false, eof = false;
	while (!k.fail) {
		long left = deadline - now_ms();
		if (left <= 0)
			break;
		int pr = poll(&p, 1, (int)left);
		if (pr < 0 && errno == EINTR)
			continue;
		if (pr <= 0)
			break;
		ssize_t m = recv(s, tmp, sizeof(tmp), 0);
		if (m < 0 && (errno == EAGAIN || errno == EINTR))
			continue;
		if (m <= 0) {
			eof = m == 0;
			break;
		}
		char *d = tmp;
		if (!in_body) {
			int c = m < (int)sizeof(hdr) - 1 - hl ? (int)m : (int)sizeof(hdr) - 1 - hl, prev = hl;
			memcpy(hdr + hl, tmp, c);
			hl += c;
			hdr[hl] = 0;
			char *end = strstr(hdr, "\r\n\r\n");
			if (!end) {
				if (hl >= (int)sizeof(hdr) - 1)
					break;
				continue;
			}
			int skip = (int)(end + 4 - hdr) - prev;	/* header bytes in this chunk */
			d += skip;
			m -= skip;
			in_body = true;
			k.status = 0;
			sscanf(hdr, "HTTP/%*d.%*d %d", &k.status);
			char *cl = strcasestr(hdr, "\r\nContent-Length:");
			if (cl && cl < end)
				clen = atol(cl + 17);
			if (k.status <= 0)
				break;
		}
		if (idle_s > 0 && in_body)
			deadline = now_ms() + idle_s * 1000L;
		if (m > 0) {
			sink_put(&k, d, (int)m);
			got += m;
		}
		if (clen >= 0 && got >= clen) {
			eof = true;
			break;
		}
	}
	close(s);
	if (!in_body || k.fail || !eof || (clen >= 0 && got < clen)) {
		free(k.buf);
		return -1;
	}
	*body = k.buf;
	*blen = k.len;
	return k.status;
}

/* GET from the relay in use (the USB link connects at once or not at all; the tailnet may need a
 * WireGuard handshake first) */
static int http_get(const char *path, int wait_s, int idle_s, int fd, int max, char **body, int *blen)
{
	int r = relay_now();
	return http_get_at(r, r ? 5000 : 3000, path, wait_s, idle_s, fd, max, body, blen);
}

/* the ping: the first relay (in order) that answers /ping becomes the one in use (j->px = which).
 * The PC's address doesn't answer at all once the cable is out, so it gets a short connect wait. */
static void run_ping(job_t *j)
{
	j->status = 0;
	for (int r = 0; r < nrelays; r++) {
		char *b;
		int bl, st = http_get_at(r, r ? 4000 : 1500, j->path, 5, 0, -1, 256, &b, &bl);
		bool ok = st == 200 && b && !strncmp(b, "ok", 2);
		free(b);
		if (ok) {
			pthread_mutex_lock(&mu);
			relay_cur = r;
			pthread_mutex_unlock(&mu);
			j->status = 200;
			j->px = r;
			return;
		}
		if (st > 0 && j->status <= 0)	/* answered, but not with ok (e.g. 403: not on the allow-list) */
			j->status = st;
	}
}

static void song_file(char *buf, int n, const char *id) { snprintf(buf, n, SONG_DIR "/%s.mp3", id); }

/* keep the phone's song cache (RAM) under SONG_CACHE_MAX: least recently played go first; the
 * current and next song (as they are now, not when this download was asked for) always stay */
static void trim_songs(void)
{
	char keep[2][24];
	pthread_mutex_lock(&mu);
	memcpy(keep, pins, sizeof(keep));
	pthread_mutex_unlock(&mu);
	DIR *d = opendir(SONG_DIR);
	if (!d)
		return;
	struct cf { char name[48]; long size; time_t mt; } *f = malloc(256 * sizeof(*f));
	int n = 0;
	long total = 0;
	struct dirent *e;
	while (f && (e = readdir(d)) && n < 256) {
		const char *nm = e->d_name;
		size_t l = strlen(nm);
		char p[400];
		struct stat st;
		if (nm[0] == '.' || l >= sizeof(f[0].name))
			continue;
		snprintf(p, sizeof(p), SONG_DIR "/%s", nm);
		if (l > 5 && !strcmp(nm + l - 5, ".part")) {	/* one download at a time: leftovers are stale */
			unlink(p);
			continue;
		}
		if (stat(p, &st))
			continue;
		total += st.st_size;
		bool pinned = false;
		for (int k = 0; k < 2; k++)
			pinned |= keep[k][0] && !strncmp(nm, keep[k], strlen(keep[k])) && !strcmp(nm + strlen(keep[k]), ".mp3");
		if (pinned)
			continue;
		snprintf(f[n].name, sizeof(f[n].name), "%s", nm);
		f[n].size = st.st_size;
		f[n].mt = st.st_mtime;
		n++;
	}
	closedir(d);
	while (f && total + (8L << 20) > SONG_CACHE_MAX) {
		int old = -1;
		for (int i = 0; i < n; i++)
			if (f[i].name[0] && (old < 0 || f[i].mt < f[old].mt))
				old = i;
		if (old < 0)
			break;
		char p[400];
		snprintf(p, sizeof(p), SONG_DIR "/%s", f[old].name);
		unlink(p);
		total -= f[old].size;
		f[old].name[0] = 0;
	}
	free(f);
}

static void run_song(job_t *j)
{
	char fin[160], part[170];
	song_file(fin, sizeof(fin), j->id);
	if (!access(fin, R_OK)) {
		j->status = 200;
		return;
	}
	mkdir(SONG_DIR, 0755);
	trim_songs();
	snprintf(part, sizeof(part), "%s.part", fin);
	int fd = open(part, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (fd < 0) {
		j->status = -1;
		return;
	}
	/* the relay answers once it has the whole MP3 (web app download + transcode: can be slow),
	 * then it streams: only a stall counts, not the song's length */
	j->status = http_get(j->path, 120, 20, fd, 1024, &j->body, &j->len);
	if (close(fd) && j->status == 200)
		j->status = -1;
	if (j->status != 200 || rename(part, fin)) {
		unlink(part);
		if (j->status == 200)
			j->status = -1;
	}
}

/* ---------- MP3s on the phone: ID3v2 / ID3v1 / "Artist - Title.mp3" ---------- */

static int put_cp(char *o, int n, int cap, unsigned cp)
{
	char b[4];
	int k;
	if (cp < 0x80) {
		b[0] = cp;
		k = 1;
	} else if (cp < 0x800) {
		b[0] = 0xc0 | cp >> 6;
		b[1] = 0x80 | (cp & 63);
		k = 2;
	} else if (cp < 0x10000) {
		b[0] = 0xe0 | cp >> 12;
		b[1] = 0x80 | (cp >> 6 & 63);
		b[2] = 0x80 | (cp & 63);
		k = 3;
	} else {
		b[0] = 0xf0 | cp >> 18;
		b[1] = 0x80 | (cp >> 12 & 63);
		b[2] = 0x80 | (cp >> 6 & 63);
		b[3] = 0x80 | (cp & 63);
		k = 4;
	}
	if (n + k >= cap)
		return -1;
	memcpy(o + n, b, k);
	return n + k;
}

static void trim_end(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = 0;
}

static void latin1(const unsigned char *d, int len, char *out, int cap)
{
	int n = 0;
	out[0] = 0;
	for (int i = 0; i < len && d[i]; i++) {
		int m = put_cp(out, n, cap, d[i]);
		if (m < 0)
			break;
		n = m;
	}
	out[n] = 0;
	trim_end(out);
}

/* ID3v2 text frame: encoding byte + text */
static void tag_text(const unsigned char *d, int len, char *out, int cap)
{
	if (len < 2)
		return;
	int enc = d[0], n = 0;
	d++;
	len--;
	if (enc == 0) {
		latin1(d, len, out, cap);
		return;
	}
	if (enc == 3) {
		int l = 0;
		while (l < len && d[l])
			l++;
		ucopy(out, cap, (const char *)d, l);
		trim_end(out);
		return;
	}
	bool be = enc == 2;
	if (enc == 1 && len >= 2 && ((d[0] == 0xff && d[1] == 0xfe) || (d[0] == 0xfe && d[1] == 0xff))) {
		be = d[0] == 0xfe;
		d += 2;
		len -= 2;
	}
	for (int i = 0; i + 1 < len; i += 2) {
		unsigned u = be ? (unsigned)d[i] << 8 | d[i + 1] : (unsigned)d[i + 1] << 8 | d[i];
		if (!u)
			break;
		if (u >= 0xd800 && u < 0xdc00 && i + 3 < len) {
			unsigned v = be ? (unsigned)d[i + 2] << 8 | d[i + 3] : (unsigned)d[i + 3] << 8 | d[i + 2];
			u = 0x10000 + ((u - 0xd800) << 10) + (v - 0xdc00);
			i += 2;
		}
		int m = put_cp(out, n, cap, u);
		if (m < 0)
			break;
		n = m;
	}
	out[n] = 0;
	trim_end(out);
}

static long syncsafe(const unsigned char *b) { return (b[0] & 0x7f) << 21 | (b[1] & 0x7f) << 14 | (b[2] & 0x7f) << 7 | (b[3] & 0x7f); }
static long be32(const unsigned char *b) { return (long)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }

/* length from the first MPEG frame: Xing/Info frame count, else constant bitrate */
static int mp3_secs(int fd, long audio, long size)
{
	static const short br1[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
	static const short br2[] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
	static const int sr[] = { 44100, 48000, 32000 };
	unsigned char b[4096];
	int n = pread(fd, b, sizeof(b), audio);
	for (int i = 0; i + 4 <= n; i++) {
		if (b[i] != 0xff || (b[i + 1] & 0xe0) != 0xe0)
			continue;
		int ver = b[i + 1] >> 3 & 3, layer = b[i + 1] >> 1 & 3, bri = b[i + 2] >> 4, sri = b[i + 2] >> 2 & 3;
		bool mono = b[i + 3] >> 6 == 3;
		if (ver == 1 || layer != 1 || bri == 0 || bri == 15 || sri == 3)
			continue;
		int rate = sr[sri] >> (ver == 3 ? 0 : ver == 2 ? 1 : 2), kbps = ver == 3 ? br1[bri] : br2[bri];
		int spf = ver == 3 ? 1152 : 576, xo = i + 4 + (ver == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17));
		if (xo + 12 <= n && (!memcmp(b + xo, "Xing", 4) || !memcmp(b + xo, "Info", 4)) && (b[xo + 7] & 1))
			return (int)(be32(b + xo + 8) * spf / rate);
		return (int)((size - audio - i) * 8 / (kbps * 1000L));
	}
	return 0;
}

static void read_tags(const char *path, long size, track_t *t)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	unsigned char h[10];
	long audio = 0;
	if (pread(fd, h, 10, 0) == 10 && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4) {
		int ver = h[3], hl = ver == 2 ? 6 : 10, idl = ver == 2 ? 3 : 4;
		long pos = 10, end = 10 + syncsafe(h + 6);
		audio = end + (h[5] & 0x10 ? 10 : 0);
		if ((h[5] & 0x40) && ver >= 3) {	/* extended header */
			unsigned char x[4];
			if (pread(fd, x, 4, pos) == 4)
				pos += ver == 4 ? syncsafe(x) : be32(x) + 4;
		}
		while (pos + hl <= end && (!t->title[0] || !t->artist[0])) {
			unsigned char f[10];
			if (pread(fd, f, hl, pos) != hl || !f[0])
				break;
			long fs = ver == 2 ? (long)f[3] << 16 | f[4] << 8 | f[5] : ver == 4 ? syncsafe(f + 4) : be32(f + 4);
			pos += hl;
			if (fs <= 0 || pos + fs > end)
				break;
			char *dst = NULL;
			int cap = 0;
			if (!memcmp(f, ver == 2 ? "TT2" : "TIT2", idl)) {
				dst = t->title;
				cap = sizeof(t->title);
			} else if (!memcmp(f, ver == 2 ? "TP1" : "TPE1", idl)) {
				dst = t->artist;
				cap = sizeof(t->artist);
			}
			if (dst) {
				unsigned char d[600];
				int m = fs < (long)sizeof(d) ? (int)fs : (int)sizeof(d);
				if (pread(fd, d, m, pos) == m)
					tag_text(d, m, dst, cap);
			}
			pos += fs;
		}
	}
	unsigned char v1[128];
	if (!t->title[0] && size > 128 && pread(fd, v1, 128, size - 128) == 128 && !memcmp(v1, "TAG", 3)) {
		latin1(v1 + 3, 30, t->title, sizeof(t->title));
		if (!t->artist[0])
			latin1(v1 + 33, 30, t->artist, sizeof(t->artist));
	}
	t->secs = mp3_secs(fd, audio, size);
	close(fd);
}

static void scan_dir(tlist_t *l, const char *dir, int depth)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d)) && l->n < 500) {
		if (e->d_name[0] == '.')
			continue;
		char p[600];
		struct stat st;
		snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
		if (stat(p, &st))
			continue;
		if (S_ISDIR(st.st_mode)) {
			if (depth < 2)
				scan_dir(l, p, depth + 1);
			continue;
		}
		size_t n = strlen(e->d_name);
		if (n < 5 || strcasecmp(e->d_name + n - 4, ".mp3"))
			continue;
		track_t t = { 0 };
		read_tags(p, st.st_size, &t);
		if (!t.title[0]) {	/* "Artist - Title.mp3" */
			char name[256];
			ucopy(name, sizeof(name), e->d_name, n - 4);
			char *sep = strstr(name, " - ");
			if (sep) {
				*sep = 0;
				if (!t.artist[0])
					ucopy(t.artist, sizeof(t.artist), name, -1);
				ucopy(t.title, sizeof(t.title), sep + 3, -1);
			} else {
				ucopy(t.title, sizeof(t.title), name, -1);
			}
		}
		if ((t.path = strdup(p)))
			tl_add(l, &t);
	}
	closedir(d);
}

static int by_title(const void *a, const void *b) { return strcasecmp(((const track_t *)a)->title, ((const track_t *)b)->title); }

static void scan_local_now(tlist_t *l)
{
	for (int i = 0; i < 3; i++)
		scan_dir(l, local_dirs[i], 0);
	if (l->n)
		qsort(l->v, l->n, sizeof(track_t), by_title);
}

/* ---------- job queue ---------- */

/* text first. Songs only on SONG_LANE (one download at a time, lists and covers never wait behind
 * it), which takes nothing else but pings: a stalled /home or cover can't hold up either. The UI
 * keeps at most one cover in flight (art_pump), so one of the other two lanes is free for text. */
static job_t *pick(int lane)
{
	job_t **hit = NULL;
	for (job_t **pp = &todo; *pp; pp = &(*pp)->next) {
		int k = (*pp)->kind;
		if (lane == SONG_LANE ? k != J_PING && k != J_SONG : k == J_SONG)
			continue;
		if (k != J_ART && k != J_SONG) {
			hit = pp;
			break;
		}
		if (!hit)
			hit = pp;
	}
	if (!hit)
		return NULL;
	job_t *j = *hit;
	*hit = j->next;
	j->next = NULL;
	return j;
}

static void run_job(job_t *j)
{
	switch (j->kind) {
	case J_SCAN:
		scan_local_now(&j->tl);
		break;
	case J_SONG:
		run_song(j);
		break;
	case J_ART:
		j->status = http_get(j->path, 20, 0, -1, j->px * j->px * 4, &j->body, &j->len);
		break;
	case J_PING:
		run_ping(j);
		break;
	default:
		j->status = http_get(j->path, 30, 0, -1, 4 << 20, &j->body, &j->len);
		break;
	}
}

static void *worker(void *arg)
{
	int lane = (int)(intptr_t)arg;
	for (;;) {
		pthread_mutex_lock(&mu);
		job_t *j;
		while (!(j = pick(lane)))
			pthread_cond_wait(&cv, &mu);
		pthread_mutex_unlock(&mu);
		run_job(j);
		pthread_mutex_lock(&mu);
		j->next = finished;
		finished = j;
		pthread_cond_broadcast(&cv);
		pthread_mutex_unlock(&mu);
	}
	return NULL;
}

static job_t *job_new(int kind, const char *fmt, ...)
{
	job_t *j = calloc(1, sizeof(*j));
	if (!j)
		return NULL;
	j->kind = kind;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(j->path, sizeof(j->path), fmt, ap);
	va_end(ap);
	return j;
}

static void submit(job_t *j)
{
	pthread_mutex_lock(&mu);
	job_t **pp = &todo;
	while (*pp)
		pp = &(*pp)->next;
	*pp = j;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

static void urlenc(char *out, int cap, const char *s)
{
	static const char hex[] = "0123456789ABCDEF";
	int n = 0;
	for (; *s && n < cap - 4; s++) {
		unsigned char c = *s;
		if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("-_.~", c)) {
			out[n++] = c;
		} else {
			out[n++] = '%';
			out[n++] = hex[c >> 4];
			out[n++] = hex[c & 15];
		}
	}
	out[n] = 0;
}

/* ---------- shared state (UI thread) ---------- */

enum { PS_IDLE, PS_LOADING, PS_PLAYING, PS_PAUSED };	/* IDLE = current song not loaded in the player */
static struct {
	int state;
	bool want;		/* LOADING: start playing once it's there */
	double pos, dur, heard;
	bool counted;		/* /played sent for this play */
	int fails;		/* songs skipped in a row because they failed */
	int resume;		/* seek here once loaded (the player was restarted) */
} pl;

static tlist_t q;	/* play queue */
static int qpos = -1, qseq;
static bool shuffle_on;
static int repeat_mode;	/* 0 off, 1 all, 2 one */
static int radio_gen, radio_pending = -1;
static char radio_for[24];
static bool radio_wait;

static int online = -1;	/* relay: -1 not asked yet, 0 unreachable, 1 answered */
static int relay_shown = -1;	/* the relay the last good ping found (UI thread; the Library footer names it) */
static bool ping_busy, home_busy, lists_busy, loc_busy, home_ok, lists_ok, lists_failed, loc_ok;
static uint32_t ping_at, home_at, lists_at, loc_at, home_hash;
static char home_err[160];
static struct shelf { char title[64]; tlist_t l; } shelves[MAX_SHELVES];
static int nshelves;
static struct plist { char id[96], title[96], count[16]; } lists[MAX_LISTS];
static int nlists;
static tlist_t loc;	/* MP3s on the phone */
static char **paths;
static int npaths, cpaths;

enum { S_IDLE, S_BUSY, S_DONE, S_ERR };
static tlist_t sres;
static char sq[160], s_err[160];
static int s_state, s_gen;

enum { PG_NONE, PG_LOADING, PG_READY, PG_ERROR };
static tlist_t pg;
static char pg_title[96], pg_err[160];
static int pg_state, pg_gen, pg_off;
static bool pg_local;

enum { D_HOME = 1, D_LIB = 2, D_SEARCH = 4, D_PAGE = 8 };
static int dirty;
static char pend[4][24];	/* song downloads asked for and not back yet */
#ifndef ORCHARD_PHONE
static int sim_wait;		/* previews: pretend the song takes a moment to fetch */
#endif

static void ui_track(void);
static void ui_sync(void);
static void ui_queue(void);
static void np_recolor(void);

static void banner(const char *title, const char *body)
{
	static char last[160];
	static uint32_t at;
	if (!strcmp(last, body) && lv_tick_elaps(at) < 8000)
		return;
	snprintf(last, sizeof(last), "%s", body);
	at = lv_tick_get();
	shell_banner(&app_music, title, body);
}

static void err_text(const job_t *j, char *out, int cap)
{
	if (j->status == 0)
		snprintf(out, cap, nrelays > 1 ? "The music server isn't answering." : "The PC isn't answering.");
	else if (j->status < 0)
		snprintf(out, cap, "The connection to the music server broke off.");
	else if (j->body && j->body[0]) {
		int n = strcspn(j->body, "\r\n");
		ucopy(out, cap, j->body, n);
	} else
		snprintf(out, cap, "The music server answered %d.", j->status);
}

static const char *intern(char *p)
{
	for (int i = 0; i < npaths; i++)
		if (!strcmp(paths[i], p)) {
			free(p);
			return paths[i];
		}
	if (npaths == cpaths) {
		int cap = cpaths ? cpaths * 2 : 64;
		char **v = realloc(paths, cap * sizeof(*v));
		if (!v)
			return p;	/* leaks one string; never happens in practice */
		paths = v;
		cpaths = cap;
	}
	return paths[npaths++] = p;
}

#ifdef ORCHARD_PHONE
static bool pend_has(const char *id)
{
	for (int i = 0; i < 4; i++)
		if (!strcmp(pend[i], id))
			return true;
	return false;
}
#endif

static void pend_set(const char *id, bool on)
{
	for (int i = 0; i < 4; i++) {
		if (on ? !pend[i][0] : !strcmp(pend[i], id)) {
			snprintf(pend[i], sizeof(pend[i]), "%s", on ? id : "");
			return;
		}
	}
}

/* drop queued (not yet running) song downloads other than keep1/keep2 */
static void cancel_songs(const char *keep1, const char *keep2)
{
	pthread_mutex_lock(&mu);
	for (job_t **pp = &todo; *pp;) {
		job_t *j = *pp;
		if (j->kind == J_SONG && strcmp(j->id, keep1) && strcmp(j->id, keep2)) {
			*pp = j->next;
			pend_set(j->id, false);
			free(j);
		} else {
			pp = &j->next;
		}
	}
	pthread_mutex_unlock(&mu);
}

static void ping(void)
{
	job_t *j = ping_busy ? NULL : job_new(J_PING, "/ping");
	if (!j)
		return;
	ping_busy = true;
	ping_at = lv_tick_get();
	submit(j);
}

static void fetch_home(void)
{
	job_t *j = home_busy ? NULL : job_new(J_HOME, "/home");
	if (!j)
		return;
	home_busy = true;
	submit(j);
}

static void fetch_lists(void)
{
	job_t *j = lists_busy ? NULL : job_new(J_LISTS, "/lists");
	if (!j)
		return;
	lists_busy = true;
	submit(j);
}

static void scan_local(void)
{
	job_t *j = loc_busy ? NULL : job_new(J_SCAN, "");
	if (!j)
		return;
	loc_busy = true;
	loc_at = lv_tick_get();
	submit(j);
}

/* ---------- covers: lazy, one fetch in flight, small LRU; images far off screen let go of theirs ---------- */

enum { A_FREE, A_WANT, A_LOADING, A_READY, A_FAILED };
typedef struct art {
	char id[24];
	int px, state, refs;	/* refs = images bound to it; only refs == 0 may be freed */
	uint32_t used, avg;
	uint8_t *buf;
	lv_image_dsc_t dsc;
} art_t;

typedef struct aimg {	/* an lv_image that shows a cover */
	lv_obj_t *obj;
	art_t *art;
	char id[24];
	int px;
} aimg_t;

#define ART_SLOTS 160
#define ART_BUDGET (24L << 20)
#define MAX_AIMG 512
static art_t arts[ART_SLOTS];
static aimg_t aimgs[MAX_AIMG];
static long art_bytes;
static int art_busy;
static bool art_dirty;
static lv_obj_t *root, *np_img, *mini_img;

static void art_free(art_t *a)
{
	if (a->buf) {
		lv_image_cache_drop(&a->dsc);
		free(a->buf);
		art_bytes -= (long)a->px * a->px * 4;
	}
	memset(a, 0, sizeof(*a));
}

static art_t *art_victim(void)
{
	art_t *v = NULL;
	for (int i = 0; i < ART_SLOTS; i++) {
		art_t *a = &arts[i];
		if (a->refs || a->state == A_LOADING || a->state == A_FREE)
			continue;
		if (!v || (int32_t)(a->used - v->used) < 0)
			v = a;
	}
	return v;
}

static art_t *art_get(const char *id, int px)
{
	art_t *fr = NULL;
	for (int i = 0; i < ART_SLOTS; i++) {
		art_t *a = &arts[i];
		if (a->state != A_FREE && a->px == px && !strcmp(a->id, id)) {
			a->used = lv_tick_get();
			return a;
		}
		if (!fr && a->state == A_FREE)
			fr = a;
	}
	if (!fr && (fr = art_victim()))
		art_free(fr);
	if (!fr)
		return NULL;
	snprintf(fr->id, sizeof(fr->id), "%s", id);
	fr->px = px;
	fr->state = A_WANT;
	fr->used = lv_tick_get();
	return fr;
}

static void img_placeholder(lv_obj_t *o)
{
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_image_set_src(o, GM_NOTE);	/* a symbol source: the glyph drawn with the image's text style */
}

static void img_show(aimg_t *ai)
{
	lv_image_set_src(ai->obj, &ai->art->dsc);
	lv_obj_set_style_bg_opa(ai->obj, LV_OPA_TRANSP, 0);
	if (ai->obj == np_img)
		np_recolor();
}

static void aimg_unbind(aimg_t *ai)
{
	art_t *a = ai->art;
	ai->art = NULL;
	if (a && --a->refs <= 0) {
		a->refs = 0;
		if (a->state == A_WANT)
			memset(a, 0, sizeof(*a));
	}
}

static void aimg_deleted(lv_event_t *e)
{
	aimg_t *ai = lv_event_get_user_data(e);
	aimg_unbind(ai);
	ai->obj = NULL;
}

static void on_scroll(lv_event_t *e)
{
	(void)e;
	art_dirty = true;
}

/* cover image: glyph tile until the art arrives (fetched only once it is near the screen) */
static lv_obj_t *art_img(lv_obj_t *parent, const char *id, int px, int radius)
{
	lv_obj_t *o = lv_image_create(parent);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(o, px, px);
	lv_obj_set_style_radius(o, radius, 0);
	lv_obj_set_style_text_font(o, font(F_MONO, px < 120 ? 34 : px < 360 ? 96 : 160), 0);
	if (px >= 400) {
		lv_obj_set_style_bg_color(o, C(mix(pal.tint, pal.bg, 25)), 0);
		lv_obj_set_style_bg_grad_color(o, C(mix(pal.tint, pal.bg, 70)), 0);
		lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
		lv_obj_set_style_text_color(o, C(mix(pal.label, pal.tint, 25)), 0);
	} else {
		lv_obj_set_style_bg_color(o, C(pal.fill), 0);
		lv_obj_set_style_text_color(o, C(pal.label3), 0);
	}
	img_placeholder(o);
	for (int i = 0; i < MAX_AIMG; i++) {
		aimg_t *ai = &aimgs[i];
		if (ai->obj)
			continue;
		memset(ai, 0, sizeof(*ai));
		ai->obj = o;
		ai->px = px;
		snprintf(ai->id, sizeof(ai->id), "%s", id ? id : "");
		lv_obj_set_user_data(o, ai);
		lv_obj_add_event_cb(o, aimg_deleted, LV_EVENT_DELETE, ai);
		art_dirty = true;
		break;
	}
	return o;
}

static void art_img_set(lv_obj_t *o, const char *id)
{
	aimg_t *ai = o ? lv_obj_get_user_data(o) : NULL;
	if (!id)
		id = "";
	if (!ai || !strcmp(ai->id, id))
		return;
	aimg_unbind(ai);
	snprintf(ai->id, sizeof(ai->id), "%s", id);
	img_placeholder(o);
	art_dirty = true;
}

static art_t *img_art(lv_obj_t *o)
{
	aimg_t *ai = o ? lv_obj_get_user_data(o) : NULL;
	return ai && ai->art && ai->art->state == A_READY ? ai->art : NULL;
}

static void art_pump(void)
{
	while (art_busy < 1) {	/* one at a time: covers never take more than one worker */
		art_t *a = NULL;
		const aimg_t *first[2] = { np_img ? lv_obj_get_user_data(np_img) : NULL,
					   mini_img ? lv_obj_get_user_data(mini_img) : NULL };
		for (int i = 0; i < 2 && !a; i++)
			if (first[i] && first[i]->art && first[i]->art->state == A_WANT)
				a = first[i]->art;
		for (int i = 0; i < MAX_AIMG && !a; i++)	/* registry order ≈ top to bottom */
			if (aimgs[i].obj && aimgs[i].art && aimgs[i].art->state == A_WANT)
				a = aimgs[i].art;
		if (!a)
			return;
		job_t *j = job_new(J_ART, "/art/%s/%d", a->id, a->px);
		if (!j)
			return;
		snprintf(j->id, sizeof(j->id), "%s", a->id);
		j->px = a->px;
		a->state = A_LOADING;
		art_busy++;
		submit(j);
	}
}

static bool near_screen(lv_obj_t *o)
{
	for (lv_obj_t *p = o; p; p = lv_obj_get_parent(p))
		if (lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN))
			return false;
	lv_area_t a;
	lv_obj_get_coords(o, &a);
	return a.x2 > -SCR_W / 2 && a.x1 < SCR_W * 3 / 2 && a.y2 > -SCR_H / 3 && a.y1 < SCR_H * 4 / 3;
}

/* a screen or more beyond the visible area (hidden or not): this image's cover can be let go — a
 * long list would otherwise hold every slot, and the covers further down would never load */
static bool far_off(lv_obj_t *o)
{
	lv_area_t a;
	lv_obj_get_coords(o, &a);
	return a.x2 < -SCR_W || a.x1 > 2 * SCR_W || a.y2 < -SCR_H || a.y1 > 2 * SCR_H;
}

static void art_pass(void)
{
	lv_obj_update_layout(root);
	for (int i = 0; i < MAX_AIMG; i++) {
		aimg_t *ai = &aimgs[i];
		if (ai->obj && ai->art && far_off(ai->obj)) {	/* stays cached (LRU) for when it comes back */
			bool shown = ai->art->state == A_READY;
			aimg_unbind(ai);
			if (shown)
				img_placeholder(ai->obj);
			continue;
		}
		if (!ai->obj || ai->art || !ai->id[0] || !near_screen(ai->obj))
			continue;
		art_t *a = art_get(ai->id, ai->px);
		if (!a)
			break;	/* every slot is on screen */
		ai->art = a;
		a->refs++;
		if (a->state == A_READY)
			img_show(ai);
	}
	art_pump();
}

static uint32_t avg_color(const uint8_t *b, int px)
{
	unsigned long r = 0, g = 0, bl = 0, n = 0;
	int step = px / 16 > 0 ? px / 16 : 1;
	for (int y = step / 2; y < px; y += step)
		for (int x = step / 2; x < px; x += step) {
			const uint8_t *p = b + ((long)y * px + x) * 4;	/* BGRX */
			bl += p[0];
			g += p[1];
			r += p[2];
			n++;
		}
	return n ? (uint32_t)(r / n << 16 | g / n << 8 | bl / n) : 0;
}

static void art_done(job_t *j)
{
	art_busy--;
	art_t *a = NULL;
	for (int i = 0; i < ART_SLOTS && !a; i++)
		if (arts[i].state == A_LOADING && arts[i].px == j->px && !strcmp(arts[i].id, j->id))
			a = &arts[i];
	long need = (long)j->px * j->px * 4;
	if (a && j->status == 200 && j->len == need) {
		a->buf = (uint8_t *)j->body;
		j->body = NULL;
		memset(&a->dsc, 0, sizeof(a->dsc));
		a->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
		a->dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
		a->dsc.header.w = a->dsc.header.h = j->px;
		a->dsc.header.stride = j->px * 4;
		a->dsc.data = a->buf;
		a->dsc.data_size = need;
		a->avg = avg_color(a->buf, j->px);
		a->state = A_READY;
		art_bytes += need;
		for (int i = 0; i < MAX_AIMG; i++)
			if (aimgs[i].obj && aimgs[i].art == a)
				img_show(&aimgs[i]);
		art_t *v;
		while (art_bytes > ART_BUDGET && (v = art_victim()))
			art_free(v);
	} else if (a) {
		a->state = A_FAILED;
	}
	art_pump();
}

/* ---------- player: mpg123 -R on the phone, a clock in the previews ---------- */

#ifdef ORCHARD_PHONE
static pid_t mp_pid;
static int mp_in = -1, mp_out = -1, mp_n, mp_ignore;
static char mp_buf[4096];
static bool mp_ack, mp_err, mp_active;	/* LOAD not confirmed yet / it failed (@E) / a song is loaded */
static bool mp_f_seen;			/* an @F arrived since the LOAD */
static uint32_t mp_heard;		/* tick of the last @F (or of the LOAD / unpause / seek that should bring one) */
static double mp_pos0;			/* where that LOAD / unpause / seek started: pl.pos is only a guess until an @F */
static pid_t mp_dead[8];		/* killed, not reaped yet */
static char mp_devbuf[16];		/* ORCHARD_MPG123_BUFFER: mpg123 --devbuffer (seconds), "" = its default */

/* reap killed players without waiting: one stuck in the kernel (audio driver) dies only once it
 * gets out, and the UI thread must not hang on it meanwhile */
static void mp_reap(void)
{
	for (int i = 0; i < 8; i++) {
		if (mp_dead[i] <= 0)
			continue;
		pid_t r = waitpid(mp_dead[i], NULL, WNOHANG);
		if (r == mp_dead[i] || (r < 0 && errno == ECHILD))
			mp_dead[i] = 0;
	}
}

static void mp_kill(void)
{
	if (mp_in >= 0)
		close(mp_in);
	if (mp_out >= 0)
		close(mp_out);
	if (mp_pid > 0) {
		kill(mp_pid, SIGKILL);
		if (waitpid(mp_pid, NULL, WNOHANG) != mp_pid) {	/* not gone yet: the engine reaps it */
			int i = 0;
			while (i < 7 && mp_dead[i] > 0)
				i++;
			mp_dead[i] = mp_pid;	/* all 8 slots stuck: the last one is given up on (a zombie) */
		}
	}
	mp_pid = 0;
	mp_in = mp_out = -1;
	mp_n = mp_ignore = 0;
	mp_ack = mp_err = mp_active = false;
}

static bool mp_spawn(void)
{
	if (mp_pid > 0)
		return true;
	if (access(MPG123, X_OK))
		return false;
	int sv[2], po[2];
	/* stdin is a socket so writes can use MSG_NOSIGNAL (a dead mpg123 must not SIGPIPE the shell) */
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv))
		return false;
	if (pipe2(po, O_CLOEXEC)) {
		close(sv[0]);
		close(sv[1]);
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {
		/* the sound must never wait behind Orchard: an animation renders flat out on one of the
		 * two cores, and mpg123 shares Orchard's autogroup. Realtime round-robin, through the raw
		 * syscall (musl's sched_setscheduler() is a stub that fails with ENOSYS, so mpg123's own
		 * -T can't work either); refused, nice -11 (with autogroup that weighs it against
		 * Orchard's threads — the ones it competes with). mpg123's threads (the bluealsa
		 * plugin's I/O thread) inherit it. stderr = /tmp/orchard.log. */
		struct sched_param sp = { .sched_priority = MP_RT_PRIO };
		if (syscall(SYS_sched_setscheduler, 0, SCHED_RR, &sp)) {
			static const char m1[] = "music: mpg123 gets no realtime priority, nice -11 instead\n",
					  m2[] = "music: mpg123 runs at normal priority (no realtime, no nice)\n";
			bool niced = setpriority(PRIO_PROCESS, 0, -11) == 0;
			ssize_t w = write(2, niced ? m1 : m2, (niced ? sizeof(m1) : sizeof(m2)) - 1);
			(void)w;
		}
		dup2(sv[1], 0);
		dup2(po[1], 1);
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		/* ALSA ring: mpg123's default 0.2 s unless ORCHARD_MPG123_BUFFER=seconds (a test knob:
		 * a bigger ring rides out a longer stall, but mpg123 -R drops what is still in it when a
		 * song ends — that much of every song's end goes unheard) */
		if (mp_devbuf[0])
			execl(MPG123, "mpg123", "-R", "--devbuffer", mp_devbuf, (char *)NULL);
		else
			execl(MPG123, "mpg123", "-R", (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
	close(po[1]);
	if (pid < 0) {
		close(sv[0]);
		close(po[0]);
		return false;
	}
	mp_pid = pid;
	mp_in = sv[0];
	mp_out = po[0];
	fcntl(mp_out, F_SETFL, O_NONBLOCK);
	fprintf(stderr, "music: mpg123 started, pid %d, ALSA buffer %ss\n", (int)pid, mp_devbuf[0] ? mp_devbuf : "0.2");
	return true;
}

static bool mp_send(const char *fmt, ...)
{
	char b[700];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (mp_in < 0 || n >= (int)sizeof(b))
		return false;
	if (send(mp_in, b, n, MSG_NOSIGNAL) != n) {
		mp_kill();
		return false;
	}
	return true;
}

/* mpg123 went away: the next play starts a new one, from where this one was */
static void mp_lost(void)
{
	bool was = pl.state == PS_PLAYING || pl.state == PS_PAUSED;
	mp_kill();
	if (was) {
		pl.resume = (int)pl.pos;
		pl.state = PS_IDLE;
		banner("Music", "The player stopped. Tap play to go on.");
		ui_sync();
	}
}
#endif

static bool be_ready(void)
{
#ifdef ORCHARD_PHONE
	return access(MPG123, X_OK) == 0;
#else
	return true;
#endif
}

static bool be_load(const char *path, bool play)
{
	pl.pos = 0;
	pl.state = play ? PS_PLAYING : PS_PAUSED;
#ifdef ORCHARD_PHONE
	for (int tries = 0; tries < 2; tries++) {	/* the player died since the last song: a fresh one */
		if (!mp_spawn())
			return false;
		mp_ack = mp_active = true;
		mp_err = mp_f_seen = false;
		mp_heard = lv_tick_get();
		mp_pos0 = 0;
		if (mp_send("%s %s\n", play ? "LOAD" : "LOADPAUSED", path))
			return true;
	}
	return false;
#else
	(void)path;
	return true;
#endif
}

static void start_current(bool play, int resume);

static void be_toggle(void)
{
	bool play = pl.state != PS_PLAYING;
	pl.state = play ? PS_PLAYING : PS_PAUSED;
#ifdef ORCHARD_PHONE
	mp_heard = lv_tick_get();
	mp_pos0 = pl.pos;
	if (!mp_send("PAUSE\n")) {	/* the player is gone: keep the spot (it used to start over) */
		pl.resume = (int)pl.pos;
		pl.state = PS_IDLE;
		if (play)
			start_current(true, pl.resume);
	}
#endif
}

static void be_stop(void)
{
#ifdef ORCHARD_PHONE
	if (mp_active) {
		mp_ignore++;	/* the @P 0 this STOP answers with is not "song ended" */
		mp_active = false;
		mp_send("STOP\n");
	}
#endif
}

static void be_seek(int s)
{
	pl.pos = s < 0 ? 0 : s;
#ifdef ORCHARD_PHONE
	mp_heard = lv_tick_get();
	mp_pos0 = pl.pos;
	if (!mp_send("JUMP %ds\n", s < 0 ? 0 : s))
		mp_lost();	/* it used to look like it was still playing, frozen */
#endif
}

/* ---------- queue ---------- */

static track_t *cur(void) { return qpos >= 0 && qpos < q.n ? &q.v[qpos] : NULL; }

static bool same_track(const track_t *a, const track_t *b)
{
	if (!a || !b)
		return false;
	return a->id[0] ? !strcmp(a->id, b->id) : a->path && a->path == b->path;
}

static bool cached(const track_t *t)
{
#ifdef ORCHARD_PHONE
	char f[160];
	if (t->path)
		return true;
	song_file(f, sizeof(f), t->id);
	return access(f, R_OK) == 0;
#else
	return t->path != NULL;
#endif
}

static int next_index(void)
{
	if (qpos + 1 < q.n)
		return qpos + 1;
	return repeat_mode == 1 && q.n ? 0 : -1;
}

/* tell the cache trim (a worker) which songs it must keep: call whenever the current or next song changes */
static void pins_update(void)
{
	const track_t *c = cur();
	int n = next_index();
	pthread_mutex_lock(&mu);
	snprintf(pins[0], sizeof(pins[0]), "%s", c ? c->id : "");
	snprintf(pins[1], sizeof(pins[1]), "%s", n >= 0 ? q.v[n].id : "");
	pthread_mutex_unlock(&mu);
}

#ifdef ORCHARD_PHONE
static void request_song(const track_t *t)
{
	if (!t || t->path || !t->id[0] || pend_has(t->id) || cached(t))
		return;
	job_t *j = job_new(J_SONG, "/song/%s.mp3", t->id);
	if (!j)
		return;
	snprintf(j->id, sizeof(j->id), "%s", t->id);
	pend_set(t->id, true);
	submit(j);
}
#endif

/* similar songs for the end of the queue (like the web app): returns true if a request went out */
static bool radio_request(void)
{
	const track_t *last = q.n ? &q.v[q.n - 1] : NULL;
	if (!last || !last->id[0] || !strcmp(radio_for, last->id) || online == 0)
		return false;
	job_t *j = job_new(J_RADIO, "/radio/%s", last->id);
	if (!j)
		return false;
	snprintf(radio_for, sizeof(radio_for), "%s", last->id);
	j->gen = radio_pending = radio_gen;
	submit(j);
	return true;
}

/* the song after this one gets fetched while this one plays */
static void prefetch(void)
{
	const track_t *c = cur();
	int n = next_index();
	const track_t *nx = n >= 0 ? &q.v[n] : NULL;
	pins_update();
	cancel_songs(c ? c->id : "", nx ? nx->id : "");
#ifdef ORCHARD_PHONE
	request_song(nx);
#endif
	if (c && qpos >= q.n - 2 && repeat_mode != 1)
		radio_request();
}

static void begin_playback(void)
{
	track_t *t = cur();
	if (!t)
		return;
	char path[600];
	if (t->path)
		snprintf(path, sizeof(path), "%s", t->path);
	else
		song_file(path, sizeof(path), t->id);
#ifdef ORCHARD_PHONE
	if (!t->path)
		utime(path, NULL);	/* most recently used: the cache trim keeps it longest */
	pl.dur = t->secs;
#else
	pl.dur = t->secs > 0 ? t->secs : 200;
#endif
	if (!be_load(path, pl.want)) {
		pl.state = PS_IDLE;
		banner("Music", be_ready() ? "The player didn't start. Tap play to try again." :
					     "Can't play: mpg123 is missing on the phone.");
		ui_sync();
		return;
	}
	int resume = pl.resume;
	pl.resume = 0;	/* (before the seek: a player lost on it keeps its own spot) */
	if (resume > 0)
		be_seek(resume);
	prefetch();
	ui_sync();
}

static void start_current(bool play, int resume)
{
	track_t *t = cur();
	if (!t)
		return;
	pins_update();	/* before a cached song is handed to mpg123: the trim must not delete it */
	if (resume <= 0) {	/* a new play; going on from a saved spot (player restarted) is the same one */
		pl.heard = 0;
		pl.counted = false;
	}
	pl.pos = 0;
	pl.dur = t->secs;
	pl.want = play;
	pl.resume = resume;
	radio_wait = false;
	if (cached(t)) {
		begin_playback();
	} else {
		be_stop();
		pl.state = PS_LOADING;
#ifdef ORCHARD_PHONE
		cancel_songs(t->id, "");
		request_song(t);
#else
		sim_wait = 700;
#endif
	}
	ui_track();
}

static void go_next(bool play)
{
	int n = next_index();
	if (n >= 0) {
		qpos = n;
		start_current(play, 0);
		return;
	}
	/* out of songs: similar ones follow */
	if (radio_pending == radio_gen || radio_request()) {
		be_stop();
		radio_wait = true;
		pl.state = PS_LOADING;
		pl.want = play;
	} else {
		be_stop();
		pl.state = PS_IDLE;
		pl.pos = 0;
	}
	ui_sync();
}

/* the song finished; play = it was playing (a pause that raced the end loads the next one paused) */
static void track_ended(bool play)
{
	pl.state = PS_IDLE;
	if (repeat_mode == 2)
		start_current(play, 0);
	else
		go_next(play);
}

static void play_failed(const char *why)
{
	const track_t *t = cur();
	bool play = music_playing();	/* paused or loading paused: the next one waits paused too */
	banner(t ? t->title : "Music", why && *why ? why : "This song can't be played.");
	pl.state = PS_IDLE;
	if (++pl.fails >= 3 || next_index() < 0) {
		pl.fails = 0;
		ui_sync();
		return;
	}
	qpos = next_index();
	start_current(play, 0);
}

static void shuffle_rest(void)
{
	for (int i = q.n - 1; i > qpos + 1; i--) {
		int k = qpos + 1 + rand() % (i - qpos);
		track_t t = q.v[i];
		q.v[i] = q.v[k];
		q.v[k] = t;
	}
}

static int by_ord(const void *a, const void *b) { return ((const track_t *)a)->ord - ((const track_t *)b)->ord; }

static void set_shuffle(bool on)
{
	shuffle_on = on;
	if (cur() && qpos + 1 < q.n) {
		if (on)
			shuffle_rest();
		else
			qsort(q.v + qpos + 1, q.n - qpos - 1, sizeof(track_t), by_ord);
		prefetch();
	}
	ui_queue();
}

/* tap on a song: it plays, the rest of its list follows */
static void play_list(const tlist_t *l, int idx)
{
	if (!l || idx < 0 || idx >= l->n)
		return;
	if (!be_ready()) {
		banner("Music", "Can't play: mpg123 is missing on the phone.");
		return;
	}
	tlist_t nq = { 0 };
	for (int i = 0; i < l->n; i++)
		tl_add(&nq, &l->v[i]);
	tl_free(&q);
	q = nq;
	for (qseq = 0; qseq < q.n; qseq++)
		q.v[qseq].ord = qseq;
	qpos = idx < q.n ? idx : 0;
	if (shuffle_on && q.n > 1) {
		track_t t = q.v[0];
		q.v[0] = q.v[qpos];
		q.v[qpos] = t;
		qpos = 0;
		shuffle_rest();
	}
	radio_gen++;
	radio_for[0] = 0;
	pl.fails = 0;
	start_current(true, 0);
}

static void music_seek(int s)
{
	if (pl.state == PS_PLAYING || pl.state == PS_PAUSED)
		be_seek(s);
}

/* ---------- results from the workers (UI thread) ---------- */

static void set_online(int v)
{
	if (v == online)
		return;
	bool was = online == 1;
	online = v;
	dirty |= D_HOME | D_LIB | D_SEARCH;
	if (v == 1 && !was) {
		home_err[0] = 0;	/* a fresh try: Loading… while it runs */
		lists_failed = false;
		if (!home_ok)
			fetch_home();
		if (!lists_ok)
			fetch_lists();
		for (int i = 0; i < ART_SLOTS; i++)	/* covers that failed while offline */
			if (arts[i].state == A_FAILED) {
				if (arts[i].refs)
					arts[i].state = A_WANT;
				else
					memset(&arts[i], 0, sizeof(arts[i]));
			}
		art_dirty = true;
	}
}

static uint32_t hash(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

static void home_done(job_t *j)
{
	home_busy = false;
	if (j->status == 200 && j->body) {
		uint32_t h = hash(j->body);
		if (!home_ok || h != home_hash) {
			for (int i = 0; i < nshelves; i++)
				tl_free(&shelves[i].l);
			nshelves = 0;
			char *s = j->body, *ln;
			bool skip = false;
			track_t t;
			while ((ln = next_line(&s))) {
				if (ln[0] == '#' && ln[1] == '\t') {
					skip = nshelves >= MAX_SHELVES;
					if (!skip)
						ucopy(shelves[nshelves++].title, sizeof(shelves[0].title), ln + 2, -1);
				} else if (nshelves && !skip && parse_track(ln, &t)) {
					tl_add(&shelves[nshelves - 1].l, &t);
				}
			}
			home_hash = h;
			dirty |= D_HOME;
		}
		home_ok = true;
		home_at = lv_tick_get();
		home_err[0] = 0;
	} else if (!home_ok) {	/* any failure, a timeout too, ends Loading… (retried after the next good ping) */
		err_text(j, home_err, sizeof(home_err));
		dirty |= D_HOME;
	}
}

static void lists_done(job_t *j)
{
	lists_busy = false;
	if (j->status != 200 || !j->body) {
		lists_failed = true;
		dirty |= D_LIB;
		return;
	}
	nlists = 0;
	char *s = j->body, *ln, *f[3];
	while ((ln = next_line(&s)) && nlists < MAX_LISTS) {
		int n = split_tabs(ln, f, 3);
		if (n < 2 || !*f[0])
			continue;
		struct plist *p = &lists[nlists++];
		ucopy(p->id, sizeof(p->id), f[0], -1);
		ucopy(p->title, sizeof(p->title), f[1], -1);
		ucopy(p->count, sizeof(p->count), n > 2 ? f[2] : "", -1);
	}
	lists_ok = true;
	lists_at = lv_tick_get();
	dirty |= D_LIB;
}

static void list_done(job_t *j)
{
	if (j->gen != pg_gen || pg_state != PG_LOADING)
		return;
	if (j->status == 200 && j->body) {
		tl_free(&pg);
		parse_tracks(j->body, &pg);
		pg_state = PG_READY;
	} else {
		err_text(j, pg_err, sizeof(pg_err));
		pg_state = PG_ERROR;
		if (j->status > 0)
			banner("Music", pg_err);
	}
	dirty |= D_PAGE;
}

static void search_done(job_t *j)
{
	if (j->gen != s_gen)
		return;
	if (j->status == 200 && j->body) {
		tl_free(&sres);
		parse_tracks(j->body, &sres);
		s_state = S_DONE;
	} else {
		err_text(j, s_err, sizeof(s_err));
		s_state = S_ERR;
		if (j->status > 0)
			banner("Search", s_err);
	}
	dirty |= D_SEARCH;
}

static void radio_done(job_t *j)
{
	if (j->gen == radio_pending)
		radio_pending = -1;
	if (j->gen != radio_gen)
		return;	/* a new queue was started since */
	int added = 0;
	if (j->status == 200 && j->body) {
		tlist_t l = { 0 };
		parse_tracks(j->body, &l);
		for (int i = 0; i < l.n; i++) {
			bool have = false;
			for (int k = 0; k < q.n && !have; k++)
				have = !strcmp(q.v[k].id, l.v[i].id);
			if (have)
				continue;
			l.v[i].ord = qseq++;
			tl_add(&q, &l.v[i]);
			added++;
		}
		tl_free(&l);
	} else {
		radio_for[0] = 0;	/* may ask again later */
		if (j->status > 0 && radio_wait) {
			char e[160];
			err_text(j, e, sizeof(e));
			banner("Music", e);
		}
	}
	if (radio_wait) {
		radio_wait = false;
		if (added && qpos + 1 < q.n) {
			qpos++;
			start_current(pl.want, 0);
		} else {
			pl.state = PS_IDLE;
			ui_sync();
		}
	} else if (added) {
		prefetch();
	}
	ui_queue();
}

static void song_done(job_t *j)
{
	pend_set(j->id, false);
	track_t *t = cur();
	bool mine = t && pl.state == PS_LOADING && !radio_wait && !strcmp(t->id, j->id);
	if (j->status == 200) {
		if (mine)
			begin_playback();
		return;
	}
	if (!mine)
		return;	/* a fetch-ahead failed: it is asked for again when its turn comes */
	if (j->status == 0) {
		pl.state = PS_IDLE;
		if (nrelays > 1)
			banner("No Connection", "Music streams from your music relay over Wi-Fi or through the PC.");
		else
			banner("Connect the PC", "Music comes from your music relay through the PC.");
		ui_sync();
		return;
	}
	char e[160];
	err_text(j, e, sizeof(e));
	play_failed(e);
}

static void scan_done(job_t *j)
{
	loc_busy = false;
	for (int i = 0; i < j->tl.n; i++)
		j->tl.v[i].path = intern((char *)j->tl.v[i].path);
	tl_free(&loc);
	loc = j->tl;
	j->tl = (tlist_t){ 0 };
	loc_ok = true;
	dirty |= D_LIB | (pg_local ? D_PAGE : 0);
}

static void handle(job_t *j)
{
	if (j->kind == J_PING) {
		ping_busy = false;
		if (j->status == 200 && j->px != relay_shown) {
			relay_shown = j->px;
			dirty |= D_LIB;
		}
		set_online(j->status == 200);
	} else if (j->kind != J_SCAN) {
		if (j->status == 0)	/* the relay in use is gone: find one again now (the ping decides offline) */
			ping();
		else if (j->status > 0 && j->kind != J_SONG)
			set_online(1);
	}
	switch (j->kind) {
	case J_PING:
		if (j->status == 200) {	/* a relay answers: try again what failed (kept on screen meanwhile) */
			if (!home_ok)
				fetch_home();
			if (!lists_ok)
				fetch_lists();
		}
		break;
	case J_HOME: home_done(j); break;
	case J_LISTS: lists_done(j); break;
	case J_LIST: list_done(j); break;
	case J_SEARCH: search_done(j); break;
	case J_RADIO: radio_done(j); break;
	case J_ART: art_done(j); break;
	case J_SONG: song_done(j); break;
	case J_SCAN: scan_done(j); break;
	default: break;
	}
}

/* ---------- mpg123 output ---------- */

#ifdef ORCHARD_PHONE
static void mp_line(const char *l)
{
	if (!strncmp(l, "@F ", 3)) {	/* @F frame frames-left seconds seconds-left */
		int f1, f2;
		double c, r;
		if (sscanf(l + 3, "%d %d %lf %lf", &f1, &f2, &c, &r) == 4) {
			pl.pos = c;
			pl.dur = c + r;
			mp_heard = lv_tick_get();
			mp_f_seen = true;
		}
	} else if (!strncmp(l, "@P ", 3)) {
		int st = atoi(l + 3);
		if (st == 3)	/* end of track: the @P 0 that follows is what counts */
			return;
		if (st != 0) {	/* 1 paused / 2 playing: the LOAD (or PAUSE) was taken — an @E before it wasn't fatal */
			mp_ack = mp_err = false;
			return;
		}
		if (mp_ignore > 0) {	/* answer to our STOP */
			mp_ignore--;
			return;
		}
		if (mp_ack && !mp_err)	/* the previous song's end, read after a new LOAD */
			return;
		mp_active = mp_ack = false;
		if (pl.state != PS_PLAYING && pl.state != PS_PAUSED)
			return;
		if (mp_err) {
			mp_err = false;
			play_failed("mpg123 can't play this file.");
		} else {
			track_ended(pl.state == PS_PLAYING);	/* PAUSED: paused just as it ended */
		}
	} else if (!strncmp(l, "@E ", 3)) {	/* fatal only while a LOAD is pending (open failed) */
		if (mp_ack)
			mp_err = true;
		fprintf(stderr, "music: mpg123: %s\n", l + 3);
	}
}

static void mp_poll(void)
{
	while (mp_out >= 0) {
		ssize_t k = read(mp_out, mp_buf + mp_n, sizeof(mp_buf) - 1 - mp_n);
		if (k > 0) {
			mp_n += k;
			mp_buf[mp_n] = 0;
			char *s = mp_buf, *e;
			while ((e = strchr(s, '\n'))) {
				*e = 0;
				mp_line(s);
				if (mp_out < 0)
					return;
				s = e + 1;
			}
			mp_n -= s - mp_buf;
			memmove(mp_buf, s, mp_n);
			if (mp_n >= (int)sizeof(mp_buf) - 1)
				mp_n = 0;
			continue;
		}
		if (k < 0 && (errno == EAGAIN || errno == EINTR))
			break;
		mp_lost();
	}
	/* the audio output is stuck: progress stopped mid-song, or not one frame since the play began
	 * (an output that blocks as it opens) — then pl.pos is only the estimate, go on from where it began */
	if (pl.state == PS_PLAYING && mp_active && lv_tick_elaps(mp_heard) > (mp_f_seen ? 10000u : 8000u)) {
		pl.pos = mp_f_seen ? pl.pos : mp_pos0;
		pl.resume = (int)pl.pos;
		pl.state = PS_IDLE;
		mp_kill();
		banner("Music", "The speaker isn't responding. Tap play to try again.");
		ui_sync();
	}
}
#endif

/* ---------- engine: every 100 ms, from music_init on, app open or not ---------- */

static void build_home(void);
static void build_library(void);
static void fill_search(void);
static void run_search(void);
static void build_page(void);
static lv_obj_t *col, *s_res, *pg_col;
static int tab;

static lv_timer_t *eng_t;

static void engine(lv_timer_t *tm)
{
	(void)tm;
	static uint32_t last;
	uint32_t now = lv_tick_get(), dt = last ? now - last : 0;
	last = now;

	pthread_mutex_lock(&mu);
	job_t *l = finished, *rev = NULL;
	finished = NULL;
	pthread_mutex_unlock(&mu);
	while (l) {
		job_t *n = l->next;
		l->next = rev;
		rev = l;
		l = n;
	}
	while (rev) {
		job_t *n = rev->next;
		handle(rev);
		free(rev->body);
		tl_free(&rev->tl);
		free(rev);
		rev = n;
	}

#ifdef ORCHARD_PHONE
	mp_poll();
	mp_reap();
	if (pl.state == PS_PLAYING && !mp_f_seen && (pl.dur <= 0 || pl.pos + dt / 1000.0 < pl.dur))
		pl.pos += dt / 1000.0;	/* no @F yet: estimate */
	bool heard = mp_f_seen && lv_tick_elaps(mp_heard) < 2000;	/* frames are really coming */
#else
	if (pl.state == PS_LOADING && !radio_wait && sim_wait > 0 && (sim_wait -= dt) <= 0)
		begin_playback();
	if (pl.state == PS_PLAYING) {
		pl.pos += dt / 1000.0;
		if (pl.pos >= pl.dur) {
			pl.pos = pl.dur;
			track_ended(true);
		}
	}
	bool heard = true;
#endif
	if (pl.state == PS_PLAYING && heard) {
		pl.heard += dt / 1000.0;
		if (pl.heard > 5)
			pl.fails = 0;
#ifdef ORCHARD_PHONE	/* previews never count plays in the stats */
		const track_t *t = cur();
		if (!pl.counted && pl.heard >= 20 && t && t->id[0]) {
			job_t *j = job_new(J_PLAYED, "/played/%s?len=%d", t->id, t->secs > 0 ? t->secs : (int)pl.dur);
			pl.counted = true;
			home_at = now - 600001;	/* "Recently played" changed: refetch on the next open */
			if (j)
				submit(j);
		}
#endif
	}
	if (!ping_busy && !power_dark() && lv_tick_elaps(ping_at) > (online == 1 ? 60000u : 15000u))
		ping();	/* (screen off: no relay pings — the radio may doze; the first tick after wake asks) */
	bool busy = pl.state == PS_PLAYING || pl.state == PS_LOADING;
	if (power_dark())	/* playing: mpg123's @F lines + the next song; else only finished jobs */
		lv_timer_set_period(tm, busy ? 500 : 5000);
	else	/* the app on screen: 10 a second; closed or under the lock screen: 4 (playing) or 2 */
		lv_timer_set_period(tm, root && !shell_locked() ? 100 : busy ? 250 : 500);

	if (root) {
		if ((dirty & D_HOME) && tab == 0 && col)
			build_home();
		if ((dirty & D_LIB) && tab == 1 && col)
			build_library();
		if ((dirty & D_SEARCH) && tab == 2 && s_res)
			fill_search();
		if ((dirty & D_PAGE) && pg_col)
			build_page();
		if (art_dirty) {
			art_dirty = false;
			art_pass();
		}
	}
	dirty = 0;
}

/* ---------- hooks for the shell (lock screen, Control Center) ---------- */

void music_init(void)
{
	const char *e1 = getenv("ORCHARD_MUSICD"), *e2 = getenv("ORCHARD_MUSICD2");
	const char *hps[MAX_RELAYS] = { e1 && *e1 ? e1 : prefs_str("music_relay", RELAY),
					e2 && *e2 ? e2 : prefs_str("music_relay2", RELAY2) };
	for (int i = 0; i < MAX_RELAYS; i++) {
		const char *hp = hps[i], *c = strrchr(hp, ':');
		if (!*hp || !strcmp(hp, "off"))
			continue;
		int r = nrelays++;
		relays[r].wifi = i == 1;
		if (c && c > hp && !strchr(c, ']')) {
			snprintf(relays[r].host, sizeof(relays[r].host), "%.*s", (int)(c - hp), hp);
			snprintf(relays[r].port, sizeof(relays[r].port), "%s", c + 1);
		} else {
			snprintf(relays[r].host, sizeof(relays[r].host), "%s", hp);
			snprintf(relays[r].port, sizeof(relays[r].port), "8650");
		}
	}
	snprintf(local_dirs[0], sizeof(local_dirs[0]), "%s/Music", data_dir());
	snprintf(local_dirs[1], sizeof(local_dirs[1]), "/root/Music");
	snprintf(local_dirs[2], sizeof(local_dirs[2]), "/tmp/music");
#ifdef ORCHARD_PHONE
	const char *db = getenv("ORCHARD_MPG123_BUFFER");
	if (db && atof(db) > 0 && strspn(db, "0123456789.") == strlen(db))
		snprintf(mp_devbuf, sizeof(mp_devbuf), "%s", db);
#endif
	srand(time(NULL) ^ getpid());
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&at, 256 << 10);
	for (int i = 0; i < LANES; i++) {
		pthread_t tid;
		pthread_create(&tid, &at, worker, (void *)(intptr_t)i);
	}
	pthread_attr_destroy(&at);
	eng_t = lv_timer_create(engine, 100, NULL);
	power_keep(eng_t, 500, "music");	/* playback goes on with the screen off */
	scan_local();
	ping();
}

bool music_available(void) { return be_ready() && (online == 1 || loc.n > 0); }
bool music_playing(void) { return pl.state == PS_PLAYING || (pl.state == PS_LOADING && pl.want); }
const char *music_title(void) { return cur() ? cur()->title : NULL; }
const char *music_artist(void) { return cur() ? cur()->artist : NULL; }

void music_toggle(void)
{
	if (!cur()) {	/* nothing chosen yet (Control Center / lock screen): start something */
		if (online == 1 && nshelves && shelves[0].l.n)
			play_list(&shelves[0].l, 0);
		else if (loc.n)
			play_list(&loc, 0);
		return;
	}
	switch (pl.state) {
	case PS_LOADING:
		pl.want = !pl.want;
		break;
	case PS_PLAYING:
	case PS_PAUSED:
		be_toggle();
		break;
	default:
		start_current(true, pl.resume);
		break;
	}
	ui_sync();
}

static bool keep_playing(void) { return pl.state != PS_PAUSED && !(pl.state == PS_LOADING && !pl.want); }

void music_next(void)
{
	if (cur())
		go_next(keep_playing());
}

void music_prev(void)
{
	if (!cur())
		return;
	bool loaded = pl.state == PS_PLAYING || pl.state == PS_PAUSED;
	if (pl.pos > 3 || qpos == 0) {
		if (loaded)
			be_seek(0);
		else
			start_current(keep_playing(), 0);
		return;
	}
	qpos--;
	start_current(keep_playing(), 0);
}

/* bt.c: the ALSA default output moved (Bluetooth audio came or went). mpg123 keeps the old device
 * open, so restart it at the same spot; losing the headphones pauses (keep_playing false), like iOS. */
void music_output_changed(bool keep_playing)
{
#ifdef ORCHARD_PHONE
	bool loaded = pl.state == PS_PLAYING || pl.state == PS_PAUSED;
	bool play = pl.state == PS_PLAYING && keep_playing, counted = pl.counted;
	double heard = pl.heard;
	mp_kill();	/* idle or still loading: the next LOAD starts a fresh mpg123 on the new output */
	if (!loaded)
		return;
	start_current(play, (int)pl.pos);
	pl.heard = heard;
	pl.counted = counted;
	ui_sync();
#else
	(void)keep_playing;
#endif
}

/* =================================== UI =================================== */

static lv_obj_t *host, *tabbar, *pg_rows, *s_ta;
static lv_obj_t *mini, *mini_title, *mini_sub, *mini_btn, *mini_play, *mini_spin;
static lv_obj_t *np, *np_big, *np_qv, *np_title, *np_artist, *np_slider, *np_el, *np_rem;
static lv_obj_t *np_btn, *np_play, *np_spin, *np_shuf, *np_rep, *np_qbtn, *np_vol;
static lv_timer_t *ui_timer, *s_timer;
static int shown_state = -1, np_shown_p = -1, np_shown_d = -1;
static bool np_dragging, np_closing, np_queue_open;
static uint32_t vol_at;

enum { V_PAGE = 16, V_SEARCH, V_QUEUE };	/* views below V_PAGE = Home shelves */
#define CODE(v, i) ((v) << 16 | (i))

static const tlist_t *view_list(int v)
{
	if (v < V_PAGE)
		return v < nshelves ? &shelves[v].l : NULL;
	if (v == V_PAGE)
		return pg_local ? &loc : &pg;
	if (v == V_SEARCH)
		return &sres;
	return v == V_QUEUE ? &q : NULL;
}

static int bottom_pad(void) { return SCR_H - (mini ? MINI_Y : TABBAR_TOP) + 24; }

static void content_setup(lv_obj_t *c)
{
	lv_obj_set_style_pad_bottom(c, bottom_pad(), 0);
	lv_obj_add_event_cb(c, on_scroll, LV_EVENT_SCROLL, NULL);
}

static lv_obj_t *lbl(lv_obj_t *p, const char *s, lv_font_t *f, uint32_t c, int x, int y, int w)
{
	lv_obj_t *l = text(p, s, f, c);
	lv_obj_set_pos(l, x, y);
	if (w > 0) {
		lv_obj_set_width(l, w);
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
		lv_label_set_max_lines(l, 1);
	}
	return l;
}

static lv_obj_t *spinner(lv_obj_t *parent, int size)
{
	lv_obj_t *s = ui_spinner(parent, 0, 0);
	lv_obj_set_size(s, size, size);
	lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_arc_width(s, size / 10 + 2, 0);
	lv_obj_set_style_arc_width(s, size / 10 + 2, LV_PART_INDICATOR);
	lv_obj_set_style_arc_color(s, C(pal.fill), 0);
	lv_obj_set_style_arc_color(s, C(pal.label2), LV_PART_INDICATOR);
	return s;
}

static void col_clear(lv_obj_t *c)
{
	while (lv_obj_get_child_count(c) > 1)	/* child 0 = the large title */
		lv_obj_delete(lv_obj_get_child(c, 1));
}

static void retry_async(void *ud)
{
	(void)ud;
	online = -1;
	home_err[0] = 0;
	fetch_home();
	fetch_lists();
	ping();
	lists_failed = false;
	if (s_state == S_ERR) {	/* the same search again */
		s_state = S_IDLE;
		run_search();
	}
	dirty |= D_HOME | D_LIB | D_SEARCH;
}

static void retry_clicked(lv_event_t *e)
{
	(void)e;
	lv_async_call(retry_async, NULL);	/* the rebuild deletes this button */
}

static lv_obj_t *empty_state(lv_obj_t *parent, const char *glyph, const char *title, const char *body, bool retry, int top)
{
	lv_obj_t *w = box(parent);
	lv_obj_set_size(w, SCR_W - 2 * 56, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(w, top, 0);
	lv_obj_set_flex_flow(w, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(w, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(w, 14, 0);
	lv_obj_t *g = ui_icon_line(w, glyph, font(F_MONO, 96), pal.label3);	/* ink centred in the line box */
	lv_obj_set_height(g, lv_font_get_line_height(font(F_MONO, 96)) + 10);	/* + the gap under it */
	lv_obj_t *t = text(w, title, font(F_TEXT_BOLD, 40), pal.label);
	lv_obj_set_width(t, LV_PCT(100));
	lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
	if (body) {
		lv_obj_t *b = text(w, body, font(F_TEXT, 30), pal.label2);
		lv_obj_set_width(b, LV_PCT(100));
		lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_line_space(b, 6, 0);
	}
	if (retry) {
		lv_obj_t *btn = ui_button(w, "Retry", true);
		lv_obj_set_style_margin_top(btn, 24, 0);
		lv_obj_add_event_cb(btn, retry_clicked, LV_EVENT_CLICKED, NULL);
	}
	return w;
}

static void offline_state(lv_obj_t *parent)
{
	if (!nrelays) {
		empty_state(parent, GM_PHONE, "No Music Yet",
			    "Copy MP3 files into the Music folder on the phone's data area (or /root/Music) and "
			    "they show up in Library.",
			    false, 110);
		return;
	}
	if (nrelays > 1) {
		empty_state(parent, G_WIFI, "No Connection",
			    "Music streams from your music relay over Wi-Fi, or through the PC while the phone is "
			    "plugged in. Songs on iPhone still play \xe2\x80\x94 find them in Library.",
			    true, 110);
		return;
	}
	empty_state(parent, GM_CABLE, "Connect the PC",
		    "Music comes from your music relay through the PC. Songs on iPhone still play \xe2\x80\x94 "
		    "find them in Library.",
		    true, 110);
}

static void loading_block(lv_obj_t *parent, int top)
{
	lv_obj_t *w = box(parent);
	lv_obj_set_size(w, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(w, top, 0);
	lv_obj_set_flex_flow(w, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(w, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(w, 22, 0);
	spinner(w, 64);
	text(w, "Loading\xe2\x80\xa6", font(F_TEXT, 30), pal.label2);
}

/* ---------- song rows ---------- */

static void row_play_async(void *ud)
{
	int code = (intptr_t)ud, v = code >> 16, i = code & 0xffff;
	if (v == V_QUEUE) {
		if (i < q.n) {
			qpos = i;
			start_current(true, 0);
		}
		return;
	}
	play_list(view_list(v), i);
}

static void row_clicked(lv_event_t *e)
{
	ui_haptic_tap();
	lv_async_call(row_play_async, lv_event_get_user_data(e));	/* may rebuild the list it sits in */
}

static void sub_text(char *b, int n, const track_t *t)
{
	char d[16] = "";
	const char *a = t->artist[0] ? t->artist : "Unknown Artist";
	if (t->secs > 0)
		fmt_time(d, sizeof(d), t->secs);
	if (d[0])
		snprintf(b, n, "%s \xc2\xb7 %s", a, d);
	else
		snprintf(b, n, "%s", a);
}

/* thumbnail, title, "artist · 3:51", hairline (as the subtitle's bottom border: one object less) */
static lv_obj_t *song_row(lv_obj_t *parent, const track_t *t, int code, int w, int x0, int h, int th_px, bool sep)
{
	lv_obj_t *r = box(parent);
	lv_obj_set_size(r, w, h);
	lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_USER_1);	/* USER_1 = a song row (rows_mark) */
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(r, C(pal.cell_hi), LV_STATE_PRESSED);
	lv_obj_set_user_data(r, (void *)(intptr_t)(code + 1));
	lv_obj_add_event_cb(r, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)code);
	lv_obj_t *img = art_img(r, t->id, th_px, th_px / 7);
	lv_obj_set_pos(img, x0, (h - th_px) / 2);
	int x = x0 + th_px + 24, ty = h / 2 - 38, sy = h / 2 + 4;
	lbl(r, t->title, font(F_TEXT, 32), same_track(cur(), t) ? pal.tint : pal.label, x, ty, w - x - x0);
	char sub[200];
	sub_text(sub, sizeof(sub), t);
	lv_font_t *f = font(F_TEXT, 26);
	lv_obj_t *s = lbl(r, sub, f, pal.label2, x, sy, w - x);
	lv_obj_set_style_pad_right(s, x0, 0);
	lv_obj_set_style_pad_bottom(s, h - sy - lv_font_get_line_height(f), 0);
	if (sep) {
		lv_obj_set_style_border_side(s, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_width(s, 1, 0);
		lv_obj_set_style_border_color(s, C(pal.separator), 0);
	}
	return r;
}

/* the playing song's title is tinted; depth = how far below c song rows may sit (Home: column in a pager) */
static void rows_mark(lv_obj_t *c, int depth)
{
	if (!c)
		return;
	const track_t *t = cur();
	uint32_t n = lv_obj_get_child_count(c);
	for (uint32_t i = 0; i < n; i++) {
		lv_obj_t *r = lv_obj_get_child(c, i);
		if (!lv_obj_has_flag(r, LV_OBJ_FLAG_USER_1)) {
			if (depth > 0)
				rows_mark(r, depth - 1);
			continue;
		}
		int code = (int)(intptr_t)lv_obj_get_user_data(r) - 1;
		const tlist_t *l = code >= 0 ? view_list(code >> 16) : NULL;
		int k = code & 0xffff;
		if (!l || k >= l->n || lv_obj_get_child_count(r) < 2)
			continue;
		lv_obj_set_style_text_color(lv_obj_get_child(r, 1), C(same_track(t, &l->v[k]) ? pal.tint : pal.label), 0);
	}
}

static lv_obj_t *hscroller(lv_obj_t *parent, int h)
{
	lv_obj_t *s = box(parent);
	lv_obj_set_size(s, SCR_W, h);
	lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(s, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_scroll_snap_x(s, LV_SCROLL_SNAP_START);
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_hor(s, SIDE, 0);
	lv_obj_set_style_pad_column(s, 24, 0);
	lv_obj_add_event_cb(s, on_scroll, LV_EVENT_SCROLL, NULL);
	return s;
}

static void card_press(lv_event_t *e)
{
	lv_obj_t *img = lv_obj_get_child(lv_event_get_current_target(e), 0);
	if (lv_event_get_code(e) == LV_EVENT_PRESSED)
		lv_obj_add_state(img, LV_STATE_PRESSED);
	else
		lv_obj_remove_state(img, LV_STATE_PRESSED);
}

static void card(lv_obj_t *parent, const track_t *t, int code)
{
	lv_obj_t *c = box(parent);
	lv_obj_set_size(c, CARD, CARD + 86);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(c, row_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)code);
	lv_obj_add_event_cb(c, card_press, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(c, card_press, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(c, card_press, LV_EVENT_PRESS_LOST, NULL);
	lv_obj_t *img = art_img(c, t->id, CARD, 20);
	lv_obj_set_style_image_recolor(img, C(0x000000), LV_STATE_PRESSED);
	lv_obj_set_style_image_recolor_opa(img, LV_OPA_30, LV_STATE_PRESSED);
	lbl(c, t->title, font(F_TEXT_MEDIUM, 28), pal.label, 0, CARD + 12, CARD);
	lbl(c, t->artist[0] ? t->artist : "Unknown Artist", font(F_TEXT, 26), pal.label2, 0, CARD + 48, CARD);
}

/* ---------- Home ---------- */

static void build_home(void)
{
	col_clear(col);
	lv_obj_scroll_to_y(col, 0, LV_ANIM_OFF);
	if (online == 0) {
		offline_state(col);
		return;
	}
	if (!home_ok) {
		if (home_err[0])
			empty_state(col, GM_NOTE, "Couldn't Load Home", home_err, true, 110);
		else
			loading_block(col, 220);
		return;
	}
	for (int i = 0; i < nshelves; i++) {
		const tlist_t *l = &shelves[i].l;
		if (!l->n)
			continue;
		lv_obj_t *h = text(col, shelves[i].title, font(F_TEXT_BOLD, 40), pal.label);
		lv_obj_set_width(h, SCR_W - 2 * SIDE);
		lv_obj_set_style_pad_top(h, i ? 40 : 4, 0);
		lv_obj_set_style_pad_bottom(h, 18, 0);
		if (i == 0) {	/* big square covers */
			lv_obj_t *s = hscroller(col, CARD + 86);
			for (int k = 0; k < l->n && k < 12; k++)
				card(s, &l->v[k], CODE(i, k));
			continue;
		}
		/* "quick picks": columns of four songs, paged sideways */
		int cw = SCR_W - 2 * SIDE - 64, n = l->n < 16 ? l->n : 16;
		lv_obj_t *s = hscroller(col, 4 * ROW_H);
		lv_obj_set_style_pad_column(s, 20, 0);
		for (int k = 0; k < n; k += 4) {
			lv_obj_t *c = box(s);
			lv_obj_set_size(c, cw, 4 * ROW_H);
			for (int m = k; m < k + 4 && m < n; m++) {
				lv_obj_t *r = song_row(c, &l->v[m], CODE(i, m), cw, 0, ROW_H, THUMB, m < k + 3 && m < n - 1);
				lv_obj_set_pos(r, 0, (m - k) * ROW_H);
				lv_obj_set_style_radius(r, 16, 0);
			}
		}
	}
}

/* ---------- Library ---------- */

static void build_page(void);
static void page_left(void);

static void open_page(bool local, int i)
{
	if (!local && (i < 0 || i >= nlists))
		return;
	lv_obj_t *c = nav_push(local ? "On iPhone" : lists[i].title);
	if (!c)
		return;
	tl_free(&pg);
	pg_col = c;
	pg_local = local;
	pg_off = 0;
	pg_err[0] = 0;
	nav_on_leave(page_left);
	lv_obj_set_style_bg_color(nav_page(), C(pal.bg), 0);
	content_setup(c);
	if (local) {
		pg_state = PG_READY;
		scan_local();
	} else {
		ucopy(pg_title, sizeof(pg_title), lists[i].title, -1);
		pg_state = PG_LOADING;
		job_t *j = job_new(J_LIST, "/list/%s", lists[i].id);
		if (j) {
			j->gen = ++pg_gen;
			submit(j);
		}
	}
	build_page();
}

static void list_tapped(lv_event_t *e) { open_page(false, (intptr_t)lv_event_get_user_data(e)); }
static void local_tapped(lv_event_t *e) { (void)e; open_page(true, 0); }

static void build_library(void)
{
	col_clear(col);
	lv_obj_t *s = ui_section(col, NULL, NULL);
	lv_obj_t *r = ui_row(s, GM_PHONE, pal.blue, "On iPhone");
	char n[16];
	snprintf(n, sizeof(n), "%d", loc.n);
	ui_row_value(r, loc_ok ? n : "");
	ui_row_chevron(r);
	ui_row_tap(r, local_tapped, NULL);

	if (!nrelays)
		return;	/* no relay configured: a local player */
	if (online == 0) {
		s = ui_section(col, "Playlists", nrelays > 1
			       ? "No connection \xe2\x80\x94 your playlists come from your music relay over Wi-Fi or through the PC."
			       : "Connect the PC \xe2\x80\x94 your playlists come from your music relay through the PC.");
		r = ui_row(s, GM_RETRY, pal.tint, "Retry");
		ui_row_tap(r, retry_clicked, NULL);
		return;
	}
	/* which relay is in use, quietly: the USB cable or Wi-Fi */
	s = ui_section(col, "Playlists", relay_shown < 0 ? NULL
		       : relays[relay_shown].wifi ? "From your music relay over Wi-Fi."
		       : !strcmp(relays[relay_shown].host, "172.16.43.2") ? "From your music relay through the PC's cable."
		       : "From your music relay.");
	if (!lists_ok && lists_failed) {
		r = ui_row(s, GM_RETRY, pal.tint, "Couldn't load playlists \xe2\x80\x94 Retry");
		ui_row_tap(r, retry_clicked, NULL);
		return;
	}
	if (!lists_ok) {
		r = ui_row(s, NULL, 0, "Loading\xe2\x80\xa6");
		lv_obj_set_style_text_color(lv_obj_get_child(lv_obj_get_user_data(r), 0), C(pal.label2), 0);
		spinner(r, 40);
		return;
	}
	if (!nlists)
		ui_row(s, NULL, 0, "No playlists yet");
	for (int i = 0; i < nlists; i++) {
		bool liked = !strcmp(lists[i].id, "LM");
		r = ui_row(s, liked ? GM_HEART : GM_PLAYLIST, liked ? pal.red : pal.tint, lists[i].title);
		if (lists[i].count[0])
			ui_row_value(r, lists[i].count);
		ui_row_chevron(r);
		ui_row_tap(r, list_tapped, (void *)(intptr_t)i);
	}
}

/* ---------- a pushed song list (playlist or On iPhone) ---------- */

static void page_left(void)
{
	pg_col = pg_rows = NULL;
	pg_gen++;
	pg_state = PG_NONE;
	pg_local = false;
	tl_free(&pg);
}

static void play_all(lv_event_t *e)
{
	bool sh = (intptr_t)lv_event_get_user_data(e);
	const tlist_t *l = view_list(V_PAGE);
	if (!l || !l->n)
		return;
	ui_haptic_tap();
	shuffle_on = sh;
	play_list(l, sh ? rand() % l->n : 0);
}

static void page_more_async(void *ud)
{
	if (!pg_col)
		return;
	pg_off += (intptr_t)ud * MAX_ROWS;
	if (pg_off < 0)
		pg_off = 0;
	build_page();
	lv_obj_scroll_to_y(pg_col, 0, LV_ANIM_OFF);
}

static void page_more(lv_event_t *e) { lv_async_call(page_more_async, lv_event_get_user_data(e)); }

static void more_row(lv_obj_t *parent, const char *label, int dir)
{
	lv_obj_t *r = box(parent);
	lv_obj_set_size(r, SCR_W, 100);
	lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(r, C(pal.cell_hi), LV_STATE_PRESSED);
	lv_obj_add_event_cb(r, page_more, LV_EVENT_CLICKED, (void *)(intptr_t)dir);
	lv_obj_center(text(r, label, font(F_TEXT_SEMI, 32), pal.tint));
}

static void page_header(const tlist_t *l)
{
	if (!pg_local) {	/* 2×2 mosaic of the first covers, like a playlist tile */
		lv_obj_t *m = box(pg_col);
		lv_obj_set_size(m, 400, 400);
		lv_obj_set_style_margin_top(m, 12, 0);
		if (l->n >= 4) {
			for (int k = 0; k < 4; k++)
				lv_obj_set_pos(art_img(m, l->v[k].id, 196, 18), k % 2 * 204, k / 2 * 204);
		} else {
			art_img(m, l->v[0].id, 400, 28);
		}
		lv_obj_t *t = text(pg_col, pg_title, font(F_TEXT_BOLD, 44), pal.label);
		lv_obj_set_width(t, SCR_W - 2 * SIDE);
		lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
		lv_label_set_max_lines(t, 2);
		lv_obj_set_style_margin_top(t, 28, 0);
	}
	long tot = 0;
	for (int i = 0; i < l->n; i++)
		tot += l->v[i].secs;
	char sub[80], d[40] = "";
	if (tot >= 3600)
		snprintf(d, sizeof(d), " \xc2\xb7 %ld hr %ld min", tot / 3600, tot / 60 % 60);
	else if (tot >= 60)
		snprintf(d, sizeof(d), " \xc2\xb7 %ld min", tot / 60);
	snprintf(sub, sizeof(sub), "%d song%s%s", l->n, l->n == 1 ? "" : "s", d);
	lv_obj_t *s = text(pg_col, sub, font(F_TEXT, 28), pal.label2);
	lv_obj_set_style_margin_top(s, pg_local ? 24 : 8, 0);

	lv_obj_t *b = box(pg_col);
	lv_obj_set_size(b, SCR_W - 2 * SIDE, 96);
	lv_obj_set_style_margin_top(b, 28, 0);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	static const char *lab[] = { G_PLAY "  Play", GM_SHUFFLE "  Shuffle" };
	for (int k = 0; k < 2; k++) {
		lv_obj_t *p = ui_button(b, lab[k], false);
		lv_obj_set_width(p, (SCR_W - 2 * SIDE - 24) / 2);
		lv_obj_add_event_cb(p, play_all, LV_EVENT_CLICKED, (void *)(intptr_t)k);
	}
}

static void build_page(void)
{
	if (!pg_col)
		return;
	lv_obj_clean(pg_col);
	pg_rows = NULL;
	const tlist_t *l = view_list(V_PAGE);
	if (pg_state == PG_LOADING) {
		loading_block(pg_col, 260);
		return;
	}
	if (pg_state == PG_ERROR) {
		empty_state(pg_col, GM_PLAYLIST, "Couldn't Load Playlist", pg_err, false, 140);
		return;
	}
	if (!l->n) {
		if (pg_local)
			empty_state(pg_col, GM_PHONE, "No Songs on iPhone",
				    "Copy MP3s to the phone with bin/orc music \xe2\x80\x94 they play without the PC.", false,
				    140);
		else
			empty_state(pg_col, GM_PLAYLIST, "No Songs", "This playlist is empty.", false, 140);
		return;
	}
	page_header(l);
	pg_rows = box(pg_col);
	lv_obj_set_size(pg_rows, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(pg_rows, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_margin_top(pg_rows, 24, 0);
	if (pg_off > 0)
		more_row(pg_rows, "Show Previous", -1);
	int end = pg_off + MAX_ROWS < l->n ? pg_off + MAX_ROWS : l->n;
	for (int i = pg_off; i < end; i++)
		song_row(pg_rows, &l->v[i], CODE(V_PAGE, i), SCR_W, SIDE, ROW_H, THUMB, i < end - 1 || end < l->n);
	if (end < l->n)
		more_row(pg_rows, "Show More", 1);
}

/* ---------- Search ---------- */

static void run_search(void)
{
	if (!s_ta)
		return;
	const char *t = lv_textarea_get_text(s_ta);
	while (*t == ' ')
		t++;
	if (!*t || (!strcmp(t, sq) && s_state != S_ERR && s_state != S_IDLE))
		return;
	ucopy(sq, sizeof(sq), t, -1);
	char enc[480];
	urlenc(enc, sizeof(enc), sq);
	job_t *j = job_new(J_SEARCH, "/search?q=%s", enc);
	if (!j)
		return;
	j->gen = ++s_gen;
	s_state = S_BUSY;
	submit(j);
	fill_search();
}

static void search_due(lv_timer_t *t)
{
	lv_timer_pause(t);
	run_search();
}

static void fill_search(void)
{
	if (!s_res)
		return;
	lv_obj_clean(s_res);
	if (online == 0) {
		offline_state(s_res);
		return;
	}
	switch (s_state) {
	case S_IDLE:
		empty_state(s_res, GM_SEARCH, "Search Your Music", "Songs and artists from your music relay.", false, 90);
		return;
	case S_BUSY:
		loading_block(s_res, 120);
		return;
	case S_ERR:
		empty_state(s_res, GM_SEARCH, "Couldn't Search", s_err, true, 90);
		return;
	}
	if (!sres.n) {
		char b[220];
		snprintf(b, sizeof(b), "Nothing found for \xe2\x80\x9c%s\xe2\x80\x9d.", sq);
		empty_state(s_res, GM_SEARCH, "No Results", b, false, 90);
		return;
	}
	int n = sres.n < MAX_ROWS ? sres.n : MAX_ROWS;
	for (int i = 0; i < n; i++)
		song_row(s_res, &sres.v[i], CODE(V_SEARCH, i), SCR_W, SIDE, ROW_H, THUMB, i < n - 1);
	art_dirty = true;
}

static void search_ev(lv_event_t *e)
{
	lv_event_code_t c = lv_event_get_code(e);
	if (c == LV_EVENT_VALUE_CHANGED) {
		const char *t = lv_textarea_get_text(s_ta);
		if (!*t) {	/* cleared: back to the hint */
			lv_timer_pause(s_timer);
			s_gen++;
			sq[0] = 0;
			s_state = S_IDLE;
			tl_free(&sres);
			fill_search();
		} else if (strlen(t) >= 2) {	/* search a moment after typing stops */
			lv_timer_reset(s_timer);
			lv_timer_resume(s_timer);
		}
	} else if (c == LV_EVENT_READY) {
		lv_timer_pause(s_timer);
		run_search();
	} else if (c == EV_KBD_SHOWN) {
		kbd_no_autocap();
		lv_obj_set_style_pad_bottom(col, kbd_height(KBD_TEXT) + 40, 0);
	} else if (c == EV_KBD_HIDDEN && col) {
		lv_obj_set_style_pad_bottom(col, bottom_pad(), 0);
	}
}

static void build_search(void)
{
	s_ta = ui_search(col, SCR_W - 2 * SIDE, "Songs, artists");
	lv_obj_set_style_margin_bottom(lv_obj_get_parent(s_ta), 12, 0);
	if (sq[0])
		lv_textarea_set_text(s_ta, sq);
	lv_obj_add_event_cb(s_ta, search_ev, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(s_ta, search_ev, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(s_ta, search_ev, EV_KBD_SHOWN, NULL);
	lv_obj_add_event_cb(s_ta, search_ev, EV_KBD_HIDDEN, NULL);
	s_res = box(col);
	lv_obj_set_size(s_res, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(s_res, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(s_res, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	fill_search();
}

/* ---------- tabs ---------- */

static void build_tab(void)
{
	static const char *titles[] = { "Home", "Library", "Search" };
	kbd_hide();
	nav_reset();	/* leave callbacks run while their objects still exist */
	col = pg_col = pg_rows = s_ta = s_res = NULL;
	if (host)
		lv_obj_delete(host);
	host = box(root);
	lv_obj_set_size(host, SCR_W, SCR_H);
	lv_obj_move_to_index(host, 0);
	col = nav_root(host, titles[tab]);
	if (tab != 1)
		lv_obj_set_style_bg_color(nav_page(), C(pal.bg), 0);
	content_setup(col);
	if (tab == 0)
		build_home();
	else if (tab == 1)
		build_library();
	else
		build_search();
}

static void tab_changed(lv_event_t *e)
{
	tab = ui_tabbar_get(lv_event_get_target(e));
	build_tab();
}

/* ---------- mini player ---------- */

static void np_open(void);

static void mini_play_cb(lv_event_t *e)
{
	(void)e;
	music_toggle();
	ui_haptic_tap();
}

static void mini_next_cb(lv_event_t *e)
{
	(void)e;
	music_next();
	ui_haptic_tap();
}

static void mini_open_cb(lv_event_t *e)
{
	(void)e;
	np_open();
}

static lv_obj_t *icon_btn(lv_obj_t *parent, const char *glyph, int gpx, int w, int h, lv_event_cb_t cb)
{
	lv_obj_t *b = box(parent);
	lv_obj_set_size(b, w, h);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_text_opa(b, LV_OPA_40, LV_STATE_PRESSED);	/* inherited by the glyph */
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	lv_obj_center(ui_icon(b, glyph, font(F_MONO, gpx), pal.label));	/* another glyph: ui_icon_set */
	return b;
}

static void pads_update(void)
{
	if (col && !(s_ta && lv_obj_has_state(s_ta, LV_STATE_FOCUSED)))
		lv_obj_set_style_pad_bottom(col, bottom_pad(), 0);
	if (pg_col)
		lv_obj_set_style_pad_bottom(pg_col, bottom_pad(), 0);
}

static void mini_build(void)
{
	if (mini || !root || !cur())
		return;
	mini = box(root);
	lv_obj_set_size(mini, SCR_W - 80, MINI_H);
	lv_obj_set_pos(mini, 40, MINI_Y);
	lv_obj_set_style_radius(mini, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(mini, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(mini, C(th.dark ? mix(pal.cell, th.fg, 4) : pal.cell), 0);
	/* no shadow: the list scrolls under it, so a soft shadow would be blurred again every frame
	 * (the software renderer has no shadow cache) — the hairline border sets it off instead */
	lv_obj_set_style_border_width(mini, 1, 0);
	lv_obj_set_style_border_color(mini, C(pal.separator), 0);
	lv_obj_add_flag(mini, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(mini, mini_open_cb, LV_EVENT_CLICKED, NULL);
	mini_img = art_img(mini, "", THUMB, 20);
	lv_obj_set_pos(mini_img, 12, 12);
	mini_title = lbl(mini, "", font(F_TEXT_SEMI, 30), pal.label, 112, 18, SCR_W - 80 - 112 - 210);
	mini_sub = lbl(mini, "", font(F_TEXT, 26), pal.label2, 112, 58, SCR_W - 80 - 112 - 210);
	mini_btn = icon_btn(mini, G_PLAY, 44, 96, MINI_H, mini_play_cb);
	lv_obj_set_pos(mini_btn, SCR_W - 80 - 204, 0);
	mini_play = lv_obj_get_child(mini_btn, 0);
	lv_obj_t *nx = icon_btn(mini, G_FORWARD, 40, 96, MINI_H, mini_next_cb);
	lv_obj_set_pos(nx, SCR_W - 80 - 112, 0);
	if (np)
		lv_obj_move_foreground(np);
	shown_state = -1;
	pads_update();
}

/* ---------- Now Playing ---------- */

static void set_y(void *o, int32_t v) { lv_obj_set_y(o, v); }

static void np_anim(int from, int to, int ms, lv_anim_completed_cb_t done)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, np);
	lv_anim_set_exec_cb(&a, set_y);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_completed_cb(&a, done);
	lv_anim_start(&a);
}

static void under_np(bool hide)	/* nothing to draw under the full-screen sheet */
{
	lv_obj_t *o[] = { host, tabbar, mini };
	for (int i = 0; i < 3; i++)
		if (o[i]) {
			if (hide)
				lv_obj_add_flag(o[i], LV_OBJ_FLAG_HIDDEN);
			else
				lv_obj_remove_flag(o[i], LV_OBJ_FLAG_HIDDEN);
		}
}

static void np_shown(lv_anim_t *a)
{
	(void)a;
	if (np && !np_closing)
		under_np(true);
}

static void np_gone(lv_anim_t *a)
{
	(void)a;
	if (np)
		lv_obj_delete(np);
	np = np_big = np_img = np_qv = np_title = np_artist = np_slider = np_el = np_rem = NULL;
	np_btn = np_play = np_spin = np_shuf = np_rep = np_qbtn = np_vol = NULL;
	np_closing = np_dragging = np_queue_open = false;
	art_dirty = true;
}

static void np_close(void)
{
	if (!np || np_closing)
		return;
	np_closing = true;
	nav_lock(false);
	under_np(false);
	np_anim(lv_obj_get_y(np), SCR_H, 260, np_gone);
}

static void np_close_cb(lv_event_t *e)
{
	(void)e;
	np_close();
}

static void np_gesture(lv_event_t *e)
{
	(void)e;
	if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM) {
		lv_indev_wait_release(lv_indev_active());
		np_close();
	}
}

static int track_len(void)
{
	const track_t *t = cur();
	return pl.dur > 0 ? (int)(pl.dur + 0.5) : t ? t->secs : 0;
}

static void np_times(int p, int d)
{
	char a[16], b[20] = "--:--";
	fmt_time(a, sizeof(a), p);
	if (d > 0) {
		b[0] = '-';
		fmt_time(b + 1, sizeof(b) - 1, d > p ? d - p : 0);
	}
	lv_label_set_text(np_el, a);
	lv_label_set_text(np_rem, b);
}

static void np_progress(void)
{
	if (!np || np_dragging)
		return;
	int d = track_len(), p = (int)pl.pos;
	lv_slider_set_value(np_slider, d > 0 ? (int)(pl.pos * 1000 / d) : 0, LV_ANIM_OFF);
	if (p != np_shown_p || d != np_shown_d) {
		np_shown_p = p;
		np_shown_d = d;
		np_times(p, d);
	}
}

static void np_seek_ev(lv_event_t *e)
{
	lv_event_code_t c = lv_event_get_code(e);
	int d = track_len(), v = lv_slider_get_value(np_slider);
	if (c == LV_EVENT_PRESSED) {
		np_dragging = true;
	} else if (c == LV_EVENT_VALUE_CHANGED) {
		if (np_dragging)
			np_times(v * d / 1000, d);
	} else if (np_dragging) {	/* released / press lost */
		np_dragging = false;
		if (d > 0)	/* length unknown: a drag would jump back to the start */
			music_seek(v * d / 1000);
		np_shown_p = -1;
		np_progress();
	}
}

static void np_vol_ev(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED && lv_tick_elaps(vol_at) < 150)
		return;	/* amixer runs per change: not on every pixel of a drag */
	vol_at = lv_tick_get();
	sys_volume_set(lv_slider_get_value(np_vol));
}

static void toggle_look(lv_obj_t *b, bool on, const char *glyph)
{
	if (!b)
		return;
	lv_obj_t *g = lv_obj_get_child(b, 0);
	lv_obj_set_style_bg_opa(b, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_text_color(g, C(on ? pal.tint : pal.label2), 0);
	if (glyph)
		ui_icon_set(g, glyph);
}

static void np_toggles(void)
{
	toggle_look(np_shuf, shuffle_on, NULL);
	toggle_look(np_rep, repeat_mode, repeat_mode == 2 ? GM_REPEAT1 : GM_REPEAT);
	toggle_look(np_qbtn, np_queue_open, NULL);
}

static int lum(uint32_t c) { return ((c >> 16 & 0xff) * 299 + (c >> 8 & 0xff) * 587 + (c & 0xff) * 114) / 1000; }

static void np_recolor(void)
{
	if (!np)
		return;
	art_t *a = img_art(np_img);
	uint32_t top = a ? mix(a->avg, pal.bg, th.dark ? 45 : 70) : mix(pal.tint, pal.bg, th.dark ? 60 : 80);
	/* keep the labels readable: dark themes get a dark tint, light themes a light one */
	for (int i = 0; i < 8 && (th.dark ? lum(top) > 70 : lum(top) < 190); i++)
		top = mix(top, pal.bg, 30);
	lv_obj_set_style_bg_color(np, C(top), 0);
}

static void np_queue_fill(void)
{
	if (!np_qv)
		return;
	lv_obj_clean(np_qv);
	const track_t *t = cur();
	if (!t)
		return;
	lv_obj_set_pos(art_img(np_qv, t->id, 120, 18), NP_X, 0);
	lbl(np_qv, t->title, font(F_TEXT_SEMI, 34), pal.label, NP_X + 144, 20, NP_ART - 144);
	lbl(np_qv, t->artist, font(F_TEXT, 28), pal.label2, NP_X + 144, 66, NP_ART - 144);
	lbl(np_qv, "Up Next", font(F_TEXT_BOLD, 36), pal.label, NP_X, 160, 0);
	int shown = 0;
	for (int k = 1; k <= 5 && qpos + k < q.n; k++, shown++) {
		int i = qpos + k;
		lv_obj_t *r = song_row(np_qv, &q.v[i], CODE(V_QUEUE, i), SCR_W, NP_X, 100, THUMB, k < 5 && i < q.n - 1);
		lv_obj_set_pos(r, 0, 212 + (k - 1) * 100);
	}
	if (!shown) {
		const track_t *last = q.n ? &q.v[q.n - 1] : NULL;
		lbl(np_qv, repeat_mode == 1 ? "The queue starts over next." :
			   last && last->id[0] ? "Similar songs play next." : "Nothing else is queued.",
		    font(F_TEXT, 30), pal.label2, NP_X, 236, NP_ART);
	}
}

static void np_queue_show(bool on)
{
	np_queue_open = on;
	lv_obj_t *big[] = { np_big, np_title, np_artist };
	for (int i = 0; i < 3; i++) {
		if (on)
			lv_obj_add_flag(big[i], LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_remove_flag(big[i], LV_OBJ_FLAG_HIDDEN);
	}
	if (on) {
		lv_obj_remove_flag(np_qv, LV_OBJ_FLAG_HIDDEN);
		np_queue_fill();
	} else {
		lv_obj_add_flag(np_qv, LV_OBJ_FLAG_HIDDEN);
		lv_obj_clean(np_qv);
	}
	np_toggles();
	art_dirty = true;
}

static void np_fill(void)
{
	const track_t *t = cur();
	if (!np || !t)
		return;
	lv_label_set_text(np_title, t->title);
	lv_label_set_text(np_artist, t->artist[0] ? t->artist : "Unknown Artist");
	art_img_set(np_img, t->id);
	np_recolor();
	np_shown_p = -1;
	np_progress();
	np_toggles();
	if (np_queue_open)
		np_queue_fill();
}

static void np_prev_cb(lv_event_t *e)
{
	(void)e;
	music_prev();
	ui_haptic_tap();
}

static void np_next_cb(lv_event_t *e)
{
	(void)e;
	music_next();
	ui_haptic_tap();
}

static void np_shuffle_cb(lv_event_t *e)
{
	(void)e;
	set_shuffle(!shuffle_on);
	np_toggles();
	ui_haptic_tap();
}

static void np_repeat_cb(lv_event_t *e)
{
	(void)e;
	repeat_mode = (repeat_mode + 1) % 3;
	prefetch();
	np_toggles();
	ui_queue();
	ui_haptic_tap();
}

static void np_queue_cb(lv_event_t *e)
{
	(void)e;
	np_queue_show(!np_queue_open);
	ui_haptic_tap();
}

static lv_obj_t *toggle_btn(lv_obj_t *parent, const char *glyph, int cx, int cy, lv_event_cb_t cb)
{
	lv_obj_t *b = icon_btn(parent, glyph, 40, 112, 80, cb);
	lv_obj_set_pos(b, cx - 56, cy - 40);
	lv_obj_set_style_radius(b, 24, 0);
	lv_obj_set_style_bg_color(b, C(pal.fill), 0);
	return b;
}

static void np_build(void)
{
	np = box(root);
	lv_obj_set_size(np, SCR_W, SCR_H);
	lv_obj_add_flag(np, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_opa(np, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(np, C(pal.bg), 0);
	lv_obj_set_style_bg_grad_color(np, C(pal.bg), 0);
	lv_obj_set_style_bg_grad_dir(np, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_main_stop(np, 0, 0);
	lv_obj_set_style_bg_grad_stop(np, 235, 0);
	lv_obj_remove_flag(np, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(np, np_gesture, LV_EVENT_GESTURE, NULL);

	lv_obj_t *grab = box(np);	/* sheet grabber; tap or swipe down closes */
	lv_obj_set_size(grab, 72, 10);
	lv_obj_set_pos(grab, (SCR_W - 72) / 2, STATUS_H + 18);
	lv_obj_set_style_radius(grab, 5, 0);
	lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(grab, C(pal.label3), 0);
	lv_obj_t *hit = box(np);
	lv_obj_set_size(hit, 320, 64);
	lv_obj_set_pos(hit, (SCR_W - 320) / 2, STATUS_H);
	lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(hit, np_close_cb, LV_EVENT_CLICKED, NULL);

	np_big = box(np);
	lv_obj_set_size(np_big, SCR_W, NP_ART);
	lv_obj_set_pos(np_big, 0, NP_TOP);
	np_img = art_img(np_big, "", NP_ART, 24);
	lv_obj_set_pos(np_img, NP_X, 0);
	np_qv = box(np);
	lv_obj_set_size(np_qv, SCR_W, 720);
	lv_obj_set_pos(np_qv, 0, NP_TOP);
	lv_obj_add_flag(np_qv, LV_OBJ_FLAG_HIDDEN);

	int y = NP_TOP + NP_ART + 40;
	np_title = lbl(np, "", font(F_TEXT_SEMI, 42), pal.label, NP_X, y, NP_ART);
	np_artist = lbl(np, "", font(F_TEXT, 34), pal.label2, NP_X, y + 54, NP_ART);

	np_slider = ui_slider(np, NP_ART, 0, 1000, 0);
	lv_obj_set_pos(np_slider, NP_X, y + 140);
	lv_obj_set_style_bg_color(np_slider, C(mix(pal.label2, pal.bg, 50)), 0);
	lv_obj_set_style_bg_color(np_slider, C(pal.label), LV_PART_INDICATOR);
	lv_obj_set_style_pad_all(np_slider, 8, LV_PART_KNOB);
	lv_obj_set_style_bg_opa(np_slider, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(np_slider, LV_OPA_TRANSP, LV_PART_KNOB);
	lv_obj_set_style_bg_opa(np_slider, LV_OPA_COVER, LV_PART_KNOB | LV_STATE_PRESSED);
	lv_obj_remove_flag(np_slider, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(np_slider, np_seek_ev, LV_EVENT_PRESSED, NULL);
	lv_obj_add_event_cb(np_slider, np_seek_ev, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(np_slider, np_seek_ev, LV_EVENT_RELEASED, NULL);
	lv_obj_add_event_cb(np_slider, np_seek_ev, LV_EVENT_PRESS_LOST, NULL);
	np_el = lbl(np, "0:00", font(F_TEXT_MEDIUM, 24), pal.label2, NP_X, y + 168, 0);
	np_rem = lbl(np, "", font(F_TEXT_MEDIUM, 24), pal.label2, NP_X + NP_ART - 200, y + 168, 200);
	lv_obj_set_style_text_align(np_rem, LV_TEXT_ALIGN_RIGHT, 0);

	bool vol = sys_volume_available();
	int cy = vol ? 1010 : 1044;
	lv_obj_t *b = icon_btn(np, G_BACKWARD, 60, 150, 130, np_prev_cb);
	lv_obj_set_pos(b, SCR_W / 2 - 200 - 75, cy - 65);
	np_btn = icon_btn(np, G_PLAY, 84, 170, 150, mini_play_cb);
	lv_obj_set_pos(np_btn, SCR_W / 2 - 85, cy - 75);
	np_play = lv_obj_get_child(np_btn, 0);
	b = icon_btn(np, G_FORWARD, 60, 150, 130, np_next_cb);
	lv_obj_set_pos(b, SCR_W / 2 + 200 - 75, cy - 65);

	if (vol) {
		int vy = 1128;
		lv_obj_t *lo = ui_icon(np, G_VOLUME_LO, font(F_MONO, 30), pal.label2);	/* ink on the slider's line, */
		lv_obj_set_pos(lo, NP_X, vy - lv_obj_get_style_height(lo, 0) / 2);	/* from the art's left edge */
		np_vol = ui_slider(np, NP_ART - 120, 0, 100, sys_volume_get());
		lv_obj_set_pos(np_vol, NP_X + 56, vy - 6);
		lv_obj_set_style_bg_color(np_vol, C(mix(pal.label2, pal.bg, 50)), 0);
		lv_obj_set_style_bg_color(np_vol, C(pal.label), LV_PART_INDICATOR);
		lv_obj_set_style_pad_all(np_vol, 12, LV_PART_KNOB);
		lv_obj_remove_flag(np_vol, LV_OBJ_FLAG_GESTURE_BUBBLE);
		lv_obj_add_event_cb(np_vol, np_vol_ev, LV_EVENT_VALUE_CHANGED, NULL);
		lv_obj_add_event_cb(np_vol, np_vol_ev, LV_EVENT_RELEASED, NULL);
		lv_obj_t *hi = ui_icon(np, G_VOLUME, font(F_MONO, 36), pal.label2);	/* … to its right edge */
		lv_obj_set_pos(hi, NP_X + NP_ART - lv_obj_get_style_width(hi, 0), vy - lv_obj_get_style_height(hi, 0) / 2);
	}

	int by = vol ? 1236 : 1206;
	np_shuf = toggle_btn(np, GM_SHUFFLE, SCR_W / 2 - 200, by, np_shuffle_cb);
	np_qbtn = toggle_btn(np, GM_QUEUE, SCR_W / 2, by, np_queue_cb);
	np_rep = toggle_btn(np, GM_REPEAT, SCR_W / 2 + 200, by, np_repeat_cb);
}

static void np_open(void)
{
	if (np || !root || !cur())
		return;
	np_build();
	np_fill();
	shown_state = -1;
	ui_sync();
	lv_obj_set_y(np, SCR_H);
	np_anim(SCR_H, 0, 300, np_shown);
	nav_lock(true);	/* the edge swipe would pop the hidden page under the sheet */
}

/* ---------- state → UI ---------- */

static void play_btn(lv_obj_t *btn, lv_obj_t *glyph, lv_obj_t **spin, int size, bool loading, bool playing)
{
	if (!btn)
		return;
	ui_icon_set(glyph, playing ? G_PAUSE : G_PLAY);
	if (loading && !*spin) {
		*spin = spinner(btn, size);
		lv_obj_center(*spin);
		lv_obj_add_flag(glyph, LV_OBJ_FLAG_HIDDEN);
	} else if (!loading && *spin) {
		lv_obj_delete(*spin);
		*spin = NULL;
		lv_obj_remove_flag(glyph, LV_OBJ_FLAG_HIDDEN);
	}
}

static void ui_sync(void)
{
	if (!root)
		return;
	bool loading = pl.state == PS_LOADING && pl.want, playing = music_playing();
	int st = loading * 2 + playing;
	if (st == shown_state)
		return;
	shown_state = st;
	play_btn(mini_btn, mini_play, &mini_spin, 48, loading, playing);
	play_btn(np_btn, np_play, &np_spin, 76, loading, playing);
}

static void ui_track(void)
{
	if (!root)
		return;
	const track_t *t = cur();
	if (t && !mini)
		mini_build();
	if (mini && t) {
		lv_label_set_text(mini_title, t->title);
		lv_label_set_text(mini_sub, t->artist[0] ? t->artist : "Unknown Artist");
		art_img_set(mini_img, t->id);
	}
	if (np) {
		if (t)
			np_fill();
		else
			np_close();
	}
	rows_mark(pg_rows, 0);
	rows_mark(s_res, 0);
	if (tab == 0)
		rows_mark(col, 2);	/* Home: col → pager → column of four → row */
	shown_state = -1;
	ui_sync();
}

static void ui_queue(void)
{
	if (root && np && np_queue_open)
		np_queue_fill();
}

static void ui_tick(lv_timer_t *t)
{
	(void)t;
	ui_sync();
	np_progress();
	if (np_vol && !lv_obj_has_state(np_vol, LV_STATE_PRESSED))
		lv_slider_set_value(np_vol, sys_volume_get(), LV_ANIM_OFF);
}

/* the lock screen over the app: progress / volume stop ticking, the engine takes its closed pace (playback is
 * the same as with the app closed); unlock = both at once */
static void lock_changed(bool locked)
{
	if (ui_timer && locked) {
		lv_timer_pause(ui_timer);
	} else if (ui_timer) {
		lv_timer_resume(ui_timer);
		lv_timer_ready(ui_timer);
	}
	if (!locked && root && eng_t && !power_dark())
		lv_timer_ready(eng_t);
}

/* ---------- app ---------- */

static void app_open(lv_obj_t *r)
{
	static const char *glyphs[] = { GM_HOME, GM_LIBRARY, GM_SEARCH };
	static const char *labels[] = { "Home", "Library", "Search" };
	root = r;
	host = NULL;
	if (!home_ok || lv_tick_elaps(home_at) > 600000)
		fetch_home();
	if (!lists_ok || lv_tick_elaps(lists_at) > 600000)
		fetch_lists();
	if (lv_tick_elaps(loc_at) > 30000)
		scan_local();
	tabbar = ui_tabbar(root, glyphs, labels, 3, tab);
	lv_obj_add_event_cb(tabbar, tab_changed, LV_EVENT_VALUE_CHANGED, NULL);
	build_tab();
	ui_track();
	ui_timer = lv_timer_create(ui_tick, 500, NULL);
	lock_listen(lock_changed, true);
	s_timer = lv_timer_create(search_due, 700, NULL);
	lv_timer_pause(s_timer);
}

static void app_close(void)
{
	if (ui_timer)
		lv_timer_delete(ui_timer);
	if (s_timer)
		lv_timer_delete(s_timer);
	ui_timer = s_timer = NULL;
	root = host = tabbar = col = pg_col = pg_rows = s_ta = s_res = NULL;
	mini = mini_img = mini_title = mini_sub = mini_btn = mini_play = mini_spin = NULL;
	np = np_big = np_img = np_qv = np_title = np_artist = np_slider = np_el = np_rem = NULL;
	np_btn = np_play = np_spin = np_shuf = np_rep = np_qbtn = np_vol = NULL;
	np_closing = np_dragging = np_queue_open = false;
	shown_state = -1;
}

/* swiped away in the app switcher: music stops, like iOS */
static void app_kill(void)
{
	be_stop();
	pl.state = PS_IDLE;
	pl.pos = 0;
	tl_free(&q);
	qpos = -1;
	pins_update();
	radio_gen++;
	radio_wait = false;
	cancel_songs("", "");
}

const app_t app_music = {
	.name = "Music",
	.glyph = G_MUSIC,
	.icon = IC_ACCENT,
	.open = app_open,
	.close = app_close,
	.kill = app_kill,
};
