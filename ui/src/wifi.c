/* Wi-Fi: wpa_supplicant (started at boot by /usr/sbin/wifiup) through its control socket
 * /var/run/wpa_supplicant/wlan0 — the wpa_ctrl protocol by hand: two AF_UNIX datagram sockets bound
 * to /tmp/orchard-wpa-<pid>-{c,m}<n> (a new n each open), "c" for request → reply (one command in flight, a queue
 * behind it), "m" ATTACHed for the "<N>CTRL-EVENT-..." stream. Everything runs on the LVGL thread from one timer
 * (20 ms while a reply is due, else 200 ms) and never blocks. DHCP is wpa_cli's action script's job.
 * On/off = `wifiup on|off` (fork/exec, reaped by the timer); off leaves a flag file <conf dir>/off.
 * Joining: SELECT_NETWORK disables every other network in wpa_supplicant's memory, so the ones that
 * were enabled get re-enabled (ENABLE_NETWORK n no-connect) once the join ends, and SAVE_CONFIG only
 * runs after that. Auto-Join off = the network's `disabled` flag (SET_NETWORK n disabled 1 — unlike
 * DISABLE_NETWORK it doesn't drop the current connection).
 * Wi-Fi Sleep (power.c says when): DISCONNECT + wlan0 down, the user's on/off untouched; wake = wlan0 up, a scan on
 * the old channel, RECONNECT (see "Wi-Fi Sleep" below).
 * Previews (snap/sdl): fake networks, ORCHARD_WIFI=off|none|idle for the other states; ORCHARD_WIFI=real
 * talks to a real wpa_supplicant (ORCHARD_WPA_CTRL = its socket, ORCHARD_WIFIUP = the on/off script). */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#ifdef ORCHARD_SNAP
#include <poll.h>
#endif
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef ORCHARD_PHONE
#include <net/if.h>
#include <pthread.h>
#include <sys/ioctl.h>
#endif
#include "orchard.h"

#define IFACE "wlan0"
#define MAXSCAN 48
#define MAXSAVED 32
#define MAXQ 32
#define MAXL 6
#define SCAN_EVERY 10000	/* rescan while the Wi-Fi page is open */
#define JOIN_TIMEOUT 30000

struct bss { char ssid[33], bssid[18]; int freq, sig; enum wifi_sec sec; };
struct saved { int id; char ssid[33]; bool disabled; };

static struct bss scan[MAXSCAN];
static int nscan;
static struct saved sv[MAXSAVED];
static int nsv;
static struct {
	bool up;	/* wpa_state=COMPLETED */
	char state[24], ssid[33], bssid[18], ip[16], mac[18], key_mgmt[32];
	int id, freq, rssi, speed;
} lk = { .id = -1 };
static enum wifi_state st = WIFI_NONE;
static bool fake, scanning, want_scan;
static uint32_t scan_at, results_at, status_at, signal_at, open_at, check_at, start_at;
static uint32_t last_sig;
static void (*listeners[MAXL])(void);
static lv_timer_t *timer;
static unsigned status_gen, scan_gen;	/* STATUS replies / scans that ended (Wi-Fi Sleep's wake waits on them) */

/* the join in progress (one at a time) */
static struct {
	bool on, added, hidden, was_disabled;
	char ssid[33], pw[72];
	enum wifi_sec sec;
	int id, gen, notfound, rejects;
	uint32_t t0;
} jn = { .id = -1 };
static int restore[MAXSAVED], nrestore;	/* enabled before SELECT_NETWORK: enable again afterwards */
static bool save_pending;

/* ================================ helpers ================================ */

static int bars_of(int dbm) { return dbm == 0 ? 0 : dbm >= -60 ? 3 : dbm >= -72 ? 2 : 1; }

/* wpa_supplicant prints SSIDs printf-escaped (\\ \" \e \n \r \t \xNN) */
static void unescape(const char *s, char *out, int max)
{
	int n = 0;
	while (*s && n < max - 1) {
		char c = *s++;
		if (c == '\\' && *s) {
			c = *s++;
			switch (c) {
			case 'n': c = '\n'; break;
			case 'r': c = '\r'; break;
			case 't': c = '\t'; break;
			case 'e': c = 27; break;
			case 'x': {
				char h[3] = { s[0], s[0] ? s[1] : 0, 0 };
				char *end;
				long v = strtol(h, &end, 16);
				if (end != h + 2)
					continue;
				c = (char)v;
				s += 2;
				break;
			}
			default: break;	/* \\ and \" */
			}
		}
		out[n++] = c;
	}
	out[n] = 0;
}

/* raw SSID bytes → something a label can show: invalid UTF-8 and control characters become '?' */
static void display_name(const char *s, char *out, int max)
{
	const unsigned char *p = (const unsigned char *)s;
	int k = 0;
	while (*p && k < max - 5) {
		int len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
		bool ok = len > 0;
		for (int i = 1; ok && i < len; i++)
			ok = (p[i] & 0xc0) == 0x80;
		if (!ok || (len == 1 && (*p < 0x20 || *p == 0x7f))) {
			out[k++] = '?';
			p++;
			continue;
		}
		memcpy(out + k, p, len);
		k += len;
		p += len;
	}
	out[k] = 0;
}

static void hex(const char *s, char *out)
{
	for (; *s; s++, out += 2)
		sprintf(out, "%02x", (unsigned char)*s);
	*out = 0;
}

static enum wifi_sec sec_of(const char *f)
{
	if (strstr(f, "EAP"))
		return WIFI_EAP;
	if (strstr(f, "PSK"))
		return strstr(f, "SAE") ? WIFI_WPA23 : WIFI_WPA;
	if (strstr(f, "SAE"))
		return WIFI_SAE;
	if (strstr(f, "OWE"))
		return WIFI_OWE;
	if (strstr(f, "WEP"))
		return WIFI_WEP;
	return WIFI_OPEN;
}

const char *wifi_sec_name(enum wifi_sec s)
{
	switch (s) {
	case WIFI_OWE: return "Enhanced Open";
	case WIFI_WEP: return "WEP";
	case WIFI_WPA: return "WPA2 Personal";
	case WIFI_WPA23: return "WPA2/WPA3 Personal";
	case WIFI_SAE: return "WPA3 Personal";
	case WIFI_EAP: return "WPA2 Enterprise";
	default: return "None";
	}
}

bool wifi_password_ok(enum wifi_sec sec, const char *pw)
{
	int n = strlen(pw), x = 0;
	for (int i = 0; i < n; i++)
		x += !!strchr("0123456789abcdefABCDEF", pw[i]);
	if (sec == WIFI_OPEN || sec == WIFI_OWE)
		return true;
	if (sec == WIFI_WEP)
		return n == 5 || n == 13 || ((n == 10 || n == 26) && x == n);
	return (n >= 8 && n <= 63) || (n == 64 && x == 64);
}

static struct saved *saved_find(const char *ssid)
{
	for (int i = 0; i < nsv; i++)
		if (!strcmp(sv[i].ssid, ssid))
			return &sv[i];
	return NULL;
}

static struct saved *saved_id(int id)
{
	for (int i = 0; i < nsv; i++)
		if (sv[i].id == id)
			return &sv[i];
	return NULL;
}

static struct bss *scan_find(const char *ssid)
{
	for (int i = 0; i < nscan; i++)
		if (!strcmp(scan[i].ssid, ssid))
			return &scan[i];
	return NULL;
}

static void alert(const char *title, const char *msg) { ui_alert(title, msg, (const char *[]){ "OK" }, 1, NULL, NULL); }

static const char *wifiup_path(void)
{
	const char *e = getenv("ORCHARD_WIFIUP");
	return e && *e ? e : "/usr/sbin/wifiup";
}

static const char *ctrl_path(void)
{
	const char *e = getenv("ORCHARD_WPA_CTRL");
	return e && *e ? e : "/var/run/wpa_supplicant/" IFACE;
}

/* `wifiup off` leaves <conf dir>/off: /data/wifi when /data is mounted, else /tmp/wifi */
static bool off_flag(void) { return !access("/data/wifi/off", F_OK) || !access("/tmp/wifi/off", F_OK); }

/* ================================ control socket ================================ */

enum { Q_OK, Q_STATUS, Q_SIGNAL, Q_SCAN, Q_RESULTS, Q_LIST, Q_ADD, Q_WS };
static struct qe { int kind, arg; char s[200]; } q[MAXQ];
static int nq;
static bool in_flight;
static uint32_t sent_at;
static int fd_c = -1, fd_m = -1;
static char lp_c[80], lp_m[80];

static void push(int kind, int arg, const char *fmt, ...)
{
	if (fd_c < 0 || nq == MAXQ)
		return;
	if (kind == Q_STATUS || kind == Q_SIGNAL || kind == Q_RESULTS || kind == Q_LIST)
		for (int i = in_flight; i < nq; i++)	/* waiting to go: it moves behind the latest changes */
			if (q[i].kind == kind) {
				memmove(q + i, q + i + 1, (nq - i - 1) * sizeof(q[0]));
				nq--;
				break;
			}
	struct qe *e = &q[nq++];
	e->kind = kind;
	e->arg = arg;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(e->s, sizeof(e->s), fmt, ap);
	va_end(ap);
	if (timer)
		lv_timer_set_period(timer, 20);
}

/* a new local name for every open (like wpa_ctrl's counter): replies wpa_supplicant still owes a socket that was
 * given up (lost: "no reply" while it was stuck) must not land in the new one — each would be read as the answer to
 * the next command, and every reply after it one command off */
static int open_sock(char *local, int n, char tag)
{
	static unsigned seq;
	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un a = { .sun_family = AF_UNIX }, d = { .sun_family = AF_UNIX };
	snprintf(local, n, "/tmp/orchard-wpa-%d-%c%u", (int)getpid(), tag, seq++);
	unlink(local);
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", local);
	snprintf(d.sun_path, sizeof(d.sun_path), "%s", ctrl_path());
	if (bind(fd, (struct sockaddr *)&a, sizeof(a)) || connect(fd, (struct sockaddr *)&d, sizeof(d))) {
		close(fd);
		unlink(local);
		local[0] = 0;
		return -1;
	}
	int sz = 1 << 16;
	setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
	return fd;
}

static void ctrl_close(void)
{
	if (fd_c >= 0)
		close(fd_c);
	if (fd_m >= 0)
		close(fd_m);
	fd_c = fd_m = -1;
	if (lp_c[0])
		unlink(lp_c);
	if (lp_m[0])
		unlink(lp_m);
	lp_c[0] = lp_m[0] = 0;
	nq = 0;
	in_flight = false;
	nscan = nsv = 0;
	memset(&lk, 0, sizeof(lk));
	lk.id = -1;
	scanning = false;
	results_at = 0;
	memset(&jn, 0, sizeof(jn));
	jn.id = -1;
	nrestore = 0;
	save_pending = false;
}

static void lost(const char *why)
{
	fprintf(stderr, "wifi: lost wpa_supplicant (%s)\n", why);
	ctrl_close();
}

static bool ctrl_open(void)
{
	if (access(ctrl_path(), F_OK))
		return false;
	fd_c = open_sock(lp_c, sizeof(lp_c), 'c');
	if (fd_c < 0)
		return false;
	fd_m = open_sock(lp_m, sizeof(lp_m), 'm');
	if (fd_m < 0 || send(fd_m, "ATTACH", 6, 0) != 6) {
		ctrl_close();
		return false;
	}
	fprintf(stderr, "wifi: attached to %s\n", ctrl_path());
	push(Q_STATUS, 0, "STATUS");
	push(Q_LIST, 0, "LIST_NETWORKS");
	push(Q_RESULTS, 0, "SCAN_RESULTS");	/* whatever it found before we came */
	scan_at = lv_tick_get() - SCAN_EVERY;	/* and a fresh scan if the page wants one */
	return true;
}

/* ================================ joining ================================ */

static void fake_autojoin(void);
static void ws_reply(int step, const char *r);
static void ws_abort(const char *why);
static void ws_abort_all(const char *why);

enum { JF_GENERIC, JF_WRONG_KEY, JF_NOT_FOUND };

static void request_save(void)
{
	if (jn.on)
		save_pending = true;	/* the other networks are disabled until the join ends */
	else
		push(Q_OK, 0, "SAVE_CONFIG");
}

static void join_end(bool ok)
{
	if (!jn.on)
		return;
	jn.on = false;
	memset(jn.pw, 0, sizeof(jn.pw));
	if (!ok && jn.added && jn.id >= 0) {
		push(Q_OK, 0, "REMOVE_NETWORK %d", jn.id);
		for (int i = 0; i < nsv; i++)
			if (sv[i].id == jn.id) {
				memmove(sv + i, sv + i + 1, (--nsv - i) * sizeof(sv[0]));
				break;
			}
	}
	for (int i = 0; i < nrestore; i++)
		push(Q_OK, 0, "ENABLE_NETWORK %d no-connect", restore[i]);
	nrestore = 0;
	if (jn.was_disabled && jn.id >= 0 && (ok || !jn.added))	/* joined by hand: Auto-Join stays off */
		push(Q_OK, 0, "SET_NETWORK %d disabled 1", jn.id);
	if (ok || save_pending || jn.was_disabled)
		push(Q_OK, 0, "SAVE_CONFIG");
	save_pending = false;
	if (!ok && !lk.up)
		push(Q_OK, 0, "REASSOCIATE");	/* back to whatever it can auto-join */
	push(Q_LIST, 0, "LIST_NETWORKS");
	push(Q_STATUS, 0, "STATUS");
	if (ok) {
		ui_haptic_success();
		fprintf(stderr, "wifi: joined network %d\n", jn.id);
	}
}

static void join_fail(int why)
{
	char name[100], t[160];
	if (!jn.on)
		return;
	display_name(jn.ssid, name, sizeof(name));
	fprintf(stderr, "wifi: join %d failed (%s)\n", jn.id, why == JF_WRONG_KEY ? "wrong key" : why == JF_NOT_FOUND ? "not found" : "timeout");
	if (why == JF_WRONG_KEY) {
		snprintf(t, sizeof(t), "Incorrect password for \xe2\x80\x9c%s\xe2\x80\x9d", name);
		alert(t, jn.added ? "Check the password and try joining again." : "The saved password didn't work. Forget the network and join it again with the new one.");
	} else {
		snprintf(t, sizeof(t), "Unable to join the network \xe2\x80\x9c%s\xe2\x80\x9d", name);
		alert(t, why == JF_NOT_FOUND ? "It isn't in range any more." : "The network didn't answer. Move closer to the router and try again.");
	}
	join_end(false);
}

static void select_net(int id)
{
	struct saved *s = saved_id(id);
	jn.id = id;
	jn.was_disabled = s && s->disabled;
	nrestore = 0;
	for (int i = 0; i < nsv; i++)
		if (sv[i].id != id && !sv[i].disabled)
			restore[nrestore++] = sv[i].id;
	push(Q_OK, 0, "SELECT_NETWORK %d", id);
}

/* the security settings of network id (psk is never logged: see on_reply) */
static void configure(int id)
{
	const char *pw = jn.pw;
	int n = strlen(pw);
	switch (jn.sec) {
	case WIFI_OPEN:
		push(Q_OK, 0, "SET_NETWORK %d key_mgmt NONE", id);
		break;
	case WIFI_OWE:
		push(Q_OK, 0, "SET_NETWORK %d key_mgmt OWE", id);
		push(Q_OK, 0, "SET_NETWORK %d ieee80211w 2", id);
		break;
	case WIFI_WEP:
		push(Q_OK, 0, "SET_NETWORK %d key_mgmt NONE", id);
		push(Q_OK, 0, n == 10 || n == 26 ? "SET_NETWORK %d wep_key0 %s" : "SET_NETWORK %d wep_key0 \"%s\"", id, pw);
		push(Q_OK, 0, "SET_NETWORK %d wep_tx_keyidx 0", id);
		break;
	case WIFI_SAE:
		push(Q_OK, 0, "SET_NETWORK %d key_mgmt SAE", id);
		push(Q_OK, 0, "SET_NETWORK %d ieee80211w 2", id);
		/* fall through: SAE takes the passphrase from psk */
	default:
		push(Q_OK, 0, n == 64 ? "SET_NETWORK %d psk %s" : "SET_NETWORK %d psk \"%s\"", id, pw);
		break;
	}
	memset(jn.pw, 0, sizeof(jn.pw));
}

/* ADD_NETWORK answered: the new network's settings, then select it */
static void added(int gen, const char *r)
{
	if (strncmp(r, "FAIL", 4) == 0 || !jn.on || gen != jn.gen) {
		if (strncmp(r, "FAIL", 4))	/* the join was called off meanwhile */
			push(Q_OK, 0, "REMOVE_NETWORK %d", atoi(r));
		else
			join_fail(JF_GENERIC);
		return;
	}
	int id = atoi(r);
	char h[67];
	jn.id = id;
	jn.added = true;
	hex(jn.ssid, h);
	push(Q_OK, 0, "SET_NETWORK %d ssid %s", id, h);
	if (jn.hidden)
		push(Q_OK, 0, "SET_NETWORK %d scan_ssid 1", id);
	configure(id);
	select_net(id);
}

static void join(const char *ssid, enum wifi_sec sec, const char *pw, bool hidden)
{
	char name[100], t[200];
	if (st != WIFI_ON || !*ssid || strlen(ssid) > 32)
		return;
	if (sec == WIFI_EAP) {
		display_name(ssid, name, sizeof(name));
		snprintf(t, sizeof(t), "\xe2\x80\x9c%s\xe2\x80\x9d uses WPA2 Enterprise (a username and password), which "
			 "Orchard can't join yet.", name);
		alert("Enterprise Network", t);
		return;
	}
	if (jn.on) {
		if (!strcmp(jn.ssid, ssid))
			return;
		join_end(false);	/* the newer tap wins */
	}
	if (lk.up && !strcmp(lk.ssid, ssid))
		return;
	ws_abort("a join");	/* woken from Wi-Fi Sleep: this join is the reconnect now */
	int gen = jn.gen + 1;
	memset(&jn, 0, sizeof(jn));
	jn.on = true;
	jn.gen = gen;
	jn.id = -1;
	jn.sec = sec;
	jn.hidden = hidden;
	jn.t0 = lv_tick_get();
	snprintf(jn.ssid, sizeof(jn.ssid), "%s", ssid);
	snprintf(jn.pw, sizeof(jn.pw), "%s", pw ? pw : "");
	if (fake) {
		jn.added = !saved_find(ssid);
		return;	/* fake_tick plays it out */
	}
	struct saved *s = saved_find(ssid);
	if (s) {
		if (jn.pw[0])
			configure(s->id);	/* a new password for a saved network */
		select_net(s->id);
	} else {
		push(Q_ADD, jn.gen, "ADD_NETWORK");
	}
	push(Q_STATUS, 0, "STATUS");
}

void wifi_join(const char *ssid, const char *password)
{
	struct bss *b = scan_find(ssid);
	struct saved *s = saved_find(ssid);
	if (!b && !s)
		return;
	join(ssid, b ? b->sec : WIFI_WPA, password, !b);
}

void wifi_join_hidden(const char *ssid, enum wifi_sec sec, const char *password) { join(ssid, sec, password, true); }

void wifi_forget(const char *ssid)
{
	struct saved *s = saved_find(ssid);
	if (!s)
		return;
	int id = s->id;
	if (jn.on && jn.id == id)
		join_end(false);
	push(Q_OK, 0, "REMOVE_NETWORK %d", id);
	if ((s = saved_id(id)))
		memmove(s, s + 1, (--nsv - (s - sv)) * sizeof(sv[0]));
	for (int i = 0; i < nrestore; i++)
		if (restore[i] == id)
			restore[i--] = restore[--nrestore];
	if (lk.up && lk.id == id) {	/* wpa_supplicant drops it and auto-joins another one */
		lk.up = false;
		if (fake) {
			lk.ssid[0] = 0;
			fake_autojoin();
		}
	}
	request_save();
	push(Q_LIST, 0, "LIST_NETWORKS");
	push(Q_STATUS, 0, "STATUS");
}

void wifi_set_autojoin(const char *ssid, bool on)
{
	struct saved *s = saved_find(ssid);
	if (!s || s->disabled == !on)
		return;
	s->disabled = !on;
	if (jn.on && jn.id == s->id) {
		jn.was_disabled = !on;
		return;
	}
	if (jn.on) {	/* SELECT_NETWORK has every other network disabled: change what comes back */
		for (int i = 0; i < nrestore; i++)
			if (restore[i] == s->id)
				restore[i--] = restore[--nrestore];
		if (on)
			restore[nrestore++] = s->id;
		else
			push(Q_OK, 0, "SET_NETWORK %d disabled 1", s->id);
		save_pending = true;
		return;
	}
	push(Q_OK, 0, on ? "ENABLE_NETWORK %d no-connect" : "SET_NETWORK %d disabled 1", s->id);
	request_save();
	push(Q_LIST, 0, "LIST_NETWORKS");
}

/* ================================ replies + events ================================ */

static void parse_status(char *r)
{
	char *sp, prev_ssid[33];
	bool was = lk.up;
	snprintf(prev_ssid, sizeof(prev_ssid), "%s", lk.ssid);
	lk.state[0] = lk.ssid[0] = lk.bssid[0] = lk.ip[0] = lk.key_mgmt[0] = 0;
	lk.id = -1;
	lk.freq = 0;
	for (char *l = strtok_r(r, "\n", &sp); l; l = strtok_r(NULL, "\n", &sp)) {
		char *v = strchr(l, '=');
		if (!v)
			continue;
		*v++ = 0;
		if (!strcmp(l, "wpa_state"))
			snprintf(lk.state, sizeof(lk.state), "%s", v);
		else if (!strcmp(l, "ssid"))
			unescape(v, lk.ssid, sizeof(lk.ssid));
		else if (!strcmp(l, "bssid"))
			snprintf(lk.bssid, sizeof(lk.bssid), "%s", v);
		else if (!strcmp(l, "ip_address"))
			snprintf(lk.ip, sizeof(lk.ip), "%s", v);
		else if (!strcmp(l, "address"))
			snprintf(lk.mac, sizeof(lk.mac), "%s", v);
		else if (!strcmp(l, "key_mgmt"))
			snprintf(lk.key_mgmt, sizeof(lk.key_mgmt), "%s", v);
		else if (!strcmp(l, "id"))
			lk.id = atoi(v);
		else if (!strcmp(l, "freq"))
			lk.freq = atoi(v);
	}
	lk.up = !strcmp(lk.state, "COMPLETED") && lk.ssid[0];
	if (!lk.up || strcmp(prev_ssid, lk.ssid))
		lk.rssi = lk.speed = 0;
	if (lk.up && (!was || !lk.rssi)) {
		signal_at = lv_tick_get();
		push(Q_SIGNAL, 0, "SIGNAL_POLL");
	}
	if (jn.on && lk.up && jn.id >= 0 && lk.id == jn.id)	/* missed the CONNECTED event */
		join_end(true);
	status_gen++;
}

static void parse_signal(char *r)
{
	char *sp;
	if (!strncmp(r, "FAIL", 4))
		return;
	for (char *l = strtok_r(r, "\n", &sp); l; l = strtok_r(NULL, "\n", &sp)) {
		if (!strncmp(l, "RSSI=", 5))
			lk.rssi = atoi(l + 5);
		else if (!strncmp(l, "LINKSPEED=", 10))
			lk.speed = atoi(l + 10);
		else if (!strncmp(l, "FREQUENCY=", 10) && atoi(l + 10))
			lk.freq = atoi(l + 10);
	}
	if (lk.rssi > 0)	/* a driver that reports quality instead of dBm */
		lk.rssi = lk.rssi >= 100 ? -40 : -100 + lk.rssi / 2;
}

/* bssid \t freq \t signal \t flags \t ssid — one line per access point: one entry per SSID, strongest wins */
static void parse_results(char *r)
{
	char *sp;
	nscan = 0;
	for (char *l = strtok_r(r, "\n", &sp); l; l = strtok_r(NULL, "\n", &sp)) {
		char *f[5] = { l };
		int k = 1;
		for (char *p = l; *p && k < 5; p++)
			if (*p == '\t') {
				*p = 0;
				f[k++] = p + 1;
			}
		if (k < 5 || strlen(f[0]) != 17)
			continue;	/* the header, or a hidden network (no SSID) */
		char ssid[33];
		unescape(f[4], ssid, sizeof(ssid));
		if (!ssid[0] || strstr(f[3], "[IBSS]") || strstr(f[3], "[MESH]"))
			continue;
		int sig = atoi(f[2]);
		if (sig > 0)
			sig = sig >= 100 ? -40 : -100 + sig / 2;
		struct bss *b = scan_find(ssid);
		if (!b) {
			if (nscan == MAXSCAN)
				continue;
			b = &scan[nscan++];
			snprintf(b->ssid, sizeof(b->ssid), "%s", ssid);
			b->sig = -200;
		}
		if (sig > b->sig) {
			b->sig = sig;
			b->freq = atoi(f[1]);
			b->sec = sec_of(f[3]);
			snprintf(b->bssid, sizeof(b->bssid), "%s", f[0]);
		}
	}
	for (int i = 0; i < nscan; i++)
		if (scan[i].sig < -100)
			scan[i].sig = -100;
}

/* network id \t ssid \t bssid \t flags */
static void parse_list(char *r)
{
	char *sp;
	if (!strncmp(r, "FAIL", 4))
		return;
	nsv = 0;
	for (char *l = strtok_r(r, "\n", &sp); l && nsv < MAXSAVED; l = strtok_r(NULL, "\n", &sp)) {
		char *f[4] = { l };
		int k = 1;
		for (char *p = l; *p && k < 4; p++)
			if (*p == '\t') {
				*p = 0;
				f[k++] = p + 1;
			}
		if (k < 4 || !(f[0][0] >= '0' && f[0][0] <= '9'))
			continue;
		struct saved *s = &sv[nsv++];
		s->id = atoi(f[0]);
		unescape(f[1], s->ssid, sizeof(s->ssid));
		/* SELECT_NETWORK disabled the others for a while: what they were stays ours */
		bool parked = false;
		for (int i = 0; i < nrestore; i++)
			parked |= restore[i] == s->id;
		s->disabled = strstr(f[3], "[DISABLED]") && !parked;
		if (jn.on && s->id == jn.id)
			s->disabled = jn.was_disabled;
	}
}

static void on_reply(const struct qe *e, char *r)
{
	switch (e->kind) {
	case Q_STATUS: parse_status(r); break;
	case Q_SIGNAL: parse_signal(r); break;
	case Q_RESULTS: parse_results(r); break;
	case Q_LIST: parse_list(r); break;
	case Q_ADD: added(e->arg, r); break;
	case Q_WS: ws_reply(e->arg, r); break;
	case Q_SCAN:
		if (!strncmp(r, "OK", 2) || !strncmp(r, "FAIL-BUSY", 9)) {
			scanning = true;
			results_at = lv_tick_get() + 8000;	/* in case the SCAN-RESULTS event never comes */
		}
		break;
	default:
		if (strncmp(r, "OK", 2)) {
			char c[64];
			snprintf(c, sizeof(c), "%s", e->s);
			char *pw = strstr(c, " psk ");
			if (!pw)
				pw = strstr(c, " wep_key0 ");
			if (pw)
				strcpy(pw, " <key>");
			fprintf(stderr, "wifi: %s -> %.40s", c, r);
		}
		break;
	}
}

static int field_int(const char *m, const char *key)
{
	const char *p = strstr(m, key);
	return p ? atoi(p + strlen(key)) : -1;
}

static void on_event(char *m)
{
	if (*m == '<') {
		char *e = strchr(m, '>');
		if (!e)
			return;
		m = e + 1;
	}
	if (!strncmp(m, "CTRL-EVENT-SCAN-STARTED", 23)) {
		scanning = true;
	} else if (!strncmp(m, "CTRL-EVENT-SCAN-RESULTS", 23)) {
		scanning = false;
		results_at = 0;
		scan_gen++;
		push(Q_RESULTS, 0, "SCAN_RESULTS");
	} else if (!strncmp(m, "CTRL-EVENT-SCAN-FAILED", 22)) {
		scanning = false;
		results_at = 0;
		scan_gen++;
	} else if (!strncmp(m, "CTRL-EVENT-CONNECTED", 20)) {
		int id = field_int(m, "[id=");
		push(Q_STATUS, 0, "STATUS");
		push(Q_LIST, 0, "LIST_NETWORKS");
		if (jn.on && id >= 0 && id == jn.id)
			join_end(true);
	} else if (!strncmp(m, "CTRL-EVENT-DISCONNECTED", 23)) {
		push(Q_STATUS, 0, "STATUS");
	} else if (!strncmp(m, "CTRL-EVENT-SSID-TEMP-DISABLED", 29)) {
		int id = field_int(m, "id=");
		if (jn.on && id == jn.id)
			join_fail(strstr(m, "reason=WRONG_KEY") || (jn.sec == WIFI_SAE && strstr(m, "reason=AUTH_FAILED"))
					  ? JF_WRONG_KEY : JF_GENERIC);
		push(Q_LIST, 0, "LIST_NETWORKS");
	} else if (strstr(m, "pre-shared key may be incorrect")) {
		if (jn.on && jn.id >= 0)
			join_fail(JF_WRONG_KEY);
	} else if (!strncmp(m, "CTRL-EVENT-NETWORK-NOT-FOUND", 28)) {
		if (jn.on && ++jn.notfound >= 3 && lv_tick_elaps(jn.t0) > 10000)
			join_fail(JF_NOT_FOUND);
	} else if (!strncmp(m, "CTRL-EVENT-ASSOC-REJECT", 23) || !strncmp(m, "CTRL-EVENT-AUTH-REJECT", 22)) {
		if (jn.on && ++jn.rejects >= 4)
			join_fail(JF_GENERIC);
	} else if (!strncmp(m, "Trying to associate", 19) || !strncmp(m, "Associated with", 15)) {
		push(Q_STATUS, 0, "STATUS");
	} else if (!strncmp(m, "CTRL-EVENT-TERMINATING", 22)) {
		lost("terminating");
	}
}

static void pump(void)
{
	static char buf[16384];
	ssize_t n;
	while (fd_m >= 0 && (n = recv(fd_m, buf, sizeof(buf) - 1, 0)) > 0) {
		buf[n] = 0;
		if (buf[0] == '<')
			on_event(buf);
	}
	for (int guard = 0; guard < 8 && fd_c >= 0; guard++) {
		if (in_flight) {
#ifdef ORCHARD_SNAP	/* simulated clock vs a real (mock) wpa_supplicant: wait for the reply in real time, or 5 "s" pass at once */
			poll(&(struct pollfd){ .fd = fd_c, .events = POLLIN }, 1, 200);
#endif
			n = recv(fd_c, buf, sizeof(buf) - 1, 0);
			if (n < 0) {
				if (errno != EAGAIN && errno != EWOULDBLOCK)
					lost("recv");
				else if (lv_tick_elaps(sent_at) > 5000)
					lost("no reply");
				return;
			}
			buf[n] = 0;
			struct qe e = q[0];
			memmove(q, q + 1, --nq * sizeof(q[0]));
			memset(&q[nq], 0, sizeof(q[0]));	/* passwords don't linger */
			in_flight = false;
			on_reply(&e, buf);
			memset(&e, 0, sizeof(e));
			continue;
		}
		if (!nq)
			return;
		if (send(fd_c, q[0].s, strlen(q[0].s), 0) < 0) {
			if (errno != EAGAIN)
				lost("send");
			return;
		}
		in_flight = true;
		sent_at = lv_tick_get();
	}
}

/* ================================ wifiup on|off ================================ */

static pid_t up_pid;
static int up_next = -1;	/* asked again while the previous one ran */
static bool up_want;

static void spawn_up(bool on)
{
	up_want = on;
	pid_t p = fork();
	if (p == 0) {
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(fd, 1);
		}
		for (int i = 3; i < 1024; i++)
			close(i);
		setsid();
		execl(wifiup_path(), "wifiup", on ? "on" : "off", (char *)NULL);
		_exit(127);
	}
	up_pid = p > 0 ? p : 0;
	fprintf(stderr, "wifi: %s %s (pid %d)\n", wifiup_path(), on ? "on" : "off", (int)p);
}

static void reap(void)
{
	int stt;
	if (up_pid <= 0)
		return;
	pid_t r = waitpid(up_pid, &stt, WNOHANG);
	if (r == 0 || (r < 0 && errno == EINTR))
		return;
	int code = r == up_pid && WIFEXITED(stt) ? WEXITSTATUS(stt) : -1;
	up_pid = 0;
	if (up_next >= 0) {
		spawn_up(up_next);
		up_next = -1;
	} else if (code) {
		fprintf(stderr, "wifi: wifiup %s exited %d\n", up_want ? "on" : "off", code);
		if (up_want)
			alert("Wi-Fi Unavailable", "Wi-Fi didn't start (wifiup on failed). Its log is in /tmp/orchard.log.");
	}
	check_at = 0;
}

static void update_state(void)
{
	uint32_t now = lv_tick_get();
	if (!up_pid && check_at && lv_tick_elaps(check_at) < 1000)
		return;
	check_at = now ? now : 1;
	enum wifi_state s;
	if (access(wifiup_path(), X_OK) && fd_c < 0 && access(ctrl_path(), F_OK))
		s = WIFI_NONE;
	else if (up_pid)
		s = up_want ? (fd_c >= 0 ? WIFI_ON : WIFI_STARTING) : WIFI_OFF;
	else if (off_flag())
		s = WIFI_OFF;
	else
		s = fd_c >= 0 ? WIFI_ON : WIFI_STARTING;
	if (s == WIFI_OFF && fd_c >= 0)
		ctrl_close();
	if (s == WIFI_OFF)
		ws_abort_all("Wi-Fi is off");	/* (switched off from the console while it slept: stays off) */
	if (s != st && s == WIFI_STARTING)
		start_at = now;
	st = s;
}

/* ================================ Wi-Fi Sleep ================================ */

/* power.c decides when (the screen dark long enough, nothing needing the network); this does it without touching the
 * user's on/off (no wifiup, no off flag), so the UI keeps showing Wi-Fi on:
 *   sleep: DISCONNECT (wpa_supplicant's "disconnected" flag: it neither scans nor joins by itself; the DISCONNECTED
 *          action stops udhcpc and flushes the address, so the default route goes), then wlan0 down — brcmfmac's
 *          "down": the radio is off, firmware + PCIe stay (no reload: the same link down / up `wifiup off|on` does).
 *   wake:  wlan0 up, STATUS until wpa_supplicant has seen it, a scan on the channel it was on (one channel ≈ 0.1 s
 *          instead of ≈ 3 s for both bands with the DFS ones passive), RECONNECT — which joins straight from those
 *          fresh results (or does its normal scan when the network isn't there). Then DHCP (the action script).
 * The interface flips run on a short thread (brcmfmac talks to the firmware: tens of ms). A marker file says it is
 * asleep (WS_MARK): an Orchard that starts again meanwhile wakes it (else wpa_supplicant would stay disconnected).
 * Never stuck half awake: a wlan0 up that fails is tried again (0.5 / 1 / 2 s later), then once the whole
 * `wifiup on`; RECONNECT is repeated until wpa_supplicant takes it, and a control socket lost on the way (lost():
 * the queue goes with it, the wake doesn't) gets it again as soon as the socket is back.
 * Previews: the fake network drops and comes back ORCHARD_WIFI_WAKE_MS (default 1200) after the wake. */
#define WS_MARK "/tmp/orchard-wifisleep"	/* (ORCHARD_WIFISLEEP_MARK = elsewhere: test/wifi/sleep.sh) */
#define WS_GIVE_UP 20000	/* woken this long without a connection: plain "Not Connected" again */

enum { WS_AWAKE, WS_DOWN, WS_ASLEEP, WS_UP, WS_SCAN, WS_RECONNECT, WS_JOIN };
enum { WSQ_DISCONNECT, WSQ_SCAN, WSQ_RECONNECT };
static int ws;
static bool ws_wake_later;	/* woken while still going down */
static bool ws_retry, ws_retry_logged;	/* RECONNECT failed (the interface not back yet): again at ws_at */
static uint32_t ws_at, ws_t0, ws_t_link, ws_t_conn, ws_poll;
static unsigned ws_gen;		/* status_gen / scan_gen when this step began */
static char ws_ssid[33];	/* the network it was on (UI: joining while it reconnects) */
static int ws_freq, ws_id = -1;
static volatile int ws_if = 1;	/* interface thread: -2 = not started, 0 = running, 1 = done, -1 = failed */
static volatile int ws_if_err;
#define WS_IF_TRIES 3		/* wlan0 up again after a failed one (0.5, 1, 2 s apart), then once `wifiup on` */
static int ws_if_tries;		/* this wake's extra ups so far (WS_IF_TRIES + 1 = `wifiup on` used too) */
static uint32_t ws_if_at;	/* the next one (0 = none planned) */
static bool ws_lost_logged;

static void fake_connect(int id);

#ifdef ORCHARD_PHONE
static void *ws_if_thread(void *arg)
{
	bool up = arg != NULL;
	struct ifreq r;
	memset(&r, 0, sizeof(r));
	snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", IFACE);
	int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0), err = 0;
	if (fd < 0 || ioctl(fd, SIOCGIFFLAGS, &r)) {
		err = errno ? errno : EIO;
	} else if (!(r.ifr_flags & IFF_UP) != !up) {
		r.ifr_flags = up ? r.ifr_flags | IFF_UP : r.ifr_flags & ~IFF_UP;
		if (ioctl(fd, SIOCSIFFLAGS, &r))
			err = errno ? errno : EIO;
	}
	if (fd >= 0)
		close(fd);
	ws_if_err = err;
	__atomic_store_n(&ws_if, err ? -1 : 1, __ATOMIC_RELEASE);
	return NULL;
}
#endif

/* wlan0 up / down off the LVGL thread (sdl / snap: only pretends — the PC's interfaces are never touched) */
static void ws_iface(bool up)
{
	ws_if = 0;
#ifdef ORCHARD_PHONE
	pthread_attr_t at;
	pthread_t tid;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&at, 64 << 10);
	if (pthread_create(&tid, &at, ws_if_thread, up ? (void *)1 : NULL)) {
		ws_if_err = EAGAIN;
		ws_if = -1;
	}
	pthread_attr_destroy(&at);
#else
	static int fail = -1;	/* ORCHARD_WIFISLEEP_IFFAIL=<n>: the first n ups fail (test/wifi/sleep.sh) */
	if (fail < 0)
		fail = getenv("ORCHARD_WIFISLEEP_IFFAIL") ? atoi(getenv("ORCHARD_WIFISLEEP_IFFAIL")) : 0;
	if (up && fail > 0) {
		fail--;
		ws_if_err = EIO;
		ws_if = -1;
		return;
	}
	ws_if_err = 0;
	ws_if = 1;
#endif
}

/* wlan0 didn't come up: up again after a backoff (it only flips the flag when it is down) — false = wait for it;
 * true = WS_IF_TRIES used up: once the whole `wifiup on` (modules, link up, power save, the daemons if they died),
 * then go on and let RECONNECT's retries wait for it */
static bool ws_up_again(uint32_t now)
{
	if (ws_if_tries >= WS_IF_TRIES) {
		if (ws_if_tries++ == WS_IF_TRIES && !up_pid) {
			fprintf(stderr, "wifi: sleep: %s still down (%s), falling back to wifiup on\n", IFACE,
				ws_if_err ? strerror(ws_if_err) : "wpa_supplicant: interface disabled");
			spawn_up(true);
		}
		return true;
	}
	if (!ws_if_at) {
		uint32_t d = 500u << ws_if_tries;
		fprintf(stderr, "wifi: sleep: %s up failed (%s), again in %u ms\n", IFACE, strerror(ws_if_err), d);
		ws_if_at = (now + d) | 1;
	} else if ((int32_t)(now - ws_if_at) >= 0) {
		ws_if_at = 0;
		ws_if_tries++;
		ws_iface(true);
	}
	return false;
}

static const char *ws_mark_path(void)
{
	const char *e = getenv("ORCHARD_WIFISLEEP_MARK");
	return e && *e ? e : WS_MARK;
}

static void ws_mark(bool on)
{
	if (fake)
		return;
	if (!on) {
		unlink(ws_mark_path());
		return;
	}
	FILE *f = fopen(ws_mark_path(), "w");
	if (f) {
		fprintf(f, "freq=%d\n", ws_freq);
		fclose(f);
	}
}

static void ws_kick(void)
{
	if (timer) {
		lv_timer_set_period(timer, 20);
		lv_timer_ready(timer);
	}
}

/* the wake is over (connected, given up, or something else took over) */
static void ws_done(const char *how)
{
	if (ws == WS_AWAKE)
		return;
	uint32_t now = lv_tick_get();
	if (ws >= WS_UP)
		fprintf(stderr, "wifi: sleep: awake, %s: link up %d ms, associated %d ms, address %d ms after the wake\n", how,
			ws_t_link ? (int)(ws_t_link - ws_t0) : -1, ws_t_conn ? (int)(ws_t_conn - ws_t0) : -1,
			lk.up && lk.ip[0] ? (int)(now - ws_t0) : -1);
	else
		fprintf(stderr, "wifi: sleep: cancelled (%s)\n", how);
	ws = WS_AWAKE;
	ws_wake_later = false;
	ws_mark(false);
}

static void ws_abort(const char *why)
{
	if (ws >= WS_UP)
		ws_done(why);
}

static void ws_abort_all(const char *why)
{
	if (ws != WS_AWAKE)
		ws_done(why);
}

static void ws_wake_start(void)
{
	ws_t0 = ws_at = lv_tick_get();
	ws_t_link = ws_t_conn = 0;
	ws_wake_later = ws_retry_logged = ws_lost_logged = false;
	ws_if_tries = 0;
	ws_if_at = 0;
	if (fake) {
		ws = WS_JOIN;
		return;
	}
	if (st == WIFI_OFF || st == WIFI_NONE) {	/* the user's off wins: wlan0 stays down */
		ws_done("Wi-Fi is off");
		return;
	}
	ws = WS_UP;
	ws_iface(true);
	ws_kick();
}

/* a Wi-Fi Sleep command waiting or in flight (q[0] while in_flight) — none in WS_RECONNECT without ws_retry = the
 * RECONNECT was dropped: push() without a socket / with a full queue, or ctrl_close() emptied the queue */
static bool ws_queued(void)
{
	for (int i = 0; i < nq; i++)
		if (q[i].kind == Q_WS)
			return true;
	return false;
}

/* (without a socket nothing is queued: WS_RECONNECT pushes it again once ctrl_open() is back) */
static void ws_reconnect(void)
{
	ws = WS_RECONNECT;
	ws_retry = false;
	ws_at = lv_tick_get();
	push(Q_WS, WSQ_RECONNECT, "RECONNECT");
}

static void ws_reply(int step, const char *r)
{
	bool ok = !strncmp(r, "OK", 2);
	switch (step) {
	case WSQ_DISCONNECT:
		if (ws == WS_DOWN)
			ws_iface(false);	/* (a failed DISCONNECT too: the link going down disconnects it) */
		break;
	case WSQ_SCAN:
		if (ws == WS_SCAN && !ok)
			ws_reconnect();	/* no scan on that channel: RECONNECT scans by itself */
		break;
	case WSQ_RECONNECT:
		if (ws != WS_RECONNECT)
			break;
		if (ok) {
			ws = WS_JOIN;
			ws_at = lv_tick_get();
			ws_mark(false);	/* not disconnected any more: nothing left to undo */
		} else {	/* the interface isn't back yet: again in a moment (ws_step) — never given up, or wpa_supplicant
			 * would stay disconnected; after the first 6 s every 5 s (the UI stops showing it at WS_GIVE_UP) */
			if (lv_tick_elaps(ws_t0) >= 6000 && !ws_retry_logged) {
				fprintf(stderr, "wifi: sleep: RECONNECT keeps failing (%.*s), trying every 5 s\n",
					(int)LV_MIN(strcspn(r, "\n"), 20), r);
				ws_retry_logged = true;
			}
			ws_at = lv_tick_get() + (lv_tick_elaps(ws_t0) < 6000 ? 300 : 5000);
			ws_retry = true;
		}
		break;
	}
}

/* one step of the wake (or of going down), from the timer */
static void ws_step(void)
{
	uint32_t now = lv_tick_get();
	int ifs = __atomic_load_n(&ws_if, __ATOMIC_ACQUIRE);
	switch (ws) {
	case WS_DOWN:	/* DISCONNECT's reply starts the interface thread (no reply in 5 s: lost, go on) */
		if (ifs == -2 && lv_tick_elaps(ws_at) > 5000)
			ws_iface(false);
		if (ifs == -2 || ifs == 0)
			break;
		if (ifs < 0)
			fprintf(stderr, "wifi: sleep: %s down failed (%s), disconnected only\n", IFACE, strerror(ws_if_err));
		ws = WS_ASLEEP;
		fprintf(stderr, "wifi: sleep: asleep (%s down)\n", IFACE);
		if (ws_wake_later)
			ws_wake_start();
		break;
	case WS_UP:	/* interface up, then wait (≤ 3 s) for wpa_supplicant to see it: SCAN / RECONNECT fail before */
		if (ifs == 0)
			break;
		if (ifs < 0 && !ws_up_again(now))
			break;	/* the up failed: again in a moment */
		if (!ws_t_link) {
			ws_t_link = now;
			ws_gen = status_gen;
			ws_poll = 0;
		}
		if (fd_c < 0) {	/* (an Orchard that just started: the socket opens a second after the first tick) */
			if (lv_tick_elaps(ws_t0) > 8000) {	/* still none: RECONNECT once there is (it may be the same
								 * wpa_supplicant, still "disconnected"; a new one shrugs) */
				fprintf(stderr, "wifi: sleep: no wpa_supplicant yet, RECONNECT once it is back\n");
				ws_lost_logged = true;
				ws_reconnect();
			}
			break;
		}
		if (status_gen != ws_gen && strcmp(lk.state, "INTERFACE_DISABLED")) {
			if (ws_freq > 0) {
				ws = WS_SCAN;
				ws_at = now;
				ws_gen = scan_gen;
				push(Q_WS, WSQ_SCAN, "SCAN freq=%d", ws_freq);
			} else {
				ws_reconnect();
			}
		} else if (lv_tick_elaps(ws_t_link) > 3000) {
			ws_reconnect();
		} else if (!ws_poll || lv_tick_elaps(ws_poll) >= 100) {
			ws_poll = now;
			push(Q_STATUS, 0, "STATUS");
		}
		break;
	case WS_SCAN:
		if (scan_gen != ws_gen || lv_tick_elaps(ws_at) > 1500)
			ws_reconnect();
		break;
	case WS_RECONNECT:	/* a FAIL reply set ws_at a little ahead: try again then */
		if (fd_c < 0)
			break;	/* the socket went (the queue with it): it reopens (tick), then below */
		if (!ws_retry && !ws_queued()) {	/* RECONNECT was dropped on the way: again */
			if (!ws_lost_logged)
				fprintf(stderr, "wifi: sleep: RECONNECT lost with the control socket, sending it again\n");
			ws_lost_logged = true;
			ws_reconnect();
		} else if (ws_retry && (int32_t)(now - ws_at) >= 0) {
			ws_retry = false;
			/* still "interface disabled" 6 s into the wake: wlan0 may be down (again) — up once more on each
			 * 5 s retry (one SIOCGIFFLAGS when it is up; also after `wifiup on` failed, so a cause that clears
			 * later — rfkill, a firmware hiccup — still ends in a connection), `wifiup on` once after
			 * WS_IF_TRIES of those */
			if (lv_tick_elaps(ws_t0) >= 6000 && ifs != 0) {
				if (ws_if_tries == WS_IF_TRIES) {
					ws_up_again(now);
				} else {
					if (ws_if_tries < WS_IF_TRIES) {
						ws_if_tries++;
						fprintf(stderr, "wifi: sleep: RECONNECT still finds %s disabled, up again (%d/%d)\n",
							IFACE, ws_if_tries, WS_IF_TRIES);
					}
					ws_iface(true);
				}
			}
			ws_reconnect();
		}
		break;
	case WS_JOIN:	/* STATUS a few times a second until the address is there (DHCP), for the UI + the log */
		if (lk.up && !ws_t_conn)
			ws_t_conn = now;
		if (lk.up && lk.ip[0]) {
			ws_done("connected");
		} else if (lv_tick_elaps(ws_t0) > WS_GIVE_UP) {
			ws_done("not connected yet");
		} else if (lv_tick_elaps(ws_poll) >= 250) {
			ws_poll = now;
			push(Q_STATUS, 0, "STATUS");
		}
		break;
	default:
		break;
	}
}

bool wifi_sleep(bool on)
{
	if (!on) {
		if (ws == WS_DOWN)
			ws_wake_later = true;
		else if (ws == WS_ASLEEP)
			ws_wake_start();
		else
			return false;
		return true;
	}
	if (ws != WS_AWAKE || st != WIFI_ON || jn.on || (!fake && (fd_c < 0 || up_pid)))
		return false;
	snprintf(ws_ssid, sizeof(ws_ssid), "%s", lk.up ? lk.ssid : "");
	ws_freq = lk.up ? lk.freq : 0;
	ws_id = lk.up ? lk.id : -1;
	ws_at = lv_tick_get();
	scanning = false;
	if (fake) {
		lk.up = false;
		lk.ip[0] = 0;
		ws = WS_ASLEEP;
		return true;
	}
	ws_mark(true);
	ws = WS_DOWN;
	ws_if = -2;
	ws_if_err = 0;
	push(Q_WS, WSQ_DISCONNECT, "DISCONNECT");
	fprintf(stderr, "wifi: sleep: going to sleep (%s, %d MHz)\n", lk.up ? "connected" : "not connected", ws_freq);
	return true;
}

bool wifi_asleep(void) { return ws != WS_AWAKE; }

bool wifi_sleep_wait(void)
{
	return ws == WS_DOWN || ws == WS_ASLEEP || (ws >= WS_UP && st == WIFI_ON && lv_tick_elaps(ws_t0) < WS_GIVE_UP);
}

bool wifi_reconnecting(void) { return ws >= WS_UP && !lk.up && st == WIFI_ON && lv_tick_elaps(ws_t0) < WS_GIVE_UP; }

/* ================================ preview backend ================================ */

static const struct fake { const char *ssid, *bssid; int freq, sig; enum wifi_sec sec; } fakes[] = {
	{ "HomeNet-5G", "02:00:00:00:00:01", 5180, -51, WIFI_WPA },
	{ "HomeNet", "02:00:00:00:00:02", 2437, -58, WIFI_WPA },
	{ "Neighbor", "02:00:00:00:00:03", 2462, -64, WIFI_WPA23 },
	{ "Upstairs-5G", "02:00:00:00:00:04", 5745, -69, WIFI_WPA },
	{ "Mesh-WPA3", "02:00:00:00:00:05", 5500, -71, WIFI_SAE },
	{ "OpenHotspot", "02:00:00:00:00:06", 2412, -74, WIFI_OPEN },
	{ "CampusNet", "02:00:00:00:00:07", 5200, -77, WIFI_EAP },
	{ "DIRECT-42-Printer", "02:00:00:00:00:08", 2437, -80, WIFI_WPA },
	{ "Caf\xc3\xa9 \xc3\x91" "and\xc3\xba" " Guest", "02:00:00:00:00:09", 2412, -84, WIFI_OPEN },
};
#define NFAKE ((int)(sizeof(fakes) / sizeof(fakes[0])))
static uint32_t fk_scan_end, fk_power_at;
static int fk_next = 3;

static void fake_results(void)
{
	nscan = 0;
	for (int i = 0; i < NFAKE; i++) {
		struct bss *b = &scan[nscan++];
		snprintf(b->ssid, sizeof(b->ssid), "%s", fakes[i].ssid);
		snprintf(b->bssid, sizeof(b->bssid), "%s", fakes[i].bssid);
		b->freq = fakes[i].freq;
		b->sig = fakes[i].sig;
		b->sec = fakes[i].sec;
	}
}

static void fake_connect(int id)
{
	struct saved *s = saved_id(id);
	struct bss *b = s ? scan_find(s->ssid) : NULL;
	if (!b)
		return;
	memset(&lk, 0, sizeof(lk));
	lk.up = true;
	lk.id = id;
	snprintf(lk.state, sizeof(lk.state), "COMPLETED");
	snprintf(lk.ssid, sizeof(lk.ssid), "%s", b->ssid);
	snprintf(lk.bssid, sizeof(lk.bssid), "%s", b->bssid);
	snprintf(lk.ip, sizeof(lk.ip), "198.51.100.37");
	snprintf(lk.mac, sizeof(lk.mac), "02:00:00:00:00:a0");
	snprintf(lk.key_mgmt, sizeof(lk.key_mgmt), "%s", b->sec == WIFI_OPEN ? "NONE" : b->sec == WIFI_SAE ? "SAE" : "WPA2-PSK");
	lk.freq = b->freq;
	lk.rssi = b->sig;
	lk.speed = b->freq > 4000 ? 433 : 72;
}

static void fake_autojoin(void)
{
	for (int k = 0; k < nscan && !lk.up; k++) {	/* strongest saved, enabled network */
		struct saved *s = saved_find(scan[k].ssid);
		if (s && !s->disabled)
			fake_connect(s->id);
	}
}

static void fake_init(void)
{
	const char *e = getenv("ORCHARD_WIFI");
	st = e && !strcmp(e, "none") ? WIFI_NONE : e && !strcmp(e, "off") ? WIFI_OFF : WIFI_ON;
	sv[0] = (struct saved){ 0, "HomeNet-5G", false };
	sv[1] = (struct saved){ 1, "HomeNet", false };
	sv[2] = (struct saved){ 2, "Hotel Lobby", false };
	nsv = 3;
	if (st != WIFI_ON)
		return;
	fake_results();
	if (!(e && !strcmp(e, "idle")))
		fake_connect(0);
}

/* the Wi-Fi page is open and seen: scans + fast STATUS / SIGNAL_POLL (not under the lock screen — they wake the
 * chip; unlock = at once, lock_changed) */
static bool page_up(void) { return want_scan && !shell_locked(); }

static void lock_changed(bool locked)
{
	if (!locked && want_scan && timer)
		lv_timer_ready(timer);
}

static void fake_tick(void)
{
	uint32_t now = lv_tick_get();
	if (ws == WS_JOIN) {	/* woken from Wi-Fi Sleep */
		const char *e = getenv("ORCHARD_WIFI_WAKE_MS");
		if (lv_tick_elaps(ws_t0) >= (uint32_t)(e && *e ? atoi(e) : 1200)) {
			ws_t_link = ws_t_conn = now;
			if (ws_id >= 0)
				fake_connect(ws_id);
			else
				fake_autojoin();
			ws_done("connected");
		}
	}
	if (fk_power_at && (int32_t)(now - fk_power_at) >= 0) {
		fk_power_at = 0;
		st = WIFI_ON;
		fake_results();
		fake_autojoin();
	}
	if (st != WIFI_ON)
		return;
	if (scanning && (int32_t)(now - fk_scan_end) >= 0) {
		scanning = false;
		fake_results();
	}
	if (page_up() && !scanning && !jn.on && lv_tick_elaps(scan_at) > SCAN_EVERY) {
		scan_at = now;
		scanning = true;
		fk_scan_end = now + 1600;
	}
	if (jn.on && lv_tick_elaps(jn.t0) > 1500) {	/* "wrongpass" = the wrong password */
		struct saved *s = saved_find(jn.ssid);
		if (!scan_find(jn.ssid) || (!s && !strcmp(jn.pw, "wrongpass"))) {
			join_fail(scan_find(jn.ssid) ? JF_WRONG_KEY : JF_NOT_FOUND);
			return;
		}
		if (!s && nsv < MAXSAVED) {
			sv[nsv] = (struct saved){ fk_next++, "", false };
			snprintf(sv[nsv].ssid, sizeof(sv[0].ssid), "%s", jn.ssid);
			s = &sv[nsv++];
		}
		if (s) {
			jn.id = s->id;
			fake_connect(s->id);
		}
		join_end(true);
	}
}

/* ================================ public ================================ */

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
	for (const unsigned char *c = p; n--; c++)
		h = (h ^ *c) * 16777619u;
	return h;
}

/* what the lists, the Settings row and the status bar show: a change notifies the listeners */
static uint32_t signature(void)
{
	uint32_t h = 2166136261u;
	int b = wifi_bars();
	bool j = jn.on;
	h = fnv(h, &st, sizeof(st));
	h = fnv(h, &b, sizeof(b));
	h = fnv(h, &scanning, sizeof(scanning));
	h = fnv(h, &j, sizeof(j));
	bool wr = wifi_reconnecting();
	h = fnv(h, &wr, sizeof(wr));
	h = fnv(h, lk.ssid, strlen(lk.ssid) + 1);
	h = fnv(h, lk.ip, strlen(lk.ip) + 1);
	h = fnv(h, jn.ssid, j ? strlen(jn.ssid) : 0);
	for (int i = 0; i < nscan; i++) {
		int k = bars_of(scan[i].sig);
		h = fnv(h, scan[i].ssid, strlen(scan[i].ssid) + 1);
		h = fnv(h, &k, sizeof(k));
		h = fnv(h, &scan[i].sec, sizeof(scan[i].sec));
	}
	for (int i = 0; i < nsv; i++) {
		h = fnv(h, &sv[i].id, sizeof(sv[i].id));
		h = fnv(h, sv[i].ssid, strlen(sv[i].ssid) + 1);
		h = fnv(h, &sv[i].disabled, sizeof(sv[i].disabled));
	}
	return h;
}

static void tick(lv_timer_t *t)
{
	uint32_t now = lv_tick_get();
	if (fake) {
		fake_tick();
	} else {
		reap();
		update_state();
		if (fd_c < 0 && st == WIFI_STARTING && lv_tick_elaps(open_at) > 1000) {
			open_at = now;
			if (ctrl_open())
				st = WIFI_ON;
		}
		if (fd_c >= 0)
			pump();
		ws_step();
		if (fd_c >= 0 && nq && !in_flight)
			pump();	/* what that step asked for goes now */
		if (fd_c >= 0) {
			/* screen off: wpa_supplicant's events still come in (the m socket), but no STATUS / SIGNAL_POLL /
			 * scans — each SIGNAL_POLL is a firmware round trip that wakes the Wi-Fi chip and the PCIe link */
			if (fd_c >= 0 && !power_dark()) {
				/* the m socket brings connects / disconnects at once: STATUS is the backstop, SIGNAL_POLL (a
				 * firmware round trip) the status bar's bars — fast only while the Wi-Fi page is open */
				bool page = page_up();
				if (lv_tick_elaps(status_at) > (page ? 3000u : 10000u)) {
					status_at = now;
					push(Q_STATUS, 0, "STATUS");
				}
				if (lk.up && lv_tick_elaps(signal_at) > (page ? 4000u : power_lpm() ? 30000u : 10000u)) {
					signal_at = now;
					push(Q_SIGNAL, 0, "SIGNAL_POLL");
				}
				if (page && !jn.on && ws == WS_AWAKE && lv_tick_elaps(scan_at) > SCAN_EVERY) {
					scan_at = now;
					push(Q_SCAN, 0, "SCAN");
				}
				if (results_at && (int32_t)(now - results_at) >= 0) {
					results_at = 0;
					scanning = false;
					push(Q_RESULTS, 0, "SCAN_RESULTS");
				}
			}
		}
		if (fd_c < 0 && st == WIFI_ON)
			st = WIFI_STARTING;
	}
	if (jn.on && lv_tick_elaps(jn.t0) > JOIN_TIMEOUT)
		join_fail(JF_GENERIC);
	uint32_t sig = signature();
	if (sig != last_sig) {
		last_sig = sig;
		for (int i = 0; i < MAXL; i++)
			if (listeners[i])
				listeners[i]();
	}
	/* idle: events wait in the m socket; 5 a second only while the Wi-Fi page is open or a join runs; Wi-Fi Sleep:
	 * 20 a second while it goes down / wakes, every 30 s while it sleeps (nothing comes; the wake kicks it) */
	bool ws_busy = ws == WS_DOWN || (ws >= WS_UP && lv_tick_elaps(ws_t0) < WS_GIVE_UP);
	lv_timer_set_period(t, in_flight || nq ? 20 : ws_busy ? 50 : !power_dark() ? (page_up() || jn.on ? 200 : 1000)
			       : ws == WS_ASLEEP ? 30000 : 5000);
}

static void unlink_socks(void)
{
	if (lp_c[0])
		unlink(lp_c);
	if (lp_m[0])
		unlink(lp_m);
}

void wifi_init(void)
{
#ifdef ORCHARD_PHONE
	fake = false;
#else
	const char *e = getenv("ORCHARD_WIFI");
	fake = !(e && !strcmp(e, "real"));
#endif
	if (fake) {
		fake_init();
	} else {
		update_state();
		atexit(unlink_socks);
		FILE *f = fopen(ws_mark_path(), "r");	/* Orchard stopped while Wi-Fi slept: wake it */
		if (f) {
			if (fscanf(f, "freq=%d", &ws_freq) != 1)
				ws_freq = 0;
			fclose(f);
			fprintf(stderr, "wifi: sleep: %s found: waking Wi-Fi\n", ws_mark_path());
			ws_wake_start();
		}
	}
	last_sig = ~signature();	/* the first tick tells the status bar */
	timer = lv_timer_create(tick, 200, NULL);
	power_keep(timer, 0, "wifi");	/* screen off: it sets its own slow pace (tick) */
	lock_listen(lock_changed, true);
}

enum wifi_state wifi_state(void) { return st; }

uint32_t wifi_starting_ms(void) { return st == WIFI_STARTING ? lv_tick_elaps(start_at) : 0; }

bool wifi_scanning(void) { return st == WIFI_ON && scanning; }

void wifi_scan(bool on)
{
	if (on && !want_scan)
		scan_at = lv_tick_get() - SCAN_EVERY;	/* one right away */
	want_scan = on;
}

void wifi_set_power(bool on)
{
	if (st == WIFI_NONE || (on ? st != WIFI_OFF : st == WIFI_OFF))
		return;
	ws_done(on ? "Wi-Fi turned on" : "Wi-Fi turned off");	/* (off: wifiup stops wpa_supplicant, link down) */
	if (on)
		start_at = lv_tick_get();
	if (!on) {
		join_end(false);
		if (!fake)
			ctrl_close();
		lk.up = false;
		lk.ssid[0] = 0;
		nscan = 0;
		scanning = false;
	}
	st = on ? WIFI_STARTING : WIFI_OFF;
	if (fake) {
		fk_power_at = on ? lv_tick_get() + 900 : 0;
		return;
	}
	if (up_pid)
		up_next = on;
	else
		spawn_up(on);
	up_want = on;
	check_at = lv_tick_get();
}

const char *wifi_status(void)
{
	static char s[100];
	switch (st) {
	case WIFI_NONE: return "Not Available";
	case WIFI_OFF: return "Off";
	case WIFI_STARTING: return "Not Connected";
	default: break;
	}
	if (!lk.up && !(wifi_reconnecting() && ws_ssid[0]))
		return "Not Connected";
	display_name(lk.up ? lk.ssid : ws_ssid, s, sizeof(s));	/* (woken from Wi-Fi Sleep: the network it rejoins) */
	return s;
}

int wifi_bars(void)
{
	if (st != WIFI_ON || !lk.up)
		return 0;
	if (lk.rssi)
		return bars_of(lk.rssi);
	struct bss *b = scan_find(lk.ssid);
	return b ? bars_of(b->sig) : 3;
}

static void fill(wifi_net_t *n, const char *ssid)
{
	struct bss *b = scan_find(ssid);
	struct saved *s = saved_find(ssid);
	memset(n, 0, sizeof(*n));
	snprintf(n->ssid, sizeof(n->ssid), "%s", ssid);
	display_name(ssid, n->name, sizeof(n->name));
	n->current = lk.up && !strcmp(lk.ssid, ssid);
	n->joining = (jn.on && !strcmp(jn.ssid, ssid)) || (wifi_reconnecting() && !strcmp(ws_ssid, ssid));
	if (b) {
		n->signal = b->sig;
		n->freq = b->freq;
		n->sec = b->sec;
	} else if (n->joining) {
		n->sec = jn.sec;
	}
	if (n->current) {
		if (lk.rssi)
			n->signal = lk.rssi;
		if (lk.freq)
			n->freq = lk.freq;
		if (!n->signal)
			n->signal = -55;
		if (!b)
			n->sec = !strcmp(lk.key_mgmt, "NONE") ? WIFI_OPEN : strstr(lk.key_mgmt, "SAE") ? WIFI_SAE : WIFI_WPA;
	}
	n->bars = bars_of(n->signal);
	n->saved = s != NULL;
	n->autojoin = s && !s->disabled;
}

static int rank(const wifi_net_t *n) { return n->current || n->joining ? 0 : n->saved && n->bars ? 1 : n->bars ? 2 : 3; }

static int net_cmp(const void *a, const void *b)
{
	const wifi_net_t *x = a, *y = b;
	if (rank(x) != rank(y))
		return rank(x) - rank(y);
	if (x->signal != y->signal)
		return y->signal - x->signal;
	return strcasecmp(x->name, y->name);
}

int wifi_networks(wifi_net_t *out, int max)
{
	wifi_net_t all[MAXSCAN + MAXSAVED + 3];
	int n = 0;
	if (st != WIFI_ON)
		return 0;
	for (int i = 0; i < nscan; i++)
		fill(&all[n++], scan[i].ssid);
	for (int i = 0; i < nsv; i++)
		if (!scan_find(sv[i].ssid))
			fill(&all[n++], sv[i].ssid);
	if (lk.up && !scan_find(lk.ssid) && !saved_find(lk.ssid))
		fill(&all[n++], lk.ssid);
	if (jn.on && !scan_find(jn.ssid) && !saved_find(jn.ssid) && (!lk.up || strcmp(lk.ssid, jn.ssid)))
		fill(&all[n++], jn.ssid);	/* a hidden network being joined */
	if (wifi_reconnecting() && ws_ssid[0] && !scan_find(ws_ssid) && !saved_find(ws_ssid) && !(jn.on && !strcmp(jn.ssid, ws_ssid)))
		fill(&all[n++], ws_ssid);	/* the one it rejoins after Wi-Fi Sleep */
	qsort(all, n, sizeof(all[0]), net_cmp);
	if (n > max)
		n = max;
	memcpy(out, all, n * sizeof(all[0]));
	return n;
}

bool wifi_network(const char *ssid, wifi_net_t *out)
{
	if (st != WIFI_ON || (!scan_find(ssid) && !saved_find(ssid) && strcmp(lk.ssid, ssid) && !(jn.on && !strcmp(jn.ssid, ssid)) &&
			      !(wifi_reconnecting() && !strcmp(ws_ssid, ssid))))
		return false;
	fill(out, ssid);
	return true;
}

static void ipv4(uint32_t be, char *out, int n)
{
	struct in_addr a = { .s_addr = be };
	if (!inet_ntop(AF_INET, &a, out, n))
		out[0] = 0;
}

bool wifi_link(wifi_link_t *o)
{
	memset(o, 0, sizeof(*o));
	if (st != WIFI_ON || !lk.up)
		return false;
	wifi_net_t n;
	fill(&n, lk.ssid);
	o->up = true;
	snprintf(o->ssid, sizeof(o->ssid), "%s", lk.ssid);
	snprintf(o->name, sizeof(o->name), "%s", n.name);
	snprintf(o->ip, sizeof(o->ip), "%s", lk.ip);
	snprintf(o->bssid, sizeof(o->bssid), "%s", lk.bssid);
	snprintf(o->mac, sizeof(o->mac), "%s", lk.mac);
	o->sec = n.sec;
	o->rssi = n.signal;
	o->bars = n.bars;
	o->freq = n.freq;
	o->speed = lk.speed;
	if (fake) {
		snprintf(o->mask, sizeof(o->mask), "255.255.255.0");
		snprintf(o->router, sizeof(o->router), "198.51.100.1");
		snprintf(o->dns, sizeof(o->dns), "198.51.100.1");
		return true;
	}
	/* router + mask: the kernel's routes for wlan0 (udhcpc put them there) */
	char l[256];
	FILE *f = fopen("/proc/net/route", "r");
	while (f && fgets(l, sizeof(l), f)) {
		char ifn[32];
		unsigned dst, gw, flags, mask;
		int refc, use, metric;
		if (sscanf(l, "%31s %x %x %x %d %d %d %x", ifn, &dst, &gw, &flags, &refc, &use, &metric, &mask) != 8 ||
		    strcmp(ifn, IFACE))
			continue;
		if (dst == 0 && gw && !o->router[0])
			ipv4(gw, o->router, sizeof(o->router));
		else if (dst && mask && !o->mask[0])
			ipv4(mask, o->mask, sizeof(o->mask));
	}
	if (f)
		fclose(f);
	f = fopen("/etc/resolv.conf", "r");
	while (f && fgets(l, sizeof(l), f)) {
		char ns[48];
		if (sscanf(l, "nameserver %47s", ns) != 1 || strlen(o->dns) + strlen(ns) + 3 > sizeof(o->dns))
			continue;
		if (o->dns[0])
			strcat(o->dns, ", ");
		strcat(o->dns, ns);
	}
	if (f)
		fclose(f);
	return true;
}

int wifi_channel(int freq)
{
	if (freq == 2484)
		return 14;
	if (freq >= 2412 && freq < 2484)
		return (freq - 2407) / 5;
	if (freq >= 5955)
		return (freq - 5950) / 5;
	if (freq >= 5000)
		return (freq - 5000) / 5;
	return 0;
}

void wifi_listen(void (*cb)(void), bool on)
{
	for (int i = 0; i < MAXL; i++)
		if (listeners[i] == cb)
			listeners[i] = NULL;
	if (!on)
		return;
	for (int i = 0; i < MAXL; i++)
		if (!listeners[i]) {
			listeners[i] = cb;
			return;
		}
}

/* ================================ the Wi-Fi symbol ================================ */

/* iOS's fan: a wedge and two bands, the ones above the signal level dimmed. Drawn (not a glyph):
 * the Nerd Font has no 3-level iOS-style Wi-Fi symbol. Colour = the object's text colour. */
static void icon_draw(lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_current_target(e);
	lv_area_t a;
	lv_obj_get_coords(o, &a);
	int w = lv_area_get_width(&a), bars = (intptr_t)lv_obj_get_user_data(o);
	int R = w * 60 / 100, t = R * 21 / 100 + 1, g = R * 12 / 100 + 1;
	lv_draw_arc_dsc_t d;
	lv_draw_arc_dsc_init(&d);
	d.color = lv_obj_get_style_text_color(o, 0);
	d.center.x = a.x1 + w / 2;
	d.center.y = a.y1 + R + 1;
	d.start_angle = 225;
	d.end_angle = 315;
	d.rounded = 1;
	int r = R;
	for (int i = 3; i >= 1; i--) {
		d.radius = r;
		d.width = i == 1 ? r : t;
		d.rounded = i > 1;	/* the wedge keeps its point */
		d.opa = bars >= i ? LV_OPA_COVER : LV_OPA_30;
		lv_draw_arc(lv_event_get_layer(e), &d);
		r -= t + g;
	}
}

lv_obj_t *wifi_icon(lv_obj_t *parent, int w)
{
	lv_obj_t *o = box(parent);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(o, w, w * 60 / 100 + 3);
	lv_obj_set_user_data(o, (void *)(intptr_t)3);
	lv_obj_add_event_cb(o, icon_draw, LV_EVENT_DRAW_MAIN, NULL);
	return o;
}

void wifi_icon_set(lv_obj_t *o, int bars)
{
	if ((intptr_t)lv_obj_get_user_data(o) == bars)
		return;
	lv_obj_set_user_data(o, (void *)(intptr_t)bars);
	lv_obj_invalidate(o);
}
