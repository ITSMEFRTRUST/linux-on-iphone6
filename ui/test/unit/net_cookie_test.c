/* net.c cookie jars against a local server (test/unit/cookie_server.py): cross-site POST sends no cookies but keeps
 * what it sets, MAX_PAGE truncation keeps Set-Cookie, two hops of one site at once both keep theirs */
#include "net.c"
#include <stdio.h>
static int fails, port, pending;
static char got[4][256];
static void cb(net_res_t *r, void *ud) {
	snprintf(got[(intptr_t)ud], sizeof got[0], "%.*s", (int)strcspn(r->body ? r->body : "", "\n"), r->body ? r->body : "");
	pending--;
}
static void run(void) { for (int i = 0; i < 1000 && pending > 0; i++) { tick(NULL); usleep(10000); } }
static void fetch(int slot, const char *q, const char *post, const char *ref, size_t max) {
	char u[256]; snprintf(u, sizeof u, "http://127.0.0.1:%d/?%s", port, q);
	net_opts_t o = { .url = u, .post = post, .referer = ref, .cookies = NET_CK_RW, .allow_private = true, .max_bytes = max, .timeout_s = 10 };
	got[slot][0] = 0;
	if (net_fetch(&o, cb, (void *)(intptr_t)slot)) pending++;
}
static void chk(const char *what, bool ok) { printf("%-60s %s\n", what, ok ? "ok" : "FAIL"); fails += !ok; }
static bool jar_has(const char *name) {
	char p[600]; snprintf(p, sizeof p, "%s/127.0.0.1", cookie_dir);
	FILE *f = fopen(p, "r"); char l[4096]; bool h = false;
	while (f && fgets(l, sizeof l, f)) { l[strcspn(l, "\n")] = 0; char *t = strrchr(l, '\t'); if (t && l[0] != '#' ) { *t = 0; char *n = strrchr(l, '\t'); if (n && !strcmp(n + 1, name)) h = true; } }
	if (f) fclose(f);
	return h;
}
int main(int argc, char **argv) {
	port = atoi(argv[1]);
	char dir[] = "/tmp/orchard-ck-XXXXXX"; mkdtemp(dir); net_set_cookie_dir(dir);
	char same[64]; snprintf(same, sizeof same, "http://127.0.0.1:%d/form", port);
	fetch(0, "set=sid=1", NULL, NULL, 0); run();
	chk("GET sets sid", jar_has("sid"));
	fetch(0, "", "a=1", same, 0); run();
	chk("same-site POST sends sid", strstr(got[0], "sid=1") != NULL);
	fetch(0, "set=xs=1", "a=1", "https://evil.example/x", 0); run();
	chk("cross-site POST sends no cookies", !strcmp(got[0], "cookie=[]"));
	chk("cross-site POST: its Set-Cookie kept, sid still there", jar_has("xs") && jar_has("sid"));
	fetch(0, "set=big=1&big=1", NULL, NULL, 200000); run();
	chk("truncated at max_bytes: Set-Cookie kept", jar_has("big"));
	fetch(0, "set=c1=1&sleep=0.5", NULL, NULL, 0);
	fetch(1, "set=c2=1&sleep=0.1", NULL, NULL, 0); run();
	chk("two hops at once: both cookies kept", jar_has("c1") && jar_has("c2") && jar_has("sid"));
	fetch(0, "set=c1=%3B%20Path=/%3B%20Max-Age=0", NULL, NULL, 0); run();
	chk("deleted cookie goes", !jar_has("c1") && jar_has("c2"));
	fetch(0, "set=k=1&sleep=0.5", NULL, NULL, 0); net_clear_cookies(); run();
	chk("cleared while loading: nothing comes back but the new one", !jar_has("sid") && !jar_has("c2"));
	fetch(0, "", NULL, NULL, 0); run();
	chk("GET after all that", strstr(got[0], "cookie=[") != NULL);
	char cmd[300]; snprintf(cmd, sizeof cmd, "ls %s | grep -q '~'", dir);
	chk("no temp files left", system(cmd) != 0);
	snprintf(cmd, sizeof cmd, "rm -rf %s", dir); if (system(cmd)) {}
	printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
