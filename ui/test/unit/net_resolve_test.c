/* net_resolve(): the reviewer's crashers (a non-http(s) <base href> with "://" in its query / fragment) + ordinary cases */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "net.h"
static int fails;
static void chk(const char *base, const char *ref, const char *want) {
	char out[2048]; memset(out, 'x', sizeof out);	/* stale bytes, no '/' */
	bool ok = net_resolve(base, ref, out, sizeof out);
	const char *got = ok ? out : "(false)";
	if (strcmp(got, want)) { printf("FAIL %s + %s = %s (want %s)\n", base, ref, got, want); fails++; }
}
int main(void) {
	/* the crashers: a <base href> that is not an absolute http(s) URL never becomes the base */
	char b[2048];
	printf("base http:x?http://a -> %s\n", net_resolve("https://example.com/", "http:x?http://a", b, sizeof b) ? b : "(rejected)");
	chk("http:x?http://a", "?q", "(false)");
	chk("http:x#://", "#top", "(false)");
	chk("http:x#://", "", "(false)");
	/* ordinary resolution still right */
	chk("https://example.com/a/b?c#d", "?q", "https://example.com/a/b?q");
	chk("https://example.com/a/b?c#d", "#top", "https://example.com/a/b?c#top");
	chk("https://example.com/a/b?c#d", "", "https://example.com/a/b?c");
	chk("https://example.com/a/b", "../x", "https://example.com/x");
	chk("https://example.com", "x", "https://example.com/x");
	chk("https://example.com?q=http://z", "#f", "https://example.com/?q=http://z#f");
	chk("https://example.com", "?q", "https://example.com/?q");
	chk("http://h/a", "//o.org", "http://o.org/");
	chk("http://h/a", "HTTPS://O.org/p", "https://O.org/p");
	chk("http://h/a", "http:x", "(false)");
	chk("http://h/a", "mailto:a@b", "(false)");
	chk("HTTP://h/a/", "b", "HTTP://h/a/b");
	printf("%s (%d failures)\n", fails ? "FAILED" : "ok", fails);
	return fails != 0;
}
