/* net.c — HTTP(S) for Orchard apps, off the LVGL thread.
 * Each request is a curl child process (static curl from tools/net.tgz on the phone, the system curl on the
 * PC) whose stdout/stderr are non-blocking pipes, drained by one LVGL timer that only runs while something
 * is in flight. Callbacks run on the LVGL thread. Never -k: the CA bundle is $ORCHARD_CACERT, else
 * /etc/ssl/certs/ca-certificates.crt, else /data/ssl/cacert.pem, else /usr/share/orchard/cacert.pem.
 * net_bg() runs CPU work (HTML parsing, image decoding) on one worker thread, done() back on LVGL's. */
#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct net_req net_req_t;

typedef struct net_res {
	int err;		/* 0 = got a response (any HTTP status), else NET_E* */
	int status;		/* HTTP status, 0 = none */
	const char *msg;	/* error text for people ("The request timed out.") */
	char *body;		/* NUL-terminated (len bytes + 0); freed after the callback unless net_take() */
	size_t len;
	bool truncated;		/* hit max_bytes: body is the first max_bytes */
	char url[2048];		/* final URL after redirects (NET_EPRIVATE / NET_EBLOCKED: the refused one) */
	char ctype[128];	/* Content-Type, lower case ("text/html; charset=utf-8") */
	bool priv;		/* the last hop was on the LAN / tailnet (see net_opts_t.allow_private) */
	bool posted;		/* the answer is to a POST (no redirect turned it into a GET) */
} net_res_t;

enum { NET_OK, NET_ENOCURL, NET_EDNS, NET_ECONNECT, NET_ETLS, NET_ETIMEOUT, NET_EHTTP, NET_EOTHER,
       NET_EBLOCKED,	/* the phone itself, the USB link, link-local, Orchard's relay ports: never fetched (see net.c) */
       NET_EREDIRECTS,
       NET_EPRIVATE };	/* LAN / tailnet address without allow_private (or reached from a public hop) */

typedef void (*net_cb_t)(net_res_t *r, void *ud);

/* cookies: one curl jar per site (registrable domain, see net_site) in the cookie directory */
enum { NET_CK_NONE, NET_CK_READ, NET_CK_RW };
typedef struct net_opts {
	const char *url;
	const char *post;	/* application/x-www-form-urlencoded body → POST (301/302/303 turn it into a GET); NULL = GET */
	const char *referer;	/* page the request comes from: Referer (origin only across sites, none https → http),
				 * Origin on a POST */
	bool desktop;		/* desktop browser User-Agent instead of the mobile one */
	int cookies;		/* NET_CK_*: each hop uses its own site's jar */
	/* the first hop may be on the LAN (10/8, 172.16/12, 192.168/16, fc00::/7) or the tailnet (100.64/10,
	 * fd7a:115c:a1e0::/48): only for what the user typed or tapped themselves, or for what a page that is itself on
	 * the LAN / tailnet loads. A redirect may only go there from a hop that already was. The phone itself,
	 * the USB link 172.16.43.0/24, link-local, MagicDNS's 100.100.100.100 and Orchard's relay ports
	 * (8650, 8656, 8657) are never fetched. */
	bool allow_private;
	size_t max_bytes;	/* 0 = 2 MB */
	int timeout_s;		/* 0 = 30 */
} net_opts_t;
net_req_t *net_fetch(const net_opts_t *o, net_cb_t cb, void *ud);
void net_set_cookie_dir(const char *dir);	/* where the jars live (created 0700); NULL = no cookies */
void net_clear_cookies(void);	/* every jar removed */
const char *net_site(const char *host, char *out, int n);	/* "old.reddit.com" → "reddit.com", "bbc.co.uk" kept */

/* GET url (http/https; redirects followed, max 8, each hop's address checked); max_bytes caps the body (the transfer stops there, truncated = true), timeout_s = whole
 * request. Returns NULL (and calls nothing) if curl can't be started. */
net_req_t *net_get(const char *url, size_t max_bytes, int timeout_s, net_cb_t cb, void *ud);
void net_cancel(net_req_t *r);	/* the callback won't run; NULL is fine */
char *net_take(net_res_t *r);	/* keep the body (caller frees) */
struct sockaddr;
bool net_addr_forbidden(const struct sockaddr *sa);	/* anything but a public address (LAN and tailnet included) */
bool net_available(void);	/* a curl binary was found */
int net_active(void);		/* requests in flight */

/* worker thread: work(arg) there, then done(arg) on the LVGL thread (FIFO, one job at a time) */
void net_bg(void (*work)(void *), void (*done)(void *), void *arg);

/* helpers */
void net_urlencode(const char *s, char *out, int n);	/* query component (space = +) */
void net_urldecode(char *s);				/* in place, %xx and + */
bool net_resolve(const char *base, const char *ref, char *out, int n);	/* relative → absolute http(s) URL */
const char *net_host(const char *url, char *out, int n);	/* "en.wikipedia.org" ("www." kept) */
