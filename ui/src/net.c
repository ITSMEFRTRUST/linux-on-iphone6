/* HTTP(S) for apps through a curl child process per request; see net.h. */
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "net.h"
#include "orchard.h"

#define TICK_MS 20
#define READ_PER_TICK (1 << 20)	/* keep each timer pass short */
#define ERR_KEEP 2048
#define MARK "@@ORCHARD@@ "
#define UA "Mozilla/5.0 (Linux; aarch64; Orchard Web) Mobile"
#define UA_DESKTOP "Mozilla/5.0 (X11; Linux aarch64; rv:128.0) Gecko/20100101 Firefox/128.0"
#define MAX_POST (64 << 10)
#ifndef WIFI_WAIT_MS	/* (test/unit/net_wifiwait_test.c: shorter) */
#define WIFI_WAIT_MS 20000	/* a request made while Wi-Fi Sleep has the network away waits this long at most */
#endif
#define MAX_HOPS 8		/* redirects, followed here (not curl -L) so every hop's address is checked */
#define MAX_ADDRS 8

struct dns_job;

struct net_req {
	pid_t pid;		/* 0 = no curl running (resolving, or failing before one started) */
	int out, err;		/* -1 once at EOF */
	char url[2048];		/* this hop's URL */
	char host[256];		/* this hop's host ("[v6]" bracketed), port */
	int port, hops;
	char *post;		/* POST body (NULL = GET) */
	char referer[2048];	/* first hop only */
	bool desktop, posted;
	int cookies;
	char post_site[256];	/* site of the page a POST came from ("" = none): cross-site = no cookies sent */
	bool ck_merge;		/* this hop's curl reads ck_snap, writes ck_out; merged into ck_jar when it exits */
	unsigned ck_epoch;
	char ck_jar[400], ck_snap[420], ck_out[420];
	bool allow_priv;	/* this hop may be PRIVATE */
	bool hop_priv;		/* this hop is */
	struct dns_job *dns;	/* resolving the host (worker thread of its own) */
	int fail;		/* NET_E*: finish with this on the next tick, no curl */
	char *buf;
	size_t len, cap, max;
	bool trunc, timed_out;
	long long trunc_at;	/* ms: when its pipe was closed at max (curl gets 1.5 s to exit on its own) */
	char ebuf[ERR_KEEP + 1];
	int elen;
	long long deadline;	/* ms, CLOCK_MONOTONIC (snap's simulated clock must not time requests out) */
	long long wifi_since;	/* ms: made while Wi-Fi Sleep had the network away — waiting for it (0 = not) */
	net_cb_t cb;
	void *ud;
	struct net_req *next;
};

static struct net_req *reqs;
static lv_timer_t *timer;
static pid_t zombies[32];
static int nzombies;

/* ---------- worker thread ---------- */

struct job { void (*work)(void *), (*done)(void *); void *arg; struct job *next; };
static pthread_mutex_t jmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t jcv = PTHREAD_COND_INITIALIZER;
static struct job *jq, *jdone;	/* pending (FIFO), finished (LIFO, reversed when delivered) */
static int jpending;		/* queued + running + finished-not-delivered (LVGL thread only) */
static bool worker_up;

static void *worker(void *a)
{
	(void)a;
	for (;;) {
		pthread_mutex_lock(&jmu);
		while (!jq)
			pthread_cond_wait(&jcv, &jmu);
		struct job *j = jq;
		jq = j->next;
		pthread_mutex_unlock(&jmu);
		j->work(j->arg);
		pthread_mutex_lock(&jmu);
		j->next = jdone;
		jdone = j;
		pthread_mutex_unlock(&jmu);
	}
	return NULL;
}

static void timer_need(void);

void net_bg(void (*work)(void *), void (*done)(void *), void *arg)
{
	if (!worker_up) {
		pthread_t t;
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setstacksize(&at, 1 << 20);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		worker_up = pthread_create(&t, &at, worker, NULL) == 0;
		pthread_attr_destroy(&at);
		if (!worker_up) {	/* no thread: do it inline rather than never */
			work(arg);
			done(arg);
			return;
		}
	}
	struct job *j = calloc(1, sizeof(*j));
	if (!j) {
		work(arg);
		done(arg);
		return;
	}
	j->work = work;
	j->done = done;
	j->arg = arg;
	pthread_mutex_lock(&jmu);
	struct job **p = &jq;
	while (*p)
		p = &(*p)->next;
	*p = j;
	pthread_cond_signal(&jcv);
	pthread_mutex_unlock(&jmu);
	jpending++;
	timer_need();
}

static void jobs_deliver(void)
{
	pthread_mutex_lock(&jmu);
	struct job *d = jdone, *rev = NULL;
	jdone = NULL;
	pthread_mutex_unlock(&jmu);
	while (d) {	/* finished list is newest first: deliver in order */
		struct job *n = d->next;
		d->next = rev;
		rev = d;
		d = n;
	}
	while (rev) {
		struct job *n = rev->next;
		jpending--;
		rev->done(rev->arg);
		free(rev);
		rev = n;
	}
}

/* ---------- curl ---------- */

static long long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static const char *curl_path(void)
{
	static char path[256];
	static bool done;
	if (done)
		return path[0] ? path : NULL;
	done = true;
	const char *env = getenv("ORCHARD_CURL");
	const char *cand[] = { env, "/usr/bin/curl", "/bin/curl", "/usr/local/bin/curl", "/data/bin/curl" };
	for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++)
		if (cand[i] && *cand[i] && access(cand[i], X_OK) == 0) {
			snprintf(path, sizeof(path), "%s", cand[i]);
			return path;
		}
	return NULL;
}

static const char *ca_path(void)
{
	static char path[256];
	static bool done;
	if (done)
		return path[0] ? path : NULL;
	done = true;
	const char *env = getenv("ORCHARD_CACERT");
	const char *cand[] = { env, "/etc/ssl/certs/ca-certificates.crt", "/data/ssl/cacert.pem",
			       "/usr/share/orchard/cacert.pem" };
	for (unsigned i = 0; i < sizeof(cand) / sizeof(cand[0]); i++)
		if (cand[i] && *cand[i] && access(cand[i], R_OK) == 0) {
			snprintf(path, sizeof(path), "%s", cand[i]);
			return path;
		}
	return NULL;	/* curl's built-in default (it fails closed without a bundle) */
}

bool net_available(void) { return curl_path() != NULL; }

int net_active(void)
{
	int n = 0;
	for (struct net_req *r = reqs; r; r = r->next)
		n++;
	return n;
}

static void tick(lv_timer_t *t);

static void timer_need(void)
{
	if (!timer) {
		timer = lv_timer_create(tick, TICK_MS, NULL);
		power_keep(timer, 0, "net");	/* jobs finish with the screen off (it pauses itself when idle) */
	} else
		lv_timer_resume(timer);
}

static void close_fd(int *fd)
{
	if (*fd >= 0)
		close(*fd);
	*fd = -1;
}

static void zombie_add(pid_t p)
{
	if (p <= 0)
		return;
	if (waitpid(p, NULL, WNOHANG) != 0)
		return;
	if (nzombies < (int)(sizeof(zombies) / sizeof(zombies[0])))
		zombies[nzombies++] = p;
	else
		waitpid(p, NULL, 0);	/* killed with SIGKILL: returns at once */
}

static void ck_done(struct net_req *r, bool exited);

static void unlink_req(struct net_req *r)
{
	for (struct net_req **p = &reqs; *p; p = &(*p)->next)
		if (*p == r) {
			*p = r->next;
			return;
		}
}

static void req_free(struct net_req *r)
{
	ck_done(r, false);
	close_fd(&r->out);
	close_fd(&r->err);
	free(r->buf);
	free(r->post);
	free(r);
}

/* ---------- which addresses Orchard may fetch from ----------
 * Three classes. BLOCKED, never fetched: the phone itself (loopback, 0.0.0.0/8, its own addresses), the USB link to
 * the PC (172.16.43.0/24: PC-side services meant only for the phone), link-local, multicast / reserved, MagicDNS's
 * 100.100.100.100 / fd7a:115c:a1e0::53 — and any address on Orchard's relay ports (8650 = Music's optional relay; 8656,
 * 8657 kept reserved for PC-side relays). PRIVATE: the LAN (10/8, 172.16/12, 192.168/16, fc00::/7, 198.18/15) and the tailnet (100.64.0.0/10,
 * fd7a:115c:a1e0::/48): services there trust the phone by its source address, so only what the user opens themselves
 * (net_opts_t.allow_private) may go there, and a redirect only from a hop that was already private — a public page
 * never makes the phone reach them (links, pictures, form posts, redirects, refreshes, DNS that points there).
 * Everything else is PUBLIC. Checked on every hop before curl connects, curl is pinned to the checked addresses
 * (--resolve), and the address curl really used is checked again. */

enum { AC_PUBLIC, AC_PRIVATE, AC_BLOCKED };

/* previews only: ORCHARD_WEB_ALLOW_LOOPBACK=1 makes 127/8 and ::1 PRIVATE (test pages from a local http.server);
 * the phone build never reads it */
static bool loopback_ok(void)
{
#ifdef ORCHARD_PHONE
	return false;
#else
	static int v = -1;
	if (v < 0) {
		const char *e = getenv("ORCHARD_WEB_ALLOW_LOOPBACK");
		v = e && *e == '1';
	}
	return v;
#endif
}

static int v4_class(uint32_t a)	/* host order */
{
	if ((a >> 24) == 127)
		return loopback_ok() ? AC_PRIVATE : AC_BLOCKED;
	if ((a >> 24) == 0 ||					/* 0.0.0.0/8 (= this host) */
	    (a & 0xffffff00u) == 0xac102b00u ||			/* 172.16.43.0/24 usb0 (phone .1, PC .2) */
	    (a & 0xffff0000u) == 0xa9fe0000u ||			/* 169.254.0.0/16 link-local */
	    a == 0x64646464u ||					/* 100.100.100.100 Tailscale's MagicDNS / quad100 */
	    (a >> 28) >= 14)					/* multicast, reserved, broadcast */
		return AC_BLOCKED;
	if ((a & 0xffc00000u) == 0x64400000u ||			/* 100.64.0.0/10 tailnet / CGNAT */
	    (a >> 24) == 10 ||					/* 10.0.0.0/8 */
	    (a & 0xfff00000u) == 0xac100000u ||			/* 172.16.0.0/12 */
	    (a & 0xffff0000u) == 0xc0a80000u ||			/* 192.168.0.0/16 */
	    (a & 0xfffe0000u) == 0xc6120000u ||			/* 198.18.0.0/15 */
	    (a & 0xffffff00u) == 0xc0000000u)			/* 192.0.0.0/24 */
		return AC_PRIVATE;
	return AC_PUBLIC;
}

static int v6_class(const unsigned char *b)
{
	static const unsigned char zero[12], mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
	static const unsigned char nat64[12] = { 0, 0x64, 0xff, 0x9b };
	static const unsigned char ts[6] = { 0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0 };
	static const unsigned char tsdns[16] = { 0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x53 };
	static const unsigned char one[16] = { [15] = 1 };
	if (!memcmp(b, one, 16))
		return loopback_ok() ? AC_PRIVATE : AC_BLOCKED;
	if (!memcmp(b, zero, 12) || !memcmp(b, mapped, 12) || !memcmp(b, nat64, 12))	/* ::, v4-compatible / mapped, NAT64 */
		return v4_class((uint32_t)b[12] << 24 | b[13] << 16 | b[14] << 8 | b[15]);
	if (b[0] == 0x20 && b[1] == 0x02)			/* 2002::/16 6to4: the embedded IPv4 */
		return v4_class((uint32_t)b[2] << 24 | b[3] << 16 | b[4] << 8 | b[5]);
	if (!memcmp(b, tsdns, 16) ||
	    (b[0] == 0xfe && (b[1] & 0xc0) == 0x80) ||		/* fe80::/10 link-local */
	    b[0] == 0xff)					/* multicast */
		return AC_BLOCKED;
	if (!memcmp(b, ts, 6) || (b[0] & 0xfe) == 0xfc)		/* Tailscale ULA, fc00::/7 */
		return AC_PRIVATE;
	return AC_PUBLIC;
}

/* the phone's own addresses + their on-link prefixes (wlan0's LAN, its tailnet IP, its IPv6 ...), cached for
 * IF_TTL_MS: a LAN whose hosts have global IPv6 (or public IPv4) addresses is still the LAN */
#define IF_TTL_MS 2000
#define IF_MAX 64
struct if_ent {
	int fam, plen;
	bool lo;
	unsigned char a[16], m[16];
};
static pthread_mutex_t if_mu = PTHREAD_MUTEX_INITIALIZER;
static struct if_ent if_tab[IF_MAX];
static int if_n = -1;
static long long if_at;

static int ent_bytes(const struct sockaddr *sa, unsigned char *out)
{
	if (sa->sa_family == AF_INET) {
		memcpy(out, &((const struct sockaddr_in *)sa)->sin_addr, 4);
		return 4;
	}
	if (sa->sa_family == AF_INET6) {
		memcpy(out, &((const struct sockaddr_in6 *)sa)->sin6_addr, 16);
		return 16;
	}
	return 0;
}

static bool if_refresh(void)	/* if_mu held */
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	long long now = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
	if (if_n >= 0 && now - if_at < IF_TTL_MS)
		return true;
	struct ifaddrs *ifs, *i;
	if (getifaddrs(&ifs))
		return false;
	int n = 0;
	for (i = ifs; i && n < IF_MAX; i = i->ifa_next) {
		if (!i->ifa_addr || (i->ifa_addr->sa_family != AF_INET && i->ifa_addr->sa_family != AF_INET6))
			continue;
		struct if_ent *e = &if_tab[n];
		memset(e, 0, sizeof(*e));
		e->fam = i->ifa_addr->sa_family;
		e->lo = i->ifa_flags & IFF_LOOPBACK;
		int len = ent_bytes(i->ifa_addr, e->a);
		if (i->ifa_netmask && i->ifa_netmask->sa_family == e->fam && ent_bytes(i->ifa_netmask, e->m) == len)
			for (int b = 0; b < len; b++)
				e->plen += __builtin_popcount(e->m[b]);
		n++;
	}
	freeifaddrs(ifs);
	if_n = n;
	if_at = now;
	return true;
}

/* own address → AC_BLOCKED; inside an on-link prefix of a non-loopback interface → AC_PRIVATE; else c */
static int if_class(const struct sockaddr *sa, int c)
{
	unsigned char b[16];
	int len = ent_bytes(sa, b);
	pthread_mutex_lock(&if_mu);
	if (!if_refresh()) {
		pthread_mutex_unlock(&if_mu);
		return AC_BLOCKED;
	}
	for (int k = 0; k < if_n && c != AC_BLOCKED; k++) {
		const struct if_ent *e = &if_tab[k];
		if (e->fam != sa->sa_family)
			continue;
		if (!memcmp(e->a, b, len)) {
			if (!(e->lo && loopback_ok()))
				c = AC_BLOCKED;
			continue;
		}
		/* skip absurdly wide prefixes (a /0 tun would make the whole internet "private") and host routes */
		if (e->lo || e->plen < (len == 4 ? 8 : 16) || e->plen >= len * 8)
			continue;
		bool in = true;
		for (int i = 0; i < len && in; i++)
			in = !((e->a[i] ^ b[i]) & e->m[i]);
		if (in && c == AC_PUBLIC)
			c = AC_PRIVATE;
	}
	pthread_mutex_unlock(&if_mu);
	return c;
}

static int addr_class(const struct sockaddr *sa)
{
	int c;
	if (sa->sa_family == AF_INET)
		c = v4_class(ntohl(((const struct sockaddr_in *)sa)->sin_addr.s_addr));
	else if (sa->sa_family == AF_INET6)
		c = v6_class(((const struct sockaddr_in6 *)sa)->sin6_addr.s6_addr);
	else
		return AC_BLOCKED;
	if (c == AC_BLOCKED)
		return c;
	return if_class(sa, c);
}

bool net_addr_forbidden(const struct sockaddr *sa) { return addr_class(sa) != AC_PUBLIC; }

/* numeric address text (as curl prints %{remote_ip}, or an IP-literal host) → its class */
static int ip_text_class(const char *ip)
{
	struct sockaddr_in a4 = { .sin_family = AF_INET };
	struct sockaddr_in6 a6 = { .sin6_family = AF_INET6 };
	if (inet_pton(AF_INET, ip, &a4.sin_addr) == 1)
		return addr_class((struct sockaddr *)&a4);
	if (inet_pton(AF_INET6, ip, &a6.sin6_addr) == 1)
		return addr_class((struct sockaddr *)&a6);
	return AC_BLOCKED;
}

static bool port_blocked(int port) { return port == 8650 || port == 8656 || port == 8657; }

/* this hop's address class → 0 or the error that stops it (r->hop_priv set) */
static int class_check(struct net_req *r, int c)
{
	if (c == AC_BLOCKED || port_blocked(r->port))
		return NET_EBLOCKED;
	if (c == AC_PRIVATE) {
		if (!r->allow_priv)
			return NET_EPRIVATE;
		r->hop_priv = true;
	}
	return 0;
}

/* the authority of an http(s) URL, strictly: [user[:pw]@]host[:port] with host = LDH name / dotted IPv4 /
 * [IPv6]; anything curl might read differently from us (backslashes, several @, %-escapes, odd chars) fails.
 * A trailing dot is dropped from the host in place (curl's name cache would otherwise miss our --resolve pin). */
static bool url_authority(char *url, char *host, int hn, int *port)
{
	bool tls;
	char *p;
	if (!strncasecmp(url, "http://", 7))
		tls = false, p = url + 7;
	else if (!strncasecmp(url, "https://", 8))
		tls = true, p = url + 8;
	else
		return false;
	int alen = strcspn(p, "/?#");
	if (memchr(p, '\\', alen))
		return false;
	char *at = NULL;
	for (int i = 0; i < alen; i++)
		if (p[i] == '@') {
			if (at)
				return false;
			at = p + i;
		}
	char *h = at ? at + 1 : p, *end = p + alen, *he;
	*port = tls ? 443 : 80;
	if (*h == '[') {
		he = memchr(h, ']', end - h);
		if (!he)
			return false;
		for (char *c = h + 1; c < he; c++)
			if (!isxdigit((unsigned char)*c) && *c != ':' && *c != '.')
				return false;	/* no zone ids */
		he++;
	} else {
		he = h;
		while (he < end && (isalnum((unsigned char)*he) || *he == '-' || *he == '.' || *he == '_'))
			he++;
	}
	if (he == h || (*h != '[' && he - h == 1 && *h == '.'))
		return false;
	if (he < end) {
		if (*he != ':')
			return false;
		char *c = he + 1;
		long v = 0;
		if (c == end)
			c = NULL;	/* "host:" = default port */
		for (; c && c < end; c++) {
			if (!isdigit((unsigned char)*c) || (v = v * 10 + (*c - '0')) > 65535)
				return false;
		}
		if (c && (v < 1))
			return false;
		if (c)
			*port = v;
	}
	if (*h != '[' && he[-1] == '.') {	/* "example.com." → "example.com" */
		memmove(he - 1, he, strlen(he) + 1);
		he--;
	}
	if (he - h >= hn)
		return false;
	for (int i = 0; i < he - h; i++)
		host[i] = tolower((unsigned char)h[i]);
	host[he - h] = 0;
	return true;
}

/* ---------- name lookups: one short-lived thread each (a slow DNS server must not hold up the decode worker) */

struct dns_job {
	char host[256];
	int n, rc;
	struct sockaddr_storage a[MAX_ADDRS];
	bool done, abandoned;
};
static pthread_mutex_t dmu = PTHREAD_MUTEX_INITIALIZER;

static void *dns_thread(void *arg)
{
	struct dns_job *j = arg;
	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL;
	int n = 0;
	int rc = getaddrinfo(j->host, NULL, &hints, &res);
	for (struct addrinfo *i = res; !rc && i && n < MAX_ADDRS; i = i->ai_next)
		if ((i->ai_family == AF_INET || i->ai_family == AF_INET6) && i->ai_addrlen <= sizeof(j->a[0]))
			memcpy(&j->a[n++], i->ai_addr, i->ai_addrlen);
	if (res)
		freeaddrinfo(res);
	pthread_mutex_lock(&dmu);
	j->rc = rc;
	j->n = n;
	j->done = true;
	bool drop = j->abandoned;
	pthread_mutex_unlock(&dmu);
	if (drop)
		free(j);
	return NULL;
}

static struct dns_job *dns_start(const char *host)
{
	struct dns_job *j = calloc(1, sizeof(*j));
	if (!j)
		return NULL;
	snprintf(j->host, sizeof(j->host), "%s", host);
	pthread_t t;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setstacksize(&at, 256 << 10);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	bool ok = pthread_create(&t, &at, dns_thread, j) == 0;
	pthread_attr_destroy(&at);
	if (!ok) {
		free(j);
		return NULL;
	}
	return j;
}

static void dns_drop(struct dns_job **jp)
{
	struct dns_job *j = *jp;
	*jp = NULL;
	if (!j)
		return;
	pthread_mutex_lock(&dmu);
	bool done = j->done;
	j->abandoned = true;
	pthread_mutex_unlock(&dmu);
	if (done)
		free(j);
}

/* ---------- one hop = one curl child ---------- */

/* ---------- cookies ---------- */

static char cookie_dir[300];

void net_set_cookie_dir(const char *dir)
{
	snprintf(cookie_dir, sizeof(cookie_dir), "%s", dir ? dir : "");
	if (*cookie_dir)
		mkdir(cookie_dir, 0700);
}

static unsigned ck_epoch;	/* bumped by net_clear_cookies: a hop running across it merges nothing back */

void net_clear_cookies(void)
{
	ck_epoch++;
	if (!*cookie_dir)
		return;
	DIR *d = opendir(cookie_dir);
	if (!d)
		return;
	struct dirent *e;
	char p[600];
	while ((e = readdir(d)))
		if (e->d_name[0] != '.') {
			snprintf(p, sizeof(p), "%s/%s", cookie_dir, e->d_name);
			unlink(p);
		}
	closedir(d);
}

/* the registrable part of a host, roughly (no public-suffix list): the last two labels, three when the
 * second-level one is a short generic one under a country code ("bbc.co.uk", "abc.net.au"); IP literals and
 * single labels (MagicDNS "mypc") stay whole */
const char *net_site(const char *host, char *out, int n)
{
	snprintf(out, n, "%s", host);
	int l = strlen(out);
	if (l && out[l - 1] == '.')
		out[--l] = 0;
	if (!l || out[0] == '[' || strspn(out, "0123456789.") == (size_t)l)
		return out;
	int dots[3] = { -1, -1, -1 }, nd = 0;
	for (int i = l - 1; i >= 0 && nd < 3; i--)
		if (out[i] == '.')
			dots[nd++] = i;
	if (nd < 2)
		return out;
	int tld = l - dots[0] - 1, sld = dots[0] - dots[1] - 1;
	static const char *const gen[] = { "co", "com", "net", "org", "ac", "gov", "edu", "or", "ne", "go", NULL };
	bool three = false;
	if (tld == 2 && nd >= 2)
		for (const char *const *g = gen; *g; g++)
			if ((int)strlen(*g) == sld && !strncmp(out + dots[1] + 1, *g, sld))
				three = true;
	int cut = three ? dots[2] : dots[1];
	if (three && nd < 3)
		return out;
	memmove(out, out + cut + 1, l - cut);
	return out;
}

static bool jar_path(const char *host, char *out, int n)
{
	if (!*cookie_dir)
		return false;
	char site[256];
	net_site(host, site, sizeof(site));
	int k = snprintf(out, n, "%s/", cookie_dir);
	for (const char *c = site; *c && k < n - 1; c++)	/* [v6]:… and anything odd → '_' */
		out[k++] = isalnum((unsigned char)*c) || *c == '.' || *c == '-' ? *c : '_';
	out[k] = 0;
	return k < n - 1 && site[0] && site[0] != '.';
}

/* ---------- cookie jars ----------
 * A page hop never lets curl rewrite the site's jar itself (two hops of one site running at once: the last
 * curl to exit would drop what the other one got). curl reads a snapshot (-b) and writes its own file (-c);
 * when it exits — on its own, also after Web closed its pipe at MAX_PAGE or at curl's own --max-time — the
 * changes are merged into the jar on the LVGL thread: cookies it wrote replace theirs, cookies of the snapshot
 * it didn't write (deleted / expired) go, everything else in the jar (from other hops meanwhile) stays. A hop
 * that is killed merges nothing. Cross-site POST (SameSite=Lax, browsers' default): no cookies sent (empty
 * snapshot), what it sets is still kept. */

#define CK_MAX 400
struct ck_line { char key[512]; char *line; };

/* the jar's cookie lines ("#HttpOnly_" ones included); key = domain (without the prefix) \t path \t name.
 * -1 = more than max, or a line too long to be sure of */
static int ck_read(const char *path, struct ck_line *v, int max)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return 0;
	int n = 0;
	char buf[16384];
	while (fgets(buf, sizeof(buf), f)) {
		size_t l = strcspn(buf, "\r\n");
		if (!buf[l] && !feof(f)) {
			n = -1 - n;	/* (freed below as -1 - n entries) */
			break;
		}
		if (n >= max) {
			n = -1 - n;
			break;
		}
		buf[l] = 0;
		const char *d = buf;
		if (!strncmp(d, "#HttpOnly_", 10))
			d += 10;
		else if (*d == '#' || !*d)
			continue;
		const char *fl[7] = { d };
		int nf = 1;
		for (const char *c = d; *c && nf < 7; c++)
			if (*c == '\t')
				fl[nf++] = c + 1;
		if (nf < 7)
			continue;
		snprintf(v[n].key, sizeof(v[n].key), "%.*s\t%.*s\t%.*s", (int)strcspn(fl[0], "\t"), fl[0],
			 (int)strcspn(fl[2], "\t"), fl[2], (int)strcspn(fl[5], "\t"), fl[5]);
		if (!(v[n].line = strdup(buf))) {
			n = -1 - n;
			break;
		}
		n++;
	}
	fclose(f);
	return n;
}

static void ck_free(struct ck_line *v, int n)
{
	if (n < 0)
		n = -1 - n;
	for (int i = 0; i < n; i++)
		free(v[i].line);
	free(v);
}

static bool ck_has(const struct ck_line *v, int n, const char *key)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(v[i].key, key))
			return true;
	return false;
}

static void ck_merge(const char *jar, const char *snap, const char *out)
{
	struct ck_line *cur = calloc(CK_MAX, sizeof(*cur)), *old = calloc(CK_MAX, sizeof(*old)),
		       *neu = calloc(CK_MAX, sizeof(*neu));
	int nc = -1, no = -1, nn = -1;
	if (cur && old && neu) {
		nc = ck_read(jar, cur, CK_MAX);
		no = ck_read(snap, old, CK_MAX);
		nn = ck_read(out, neu, CK_MAX);
	}
	if (nc < 0 || no < 0 || nn < 0) {	/* can't merge: curl's file as it is (what -c jar did before) */
		rename(out, jar);
	} else {
		char tmp[440];
		snprintf(tmp, sizeof(tmp), "%s~m", jar);
		int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
		if (f) {
			fputs("# Netscape HTTP Cookie File\n", f);
			for (int i = 0; i < nc; i++)
				if (!ck_has(neu, nn, cur[i].key) && !ck_has(old, no, cur[i].key))
					fprintf(f, "%s\n", cur[i].line);	/* not this hop's business: kept */
			for (int i = 0; i < nn; i++)
				fprintf(f, "%s\n", neu[i].line);
			if (fclose(f) || rename(tmp, jar))
				unlink(tmp);
		} else if (fd >= 0) {
			close(fd);
			unlink(tmp);
		}
	}
	if (cur)
		ck_free(cur, nc);
	if (old)
		ck_free(old, no);
	if (neu)
		ck_free(neu, nn);
}

static bool ck_copy(const char *from, const char *to)	/* a missing source = an empty copy */
{
	int o = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (o < 0)
		return false;
	int i = from ? open(from, O_RDONLY | O_CLOEXEC) : -1;
	char buf[16384];
	ssize_t n;
	bool ok = true;
	while (i >= 0 && (n = read(i, buf, sizeof(buf))) > 0)
		ok = ok && write(o, buf, n) == n;
	if (i >= 0)
		close(i);
	return !close(o) && ok;
}

/* the hop's curl is gone: exited = on its own (its -c file is complete) */
static void ck_done(struct net_req *r, bool exited)
{
	if (!r->ck_merge)
		return;
	r->ck_merge = false;
	if (exited && r->ck_epoch == ck_epoch && !access(r->ck_out, F_OK))
		ck_merge(r->ck_jar, r->ck_snap, r->ck_out);
	unlink(r->ck_snap);
	unlink(r->ck_out);
}

/* scheme://host[:port] of an http(s) URL (lower case), "" when it isn't one */
static void origin_of(const char *url, char *out, int n)
{
	out[0] = 0;
	const char *p = strstr(url, "://");
	if (!p || (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)))
		return;
	int len = (p - url) + 3 + strcspn(p + 3, "/?#");
	if (len >= n)
		return;
	for (int i = 0; i < len; i++)
		out[i] = tolower((unsigned char)url[i]);
	out[len] = 0;
	char *at = strchr(out + (p - url) + 3, '@');	/* never pass credentials on */
	if (at)
		memmove(out + (p - url) + 3, at + 1, strlen(at));
}

/* ---------- one hop = one curl child ---------- */

static bool spawn(struct net_req *r, const char *pin)
{
	long long left = (r->deadline - 3000 - now_ms()) / 1000;
	char tmo[16], ua[] = UA, uad[] = UA_DESKTOP;
	snprintf(tmo, sizeof(tmo), "%lld", left < 1 ? 1 : left);
	const char *ca = ca_path();
	const char *argv[64];
	char jar[400], refh[2100], origh[300];
	int a = 0;
	argv[a++] = "curl";
	argv[a++] = "-q";	/* first: never read a .curlrc */
	argv[a++] = "-sS";
	argv[a++] = "-g";	/* no {} [] globbing in URLs from pages */
	argv[a++] = "--compressed";
	argv[a++] = "--proto";
	argv[a++] = "=http,https";
	argv[a++] = "--max-redirs";	/* redirects come back here (see finish) */
	argv[a++] = "0";
	argv[a++] = "--noproxy";	/* connect where the checked address says, never via a proxy from the env */
	argv[a++] = "*";
	argv[a++] = "--max-time";
	argv[a++] = tmo;
	argv[a++] = "--connect-timeout";
	argv[a++] = "15";
	argv[a++] = "-A";
	argv[a++] = r->desktop ? uad : ua;
	argv[a++] = "-H";
	argv[a++] = "Accept-Language: en-US,en;q=0.9";
	argv[a++] = "-H";
	argv[a++] = "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8";
	if (ca) {
		argv[a++] = "--cacert";
		argv[a++] = ca;
	}
	if (pin && *pin) {
		argv[a++] = "--resolve";
		argv[a++] = pin;
	}
	if (r->cookies != NET_CK_NONE && jar_path(r->host, jar, sizeof(jar))) {
		char site[256];
		bool xpost = r->post && r->post_site[0] && strcmp(net_site(r->host, site, sizeof(site)), r->post_site);
		if (r->cookies == NET_CK_RW) {	/* see "cookie jars" */
			static unsigned ck_n;
			ck_n++;
			snprintf(r->ck_jar, sizeof(r->ck_jar), "%s", jar);
			snprintf(r->ck_snap, sizeof(r->ck_snap), "%s~r%u", jar, ck_n);
			snprintf(r->ck_out, sizeof(r->ck_out), "%s~w%u", jar, ck_n);
			unlink(r->ck_out);
			if (ck_copy(xpost ? NULL : jar, r->ck_snap)) {
				r->ck_merge = true;
				r->ck_epoch = ck_epoch;
				argv[a++] = "-b";
				argv[a++] = r->ck_snap;
				argv[a++] = "-c";
				argv[a++] = r->ck_out;
			}
		} else if (!xpost) {
			argv[a++] = "-b";	/* a file name (no '='): its cookies are sent, a missing file = none yet */
			argv[a++] = jar;
		}
	}
	if (r->referer[0]) {	/* strict-origin-when-cross-origin, as browsers do by default */
		char ro[300], to[300];
		origin_of(r->referer, ro, sizeof(ro));
		origin_of(r->url, to, sizeof(to));
		bool downgrade = !strncmp(ro, "https:", 6) && strncmp(to, "https:", 6);
		if (ro[0] && !downgrade) {
			if (!strcmp(ro, to))
				snprintf(refh, sizeof(refh), "Referer: %.*s", (int)strcspn(r->referer, "#"), r->referer);
			else
				snprintf(refh, sizeof(refh), "Referer: %s/", ro);
			argv[a++] = "-H";
			argv[a++] = refh;
		}
		if (r->post && ro[0]) {
			snprintf(origh, sizeof(origh), "Origin: %s", ro);
			argv[a++] = "-H";
			argv[a++] = origh;
		}
	}
	if (r->post) {
		argv[a++] = "-H";
		argv[a++] = "Content-Type: application/x-www-form-urlencoded";
		argv[a++] = "--data-raw";	/* not --data / -d: a leading '@' would read a file */
		argv[a++] = r->post;
	}
	argv[a++] = "-w";
	argv[a++] = "%{stderr}\n" MARK "%{http_code}\t%{remote_ip}\t%{url_effective}\t%{redirect_url}\t%{content_type}\n";
	argv[a++] = "-o";
	argv[a++] = "-";
	argv[a++] = "--url";
	argv[a++] = r->url;
	argv[a] = NULL;

	const char *curl = curl_path();
	int po[2], pe[2];
	if (!curl || pipe2(po, O_CLOEXEC))
		return false;
	if (pipe2(pe, O_CLOEXEC)) {
		close(po[0]);
		close(po[1]);
		return false;
	}
	pid_t pid = fork();
	if (pid == 0) {	/* child: only async-signal-safe calls until exec */
		int nul = open("/dev/null", O_RDONLY);
		if (nul >= 0)
			dup2(nul, 0);
		dup2(po[1], 1);
		dup2(pe[1], 2);
		for (int fd = 3; fd < 256; fd++)
			close(fd);
		umask(077);	/* cookie jars */
		execv(curl, (char *const *)argv);
		_exit(127);
	}
	close(po[1]);
	close(pe[1]);
	if (pid < 0) {
		close(po[0]);
		close(pe[0]);
		return false;
	}
	r->pid = pid;
	r->out = po[0];
	r->err = pe[0];
	fcntl(r->out, F_SETFL, O_NONBLOCK);
	fcntl(r->err, F_SETFL, O_NONBLOCK);
	return true;
}

/* the looked-up addresses → curl's --resolve pin, or the error when any of them is off limits */
static int pin_from(struct net_req *r, const struct sockaddr_storage *a, int n, char *pin, int pn)
{
	if (n <= 0)
		return NET_EDNS;
	int k = snprintf(pin, pn, "%s:%d:", r->host, r->port);
	for (int i = 0; i < n; i++) {
		const struct sockaddr *sa = (const struct sockaddr *)&a[i];
		int e = class_check(r, addr_class(sa));	/* one address off limits = the whole name is */
		if (e)
			return e;
		char ip[INET6_ADDRSTRLEN];
		if (sa->sa_family == AF_INET)
			inet_ntop(AF_INET, &((const struct sockaddr_in *)sa)->sin_addr, ip, sizeof(ip));
		else
			inet_ntop(AF_INET6, &((const struct sockaddr_in6 *)sa)->sin6_addr, ip, sizeof(ip));
		if (k < pn)
			k += snprintf(pin + k, pn - k, sa->sa_family == AF_INET6 ? "%s[%s]" : "%s%s", i ? "," : "", ip);
	}
	return k < pn ? NET_OK : NET_EOTHER;
}

/* start r->url: IP literal → check + curl now; name → look it up first (tick() continues) */
static void hop_start(struct net_req *r)
{
	r->pid = 0;
	r->fail = 0;
	r->hop_priv = false;
	for (const char *p = r->url; *p; p++)
		if ((unsigned char)*p <= ' ' || (unsigned char)*p == 0x7f) {
			r->fail = NET_EOTHER;
			return;
		}
	if (!url_authority(r->url, r->host, sizeof(r->host), &r->port)) {
		r->fail = NET_EOTHER;
		return;
	}
	char lit[256];
	snprintf(lit, sizeof(lit), "%s", r->host);
	if (lit[0] == '[') {
		lit[strlen(lit) - 1] = 0;
		if ((r->fail = class_check(r, ip_text_class(lit + 1))))
			;
		else if (!spawn(r, NULL))
			r->fail = NET_ENOCURL;
		return;
	}
	/* every host name goes through getaddrinfo, which also reads the odd IPv4 spellings (127.1, 0x7f.1,
	 * 2130706433) the way curl does, so those land on the same check */
	r->dns = dns_start(r->host);
	if (!r->dns)
		r->fail = NET_EOTHER;
}

/* called by tick(): name lookup finished? → pin + curl */
static void hop_resolved(struct net_req *r)
{
	pthread_mutex_lock(&dmu);
	bool done = r->dns->done;
	pthread_mutex_unlock(&dmu);
	if (!done)
		return;
	struct dns_job *j = r->dns;
	char pin[600];
	int e = j->rc ? NET_EDNS : pin_from(r, j->a, j->n, pin, sizeof(pin));
	r->dns = NULL;
	free(j);
	if (e)
		r->fail = e;
	else if (!spawn(r, pin))
		r->fail = NET_ENOCURL;
}

net_req_t *net_fetch(const net_opts_t *o, net_cb_t cb, void *ud)
{
	const char *url = o->url;
	if (!curl_path() || !url || strncasecmp(url, "http", 4) || strlen(url) >= sizeof(((struct net_req *)0)->url))
		return NULL;
	for (const char *p = url; *p; p++)
		if ((unsigned char)*p <= ' ')
			return NULL;	/* callers encode spaces */
	if (o->post && strlen(o->post) > MAX_POST)
		return NULL;
	struct net_req *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->out = r->err = -1;
	r->max = o->max_bytes ? o->max_bytes : 2 << 20;
	r->cb = cb;
	r->ud = ud;
	r->deadline = now_ms() + (o->timeout_s > 0 ? o->timeout_s : 30) * 1000LL + 3000;
	snprintf(r->url, sizeof(r->url), "%s", url);
	r->desktop = o->desktop;
	r->cookies = o->cookies;
	r->allow_priv = o->allow_private;
	if (o->post) {
		r->post = strdup(o->post);
		r->posted = true;
		if (!r->post) {
			free(r);
			return NULL;
		}
	}
	if (o->referer) {	/* a header value: nothing that could end the line */
		bool ok = strlen(o->referer) < sizeof(r->referer);
		for (const char *p = o->referer; ok && *p; p++)
			ok = (unsigned char)*p > ' ' && *p != 0x7f;
		if (ok)
			snprintf(r->referer, sizeof(r->referer), "%s", o->referer);
	}
	if (o->post && o->referer) {	/* the site the form was on (stays for 307 / 308 hops) */
		char h[256];
		net_site(net_host(o->referer, h, sizeof(h)), r->post_site, sizeof(r->post_site));
		if (!r->post_site[0])
			snprintf(r->post_site, sizeof(r->post_site), "?");	/* unknown: cross-site to everything */
	}
	if (wifi_sleep_wait()) {	/* Wi-Fi Sleep: wake it, start once it is back (tick) — else the lookup fails at once */
		r->wifi_since = now_ms() | 1;
		power_net_for("fetch", 5000);	/* (then the request itself keeps it awake: net_active) */
	} else {
		hop_start(r);	/* a refused address still answers through the callback (tick), with NET_EBLOCKED */
	}
	r->next = reqs;
	reqs = r;
	timer_need();
	return r;
}

net_req_t *net_get(const char *url, size_t max_bytes, int timeout_s, net_cb_t cb, void *ud)
{
	net_opts_t o = { .url = url, .max_bytes = max_bytes, .timeout_s = timeout_s };
	return net_fetch(&o, cb, ud);
}

void net_cancel(net_req_t *r)
{
	if (!r)
		return;
	unlink_req(r);
	if (r->pid > 0) {
		kill(r->pid, SIGKILL);
		zombie_add(r->pid);
	}
	dns_drop(&r->dns);
	req_free(r);
}

char *net_take(net_res_t *res)
{
	char *b = res->body;
	res->body = NULL;
	return b;
}

static void drain_out(struct net_req *r)
{
	size_t got = 0;
	while (r->out >= 0 && got < READ_PER_TICK) {
		if (r->len + 65536 + 1 > r->cap) {
			size_t nc = r->cap ? r->cap * 2 : 131072;
			if (nc > r->max + 65536 + 1)
				nc = r->max + 65536 + 1;
			if (nc < r->len + 65536 + 1)
				nc = r->len + 65536 + 1;
			char *nb = realloc(r->buf, nc);
			if (!nb) {
				r->trunc = true;
				close_fd(&r->out);
				kill(r->pid, SIGKILL);
				return;
			}
			r->buf = nb;
			r->cap = nc;
		}
		ssize_t n = read(r->out, r->buf + r->len, 65536);
		if (n > 0) {
			r->len += n;
			got += n;
			if (r->len >= r->max) {	/* enough: curl stops at its next write (error 23), its cookies still saved */
				r->len = r->max;
				r->trunc = true;
				r->trunc_at = now_ms();
				close_fd(&r->out);
			}
		} else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
			close_fd(&r->out);
		} else {
			return;
		}
	}
}

static void drain_err(struct net_req *r)
{
	char tmp[1024];
	while (r->err >= 0) {
		ssize_t n = read(r->err, tmp, sizeof(tmp));
		if (n > 0) {
			if (r->elen + n > ERR_KEEP) {	/* keep the tail: the write-out line comes last */
				int drop = r->elen + n - ERR_KEEP;
				if (drop > r->elen)
					drop = r->elen;
				memmove(r->ebuf, r->ebuf + drop, r->elen - drop);
				r->elen -= drop;
				if (n > ERR_KEEP) {
					memcpy(r->ebuf, tmp + n - ERR_KEEP, ERR_KEEP);
					r->elen = ERR_KEEP;
					continue;
				}
			}
			memcpy(r->ebuf + r->elen, tmp, n);
			r->elen += n;
		} else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
			close_fd(&r->err);
		} else {
			return;
		}
	}
}

/* the -w line curl wrote last on stderr: status, remote ip, effective URL, redirect target, content type */
struct mark { int status; char ip[64], url[2048], redir[2048], ctype[128]; bool ok; };

static void parse_mark(struct net_req *r, struct mark *mk)
{
	memset(mk, 0, sizeof(*mk));
	r->ebuf[r->elen] = 0;
	char *m = strstr(r->ebuf, MARK);
	if (!m)
		return;
	char *line = m + strlen(MARK), *e = strchr(line, '\n'), *f[5] = { line };
	if (e)
		*e = 0;
	int nf = 1;
	for (char *p = line; *p && nf < 5; p++)
		if (*p == '\t') {
			*p = 0;
			f[nf++] = p + 1;
		}
	*m = 0;
	if (nf < 5)
		return;
	mk->ok = true;
	mk->status = atoi(f[0]);
	snprintf(mk->ip, sizeof(mk->ip), "%s", f[1]);
	snprintf(mk->url, sizeof(mk->url), "%s", f[2]);
	snprintf(mk->redir, sizeof(mk->redir), "%s", f[3]);
	snprintf(mk->ctype, sizeof(mk->ctype), "%s", f[4]);
	for (char *p = mk->ctype; *p; p++)
		*p = tolower((unsigned char)*p);
}

static void finish(struct net_req *r, int code_exit, int fail, const struct mark *mk)
{
	static net_res_t res;
	memset(&res, 0, sizeof(res));
	if (mk && mk->ok) {
		res.status = mk->status;
		snprintf(res.url, sizeof(res.url), "%s", mk->url);
		snprintf(res.ctype, sizeof(res.ctype), "%s", mk->ctype);
	}
	res.priv = r->hop_priv;
	res.posted = r->posted;
	if (fail == NET_EPRIVATE || fail == NET_EBLOCKED)	/* the address that was refused (a redirect's target) */
		snprintf(res.url, sizeof(res.url), "%s", r->url);
	if (fail) {	/* nothing of this hop reaches the app */
		free(r->buf);
		r->buf = NULL;
		r->len = 0;
		r->trunc = false;
		res.status = 0;
	}
	res.body = r->buf ? r->buf : malloc(1);
	r->buf = NULL;
	res.len = res.body ? r->len : 0;
	if (res.body)
		res.body[res.len] = 0;
	res.truncated = r->trunc;
	if (fail) {
		res.err = fail;
	} else if (r->timed_out) {
		res.err = NET_ETIMEOUT;
	} else if (!r->trunc && code_exit != 0) {
		switch (code_exit) {
		case 127: res.err = NET_ENOCURL; break;
		case 5: case 6: res.err = NET_EDNS; break;
		case 7: res.err = NET_ECONNECT; break;
		case 28: res.err = NET_ETIMEOUT; break;
		case 35: case 51: case 53: case 54: case 58: case 59: case 60: case 64: case 66: case 77: case 80:
		case 82: case 83: case 90: case 91: res.err = NET_ETLS; break;
		case 22: res.err = NET_EHTTP; break;
		default: res.err = NET_EOTHER; break;
		}
	}
	if (res.status == 0 && !res.err && !r->trunc)
		res.err = NET_EOTHER;
	static const char *const msgs[] = {
		[NET_OK] = "",
		[NET_ENOCURL] = "The internet tools (curl) aren\xe2\x80\x99t installed.",
		[NET_EDNS] = "The server can\xe2\x80\x99t be found. Check the address and your Wi-Fi connection.",
		[NET_ECONNECT] = "The server isn\xe2\x80\x99t responding, or you aren\xe2\x80\x99t connected to the internet.",
		[NET_ETLS] = "A secure connection to the server can\xe2\x80\x99t be made.",
		[NET_ETIMEOUT] = "The request timed out.",
		[NET_EHTTP] = "The server returned an error.",
		[NET_EOTHER] = "The page couldn\xe2\x80\x99t be loaded.",
		[NET_EBLOCKED] = "This address is on the phone itself, its USB link or one of Orchard\xe2\x80\x99s own services, which Web never opens.",
		[NET_EREDIRECTS] = "The page redirected too many times.",
		[NET_EPRIVATE] = "This address is on your local network or tailnet. A page from the internet can\xe2\x80\x99t open it.",
	};
	res.msg = msgs[res.err];
	if (getenv("ORCHARD_NET_DEBUG"))
		fprintf(stderr, "net: exit %d fail %d status %d len %zu%s %s [%s] ip %s hops %d %s\n", code_exit, fail,
			res.status, res.len, r->trunc ? " (truncated)" : "", res.url, res.ctype, mk ? mk->ip : "-",
			r->hops, r->ebuf);
	if (r->cb)
		r->cb(&res, r->ud);
	free(res.body);
	res.body = NULL;
}

/* curl exited cleanly with a 3xx + Location: start the next hop on the same request. false = finish now. */
static bool redirect(struct net_req *r, int code_exit, const struct mark *mk, int *fail)
{
	if (code_exit || r->timed_out || r->trunc || !mk->ok || mk->status < 300 || mk->status > 399 || !mk->redir[0])
		return false;
	if (r->hops >= MAX_HOPS) {
		*fail = NET_EREDIRECTS;
		return false;
	}
	if (strncasecmp(mk->redir, "http://", 7) && strncasecmp(mk->redir, "https://", 8)) {
		*fail = NET_EOTHER;
		return false;
	}
	r->hops++;
	snprintf(r->url, sizeof(r->url), "%s", mk->redir);
	r->allow_priv = r->allow_priv && r->hop_priv;	/* LAN / tailnet only from a hop that already was */
	r->referer[0] = 0;
	if (r->post && mk->status != 307 && mk->status != 308) {	/* 301 / 302 / 303 after a POST: GET */
		free(r->post);
		r->post = NULL;
		r->posted = false;
	}
	free(r->buf);
	r->buf = NULL;
	r->len = r->cap = 0;
	r->elen = 0;
	r->pid = 0;
	hop_start(r);
	return true;
}

static void tick(lv_timer_t *t)
{
	for (int i = 0; i < nzombies;)
		if (waitpid(zombies[i], NULL, WNOHANG) != 0)
			zombies[i] = zombies[--nzombies];
		else
			i++;
	long long now = now_ms();
	struct net_req *r = reqs;
	while (r) {
		struct net_req *next = r->next;
		bool done = false;
		int code = 0, fail = 0;
		struct mark mk = { 0 };
		if (r->wifi_since) {	/* made while Wi-Fi slept: go once it is back (at most WIFI_WAIT_MS, then as before);
					 * its timeout counts from then */
			if (wifi_sleep_wait() && now - r->wifi_since < WIFI_WAIT_MS)
				goto next_req;
			r->deadline += now - r->wifi_since;
			r->wifi_since = 0;
			hop_start(r);
		}
		if (r->dns) {
			if (now > r->deadline) {
				dns_drop(&r->dns);
				r->fail = NET_ETIMEOUT;
			} else {
				hop_resolved(r);
			}
		}
		if (r->fail) {
			fail = r->fail;
			done = true;
		} else if (r->pid > 0) {
			drain_out(r);
			drain_err(r);
			if (now > r->deadline && (r->out >= 0 || r->err >= 0)) {
				r->timed_out = !r->trunc;
				kill(r->pid, SIGKILL);
			} else if (r->trunc && r->err >= 0 && now - r->trunc_at > 1500) {	/* a stalled server */
				kill(r->pid, SIGKILL);
			}
			if (r->out < 0 && r->err < 0) {
				int st = 0;
				pid_t w = waitpid(r->pid, &st, WNOHANG);
				if (w != 0 || now > r->deadline + 2000) {	/* else: pipes closed, still exiting */
					if (w == 0) {
						kill(r->pid, SIGKILL);
						zombie_add(r->pid);
					}
					code = w == r->pid ? (WIFEXITED(st) ? WEXITSTATUS(st) : -1) : 0;
					ck_done(r, w == r->pid && WIFEXITED(st));
					r->pid = 0;
					parse_mark(r, &mk);
					/* belt and braces: the address curl really talked to */
					int ce = mk.ip[0] ? class_check(r, ip_text_class(mk.ip)) : 0;
					if (ce)
						fail = ce;
					else if (redirect(r, code, &mk, &fail))
						goto next_req;
					done = true;
				}
			}
		}
		if (done) {
			unlink_req(r);
			finish(r, code, fail, &mk);	/* the callback may start or cancel other requests */
			req_free(r);
		}
next_req:
		r = next;
		/* the callback may have changed the list: restart from the head if r is gone */
		bool found = !r;
		for (struct net_req *q = reqs; q && !found; q = q->next)
			found = q == r;
		if (!found)
			r = reqs;
	}
	jobs_deliver();
	if (!reqs && !jpending && !nzombies)
		lv_timer_pause(t);
}

/* ---------- URL helpers ---------- */

void net_urlencode(const char *s, char *out, int n)
{
	static const char hex[] = "0123456789ABCDEF";
	int k = 0;
	for (; *s && k < n - 4; s++) {
		unsigned char c = *s;
		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out[k++] = c;
		else if (c == ' ')
			out[k++] = '+';
		else {
			out[k++] = '%';
			out[k++] = hex[c >> 4];
			out[k++] = hex[c & 15];
		}
	}
	out[k] = 0;
}

static int hexv(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1; }

void net_urldecode(char *s)
{
	char *w = s;
	for (; *s; s++) {
		if (*s == '%' && hexv((unsigned char)s[1]) >= 0 && hexv((unsigned char)s[2]) >= 0) {
			*w++ = hexv((unsigned char)s[1]) * 16 + hexv((unsigned char)s[2]);
			s += 2;
		} else if (*s == '+') {
			*w++ = ' ';
		} else {
			*w++ = *s;
		}
	}
	*w = 0;
}

const char *net_host(const char *url, char *out, int n)
{
	out[0] = 0;
	const char *p = strstr(url, "://");
	p = p ? p + 3 : url;
	const char *e = p + strcspn(p, "/?#");
	const char *at = memchr(p, '@', e - p);
	if (at)
		p = at + 1;
	const char *colon = memchr(p, ':', e - p);
	if (colon && !memchr(p, '[', e - p))
		e = colon;
	int len = e - p;
	if (len > n - 1)
		len = n - 1;
	for (int i = 0; i < len; i++)
		out[i] = tolower((unsigned char)p[i]);
	out[len] = 0;
	return out;
}

/* copy with spaces / controls percent-encoded (hrefs in the wild contain them) */
static void put_clean(char *out, int n, int *k, const char *s, int len)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < len && *k < n - 4; i++) {
		unsigned char c = s[i];
		if (c <= ' ' || c == 0x7f) {
			if (c == '\n' || c == '\r' || c == '\t')
				continue;
			out[(*k)++] = '%';
			out[(*k)++] = hex[c >> 4];
			out[(*k)++] = hex[c & 15];
		} else {
			out[(*k)++] = c;
		}
	}
	out[*k] = 0;
}

/* RFC 3986 remove_dot_segments on the URL's path (in place; n = size of url's buffer) */
static void dot_segments(char *url, int n)
{
	char *p = strstr(url, "://");
	if (!p || !(p = strchr(p + 3, '/')))
		return;
	int plen = strcspn(p, "?#");
	char tail[2048], path[2048], *segs[256];
	snprintf(tail, sizeof(tail), "%s", p + plen);
	snprintf(path, sizeof(path), "%.*s", plen, p);
	int ns = 0;
	bool trailing = false;
	for (char *s = path + 1;;) {
		char *e = strchr(s, '/');
		if (e)
			*e = 0;
		bool last = !e;
		if (!strcmp(s, ".")) {
			trailing = last;
		} else if (!strcmp(s, "..")) {
			if (ns)
				ns--;
			trailing = last;
		} else if (last && !*s) {
			trailing = true;
		} else if (ns < 256) {
			segs[ns++] = s;
			trailing = false;
		}
		if (last)
			break;
		s = e + 1;
	}
	int room = n - (p - url), k = 0;
	char out[2048];
	for (int i = 0; i < ns && k < (int)sizeof(out) - 2; i++)
		k += snprintf(out + k, sizeof(out) - k, "/%s", segs[i]);
	if (k >= (int)sizeof(out))
		k = sizeof(out) - 1;
	if ((trailing || !ns) && k < (int)sizeof(out) - 1)
		out[k++] = '/';
	out[k] = 0;
	snprintf(p, room, "%s%s", out, tail);
}

bool net_resolve(const char *base, const char *ref, char *out, int n)
{
	while (*ref && isspace((unsigned char)*ref))
		ref++;
	int rlen = strlen(ref);
	while (rlen > 0 && isspace((unsigned char)ref[rlen - 1]))
		rlen--;
	int k = 0;
	out[0] = 0;
	/* scheme? */
	int s = 0;
	while (s < rlen && (isalnum((unsigned char)ref[s]) || ref[s] == '+' || ref[s] == '-' || ref[s] == '.'))
		s++;
	if (s > 0 && s < rlen && ref[s] == ':' && isalpha((unsigned char)ref[0])) {
		if (!(s == 4 && !strncasecmp(ref, "http", 4)) && !(s == 5 && !strncasecmp(ref, "https", 5)))
			return false;	/* mailto:, javascript:, data:, tel: … */
		if (rlen < s + 3 || ref[s + 1] != '/' || ref[s + 2] != '/')
			return false;	/* "http:x" / "http:/x" — no authority: never a usable absolute URL */
		for (int i = 0; i < s; i++)
			out[k++] = tolower((unsigned char)ref[i]);
		put_clean(out, n, &k, ref + s, rlen - s);
		dot_segments(out, n);
		return true;
	}
	/* the base must itself be absolute http(s): a "://" found anywhere else (query, fragment,
	 * a "http:x?http://a" <base href>) would put scheme_end past the end of shorter results */
	int scheme_end = !strncasecmp(base, "http://", 7) ? 7 : !strncasecmp(base, "https://", 8) ? 8 : 0;
	if (!scheme_end)
		return false;
	int auth_end = scheme_end + strcspn(base + scheme_end, "/?#");
	if (ref[0] == '/' && ref[1] == '/') {	/* network-path */
		put_clean(out, n, &k, base, scheme_end - 2);
		put_clean(out, n, &k, ref, rlen);
	} else if (ref[0] == '/') {
		put_clean(out, n, &k, base, auth_end);
		put_clean(out, n, &k, ref, rlen);
	} else if (rlen == 0 || ref[0] == '#') {
		int bl = strcspn(base, "#");
		put_clean(out, n, &k, base, bl);
		put_clean(out, n, &k, ref, rlen);
	} else if (ref[0] == '?') {
		int bl = strcspn(base, "?#");
		put_clean(out, n, &k, base, bl);
		put_clean(out, n, &k, ref, rlen);
	} else {
		int pe = auth_end + strcspn(base + auth_end, "?#");
		int slash = pe;
		while (slash > auth_end && base[slash - 1] != '/')
			slash--;
		if (slash == auth_end) {	/* no path: "http://host" */
			put_clean(out, n, &k, base, auth_end);
			put_clean(out, n, &k, "/", 1);
		} else {
			put_clean(out, n, &k, base, slash);
		}
		put_clean(out, n, &k, ref, rlen);
	}
	int q = k >= scheme_end ? scheme_end + (int)strcspn(out + scheme_end, "/?#") : k;
	if (k >= scheme_end && out[q] != '/' && k < n - 2) {	/* "http://host?q" → "http://host/?q" */
		memmove(out + q + 1, out + q, k - q + 1);
		out[q] = '/';
		k++;
	}
	dot_segments(out, n);
	return true;
}
