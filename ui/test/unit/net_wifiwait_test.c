/* net.c + Wi-Fi Sleep: a request made while the network is away (wifi_sleep_wait) asks power.c to wake Wi-Fi
 * (power_net_for "fetch") and doesn't start (no lookup, no curl, no callback) until it is back, then runs as usual
 * with its timeout counted from then; one made while Wi-Fi is up starts at once; a wait that never ends gives up
 * after WIFI_WAIT_MS and runs (failing as it did before). No network: [::1] is refused, curl is /bin/true.
 * From ui/:
 *   cc -Isrc -I. -Ithird_party/lvgl -DLV_CONF_INCLUDE_SIMPLE -D_GNU_SOURCE -DWIFI_WAIT_MS=300 \
 *      test/unit/net_wifiwait_test.c test/unit/lv_stub.c -lpthread -o $TMPDIR/nww && $TMPDIR/nww */
#include "net.c"
#include <stdio.h>
static bool away;
static int wakes, calls, fails;
bool wifi_sleep_wait(void) { return away; }
void power_net_for(const char *token, uint32_t ms) { wakes += !strcmp(token, "fetch") && ms > 0; }
static void cb(net_res_t *r, void *ud) { (void)r; (void)ud; calls++; }
static void ticks(int n) { for (int i = 0; i < n; i++) { tick(NULL); usleep(10000); } }
#define CHECK(c, what) do { if (!(c)) { printf("FAIL: %s (wakes %d, calls %d, active %d)\n", what, wakes, calls, net_active()); fails++; } } while (0)

int main(void)
{
	unsetenv("ORCHARD_WEB_ALLOW_LOOPBACK");
	setenv("ORCHARD_CURL", "/bin/true", 1);	/* (the last case starts "curl": nothing leaves the PC) */
	if (!net_available()) {
		printf("SKIP: no curl\n");
		return 0;
	}
	/* Wi-Fi up: starts at once (refused at its first tick) */
	CHECK(net_get("http://[::1]:9/", 0, 5, cb, NULL), "net_get");
	ticks(2);
	CHECK(wakes == 0 && calls == 1, "up: no wake, done at once");
	/* away: wakes Wi-Fi, waits, then runs */
	away = true, calls = 0;
	net_get("http://[::1]:9/", 0, 5, cb, NULL);
	CHECK(wakes == 1, "away: power_net_for(\"fetch\") once");
	ticks(10);
	CHECK(calls == 0 && net_active() == 1, "away: still waiting after 100 ms");
	away = false;
	ticks(2);
	CHECK(calls == 1 && net_active() == 0, "back: ran");
	/* away for good: gives up after WIFI_WAIT_MS (300 here) */
	away = true, calls = 0;
	net_get("http://[::1]:9/", 0, 5, cb, NULL);
	ticks(15);
	CHECK(calls == 0, "stuck: waiting at 150 ms");
	ticks(30);
	CHECK(calls == 1, "stuck: ran after the cap");
	/* the timeout counts from when it starts: deadline moved by the wait */
	away = true, calls = 0;
	struct net_req *r = net_get("http://192.0.2.1:9/", 0, 5, cb, NULL);	/* (a lookup thread, then curl: r stays a tick) */
	long long d0 = r->deadline;
	ticks(10);
	long long w0 = now_ms() - r->wifi_since;
	away = false;
	tick(NULL);
	CHECK(r->wifi_since == 0 && (r->pid > 0 || r->dns) && r->deadline - d0 >= w0, "deadline moved by the wait");
	net_cancel(r);
	printf(fails ? "%d FAILED\n" : "ok: net.c waits for Wi-Fi Sleep\n", fails);
	return fails != 0;
}
