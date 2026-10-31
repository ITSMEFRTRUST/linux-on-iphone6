/* Bluetooth: BlueZ through ONE long-lived `bluetoothctl --agent DisplayYesNo` (stdin = socket, stdout
 * = pipe) owned by a worker thread. It parses the output — [NEW]/[CHG]/[DEL] events, `show`/`info`
 * blocks, command results, agent prompts — into a device table and runs one operation at a time
 * (pair → trust → connect, with timeouts + a retry). The LVGL thread only reads the table under a
 * mutex; a 150 ms timer delivers changes, errors and pairing requests (ui_alert) to the UI.
 * A2DP: once BlueALSA has a PCM for a connected sink, /etc/asound.conf's default PCM is pointed at it
 * (the speaker config is saved and restored on disconnect), Music restarts its output there and the
 * volume controls switch to the device's BlueALSA mixer.
 * Off-phone (snap/sdl) a fake backend with a few devices previews the UI. */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "orchard.h"

#define MAXDEV 40
#define MAXL 6
#define ALIAS "iPhone 6"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------- shared state (under mu) ---------- */

/* the Bluetooth page can be seen: not under the lock screen, screen on (the UI side keeps it, seen_check). Discovery
 * runs only then: an inquiry keeps the radio busy and each device it finds rebuilds a page nobody sees */
static bool seen = true;

static struct dev {
	bt_device_t d;
	bool info_asked, aliased;	/* aliased: has an alias of its own (a [CHG] Name doesn't replace it) */
	int order;
	long conn_at;	/* when it connected: the newest audio device gets the sound */
	long info_at, info_t;	/* `info` not before info_at; last asked at info_t (battery level) */
} devs[MAXDEV];
static int ndev, order_ctr;
static bool avail, powered, discovering, changed;
static char status_txt[96] = "Starting\xe2\x80\xa6";
static char audio_name[64];

/* worker → UI */
static bool err_pending;
static char err_title[64], err_msg[240];
static struct { bool pending; int id, kind; char code[16], name[64]; } ask;
static bool route_pending, route_keep;
static char route_dev[48], route_ctl[96];
static int route_pct = -1;

static void (*listeners[MAXL])(void);

static struct dev *find(const char *addr)
{
	for (int i = 0; i < ndev; i++)
		if (!strcasecmp(devs[i].d.addr, addr))
			return &devs[i];
	return NULL;
}

static void drop(struct dev *v)
{
	int i = v - devs;
	memmove(devs + i, devs + i + 1, (ndev - i - 1) * sizeof(devs[0]));
	ndev--;
}

static struct dev *get(const char *addr)
{
	struct dev *v = find(addr);
	if (v)
		return v;
	if (ndev == MAXDEV) {	/* evict the oldest stranger */
		for (int i = 0; i < ndev; i++)
			if (!devs[i].d.paired && !devs[i].d.connected && !devs[i].d.busy) {
				drop(&devs[i]);
				break;
			}
		if (ndev == MAXDEV)
			return NULL;
	}
	v = &devs[ndev++];
	memset(v, 0, sizeof(*v));
	snprintf(v->d.addr, sizeof(v->d.addr), "%s", addr);
	snprintf(v->d.name, sizeof(v->d.name), "%s", addr);
	v->d.battery = -1;
	v->order = ++order_ctr;
	return v;
}

/* ================================ phone backend ================================ */
#ifdef ORCHARD_PHONE

static long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000L + ts.tv_nsec / 1000000;
}

/* "AA:BB:..." → "AA-BB-..." is what BlueZ uses as the alias of a nameless device */
static bool is_addr_alias(const char *addr, const char *name)
{
	if (strlen(name) != 17)
		return false;
	for (int i = 0; i < 17; i++)
		if ((addr[i] == ':' ? '-' : addr[i]) != name[i])
			return false;
	return true;
}

static void set_name(struct dev *v, const char *name, bool real)
{
	if (!*name)
		return;
	if (real || !is_addr_alias(v->d.addr, name)) {
		if (!v->d.named || strcmp(v->d.name, name))
			changed = true;
		v->d.named = true;
		snprintf(v->d.name, sizeof(v->d.name), "%s", name);
	}
}

/* device type: BlueZ's Icon property first, else the Class of Device */
static void set_icon(struct dev *v, const char *icon)
{
	enum bt_kind k = BT_OTHER;
	if (!strcmp(icon, "audio-headphones") || !strcmp(icon, "audio-headset"))
		k = BT_HEADPHONES;
	else if (!strcmp(icon, "audio-card"))
		k = BT_SPEAKER;
	else if (!strcmp(icon, "computer"))
		k = BT_COMPUTER;
	else if (!strcmp(icon, "phone"))
		k = BT_PHONE;
	else if (!strncmp(icon, "input-", 6))
		k = BT_INPUT;
	else if (!strcmp(icon, "video-display") || !strcmp(icon, "multimedia-player"))
		k = BT_TV;
	if (!strncmp(icon, "audio-", 6) && strcmp(icon, "audio-input-microphone"))
		v->d.audio = true;
	if (v->d.kind != k)
		changed = true;
	v->d.kind = k;
}

static void set_class(struct dev *v, unsigned cls)
{
	unsigned major = cls >> 8 & 0x1f, minor = cls >> 2 & 0x3f;
	enum bt_kind k = v->d.kind;
	if (k != BT_OTHER)
		return;	/* the icon already said */
	switch (major) {
	case 1: k = BT_COMPUTER; break;
	case 2: k = BT_PHONE; break;
	case 5: k = BT_INPUT; break;
	case 7: k = BT_WATCH; break;
	case 4:
		if (minor == 1 || minor == 2 || minor == 6)
			k = BT_HEADPHONES;
		else if (minor == 0x0f || minor == 0x10)
			k = BT_TV;
		else
			k = BT_SPEAKER;
		if (minor != 4 && minor < 0x0b)	/* not a microphone / camera / display */
			v->d.audio = true;
		break;
	}
	if (v->d.kind != k)
		changed = true;
	v->d.kind = k;
}

#define BTCTL "/usr/bin/bluetoothctl"
#define BACLI "/usr/bin/bluealsa-cli"
#define AMIXER "/usr/bin/amixer"
#ifndef ASOUND	/* overridable for a test harness */
#define ASOUND "/etc/asound.conf"
#define ASOUND_SAVE "/tmp/orchard-asound-speaker.conf"
#endif
#define MARK "# orchard-bt:"

/* tools/alsa.tgz's /etc/asound.conf: used when the saved copy is gone */
static const char speaker_conf[] =
	"# iPhone 6 speaker: the MCA takes only S16_LE 48 kHz mono -> convert everything, downmix stereo\n"
	"pcm.!default {\n\ttype plug\n\tslave {\n\t\tpcm \"hw:0,0\"\n\t\tformat S16_LE\n\t\trate 48000\n"
	"\t\tchannels 1\n\t}\n\tttable.0.0 0.5\n\tttable.1.0 0.5\n}\nctl.!default {\n\ttype hw\n\tcard 0\n}\n";

#define DBUS_SEND "/usr/bin/dbus-send"

enum { R_POWER_ON = 1, R_POWER_OFF, R_SCAN_ON, R_SCAN_OFF, R_CONNECT, R_DISCONNECT, R_FORGET, R_ANSWER, R_RENAME };
static struct req { int op, arg; char addr[18], name[64]; } reqs[16];
static int nreq;
static int wake_fd[2] = { -1, -1 };

/* worker-only */
static pid_t cpid;
static int c_in = -1, c_out = -1;
static char rbuf[8192];
static int rlen;
static long next_spawn, show_at, last_show;
static bool ready, have_ctl, want_scan, scan_sent, agent_ok;
static char ctl_alias[64];
static char prompt_dev[64];	/* bluetoothctl's default device, as its prompt names it ("[name]> ") */
enum { CTX_NONE, CTX_CTL, CTX_DEV };
static int ctx;
static char ctx_addr[18];

enum { OP_NONE, OP_CONNECT, OP_DISCONNECT, OP_FORGET };
enum { S_PAIR = 1, S_TRUST, S_CONNECT, S_WAIT, S_DONE };
static struct { int kind, step, tries; char addr[18]; long deadline, retry_at; } op;
static struct req pending_ops[8];
static int npending;

enum { AG_CONFIRM = 1, AG_AUTHORIZE, AG_SERVICE, AG_PIN, AG_PASSKEY };
static struct { int kind, id; bool answered, user; long at; char code[16]; } ag;
static int ag_ids;
static char last_conn[18];	/* last device that connected (who an incoming agent request is from) */

static char routed[18], route_wait[18], route_failed[18];
static long route_since, route_poll;

static void cmd(const char *fmt, ...)
{
	char b[200];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof(b) - 1, fmt, ap);
	va_end(ap);
	if (c_in < 0 || n <= 0)
		return;
	b[n++] = '\n';
	if (send(c_in, b, n, MSG_NOSIGNAL) != n)
		fprintf(stderr, "bt: write to bluetoothctl failed\n");
}

static void post_error(const char *title, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	snprintf(err_title, sizeof(err_title), "%s", title);
	vsnprintf(err_msg, sizeof(err_msg), fmt, ap);
	va_end(ap);
	err_pending = true;
}

static const char *dev_name(const char *addr)
{
	struct dev *v = find(addr);
	return v ? v->d.name : addr;
}

/* org.bluez.Error.* → what iOS would say */
static const char *friendly(const char *err)
{
	if (strstr(err, "AuthenticationRejected") || strstr(err, "AuthenticationFailed"))
		return "Pairing was rejected. Put \xe2\x80\x9c%s\xe2\x80\x9d in pairing mode and try again.";
	if (strstr(err, "AuthenticationCanceled"))
		return "Pairing with \xe2\x80\x9c%s\xe2\x80\x9d was cancelled.";
	if (strstr(err, "profile-unavailable"))
		return "\xe2\x80\x9c%s\xe2\x80\x9d has no profile Orchard can use (A2DP audio needs bluealsa running).";
	if (strstr(err, "NotAvailable") || strstr(err, "DoesNotExist"))
		return "\xe2\x80\x9c%s\xe2\x80\x9d is out of range. Turn it on and try again.";
	if (strstr(err, "Passkey"))
		return "\xe2\x80\x9c%s\xe2\x80\x9d wants a passkey typed in, which Orchard can't do yet.";
	if (strstr(err, "Timeout") || strstr(err, "ConnectionAttemptFailed") || strstr(err, "page-timeout"))
		return "Couldn't reach \xe2\x80\x9c%s\xe2\x80\x9d. Make sure it's turned on, close by, and in pairing mode.";
	return "Make sure \xe2\x80\x9c%s\xe2\x80\x9d is turned on, in range, and is ready to pair.";
}

static bool retryable(const char *err)
{
	return strstr(err, "ConnectionAttemptFailed") || strstr(err, "page-timeout") || strstr(err, "Page Timeout") ||
	       strstr(err, "Host is down") || strstr(err, "InProgress") || strstr(err, "Error.Failed") ||
	       strstr(err, "create-socket") || strstr(err, "le-connection-abort") || !strcmp(err, "Timeout");
}

/* ---------- operations ---------- */

static void set_busy(const char *addr, enum bt_busy b)
{
	struct dev *v = find(addr);
	if (v && v->d.busy != b) {
		v->d.busy = b;
		changed = true;
	}
}

static void op_end(void)
{
	set_busy(op.addr, BT_IDLE);
	op.kind = OP_NONE;
}

static void op_fail(const char *err)
{
	bool pairing = op.step == S_PAIR || op.step == S_TRUST;
	fprintf(stderr, "bt: %s %s failed: %s\n", pairing ? "pair" : "connect", op.addr, err);
	if (op.kind == OP_CONNECT && retryable(err) && op.tries < 2) {
		op.tries++;
		op.retry_at = now_ms() + 1500;	/* step() resends the same step */
		op.deadline = 0;
		return;
	}
	if (op.kind == OP_CONNECT)
		post_error(pairing ? "Pairing Unsuccessful" : "Connection Unsuccessful", friendly(err), dev_name(op.addr));
	else if (op.kind == OP_DISCONNECT)
		post_error("Couldn't Disconnect", "%s", err);
	else
		post_error("Couldn't Forget Device", "%s", err);
	op_end();
}

static void op_send(void)
{
	long now = now_ms();
	op.retry_at = 0;
	switch (op.step) {
	case S_PAIR:
		set_busy(op.addr, BT_PAIRING);
		cmd("pair %s", op.addr);
		op.deadline = now + 45000;	/* the remote may want a button press */
		break;
	case S_TRUST:
		cmd("trust %s", op.addr);
		op.deadline = now + 6000;
		break;
	case S_CONNECT:
		set_busy(op.addr, BT_CONNECTING);
		cmd("connect %s", op.addr);
		op.deadline = now + 25000;
		break;
	}
}

static void op_start(const struct req *r)
{
	struct dev *v = find(r->addr);
	memset(&op, 0, sizeof(op));
	snprintf(op.addr, sizeof(op.addr), "%s", r->addr);
	if (!v && r->op != R_FORGET) {
		post_error("Connection Unsuccessful", "The device went out of range.");
		return;
	}
	if (r->op == R_CONNECT) {
		if (v->d.connected) {
			set_busy(op.addr, BT_IDLE);
			return;
		}
		op.kind = OP_CONNECT;
		op.step = v->d.paired ? S_CONNECT : S_PAIR;
		op_send();
	} else if (r->op == R_DISCONNECT) {
		op.kind = OP_DISCONNECT;
		op.step = S_WAIT;
		set_busy(op.addr, BT_DISCONNECTING);
		cmd("disconnect %s", op.addr);
		op.deadline = now_ms() + 10000;
	} else {
		op.kind = OP_FORGET;
		op.step = S_WAIT;
		set_busy(op.addr, BT_REMOVING);
		cmd("remove %s", op.addr);
		op.deadline = now_ms() + 10000;
	}
}

static void op_next(int step)
{
	if (op.kind != OP_CONNECT || op.step >= step)
		return;
	op.step = step;
	op.tries = 0;
	op_send();
}

/* ---------- agent prompts ---------- */

static void agent_answer(const char *s)
{
	cmd("%s", s);
	ag.answered = true;
	ag.user = false;
}

static void agent_prompt(const char *p)
{
	int kind = strstr(p, "Confirm passkey") ? AG_CONFIRM : strstr(p, "Accept pairing") ? AG_AUTHORIZE :
		   strstr(p, "Authorize service") ? AG_SERVICE : strstr(p, "PIN code:") && strstr(p, "Enter") ? AG_PIN :
		   strstr(p, "Enter passkey") ? AG_PASSKEY : 0;
	if (!kind) {	/* display-only: "[agent] PIN code: 1234" / "[agent] Passkey: 123456" */
		const char *c = strstr(p, "PIN code: ");
		c = c ? c + 10 : (c = strstr(p, "Passkey: ")) ? c + 9 : NULL;
		if (c)
			post_error("Bluetooth Pairing", "Type %s on \xe2\x80\x9c%s\xe2\x80\x9d, then press Enter.", c,
				   dev_name(op.kind ? op.addr : last_conn));
		return;
	}
	if (ag.kind && ag.answered && kind == ag.kind && now_ms() - ag.at < 3000)
		return;	/* the same prompt redrawn after we answered */
	if (ag.kind != kind || ag.answered) {	/* missed the announcement line */
		memset(&ag, 0, sizeof(ag));
		ag.kind = kind;
		ag.at = now_ms();
	}
	if (ag.user)
		return;	/* waiting for the alert */
	bool ours = op.kind == OP_CONNECT;
	const char *c = strstr(p, "passkey ");
	if (kind == AG_CONFIRM && c)
		snprintf(ag.code, sizeof(ag.code), "%.6s", c + 8);
	switch (kind) {
	case AG_CONFIRM:
	case AG_AUTHORIZE:
		if (ours) {
			agent_answer("yes");	/* we started it: headsets want a plain yes */
		} else {
			ag.user = true;
			ag.id = ++ag_ids;
			ask.pending = true;
			ask.id = ag.id;
			ask.kind = kind;
			snprintf(ask.code, sizeof(ask.code), "%s", ag.code);
			snprintf(ask.name, sizeof(ask.name), "%s", last_conn[0] ? dev_name(last_conn) : "A device");
		}
		break;
	case AG_SERVICE: {
		struct dev *v = find(last_conn);
		agent_answer(ours || (v && v->d.paired) ? "yes" : "no");
		break;
	}
	case AG_PIN:
		agent_answer(ours ? "0000" : "");	/* legacy (pre-2.1) headsets use 0000 */
		break;
	case AG_PASSKEY:
		agent_answer("0");
		if (ours)
			post_error("Pairing Unsuccessful", friendly("Passkey"), dev_name(op.addr));
		break;
	}
}

/* ---------- output parser ---------- */

static void dev_prop(struct dev *v, const char *k, const char *val)
{
	bool yes = !strncmp(val, "yes", 3);
	if (!strcmp(k, "Name")) {
		if (!v->aliased)
			set_name(v, val, true);
	} else if (!strcmp(k, "Alias")) {
		set_name(v, val, false);
		v->aliased = !is_addr_alias(v->d.addr, val);
	} else if (!strcmp(k, "Paired")) {
		if (v->d.paired != yes)
			changed = true;
		v->d.paired = yes;
		if (yes && op.kind == OP_CONNECT && !strcasecmp(op.addr, v->d.addr) && op.step == S_PAIR)
			op_next(S_TRUST);
	} else if (!strcmp(k, "Connected")) {
		bool was = v->d.connected;
		if (was != yes)
			changed = true;
		v->d.connected = yes;
		/* every `info` reply repeats "Connected: yes": act on the change only, or the 3 s battery
		 * re-read loops and the newest-connection audio route keeps moving */
		if (yes && !was) {
			v->conn_at = now_ms();
			snprintf(last_conn, sizeof(last_conn), "%s", v->d.addr);
			v->info_asked = false;	/* again once its profiles are up: the battery level shows then */
			v->info_at = now_ms() + 3000;
		} else if (!yes) {
			if (was) {
				if (v->d.battery >= 0)
					changed = true;
				v->d.battery = -1;
				if (!strcasecmp(route_failed, v->d.addr))
					route_failed[0] = 0;
			}
			if (op.kind == OP_DISCONNECT && !strcasecmp(op.addr, v->d.addr))
				op_end();
		}
	} else if (!strcmp(k, "Battery Percentage")) {	/* "0x50 (80)" */
		const char *p = strchr(val, '(');
		int b = p ? atoi(p + 1) : (int)strtol(val, NULL, 0);
		if (b >= 0 && b <= 100 && b != v->d.battery) {
			v->d.battery = b;
			changed = true;
		}
	} else if (!strcmp(k, "RSSI")) {
		const char *p = strchr(val, '(');
		v->d.rssi = p ? atoi(p + 1) : atoi(val);
	} else if (!strcmp(k, "Icon")) {
		set_icon(v, val);
	} else if (!strcmp(k, "Class")) {
		set_class(v, strtoul(val, NULL, 0));
	} else if (!strcmp(k, "UUID") || !strcmp(k, "UUIDs")) {
		if (strstr(val, "0000110b-") && !v->d.audio) {	/* Audio Sink */
			v->d.audio = true;
			changed = true;
		}
	}
}

static void ctl_prop(const char *k, const char *val)
{
	bool yes = !strncmp(val, "yes", 3);
	if (!strcmp(k, "Powered")) {
		if (powered != yes)
			changed = true;
		powered = yes;
		if (!yes)
			scan_sent = false;	/* power off ends discovery */
	} else if (!strcmp(k, "Discovering")) {
		if (discovering != yes)
			changed = true;
		discovering = yes;
	} else if (!strcmp(k, "Alias")) {
		snprintf(ctl_alias, sizeof(ctl_alias), "%s", val);
	}
}

/* "Key: value" → k, val (in place) */
static bool split_kv(char *s, char **k, char **val)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char *c = strstr(s, ": ");
	if (!c) {
		if ((c = strstr(s, " is nil"))) {	/* "RSSI is nil" */
			*c = 0;
			*k = s;
			*val = "";
			return true;
		}
		return false;
	}
	*c = 0;
	*k = s;
	*val = c + 2;
	return true;
}

static bool is_addr(const char *s)
{
	for (int i = 0; i < 17; i++)
		if (i % 3 == 2 ? s[i] != ':' : !strchr("0123456789ABCDEFabcdef", s[i]))
			return false;
	return s[17] == 0 || s[17] == ' ';
}

static void controller_seen(void)
{
	if (!have_ctl)
		changed = true;
	have_ctl = true;
	snprintf(status_txt, sizeof(status_txt), "On");
}

static bool debug;	/* ORCHARD_BT_DEBUG=1: bluetoothctl's output on stderr */

static void line(char *l)
{
	if (debug)
		fprintf(stderr, "btctl| %s\n", l);
	/* readline prompts in front: "[bluetoothctl]> " or "[Speaker]> " */
	while (l[0] == '[' && strncmp(l, "[NEW]", 5) && strncmp(l, "[CHG]", 5) && strncmp(l, "[DEL]", 5) &&
	       strncmp(l, "[agent]", 7)) {
		char *e = strstr(l, "]> ");
		if (!e)
			e = strstr(l, "]# ");
		if (!e)
			break;
		snprintf(prompt_dev, sizeof(prompt_dev), "%.*s", (int)(e - l - 1), l + 1);
		l = e + 3;
	}
	if (!*l)
		return;
	char *k, *val;
	if ((l[0] == '\t' || l[0] == ' ') && ctx != CTX_NONE) {
		if (!split_kv(l, &k, &val))
			return;
		if (ctx == CTX_CTL) {
			ctl_prop(k, val);
		} else {
			struct dev *v = find(ctx_addr);
			if (v)
				dev_prop(v, k, val);
		}
		return;
	}
	ctx = CTX_NONE;

	if (!strncmp(l, "[agent] ", 8)) {
		agent_prompt(l + 8);
		return;
	}
	if (!strncmp(l, "[NEW] ", 6) || !strncmp(l, "[CHG] ", 6) || !strncmp(l, "[DEL] ", 6)) {
		char tag = l[1], *obj = l + 6;
		bool is_dev = !strncmp(obj, "Device ", 7), is_ctl = !strncmp(obj, "Controller ", 11);
		char *a = obj + (is_dev ? 7 : 11);
		if ((!is_dev && !is_ctl) || !is_addr(a))
			return;
		char addr[18];
		snprintf(addr, sizeof(addr), "%.17s", a);
		char *rest = a[17] ? a + 18 : a + 17;
		if (is_ctl) {
			if (tag == 'N') {
				controller_seen();
				show_at = now_ms();	/* re-read everything for the new controller */
			} else if (tag == 'D') {
				have_ctl = powered = discovering = false;
				changed = true;
				snprintf(status_txt, sizeof(status_txt), "No Bluetooth controller");
			} else if (split_kv(rest, &k, &val)) {
				ctl_prop(k, val);
			}
			return;
		}
		if (tag == 'D') {
			struct dev *v = find(addr);
			if (v) {
				drop(v);
				changed = true;
			}
			if (op.kind == OP_FORGET && !strcasecmp(op.addr, addr))
				op_end();
			return;
		}
		struct dev *v = get(addr);
		if (!v)
			return;
		if (tag == 'N') {
			set_name(v, rest, false);
			v->info_asked = false;
			changed = true;
		} else if (split_kv(rest, &k, &val)) {
			dev_prop(v, k, val);
		}
		return;
	}
	if (!strncmp(l, "Controller ", 11) && is_addr(l + 11)) {	/* `show` */
		ctx = CTX_CTL;
		controller_seen();
		return;
	}
	if (!strncmp(l, "Device ", 7) && is_addr(l + 7) && !strcmp(l + 24, " not available")) {
		if (op.kind)	/* pair/connect/remove of a device bluetoothd dropped */
			op_fail("NotAvailable");
		return;
	}
	if (!strncmp(l, "Device ", 7) && is_addr(l + 7)) {	/* `info` header or a `devices` line */
		char addr[18];
		snprintf(addr, sizeof(addr), "%.17s", l + 7);
		struct dev *v = get(addr);
		const char *rest = l[24] ? l + 25 : "";
		if (!strcmp(rest, "(public)") || !strcmp(rest, "(random)")) {
			ctx = CTX_DEV;
			snprintf(ctx_addr, sizeof(ctx_addr), "%s", addr);
			if (v)
				v->info_asked = true;
		} else if (v) {
			set_name(v, rest, false);
		}
		return;
	}

	/* results and announcements */
	if (strstr(l, "Waiting to connect to bluetoothd")) {
		snprintf(status_txt, sizeof(status_txt), "Waiting for bluetoothd\xe2\x80\xa6");
		changed = true;
	} else if (strstr(l, "No default controller available")) {
		have_ctl = false;
		snprintf(status_txt, sizeof(status_txt), "No Bluetooth controller");
		changed = true;
	} else if (!strcmp(l, "Agent registered")) {
		agent_ok = true;
		cmd("default-agent");
		show_at = now_ms();
	} else if (!strcmp(l, "Request confirmation") || !strcmp(l, "Request authorization") ||
		   !strcmp(l, "Authorize service") || !strcmp(l, "Request PIN code") || !strcmp(l, "Request passkey")) {
		memset(&ag, 0, sizeof(ag));
		ag.kind = l[8] == 'c' ? AG_CONFIRM : l[8] == 'a' ? AG_AUTHORIZE : l[0] == 'A' ? AG_SERVICE :
			  l[8] == 'P' ? AG_PIN : AG_PASSKEY;
		ag.at = now_ms();
	} else if (!strcmp(l, "Request canceled")) {
		memset(&ag, 0, sizeof(ag));
	} else if (!strcmp(l, "Pairing successful")) {
		op_next(S_TRUST);
	} else if (!strncmp(l, "Failed to pair: ", 16)) {
		if (op.kind == OP_CONNECT && op.step == S_PAIR) {
			if (strstr(l, "AlreadyExists"))
				op_next(S_TRUST);
			else
				op_fail(l + 16);
		}
	} else if (strstr(l, " trust succeeded") || !strncmp(l, "Failed to set trusted", 21)) {
		op_next(S_CONNECT);
	} else if (!strcmp(l, "Connection successful")) {
		if (op.kind == OP_CONNECT)
			op_end();
	} else if (!strncmp(l, "Failed to connect: ", 19)) {
		if (op.kind == OP_CONNECT && op.step == S_CONNECT)
			op_fail(l + 19);
	} else if (!strcmp(l, "Successful disconnected")) {
		if (op.kind == OP_DISCONNECT)
			op_end();
	} else if (!strncmp(l, "Failed to disconnect: ", 22)) {
		if (op.kind == OP_DISCONNECT)
			op_fail(l + 22);
	} else if (!strcmp(l, "Device has been removed")) {
		if (op.kind == OP_FORGET)
			op_end();
	} else if (!strncmp(l, "Failed to remove device: ", 25)) {
		if (op.kind == OP_FORGET)
			op_fail(l + 25);
	} else if (!strncmp(l, "Failed to set alias", 19)) {
		post_error("Couldn't Rename", "%s", l[19] == ':' ? l + 21 : "Bluetooth didn't take the new name.");
		for (int i = 0; i < ndev; i++)	/* re-read the names it really has */
			devs[i].info_asked = false;
	} else if (!strncmp(l, "Failed to set power on: ", 24)) {
		post_error("Bluetooth Unavailable", strstr(l, "Blocked") ? "Bluetooth is blocked (rfkill)." : "%s", l + 24);
	}
}

/* bytes from bluetoothctl: strip colour/erase escapes, split on \r and \n; a trailing "[agent] ...:"
 * (a prompt waiting for input, no newline) is handled right away */
static void feed(const char *buf, int n)
{
	for (int i = 0; i < n; i++) {
		char c = buf[i];
		if (c == '\x1b') {	/* CSI: ESC [ params final */
			int j = i + 1;
			if (j < n && buf[j] == '[') {
				j++;
				while (j < n && !((buf[j] >= '@' && buf[j] <= '~')))
					j++;
			}
			i = j;
			continue;
		}
		if (c == '\r' || c == '\n') {
			rbuf[rlen] = 0;
			if (rlen)
				line(rbuf);
			rlen = 0;
			continue;
		}
		if (c == '\x01' || c == '\x02')	/* readline's invisible-prompt markers */
			continue;
		if (rlen < (int)sizeof(rbuf) - 1)
			rbuf[rlen++] = c;
	}
	rbuf[rlen] = 0;
	char *p = rbuf;
	while (*p == '[' && strncmp(p, "[agent]", 7)) {	/* prompt in front of the partial line */
		char *e = strstr(p, "]> ");
		if (!e)
			break;
		p = e + 3;
	}
	if (!strncmp(p, "[agent] ", 8) && strchr(p, ':'))
		agent_prompt(p + 8);
}

/* ---------- audio routing ---------- */

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *b = malloc(16384);
	size_t n = b ? fread(b, 1, 16383, f) : 0;
	fclose(f);
	if (b)
		b[n] = 0;
	return b;
}

static bool put_file(const char *path, const char *s)
{
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return false;
	fputs(s, f);
	if (fclose(f)) {
		unlink(tmp);
		return false;
	}
	return rename(tmp, path) == 0;
}

static void route_speaker(void)
{
	char *curc = slurp(ASOUND);
	if (curc && strstr(curc, MARK)) {
		char *saved = slurp(ASOUND_SAVE);
		put_file(ASOUND, saved && *saved ? saved : speaker_conf);
		free(saved);
		fprintf(stderr, "bt: ALSA default -> speaker\n");
	}
	free(curc);
}

static void route_bt(const char *addr, const char *name)
{
	char *curc = slurp(ASOUND);
	if (!curc || !strstr(curc, MARK))	/* keep the speaker config for the way back */
		put_file(ASOUND_SAVE, curc && *curc ? curc : speaker_conf);
	free(curc);
	char nm[64], conf[600];
	snprintf(nm, sizeof(nm), "%s", name);
	for (char *p = nm; *p; p++)
		if (*p == '\n' || *p == '"')
			*p = ' ';
	snprintf(conf, sizeof(conf),
		 MARK " Bluetooth audio \"%s\" - written by Orchard (ui/src/bt.c); the speaker config\n"
		 "# (" ASOUND_SAVE ") comes back when the device disconnects.\n"
		 "pcm.!default {\n\ttype plug\n\tslave.pcm \"bluealsa:DEV=%s,PROFILE=a2dp\"\n}\n"
		 "ctl.!default {\n\ttype hw\n\tcard 0\n}\n", nm, addr);
	put_file(ASOUND, conf);
	fprintf(stderr, "bt: ALSA default -> bluealsa %s (%s)\n", addr, nm);
}

/* does BlueALSA have the A2DP PCM for this device yet? */
static bool pcm_ready(const char *addr)
{
	if (access(BACLI, X_OK))
		return true;	/* can't tell: try it */
	char want[40], l[200];
	snprintf(want, sizeof(want), "dev_%s/a2dp", addr);
	for (char *p = want; *p; p++)
		if (*p == ':')
			*p = '_';
	FILE *p = popen(BACLI " list-pcms 2>/dev/null", "r");
	bool hit = false;
	while (p && fgets(l, sizeof(l), p))
		if (strcasestr(l, want))
			hit = true;
	if (p)
		pclose(p);
	return hit;
}

/* the device's BlueALSA mixer control ("<name> A2DP") + its volume */
static void find_mixer(const char *addr)
{
	char c[200], l[200], name[96] = "";
	route_dev[0] = route_ctl[0] = 0;
	route_pct = -1;
	if (access(AMIXER, X_OK))
		return;
	snprintf(c, sizeof(c), AMIXER " -D bluealsa:DEV=%s scontrols 2>/dev/null", addr);
	FILE *p = popen(c, "r");
	while (p && fgets(l, sizeof(l), p)) {
		char *a = strchr(l, '\''), *b = a ? strrchr(a + 1, '\'') : NULL;
		if (!a || !b || b - a - 1 >= (int)sizeof(name))
			continue;
		if (!name[0] || strstr(l, "A2DP"))
			snprintf(name, sizeof(name), "%.*s", (int)(b - a - 1), a + 1);
	}
	if (p)
		pclose(p);
	if (!name[0] || strchr(name, '\''))
		return;
	snprintf(c, sizeof(c), AMIXER " -D bluealsa:DEV=%s sget '%s' 2>/dev/null", addr, name);
	int pct = -1;
	p = popen(c, "r");
	while (p && fgets(l, sizeof(l), p)) {
		char *b = strchr(l, '[');
		if (b && strchr(b, '%') && pct < 0)
			pct = atoi(b + 1);
	}
	if (p)
		pclose(p);
	pthread_mutex_lock(&mu);
	snprintf(route_dev, sizeof(route_dev), "bluealsa:DEV=%s", addr);
	snprintf(route_ctl, sizeof(route_ctl), "%s", name);
	route_pct = pct;
	pthread_mutex_unlock(&mu);
}

/* runs with mu held; drops it around the slow bits (popen) */
static void route_step(void)
{
	const struct dev *best = NULL;
	for (int i = 0; i < ndev; i++) {
		const struct dev *v = &devs[i];
		if (v->d.connected && v->d.audio && v->d.paired && strcasecmp(route_failed, v->d.addr) &&
		    (!best || v->conn_at > best->conn_at))
			best = v;
	}
	long now = now_ms();
	if (!best) {
		route_wait[0] = 0;
		if (routed[0]) {
			routed[0] = audio_name[0] = 0;
			pthread_mutex_unlock(&mu);
			route_speaker();
			pthread_mutex_lock(&mu);
			route_dev[0] = route_ctl[0] = 0;
			route_pct = -1;
			route_pending = true;
			route_keep = false;	/* headphones gone: Music pauses, like iOS */
			changed = true;
		}
		return;
	}
	if (!strcasecmp(best->d.addr, routed)) {
		if (strcmp(audio_name, best->d.name)) {
			snprintf(audio_name, sizeof(audio_name), "%s", best->d.name);
			changed = true;
		}
		return;
	}
	if (strcasecmp(route_wait, best->d.addr)) {
		snprintf(route_wait, sizeof(route_wait), "%s", best->d.addr);
		route_since = now;
		route_poll = 0;
	}
	if (now - route_poll < 700)
		return;
	route_poll = now;
	char addr[18], name[64];
	snprintf(addr, sizeof(addr), "%s", best->d.addr);
	snprintf(name, sizeof(name), "%s", best->d.name);
	pthread_mutex_unlock(&mu);
	bool ok = pcm_ready(addr);
	if (ok) {
		route_bt(addr, name);
		find_mixer(addr);
	}
	pthread_mutex_lock(&mu);
	if (ok) {
		snprintf(routed, sizeof(routed), "%s", addr);
		snprintf(audio_name, sizeof(audio_name), "%s", name);
		route_wait[0] = 0;
		route_pending = true;
		route_keep = true;
		changed = true;
	} else if (now - route_since > 12000) {
		fprintf(stderr, "bt: no BlueALSA PCM for %s, staying on the speaker\n", addr);
		snprintf(route_failed, sizeof(route_failed), "%s", addr);
		route_wait[0] = 0;
		post_error("No Audio", "\xe2\x80\x9c%s\xe2\x80\x9d connected, but its audio (A2DP) didn't start. "
			   "Music stays on the speaker.", name);
	}
}

/* ---------- the worker ---------- */

static void child_gone(void)
{
	if (c_in >= 0)
		close(c_in);
	if (c_out >= 0)
		close(c_out);
	c_in = c_out = -1;
	if (cpid > 0) {
		kill(cpid, SIGKILL);
		waitpid(cpid, NULL, 0);
	}
	cpid = 0;
	pthread_mutex_lock(&mu);
	ready = have_ctl = powered = discovering = agent_ok = false;
	scan_sent = false;
	ndev = 0;	/* re-read when it comes back */
	if (op.kind)
		post_error("Bluetooth", "The Bluetooth helper stopped. Try again in a moment.");
	op.kind = OP_NONE;
	memset(&ag, 0, sizeof(ag));
	snprintf(status_txt, sizeof(status_txt), "Starting\xe2\x80\xa6");
	changed = true;
	pthread_mutex_unlock(&mu);
	next_spawn = now_ms() + 3000;
}

static void spawn(void)
{
	next_spawn = now_ms() + 3000;
	if (access(BTCTL, X_OK)) {
		pthread_mutex_lock(&mu);
		if (strcmp(status_txt, "Not Installed")) {
			snprintf(status_txt, sizeof(status_txt), "Not Installed");
			changed = true;
		}
		pthread_mutex_unlock(&mu);
		return;
	}
	int sv[2], po[2];
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv))
		return;
	if (pipe2(po, O_CLOEXEC)) {
		close(sv[0]);
		close(sv[1]);
		return;
	}
	pid_t pid = fork();
	if (pid == 0) {
		dup2(sv[1], 0);
		dup2(po[1], 1);
		dup2(po[1], 2);
		for (int fd = 3; fd < 1024; fd++)
			close(fd);
		setenv("TERM", "dumb", 1);
		execl(BTCTL, "bluetoothctl", "--agent", "DisplayYesNo", (char *)NULL);
		_exit(127);
	}
	close(sv[1]);
	close(po[1]);
	if (pid < 0) {
		close(sv[0]);
		close(po[0]);
		return;
	}
	cpid = pid;
	c_in = sv[0];
	c_out = po[0];
	rlen = 0;
	show_at = now_ms() + 600;	/* `show` + `devices` once it has connected */
	fprintf(stderr, "bt: bluetoothctl started (pid %d)\n", pid);
}

/* the alias: bluetoothctl's set-alias acts on its default device (the connected one its prompt
 * names); any other device goes through D-Bus directly. Runs with mu held, drops it around dbus-send */
static void rename_dev(const struct req *r)
{
	struct dev *v = find(r->addr);
	if (!v)
		return;
	char nm[64], q[200], c[400], l[200];
	int k = 0;
	for (const char *p = r->name; *p && k < (int)sizeof(nm) - 1; p++)	/* bluetoothctl expands $, `, \, " */
		if ((unsigned char)*p >= ' ' && !strchr("\"$`\\", *p))
			nm[k++] = *p;
	nm[k] = 0;
	utf8_trim(nm);
	/* D-Bus names the device by its address. bluetoothctl's set-alias only reaches its default
	 * device, which its prompt names by alias — another device with the same name would get it — so
	 * that is only the fallback without dbus-send, and only when the name is unambiguous */
	bool is_default = access(DBUS_SEND, X_OK) && v->d.connected && c_in >= 0 && !strcmp(prompt_dev, v->d.name);
	for (int i = 0; is_default && i < ndev; i++)
		if (&devs[i] != v && !strcmp(devs[i].d.name, v->d.name))
			is_default = false;
	if (nm[0] && strcmp(nm, v->d.name)) {	/* shown at once; `info` / [CHG] Alias confirm it */
		snprintf(v->d.name, sizeof(v->d.name), "%s", nm);
		v->d.named = v->aliased = changed = true;
	}
	if (is_default) {
		if (nm[0])
			cmd("set-alias \"%s\"", nm);
		else
			cmd("reset-alias");
		v->info_asked = false;
		v->info_at = now_ms() + 1000;
		return;
	}
	k = 0;
	for (const char *p = nm; *p && k < (int)sizeof(q) - 5; p++) {	/* one shell word in '...' */
		if (*p == '\'') {
			memcpy(q + k, "'\\''", 4);
			k += 4;
		} else {
			q[k++] = *p;
		}
	}
	q[k] = 0;
	char path[18];
	snprintf(path, sizeof(path), "%s", v->d.addr);
	for (char *p = path; *p; p++)
		if (*p == ':')
			*p = '_';
	snprintf(c, sizeof(c), DBUS_SEND " --system --print-reply --reply-timeout=3000 --dest=org.bluez /org/bluez/hci0/dev_%s "
		 "org.freedesktop.DBus.Properties.Set string:org.bluez.Device1 string:Alias variant:string:'%s' 2>&1",
		 path, q);
	pthread_mutex_unlock(&mu);
	bool ok = false;
	FILE *p = access(DBUS_SEND, X_OK) ? NULL : popen(c, "r");
	if (p) {
		ok = true;
		while (fgets(l, sizeof(l), p))
			if (strstr(l, "Error"))
				ok = false;
		ok &= pclose(p) == 0;
	}
	pthread_mutex_lock(&mu);
	if (!ok) {
		fprintf(stderr, "bt: rename %s failed\n", r->addr);
		post_error("Couldn't Rename", "Bluetooth didn't take the new name.");
	}
	if ((v = find(r->addr)))
		v->info_asked = false;	/* the name it really has now */
}

static void step(void)
{
	long now = now_ms();
	pthread_mutex_lock(&mu);
	bool prompt = ag.kind && !ag.answered;
	if (prompt && now - ag.at > 35000) {	/* bluetoothd gave up on it long ago */
		memset(&ag, 0, sizeof(ag));
		prompt = false;
	}

	/* requests from the UI */
	struct req ren[4];
	int nren = 0;
	for (int i = 0; i < nreq; i++) {
		struct req *r = &reqs[i];
		switch (r->op) {
		case R_ANSWER:
			if (ag.user && (r->arg >> 1) == ag.id) {
				agent_answer(r->arg & 1 ? "yes" : "no");
				prompt = false;
			}
			break;
		case R_POWER_ON:
		case R_POWER_OFF:
			if (!prompt)
				cmd("power %s", r->op == R_POWER_ON ? "on" : "off");
			break;
		case R_SCAN_ON:
		case R_SCAN_OFF:
			want_scan = r->op == R_SCAN_ON;
			break;
		case R_RENAME:
			if (nren < 4)
				ren[nren++] = *r;
			break;
		default:
			if (npending < 8)
				pending_ops[npending++] = *r;
			break;
		}
	}
	nreq = 0;
	for (int i = 0; i < nren; i++)
		rename_dev(&ren[i]);

	if (!prompt && ready) {
		/* the current operation: timeouts + retries, else the next queued one */
		if (op.kind && op.retry_at && now >= op.retry_at) {
			op_send();
		} else if (op.kind && op.deadline && now > op.deadline) {
			if (op.step == S_PAIR)
				cmd("cancel-pairing %s", op.addr);
			if (op.step == S_TRUST)
				op_next(S_CONNECT);
			else
				op_fail(op.step == S_PAIR ? "AuthenticationTimeout" : "Timeout");
		}
		/* discovery (and being discoverable) only while the Bluetooth page wants it and nothing is pairing:
		 * an inquiry running next to a page makes pairing slow and flaky, so it stops first */
		bool scan = want_scan && seen && powered && !op.kind && !npending;
		if (scan != scan_sent) {
			if (scan) {
				cmd("pairable on");
				cmd("discoverable on");
				cmd("scan bredr");	/* A2DP speakers/headphones are BR/EDR; skips the LE beacon noise */
			} else {
				cmd("scan off");
				if (!want_scan || !seen)	/* (locked: back with the scan at unlock) */
					cmd("discoverable off");
			}
			scan_sent = scan;
		}
		if (!op.kind && npending) {
			struct req r = pending_ops[0];
			memmove(pending_ops, pending_ops + 1, --npending * sizeof(pending_ops[0]));
			op_start(&r);
		}
		/* details of new devices (bluetoothctl answers `info` from its own cache); connected ones
		 * again every minute for their battery level */
		for (int i = 0, n = 0; i < ndev && n < 4; i++) {
			struct dev *v = &devs[i];
			if ((!v->info_asked && now >= v->info_at) || (v->d.connected && now >= v->info_at && now - v->info_t > 60000)) {
				v->info_asked = true;
				v->info_t = now;
				cmd("info %s", v->d.addr);
				n++;
			}
		}
	}
	if (!prompt && ready && !have_ctl && !show_at && now - last_show > 3000)
		show_at = now;	/* bluetoothd/controller came up later */
	if (!prompt && show_at && now >= show_at && c_in >= 0) {
		last_show = now;
		show_at = 0;
		cmd("show");
		cmd("devices");
		ready = true;
	}
	if (ready && have_ctl && ctl_alias[0] && strcmp(ctl_alias, ALIAS) && !prompt) {
		cmd("system-alias \"" ALIAS "\"");	/* quoted: the shell splits arguments */
		snprintf(ctl_alias, sizeof(ctl_alias), "%s", ALIAS);
	}
	avail = ready && have_ctl;
	if (ready)
		route_step();
	pthread_mutex_unlock(&mu);
}

static void *worker(void *arg)
{
	(void)arg;
	char buf[4096];
	for (;;) {
		if (c_out < 0 && now_ms() >= next_spawn)
			spawn();
		struct pollfd p[2] = { { wake_fd[0], POLLIN, 0 }, { c_out, POLLIN, 0 } };
		int n = poll(p, c_out >= 0 ? 2 : 1, 250);
		if (n > 0 && p[0].revents & POLLIN)
			while (read(wake_fd[0], buf, sizeof(buf)) > 0)
				;
		if (c_out >= 0 && n > 0 && p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
			ssize_t k = read(c_out, buf, sizeof(buf));
			if (k > 0) {
				pthread_mutex_lock(&mu);
				feed(buf, k);
				pthread_mutex_unlock(&mu);
			} else if (k == 0 || (errno != EAGAIN && errno != EINTR)) {
				fprintf(stderr, "bt: bluetoothctl exited\n");
				child_gone();
				continue;
			}
		}
		step();
	}
	return NULL;
}

static void request_named(int op, const char *addr, int arg, const char *name)
{
	pthread_mutex_lock(&mu);
	if (nreq < 16) {
		reqs[nreq].op = op;
		reqs[nreq].arg = arg;
		snprintf(reqs[nreq].addr, sizeof(reqs[0].addr), "%s", addr ? addr : "");
		snprintf(reqs[nreq].name, sizeof(reqs[0].name), "%s", name ? name : "");
		utf8_trim(reqs[nreq].name);
		nreq++;
	}
	if (op == R_CONNECT || op == R_DISCONNECT || op == R_FORGET) {	/* show it at once */
		struct dev *v = find(addr);
		if (v && !v->d.busy) {
			v->d.busy = op == R_CONNECT ? (v->d.paired ? BT_CONNECTING : BT_PAIRING) :
				    op == R_DISCONNECT ? BT_DISCONNECTING : BT_REMOVING;
			changed = true;
		}
	}
	pthread_mutex_unlock(&mu);
	if (write(wake_fd[1], "x", 1) < 0)
		return;
}

static void request(int op, const char *addr, int arg) { request_named(op, addr, arg, NULL); }
static bool scan_wanted(void) { return want_scan; }	/* (mu) */
static void seen_changed(void)	/* the worker's step() starts / stops the scan now, not at its next 250 ms poll */
{
	if (wake_fd[1] >= 0 && write(wake_fd[1], "x", 1) < 0)
		return;
}

static void backend_init(void)
{
	const char *e = getenv("ORCHARD_BT_DEBUG");
	debug = e && *e == '1';
	/* Orchard restarted while the default PCM pointed at a device: start from the speaker */
	route_speaker();
	if (pipe2(wake_fd, O_CLOEXEC | O_NONBLOCK))
		return;
	pthread_attr_t at;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&at, 256 << 10);
	pthread_t tid;
	pthread_create(&tid, &at, worker, NULL);
	pthread_attr_destroy(&at);
}

void bt_set_power(bool on) { request(on ? R_POWER_ON : R_POWER_OFF, NULL, 0); }
void bt_scan(bool on) { request(on ? R_SCAN_ON : R_SCAN_OFF, NULL, 0); }
void bt_connect(const char *addr) { request(R_CONNECT, addr, 0); }
void bt_disconnect(const char *addr) { request(R_DISCONNECT, addr, 0); }
void bt_forget(const char *addr) { request(R_FORGET, addr, 0); }
void bt_rename(const char *addr, const char *name) { request_named(R_RENAME, addr, 0, name); }
static void answer(int id, bool yes) { request(R_ANSWER, NULL, id << 1 | yes); }

/* ================================ preview backend ================================ */
#else

/* snap/sdl: a few made-up devices so every state of the UI can be looked at */
static struct fake {
	const char *addr, *real;
	enum bt_kind kind;
	bool audio, paired, connected, fails;
	int appear;	/* ms after the scan starts; 0 = known from the start */
	int battery;	/* while connected */
	char name[64];	/* alias ("" = the real name) */
} fakes[] = {
	{ "02:00:00:00:B7:01", "Headphones", BT_HEADPHONES, true, true, true, false, 0, 80, "" },
	{ "02:00:00:00:B7:02", "Soundbar", BT_SPEAKER, true, true, false, false, 0, -1, "" },
	{ "02:00:00:00:B7:03", "Portable Speaker", BT_SPEAKER, true, false, false, false, 700, 55, "" },
	{ "02:00:00:00:B7:04", "desktop-pc", BT_COMPUTER, true, false, false, false, 1600, -1, "" },
	{ "02:00:00:00:B7:05", "Earbuds", BT_HEADPHONES, true, false, false, false, 2600, 100, "" },
	{ "02:00:00:00:B7:06", "Living Room TV", BT_TV, false, false, false, true, 3600, -1, "" },
};
static const char *fake_name(const struct fake *f) { return f->name[0] ? f->name : f->real; }
#define NFAKE ((int)(sizeof(fakes) / sizeof(fakes[0])))
static bool fake_scan;
static uint32_t scan_t0;	/* preview time is simulated: lv_tick, not the clock */
static struct { int idx, phase; uint32_t until; } fop = { -1, 0, 0 };

static void fake_sync(void)
{
	uint32_t now = lv_tick_get();
	pthread_mutex_lock(&mu);
	for (int i = 0; i < NFAKE; i++) {
		struct fake *f = &fakes[i];
		bool visible = powered && (f->paired || (fake_scan && f->appear && now - scan_t0 >= (uint32_t)f->appear) ||
					   (find(f->addr) != NULL && (f->paired || fop.idx == i)));
		struct dev *v = find(f->addr);
		if (!visible) {
			if (v) {
				drop(v);
				changed = true;
			}
			continue;
		}
		if (!v) {
			v = get(f->addr);
			changed = true;
		}
		int bat = f->connected ? f->battery : -1;
		bool was = v->d.paired != f->paired || v->d.connected != f->connected || strcmp(v->d.name, fake_name(f)) ||
			   v->d.battery != bat;
		snprintf(v->d.name, sizeof(v->d.name), "%s", fake_name(f));
		v->d.battery = bat;
		v->d.named = true;
		v->d.kind = f->kind;
		v->d.audio = f->audio;
		v->d.paired = f->paired;
		v->d.connected = f->connected;
		v->d.rssi = -40 - i * 7;
		enum bt_busy b = fop.idx == i ? (fop.phase == 1 ? BT_PAIRING : fop.phase == 2 ? BT_CONNECTING :
						 fop.phase == 3 ? BT_DISCONNECTING : BT_REMOVING) : BT_IDLE;
		if (was || v->d.busy != b)
			changed = true;
		v->d.busy = b;
		if (f->connected)
			v->conn_at = i;
	}
	const char *an = NULL;
	for (int i = 0; i < NFAKE; i++)
		if (powered && fakes[i].connected && fakes[i].audio)
			an = fake_name(&fakes[i]);
	bool route = strcmp(audio_name, an ? an : "") != 0;
	if (route) {
		snprintf(audio_name, sizeof(audio_name), "%s", an ? an : "");
		route_pending = true;
		route_keep = an != NULL;
		changed = true;
	}
	discovering = fake_scan && seen && powered && fop.idx < 0;
	pthread_mutex_unlock(&mu);
}

static void fake_tick(lv_timer_t *t)
{
	(void)t;
	if (fop.idx >= 0 && (int32_t)(lv_tick_get() - fop.until) >= 0) {
		struct fake *f = &fakes[fop.idx];
		if (fop.phase == 1 && f->fails) {
			pthread_mutex_lock(&mu);
			snprintf(err_title, sizeof(err_title), "Pairing Unsuccessful");
			snprintf(err_msg, sizeof(err_msg), "Make sure \xe2\x80\x9c%s\xe2\x80\x9d is turned on, in range, "
				 "and is ready to pair.", fake_name(f));
			err_pending = true;
			pthread_mutex_unlock(&mu);
			fop.idx = -1;
		} else if (fop.phase == 1) {
			f->paired = true;
			fop.phase = 2;
			fop.until = lv_tick_get() + 900;
		} else if (fop.phase == 2) {
			for (int i = 0; i < NFAKE; i++)
				if (fakes[i].audio && f->audio)
					fakes[i].connected = false;	/* one audio device at a time, like iOS */
			f->connected = true;
			fop.idx = -1;
		} else if (fop.phase == 3) {
			f->connected = false;
			fop.idx = -1;
		} else {
			f->paired = f->connected = false;
			f->name[0] = 0;	/* the alias goes with the pairing */
			fop.idx = -1;
		}
	}
	fake_sync();
}

static int fake_find(const char *addr)
{
	for (int i = 0; i < NFAKE; i++)
		if (!strcmp(fakes[i].addr, addr))
			return i;
	return -1;
}

static void fake_op(const char *addr, int phase, int ms)
{
	int i = fake_find(addr);
	if (i < 0 || fop.idx >= 0)
		return;
	fop.idx = i;
	fop.phase = phase;
	fop.until = lv_tick_get() + ms;
	fake_sync();
}

static void backend_init(void)
{
	const char *e = getenv("ORCHARD_BT");	/* "off" = start powered off, "none" = no Bluetooth */
	avail = !(e && !strcmp(e, "none"));
	powered = avail && !(e && !strcmp(e, "off"));
	snprintf(status_txt, sizeof(status_txt), avail ? "On" : "Not Installed");
	fake_sync();
	if (avail)	/* ORCHARD_BT=none: no 100 ms fake tick either, so snap `idle` counts what the phone wakes for */
		lv_timer_create(fake_tick, 100, NULL);
}

void bt_set_power(bool on)
{
	if (!avail)
		return;
	powered = on;
	if (!on)
		for (int i = 0; i < NFAKE; i++)
			fakes[i].connected = false;
	else
		fakes[0].connected = true;	/* the headphones reconnect by themselves */
	fop.idx = -1;
	fake_sync();
}

void bt_scan(bool on)
{
	if (on && !fake_scan)
		scan_t0 = lv_tick_get();
	fake_scan = on;
	fake_sync();
}
static bool scan_wanted(void) { return fake_scan; }
static void seen_changed(void) { fake_sync(); }

void bt_connect(const char *addr)
{
	int i = fake_find(addr);
	if (i >= 0 && !fakes[i].connected)
		fake_op(addr, fakes[i].paired ? 2 : 1, fakes[i].paired ? 900 : 1500);
}

void bt_disconnect(const char *addr) { fake_op(addr, 3, 500); }
void bt_forget(const char *addr) { fake_op(addr, 4, 400); }

void bt_rename(const char *addr, const char *name)
{
	int i = fake_find(addr);
	if (i < 0)
		return;
	snprintf(fakes[i].name, sizeof(fakes[i].name), "%s", name);
	fake_sync();
}
static void answer(int id, bool yes) { (void)id; (void)yes; }
#endif

/* ================================ UI side (LVGL thread) ================================ */

static void ask_cb(int idx, void *ud) { answer((intptr_t)ud, idx == 1); }

/* the page is seen (unlocked, screen on) or not: discovery follows (seen); the lock edges come from lock_changed, a dark
 * screen without the lock from ui_poll */
static void seen_check(void)
{
	bool on = !shell_locked() && !power_dark();
	pthread_mutex_lock(&mu);
	bool ch = on != seen, wanted = scan_wanted();
	seen = on;
	pthread_mutex_unlock(&mu);
	if (!ch)
		return;
	if (wanted)
		fprintf(stderr, "bt: discovery %s\n", on ? "on again (unlocked)" : "paused (locked / screen off)");
	seen_changed();
}

static void ui_poll(lv_timer_t *t)
{
	seen_check();
	if (power_dark())	/* screen off: a connected headset's events within a second, else every 5 s */
		lv_timer_set_period(t, bt_connected() ? 1000 : 5000);
	else	/* the Bluetooth page (discovery on) and seen: pairing at once; else (or locked) twice a second */
		lv_timer_set_period(t, bt_scanning() && !shell_locked() ? 150 : 500);
	char et[64], em[240], dev[48], ctl[96], msg[300];
	pthread_mutex_lock(&mu);
	bool ch = changed, e = err_pending, r = route_pending, keep = route_keep, overlay_busy;
	int pct = route_pct;
	typeof(ask) a = ask;
	changed = route_pending = false;
	snprintf(dev, sizeof(dev), "%s", route_dev);
	snprintf(ctl, sizeof(ctl), "%s", route_ctl);
	overlay_busy = lv_obj_get_child_count(shell_overlay()) > 0;	/* an alert is up: keep ours queued */
	if (e && !overlay_busy) {
		snprintf(et, sizeof(et), "%s", err_title);
		snprintf(em, sizeof(em), "%s", err_msg);
		err_pending = false;
	} else {
		e = false;
	}
	if (a.pending && !overlay_busy && !e)
		ask.pending = false;
	else
		a.pending = false;
	pthread_mutex_unlock(&mu);

	if (r) {
		sys_volume_route(dev[0] ? dev : NULL, ctl, pct);
		music_output_changed(keep);
	}
	if (ch || r)
		for (int i = 0; i < MAXL; i++)
			if (listeners[i])
				listeners[i]();
	if (e) {
		ui_alert(et, em, (const char *[]){ "OK" }, 1, NULL, NULL);
	} else if (a.pending) {
		if (a.kind == 1 && a.code[0])
			snprintf(msg, sizeof(msg), "\xe2\x80\x9c%s\xe2\x80\x9d would like to pair with your iPhone. Confirm that "
				 "this code is shown on \xe2\x80\x9c%s\xe2\x80\x9d.\n\n%s", a.name, a.name, a.code);
		else
			snprintf(msg, sizeof(msg), "\xe2\x80\x9c%s\xe2\x80\x9d would like to pair with your iPhone.", a.name);
		ui_alert("Bluetooth Pairing Request", msg, (const char *[]){ "Cancel", "Pair" }, 2, ask_cb,
			 (void *)(intptr_t)a.id);
	}
}

static lv_timer_t *ui_t;

static void lock_changed(bool locked)
{
	seen_check();
	if (!locked && ui_t && bt_scanning())
		lv_timer_ready(ui_t);
}

void bt_init(void)
{
	backend_init();
	ui_t = lv_timer_create(ui_poll, 150, NULL);
	power_keep(ui_t, 1000, "bt");	/* screen off: audio still follows the headset */
	lock_listen(lock_changed, true);
}

bool bt_available(void) { return avail; }
bool bt_powered(void) { return avail && powered; }
bool bt_scanning(void) { return avail && powered && discovering; }

const char *bt_status(void)
{
	static char s[96];
	pthread_mutex_lock(&mu);
	snprintf(s, sizeof(s), "%s", avail ? (powered ? "On" : "Off") : status_txt);
	pthread_mutex_unlock(&mu);
	return s;
}

int bt_devices(bt_device_t *out, int max)
{
	struct dev *ord[MAXDEV];
	int n = 0;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < ndev; i++) {	/* paired first, then discovery order */
		int j = n++;
		for (; j > 0 && (ord[j - 1]->d.paired < devs[i].d.paired ||
				 (ord[j - 1]->d.paired == devs[i].d.paired && ord[j - 1]->order > devs[i].order)); j--)
			ord[j] = ord[j - 1];
		ord[j] = &devs[i];
	}
	if (n > max)
		n = max;
	for (int i = 0; i < n; i++)
		out[i] = ord[i]->d;
	pthread_mutex_unlock(&mu);
	return n;
}

bool bt_device(const char *addr, bt_device_t *out)
{
	pthread_mutex_lock(&mu);
	struct dev *v = find(addr);
	if (v)
		*out = v->d;
	pthread_mutex_unlock(&mu);
	return v != NULL;
}

int bt_connected(void)
{
	int n = 0;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < ndev; i++)
		n += devs[i].d.connected;
	pthread_mutex_unlock(&mu);
	return n;
}

const char *bt_audio_name(void)
{
	static char s[64];
	pthread_mutex_lock(&mu);
	snprintf(s, sizeof(s), "%s", audio_name);
	pthread_mutex_unlock(&mu);
	return s[0] ? s : NULL;
}

const char *bt_glyph(enum bt_kind k)
{
	switch (k) {
	case BT_HEADPHONES: return G_HEADPHONES;
	case BT_SPEAKER: return "\xf3\xb0\x93\x83";	/* md-speaker */
	case BT_COMPUTER: return "\xef\x84\x89";	/* fa-laptop */
	case BT_PHONE: return "\xef\x84\x8b";		/* fa-mobile */
	case BT_INPUT: return G_KEYBOARD;
	case BT_TV: return "\xef\x89\xac";		/* fa-tv */
	case BT_WATCH: return "\xf3\xb0\xa2\x97";	/* md-watch */
	default: return G_BLUETOOTH;
	}
}

void bt_listen(void (*cb)(void), bool on)
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
