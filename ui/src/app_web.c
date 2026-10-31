/* Web: a reader-style browser (no JavaScript, no CSS). The root page is the start page — an address
 * field that takes a URL or a search (DuckDuckGo's no-JS html endpoint), a Favorites grid (monogram tiles), Recently Visited and a Full Site button.
 * Every page opened slides in as a nav page (edge swipe / back = previous page); its text is rebuilt from
 * the HTML by html.c into headings, paragraphs (LVGL spangroups with tappable link spans), lists, quotes,
 * code and pictures. Fetching (net.c → curl child), parsing and image decoding (net_bg worker thread)
 * never run on the LVGL thread. Pages are built in batches as you scroll; pictures load when they come
 * near the screen, and decoded pixels across all open pages stay under a budget (the oldest pages give
 * theirs back and reload them when shown again). Forms work (text fields on Orchard's keyboard, textareas,
 * selects, checkboxes / radios, GET + POST with the hidden fields), a site's navigation links sit in a
 * collapsed "Site Menu" card at the top, and cookies persist in one jar per site (data_dir()/web/cookies/, menu ›
 * Clear Cookies & Data). Addresses on the LAN / tailnet open only from what the user typed or tapped themselves (or from
 * a page that is itself there) — a public page can't reach them (net.c). Page menu: Find on Page, Text Size,
 * Desktop / Mobile site (per site), Copy Link (Web's own clipboard: "Paste and Go"), Full Site (a renderer, when
 * linked in). Long press on a link: Open / Copy Link / Open in Full Site. Favorites + history: data_dir()/web/.
 * Site icons (web_icon.c): looked up when a page loads, shown from data_dir()/web/icons/ on the start page. */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "html.h"
#include "imgdec.h"
#include "net.h"
#include "orchard.h"
#include "web_icon.h"
#include "src/misc/cache/instance/lv_image_cache.h"	/* a tab's snapshot changed in place */
#include "src/misc/cache/instance/lv_image_header_cache.h"

#define SIDE 32
#define TEXT_W (SCR_W - 2 * SIDE)
#define TOP_H 100		/* omnibox bar under the status bar */
#define PILL_H 76		/* the omnibox capsule in it */
#define PILL_Y (STATUS_H + (TOP_H - PILL_H) / 2)
#define TB_H (96 + BOTTOM_PAD)	/* bottom toolbar: back, forward, new tab, tabs, menu */
#define PAGE_Y (STATUS_H + TOP_H)	/* where a web page's text starts */
#define MAX_TABS 20
#define GRID_GAP 28
#define THUMB_W ((SCR_W - 3 * GRID_GAP) / 2)	/* tab grid card = its snapshot's width */
#define THUMB_H 420
#define MAX_PAGE (2 << 20)	/* HTML bytes kept per page */
#define MAX_IMG_BYTES (3 << 20)
#define IMG_MAX_H 1100
#define IMG_DIRECT_MAX_H 2400	/* a picture opened on its own: tall infographics stay readable (~36 MB transient) */
#define IMG_BUDGET (48 << 20)	/* decoded picture bytes across open pages */
#define IMG_PARALLEL 2
#define BATCH 12		/* blocks built per step (spangroup layout is the costly part on the A8) */
#define ANCHOR_BATCH 48		/* per tick while building down to a #fragment (and <= ANCHOR_MS of work) */
#define ANCHOR_MS 40
#define MAX_WEB_PAGES 15	/* deeper than this, links replace the current page (ui.c's nav stack: 16 with the start page) */
#define MAX_FAV 60
#define MAX_HIST 200
#define TICK_MS 250

#define G_STAR "\xef\x80\x85"
#define G_STAR_O "\xef\x80\x86"
#define G_DOTS "\xef\x85\x81"	/* fa-ellipsis-h */
#define G_RELOAD "\xef\x80\xa1"
#define G_BOOKMARK "\xef\x80\xae"
#define G_HISTORY "\xef\x87\x9a"	/* fa-history */
#define G_WARN "\xef\x81\xb1"	/* fa-exclamation-triangle */
#define G_CLEAR "\xf3\xb0\x85\x99"	/* md-close_circle */
#define G_SQUARE_O "\xef\x82\x96"	/* fa-square-o */
#define G_CHECK_SQ "\xef\x85\x8a"	/* fa-check-square */
#define G_CIRCLE_O "\xef\x84\x8c"	/* fa-circle-o */
#define G_DOT_CIRCLE "\xef\x86\x92"	/* fa-dot-circle-o */
#define G_CARET_D "\xef\x83\x97"	/* fa-caret-down */
#define G_CHEVRON_U "\xef\x81\xb7"	/* fa-chevron-up */
#define G_CHEVRON_D "\xef\x81\xb8"	/* fa-chevron-down */
#define G_DESKTOP "\xef\x84\x88"	/* fa-desktop */
#define G_MOBILE "\xef\x84\x8b"	/* fa-mobile */
#define G_COPY "\xef\x83\x85"	/* fa-files-o */
#define G_COOKIE "\xf3\xb0\x86\x98"	/* md-cookie */
#define G_EXPAND "\xef\x81\xa5"	/* fa-expand: full site */
#define G_LINK "\xef\x83\x81"	/* fa-link */
#define G_PASTE "\xef\x83\xaa"	/* fa-clipboard */
/* browser chrome (Material Design Icons in the Nerd Font: thin, one weight) */
#define G_M_BACK "\xf3\xb0\x85\x81"	/* md-chevron_left */
#define G_M_FWD "\xf3\xb0\x85\x82"	/* md-chevron_right */
#define G_M_PLUS "\xf3\xb0\x90\x95"	/* md-plus */
#define G_M_DOTS "\xf3\xb0\x87\x98"	/* md-dots_horizontal */
#define G_M_RELOAD "\xf3\xb0\x91\x90"	/* md-refresh */
#define G_M_CLOSE "\xf3\xb0\x85\x96"	/* md-close */
#define G_M_LOCK "\xf3\xb0\x8c\xbe"	/* md-lock */
#define G_M_STAR "\xf3\xb0\x93\x8e"	/* md-star */
#define G_M_STAR_O "\xf3\xb0\x93\x92"	/* md-star_outline */
#define G_M_HISTORY "\xf3\xb0\x8b\x9a"	/* md-history */
#define G_M_COPY "\xf3\xb0\x86\x8f"	/* md-content_copy */
#define G_M_FIND "\xf3\xb0\x8d\x89"	/* md-magnify */
#define G_M_DESKTOP "\xf3\xb0\x8d\xb9"	/* md-monitor */
#define G_M_MOBILE "\xf3\xb0\x84\x9c"	/* md-cellphone */
#define G_M_COOKIE "\xf3\xb0\x86\x98"	/* md-cookie */
#define G_M_FULL "\xf3\xb0\x8f\x8c"	/* md-open_in_new */
#define G_M_TEXT "\xf3\xb0\x89\xbf"	/* md-format_size */
#define G_M_NEWTAB "\xf3\xb0\x9d\x9c"	/* md-tab_plus */
#define G_M_PENCIL "\xf3\xb0\xb2\xb6"	/* md-pencil_outline */
#define G_M_TAB_X "\xf3\xb0\x85\x96"	/* md-close (close tab) */

/* optional full-site renderer (none ships with this project; another build may link one in). Menu items calling it only show when it is. */
void web_full_open(const char *url) __attribute__((weak));

/* ---------- favorites + history ---------- */

struct entry { char url[1024]; char title[160]; long when; };
static struct entry favs[MAX_FAV], hist[MAX_HIST];
static int nfav, nhist;	/* hist: oldest first */
static bool store_loaded;

static const char *web_dir(void)
{
	static char d[300];
	snprintf(d, sizeof(d), "%s/web", data_dir());
	mkdir(d, 0755);
	return d;
}

static void store_path(char *b, int n, const char *name) { snprintf(b, n, "%s/%s", web_dir(), name); }

static void clean_field(char *s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\n' || *s == '\r')
			*s = ' ';
}

static int load_list(const char *name, struct entry *e, int max, bool timed)
{
	char path[400], line[1400];
	store_path(path, sizeof(path), name);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	int n = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		char *p = line, *t1 = strchr(p, '\t');
		if (!t1)
			continue;
		long when = 0;
		if (timed) {
			when = atol(p);
			p = t1 + 1;
			t1 = strchr(p, '\t');
			if (!t1)
				continue;
		}
		*t1 = 0;
		if (n == max) {	/* keep the newest max */
			memmove(e, e + 1, (max - 1) * sizeof(*e));
			n--;
		}
		snprintf(e[n].url, sizeof(e[n].url), "%s", p);
		snprintf(e[n].title, sizeof(e[n].title), "%s", t1 + 1);
		e[n].when = when;
		n++;
	}
	fclose(f);
	return n;
}

static void save_list(const char *name, const struct entry *e, int n, bool timed)
{
	char path[400], tmp[410];
	store_path(path, sizeof(path), name);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int i = 0; i < n; i++) {
		if (timed)
			fprintf(f, "%ld\t", e[i].when);
		fprintf(f, "%s\t%s\n", e[i].url, e[i].title);
	}
	if (fclose(f) == 0)
		rename(tmp, path);
	else
		unlink(tmp);
}

static void store_load(void)
{
	if (store_loaded)
		return;
	store_loaded = true;
	nfav = load_list("favorites.txt", favs, MAX_FAV, false);
	if (nfav < 0) {	/* first run: sites that read well without JavaScript */
		static const char *const def[][2] = {
			{ "https://en.m.wikipedia.org/wiki/Main_Page", "Wikipedia" },
			{ "https://text.npr.org/", "NPR (text)" },
			{ "https://lite.cnn.com/", "CNN Lite" },
			{ "https://news.ycombinator.com/", "Hacker News" },
			{ "https://wiki.archlinux.org/title/Main_page", "ArchWiki" },
		};
		nfav = 0;
		for (unsigned i = 0; i < sizeof(def) / sizeof(def[0]); i++) {
			snprintf(favs[nfav].url, sizeof(favs[0].url), "%s", def[i][0]);
			snprintf(favs[nfav].title, sizeof(favs[0].title), "%s", def[i][1]);
			nfav++;
		}
		save_list("favorites.txt", favs, nfav, false);
	}
	nhist = load_list("history.txt", hist, MAX_HIST, true);
	if (nhist < 0)
		nhist = 0;
}

static void root_build(void);

/* sites the user asked the desktop version of (web/desktop.txt, one host a line) */
#define MAX_DESK 64
static char desk[MAX_DESK][128];
static int ndesk = -1;

static void desk_load(void)
{
	if (ndesk >= 0)
		return;
	ndesk = 0;
	char path[400], line[200];
	store_path(path, sizeof(path), "desktop.txt");
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	while (ndesk < MAX_DESK && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (*line)
			snprintf(desk[ndesk++], sizeof(desk[0]), "%s", line);
	}
	fclose(f);
}

static bool desk_on(const char *url)
{
	char h[256];
	net_host(url, h, sizeof(h));
	desk_load();
	for (int i = 0; i < ndesk; i++)
		if (!strcmp(desk[i], h))
			return true;
	return false;
}

static void desk_set(const char *url, bool on)
{
	char h[256];
	net_host(url, h, sizeof(h));
	desk_load();
	int i = 0;
	while (i < ndesk && strcmp(desk[i], h))
		i++;
	if (on && i == ndesk && ndesk < MAX_DESK)
		snprintf(desk[ndesk++], sizeof(desk[0]), "%s", h);
	else if (!on && i < ndesk)
		memmove(desk[i], desk[i + 1], (--ndesk - i) * sizeof(desk[0]));
	char path[400];
	store_path(path, sizeof(path), "desktop.txt");
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	for (int k = 0; k < ndesk; k++)
		fprintf(f, "%s\n", desk[k]);
	fclose(f);
}

/* text size steps (%): few, so the font cache (fonts.c) stays small */
static const int tsizes[] = { 85, 100, 115, 130, 150 };
#define NTSIZES ((int)(sizeof(tsizes) / sizeof(tsizes[0])))
static int tsize_i = -1;

static int tsize(void)
{
	if (tsize_i < 0) {
		tsize_i = prefs_int("web_text", 1);
		if (tsize_i < 0 || tsize_i >= NTSIZES)
			tsize_i = 1;
	}
	return tsizes[tsize_i];
}

static int sz(int px)	/* a page font size at the current text size (even px: fewer distinct fonts) */
{
	int v = (px * tsize() + 50) / 100;
	return v & ~1;
}

static char clip[2048];	/* "Copy Link": the Web app's own clipboard (Orchard has no system one) */

static int fav_find(const char *url)
{
	for (int i = 0; i < nfav; i++)
		if (!strcmp(favs[i].url, url))
			return i;
	return -1;
}

static void fav_toggle(const char *url, const char *title)
{
	int i = fav_find(url);
	if (i >= 0) {
		memmove(favs + i, favs + i + 1, (nfav - i - 1) * sizeof(favs[0]));
		nfav--;
	} else {
		if (nfav == MAX_FAV)
			return;
		snprintf(favs[nfav].url, sizeof(favs[0].url), "%s", url);
		snprintf(favs[nfav].title, sizeof(favs[0].title), "%s", title && *title ? title : url);
		clean_field(favs[nfav].title);
		nfav++;
	}
	save_list("favorites.txt", favs, nfav, false);
	root_build();	/* the start page is under the web pages: rebuilt unseen */
}

static void hist_add(const char *url, const char *title)
{
	for (int i = nhist - 1; i >= 0; i--)	/* a revisit moves to the top */
		if (!strcmp(hist[i].url, url)) {
			memmove(hist + i, hist + i + 1, (nhist - i - 1) * sizeof(hist[0]));
			nhist--;
			break;
		}
	if (nhist == MAX_HIST) {
		memmove(hist, hist + 1, (MAX_HIST - 1) * sizeof(hist[0]));
		nhist--;
	}
	struct entry *e = &hist[nhist++];
	snprintf(e->url, sizeof(e->url), "%s", url);
	snprintf(e->title, sizeof(e->title), "%s", title && *title ? title : url);
	clean_field(e->title);
	clean_field(e->url);
	e->when = time(NULL);
	save_list("history.txt", hist, nhist, true);
	root_build();
}

/* ---------- URL / search input ---------- */

/* "198.51.100.1", "100.64.0.1:8080" */
static bool dotted_quad(const char *s, int n)
{
	int parts = 0, digits = 0;
	for (int i = 0; i < n; i++) {
		if (isdigit((unsigned char)s[i])) {
			if (++digits > 3)
				return false;
		} else if (s[i] == '.' && digits) {
			parts++;
			digits = 0;
		} else {
			return false;
		}
	}
	return digits && parts == 3;
}

/* host (+ :port) typed without a scheme that is a LAN / tailnet name or address: http://, not https:// */
static bool local_host(const char *s)
{
	int hl = strcspn(s, "/?#:");
	const char *colon = s[hl] == ':' ? s + hl : NULL;
	if (colon && strncmp(colon, ":443", 4))
		return true;	/* a port of its own: a home service */
	if (dotted_quad(s, hl) || !memchr(s, '.', hl))
		return true;	/* IP address, single label (MagicDNS "mypc") */
	static const char *const tl[] = { ".local", ".lan", ".home", ".internal", ".home.arpa", NULL };
	for (const char *const *t = tl; *t; t++) {
		int l = strlen(*t);
		if (hl > l && !strncasecmp(s + hl - l, *t, l))
			return true;
	}
	return false;
}

static bool looks_like_url(const char *s)
{
	if (strstr(s, "://"))
		return true;
	if (strchr(s, ' ') || !*s)
		return false;
	if (!strncasecmp(s, "localhost", 9))
		return true;
	int hl = strcspn(s, "/?#:");
	if (dotted_quad(s, hl))
		return true;
	if (s[hl] == ':' && isdigit((unsigned char)s[hl + 1])) {	/* name:port */
		const char *q = s + hl + 1;
		while (isdigit((unsigned char)*q))
			q++;
		if (!*q || *q == '/' || *q == '?' || *q == '#')
			return true;
	}
	if (s[hl] == '/' && hl > 0)	/* "mypc/" */
		return true;
	const char *host_end = s + strcspn(s, "/?#");
	const char *dot = NULL;
	for (const char *p = s; p < host_end; p++)
		if (*p == '.')
			dot = p;
	if (!dot || dot == s)
		return false;
	int tld = 0;
	for (const char *p = dot + 1; p < host_end && *p != ':'; p++, tld++)
		if (!isalnum((unsigned char)*p))
			return false;
	return tld >= 2;
}

/* address bar text → URL to load; *search set when it became a search */
static bool input_url(const char *in, char *out, int n, bool *search)
{
	while (isspace((unsigned char)*in))
		in++;
	char t[1024];
	snprintf(t, sizeof(t), "%s", in);
	int l = strlen(t);
	while (l && isspace((unsigned char)t[l - 1]))
		t[--l] = 0;
	if (!l)
		return false;
	*search = false;
	if (looks_like_url(t)) {
		char tmp[1100];
		if (!strstr(t, "://"))
			snprintf(tmp, sizeof(tmp), "%s%s", local_host(t) ? "http://" : "https://", t);
		else
			snprintf(tmp, sizeof(tmp), "%s", t);
		/* resolving against "" normalises the URL but drops its #fragment (RFC 3986): put it back */
		const char *h = strchr(tmp, '#');
		if (!net_resolve(tmp, h ? h : "", out, n))
			snprintf(out, n, "%s", tmp);
		return true;
	}
	char enc[2048];
	net_urlencode(t, enc, sizeof(enc));
	snprintf(out, n, "https://html.duckduckgo.com/html/?q=%s", enc);
	*search = true;
	return true;
}

static bool is_ddg(const char *url) { return !strncmp(url, "https://html.duckduckgo.com/html", 32); }

/* the search words back out of a results URL */
static bool ddg_query(const char *url, char *out, int n)
{
	if (!is_ddg(url))
		return false;
	const char *q = strstr(url, "?q=");
	if (!q)
		q = strstr(url, "&q=");
	if (!q)
		return false;
	q += 3;
	snprintf(out, n, "%.*s", (int)strcspn(q, "&#"), q);
	net_urldecode(out);
	return true;
}

/* ---------- pages ---------- */

enum { IM_IDLE, IM_LOADING, IM_SHOWN, IM_FAILED };
struct wimg { lv_obj_t *box, *img; lv_image_dsc_t *dsc; net_req_t *req; int state, tries; };

/* bottom toolbar (one per page + the start page's): what it shows, so a sync only redraws changes */
struct bar { lv_obj_t *bk, *fw, *cnt; int bk_on, fw_on, n; };

struct wpage {
	int gen;
	char url[2048];		/* what was asked for; final URL after load */
	char title[256];
	bool search, loading, failed;
	/* how it is fetched */
	bool allow_priv;	/* may be on the LAN / tailnet: the user opened it, or a page there linked it */
	bool priv;		/* it is (its pictures and links may be too) */
	char *post;		/* form body for the next load (a reload after it is a GET) */
	char referer[2048];	/* the page it was opened from */
	int err;		/* last load's NET_E* */
	/* forms + extras */
	lv_obj_t **boxsg;	/* spangroups showing checkboxes / radios (redrawn when one changes) */
	int nboxsg, capboxsg;
	lv_obj_t *menu_card, *menu_list;	/* "Site Menu" */
	lv_obj_t *hl;		/* Find on Page: the block highlighted */
	long long refresh_at;	/* <meta refresh> due (lv_tick ms), 0 = none */
	int refreshes;
	int hl_want;		/* Find: block index + 1 to tint once anchor_scroll gets there */
	int tsz;		/* text size it was built at */
	lv_obj_t *page, *col, *tb, *host, *lock, *reload, *spinner, *status;
	lv_obj_t *prog;		/* loading bar along the omnibox bar's bottom edge */
	bool prog_on;
	uint32_t prog_t0;
	bool lazy;		/* a restored tab's older page: loaded when it is on top again */
	struct bar bar;
	net_req_t *req;
	html_doc_t *doc;
	int built;		/* blocks created */
	struct wimg *imgs;	/* doc->nimgs */
	lv_obj_t *more;		/* search: "More Results" button */
	char frag[256];		/* #fragment to scroll to once built */
	int anchor;		/* block index + 1 to bring to the top once tick() has built down to it; 0 = none */
};

static struct wpage *wps[MAX_WEB_PAGES + 1];	/* [1..nwp], nav depth = index */
static int nwp, gen_next = 1;
static long img_bytes;
static int img_active;
static lv_timer_t *ticker;
static char fwd[16][1024];
static int nfwd;

/* root page */
static lv_obj_t *root_col, *root_box, *addr_ta, *addr_clear, *addr_cancel;
/* address sheet over a web page */
static lv_obj_t *sheet, *sheet_ta, *sheet_box, *sheet_x;
static lv_obj_t *sel_ta;	/* a field whose text shows selected whole (sel_all) */

static struct wpage *top_page(void) { return nwp ? wps[nwp] : NULL; }

static struct wpage *page_by_gen(int gen)
{
	for (int i = 1; i <= nwp; i++)
		if (wps[i] && wps[i]->gen == gen)
			return wps[i];
	return NULL;
}

static void navigate(const char *url, bool from_link, bool user, const char *post);
static void page_load(struct wpage *w, const char *url);
static void tab_dirty(void);

/* ---------- pictures ---------- */

static void img_drop(struct wimg *im)
{
	net_cancel(im->req);
	if (im->req)
		img_active--;
	im->req = NULL;
	if (im->dsc) {
		if (im->img)
			lv_image_set_src(im->img, NULL);
		img_bytes -= im->dsc->data_size;
		img_free(im->dsc);
		im->dsc = NULL;
	}
	if (im->state == IM_LOADING || im->state == IM_SHOWN)
		im->state = IM_IDLE;
}

/* over budget: pictures on pages under the top one go first (they reload when shown again) */
static void img_budget(void)
{
	for (int i = 1; i < nwp && img_bytes > IMG_BUDGET; i++) {
		struct wpage *w = wps[i];
		if (!w || !w->doc)
			continue;
		for (int k = 0; k < w->doc->nimgs && img_bytes > IMG_BUDGET; k++)
			if (w->imgs[k].dsc)
				img_drop(&w->imgs[k]);
	}
}

struct ijob { int gen, idx; char *data; size_t len; int max_w, max_h; uint32_t bg; lv_image_dsc_t *out; const char *err; };

#define IMG_RADIUS 20

/* rounded corners drawn into the picture itself (no clip_corner mask while scrolling): opaque pictures get the
 * page colour outside the curve, see-through ones lose their alpha there; edge pixels blended (anti-aliased) */
static void img_round(lv_image_dsc_t *d, uint32_t bg)
{
	int w = d->header.w, h = d->header.h, r = IMG_RADIUS;
	if (w < 120 || h < 120)	/* icons, small logos: as they are */
		return;
	bool alpha = d->header.cf == LV_COLOR_FORMAT_ARGB8888;
	if (!alpha && d->header.cf != LV_COLOR_FORMAT_XRGB8888)
		return;
	int stride = d->header.stride / 4;
	uint32_t *px = (uint32_t *)d->data;
	for (int y = 0; y < r; y++)
		for (int x = 0; x < r; x++) {
			/* coverage of pixel (x, y) by the corner's circle (centre (r, r)): 4×4 samples, in 1/8 px units */
			int in = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) {
					int dx = 8 * r - (8 * x + 2 * sx + 1), dy = 8 * r - (8 * y + 2 * sy + 1);
					in += dx * dx + dy * dy <= 64 * r * r;
				}
			if (in == 16)
				continue;
			int cov = in * 255 / 16;
			int xs[2] = { x, w - 1 - x }, ys[2] = { y, h - 1 - y };
			for (int a = 0; a < 2; a++)
				for (int c = 0; c < 2; c++) {
					uint32_t *q = &px[ys[a] * stride + xs[c]];
					if (alpha) {
						uint32_t al = (*q >> 24) * cov / 255;
						*q = (*q & 0xffffff) | al << 24;
					} else {
						*q = mix(bg, *q, cov * 100 / 255) | 0xff000000;
					}
				}
		}
}

static void ijob_work(void *a)
{
	struct ijob *j = a;
	j->out = img_decode(j->data, j->len, j->max_w, j->max_h, &j->err);
	free(j->data);
	j->data = NULL;
	if (j->out)
		img_round(j->out, j->bg);
}

static void img_show(struct wpage *w, int idx, lv_image_dsc_t *d)
{
	struct wimg *im = &w->imgs[idx];
	im->dsc = d;
	im->state = IM_SHOWN;
	img_bytes += d->data_size;
	if (!im->box)
		return;
	lv_obj_set_size(im->box, d->header.w, d->header.h);
	/* see-through pictures (logos, diagrams) are mostly drawn for white pages: give them one */
	bool clear = d->header.cf == LV_COLOR_FORMAT_ARGB8888 && th.dark;
	lv_obj_set_style_bg_opa(im->box, clear ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_color(im->box, C(pal.label), 0);
	lv_obj_set_style_radius(im->box, d->header.w < 120 || d->header.h < 120 ? 0 : IMG_RADIUS, 0);
	if (!im->img) {
		lv_obj_clean(im->box);	/* the placeholder's glyph */
		im->img = lv_image_create(im->box);
		lv_obj_remove_flag(im->img, LV_OBJ_FLAG_CLICKABLE);
	}
	lv_image_set_src(im->img, d);
	lv_obj_center(im->img);
	img_budget();
}

static void img_failed(struct wpage *w, int idx)
{
	struct wimg *im = &w->imgs[idx];
	im->state = IM_FAILED;
	if (im->box)	/* nothing to show: the gap closes (the wrap around it too, or its height stays) */
		lv_obj_add_flag(lv_obj_get_parent(im->box), LV_OBJ_FLAG_HIDDEN);
}

static void ijob_done(void *a)
{
	struct ijob *j = a;
	struct wpage *w = page_by_gen(j->gen);
	if (w && w->doc && j->idx < w->doc->nimgs && w->imgs[j->idx].state == IM_LOADING) {
		if (j->out)
			img_show(w, j->idx, j->out);
		else
			img_failed(w, j->idx);
		j->out = NULL;
	}
	img_free(j->out);
	free(j);
}

/* (page gen, picture index) packed into the callback's ud: nothing to free when img_drop() cancels */
struct iref { int gen, idx; };
_Static_assert(sizeof(uintptr_t) >= 8, "iref packs two ints into a pointer");
static void *iref_pack(int gen, int idx) { return (void *)((uintptr_t)(uint32_t)gen << 32 | (uint32_t)idx); }
static struct iref iref_unpack(void *ud) { return (struct iref){ (int)((uintptr_t)ud >> 32), (int)(uint32_t)(uintptr_t)ud }; }

static void img_fetched(net_res_t *r, void *ud)
{
	struct iref rf = iref_unpack(ud), *ref = &rf;
	struct wpage *w = page_by_gen(ref->gen);
	img_active--;
	if (!w || !w->doc || ref->idx >= w->doc->nimgs) {
		return;
	}
	struct wimg *im = &w->imgs[ref->idx];
	im->req = NULL;
	if (r->err || r->status >= 400 || r->truncated || !r->len) {
		if (!r->err || ++im->tries >= 2)
			img_failed(w, ref->idx);
		else
			im->state = IM_IDLE;	/* one more go later */
		return;
	}
	struct ijob *j = calloc(1, sizeof(*j));
	if (!j) {
		img_failed(w, ref->idx);
		return;
	}
	j->gen = ref->gen;
	j->idx = ref->idx;
	j->len = r->len;
	j->data = net_take(r);
	const html_img_t *hi = &w->doc->imgs[ref->idx];
	j->bg = pal.bg;
	j->max_w = w->doc->nimgs == 1 && w->doc->nb == 1 ? SCR_W : TEXT_W;
	j->max_h = IMG_MAX_H;
	if (hi->w > 0 && hi->w * 2 < j->max_w)	/* the page's own size for it, at @2x */
		j->max_w = hi->w * 2;
	net_bg(ijob_work, ijob_done, j);
}

static bool same_site(const char *a, const char *b)
{
	char ha[256], hb[256], sa[256], sb[256];
	net_site(net_host(a, ha, sizeof(ha)), sa, sizeof(sa));
	net_site(net_host(b, hb, sizeof(hb)), sb, sizeof(sb));
	return *sa && !strcmp(sa, sb);
}

static void img_start(struct wpage *w, int idx)
{
	struct wimg *im = &w->imgs[idx];
	im->state = IM_LOADING;
	const char *u = w->doc->imgs[idx].url;
	net_opts_t o = { .url = u, .max_bytes = MAX_IMG_BYTES, .timeout_s = 25, .referer = w->url,
			 .allow_private = w->priv, .desktop = desk_on(w->url),
			 .cookies = same_site(u, w->url) ? NET_CK_READ : NET_CK_NONE };
	im->req = net_fetch(&o, img_fetched, iref_pack(w->gen, idx));
	if (!im->req) {
		img_failed(w, idx);
		return;
	}
	img_active++;
}

/* ---------- building blocks ---------- */

static lv_font_t *block_font(enum html_kind k)
{
	switch (k) {
	case HB_H1: return font(F_TEXT_BOLD, sz(52));
	case HB_H2: return font(F_TEXT_BOLD, sz(44));
	case HB_H3: return font(F_TEXT_SEMI, sz(38));
	case HB_H4: return font(F_TEXT_SEMI, sz(34));
	case HB_CAPTION: return font(F_TEXT, sz(26));
	case HB_PRE: return font(F_MONO, sz(24));
	default: return font(F_TEXT, sz(32));
	}
}

/* tap on a paragraph: the run (link or checkbox) under the finger (a few px of slack), -1 none */
/* a table cell's spangroup shows the runs from this one on (span i = run r0 + i); a paragraph's: all of them */
static int cell_r0(lv_obj_t *sg, const html_block_t *b)
{
	if (b->kind != HB_ROW)
		return 0;
	int c = lv_obj_get_index(sg), off = c >= 0 && c < b->ncells ? b->cells[c].off : 0, r = 0;
	while (r < b->nruns && b->runs[r].off + b->runs[r].len <= off)
		r++;
	return r;
}

static int span_run_at(lv_obj_t *sg, const html_block_t *b, lv_point_t p)
{
	static const int off[][2] = { { 0, 0 }, { 0, -14 }, { 0, 14 }, { -16, 0 }, { 16, 0 } };
	for (unsigned k = 0; k < sizeof(off) / sizeof(off[0]); k++) {
		lv_point_t q = { p.x + off[k][0], p.y + off[k][1] };
		lv_span_t *s = lv_spangroup_get_span_by_point(sg, &q);
		if (!s)
			continue;
		int n = lv_spangroup_get_span_count(sg), r0 = cell_r0(sg, b);
		for (int i = 0; i < n && r0 + i < b->nruns; i++)
			if (lv_spangroup_get_child(sg, i) == s)
				return b->runs[r0 + i].link >= 0 ? r0 + i : -1;
		return -1;
	}
	return -1;
}

static int span_link_at(lv_obj_t *sg, const html_block_t *b, lv_point_t p)
{
	int i = span_run_at(sg, b, p);
	return i >= 0 && !(b->runs[i].flags & HR_FIELD) ? b->runs[i].link : -1;
}

static struct wpage *building;	/* build_more's page, for spans() */
static lv_span_t *pressed_span;
static lv_obj_t *pressed_sg;

static void span_press(lv_event_t *e)
{
	lv_obj_t *sg = lv_event_get_target(e);
	const html_block_t *b = lv_event_get_user_data(e);
	lv_event_code_t code = lv_event_get_code(e);
	if (pressed_span && (code != LV_EVENT_PRESSED || pressed_sg != sg)) {
		lv_style_set_text_opa(lv_span_get_style(pressed_span), LV_OPA_COVER);
		if (pressed_sg)
			lv_obj_invalidate(pressed_sg);
		pressed_span = NULL;
		pressed_sg = NULL;
	}
	if (code != LV_EVENT_PRESSED)
		return;
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (span_run_at(sg, b, p) < 0)
		return;
	lv_span_t *s = lv_spangroup_get_span_by_point(sg, &p);
	if (!s)
		return;
	pressed_span = s;
	pressed_sg = sg;
	lv_style_set_text_opa(lv_span_get_style(s), LV_OPA_50);
	lv_obj_invalidate(sg);
}

static void box_toggle(struct wpage *w, int fi);
static void link_menu(const char *url, int y);
static void page_leave(void);

static void span_click(lv_event_t *e)
{
	lv_obj_t *sg = lv_event_get_target(e);
	const html_block_t *b = lv_event_get_user_data(e);
	struct wpage *w = top_page();
	if (!w || !w->doc)
		return;
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	int ri = span_run_at(sg, b, p);
	if (ri < 0)
		return;
	int li = b->runs[ri].link;
	if (b->runs[ri].flags & HR_FIELD) {
		if (li < w->doc->nfields)
			box_toggle(w, li);
	} else if (li < w->doc->nlinks) {
		ui_haptic_tap();
		navigate(w->doc->links[li], true, false, NULL);
	}
}

static void span_long(lv_event_t *e)
{
	lv_obj_t *sg = lv_event_get_target(e);
	const html_block_t *b = lv_event_get_user_data(e);
	struct wpage *w = top_page();
	if (!w || !w->doc)
		return;
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	int li = span_link_at(sg, b, p);
	if (li >= 0 && li < w->doc->nlinks) {
		ui_haptic_tap();
		link_menu(w->doc->links[li], p.y);
	}
}

/* what a checkbox / radio run shows: its glyph + a space before the label */
static const char *box_glyph(const html_field_t *f)
{
	if (f->type == HF_RADIO)
		return f->checked ? G_DOT_CIRCLE " " : G_CIRCLE_O " ";
	return f->checked ? G_CHECK_SQ " " : G_SQUARE_O " ";
}

/* text[from, to) of block b as a spangroup w px wide: links, bold, code, <small> and checkboxes as styled spans
 * (span i = run i, or for a table cell the cell's first run + i) */
static lv_obj_t *spans_rng(lv_obj_t *parent, const html_block_t *b, int from, int to, int w, lv_font_t *f, uint32_t color,
			   lv_font_t *bold)	/* font of bold runs, NULL = f */
{
	lv_obj_t *sg = lv_spangroup_create(parent);
	lv_obj_set_width(sg, w);
	lv_obj_set_height(sg, LV_SIZE_CONTENT);
	lv_obj_set_style_text_font(sg, f, 0);
	lv_obj_set_style_text_color(sg, C(color), 0);
	int lh = lv_font_get_line_height(f);
	/* ~1.5 lines for body text, tighter for headings and small text */
	lv_obj_set_style_text_line_space(sg, b->kind == HB_P || b->kind == HB_LI || b->kind == HB_QUOTE ? lh / 4 + 1
					 : b->kind == HB_ROW || b->kind == HB_CAPTION ? lh / 6 : lh / 10, 0);
	lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);
	bool links = false, boxes = false;
	char tmp[4096], prev[4096];
	lv_span_t *ps = NULL;	/* the span before, its text in prev */
	struct wpage *wp = building;	/* the page whose blocks build_more is making */
	const html_doc_t *d = wp ? wp->doc : NULL;
	for (int i = 0; i < b->nruns; i++) {
		const html_run_t *r = &b->runs[i];
		int ro = r->off > from ? r->off : from, re = r->off + r->len < to ? r->off + r->len : to;
		if (re <= ro)
			continue;
		int n = re - ro < (int)sizeof(tmp) - 1 ? re - ro : (int)sizeof(tmp) - 1;
		memcpy(tmp, b->text + ro, n);
		tmp[n] = 0;
		if ((r->flags & HR_BOX) && d && r->link >= 0 && r->link < d->nfields) {
			snprintf(tmp, sizeof(tmp), "%s", box_glyph(&d->fields[r->link]));
			boxes = true;
		}
		/* punctuation right after a link / bold word stays on its line: LVGL may break between any two spans,
		 * so it moves to the end of the span before (span i still = run i, even when this one is left empty) */
		int pn = 0;
		while (pn < 3 && tmp[pn] && strchr(".,;:!?)\"'", tmp[pn]))
			pn++;
		if (pn && ps && *prev && prev[strlen(prev) - 1] != ' ' && strlen(prev) + pn < sizeof(prev) &&
		    !(r->flags & HR_BOX)) {
			strncat(prev, tmp, pn);
			lv_spangroup_set_span_text(sg, ps, prev);
			memmove(tmp, tmp + pn, strlen(tmp + pn) + 1);
		}
		/* and an opening bracket / quote before one goes with it: "(github.com)" */
		size_t pl = ps ? strlen(prev) : 0;
		if (pl >= 2 && strchr("(\"[", prev[pl - 1]) && prev[pl - 2] == ' ' && tmp[0] && tmp[0] != ' ' &&
		    strlen(tmp) + 2 < sizeof(tmp) && !(r->flags & HR_BOX)) {
			memmove(tmp + 1, tmp, strlen(tmp) + 1);
			tmp[0] = prev[pl - 1];
			prev[pl - 1] = 0;
			lv_spangroup_set_span_text(sg, ps, prev);
		}
		lv_span_t *s = lv_spangroup_add_span(sg);
		lv_spangroup_set_span_text(sg, s, tmp);
		ps = s;
		memcpy(prev, tmp, strlen(tmp) + 1);
		lv_style_t *st = lv_span_get_style(s);
		bool small = (r->flags & HR_SMALL) && b->kind != HB_CAPTION && lh > lv_font_get_line_height(font(F_TEXT, sz(26)));
		if (r->flags & HR_CODE) {
			int cs = small ? sz(22) : lh * 76 / 100 > 36 ? 36 : lh * 76 / 100 < 20 ? 20 : (lh * 76 / 100) & ~1;	/* looks as big as the text */
			lv_style_set_text_font(st, font(F_MONO, cs));
		} else if (small) {
			lv_style_set_text_font(st, font((r->flags & HR_BOLD) ? F_TEXT_SEMI : F_TEXT, sz(26)));
		} else if ((r->flags & HR_BOLD) && bold) {
			lv_style_set_text_font(st, bold);
		}
		if (r->flags & HR_BOX)
			lv_style_set_text_font(st, font(F_MONO, sz(36)));
		if (r->link >= 0) {
			if (!(r->flags & HR_FIELD) || (r->flags & HR_BOX))
				lv_style_set_text_color(st, C(small ? mix(pal.tint, pal.label2, 35) : pal.tint));
			links = true;
		} else if (small) {
			lv_style_set_text_color(st, C(pal.label2));
		}
	}
	lv_spangroup_refresh(sg);
	if (boxes && wp) {	/* redrawn when a checkbox / radio changes */
		if (wp->nboxsg == wp->capboxsg) {
			int nc = wp->capboxsg ? wp->capboxsg * 2 : 8;
			lv_obj_t **nb = realloc(wp->boxsg, nc * sizeof(*nb));
			if (nb) {
				wp->boxsg = nb;
				wp->capboxsg = nc;
			}
		}
		if (wp->nboxsg < wp->capboxsg)
			wp->boxsg[wp->nboxsg++] = sg;
	}
	if (links) {
		lv_obj_add_flag(sg, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(sg, span_click, LV_EVENT_SHORT_CLICKED, (void *)b);
		lv_obj_add_event_cb(sg, span_long, LV_EVENT_LONG_PRESSED, (void *)b);
		lv_obj_add_event_cb(sg, span_press, LV_EVENT_PRESSED, (void *)b);
		lv_obj_add_event_cb(sg, span_press, LV_EVENT_RELEASED, (void *)b);
		lv_obj_add_event_cb(sg, span_press, LV_EVENT_PRESS_LOST, (void *)b);
		lv_obj_add_event_cb(sg, span_press, LV_EVENT_SCROLL_BEGIN, (void *)b);
	}
	return sg;
}

static lv_obj_t *spans(lv_obj_t *parent, const html_block_t *b, int w, lv_font_t *f, uint32_t color)
{
	return spans_rng(parent, b, 0, b->len, w, f, color, f == font(F_TEXT, sz(32)) ? font(F_TEXT_SEMI, sz(32)) : NULL);
}

static void result_click(lv_event_t *e)
{
	const html_block_t *b = lv_event_get_user_data(e);
	struct wpage *w = top_page();
	if (w && w->doc && b->nruns && b->runs[0].link >= 0 && b->runs[0].link < w->doc->nlinks)
		navigate(w->doc->links[b->runs[0].link], true, false, NULL);
}

static void result_long(lv_event_t *e)
{
	const html_block_t *b = lv_event_get_user_data(e);
	struct wpage *w = top_page();
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (w && w->doc && b->nruns && b->runs[0].link >= 0 && b->runs[0].link < w->doc->nlinks) {
		ui_haptic_tap();
		link_menu(w->doc->links[b->runs[0].link], p.y);
	}
}

/* ---------- forms ---------- */

static void form_submit(int form, int submitter)
{
	struct wpage *w = top_page();
	if (!w || !w->doc || form < 0)
		return;
	char *post = NULL, *u = html_form_url(w->doc, form, submitter, &post);
	if (!u)
		return;
	if (kbd_visible())
		kbd_hide();
	ui_haptic_tap();
	if (strlen(u) >= 2048 || (post && strlen(post) > (64 << 10)))
		ui_alert("Can\xe2\x80\x99t Send Form", "What you entered is too long to send.", (const char *[]){ "OK" }, 1, NULL, NULL);
	else
		navigate(u, true, false, post);
	free(u);
	free(post);
}

/* the field a text box edits: field index in its user data (the doc is the top page's) */
static html_field_t *field_of(lv_event_t *e)
{
	struct wpage *w = top_page();
	int fi = (int)(intptr_t)lv_event_get_user_data(e);
	return w && w->doc && fi >= 0 && fi < w->doc->nfields ? &w->doc->fields[fi] : NULL;
}

static void field_changed(lv_event_t *e)
{
	html_field_t *f = field_of(e);
	if (!f)
		return;
	char *v = strdup(lv_textarea_get_text(lv_event_get_target(e)));
	if (v) {
		free(f->value);
		f->value = v;
	}
}

static void field_ready(lv_event_t *e)	/* "done" / return on a one-line field: submit, as browsers do */
{
	struct wpage *w = top_page();
	html_field_t *f = field_of(e);
	if (!f || !w)
		return;
	int sub = -1;	/* the form's first submit button is the submitter */
	for (int i = 0; i < w->doc->nfields && sub < 0; i++)
		if (w->doc->fields[i].form == f->form &&
		    (w->doc->fields[i].type == HF_SUBMIT || w->doc->fields[i].type == HF_IMAGE))
			sub = i;
	form_submit(f->form, sub);
}

static void field_kbd(lv_event_t *e)	/* keyboard up for a field: room for it under the page, field in view */
{
	struct wpage *w = top_page();
	html_field_t *f = field_of(e);
	if (!w || !w->col)
		return;
	lv_obj_t *ta = lv_event_get_target(e);
	if (lv_event_get_code(e) == EV_KBD_SHOWN) {
		if (!f || (f->type != HF_TEXT && f->type != HF_TEXTAREA))	/* !f: the OpenSearch box */
			kbd_no_autocap();
		lv_obj_set_style_pad_bottom(w->col, kbd_height(KBD_TEXT) + 60, 0);
		lv_obj_update_layout(w->col);
		lv_area_t a;
		lv_obj_get_coords(lv_obj_get_parent(ta), &a);
		int room = SCR_H - kbd_height(KBD_TEXT) - 40;
		if (a.y2 > room || a.y1 < PAGE_Y)
			lv_obj_scroll_by_bounded(w->col, 0, a.y2 > room ? room - a.y2 : PAGE_Y + 20 - a.y1, LV_ANIM_ON);
	} else {
		lv_obj_set_style_pad_bottom(w->col, TB_H + 40, 0);
	}
}

static void submit_click(lv_event_t *e)
{
	struct wpage *w = top_page();
	html_field_t *f = field_of(e);
	if (f && w)
		form_submit(f->form, (int)(f - w->doc->fields));
}

static void textarea_click(lv_event_t *e)
{
	kbd_attach(lv_event_get_target(e), KBD_TEXT);
}

static void box_refresh(struct wpage *w)
{
	for (int i = 0; i < w->nboxsg; i++) {
		lv_obj_t *sg = w->boxsg[i];
		const html_block_t *b = NULL;
		for (uint32_t k = 0; k < lv_obj_get_event_count(sg) && !b; k++) {
			lv_event_dsc_t *ed = lv_obj_get_event_dsc(sg, k);
			if (lv_event_dsc_get_cb(ed) == span_click)
				b = lv_event_dsc_get_user_data(ed);
		}
		if (!b)
			continue;
		int n = lv_spangroup_get_span_count(sg), r0 = cell_r0(sg, b);
		for (int r = r0; r < b->nruns && r - r0 < n; r++)
			if ((b->runs[r].flags & HR_BOX) && b->runs[r].link < w->doc->nfields)
				lv_spangroup_set_span_text(sg, lv_spangroup_get_child(sg, r - r0), box_glyph(&w->doc->fields[b->runs[r].link]));
		lv_spangroup_refresh(sg);
	}
}

static void box_toggle(struct wpage *w, int fi)
{
	html_field_t *f = &w->doc->fields[fi];
	if (f->disabled)
		return;
	ui_haptic_tap();
	if (f->type == HF_RADIO) {	/* one of its group (same form + name) */
		for (int i = 0; i < w->doc->nfields; i++) {
			html_field_t *o = &w->doc->fields[i];
			if (o != f && o->type == HF_RADIO && o->form == f->form && o->name && f->name && !strcmp(o->name, f->name))
				o->checked = false;
		}
		f->checked = true;
	} else {
		f->checked = !f->checked;
	}
	box_refresh(w);
}

/* select: a popover for a few options, a page of rows for many */
static int sel_field = -1, sel_gen;
static lv_obj_t *sel_lab;	/* the capsule's label (its page is the top web page while the choice is made) */

static void sel_set(int opt)
{
	struct wpage *w = top_page();
	if (!w || !w->doc || w->gen != sel_gen || sel_field < 0 || sel_field >= w->doc->nfields)
		return;
	html_field_t *f = &w->doc->fields[sel_field];
	if (opt < 0 || opt >= f->nopt)
		return;
	f->sel = opt;
	lv_obj_t *lab = sel_lab;
	if (lab)
		lv_label_set_text(lab, f->opt[opt] && *f->opt[opt] ? f->opt[opt] : " ");
}

static void sel_menu_pick(int id) { sel_set(id); }

static void sel_row_click(lv_event_t *e)
{
	int opt = (int)(intptr_t)lv_event_get_user_data(e);
	nav_pop();	/* back to the page (its objects are still there) */
	sel_set(opt);
}

static void select_click(lv_event_t *e)
{
	struct wpage *w = top_page();
	html_field_t *f = field_of(e);
	if (!f || !w || !f->nopt || f->disabled)
		return;
	sel_field = (int)(f - w->doc->fields);
	sel_gen = w->gen;
	sel_lab = lv_obj_get_child(lv_event_get_current_target(e), 0);
	if (f->nopt <= 10) {
		ui_menu_item_t it[10];
		for (int i = 0; i < f->nopt; i++)
			it[i] = (ui_menu_item_t){ i, MI_CHECK | (i == f->sel ? MI_ON : 0), NULL,
						  f->opt[i] && *f->opt[i] ? f->opt[i] : " " };
		lv_area_t a;
		lv_obj_get_coords(lv_event_get_target(e), &a);
		ui_menu(it, f->nopt, a.x2, a.y2 + 10, a.y1 - 10, sel_menu_pick);
		return;
	}
	if (nwp >= MAX_WEB_PAGES)
		return;
	lv_obj_t *c = nav_push(f->label ? f->label : "Choose");
	if (!c)
		return;
	wps[++nwp] = NULL;	/* not a web page: page_leave just pops it */
	nav_on_leave(page_leave);
	lv_obj_t *sec = ui_section(c, NULL, NULL);
	for (int i = 0; i < f->nopt && i < 150; i++) {
		lv_obj_t *r = ui_row(sec, NULL, 0, f->opt[i] && *f->opt[i] ? f->opt[i] : " ");
		if (i == f->sel)
			ui_row_check(r, true);
		ui_row_tap(r, sel_row_click, (void *)(intptr_t)i);
	}
}

static void build_field(struct wpage *w, const html_block_t *b)
{
	html_field_t *f = &w->doc->fields[b->field];
	void *ud = (void *)(intptr_t)b->field;
	lv_obj_t *col = w->col;
	lv_obj_t *wrap = box(col);	/* one child per block (anchors, Find), the control inside */
	lv_obj_set_size(wrap, TEXT_W, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_ver(wrap, 10, 0);
	lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
	bool lone = f->type == HF_SUBMIT || f->type == HF_IMAGE;	/* a site search's button: Return does it */
	for (int i = 0; lone && i < w->doc->nfields; i++) {
		const html_field_t *o = &w->doc->fields[i];
		lone = !(o->form == f->form && (o->type == HF_SEARCH || o->type == HF_TEXT));
	}
	if (!lone && w->doc->forms[f->form].search && (f->type == HF_SUBMIT || f->type == HF_IMAGE)) {
		lv_obj_add_flag(wrap, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	switch (f->type) {
	case HF_SUBMIT:
	case HF_IMAGE: {
		lv_obj_t *bt = ui_button(wrap, f->label ? f->label : "Submit", true);
		lv_obj_set_style_max_width(bt, TEXT_W, 0);
		bool login = false;	/* a sign-in form's button spans the column, as in iOS */
		for (int k = 0; k < w->doc->nfields && !login; k++)
			login = w->doc->fields[k].form == f->form && w->doc->fields[k].type == HF_PASSWORD;
		if (login) {
			lv_obj_set_width(bt, TEXT_W);
			lv_obj_set_style_margin_top(bt, 12, 0);
			lv_obj_t *l = lv_obj_get_child(bt, 0);
			if (l)
				lv_obj_center(l);
		}
		lv_obj_add_event_cb(bt, submit_click, LV_EVENT_CLICKED, ud);
		if (f->disabled)
			lv_obj_set_style_opa(bt, LV_OPA_50, 0);
		break;
	}
	case HF_SELECT: {
		lv_obj_t *cap = box(wrap);
		lv_obj_set_size(cap, LV_SIZE_CONTENT, 76);
		lv_obj_set_style_min_width(cap, 240, 0);
		lv_obj_set_style_max_width(cap, TEXT_W, 0);
		lv_obj_set_style_radius(cap, 20, 0);
		lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(cap, C(pal.fill), 0);
		lv_obj_set_style_bg_color(cap, C(pal.cell_hi), LV_STATE_PRESSED);
		lv_obj_set_style_pad_hor(cap, 26, 0);
		lv_obj_set_flex_flow(cap, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(cap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(cap, 20, 0);
		lv_obj_add_flag(cap, LV_OBJ_FLAG_CLICKABLE);
		const char *cur = f->sel >= 0 && f->sel < f->nopt && f->opt[f->sel] && *f->opt[f->sel] ? f->opt[f->sel] : " ";
		lv_obj_t *l = text(cap, cur, font(F_TEXT, sz(32)), pal.label);
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_set_style_max_width(l, TEXT_W - 120, 0);
		ui_icon(cap, G_CARET_D, font(F_MONO, 30), pal.label2);
		lv_obj_add_event_cb(cap, select_click, LV_EVENT_CLICKED, ud);
		break;
	}
	case HF_TEXTAREA: {
		lv_obj_t *ta = lv_textarea_create(wrap);
		lv_obj_set_size(ta, TEXT_W, lv_font_get_line_height(font(F_TEXT, sz(32))) * 5 + 40);
		lv_obj_set_style_radius(ta, 20, 0);
		lv_obj_set_style_border_width(ta, 0, 0);
		lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(ta, C(pal.fill), 0);
		lv_obj_set_style_pad_all(ta, 20, 0);
		lv_obj_set_style_text_font(ta, font(F_TEXT, sz(32)), 0);
		lv_obj_set_style_text_color(ta, C(pal.label), 0);
		lv_obj_set_style_text_color(ta, C(pal.label2), LV_PART_TEXTAREA_PLACEHOLDER);
		lv_obj_set_style_border_color(ta, C(pal.tint), LV_PART_CURSOR | LV_STATE_FOCUSED);
		lv_obj_set_style_border_width(ta, 3, LV_PART_CURSOR | LV_STATE_FOCUSED);
		lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
		lv_obj_set_style_bg_opa(ta, LV_OPA_TRANSP, LV_PART_CURSOR | LV_STATE_FOCUSED);
		lv_obj_remove_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
		if (f->label)
			lv_textarea_set_placeholder_text(ta, f->label);
		lv_textarea_set_text(ta, f->value ? f->value : "");
		if (f->maxlen)
			lv_textarea_set_max_length(ta, f->maxlen);
		lv_obj_add_event_cb(ta, textarea_click, LV_EVENT_CLICKED, NULL);
		lv_obj_add_event_cb(ta, field_changed, LV_EVENT_VALUE_CHANGED, ud);
		lv_obj_add_event_cb(ta, field_kbd, EV_KBD_SHOWN, ud);
		lv_obj_add_event_cb(ta, field_kbd, EV_KBD_HIDDEN, ud);
		break;
	}
	default: {	/* one-line text fields */
		const char *ph = f->label ? f->label : f->type == HF_PASSWORD ? "Password" : f->type == HF_SEARCH ? "Search" : "";
		lv_obj_t *ta = f->type == HF_SEARCH ? ui_search(wrap, TEXT_W, ph) : ui_text_field(wrap, TEXT_W, ph);
		lv_obj_set_style_text_font(ta, font(F_TEXT, sz(34)), 0);
		if (f->type == HF_PASSWORD)	/* the keyboard keeps nothing of it (kbd.c: password mode = secret) */
			lv_textarea_set_password_mode(ta, true);
		lv_textarea_set_text(ta, f->value ? f->value : "");
		if (f->maxlen)
			lv_textarea_set_max_length(ta, f->maxlen);
		if (f->type == HF_NUMBER)
			lv_textarea_set_accepted_chars(ta, "0123456789.-+eE");
		lv_obj_add_event_cb(ta, field_changed, LV_EVENT_VALUE_CHANGED, ud);
		lv_obj_add_event_cb(ta, field_ready, LV_EVENT_READY, ud);
		lv_obj_add_event_cb(ta, field_kbd, EV_KBD_SHOWN, ud);
		lv_obj_add_event_cb(ta, field_kbd, EV_KBD_HIDDEN, ud);
		break;
	}
	}
}

/* most of the block's text is <small> / byline text: the whole block is shown that way */
static bool block_small(const html_block_t *b, int from, int to)
{
	int n = 0, all = 0;
	for (int i = 0; i < b->nruns; i++) {
		const html_run_t *r = &b->runs[i];
		int ro = r->off > from ? r->off : from, re = r->off + r->len < to ? r->off + r->len : to;
		if (re <= ro)
			continue;
		all += re - ro;
		if (r->flags & HR_SMALL)
			n += re - ro;
	}
	return all && n * 10 >= all * 8;
}

/* a data table's row: one cell box per column it fills, widths from the table's column shares; header rows in
 * small semibold secondary text, a hairline under every row but the last */
static void build_row(struct wpage *w, int i)
{
	const html_block_t *b = &w->doc->b[i];
	const html_table_t *tb = &w->doc->tables[b->table];
	lv_obj_t *row = box(w->col);
	lv_obj_set_size(row, TEXT_W, LV_SIZE_CONTENT);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
	/* a row of only small text (Hacker News' "points by … | comments") belongs to the row above it */
	bool meta = block_small(b, 0, b->len) && !b->head;
	const html_block_t *nx = i + 1 < w->doc->nb ? &w->doc->b[i + 1] : NULL;
	bool next_meta = nx && nx->kind == HB_ROW && nx->table == b->table && !nx->head && block_small(nx, 0, nx->len);
	lv_obj_set_style_pad_top(row, b->head ? 10 : meta ? 0 : 16, 0);
	lv_obj_set_style_pad_bottom(row, b->head ? 10 : next_meta ? 6 : 16, 0);
	if (b->row == 0)
		lv_obj_set_style_margin_top(row, 14, 0);
	if (b->row == tb->nrows - 1) {
		lv_obj_set_style_margin_bottom(row, 20, 0);
	} else if (!next_meta) {
		lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
		lv_obj_set_style_border_width(row, 2, 0);
		lv_obj_set_style_border_color(row, C(b->head ? mix(pal.bg, pal.label3, 70) : pal.separator), 0);
	}
	const int gap = 20, avail = TEXT_W - gap * (tb->ncols - 1);
	/* a table with a header row = a data grid (smaller text); without = a list laid out with a table */
	bool grid = false;
	for (int k = i; k >= 0 && w->doc->b[k].kind == HB_ROW && w->doc->b[k].table == b->table && !grid; k--)
		grid = w->doc->b[k].head;
	int px = grid ? 28 : 30;
	lv_font_t *f = b->head ? font(F_TEXT_SEMI, sz(26)) : meta ? font(F_TEXT, sz(26)) : font(F_TEXT, sz(px));
	uint32_t color = b->head || meta ? pal.label2 : pal.label;
	int x = 0, col = 0;	/* next column, its x */
	for (int c = 0; c < b->ncells; c++) {	/* exactly one child per cell (cell_r0 counts on it) */
		const html_cell_t *cl = &b->cells[c];
		while (col < cl->col && col < tb->ncols)
			x += avail * tb->pct[col++] / 100 + gap;
		int cw = 0;
		for (int k = cl->col; k < cl->col + cl->span && k < tb->ncols; k++)
			cw += avail * tb->pct[k] / 100 + (k > cl->col ? gap : 0);
		col = cl->col + cl->span;
		lv_obj_t *o;
		if (!cl->len) {
			o = box(row);
			lv_obj_set_size(o, cw, 1);
		} else {
			o = spans_rng(row, b, cl->off, cl->off + cl->len, cw, f, color, b->head ? NULL : font(F_TEXT_SEMI, sz(meta ? 26 : px)));
		}
		lv_obj_set_x(o, x);
		x += cw + gap;
	}
}

static void build_block(struct wpage *w, int i)
{
	html_block_t *b = &w->doc->b[i];
	lv_obj_t *col = w->col;
	int indent = b->level > 0 ? (b->level > 4 ? 4 : b->level) * 28 : 0;
	if (b->kind == HB_FIELD)
		indent = 0;
	switch (b->kind) {
	case HB_IMG: {
		struct wimg *im = &w->imgs[b->img];
		const html_img_t *hi = &w->doc->imgs[b->img];
		lv_obj_t *wrap = box(col);
		lv_obj_set_size(wrap, TEXT_W, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_top(wrap, 18, 0);
		lv_obj_set_style_pad_bottom(wrap, i + 1 < w->doc->nb && w->doc->b[i + 1].kind == HB_CAPTION ? 12 : 18, 0);
		lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);	/* narrower pictures centred */
		lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
		im->box = box(wrap);
		lv_obj_remove_flag(im->box, LV_OBJ_FLAG_CLICKABLE);
		int bw = TEXT_W, bh = 300;
		if (hi->w > 0 && hi->h > 0) {	/* declared size → the same box the picture will need */
			bw = hi->w * 2 > TEXT_W ? TEXT_W : hi->w * 2;
			bh = (int)((long)hi->h * bw / hi->w);
			if (bh > IMG_MAX_H) {
				bw = (int)((long)bw * IMG_MAX_H / bh);
				bh = IMG_MAX_H;
			}
		}
		lv_obj_set_size(im->box, bw, bh);
		lv_obj_set_style_radius(im->box, bw < 120 || bh < 120 ? 8 : IMG_RADIUS, 0);
		lv_obj_set_style_bg_opa(im->box, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(im->box, C(mix(pal.bg, pal.fill, 60)), 0);
		if (bw >= 120 && bh >= 100) {	/* placeholder until it loads (img_show cleans it away) */
			lv_obj_t *g = ui_icon(im->box, G_IMAGE, font(F_MONO, 56), mix(pal.bg, pal.label3, 70));
			lv_obj_center(g);
		}
		if (im->state == IM_SHOWN && im->dsc) {
			lv_image_dsc_t *d = im->dsc;
			im->dsc = NULL;
			img_bytes -= d->data_size;
			img_show(w, b->img, d);
		} else if (im->state == IM_FAILED) {
			lv_obj_add_flag(wrap, LV_OBJ_FLAG_HIDDEN);
		}
		return;
	}
	case HB_HR: {
		lv_obj_t *l = box(col);
		lv_obj_set_size(l, TEXT_W, 2);
		lv_obj_set_style_margin_ver(l, 24, 0);
		lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(l, C(pal.separator), 0);
		return;
	}
	case HB_RESULT: {
		lv_obj_t *card = box(col);
		lv_obj_set_size(card, TEXT_W, LV_SIZE_CONTENT);
		lv_obj_set_style_margin_bottom(card, 18, 0);
		lv_obj_set_style_radius(card, 32, 0);
		lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(card, C(pal.cell), 0);
		lv_obj_set_style_bg_color(card, C(pal.cell_hi), LV_STATE_PRESSED);
		lv_obj_set_style_pad_all(card, 28, 0);
		lv_obj_set_style_pad_row(card, 8, 0);
		lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
		lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(card, result_click, LV_EVENT_SHORT_CLICKED, b);
		lv_obj_add_event_cb(card, result_long, LV_EVENT_LONG_PRESSED, b);
		if (b->sub) {
			lv_obj_t *u = text(card, b->sub, font(F_TEXT, 24), pal.label2);
			lv_obj_set_width(u, TEXT_W - 56);
			lv_label_set_long_mode(u, LV_LABEL_LONG_MODE_DOTS);
			lv_obj_set_height(u, lv_font_get_line_height(font(F_TEXT, 24)));
		}
		lv_obj_t *t = text(card, b->text, font(F_TEXT_SEMI, sz(34)), pal.tint);
		lv_obj_set_width(t, TEXT_W - 56);
		lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
		if (b->snip && *b->snip) {
			lv_obj_t *s = text(card, b->snip, font(F_TEXT, sz(28)), pal.label2);
			lv_obj_set_width(s, TEXT_W - 56);
			lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
			lv_obj_set_style_text_line_space(s, 4, 0);
			lv_obj_set_style_max_height(s, lv_font_get_line_height(font(F_TEXT, sz(28))) * 4 + 12, 0);
			lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
		}
		return;
	}
	case HB_FIELD:
		build_field(w, b);
		return;
	case HB_ROW:
		build_row(w, i);
		return;
	case HB_NOTE: {
		lv_obj_t *t = text(col, b->text, font(F_TEXT, 32), pal.label2);
		lv_obj_set_width(t, TEXT_W);
		lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_pad_top(t, 120, 0);
		return;
	}
	case HB_PRE: {
		/* code keeps its lines: wider ones scroll sideways inside the box (vertical drags go to the page) */
		lv_obj_t *bx = box(col);
		lv_obj_set_size(bx, TEXT_W, LV_SIZE_CONTENT);
		lv_obj_set_style_margin_ver(bx, 16, 0);
		lv_obj_set_style_radius(bx, 24, 0);
		lv_obj_set_style_bg_opa(bx, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(bx, C(pal.cell), 0);
		lv_obj_set_style_pad_hor(bx, 26, 0);
		lv_obj_set_style_pad_ver(bx, 22, 0);
		lv_obj_add_flag(bx, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
		lv_obj_remove_flag(bx, LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_ELASTIC);
		lv_obj_set_scroll_dir(bx, LV_DIR_HOR);
		lv_obj_set_scrollbar_mode(bx, LV_SCROLLBAR_MODE_OFF);
		char *t = b->len > 16000 ? NULL : b->text;
		lv_obj_t *l = text(bx, t ? t : "", font(F_MONO, sz(24)), pal.label);
		if (!t) {
			char tmp[16001];
			memcpy(tmp, b->text, 16000);
			tmp[16000] = 0;
			utf8_trim(tmp);
			lv_label_set_text(l, tmp);
		}
		lv_obj_set_style_text_line_space(l, 6, 0);
		return;
	}
	default:
		break;
	}
	uint32_t color = b->kind == HB_CAPTION ? pal.label2 : b->kind == HB_QUOTE ? mix(pal.label, pal.label2, 40) : pal.label;
	lv_font_t *f = block_font(b->kind);
	if ((b->kind == HB_P || b->kind == HB_LI) && block_small(b, 0, b->len)) {	/* a byline, a footnote */
		f = font(F_TEXT, sz(26));
		color = pal.label2;
	}
	const html_block_t *nx = i + 1 < w->doc->nb ? &w->doc->b[i + 1] : NULL;
	const html_field_t *nf = nx && nx->kind == HB_FIELD ? &w->doc->fields[nx->field] : NULL;
	bool flabel = b->kind == HB_P && !indent && b->len < 48 && nf && nf->type != HF_SUBMIT &&
		      nf->type != HF_IMAGE;	/* "Email" over its field: a small form label */
	if (flabel) {
		f = font(F_TEXT_MEDIUM, sz(28));
		color = pal.label2;
	}
	if (b->kind == HB_LI) {
		lv_obj_t *row = box(col);
		lv_obj_set_size(row, TEXT_W, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_left(row, indent - 28 > 0 ? indent - 28 : 0, 0);
		lv_obj_set_style_pad_ver(row, 6, 0);
		lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
		lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
		char mark[16];
		if (b->num > 0)
			snprintf(mark, sizeof(mark), "%d.", b->num);
		else
			snprintf(mark, sizeof(mark), b->level > 1 ? "\xe2\x97\xa6" : "\xe2\x80\xa2");
		lv_obj_t *m = text(row, mark, f, pal.label2);
		lv_obj_set_width(m, b->num > 9 ? 64 : 44);
		int rw = TEXT_W - lv_obj_get_style_pad_left(row, 0) - (b->num > 9 ? 64 : 44);
		spans(row, b, rw, f, color);
		return;
	}
	lv_obj_t *sg = spans(col, b, TEXT_W - indent - (b->kind == HB_QUOTE ? 28 : 0), f, color);
	int top = 14, bottom = 14;
	if (b->kind >= HB_H1 && b->kind <= HB_H4) {
		top = i == 0 ? 20 : b->kind == HB_H1 ? 40 : 34;
		bottom = 8;
	} else if (b->kind == HB_CAPTION) {
		top = 0;
	} else if (flabel) {
		top = 18;
		bottom = 0;
	}
	lv_obj_set_style_margin_top(sg, top, 0);
	lv_obj_set_style_margin_bottom(sg, bottom, 0);
	if (indent || b->kind == HB_QUOTE)
		lv_obj_set_style_margin_left(sg, indent, 0);
	if (b->kind == HB_QUOTE) {	/* a tinted bar on the left */
		lv_obj_set_style_border_side(sg, LV_BORDER_SIDE_LEFT, 0);
		lv_obj_set_style_border_width(sg, 6, 0);
		lv_obj_set_style_border_color(sg, C(mix(pal.bg, pal.tint, 70)), 0);
		lv_obj_set_style_pad_left(sg, 26, 0);
		lv_obj_set_style_pad_ver(sg, 4, 0);
	}
	/* flex column centres children: indented blocks keep the left edge */
	if (indent || b->kind == HB_QUOTE)
		lv_obj_set_style_translate_x(sg, indent / 2, 0);
}

static void more_click(lv_event_t *e);

static void menu_chip_click(lv_event_t *e)
{
	struct wpage *w = top_page();
	int li = (int)(intptr_t)lv_event_get_user_data(e);
	if (w && w->doc && li >= 0 && li < w->doc->nlinks)
		navigate(w->doc->links[li], true, false, NULL);
}

static void menu_chip_long(lv_event_t *e)
{
	struct wpage *w = top_page();
	int li = (int)(intptr_t)lv_event_get_user_data(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	if (w && w->doc && li >= 0 && li < w->doc->nlinks) {
		ui_haptic_tap();
		link_menu(w->doc->links[li], p.y);
	}
}

/* "Site Menu": the page's navigation links, collapsed; a tap opens them as wrapping chips */
static void site_menu_toggle(lv_event_t *e)
{
	(void)e;
	struct wpage *w = top_page();
	if (!w || !w->doc || !w->menu_card)
		return;
	lv_obj_t *chev = lv_obj_get_child(lv_obj_get_child(w->menu_card, 0), -1);
	if (w->menu_list) {
		lv_obj_delete(w->menu_list);
		w->menu_list = NULL;
		ui_icon_set(chev, G_CHEVRON_D);
		return;
	}
	ui_icon_set(chev, G_CHEVRON_U);
	lv_obj_t *l = w->menu_list = box(w->menu_card);
	lv_obj_set_size(l, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(l, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_row(l, 14, 0);
	lv_obj_set_style_pad_column(l, 14, 0);
	lv_obj_set_style_pad_top(l, 18, 0);
	lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
	for (int i = 0; i < w->doc->nmenu; i++) {
		char t[64];
		snprintf(t, sizeof(t), "%s", w->doc->menu[i].text);
		utf8_trim(t);
		if (strlen(w->doc->menu[i].text) >= sizeof(t))
			snprintf(t + sizeof(t) - 4, 4, "\xe2\x80\xa6");
		lv_obj_t *c = box(l);
		lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_style_max_width(c, TEXT_W - 56, 0);
		lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(c, C(pal.fill), 0);
		lv_obj_set_style_bg_color(c, C(pal.cell_hi), LV_STATE_PRESSED);
		lv_obj_set_style_pad_hor(c, 24, 0);
		lv_obj_set_style_pad_ver(c, 12, 0);
		lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_t *lb = text(c, t, font(F_TEXT_MEDIUM, 28), pal.tint);
		lv_label_set_long_mode(lb, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_set_style_max_width(lb, TEXT_W - 104, 0);
		lv_obj_add_event_cb(c, menu_chip_click, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)w->doc->menu[i].link);
		lv_obj_add_event_cb(c, menu_chip_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)w->doc->menu[i].link);
	}
}

static void full_click(lv_event_t *e);

static lv_obj_t *card_box(lv_obj_t *col)
{
	lv_obj_t *card = box(col);
	lv_obj_set_size(card, TEXT_W, LV_SIZE_CONTENT);
	lv_obj_set_style_margin_bottom(card, 18, 0);
	lv_obj_set_style_radius(card, 28, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, C(pal.cell), 0);
	lv_obj_set_style_pad_hor(card, 28, 0);
	lv_obj_set_style_pad_ver(card, 22, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
	return card;
}

/* a page that is mostly script: say so, offer the full site */
static bool needs_js(const html_doc_t *d) { return d->text_len < 280 && d->scripts >= 2 && !d->nfields; }	/* a short page with a form (a search page) works */

/* ---------- OpenSearch: the site's own search when its search box needs JavaScript (GitHub) ---------- */

static char os_q[512];	/* what the user searched for while the description loads */
static int os_gen;
static net_req_t *os_req;

/* the text/html <Url template="…"> of an OpenSearch description, {searchTerms} filled in → out (absolute) */
static bool os_url(const char *xml, const char *base, const char *q, char *out, int n)
{
	for (const char *u = xml; (u = strstr(u, "<Url")) != NULL; u += 4) {
		const char *e = strchr(u, '>');
		if (!e)
			return false;
		char tag[2048];
		snprintf(tag, sizeof(tag), "%.*s", (int)(e - u) < (int)sizeof(tag) - 1 ? (int)(e - u) : (int)sizeof(tag) - 1, u);
		const char *ty = strstr(tag, "type=\"");
		if (!ty || strncmp(ty + 6, "text/html\"", 10) || (strstr(tag, "method=\"") && !strstr(tag, "method=\"get\"") &&
								    !strstr(tag, "method=\"GET\"")))
			continue;
		const char *t = strstr(tag, "template=\"");
		if (!t)
			continue;
		t += 10;
		char tpl[2048], res[2048], enc[1536];
		snprintf(tpl, sizeof(tpl), "%.*s", (int)strcspn(t, "\""), t);
		net_urlencode(q, enc, sizeof(enc));
		int o = 0;
		for (const char *p = tpl; *p && o < (int)sizeof(res) - 1;) {
			if (!strncmp(p, "&amp;", 5)) {
				res[o++] = '&';
				p += 5;
			} else if (*p == '{') {	/* {searchTerms} = the query, any other parameter left empty */
				const char *c = strchr(p, '}');
				if (!c)
					break;
				if (!strncmp(p, "{searchTerms}", 13))
					o += snprintf(res + o, sizeof(res) - o, "%s", enc);
				p = c + 1;
			} else {
				res[o++] = *p++;
			}
			if (o >= (int)sizeof(res))
				o = sizeof(res) - 1;
		}
		res[o] = 0;
		return net_resolve(base, res, out, n);
	}
	return false;
}

static void os_fetched(net_res_t *r, void *ud)
{
	os_req = NULL;
	struct wpage *w = top_page();
	if (!w || w->gen != (int)(intptr_t)ud)
		return;
	char u[2048];
	if (!r->err && r->status < 400 && r->body && os_url(r->body, r->url[0] ? r->url : w->doc->opensearch, os_q, u, sizeof(u)))
		navigate(u, true, false, NULL);	/* from the page: its LAN / tailnet rules */
	else
		ui_alert("Can\xe2\x80\x99t Search", "This site\xe2\x80\x99s search couldn\xe2\x80\x99t be loaded.", (const char *[]){ "OK" }, 1, NULL, NULL);
}

static void os_ready(lv_event_t *e)
{
	struct wpage *w = top_page();
	const char *q = lv_textarea_get_text(lv_event_get_target(e));
	if (!w || !w->doc || !w->doc->opensearch || !q || !*q)
		return;
	snprintf(os_q, sizeof(os_q), "%s", q);
	if (kbd_visible())
		kbd_hide();
	ui_haptic_tap();
	net_cancel(os_req);
	os_gen = w->gen;
	net_opts_t o = { .url = w->doc->opensearch, .max_bytes = 64 << 10, .timeout_s = 20, .referer = w->url,
			 .allow_private = w->priv, .desktop = desk_on(w->url), .cookies = NET_CK_NONE };
	os_req = net_fetch(&o, os_fetched, (void *)(intptr_t)os_gen);
}

static bool has_search_form(const html_doc_t *d)
{
	for (int i = 0; i < d->nfields; i++)
		if (d->fields[i].form >= 0 && (d->fields[i].type == HF_SEARCH || d->forms[d->fields[i].form].search))
			return true;
	return false;
}

static void build_head(struct wpage *w)
{
	html_doc_t *d = w->doc;
	if (d->nmenu) {
		lv_obj_t *card = w->menu_card = card_box(w->col);
		lv_obj_t *head = box(card);
		lv_obj_set_size(head, LV_PCT(100), 52);
		lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_opa(head, LV_OPA_60, LV_STATE_PRESSED);
		lv_obj_set_ext_click_area(head, 22);
		lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(head, 18, 0);
		lv_obj_add_event_cb(head, site_menu_toggle, LV_EVENT_CLICKED, NULL);
		ui_icon(head, G_LIST, font(F_MONO, 30), pal.label2);
		char t[48];
		snprintf(t, sizeof(t), "Site Menu");
		lv_obj_t *l = text(head, t, font(F_TEXT_SEMI, 32), pal.label);
		lv_obj_set_flex_grow(l, 1);
		snprintf(t, sizeof(t), "%d", d->nmenu);
		text(head, t, font(F_TEXT, 28), pal.label3);
		ui_icon(head, G_CHEVRON_D, font(F_MONO, 26), pal.label3);
	}
	if (d->opensearch && !has_search_form(d)) {
		lv_obj_t *wrap = box(w->col);
		lv_obj_set_size(wrap, TEXT_W, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_ver(wrap, 10, 0);
		lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
		char ph[96], host[64];
		snprintf(ph, sizeof(ph), "Search %s", net_host(w->url, host, sizeof(host)));
		lv_obj_t *ta = ui_search(wrap, TEXT_W, ph);
		lv_obj_set_style_text_font(ta, font(F_TEXT, sz(34)), 0);
		lv_obj_add_event_cb(ta, os_ready, LV_EVENT_READY, NULL);
		lv_obj_add_event_cb(ta, field_kbd, EV_KBD_SHOWN, (void *)(intptr_t)-1);
		lv_obj_add_event_cb(ta, field_kbd, EV_KBD_HIDDEN, (void *)(intptr_t)-1);
	}
	if (needs_js(d) && d->nb) {
		lv_obj_t *card = card_box(w->col);
		lv_obj_set_style_pad_row(card, 12, 0);
		lv_obj_t *t = text(card, "This page needs JavaScript", font(F_TEXT_SEMI, 32), pal.label);
		lv_obj_set_width(t, LV_PCT(100));
		lv_obj_t *m = text(card, "Web shows pages without it, so most of this one is missing.", font(F_TEXT, 28), pal.label2);
		lv_obj_set_width(m, LV_PCT(100));
		lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
		if (web_full_open) {
			lv_obj_t *b = ui_button(card, "Try Full Site", true);
			lv_obj_set_style_margin_top(b, 8, 0);
			lv_obj_add_event_cb(b, full_click, LV_EVENT_CLICKED, NULL);
		}
	}
}

static void build_more(struct wpage *w, int upto)
{
	if (!w->doc)
		return;
	if (w->built == 0 && !w->menu_card && lv_obj_get_child_count(w->col) == 0)
		build_head(w);
	int end = upto > 0 ? upto : w->built + BATCH;
	if (end > w->doc->nb)
		end = w->doc->nb;
	building = w;
	for (; w->built < end; w->built++) {
		build_block(w, w->built);
		/* each block adds one child to col: tag it with its index (+1) for scroll_to_anchor */
		lv_obj_t *o = lv_obj_get_child(w->col, -1);
		if (o)
			lv_obj_set_user_data(o, (void *)(intptr_t)(w->built + 1));
	}
	building = NULL;
	if (w->built == w->doc->nb && !w->more) {
		if (w->doc->more) {
			w->more = ui_button(w->col, "More Results", false);
			lv_obj_set_style_margin_top(w->more, 20, 0);
			lv_obj_add_event_cb(w->more, more_click, LV_EVENT_CLICKED, NULL);
		} else if (w->doc->cut) {
			w->more = text(w->col, "The rest of this page is too long to show.", font(F_TEXT, 26), pal.label3);
			lv_obj_set_style_pad_top(w->more, 30, 0);
		}
	}
}

static void more_click(lv_event_t *e)
{
	(void)e;
	struct wpage *w = top_page();
	if (w && w->doc && w->doc->more)
		navigate(w->doc->more, true, false, NULL);
}

/* bring block w->anchor - 1 (already built) to the top */
static void anchor_scroll(struct wpage *w)
{
	int want = w->anchor;
	w->anchor = 0;
	lv_obj_update_layout(w->col);
	for (int k = lv_obj_get_child_count(w->col) - 1; k >= 0; k--) {
		lv_obj_t *c = lv_obj_get_child(w->col, k);
		if ((intptr_t)lv_obj_get_user_data(c) != want)
			continue;
		int y = lv_obj_get_y(c) - 20;
		if (w->hl_want == want) {	/* Find: the match's block, tinted */
			if (w->hl)
				lv_obj_set_style_bg_opa(w->hl, LV_OPA_TRANSP, 0);
			w->hl = c;
			w->hl_want = 0;
			lv_obj_set_style_bg_opa(c, LV_OPA_40, 0);
			lv_obj_set_style_bg_color(c, C(pal.yellow), 0);
			lv_obj_set_style_radius(c, 10, 0);
			y = lv_obj_get_y(c) - 200 > 0 ? lv_obj_get_y(c) - 200 : 0;	/* a little context above it */
		}
		lv_obj_scroll_to_y(w->col, y < 0 ? 0 : y, LV_ANIM_ON);
		return;
	}
}

/* fragment: bring its block to the top; blocks not built yet are built a few per tick() first (a long page's
 * anchor near the end must not stall the UI building everything in one go) */
static bool scroll_to_anchor(struct wpage *w, const char *frag)
{
	if (!w->doc || !*frag)
		return false;
	for (int i = 0; i < w->doc->nb; i++)
		if (w->doc->b[i].anchor && !strcmp(w->doc->b[i].anchor, frag)) {
			w->anchor = i + 1;
			if (i < w->built)
				anchor_scroll(w);
			return true;
		}
	return false;
}

/* tick(): one step towards a pending anchor; true = it did something (let the layout settle) */
static bool anchor_step(struct wpage *w)
{
	if (!w->anchor)
		return false;
	if (!w->doc || w->anchor > w->doc->nb) {	/* a block this document doesn't have */
		w->anchor = 0;
		return false;
	}
	if (w->built >= w->anchor) {
		anchor_scroll(w);
		return true;
	}
	uint32_t t0 = lv_tick_get();
	int stop = w->built + ANCHOR_BATCH < w->anchor ? w->built + ANCHOR_BATCH : w->anchor;
	while (w->built < stop && lv_tick_elaps(t0) < ANCHOR_MS)
		build_more(w, w->built + 1);
	return true;
}

/* ---------- page chrome ---------- */

/* loading bar (Chrome's): no byte counts from curl, so it runs on time — 10 % at once, easing towards 85 % over
 * 6 s (a 100 ms timer: 10 small redraws a second, only its 4 px strip), then to the end and away when the page is
 * there. Only the top page's bar moves. */
static lv_timer_t *prog_timer;
static void prog_w(void *o, int32_t v) { lv_obj_set_width(o, v); }
static void prog_gone(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

static void prog_tick(lv_timer_t *t)
{
	struct wpage *w = top_page();
	if (!w || !w->prog_on) {
		lv_timer_delete(t);
		prog_timer = NULL;
		return;
	}
	uint32_t ms = lv_tick_elaps(w->prog_t0);
	int k = ms >= 6000 ? 1000 : (int)(ms / 6);	/* 0..1000 */
	int r = 1000 - k;
	int ease = 1000 - (int)((int64_t)r * r * r / 1000000);	/* ease-out cubic */
	int v = SCR_W / 10 + SCR_W * 75 / 100 * ease / 1000;
	if (v != lv_obj_get_width(w->prog))
		lv_obj_set_width(w->prog, v);
}

static void prog_sync(struct wpage *w)
{
	if (!w->prog || w->prog_on == w->loading)
		return;
	w->prog_on = w->loading;
	lv_anim_delete(w->prog, prog_w);
	if (w->loading) {
		w->prog_t0 = lv_tick_get();
		lv_obj_remove_flag(w->prog, LV_OBJ_FLAG_HIDDEN);
		lv_obj_set_width(w->prog, SCR_W / 10);
		if (!prog_timer)
			prog_timer = lv_timer_create(prog_tick, 100, NULL);
		return;
	}
	motion_anim(w->prog, prog_w, lv_obj_get_width(w->prog), SCR_W, 200, prog_gone);
}

static void set_host(struct wpage *w)
{
	char h[256];
	if (w->search) {
		char q[256];
		if (ddg_query(w->url, q, sizeof(q)))
			snprintf(h, sizeof(h), "%s", q);
		else
			snprintf(h, sizeof(h), "Search");
	} else {
		net_host(w->url, h, sizeof(h));
		if (!strncmp(h, "www.", 4))
			memmove(h, h + 4, strlen(h + 4) + 1);
	}
	if (strcmp(lv_label_get_text(w->host), h))
		lv_label_set_text(w->host, h);
	if (!w->search && !strncmp(w->url, "https:", 6))
		lv_obj_remove_flag(w->lock, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(w->lock, LV_OBJ_FLAG_HIDDEN);
	ui_icon_set(w->reload, w->loading ? G_M_CLOSE : G_M_RELOAD);
	prog_sync(w);
}

enum { ST_RETRY = 1, ST_OPEN = 2, ST_FULL = 4 };
static void status_show(struct wpage *w, const char *glyph, const char *title, const char *msg, int buttons);

static struct wpage *find_page;	/* Find on Page (below) */
static int nhits;
static void find_label(void);

static void page_clear(struct wpage *w)
{
	if (find_page == w) {	/* the hits were block numbers of the old document */
		nhits = 0;
		if (w->col)	/* (not while the page is going away) */
			find_label();
	}
	net_cancel(w->req);
	w->req = NULL;
	free(w->boxsg);
	w->boxsg = NULL;
	w->nboxsg = w->capboxsg = 0;
	w->menu_card = w->menu_list = w->hl = NULL;
	w->refresh_at = 0;
	if (w->doc) {
		for (int k = 0; k < w->doc->nimgs; k++)
			img_drop(&w->imgs[k]);
		free(w->imgs);
		w->imgs = NULL;
		html_free(w->doc);
		w->doc = NULL;
	}
	pressed_span = NULL;	/* it may have been on this page */
	pressed_sg = NULL;
	if (w->col)
		lv_obj_clean(w->col);
	w->spinner = w->status = w->more = NULL;
	w->built = 0;
	w->anchor = 0;
	w->failed = false;
}

static void retry_now(void *ud)	/* after the button is gone */
{
	(void)ud;
	struct wpage *w = top_page();
	if (w)
		page_load(w, w->url);
}

static void retry_click(lv_event_t *e)
{
	(void)e;
	lv_async_call(retry_now, NULL);
}

static void open_anyway_now(void *ud)
{
	(void)ud;
	struct wpage *w = top_page();
	if (!w)
		return;
	w->allow_priv = true;	/* the user's own tap on the button */
	page_load(w, w->url);
}

static void open_anyway_click(lv_event_t *e)
{
	(void)e;
	lv_async_call(open_anyway_now, NULL);
}

static void full_click(lv_event_t *e)
{
	(void)e;
	struct wpage *w = top_page();
	if (w && web_full_open)
		web_full_open(w->url);
}

static void status_show(struct wpage *w, const char *glyph, const char *title, const char *msg, int buttons)
{
	if (w->spinner) {
		lv_obj_delete(w->spinner);
		w->spinner = NULL;
	}
	w->status = box(w->col);
	lv_obj_set_size(w->status, TEXT_W, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(w->status, 200, 0);
	lv_obj_set_flex_flow(w->status, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(w->status, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(w->status, 18, 0);
	lv_obj_remove_flag(w->status, LV_OBJ_FLAG_CLICKABLE);
	ui_icon_line(w->status, glyph, font(F_MONO, 96), pal.label3);
	lv_obj_t *t = text(w->status, title, font(F_TEXT_BOLD, 44), pal.label);
	lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_t *m = text(w->status, msg, font(F_TEXT, 30), pal.label2);
	lv_obj_set_width(m, TEXT_W - 40);
	lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
	if (buttons & ST_OPEN) {
		lv_obj_t *b = ui_button(w->status, "Open Anyway", true);
		lv_obj_set_style_margin_top(b, 20, 0);
		lv_obj_add_event_cb(b, open_anyway_click, LV_EVENT_CLICKED, NULL);
	}
	if (buttons & ST_RETRY) {
		lv_obj_t *b = ui_button(w->status, "Try Again", !(buttons & ST_OPEN));
		lv_obj_set_style_margin_top(b, 20, 0);
		lv_obj_add_event_cb(b, retry_click, LV_EVENT_CLICKED, NULL);
	}
	if ((buttons & ST_FULL) && web_full_open) {
		lv_obj_t *b = ui_button(w->status, "Try Full Site", false);
		lv_obj_set_style_margin_top(b, buttons & (ST_RETRY | ST_OPEN) ? 0 : 20, 0);
		lv_obj_add_event_cb(b, full_click, LV_EVENT_CLICKED, NULL);
	}
}

static void page_title(struct wpage *w, const char *t)
{
	snprintf(w->title, sizeof(w->title), "%s", t && *t ? t : w->url);
	nav_set_title(w->page, w->title);
}

/* ---------- loading ---------- */

struct pjob { int gen; char *body; size_t len; char url[2048], ctype[128]; int mode; html_doc_t *doc;
	      lv_image_dsc_t *img; const char *err; };
enum { PJ_HTML, PJ_DDG, PJ_PLAIN, PJ_IMAGE };

static void pjob_work(void *a)
{
	struct pjob *j = a;
	if (j->mode == PJ_IMAGE)
		j->img = img_decode(j->body, j->len, SCR_W, IMG_DIRECT_MAX_H, &j->err);
	else if (j->mode == PJ_DDG)
		j->doc = html_ddg(j->body, j->len, j->url);
	else if (j->mode == PJ_PLAIN)
		j->doc = html_plain(j->body, j->len, j->url, j->ctype);
	else
		j->doc = html_parse(j->body, j->len, j->url, j->ctype);
	free(j->body);
	j->body = NULL;
}

#ifndef ORCHARD_PHONE
static int obj_count(lv_obj_t *o)
{
	int n = 1;
	for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
		n += obj_count(lv_obj_get_child(o, i));
	return n;
}

/* previews: ORCHARD_WEB_STATS=1 builds the whole page at once and prints what it cost (objects, build ms, one
 * full redraw ms) — to compare layouts; the phone builds in batches as you scroll */
static void web_stats(struct wpage *w)
{
	static int on = -1;
	if (on < 0)
		on = getenv("ORCHARD_WEB_STATS") != NULL;
	if (!on || !w->doc)
		return;
	struct timespec t0, t1, t2;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	build_more(w, w->doc->nb);
	lv_obj_update_layout(w->col);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	double build = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
	lv_refr_now(NULL);	/* the layout's own invalidations first */
	clock_gettime(CLOCK_MONOTONIC, &t1);
	for (int k = 0; k < 10; k++) {
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(NULL);
	}
	clock_gettime(CLOCK_MONOTONIC, &t2);
	fprintf(stderr, "web-stats: %s blocks %d objects %d build %.1f ms redraw (top, full screen) %.1f ms height %d\n", w->url, w->doc->nb,
		obj_count(w->col), build,
		((t2.tv_sec - t1.tv_sec) * 1e3 + (t2.tv_nsec - t1.tv_nsec) / 1e6) / 10, (int)lv_obj_get_scroll_bottom(w->col) + (int)lv_obj_get_height(w->col));
}
#else
static void web_stats(struct wpage *w) { (void)w; }
#endif

static void pjob_done(void *a)
{
	struct pjob *j = a;
	struct wpage *w = page_by_gen(j->gen);
	if (!w || !w->loading) {
		html_free(j->doc);
		img_free(j->img);
		free(j);
		return;
	}
	w->loading = false;
	if (w->spinner) {
		lv_obj_delete(w->spinner);
		w->spinner = NULL;
	}
	set_host(w);
	if (j->mode == PJ_IMAGE) {	/* a picture on its own: one image block */
		if (!j->img) {
			status_show(w, G_IMAGE, "Can\xe2\x80\x99t Show Image", j->err ? j->err : "", 0);
			free(j);
			return;
		}
		html_doc_t *d = calloc(1, sizeof(*d));
		html_block_t *b = d ? calloc(1, sizeof(*b)) : NULL;
		html_img_t *im = d ? calloc(1, sizeof(*im)) : NULL;
		if (!d || !b || !im) {
			free(d);
			free(b);
			free(im);
			img_free(j->img);
			free(j);
			return;
		}
		b->kind = HB_IMG;
		b->img = 0;
		b->text = calloc(1, 1);
		b->runs = calloc(1, sizeof(html_run_t));
		im->url = strdup(w->url);
		d->b = b;
		d->nb = 1;
		d->imgs = im;
		d->nimgs = 1;
		d->base = strdup(w->url);
		j->doc = d;
	}
	if (!j->doc) {
		status_show(w, G_WARN, "Can\xe2\x80\x99t Open Page", "Not enough memory to show this page.", ST_RETRY);
		free(j);
		return;
	}
	w->doc = j->doc;
	w->imgs = calloc(w->doc->nimgs + 1, sizeof(struct wimg));
	if (j->img) {	/* the picture is already decoded */
		w->imgs[0].state = IM_SHOWN;
		w->imgs[0].dsc = j->img;
		img_bytes += j->img->data_size;
	}
	char q[256];
	if (w->search && ddg_query(w->url, q, sizeof(q)))
		page_title(w, q);
	else if (w->doc->title)
		page_title(w, w->doc->title);
	else if (j->mode == PJ_IMAGE) {
		const char *s = strrchr(w->url, '/');
		page_title(w, s && s[1] ? s + 1 : w->url);
	} else {
		char h[256];
		page_title(w, net_host(w->url, h, sizeof(h)));
	}
	if (!w->search)
		hist_add(w->url, w->title);
	if (!w->search && j->mode == PJ_HTML)	/* its site's icon, for the start page (at most weekly) */
		wicon_page(w->url, w->doc, w->priv, desk_on(w->url));
	w->tsz = tsize();
	build_more(w, 0);
	web_stats(w);
	if (!w->doc->nb)
		status_show(w, G_FILE, needs_js(w->doc) ? "This Page Needs JavaScript" : "Nothing to Show",
			    "This page has no text that can be shown without JavaScript.", ST_FULL);
	if (w->doc->refresh && w->refreshes < 3 && w->doc->refresh_s <= 5 && strcmp(w->doc->refresh, w->url))
		w->refresh_at = lv_tick_get() + (w->doc->refresh_s > 0 ? w->doc->refresh_s * 1000 : 300);
	if (w->frag[0])
		scroll_to_anchor(w, w->frag);
	free(j);
	tab_dirty();	/* its final address + title */
}

static void page_fetched(net_res_t *r, void *ud)
{
	int gen = (intptr_t)ud;
	struct wpage *w = page_by_gen(gen);
	if (!w)
		return;
	w->req = NULL;
	w->err = r->err;
	if (r->err || (r->status >= 400 && !r->len)) {
		w->loading = false;
		set_host(w);
		char msg[300];
		size_t ul = strcspn(w->url, "#");
		bool redirected = r->url[0] && (strlen(r->url) != ul || strncmp(r->url, w->url, ul));
		char rh[256];
		net_host(r->url, rh, sizeof(rh));
		if (r->err == NET_EPRIVATE && w->post)
			snprintf(msg, sizeof(msg), "This form sends to an address on your local network or tailnet. A page from the internet can\xe2\x80\x99t do that.");
		else if (r->err == NET_EPRIVATE && redirected) {
			snprintf(msg, sizeof(msg), "This page redirects to %s, which is on your local network or tailnet.", rh);
			snprintf(w->url, sizeof(w->url), "%s", r->url);	/* Open Anyway goes straight there */
		} else if (r->err == NET_EPRIVATE)
			snprintf(msg, sizeof(msg), "%s", "This link goes to an address on your local network or tailnet, and the page it is on is from the internet.");
		else if (r->err)
			snprintf(msg, sizeof(msg), "%s", r->msg);
		else
			snprintf(msg, sizeof(msg), "The server answered with error %d.", r->status);
		int bt = r->err == NET_ENOCURL || r->err == NET_EBLOCKED ? 0 : r->err == NET_EPRIVATE ? (w->post ? 0 : ST_OPEN) : ST_RETRY;
		if (r->err == NET_ETLS || r->err == NET_EHTTP || (!r->err && r->status >= 400))
			bt |= ST_FULL;
		status_show(w, r->err == NET_ENOCURL || r->err == NET_EBLOCKED || r->err == NET_EPRIVATE ? G_SHIELD : G_GLOBE,
			    r->err == NET_EPRIVATE ? "Local Address" : "Can\xe2\x80\x99t Open Page", msg, bt);
		w->failed = true;
		return;
	}
	w->priv = r->priv;
	w->allow_priv = w->priv;	/* reload / retry: a pass only for where it ended up (no DNS-rebinding the LAN) */
	free(w->post);	/* answered: a reload is a GET */
	w->post = NULL;
	if (r->url[0]) {	/* redirected: links resolve against where we ended up */
		char frag[256] = "";
		const char *h = strchr(w->url, '#');
		if (h && !strchr(r->url, '#'))
			snprintf(frag, sizeof(frag), "%s", h);
		snprintf(w->url, sizeof(w->url), "%s%s", r->url, frag);
	}
	const char *ct = r->ctype;
	int mode = PJ_HTML;
	if (is_ddg(w->url))
		mode = PJ_DDG;
	else if (!strncmp(ct, "image/", 6))
		mode = PJ_IMAGE;
	else if (!strncmp(ct, "text/plain", 10))
		mode = PJ_PLAIN;
	else if (*ct && strncmp(ct, "text/html", 9) && !strstr(ct, "xhtml") && !strstr(ct, "xml")) {
		w->loading = false;
		set_host(w);
		char msg[220];
		snprintf(msg, sizeof(msg), "Web can\xe2\x80\x99t show this kind of file (%.*s).", (int)strcspn(ct, ";"), ct);
		status_show(w, G_FILE, "Can\xe2\x80\x99t Open File", msg, 0);
		return;
	}
	struct pjob *j = calloc(1, sizeof(*j));
	if (!j)
		return;
	j->gen = gen;
	j->mode = mode;
	j->len = r->len;
	j->body = net_take(r);
	snprintf(j->url, sizeof(j->url), "%s", w->url);
	snprintf(j->ctype, sizeof(j->ctype), "%s", ct);
	net_bg(pjob_work, pjob_done, j);
}

static void page_load(struct wpage *w, const char *url)
{
	page_clear(w);
	if (url != w->url)
		snprintf(w->url, sizeof(w->url), "%s", url);
	const char *h = strchr(w->url, '#');
	snprintf(w->frag, sizeof(w->frag), "%s", h ? h + 1 : "");
	net_urldecode(w->frag);
	w->gen = gen_next++;	/* late answers for what this page showed before are ignored */
	w->search = is_ddg(w->url);
	w->loading = true;
	set_host(w);
	if (!net_available()) {
		w->loading = false;
		set_host(w);
		status_show(w, G_WARN, "No Internet Tools",
			    "curl isn\xe2\x80\x99t installed on this phone (tools/net.tgz).", 0);
		return;
	}
	/* loading: the omnibox's bar runs (set_host); the site's name is the title until the page has its own */
	char host[256];
	net_host(w->url, host, sizeof(host));
	if (!strncmp(host, "www.", 4))
		memmove(host, host + 4, strlen(host + 4) + 1);
	if (!w->title[0] && *host && !w->search)
		nav_set_title(w->page, host);
	char base[2048];
	snprintf(base, sizeof(base), "%.*s", (int)strcspn(w->url, "#"), w->url);
	net_opts_t o = { .url = base, .max_bytes = MAX_PAGE, .timeout_s = 30, .post = w->post,
			 .referer = w->referer[0] ? w->referer : NULL, .desktop = desk_on(base),
			 .cookies = NET_CK_RW, .allow_private = w->allow_priv };
	w->req = net_fetch(&o, page_fetched, (void *)(intptr_t)w->gen);
	if (!w->req) {
		w->loading = false;
		set_host(w);
		status_show(w, G_WARN, "Can\xe2\x80\x99t Open Page", "The address isn\xe2\x80\x99t valid.", 0);
	}
}

/* ---------- page UI: omnibox bar on top, toolbar at the bottom (Chrome for iOS's layout) ---------- */

static void sheet_open(void);
static void tab_new(void);
static void tab_close(int i);
static void tab_add_bg(const char *url, bool priv);
static void grid_open(void);
static void msheet_open(void);
static int ntabs, cur_tab;
static bool cutting;		/* a tab switch cuts the pages away: they don't become Forward */
static lv_obj_t *app_rt;	/* the app's root: the tab grid + the menu sheet go over the pages */
static struct bar root_bar;	/* the start page's toolbar */

static void host_click(lv_event_t *e)
{
	(void)e;
	sheet_open();
}

static void reload_click(lv_event_t *e)
{
	(void)e;
	struct wpage *w = top_page();
	if (!w)
		return;
	if (w->loading) {	/* stop */
		net_cancel(w->req);
		w->req = NULL;
		w->loading = false;
		w->gen = gen_next++;
		if (w->spinner) {
			lv_obj_delete(w->spinner);
			w->spinner = NULL;
		}
		set_host(w);
		if (!w->doc)
			status_show(w, G_GLOBE, "Stopped", "", ST_RETRY);
		return;
	}
	page_load(w, w->url);
}

enum { M_FWD = 1, M_RELOAD, M_FAV, M_FAVS, M_HIST, M_FIND, M_TEXT, M_DESK, M_COPY, M_FULL, M_CLEAR, M_NEWTAB, M_CLOSETAB };

static void open_list(bool favorites);
static void find_open(void);
static void text_bar(void);

static void clear_data_ok(int idx, void *ud)
{
	(void)ud;
	if (idx != 1)
		return;
	net_clear_cookies();
	wicon_clear(NULL, 0);
	root_build();
	ui_haptic_success();
}

static void menu_pick(int id)
{
	struct wpage *w = top_page();
	switch (id) {
	case M_FWD:
		if (nfwd) {
			char u[1024];
			snprintf(u, sizeof(u), "%s", fwd[--nfwd]);
			int keep = nfwd;
			navigate(u, false, true, NULL);
			nfwd = keep;	/* still more forward pages after this one */
		}
		break;
	case M_FIND:
		find_open();
		break;
	case M_TEXT:
		text_bar();
		break;
	case M_DESK:
		if (w) {
			desk_set(w->url, !desk_on(w->url));
			page_load(w, w->url);
		}
		break;
	case M_COPY:
		if (w) {
			snprintf(clip, sizeof(clip), "%s", w->url);
			root_build();
			ui_haptic_success();
		}
		break;
	case M_FULL:
		if (w && web_full_open)
			web_full_open(w->url);
		else if (web_full_open)
			web_full_open("");
		break;
	case M_CLEAR:
		ui_alert("Clear Cookies & Data?", "Every site you are logged in to logs you out.",
			 (const char *[]){ "Cancel", "!Clear" }, 2, clear_data_ok, NULL);
		break;
	case M_RELOAD:
		if (w)
			reload_click(NULL);	/* (stop while it loads) */
		break;
	case M_FAV:
		if (w) {
			fav_toggle(w->url, w->title);
			ui_haptic_success();
		}
		break;
	case M_FAVS:
		open_list(true);
		break;
	case M_HIST:
		open_list(false);
		break;
	case M_NEWTAB:
		tab_new();
		break;
	case M_CLOSETAB:
		tab_close(cur_tab);
		break;
	}
}

static void page_leave(void);
static void find_forget(struct wpage *w);
static void tbar_forget(struct wpage *w);

static void back_click(lv_event_t *e)
{
	(void)e;
	if (nav_depth() > 0 && !nav_locked())
		nav_pop();
}

static void fwd_click(lv_event_t *e)
{
	(void)e;
	menu_pick(M_FWD);
}

static void tab_thumb(void);

static void plus_click(lv_event_t *e)
{
	(void)e;
	tab_thumb();	/* the page as it is (the ⋯ sheet and the grid took theirs before they covered it) */
	tab_new();
}

static void tabs_click(lv_event_t *e)
{
	(void)e;
	grid_open();
}

static void dots_click(lv_event_t *e)
{
	(void)e;
	msheet_open();
}

static lv_obj_t *tool_btn(lv_obj_t *tb, const char *glyph, lv_event_cb_t cb)
{
	lv_obj_t *b = box(tb);
	lv_obj_set_size(b, 120, 96);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_opa(b, LV_OPA_40, LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	if (glyph)
		lv_obj_center(ui_icon(b, glyph, font(F_MONO, 54), pal.label));
	return b;
}

/* back, forward, new tab, tabs (a rounded square with the count), menu — evenly spread, solid, a hairline on top */
static lv_obj_t *toolbar(lv_obj_t *page, struct bar *br)
{
	lv_obj_t *tb = box(page);
	lv_obj_set_size(tb, SCR_W, TB_H);
	lv_obj_align(tb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(tb, C(pal.bg), 0);
	lv_obj_set_style_border_side(tb, LV_BORDER_SIDE_TOP, 0);
	lv_obj_set_style_border_width(tb, 1, 0);
	lv_obj_set_style_border_color(tb, C(pal.separator), 0);
	lv_obj_set_style_pad_bottom(tb, BOTTOM_PAD, 0);
	lv_obj_set_style_pad_hor(tb, 6, 0);
	lv_obj_set_flex_flow(tb, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tb, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);	/* taps between the buttons stay here */
	br->bk = lv_obj_get_child(tool_btn(tb, G_M_BACK, back_click), 0);
	br->fw = lv_obj_get_child(tool_btn(tb, G_M_FWD, fwd_click), 0);
	tool_btn(tb, G_M_PLUS, plus_click);
	lv_obj_t *t = tool_btn(tb, NULL, tabs_click);
	lv_obj_t *sq = box(t);
	lv_obj_set_size(sq, 46, 46);
	lv_obj_center(sq);
	lv_obj_set_style_radius(sq, 11, 0);
	lv_obj_set_style_border_width(sq, 4, 0);
	lv_obj_set_style_border_color(sq, C(pal.label), 0);
	lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE);	/* the tap is its button's */
	br->cnt = text(sq, "", font(F_TEXT_BOLD, 24), pal.label);
	lv_obj_center(br->cnt);
	tool_btn(tb, G_M_DOTS, dots_click);
	br->bk_on = br->fw_on = br->n = -1;
	return tb;
}

/* only what changed is set: an unchanged toolbar draws nothing */
static void bar_set(struct bar *b, bool bk, bool fw)
{
	if (!b->bk)
		return;
	if (b->bk_on != bk) {
		b->bk_on = bk;
		lv_obj_set_style_text_color(b->bk, C(bk ? pal.label : pal.label3), 0);
	}
	if (b->fw_on != fw) {
		b->fw_on = fw;
		lv_obj_set_style_text_color(b->fw, C(fw ? pal.label : pal.label3), 0);
	}
	if (b->n != ntabs) {
		b->n = ntabs;
		char t[8];
		snprintf(t, sizeof(t), ntabs > 99 ? ":)" : "%d", ntabs);
		lv_label_set_text(b->cnt, t);
	}
}

/* the toolbars that can show: the top page's and the start page's (a page under another syncs when it is on top again) */
static void bars_sync(void)
{
	struct wpage *w = top_page();
	if (w)
		bar_set(&w->bar, true, nfwd > 0);
	bar_set(&root_bar, false, nfwd > 0);
}

/* omnibox: a capsule with the lock + the host (or the search words) centred, reload / stop at its end; the
 * loading bar runs along the bar's bottom edge. Tap = edit the address (sheet_open) */
static void omnibox(struct wpage *w)
{
	lv_obj_t *bar = box(w->page);
	lv_obj_set_size(bar, SCR_W, PAGE_Y);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, C(pal.bg), 0);
	lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_width(bar, 1, 0);
	lv_obj_set_style_border_color(bar, C(pal.separator), 0);
	lv_obj_t *pill = box(bar);
	lv_obj_set_size(pill, SCR_W - 2 * 24, PILL_H);
	lv_obj_set_pos(pill, 24, PILL_Y);
	lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(pill, C(pal.fill), 0);
	lv_obj_set_style_bg_color(pill, C(mix(pal.fill, pal.label, 12)), LV_STATE_PRESSED);
	lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(pill, host_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *mid = box(pill);
	lv_obj_set_size(mid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_max_width(mid, SCR_W - 48 - 2 * 92, 0);
	lv_obj_center(mid);
	lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(mid, 10, 0);
	lv_obj_remove_flag(mid, LV_OBJ_FLAG_CLICKABLE);
	w->lock = ui_icon(mid, G_M_LOCK, font(F_MONO, 28), pal.label2);
	w->host = text(mid, "", font(F_TEXT_MEDIUM, 32), pal.label);
	lv_label_set_long_mode(w->host, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_height(w->host, lv_font_get_line_height(font(F_TEXT_MEDIUM, 32)));	/* one line, then … */
	lv_obj_set_style_max_width(w->host, SCR_W - 48 - 2 * 92 - 40, 0);
	w->reload = ui_icon(pill, G_M_RELOAD, font(F_MONO, 42), pal.label);
	lv_obj_align(w->reload, LV_ALIGN_RIGHT_MID, -26, 0);
	lv_obj_add_flag(w->reload, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(w->reload, 24);
	lv_obj_set_style_opa(w->reload, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_add_event_cb(w->reload, reload_click, LV_EVENT_CLICKED, NULL);
	w->prog = box(bar);
	lv_obj_set_size(w->prog, 0, 4);
	lv_obj_set_pos(w->prog, 0, PAGE_Y - 4);
	lv_obj_set_style_bg_opa(w->prog, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(w->prog, C(pal.tint), 0);
	lv_obj_set_style_radius(w->prog, 2, 0);
	lv_obj_add_flag(w->prog, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_remove_flag(w->prog, LV_OBJ_FLAG_CLICKABLE);
}

static struct wpage *page_new(const char *url)
{
	if (nwp >= MAX_WEB_PAGES)
		return NULL;
	struct wpage *w = calloc(1, sizeof(*w));
	if (!w)
		return NULL;
	lv_obj_t *c = nav_push("");
	if (!c) {
		free(w);
		return NULL;
	}
	wps[++nwp] = w;
	nav_on_leave(page_leave);
	w->page = nav_page();
	w->col = c;
	snprintf(w->url, sizeof(w->url), "%s", url);
	lv_obj_set_style_bg_color(w->page, C(pal.bg), 0);
	/* the nav bar gives way to the omnibox (the page keeps the edge swipe back) */
	for (uint32_t k = 0; k < lv_obj_get_child_count(w->page); k++)
		if (lv_obj_get_child(w->page, k) != c)
			lv_obj_add_flag(lv_obj_get_child(w->page, k), LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_pos(c, 0, PAGE_Y);
	lv_obj_set_height(c, SCR_H - PAGE_Y);
	lv_obj_set_style_pad_bottom(c, TB_H + 40, 0);
	lv_obj_set_style_pad_top(c, 8, 0);
	lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_ACTIVE);
	lv_obj_set_style_width(c, 6, LV_PART_SCROLLBAR);
	lv_obj_set_style_radius(c, 3, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_color(c, C(pal.label3), LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_right(c, 4, LV_PART_SCROLLBAR);
	omnibox(w);
	w->tb = toolbar(w->page, &w->bar);
	bar_set(&w->bar, true, nfwd > 0);
	return w;
}

/* a restored tab's older page comes on top (back): load it now */
static void lazy_now(void *ud)
{
	(void)ud;
	struct wpage *t = top_page();
	if (t && t->lazy) {
		t->lazy = false;
		page_load(t, t->url);
	}
}

static void page_leave(void)
{
	if (!nwp)
		return;
	struct wpage *w = wps[nwp];
	wps[nwp--] = NULL;
	if (w) {
		if (sheet && lv_obj_get_parent(sheet) == w->page) {
			if (sel_ta == sheet_ta)
				sel_ta = NULL;
			sheet = sheet_ta = sheet_box = sheet_x = NULL;
			nav_lock(false);
		}
		find_forget(w);
		tbar_forget(w);
		if (!cutting && nfwd < 16 && w->url[0])	/* back → this one becomes Forward */
			snprintf(fwd[nfwd++], sizeof(fwd[0]), "%s", w->url);
		if (pressed_sg)
			pressed_span = NULL, pressed_sg = NULL;
		w->col = NULL;	/* the page object goes right after this */
		page_clear(w);
		free(w->post);
		free(w);
	}
	if (cutting)
		return;
	bars_sync();
	struct wpage *t = top_page();
	if (t && t->lazy)
		lv_async_call(lazy_now, NULL);
	tab_dirty();
}

static void sheet_drop(void)
{
	if (!sheet)
		return;
	if (kbd_visible())
		kbd_hide();
	if (sel_ta == sheet_ta)
		sel_ta = NULL;
	lv_obj_delete(sheet);
	sheet = sheet_ta = sheet_box = sheet_x = NULL;
	nav_lock(false);
}

/* open url: links push a new page (or replace the top one when the stack is full). Runs from
 * lv_async_call: the callers are event handlers of objects this may delete (rows, the sheet). */
static char go_url[2048], go_ref[2048];
static char *go_post;
static bool go_link, go_priv;

/* what the page is fetched with: from navigate() */
static void go_apply(struct wpage *w)
{
	w->allow_priv = go_priv;
	snprintf(w->referer, sizeof(w->referer), "%s", go_ref);
	free(w->post);
	w->post = go_post;
	go_post = NULL;
	w->refreshes = 0;
}

static void navigate_now(void *ud)
{
	(void)ud;
	const char *url = go_url;
	bool from_link = go_link;
	sheet_drop();
	struct wpage *cur = top_page();
	if (from_link)
		nfwd = 0;
	if (cur && cur->doc && !go_post) {	/* same page, other #fragment: just scroll */
		const char *h = strchr(url, '#');
		int bl = strcspn(cur->url, "#");
		if (h && (int)(h - url) == bl && !strncmp(url, cur->url, bl)) {
			char frag[256];
			snprintf(frag, sizeof(frag), "%s", h + 1);
			net_urldecode(frag);
			scroll_to_anchor(cur, frag);
			return;
		}
	}
	if (kbd_visible())
		kbd_hide();
	if (cur && !strcmp(cur->url, url) && !go_post) {	/* the same address again = reload */
		go_apply(cur);
		page_load(cur, url);
		return;
	}
	struct wpage *w = page_new(url);
	if (!w) {	/* too deep (or nav busy): the top page shows it instead */
		w = top_page();
		if (!w)
			return;
	}
	go_apply(w);
	page_load(w, url);
	bars_sync();
	tab_dirty();
}

/* open url. from_link: a new branch (Forward is forgotten). user: the user typed / picked it (start page,
 * address sheet, favorites, history, Forward, Open Anyway) — it may be on the LAN / tailnet; otherwise it comes
 * from the top page (link, form, refresh): that page is the Referer, and only a page that is itself on the LAN /
 * tailnet may lead there. post = form body (copied). */
static void navigate(const char *url, bool from_link, bool user, const char *post)
{
	struct wpage *src = top_page();
	snprintf(go_url, sizeof(go_url), "%s", url);
	go_link = from_link;
	go_priv = user || (src && src->priv);
	snprintf(go_ref, sizeof(go_ref), "%s", !user && src ? src->url : "");
	free(go_post);
	go_post = post ? strdup(post) : NULL;
	lv_async_call_cancel(navigate_now, NULL);
	lv_async_call(navigate_now, NULL);
}

/* Full Site's Reader button: open its page here. Not user-typed — the address came from a remote page, so it gets
 * no LAN / tailnet pass (only the top page's own, like a link). */
void web_reader_open(const char *url)
{
	if (url && *url)
		navigate(url, true, false, NULL);
}

/* ---------- long press on a link ---------- */

static char lm_url[2048];
enum { LM_OPEN = 1, LM_COPY, LM_FULL, LM_FAV, LM_NEWTAB };

static void link_menu_pick(int id)
{
	switch (id) {
	case LM_OPEN:
		navigate(lm_url, true, false, NULL);
		break;
	case LM_NEWTAB: {	/* in the background, with the page's own LAN / tailnet pass (like a tap on it) */
		struct wpage *w = top_page();
		tab_add_bg(lm_url, w && w->priv);
		break;
	}
	case LM_COPY:
		snprintf(clip, sizeof(clip), "%s", lm_url);
		root_build();	/* "Paste and Go" on the start page */
		ui_haptic_success();
		break;
	case LM_FULL:
		if (web_full_open)
			web_full_open(lm_url);
		break;
	case LM_FAV:
		if (fav_find(lm_url) < 0)
			fav_toggle(lm_url, NULL);
		ui_haptic_success();
		break;
	}
}

static void link_menu(const char *url, int y)
{
	snprintf(lm_url, sizeof(lm_url), "%s", url);
	char head[72];
	const char *u = strstr(url, "://");
	u = u ? u + 3 : url;
	snprintf(head, sizeof(head), "%s", u);
	utf8_trim(head);
	if (strlen(u) >= sizeof(head))
		snprintf(head + sizeof(head) - 4, 4, "\xe2\x80\xa6");
	ui_menu_item_t it[6];
	int n = 0;
	it[n++] = (ui_menu_item_t){ 0, MI_HEAD, NULL, head };
	it[n++] = (ui_menu_item_t){ LM_OPEN, 0, G_GLOBE, "Open" };
	it[n++] = (ui_menu_item_t){ LM_NEWTAB, 0, G_M_NEWTAB, "Open in New Tab" };
	it[n++] = (ui_menu_item_t){ LM_COPY, 0, G_COPY, "Copy Link" };
	if (web_full_open)
		it[n++] = (ui_menu_item_t){ LM_FULL, 0, G_EXPAND, "Open in Full Site" };
	it[n++] = (ui_menu_item_t){ LM_FAV, MI_GROUP, G_STAR_O, "Add to Favorites" };
	ui_menu(it, n, SCR_W - SIDE, y + 40, y - 40, link_menu_pick);
}

/* ---------- Find on Page ---------- */

#define MAX_HITS 1000
static struct wpage *find_page;
static lv_obj_t *find_bar, *find_ta, *find_count;
static int find_hits[MAX_HITS], nhits, find_cur;

static void find_forget(struct wpage *w)
{
	if (w && find_page == w) {
		find_page = NULL;
		find_bar = find_ta = find_count = NULL;
		nhits = 0;
	}
}

static int count_in(const char *hay, const char *needle, int *hits, int room, int block)
{
	int n = 0, nl = strlen(needle);
	for (const char *s = hay; s && n < room && (s = strcasestr(s, needle)); s += nl)
		hits[n++] = block;
	return n;
}

static void find_label(void)
{
	char t[32];
	if (!find_count)
		return;
	if (!find_ta || !*lv_textarea_get_text(find_ta))
		t[0] = 0;
	else if (!nhits)
		snprintf(t, sizeof(t), "No matches");
	else
		snprintf(t, sizeof(t), "%d of %d%s", find_cur + 1, nhits, nhits == MAX_HITS ? "+" : "");
	lv_label_set_text(find_count, t);
}

static void find_go(int k)
{
	struct wpage *w = top_page();
	if (!w || w != find_page || !nhits)
		return;
	find_cur = (k % nhits + nhits) % nhits;
	find_label();
	int bi = find_hits[find_cur];
	w->anchor = w->hl_want = bi + 1;
	if (bi < w->built)
		anchor_scroll(w);
}

static void find_changed(lv_event_t *e)
{
	(void)e;
	struct wpage *w = top_page();
	if (!w || w != find_page || !w->doc)
		return;
	const char *q = lv_textarea_get_text(find_ta);
	nhits = 0;
	find_cur = 0;
	if (w->hl) {
		lv_obj_set_style_bg_opa(w->hl, LV_OPA_TRANSP, 0);
		w->hl = NULL;
	}
	if (*q)
		for (int i = 0; i < w->doc->nb && nhits < MAX_HITS; i++) {
			html_block_t *b = &w->doc->b[i];
			nhits += count_in(b->text, q, find_hits + nhits, MAX_HITS - nhits, i);
			if (b->snip && nhits < MAX_HITS)
				nhits += count_in(b->snip, q, find_hits + nhits, MAX_HITS - nhits, i);
		}
	find_label();
	if (nhits)
		find_go(0);
}

static void find_next(lv_event_t *e)
{
	(void)e;
	find_go(find_cur + 1);
}

static void find_prev(lv_event_t *e)
{
	(void)e;
	find_go(find_cur - 1);
}

static void find_kbd(lv_event_t *e)	/* the bar rides on top of the keyboard */
{
	if (!find_bar)
		return;
	bool up = lv_event_get_code(e) == EV_KBD_SHOWN;
	lv_obj_align(find_bar, LV_ALIGN_BOTTOM_MID, 0, up ? -kbd_height(KBD_TEXT) + BOTTOM_PAD + 6 : 0);
	if (up)
		kbd_no_autocap();
}

static void find_close_now(void *ud)
{
	(void)ud;
	struct wpage *w = find_page;
	if (!w || !find_bar)
		return;
	if (kbd_visible())
		kbd_hide();
	lv_obj_delete(find_bar);
	if (w->hl)
		lv_obj_set_style_bg_opa(w->hl, LV_OPA_TRANSP, 0);
	w->hl = NULL;
	if (w->tb)
		lv_obj_remove_flag(w->tb, LV_OBJ_FLAG_HIDDEN);
	find_forget(w);
}

static void find_done(lv_event_t *e)
{
	(void)e;
	lv_async_call(find_close_now, NULL);	/* the button is inside the bar */
}

static void find_ready(lv_event_t *e)	/* "done" on the keyboard: next match, keyboard away */
{
	(void)e;
	kbd_hide();
	find_go(find_cur + 1);
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb)
{
	lv_obj_t *b = ui_icon_button(parent, glyph, 76, pal.cell, pal.label);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

static void find_open(void)
{
	struct wpage *w = top_page();
	if (!w || !w->doc || find_bar)
		return;
	find_page = w;
	find_bar = box(w->page);
	lv_obj_set_size(find_bar, SCR_W, TB_H);
	lv_obj_align(find_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(find_bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(find_bar, C(pal.bg), 0);
	lv_obj_set_style_pad_hor(find_bar, SIDE - 8, 0);
	lv_obj_set_style_pad_bottom(find_bar, BOTTOM_PAD + 6, 0);
	lv_obj_set_flex_flow(find_bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(find_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(find_bar, 12, 0);
	lv_obj_add_flag(find_bar, LV_OBJ_FLAG_CLICKABLE);
	find_ta = ui_search(find_bar, 300, "Find on Page");
	lv_obj_t *wrap = lv_obj_get_parent(find_ta);
	lv_obj_set_flex_grow(wrap, 1);
	find_count = text(wrap, "", font(F_TEXT, 24), pal.label2);
	round_btn(find_bar, G_CHEVRON_U, find_prev);
	round_btn(find_bar, G_CHEVRON_D, find_next);
	lv_obj_t *done = text(find_bar, "Done", font(F_TEXT_SEMI, 32), pal.tint);
	lv_obj_add_flag(done, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(done, 20);
	lv_obj_set_style_pad_bottom(done, 20, 0);
	lv_obj_set_style_opa(done, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_add_event_cb(done, find_done, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(find_ta, find_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(find_ta, find_ready, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(find_ta, find_kbd, EV_KBD_SHOWN, NULL);
	lv_obj_add_event_cb(find_ta, find_kbd, EV_KBD_HIDDEN, NULL);
	lv_obj_add_flag(w->tb, LV_OBJ_FLAG_HIDDEN);
	nhits = find_cur = 0;
	kbd_attach(find_ta, KBD_TEXT);
}

/* ---------- Text Size: a small bar over the toolbar (A− 115% A+), gone after a few seconds ---------- */

static struct wpage *tbar_page;
static lv_obj_t *tbar, *tbar_pct;
static lv_timer_t *tbar_timer;

static void tbar_forget(struct wpage *w)
{
	if (w && tbar_page == w) {
		tbar_page = NULL;
		tbar = tbar_pct = NULL;
		if (tbar_timer)
			lv_timer_delete(tbar_timer);
		tbar_timer = NULL;
	}
}

static void tbar_expire(lv_timer_t *t)
{
	(void)t;
	tbar_timer = NULL;	/* one-shot: LVGL deletes it */
	if (tbar)
		lv_obj_delete(tbar);
	tbar_page = NULL;
	tbar = tbar_pct = NULL;
}

static void tbar_keep(void)
{
	if (tbar_timer)
		lv_timer_reset(tbar_timer);
	else {
		tbar_timer = lv_timer_create(tbar_expire, 3500, NULL);
		lv_timer_set_repeat_count(tbar_timer, 1);
	}
}

static void page_rebuild(struct wpage *w);

static void tsize_step(lv_event_t *e)
{
	int d = (int)(intptr_t)lv_event_get_user_data(e);
	tsize();
	int ni = tsize_i + d;
	if (ni < 0 || ni >= NTSIZES)
		return;
	tsize_i = ni;
	prefs_set_int("web_text", ni);
	char t[16];
	snprintf(t, sizeof(t), "%d%%", tsize());
	if (tbar_pct)
		lv_label_set_text(tbar_pct, t);
	tbar_keep();
	struct wpage *w = top_page();
	if (w)
		page_rebuild(w);
}

static void text_bar(void)
{
	struct wpage *w = top_page();
	if (!w || tbar)
		return;
	tbar_page = w;
	tbar = box(w->page);
	lv_obj_set_size(tbar, 360, 96);
	lv_obj_align(tbar, LV_ALIGN_BOTTOM_MID, 0, -TB_H + BOTTOM_PAD - 6);
	lv_obj_set_style_radius(tbar, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(tbar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(tbar, C(th.dark ? mix(pal.cell, th.fg, 7) : 0xffffff), 0);
	lv_obj_set_style_border_width(tbar, 1, 0);
	lv_obj_set_style_border_color(tbar, C(pal.separator), 0);
	lv_obj_set_style_pad_hor(tbar, 10, 0);
	lv_obj_set_flex_flow(tbar, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(tbar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(tbar, LV_OBJ_FLAG_CLICKABLE);
	static const int dn = -1, up = 1;
	lv_obj_t *a = ui_icon_button(tbar, "A", 76, pal.fill, pal.label);
	lv_obj_set_style_text_font(lv_obj_get_child(a, 0), font(F_TEXT_SEMI, 26), 0);
	lv_obj_add_event_cb(a, tsize_step, LV_EVENT_CLICKED, (void *)(intptr_t)dn);
	char t[16];
	snprintf(t, sizeof(t), "%d%%", tsize());
	tbar_pct = text(tbar, t, font(F_TEXT_SEMI, 32), pal.label);
	lv_obj_t *b = ui_icon_button(tbar, "A", 76, pal.fill, pal.label);
	lv_obj_set_style_text_font(lv_obj_get_child(b, 0), font(F_TEXT_SEMI, 40), 0);
	lv_obj_add_event_cb(b, tsize_step, LV_EVENT_CLICKED, (void *)(intptr_t)up);
	tbar_keep();
}

/* everything built again at the current text size, the first block on screen kept there */
static void page_rebuild(struct wpage *w)
{
	if (w->doc && !w->doc->nb)	/* only the status card: nothing to resize */
		w->tsz = tsize();
	if (!w->doc || !w->doc->nb || !w->col || w->loading)
		return;
	int first = 0, sy = lv_obj_get_scroll_y(w->col);
	for (uint32_t k = 0; k < lv_obj_get_child_count(w->col); k++) {
		lv_obj_t *c = lv_obj_get_child(w->col, k);
		int ud = (int)(intptr_t)lv_obj_get_user_data(c);
		if (ud > 0 && lv_obj_get_y(c) + lv_obj_get_height(c) > sy) {
			first = ud - 1;
			break;
		}
	}
	for (int k = 0; k < w->doc->nimgs; k++)
		w->imgs[k].box = w->imgs[k].img = NULL;
	pressed_span = NULL;
	pressed_sg = NULL;
	lv_obj_clean(w->col);
	w->spinner = w->status = w->more = NULL;
	free(w->boxsg);
	w->boxsg = NULL;
	w->nboxsg = w->capboxsg = 0;
	w->menu_card = w->menu_list = w->hl = NULL;
	w->built = 0;
	w->tsz = tsize();
	build_more(w, 0);
	if (first > 0) {
		w->anchor = first + 1;
		w->hl_want = 0;
	}
}

/* ---------- address sheet (over a web page) + start page suggestions ---------- */

static void go_text(const char *t)
{
	char url[2048];
	bool search;
	if (!input_url(t, url, sizeof(url), &search))
		return;
	navigate(url, true, true, NULL);
}

static void suggest_click(lv_event_t *e)
{
	navigate(lv_event_get_user_data(e), true, true, NULL);
}

static void paste_click(lv_event_t *e)
{
	(void)e;
	if (clip[0])
		navigate(clip, true, true, NULL);
}

static void search_click(lv_event_t *e)
{
	(void)e;
	lv_obj_t *ta = sheet_ta ? sheet_ta : addr_ta;
	char t[1024];
	snprintf(t, sizeof(t), "%s", lv_textarea_get_text(ta));
	go_text(t);	/* the sheet goes when the page opens */
}

/* a favorite / history entry as a list row (start page, suggestions, address sheet, Favorites / History
 * pages): the site's monogram tile, title, host (+ when, for history) */
static lv_obj_t *site_row(lv_obj_t *sec, const struct entry *en, bool timed);

static void clear_hist_ok(int idx, void *ud)
{
	(void)ud;
	if (idx != 1)
		return;
	nhist = 0;
	save_list("history.txt", hist, 0, true);
	const char *keep[MAX_FAV];	/* the icons of the sites it listed go too, the favorites' stay */
	for (int i = 0; i < nfav; i++)
		keep[i] = favs[i].url;
	wicon_clear(keep, nfav);
	root_build();
}

static void clear_hist_click(lv_event_t *e)
{
	(void)e;
	ui_alert("Clear History?", "Every site you visited will be removed from the list.",
		 (const char *[]){ "Cancel", "!Clear" }, 2, clear_hist_ok, NULL);
}

/* ---------- start page: Favorites grid, Recently Visited, Full Site (iOS 26 Safari's start page) ---------- */

#define FAV_COLS 4
#define FAV_FOLD 8	/* favorites shown before "Show All" (2 rows) */
#define FAV_PAD 12	/* grid card padding */
#define FAV_CELL ((SCR_W - 2 * SIDE - 2 * FAV_PAD) / FAV_COLS)
#define FAV_TILE 108
_Static_assert(FAV_TILE == WICON_BIG, "favorite tiles are the size web_icon.c composes");
#define RECENT_N 6	/* Recently Visited rows on the start page; the rest under "Show All History" */

static bool fav_all;	/* the start page shows every favorite */

/* what a site goes by: "en.m.wikipedia.org" → "wikipedia", "news.ycombinator.com" → "ycombinator",
 * "www.bbc.co.uk" → "bbc"; an IP address stays whole */
static void site_name(const char *host, char *out, int n)
{
	int len = strcspn(host, ":");
	if (dotted_quad(host, len) || !strchr(host, '.')) {
		snprintf(out, n, "%.*s", len, host);
		return;
	}
	const char *lab[16];
	int ll[16], k = 0;
	for (const char *p = host; k < 16;) {
		const char *d = strchr(p, '.');
		lab[k] = p;
		ll[k++] = d ? (int)(d - p) : (int)strcspn(p, ":");
		if (!d)
			break;
		p = d + 1;
	}
	int pick = k - 2;
	if (k >= 3 && ll[k - 1] == 2 && ll[k - 2] <= 3)	/* co.uk, com.au, ne.jp */
		pick = k - 3;
	snprintf(out, n, "%.*s", ll[pick], lab[pick]);
}

/* deterministic tile colour from the site's name, out of the theme's own colours */
static uint32_t site_color(const char *name)
{
	const uint32_t c[] = { pal.blue, pal.purple, pal.teal, pal.green, pal.orange, pal.red, pal.yellow, pal.indigo };
	uint32_t h = 2166136261u;
	for (const char *p = name; *p; p++)
		h = (h ^ (uint8_t)tolower((unsigned char)*p)) * 16777619u;
	return c[h % (sizeof(c) / sizeof(c[0]))];
}

static int lum(uint32_t c) { return ((c >> 16 & 0xff) * 299 + (c >> 8 & 0xff) * 587 + (c & 0xff) * 114) / 1000; }

/* same hue, saturation ≥ 55 %, value ≤ 78 % (HSV): a pastel theme colour that can carry a white letter */
static uint32_t richer(uint32_t c)
{
	int r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
	int mx = LV_MAX(r, LV_MAX(g, b)), mn = LV_MIN(r, LV_MIN(g, b));
	if (mx == 0)
		return c;
	int v = LV_MIN(mx, 199), s = LV_MAX((mx - mn) * 255 / mx, 140);	/* 0..255 */
	int lo = v * (255 - s) / 255;	/* new min */
	int ch[3] = { r, g, b };
	for (int i = 0; i < 3; i++)	/* each channel keeps its place between min and max: the hue stays */
		ch[i] = mx == mn ? v : lo + (ch[i] - mn) * (v - lo) / (mx - mn);
	return (uint32_t)ch[0] << 16 | (uint32_t)ch[1] << 8 | (uint32_t)ch[2];
}

/* monogram tile: the site's first letter on its colour, a soft top-to-bottom shade like the home icons */
static lv_obj_t *mono_tile(lv_obj_t *parent, const struct entry *en, int size)
{
	char h[256], name[128], m[8] = "";
	net_host(en->url, h, sizeof(h));
	site_name(h, name, sizeof(name));
	const char *src = !strncmp(name, "xn--", 4) || isdigit((unsigned char)name[0]) ? en->title : name;
	for (const char *p = src; *p && !m[0]; p++)
		if (isalnum((unsigned char)*p))
			m[0] = toupper((unsigned char)*p);
	if (!m[0])
		snprintf(m, sizeof(m), "%s", G_GLOBE);
	uint32_t c = site_color(name);
	if (lum(c) > 140)	/* pastel themes: the same hue, richer + deeper, so tiles read as solid icons */
		c = richer(c);
	uint32_t top = mix(c, 0xffffff, 12), bot = mix(c, 0x000000, 14);
	lv_obj_t *t = box(parent);
	lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(t, size, size);
	lv_obj_set_style_radius(t, size * 27 / 100, 0);
	lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(t, C(top), 0);
	lv_obj_set_style_bg_grad_color(t, C(bot), 0);
	lv_obj_set_style_bg_grad_dir(t, LV_GRAD_DIR_VER, 0);
	uint32_t ink = lum(c) > 180 ? mix(c, 0x000000, 72) : 0xffffff;
	lv_obj_t *l = text(t, m, font(m[1] ? F_MONO : F_TEXT_BOLD, size * 46 / 100 & ~1), ink);
	if (m[1])	/* a glyph, not a letter: its ink centred */
		ui_glyph_fit(l);
	lv_obj_center(l);
	return t;
}

/* the site's icon (web_icon.c) when there is one, else its monogram */
static lv_obj_t *site_tile(lv_obj_t *parent, const struct entry *en, int size)
{
	const lv_image_dsc_t *ic = wicon_tile(en->url, size);
	if (!ic)
		return mono_tile(parent, en, size);
	lv_obj_t *t = box(parent);
	lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(t, size, size);
	lv_obj_t *im = lv_image_create(t);
	lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
	lv_image_set_src(im, ic);
	lv_obj_center(im);
	if (!th.dark) {	/* light themes: a hairline, so a white icon doesn't melt into the white card */
		lv_obj_set_style_radius(t, size * 27 / 100, 0);
		lv_obj_set_style_border_width(t, 1, 0);
		lv_obj_set_style_border_color(t, C(pal.separator), 0);
		lv_obj_set_style_border_post(t, true, 0);
	}
	return t;
}

/* "Systemd - ArchWiki" → "Systemd", "Show HN: … | Hacker News" → "Show HN: …" (tile labels) */
static void short_title(const char *t, char *out, int n)
{
	snprintf(out, n, "%s", t);
	static const char *const seps[] = { " - ", " | ", " \xe2\x80\x94 ", " \xe2\x80\x93 ", " \xc2\xb7 " };
	for (unsigned i = 0; i < sizeof(seps) / sizeof(seps[0]); i++) {
		char *s = strstr(out, seps[i]);
		if (s && s - out >= 3)
			*s = 0;
	}
}

static void rel_time(long when, char *b, int n)
{
	long d = time(NULL) - when;
	if (when <= 0)
		*b = 0;
	else if (d < 60)
		snprintf(b, n, "Just now");
	else if (d < 3600)
		snprintf(b, n, "%ld min ago", d / 60);
	else if (d < 86400)
		snprintf(b, n, "%ld h ago", d / 3600);
	else if (d < 2 * 86400)
		snprintf(b, n, "Yesterday");
	else if (d < 7 * 86400)
		snprintf(b, n, "%ld days ago", d / 86400);
	else {
		time_t w = when;
		strftime(b, n, "%b %-d", localtime(&w));
	}
}

/* section header: bold title + an optional tint action on the right */
static void start_head(lv_obj_t *boxo, const char *title, const char *action, lv_event_cb_t cb)
{
	lv_obj_t *h = box(boxo);
	lv_obj_set_size(h, SCR_W - 2 * SIDE - 16, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(h, 40, 0);
	lv_obj_set_style_pad_bottom(h, 16, 0);
	lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(h, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	text(h, title, font(F_TEXT_SEMI, 32), pal.label);
	if (action) {
		lv_obj_t *a = text(h, action, font(F_TEXT, 30), pal.tint);
		lv_obj_add_flag(a, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(a, 24);
		lv_obj_set_style_opa(a, LV_OPA_50, LV_STATE_PRESSED);
		lv_obj_set_style_pad_bottom(a, 4, 0);
		lv_obj_add_event_cb(a, cb, LV_EVENT_CLICKED, NULL);
	}
}

static lv_obj_t *start_card(lv_obj_t *boxo)
{
	lv_obj_t *s = box(boxo);
	lv_obj_set_size(s, SCR_W - 2 * SIDE, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(s, 36, 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(s, C(pal.cell), 0);
	return s;
}

/* long press on a favorite tile / a Recently Visited row */
enum { SM_OPEN = 1, SM_COPY, SM_FULL, SM_FAV_ADD, SM_FAV_DEL, SM_HIST_DEL, SM_NEWTAB };
static char sm_url[1024];

static int hist_find(const char *url)
{
	for (int i = nhist - 1; i >= 0; i--)
		if (!strcmp(hist[i].url, url))
			return i;
	return -1;
}

static void hist_remove(const char *url)
{
	int i = hist_find(url);
	if (i < 0)
		return;
	memmove(hist + i, hist + i + 1, (nhist - i - 1) * sizeof(hist[0]));
	nhist--;
	save_list("history.txt", hist, nhist, true);
	root_build();
}

static void start_menu_pick(int id)
{
	switch (id) {
	case SM_OPEN:
		navigate(sm_url, true, true, NULL);
		break;
	case SM_NEWTAB:
		tab_add_bg(sm_url, true);	/* the user's own pick */
		break;
	case SM_COPY:
		snprintf(clip, sizeof(clip), "%s", sm_url);
		root_build();	/* "Paste and Go" */
		ui_haptic_success();
		break;
	case SM_FULL:
		if (web_full_open)
			web_full_open(sm_url);
		break;
	case SM_FAV_ADD: {
		int i = hist_find(sm_url);
		if (fav_find(sm_url) < 0)
			fav_toggle(sm_url, i >= 0 ? hist[i].title : NULL);
		ui_haptic_success();
		break;
	}
	case SM_FAV_DEL:
		if (fav_find(sm_url) >= 0)
			fav_toggle(sm_url, NULL);
		ui_haptic_tap();
		break;
	case SM_HIST_DEL:
		hist_remove(sm_url);
		ui_haptic_tap();
		break;
	}
}

static int start_menu_items(ui_menu_item_t *it, const char *url, bool fav)
{
	static char head[72];
	snprintf(sm_url, sizeof(sm_url), "%s", url);
	net_host(url, head, sizeof(head));
	int n = 0;
	it[n++] = (ui_menu_item_t){ 0, MI_HEAD, NULL, head };
	it[n++] = (ui_menu_item_t){ SM_OPEN, 0, G_GLOBE, "Open" };
	it[n++] = (ui_menu_item_t){ SM_NEWTAB, 0, G_M_NEWTAB, "Open in New Tab" };
	it[n++] = (ui_menu_item_t){ SM_COPY, 0, G_COPY, "Copy Link" };
	if (web_full_open)
		it[n++] = (ui_menu_item_t){ SM_FULL, 0, G_EXPAND, "Open in Full Site" };
	if (fav)
		it[n++] = (ui_menu_item_t){ SM_FAV_DEL, MI_GROUP | MI_RED, G_TRASH, "Delete" };
	else {
		if (fav_find(url) < 0)
			it[n++] = (ui_menu_item_t){ SM_FAV_ADD, MI_GROUP, G_STAR_O, "Add to Favorites" };
		it[n++] = (ui_menu_item_t){ SM_HIST_DEL, fav_find(url) < 0 ? 0 : MI_GROUP, G_TRASH, "Delete" };
		it[n - 1].flags |= MI_RED;
	}
	return n;
}

static void fav_hold(lv_event_t *e)
{
	lv_obj_t *cell = lv_event_get_current_target(e);
	ui_menu_item_t it[8];
	int n = start_menu_items(it, lv_event_get_user_data(e), true);
	lv_area_t a;
	lv_obj_get_coords(cell, &a);
	int right = a.x1 < SCR_W / 2 ? a.x1 + 480 : a.x2;	/* the card hangs from the tile's side (ui.c MENU_W) */
	ui_menu(it, n, right, a.y2 + 10, a.y1 - 10, start_menu_pick);
}

static void hist_hold(lv_event_t *e)
{
	ui_menu_item_t it[8];
	int n = start_menu_items(it, lv_event_get_user_data(e), false);
	ui_row_menu(lv_event_get_current_target(e), it, n, start_menu_pick);
}

static void hist_swiped(lv_event_t *e)
{
	hist_remove(lv_event_get_user_data(e));
	ui_haptic_tap();
}

static void fav_all_click(lv_event_t *e)
{
	(void)e;
	fav_all = !fav_all;
	root_build();
}

static void hist_all_click(lv_event_t *e)
{
	(void)e;
	open_list(false);
}

static void full_site_click(lv_event_t *e)
{
	(void)e;
	if (web_full_open)
		web_full_open("");
}

static void fav_cell(lv_obj_t *grid, struct entry *en)
{
	lv_obj_t *c = box(grid);
	lv_obj_set_size(c, FAV_CELL, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(c, 28, 0);
	lv_obj_set_style_pad_top(c, 16, 0);
	lv_obj_set_style_pad_bottom(c, 14, 0);
	lv_obj_set_style_pad_row(c, 12, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	ui_row_tap(c, suggest_click, en->url);
	ui_row_hold(c, fav_hold, en->url);
	site_tile(c, en, FAV_TILE);
	char t[160];
	short_title(en->title[0] ? en->title : en->url, t, sizeof(t));
	lv_obj_t *l = text(c, t, font(F_TEXT, 24), pal.label);
	lv_obj_set_size(l, FAV_CELL - 8, lv_font_get_line_height(font(F_TEXT, 24)));	/* one line, "…" */
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

static lv_obj_t *site_row(lv_obj_t *sec, const struct entry *en, bool timed)
{
	char h[256], sub[320], when[40] = "";
	net_host(en->url, h, sizeof(h));
	lv_obj_t *r = ui_row(sec, NULL, 0, en->title[0] ? en->title : en->url);
	lv_obj_t *t = site_tile(r, en, WICON_SMALL);
	lv_obj_move_to_index(t, lv_obj_get_index(r) > 0 ? 1 : 0);	/* after the separator, before the text */
	if (lv_obj_get_index(r) > 0)	/* ui_row insets the hairline to the text only when it has an icon */
		lv_obj_set_x(lv_obj_get_child(r, 0), 82);
	if (timed)
		rel_time(en->when, when, sizeof(when));
	snprintf(sub, sizeof(sub), when[0] ? "%s  \xc2\xb7  %s" : "%s", h, when);
	ui_row_subtitle(r, sub);
	ui_row_tap(r, suggest_click, (void *)en->url);
	return r;
}

static void recent_row(lv_obj_t *sec, struct entry *en)
{
	lv_obj_t *r = site_row(sec, en, true);
	ui_row_hold(r, hist_hold, en->url);
	ui_row_swipe(r, G_TRASH, "Delete", pal.red, hist_swiped, en->url);
}

static void start_build(lv_obj_t *boxo)
{
	if (clip[0]) {
		start_head(boxo, "Copied Link", NULL, NULL);
		lv_obj_t *sec = start_card(boxo);
		lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
		lv_obj_t *r = ui_row(sec, G_PASTE, pal.blue, "Paste and Go");
		ui_row_subtitle(r, clip);
		ui_row_tap(r, paste_click, NULL);
	}
	if (nfav) {
		char all[32] = "";
		if (nfav > FAV_FOLD)
			snprintf(all, sizeof(all), fav_all ? "Show Less" : "Show All");
		start_head(boxo, "Favorites", all[0] ? all : NULL, fav_all_click);
		lv_obj_t *g = start_card(boxo);	/* the tiles on the page itself (Chrome's shortcuts) */
		lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
		lv_obj_set_style_pad_all(g, FAV_PAD, 0);
		lv_obj_set_style_pad_top(g, 0, 0);
		lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
		int n = fav_all ? nfav : LV_MIN(nfav, FAV_FOLD);
		for (int i = 0; i < n; i++)
			fav_cell(g, &favs[i]);
	}
	if (nhist) {
		start_head(boxo, "Recently visited", "Clear", clear_hist_click);
		lv_obj_t *sec = start_card(boxo);
		lv_obj_set_flex_flow(sec, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_clip_corner(sec, true, 0);	/* pressed rows + swipe pills keep the corners */
		for (int i = nhist - 1, k = 0; i >= 0 && k < RECENT_N; i--, k++)
			recent_row(sec, &hist[i]);
		if (nhist > RECENT_N) {
			lv_obj_t *r = ui_row(sec, NULL, 0, "Show All History");
			lv_obj_t *l = lv_obj_get_child(lv_obj_get_user_data(r), 0);
			lv_obj_set_style_text_color(l, C(pal.tint), 0);
			ui_row_chevron(r);
			ui_row_tap(r, hist_all_click, NULL);
		}
	}
	if (!nfav && !nhist) {
		lv_obj_t *e = box(boxo);
		lv_obj_set_size(e, SCR_W - 2 * SIDE, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_top(e, 120, 0);
		lv_obj_set_style_pad_row(e, 18, 0);
		lv_obj_set_flex_flow(e, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(e, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_t *ic = box(e);
		lv_obj_set_size(ic, 148, 148);
		lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(ic, C(pal.cell), 0);
		lv_obj_set_style_margin_bottom(ic, 16, 0);
		lv_obj_center(ui_icon(ic, G_GLOBE, font(F_MONO, 72), pal.tint));
		text(e, "Start Browsing", font(F_TEXT_SEMI, 40), pal.label);
		lv_obj_t *b = text(e, "Type a website or a search above. Sites you add to Favorites from a page\xe2\x80\x99s "
				   "menu show up here.", font(F_TEXT, 28), pal.label2);
		lv_obj_set_width(b, 540);
		lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_line_space(b, 6, 0);
	}
	if (web_full_open) {	/* quiet, at the end: iOS Safari's "Edit" capsule spot */
		lv_obj_t *f = box(boxo);
		lv_obj_set_size(f, SCR_W - 2 * SIDE, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_top(f, nfav || nhist ? 56 : 64, 0);
		lv_obj_set_style_pad_row(f, 14, 0);
		lv_obj_set_flex_flow(f, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(f, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_t *b = box(f);
		lv_obj_set_size(b, LV_SIZE_CONTENT, 68);
		lv_obj_set_style_pad_hor(b, 34, 0);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(b, C(pal.cell), 0);
		lv_obj_set_style_bg_color(b, C(pal.cell_hi), LV_STATE_PRESSED);
		lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(b, 16, 0);
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(b, full_site_click, LV_EVENT_CLICKED, NULL);
		ui_icon(b, G_EXPAND, font(F_MONO, 28), pal.tint);
		text(b, "Open Full Site", font(F_TEXT_MEDIUM, 30), pal.tint);
		lv_obj_t *cap = text(f, "For pages that need JavaScript", font(F_TEXT, 24),
				     pal.label3);
		lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
	}
}

/* suggestions for typed text, or (empty) favorites + recent */
static void suggest_build(lv_obj_t *boxo, const char *q, bool root)
{
	lv_obj_clean(boxo);
	char t[256];
	snprintf(t, sizeof(t), "%s", q);
	while (*t && t[strlen(t) - 1] == ' ')
		t[strlen(t) - 1] = 0;
	if (*t) {
		lv_obj_t *sec = ui_section(boxo, NULL, NULL);
		char label[320], url[2048];
		bool search;
		input_url(t, url, sizeof(url), &search);
		if (search)
			snprintf(label, sizeof(label), "Search for \xe2\x80\x9c%s\xe2\x80\x9d", t);
		else
			snprintf(label, sizeof(label), "Go to %s", t);
		lv_obj_t *r = ui_row(sec, search ? G_SEARCH : G_GLOBE, pal.blue, label);
		ui_row_tap(r, search_click, NULL);
		int shown = 0;
		lv_obj_t *s2 = NULL;
		for (int i = 0; i < nfav && shown < 8; i++)
			if (strcasestr(favs[i].title, t) || strcasestr(favs[i].url, t)) {
				if (!s2)
					s2 = ui_section(boxo, "Favorites", NULL);
				site_row(s2, &favs[i], false);
				shown++;
			}
		s2 = NULL;
		for (int i = nhist - 1; i >= 0 && shown < 16; i--)
			if (strcasestr(hist[i].title, t) || strcasestr(hist[i].url, t)) {
				if (!s2)
					s2 = ui_section(boxo, "History", NULL);
				site_row(s2, &hist[i], false);
				shown++;
			}
		return;
	}
	if (root) {
		start_build(boxo);
		return;
	}
	if (clip[0]) {
		lv_obj_t *sec = ui_section(boxo, NULL, NULL);
		lv_obj_t *r = ui_row(sec, G_PASTE, pal.blue, "Paste and Go");
		ui_row_subtitle(r, clip);
		ui_row_tap(r, paste_click, NULL);
	}
	if (nfav) {
		lv_obj_t *sec = ui_section(boxo, "Favorites", NULL);
		for (int i = 0; i < nfav; i++)
			site_row(sec, &favs[i], false);
	}
	if (nhist) {
		lv_obj_t *sec = ui_section(boxo, "Recently visited", NULL);
		for (int i = nhist - 1, k = 0; i >= 0 && k < 12; i--, k++)
			site_row(sec, &hist[i], true);
	}
}

static bool addr_active(void);

static void root_build_now(void *ud)
{
	(void)ud;
	if (root_box)	/* the start page itself, or (the address field focused) suggestions for what is typed */
		suggest_build(root_box, addr_ta ? lv_textarea_get_text(addr_ta) : "", !addr_active());
}

/* deferred: callers may be inside an event of a row that the rebuild deletes */
static void root_build(void)
{
	lv_async_call_cancel(root_build_now, NULL);
	lv_async_call(root_build_now, NULL);
}

/* ---------- omnibox editing over a web page: the address selected whole, suggestions under it ---------- */

/* select-all (there is no text selection in Orchard's keyboard): the label shows it highlighted, the first key
 * typed replaces it, backspace clears it, a tap into it edits it (LVGL clears the highlight on an insert) */
static char sel_text[2048];

static void sel_drop(void)
{
	if (!sel_ta)
		return;
	lv_obj_t *l = lv_textarea_get_label(sel_ta);
	lv_label_set_text_selection_start(l, LV_LABEL_TEXT_SELECTION_OFF);
	lv_label_set_text_selection_end(l, LV_LABEL_TEXT_SELECTION_OFF);
	sel_ta = NULL;
}

static void sel_all(lv_obj_t *ta)
{
	const char *t = lv_textarea_get_text(ta);
	snprintf(sel_text, sizeof(sel_text), "%s", t);
	uint32_t n = 0;
	for (const char *p = t; *p; p++)
		n += ((unsigned char)*p & 0xc0) != 0x80;	/* characters, not bytes */
	lv_obj_t *l = lv_textarea_get_label(ta);
	lv_obj_set_style_bg_color(l, C(mix(pal.fill, pal.tint, 42)), LV_PART_SELECTED);
	lv_obj_set_style_text_color(l, C(pal.label), LV_PART_SELECTED);
	lv_label_set_text_selection_start(l, 0);
	lv_label_set_text_selection_end(l, n);
	sel_ta = ta;
}

/* VALUE_CHANGED while it was all selected: true = the text was replaced (its own VALUE_CHANGED already ran) */
static bool sel_typed(lv_obj_t *ta)
{
	if (sel_ta != ta)
		return false;
	const char *t = lv_textarea_get_text(ta);
	size_t n = strlen(sel_text), tl = strlen(t);
	char rest[256] = "";
	sel_drop();
	if (tl > n && !strncmp(t, sel_text, n))
		snprintf(rest, sizeof(rest), "%s", t + n);	/* typed at its end: that replaces it */
	else if (!(tl < n && !strncmp(t, sel_text, tl)))
		return false;	/* typed somewhere inside (a tap moved the caret): an edit */
	lv_textarea_set_text(ta, rest);
	kbd_set_context("");	/* the keyboard's word starts over (it adds the key it typed after this) */
	return true;
}

static void sheet_close_now(void *ud)
{
	(void)ud;
	sheet_drop();
}

static void sheet_close(lv_event_t *e)
{
	(void)e;
	lv_async_call(sheet_close_now, NULL);	/* Cancel is inside the sheet */
}

static void cur_copy_click(lv_event_t *e)
{
	(void)e;
	struct wpage *w = top_page();
	if (!w)
		return;
	snprintf(clip, sizeof(clip), "%s", w->url);
	root_build();
	ui_haptic_success();
	lv_async_call(sheet_close_now, NULL);
}

static void sheet_fill(void);

static void cur_edit_click(lv_event_t *e)	/* the address kept to edit, caret at its end */
{
	(void)e;
	if (!sheet_ta)
		return;
	sel_drop();
	lv_textarea_set_cursor_pos(sheet_ta, LV_TEXTAREA_CURSOR_LAST);
	sheet_fill();
}

/* the page shown, as the first suggestion (Chrome's zero-suggest): its tile, title + address, Copy, Edit */
static void cur_card(lv_obj_t *boxo, struct wpage *w)
{
	lv_obj_t *c = box(boxo);
	lv_obj_set_size(c, SCR_W - 2 * SIDE, LV_SIZE_CONTENT);
	lv_obj_set_style_margin_top(c, 24, 0);
	lv_obj_set_style_radius(c, 30, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(c, C(pal.cell), 0);
	lv_obj_set_style_pad_all(c, 20, 0);
	lv_obj_set_style_pad_column(c, 20, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
	struct entry en = { .when = 0 };
	snprintf(en.url, sizeof(en.url), "%s", w->url);
	snprintf(en.title, sizeof(en.title), "%s", w->title);
	site_tile(c, &en, WICON_SMALL);
	lv_obj_t *col = box(c);
	lv_obj_set_size(col, 1, LV_SIZE_CONTENT);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 4, 0);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *t = text(col, w->title[0] ? w->title : w->url, font(F_TEXT_MEDIUM, 30), pal.label);
	lv_obj_set_size(t, LV_PCT(100), lv_font_get_line_height(font(F_TEXT_MEDIUM, 30)));
	lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
	const char *u = strstr(w->url, "://");
	lv_obj_t *s = text(col, u ? u + 3 : w->url, font(F_TEXT, 24), pal.label2);
	lv_obj_set_size(s, LV_PCT(100), lv_font_get_line_height(font(F_TEXT, 24)));
	lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
	const char *g[] = { G_M_COPY, G_M_PENCIL };
	lv_event_cb_t cb[] = { cur_copy_click, cur_edit_click };
	for (int i = 0; i < 2; i++) {
		lv_obj_t *b = ui_icon_button(c, g[i], 68, pal.fill, pal.label);
		lv_obj_add_event_cb(b, cb[i], LV_EVENT_CLICKED, NULL);
	}
}

static void sheet_fill(void)
{
	if (!sheet_box)
		return;
	const char *t = lv_textarea_get_text(sheet_ta);
	bool zero = sel_ta == sheet_ta || !*t;
	suggest_build(sheet_box, zero ? "" : t, false);
	struct wpage *w = top_page();
	if (zero && w) {
		cur_card(sheet_box, w);
		lv_obj_move_to_index(lv_obj_get_child(sheet_box, -1), 0);
	}
	if (*t)
		lv_obj_remove_flag(sheet_x, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(sheet_x, LV_OBJ_FLAG_HIDDEN);
}

static void sheet_changed(lv_event_t *e)
{
	(void)e;
	if (sel_typed(sheet_ta))
		return;
	sheet_fill();
}

static void sheet_ready(lv_event_t *e)
{
	(void)e;
	sel_drop();
	search_click(NULL);
}

static void sheet_clear_click(lv_event_t *e)
{
	(void)e;
	sel_drop();
	lv_textarea_set_text(sheet_ta, "");
	kbd_set_context("");
}

/* the omnibox turns into a field in place: the whole address in it, selected; Cancel at its side; the page
 * gives way to suggestions (the current page first), keyboard up */
static void sheet_open(void)
{
	struct wpage *w = top_page();
	if (!w || sheet)
		return;
	nav_lock(true);
	sheet = box(w->page);
	lv_obj_set_size(sheet, SCR_W, SCR_H);
	lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(sheet, C(pal.bg), 0);
	lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *row = box(sheet);
	lv_obj_set_size(row, SCR_W - 48, PILL_H);
	lv_obj_set_pos(row, 24, PILL_Y);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 24, 0);
	sheet_ta = ui_search(row, SCR_W - 48, "Search or type URL");
	lv_obj_t *wrap = lv_obj_get_parent(sheet_ta);
	lv_obj_set_flex_grow(wrap, 1);
	lv_obj_set_style_text_font(sheet_ta, font(F_TEXT, 32), 0);
	sheet_x = ui_icon(wrap, G_CLEAR, font(F_MONO, 36), pal.label3);
	lv_obj_add_flag(sheet_x, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(sheet_x, 20);
	lv_obj_add_event_cb(sheet_x, sheet_clear_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *cancel = text(row, "Cancel", font(F_TEXT, 34), pal.tint);
	lv_obj_add_flag(cancel, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(cancel, 20);
	lv_obj_set_style_opa(cancel, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_add_event_cb(cancel, sheet_close, LV_EVENT_CLICKED, NULL);
	lv_obj_t *hair = box(sheet);
	lv_obj_set_size(hair, SCR_W, 1);
	lv_obj_set_pos(hair, 0, PAGE_Y - 1);
	lv_obj_set_style_bg_opa(hair, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(hair, C(pal.separator), 0);
	lv_obj_remove_flag(hair, LV_OBJ_FLAG_CLICKABLE);
	sheet_box = box(sheet);
	lv_obj_set_pos(sheet_box, 0, PAGE_Y);
	lv_obj_set_size(sheet_box, SCR_W, SCR_H - PAGE_Y);
	lv_obj_add_flag(sheet_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(sheet_box, LV_DIR_VER);
	lv_obj_set_flex_flow(sheet_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(sheet_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_bottom(sheet_box, kbd_height(KBD_TEXT) + 40, 0);
	char q[1024];
	if (w->search && ddg_query(w->url, q, sizeof(q)))	/* a results page: its search words */
		lv_textarea_set_text(sheet_ta, q);
	else
		lv_textarea_set_text(sheet_ta, w->url);
	sel_all(sheet_ta);
	sheet_fill();
	lv_obj_add_event_cb(sheet_ta, sheet_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(sheet_ta, sheet_ready, LV_EVENT_READY, NULL);
	kbd_attach(sheet_ta, KBD_TEXT);
	kbd_no_autocap();
	kbd_set_context("");	/* (the selection goes with the first key: no word in progress) */
	lv_obj_update_layout(sheet);
	lv_obj_scroll_to_x(sheet_ta, 0, LV_ANIM_OFF);	/* the address from its start, like Chrome */
}

/* Favorites / History as their own page (from a web page's menu) */
static void open_list(bool favorites)
{
	if (nwp >= MAX_WEB_PAGES)
		return;
	lv_obj_t *c = nav_push(favorites ? "Favorites" : "History");
	if (!c)
		return;
	wps[++nwp] = NULL;	/* not a web page: page_leave just pops it */
	nav_on_leave(page_leave);
	lv_obj_t *sec = ui_section(c, NULL, NULL);
	int n = favorites ? nfav : nhist;
	if (!n) {
		lv_obj_t *r = ui_row(sec, NULL, 0, favorites ? "No favorites yet" : "No history");
		(void)r;
	}
	for (int k = 0; k < n && k < 120; k++) {
		int i = favorites ? k : nhist - 1 - k;
		site_row(sec, favorites ? &favs[i] : &hist[i], !favorites);
	}
}

/* ---------- start page = the new tab page: our mark, the search box, Favorites, Recently visited ---------- */

static lv_obj_t *ntp_logo, *root_tb;

static bool addr_active(void) { return addr_ta && kbd_visible() && lv_obj_has_state(addr_ta, LV_STATE_FOCUSED); }

/* focused, the search box rises to the omnibox's place (the mark gives way) and Cancel shows beside it */
static void addr_chrome(void)
{
	if (!addr_ta)
		return;
	bool on = addr_active();
	const char *t = lv_textarea_get_text(addr_ta);
	if (on || *t)
		lv_obj_remove_flag(addr_cancel, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(addr_cancel, LV_OBJ_FLAG_HIDDEN);
	if (*t)
		lv_obj_remove_flag(addr_clear, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(addr_clear, LV_OBJ_FLAG_HIDDEN);
	bool up = on || *t;
	if (lv_obj_has_flag(ntp_logo, LV_OBJ_FLAG_HIDDEN) != up) {
		if (up) {
			lv_obj_add_flag(ntp_logo, LV_OBJ_FLAG_HIDDEN);
			lv_obj_add_flag(root_tb, LV_OBJ_FLAG_HIDDEN);
		} else {
			lv_obj_remove_flag(ntp_logo, LV_OBJ_FLAG_HIDDEN);
			lv_obj_remove_flag(root_tb, LV_OBJ_FLAG_HIDDEN);
		}
		lv_obj_set_height(lv_obj_get_parent(addr_ta), up ? PILL_H : 92);
		lv_obj_set_style_pad_top(root_col, up ? PILL_Y - STATUS_H : 0, 0);
		lv_obj_scroll_to_y(root_col, 0, LV_ANIM_OFF);
	}
	lv_obj_set_style_pad_bottom(root_col, on ? kbd_height(KBD_TEXT) + 40 : TB_H + 40, 0);
}

static void addr_changed(lv_event_t *e)
{
	(void)e;
	addr_chrome();
	root_build();
}

static void addr_focus(lv_event_t *e)
{
	(void)e;
	if (addr_active())
		kbd_no_autocap();
	addr_chrome();
	root_build();	/* focused: suggestions instead of the start page */
}

static void addr_ready(lv_event_t *e)
{
	(void)e;
	char t[1024];
	snprintf(t, sizeof(t), "%s", lv_textarea_get_text(addr_ta));
	kbd_hide();
	lv_textarea_set_text(addr_ta, "");
	go_text(t);
}

static void addr_clear_click(lv_event_t *e)
{
	(void)e;
	lv_textarea_set_text(addr_ta, "");
	kbd_attach(addr_ta, KBD_TEXT);
	kbd_no_autocap();
}

static void addr_cancel_click(lv_event_t *e)
{
	(void)e;
	lv_textarea_set_text(addr_ta, "");
	kbd_hide();
	addr_chrome();
	root_build();
}

/* "Search DuckDuckGo"-row taps on the start page read the field: keep it until the page is pushed */
static void root_leave(void)
{
	lv_async_call_cancel(root_build_now, NULL);
	root_col = root_box = addr_ta = addr_clear = addr_cancel = NULL;
	ntp_logo = root_tb = NULL;
	memset(&root_bar, 0, sizeof(root_bar));
}

static void ntp_build(lv_obj_t *root)
{
	root_col = nav_root(root, "Web");
	nav_on_leave(root_leave);
	lv_obj_t *page = lv_obj_get_parent(root_col);
	lv_obj_set_style_bg_color(page, C(pal.bg), 0);
	for (uint32_t k = 0; k < lv_obj_get_child_count(page); k++)	/* no nav bar */
		if (lv_obj_get_child(page, k) != root_col)
			lv_obj_add_flag(lv_obj_get_child(page, k), LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(lv_obj_get_child(root_col, 0), LV_OBJ_FLAG_HIDDEN);	/* nor its large title */
	lv_obj_set_pos(root_col, 0, STATUS_H);
	lv_obj_set_height(root_col, SCR_H - STATUS_H);
	ntp_logo = box(root_col);
	lv_obj_set_size(ntp_logo, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(ntp_logo, 96, 0);
	lv_obj_set_style_pad_bottom(ntp_logo, 56, 0);
	lv_obj_remove_flag(ntp_logo, LV_OBJ_FLAG_CLICKABLE);
	app_icon(ntp_logo, &app_web, 120);
	lv_obj_t *row = box(root_col);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(row, SCR_W - 48, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 24, 0);
	addr_ta = ui_search(row, SCR_W - 48, "Search or type URL");
	lv_obj_t *wrap = lv_obj_get_parent(addr_ta);
	lv_obj_set_flex_grow(wrap, 1);
	lv_obj_set_height(wrap, 92);
	lv_obj_set_style_pad_left(wrap, 30, 0);	/* the magnifier gets air on both sides */
	lv_obj_set_style_pad_column(wrap, 18, 0);
	lv_obj_set_style_text_color(lv_obj_get_child(wrap, 0), C(pal.label2), 0);
	addr_clear = ui_icon(wrap, G_CLEAR, font(F_MONO, 36), pal.label3);
	lv_obj_add_flag(addr_clear, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_ext_click_area(addr_clear, 20);
	lv_obj_add_event_cb(addr_clear, addr_clear_click, LV_EVENT_CLICKED, NULL);
	addr_cancel = text(row, "Cancel", font(F_TEXT, 34), pal.tint);
	lv_obj_add_flag(addr_cancel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_ext_click_area(addr_cancel, 20);
	lv_obj_set_style_opa(addr_cancel, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_add_event_cb(addr_cancel, addr_cancel_click, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(addr_ta, addr_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(addr_ta, addr_focus, EV_KBD_SHOWN, NULL);
	lv_obj_add_event_cb(addr_ta, addr_focus, EV_KBD_HIDDEN, NULL);
	lv_obj_add_event_cb(addr_ta, addr_ready, LV_EVENT_READY, NULL);
	root_box = box(root_col);
	lv_obj_remove_flag(root_box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(root_box, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(root_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(root_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	root_tb = toolbar(page, &root_bar);
	lv_obj_add_flag(root_tb, LV_OBJ_FLAG_IGNORE_LAYOUT);
}

/* ---------- tabs ----------
 * One tab is live: the nav stack (start page + its web pages). The others are kept as their addresses (oldest
 * first, each with its LAN / tailnet pass), Forward list, title and a snapshot — no page objects, no pictures,
 * so a tab costs ~270 KB (the snapshot) however big its pages were. Switching cuts the live pages away and
 * pushes the tab's again without a slide: the one on top loads, the older ones load when back reaches them
 * (lazy). Tabs outlive closing the app (statics) and Orchard (data_dir()/web/tabs.txt, no snapshots). */

struct tab {
	char title[160];
	char *u[MAX_WEB_PAGES];	/* its pages, oldest first, the last one shown; none = the start page */
	bool pv[MAX_WEB_PAGES];	/* each one's LAN / tailnet pass */
	int nu;
	char *f[16];		/* Forward (fwd[]'s order: the next one last) */
	int nf;
	lv_image_dsc_t *thumb;	/* THUMB_W × THUMB_H RGB565, NULL = none yet */
	unsigned id;		/* identity for the grid's queued taps (0 = none yet, given by tab_id) */
};
static struct tab tabs[MAX_TABS];

/* a tab's identity: survives the memmove of a close, unlike its index (a zeroed slot gets a new one) */
static unsigned tab_id(int i)
{
	static unsigned next;
	if (!tabs[i].id)
		tabs[i].id = ++next;
	return tabs[i].id;
}

static int tab_by_id(unsigned id)	/* → its index, -1 = gone */
{
	for (int i = 0; id && i < ntabs; i++)
		if (tabs[i].id == id)
			return i;
	return -1;
}
static bool tabs_loaded;

static void tab_lists_free(struct tab *t)
{
	for (int k = 0; k < t->nu; k++)
		free(t->u[k]);
	for (int k = 0; k < t->nf; k++)
		free(t->f[k]);
	t->nu = t->nf = 0;
}

static void tab_free(struct tab *t)
{
	tab_lists_free(t);
	if (t->thumb) {
		lv_image_cache_drop(t->thumb);
		lv_image_header_cache_drop(t->thumb);
		free((void *)t->thumb->data);
		free(t->thumb);
	}
	memset(t, 0, sizeof(*t));
}

static void tab_push(struct tab *t, const char *url, bool priv)
{
	if (t->nu < MAX_WEB_PAGES && (t->u[t->nu] = strdup(url))) {
		t->pv[t->nu] = priv;
		t->nu++;
	}
}

/* the live pages → t */
static void tab_capture(struct tab *t)
{
	tab_lists_free(t);
	t->title[0] = 0;
	for (int i = 1; i <= nwp; i++) {
		struct wpage *w = wps[i];
		if (!w || !w->url[0])
			continue;
		tab_push(t, w->url, w->allow_priv);
		if (w->title[0])
			snprintf(t->title, sizeof(t->title), "%s", w->title);
		else
			net_host(w->url, t->title, sizeof(t->title));
	}
	clean_field(t->title);
	for (int k = 0; k < nfwd && t->nf < 16; k++)
		if ((t->f[t->nf] = strdup(fwd[k])))
			t->nf++;
}

static void tabs_save(void)
{
	char p[400], tmp[420];
	store_path(p, sizeof(p), "tabs.txt");
	snprintf(tmp, sizeof(tmp), "%s.tmp", p);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fprintf(f, "c\t%d\n", cur_tab);
	for (int i = 0; i < ntabs; i++) {
		fprintf(f, "t\t%s\n", tabs[i].title);
		for (int k = 0; k < tabs[i].nu; k++)
			fprintf(f, "u\t%d\t%s\n", tabs[i].pv[k], tabs[i].u[k]);
		for (int k = 0; k < tabs[i].nf; k++)
			fprintf(f, "f\t%s\n", tabs[i].f[k]);
	}
	bool bad = ferror(f);
	if (fclose(f) || bad) {	/* (full /data): the old file stays */
		remove(tmp);
		return;
	}
	rename(tmp, p);
}

static void tabs_load(void)
{
	if (tabs_loaded)
		return;
	tabs_loaded = true;
	char p[400], line[2200];
	store_path(p, sizeof(p), "tabs.txt");
	FILE *f = fopen(p, "r");
	int cur = 0;
	struct tab *t = NULL;
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] == 'c' && line[1] == '\t')
			cur = atoi(line + 2);
		else if (line[0] == 't' && line[1] == '\t' && ntabs < MAX_TABS) {
			t = &tabs[ntabs++];
			snprintf(t->title, sizeof(t->title), "%s", line + 2);
		} else if (t && line[0] == 'u' && line[1] == '\t' && line[2] && line[3] == '\t' && line[4])
			tab_push(t, line + 4, line[2] == '1');
		else if (t && line[0] == 'f' && line[1] == '\t' && line[2] && t->nf < 16 && (t->f[t->nf] = strdup(line + 2)))
			t->nf++;
	}
	if (f)
		fclose(f);
	if (!ntabs)
		ntabs = 1;	/* tabs[0] zeroed: a new tab page */
	cur_tab = cur >= 0 && cur < ntabs ? cur : 0;
}

/* the screen shows the live tab: a THUMB_W × THUMB_H copy of its page (under the omnibox) for the grid, straight
 * from the framebuffer (direct rendering: it holds the last frame), each pixel the mean of the ~2.3 × 2.3 it covers */
static void tab_thumb(void)
{
	lv_draw_buf_t *fb = lv_display_get_buf_active(NULL);
	if (!fb || fb->header.w != SCR_W || fb->header.h != SCR_H || lv_color_format_get_size(fb->header.cf) != 4)
		return;
	struct tab *t = &tabs[cur_tab];
	if (!t->thumb) {
		lv_image_dsc_t *d = calloc(1, sizeof(*d));
		void *px = malloc(THUMB_W * THUMB_H * 2);
		if (!d || !px) {
			free(d);
			free(px);
			return;
		}
		d->header.magic = LV_IMAGE_HEADER_MAGIC;
		d->header.cf = LV_COLOR_FORMAT_RGB565;
		d->header.w = THUMB_W;
		d->header.h = THUMB_H;
		d->header.stride = THUMB_W * 2;
		d->data_size = THUMB_W * THUMB_H * 2;
		d->data = px;
		t->thumb = d;
	}
	int y0 = nav_depth() > 0 ? PAGE_Y : STATUS_H, sh = THUMB_H * SCR_W / THUMB_W;
	if (y0 + sh > SCR_H)
		sh = SCR_H - y0;
	static int16_t xs[THUMB_W + 1];
	for (int x = 0; x <= THUMB_W; x++)
		xs[x] = x * SCR_W / THUMB_W;
	uint16_t *out = (uint16_t *)t->thumb->data;
	for (int y = 0; y < THUMB_H; y++) {
		int ya = y0 + y * sh / THUMB_H, yb = y0 + (y + 1) * sh / THUMB_H;
		if (yb <= ya)
			yb = ya + 1;
		for (int x = 0; x < THUMB_W; x++) {
			uint32_t r = 0, g = 0, b = 0, n = 0;
			for (int sy = ya; sy < yb; sy++) {
				const uint32_t *s = (const uint32_t *)(fb->data + sy * fb->header.stride) + xs[x];
				for (int sx = xs[x]; sx < xs[x + 1]; sx++, s++, n++) {
					r += *s >> 16 & 0xff;
					g += *s >> 8 & 0xff;
					b += *s & 0xff;
				}
			}
			r /= n, g /= n, b /= n;
			*out++ = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
		}
	}
	lv_image_cache_drop(t->thumb);
}

static void bars_sync(void);

/* tab t's pages, pushed without a slide over the start page (only the top one loads) */
static void tab_restore(struct tab *t)
{
	nfwd = 0;
	for (int k = 0; k < t->nf && k < 16; k++)
		snprintf(fwd[nfwd++], sizeof(fwd[0]), "%s", t->f[k]);
	for (int k = 0; k < t->nu; k++) {
		struct wpage *w = page_new(t->u[k]);
		if (!w)
			break;
		w->allow_priv = t->pv[k];
		if (k < t->nu - 1) {
			w->lazy = true;
			set_host(w);
		} else
			page_load(w, w->url);
	}
	nav_land();
	bars_sync();
}

static void msheet_drop(void);

/* the live pages become tab i's */
static void tab_activate(int i)
{
	sheet_drop();
	msheet_drop();
	if (kbd_visible())
		kbd_hide();
	cutting = true;
	nav_cut(0);
	cutting = false;
	nfwd = 0;
	cur_tab = i;
	if (root_col)
		lv_obj_scroll_to_y(root_col, 0, LV_ANIM_OFF);
	tab_restore(&tabs[i]);
	tabs_save();
}

static void tab_switch(int i)
{
	if (i < 0 || i >= ntabs || i == cur_tab)
		return;
	tab_capture(&tabs[cur_tab]);
	tab_activate(i);
}

static bool tabs_full(void)
{
	if (ntabs < MAX_TABS)
		return false;
	ui_alert("Too Many Tabs", "Close a tab to open another one.", (const char *[]){ "OK" }, 1, NULL, NULL);
	return true;
}

static lv_obj_t *grid;

static void tab_new(void)
{
	if (tabs_full())
		return;
	tab_capture(&tabs[cur_tab]);
	memset(&tabs[ntabs], 0, sizeof(tabs[0]));
	ntabs++;
	tab_activate(ntabs - 1);
}

/* Open in New Tab: in the background, loaded when it is picked */
static void tab_add_bg(const char *url, bool priv)
{
	if (tabs_full())
		return;
	struct tab *t = &tabs[ntabs];
	memset(t, 0, sizeof(*t));
	tab_push(t, url, priv);
	net_host(url, t->title, sizeof(t->title));
	ntabs++;
	bars_sync();
	ui_haptic_success();
	tab_dirty();
}

static void tab_close(int i)
{
	if (i < 0 || i >= ntabs)
		return;
	tab_free(&tabs[i]);
	memmove(tabs + i, tabs + i + 1, (ntabs - i - 1) * sizeof(tabs[0]));
	memset(&tabs[--ntabs], 0, sizeof(tabs[0]));
	if (!ntabs)
		ntabs = 1;	/* the last one went: a new tab page */
	if (i < cur_tab)
		cur_tab--;
	else if (i == cur_tab) {
		tab_activate(i > 0 ? i - 1 : 0);	/* the one before it (or after, for the first) */
		return;
	}
	bars_sync();
	tabs_save();
}

static void tab_sync_now(void *ud)
{
	(void)ud;
	if (!root_col)	/* the app closed meanwhile (its pages left one by one: not a change of the tab) */
		return;
	tab_capture(&tabs[cur_tab]);
	tabs_save();
}

/* the live tab changed (a page opened / loaded / went back): its record follows, once things settle */
static void tab_dirty(void)
{
	lv_async_call_cancel(tab_sync_now, NULL);
	lv_async_call(tab_sync_now, NULL);
}

/* ---------- tab grid: two columns of cards (title, close, snapshot), the live one outlined ---------- */

#define CARD_W (THUMB_W + 12)
#define CARD_HEAD 64

static lv_obj_t *grid_list;

static void grid_close(void)
{
	if (!grid)
		return;
	lv_obj_delete(grid);
	grid = grid_list = NULL;
	nav_lock(false);
}

/* the cards carry their tab's id, not its index: a × queued before the grid refilled still means its own tab */
static void grid_pick_now(void *ud)
{
	int i = tab_by_id((unsigned)(uintptr_t)ud);
	if (!app_rt || i < 0)	/* the app closed first (app_close can't cancel a call by its id) / the tab went */
		return;
	grid_close();
	tab_switch(i);
}

static void grid_fill(void);

static void card_x_now(void *ud)
{
	if (!app_rt)
		return;
	if (ud)	/* (0 = only a refill) */
		tab_close(tab_by_id((unsigned)(uintptr_t)ud));
	if (grid)
		grid_fill();
}

static void card_click(lv_event_t *e) { lv_async_call(grid_pick_now, lv_event_get_user_data(e)); }
static void card_x_click(lv_event_t *e) { lv_async_call(card_x_now, lv_event_get_user_data(e)); }

static void grid_done_now(void *ud)
{
	(void)ud;
	grid_close();
}

static void grid_done_click(lv_event_t *e)
{
	(void)e;
	lv_async_call(grid_done_now, NULL);
}

static void grid_new_now(void *ud)
{
	(void)ud;
	if (ntabs >= MAX_TABS) {
		tabs_full();
		return;
	}
	grid_close();
	tab_new();
}

static void grid_new_click(lv_event_t *e)
{
	(void)e;
	lv_async_call(grid_new_now, NULL);
}

static void close_all_ok(int idx, void *ud)
{
	(void)ud;
	if (idx != 1)
		return;
	for (int i = 0; i < ntabs; i++)
		tab_free(&tabs[i]);
	ntabs = 1;
	grid_close();
	tab_activate(0);
	ui_haptic_success();
}

static void close_all_click(lv_event_t *e)
{
	(void)e;
	char t[48];
	snprintf(t, sizeof(t), ntabs == 1 ? "Close This Tab?" : "Close All %d Tabs?", ntabs);
	ui_alert(t, NULL, (const char *[]){ "Cancel", "!Close All" }, 2, close_all_ok, NULL);
}

static void grid_hist_now(void *ud)
{
	(void)ud;
	grid_close();
	open_list(false);
}

static void grid_hist_click(lv_event_t *e)
{
	(void)e;
	lv_async_call(grid_hist_now, NULL);
}

/* a site's small tile for a card's header: its icon (scaled from the list size) or its monogram */
static lv_obj_t *tab_icon(lv_obj_t *parent, const char *url, const char *title, int size)
{
	struct entry en = { .when = 0 };
	snprintf(en.url, sizeof(en.url), "%s", url);
	snprintf(en.title, sizeof(en.title), "%s", title);
	const lv_image_dsc_t *ic = wicon_tile(url, WICON_SMALL);
	if (!ic)
		return mono_tile(parent, &en, size);
	lv_obj_t *im = lv_image_create(parent);
	lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
	lv_image_set_src(im, ic);
	lv_image_set_scale(im, 256 * size / WICON_SMALL);
	lv_image_set_antialias(im, true);
	lv_obj_set_size(im, size, size);
	lv_image_set_inner_align(im, LV_IMAGE_ALIGN_CENTER);
	return im;
}

static void grid_fill(void)
{
	lv_obj_clean(grid_list);
	uint32_t card_bg = pal.cell;
	for (int i = 0; i < ntabs; i++) {
		struct tab *t = &tabs[i];
		const char *url = t->nu ? t->u[t->nu - 1] : "";
		lv_obj_t *card = box(grid_list);
		lv_obj_set_size(card, CARD_W, CARD_HEAD + THUMB_H + 6);
		lv_obj_set_style_radius(card, 26, 0);
		lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(card, C(card_bg), 0);
		lv_obj_set_style_bg_color(card, C(pal.cell_hi), LV_STATE_PRESSED);
		if (i == cur_tab) {
			lv_obj_set_style_outline_width(card, 5, 0);
			lv_obj_set_style_outline_pad(card, 4, 0);
			lv_obj_set_style_outline_color(card, C(pal.tint), 0);
		}
		lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(card, card_click, LV_EVENT_CLICKED, (void *)(uintptr_t)tab_id(i));
		lv_obj_t *head = box(card);
		lv_obj_set_size(head, CARD_W, CARD_HEAD);
		lv_obj_set_style_pad_left(head, 16, 0);
		lv_obj_set_style_pad_right(head, 8, 0);
		lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(head, 12, 0);
		lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
		if (t->nu)
			tab_icon(head, url, t->title, 34);
		lv_obj_t *l = text(head, t->nu ? (t->title[0] ? t->title : url) : "New Tab", font(F_TEXT_MEDIUM, 24), pal.label);
		lv_obj_set_size(l, 1, lv_font_get_line_height(font(F_TEXT_MEDIUM, 24)));
		lv_obj_set_flex_grow(l, 1);
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
		lv_obj_t *x = box(head);
		lv_obj_set_size(x, 52, 52);
		lv_obj_add_flag(x, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(x, 12);
		lv_obj_set_style_opa(x, LV_OPA_50, LV_STATE_PRESSED);
		lv_obj_add_event_cb(x, card_x_click, LV_EVENT_CLICKED, (void *)(uintptr_t)tab_id(i));
		lv_obj_center(ui_icon(x, G_M_TAB_X, font(F_MONO, 32), pal.label2));
		lv_obj_t *body;
		if (t->thumb) {
			body = lv_image_create(card);
			lv_image_set_src(body, t->thumb);
		} else {	/* not seen yet (a background tab, or after a restart): its tile on a blank page */
			body = box(card);
			lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
			lv_obj_set_style_bg_color(body, C(pal.bg), 0);
			if (t->nu) {
				struct entry en = { .when = 0 };
				snprintf(en.url, sizeof(en.url), "%s", url);
				snprintf(en.title, sizeof(en.title), "%s", t->title);
				lv_obj_center(site_tile(body, &en, WICON_BIG));
			} else
				lv_obj_center(app_icon(body, &app_web, 96));
		}
		lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(body, THUMB_W, THUMB_H);
		lv_obj_set_pos(body, 6, CARD_HEAD);
		lv_obj_set_style_radius(body, 20, 0);
	}
}

/* the tab switcher, over the pages (no zoom: one redraw); its header counts the tabs */
static void grid_open(void)
{
	if (grid)
		return;
	sheet_drop();
	msheet_drop();
	tab_thumb();
	tab_capture(&tabs[cur_tab]);
	grid = box(app_rt);
	nav_lock(true);	/* an edge swipe would take the page under it back */
	lv_obj_set_size(grid, SCR_W, SCR_H);
	lv_obj_set_style_bg_opa(grid, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(grid, C(pal.grouped_bg), 0);
	lv_obj_add_flag(grid, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(grid, LV_OBJ_FLAG_GESTURE_BUBBLE);
	/* header: a capsule with two segments — the open tabs (count square, selected) and History */
	lv_obj_t *seg = box(grid);
	lv_obj_set_size(seg, 260, 72);
	lv_obj_align(seg, LV_ALIGN_TOP_MID, 0, STATUS_H + 14);
	lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(seg, C(pal.fill), 0);
	lv_obj_set_style_pad_all(seg, 6, 0);
	lv_obj_t *on = box(seg);
	lv_obj_set_size(on, 124, 60);
	lv_obj_set_style_radius(on, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(on, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(on, C(th.dark ? mix(pal.fill, th.fg, 18) : 0xffffff), 0);
	lv_obj_t *sq = box(on);
	lv_obj_set_size(sq, 40, 40);
	lv_obj_center(sq);
	lv_obj_set_style_radius(sq, 10, 0);
	lv_obj_set_style_border_width(sq, 4, 0);
	lv_obj_set_style_border_color(sq, C(pal.label), 0);
	lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(on, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(sq, LV_OBJ_FLAG_CLICKABLE);
	char n[8];
	snprintf(n, sizeof(n), "%d", ntabs);
	lv_obj_center(text(sq, n, font(F_TEXT_BOLD, 22), pal.label));
	lv_obj_t *hb = box(seg);
	lv_obj_set_size(hb, 124, 60);
	lv_obj_align(hb, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_flag(hb, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_opa(hb, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_add_event_cb(hb, grid_hist_click, LV_EVENT_CLICKED, NULL);
	lv_obj_center(ui_icon(hb, G_M_HISTORY, font(F_MONO, 40), pal.label2));
	grid_list = box(grid);
	lv_obj_set_pos(grid_list, 0, PAGE_Y);
	lv_obj_set_size(grid_list, SCR_W, SCR_H - PAGE_Y - TB_H);
	lv_obj_add_flag(grid_list, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(grid_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(grid_list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(grid_list, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_all(grid_list, GRID_GAP - 6, 0);
	lv_obj_set_style_pad_top(grid_list, 18, 0);
	lv_obj_set_style_pad_bottom(grid_list, 40, 0);
	lv_obj_set_style_pad_column(grid_list, GRID_GAP - 12, 0);
	lv_obj_set_style_pad_row(grid_list, GRID_GAP, 0);
	grid_fill();
	/* bottom: Close All · + · Done */
	lv_obj_t *tb = box(grid);
	lv_obj_set_size(tb, SCR_W, TB_H);
	lv_obj_align(tb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(tb, C(pal.grouped_bg), 0);
	lv_obj_set_style_pad_bottom(tb, BOTTOM_PAD, 0);
	lv_obj_set_style_pad_hor(tb, SIDE, 0);
	lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *ca = text(tb, "Close All", font(F_TEXT, 32), pal.tint);
	lv_obj_align(ca, LV_ALIGN_LEFT_MID, 0, -BOTTOM_PAD / 2);
	lv_obj_add_flag(ca, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(ca, 24);
	lv_obj_set_style_opa(ca, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_add_event_cb(ca, close_all_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *plus = ui_icon_button(tb, G_M_PLUS, 76, pal.tint, ui_on_color(pal.tint));
	lv_obj_set_style_text_font(lv_obj_get_child(plus, 0), font(F_MONO, 48), 0);
	ui_glyph_fit(lv_obj_get_child(plus, 0));
	lv_obj_align(plus, LV_ALIGN_CENTER, 0, -BOTTOM_PAD / 2);
	lv_obj_add_event_cb(plus, grid_new_click, LV_EVENT_CLICKED, NULL);
	lv_obj_t *done = text(tb, "Done", font(F_TEXT_SEMI, 32), pal.tint);
	lv_obj_align(done, LV_ALIGN_RIGHT_MID, 0, -BOTTOM_PAD / 2);
	lv_obj_add_flag(done, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(done, 24);
	lv_obj_set_style_opa(done, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_add_event_cb(done, grid_done_click, LV_EVENT_CLICKED, NULL);
	/* the live tab's card in view */
	lv_obj_update_layout(grid_list);
	lv_obj_t *c = lv_obj_get_child(grid_list, cur_tab);
	if (c)
		lv_obj_scroll_to_view(c, LV_ANIM_OFF);
}

/* ---------- ⋯ menu: a sheet from the bottom (Chrome's overflow menu): destinations, then actions ---------- */

static lv_obj_t *msheet, *mscrim;

static void msheet_y(void *o, int32_t y) { slide_move(o, 0, y); }
static void msheet_landed(lv_anim_t *a) { slide_track(a->var, false); }

static void msheet_drop(void)
{
	if (mscrim)
		lv_obj_delete(mscrim);
	if (msheet) {
		lv_obj_delete(msheet);	/* (its slide goes with it) */
		nav_lock(false);
	}
	msheet = mscrim = NULL;
}

static void msheet_pick_now(void *ud)
{
	if (!app_rt)
		return;
	msheet_drop();
	int id = (int)(intptr_t)ud;
	if (id)
		menu_pick(id);
}

static void msheet_click(lv_event_t *e)
{
	lv_async_call(msheet_pick_now, lv_event_get_user_data(e));
}

static uint32_t msheet_card_bg(void) { return th.dark ? mix(pal.cell, th.fg, 5) : pal.cell; }

static void msheet_dest(lv_obj_t *row, int id, const char *glyph, const char *label)
{
	lv_obj_t *c = box(row);
	lv_obj_set_size(c, 150, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(c, 12, 0);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_opa(c, LV_OPA_60, LV_STATE_PRESSED);
	lv_obj_add_event_cb(c, msheet_click, LV_EVENT_CLICKED, (void *)(intptr_t)id);
	lv_obj_t *t = box(c);
	lv_obj_set_size(t, 108, 108);
	lv_obj_set_style_radius(t, 30, 0);
	lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(t, C(msheet_card_bg()), 0);
	lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_center(ui_icon(t, glyph, ui_icon_font(glyph, 46), pal.tint));
	lv_obj_t *l = text(c, label, font(F_TEXT, 24), pal.label);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static lv_obj_t *msheet_card(lv_obj_t *s)
{
	lv_obj_t *c = box(s);
	lv_obj_set_size(c, SCR_W - 2 * SIDE, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(c, 30, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(c, C(msheet_card_bg()), 0);
	lv_obj_set_style_clip_corner(c, true, 0);	/* small: a pressed first / last row keeps the corners */
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	return c;
}

static void msheet_row(lv_obj_t *card, int id, const char *glyph, const char *label)
{
	lv_obj_t *r = box(card);
	lv_obj_set_size(r, LV_PCT(100), 88);
	lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(r, C(mix(msheet_card_bg(), th.fg, 12)), LV_STATE_PRESSED);
	if (lv_obj_get_index(r) > 0) {
		lv_obj_t *sep = box(r);
		lv_obj_set_size(sep, SCR_W - 2 * SIDE - 30, 1);
		lv_obj_set_pos(sep, 30, 0);
		lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(sep, C(pal.separator), 0);
		lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);
	}
	lv_obj_add_event_cb(r, msheet_click, LV_EVENT_CLICKED, (void *)(intptr_t)id);
	lv_obj_t *l = text(r, label, font(F_TEXT, 32), pal.label);
	lv_obj_align(l, LV_ALIGN_LEFT_MID, 30, 0);
	lv_obj_t *g = ui_icon(r, glyph, ui_icon_font(glyph, 34), pal.label);
	lv_obj_align(g, LV_ALIGN_RIGHT_MID, -30, 0);
}

static void scrim_pressed(lv_event_t *e)
{
	(void)e;
	lv_async_call(msheet_pick_now, NULL);
}

static void msheet_open(void)
{
	if (msheet || grid || !app_rt)
		return;
	sheet_drop();
	tab_thumb();	/* (New Tab from here: the grid shows this page as it was) */
	struct wpage *w = top_page();
	mscrim = box(app_rt);
	lv_obj_set_size(mscrim, SCR_W, SCR_H);
	lv_obj_set_style_bg_opa(mscrim, LV_OPA_40, 0);
	lv_obj_set_style_bg_color(mscrim, C(0x000000), 0);
	lv_obj_add_flag(mscrim, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(mscrim, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(mscrim, scrim_pressed, LV_EVENT_PRESSED, NULL);

	/* built below the screen without invalidating anything: the slide draws it strip by strip */
	lv_display_enable_invalidation(NULL, false);
	lv_obj_t *s = msheet = box(app_rt);
	nav_lock(true);
	lv_obj_set_width(s, SCR_W);
	lv_obj_set_style_radius(s, 44, 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(s, C(th.dark ? mix(pal.bg, pal.cell, 60) : pal.grouped_bg), 0);
	lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(s, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(s, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(s, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(s, 16, 0);
	lv_obj_set_style_pad_row(s, 22, 0);
	lv_obj_set_style_pad_bottom(s, 44 + BOTTOM_PAD + 12, 0);	/* the bottom corners stay below the screen */
	lv_obj_t *grab = box(s);
	lv_obj_set_size(grab, 72, 10);
	lv_obj_set_style_radius(grab, 5, 0);
	lv_obj_set_style_bg_opa(grab, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(grab, C(pal.label3), 0);
	lv_obj_t *dest = box(s);
	lv_obj_set_size(dest, SCR_W - 2 * SIDE, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(dest, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(dest, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_remove_flag(dest, LV_OBJ_FLAG_CLICKABLE);
	msheet_dest(dest, M_FAVS, G_M_STAR, "Favorites");
	msheet_dest(dest, M_HIST, G_M_HISTORY, "History");
	if (web_full_open)
		msheet_dest(dest, M_FULL, G_M_FULL, "Full Site");
	msheet_dest(dest, M_CLEAR, G_M_COOKIE, "Clear Data");
	lv_obj_t *c = msheet_card(s);
	msheet_row(c, M_NEWTAB, G_M_NEWTAB, "New Tab");
	msheet_row(c, M_CLOSETAB, G_M_CLOSE, "Close Tab");
	if (w) {
		bool fav = fav_find(w->url) >= 0, dk = desk_on(w->url);
		char tl[40];
		snprintf(tl, sizeof(tl), "Text Size (%d%%)", tsize());
		c = msheet_card(s);
		msheet_row(c, M_RELOAD, w->loading ? G_M_CLOSE : G_M_RELOAD, w->loading ? "Stop" : "Reload");
		msheet_row(c, M_FAV, fav ? G_M_STAR : G_M_STAR_O, fav ? "Remove from Favorites" : "Add to Favorites");
		msheet_row(c, M_COPY, G_M_COPY, "Copy Link");
		if (w->doc)
			msheet_row(c, M_FIND, G_M_FIND, "Find in Page");
		msheet_row(c, M_TEXT, G_M_TEXT, tl);
		msheet_row(c, M_DESK, dk ? G_M_MOBILE : G_M_DESKTOP, dk ? "Request Mobile Site" : "Request Desktop Site");
		if (web_full_open)
			msheet_row(c, M_FULL, G_M_FULL, "Open in Full Site");
	}
	lv_obj_set_height(s, LV_SIZE_CONTENT);
	lv_obj_update_layout(s);
	int h = lv_obj_get_height(s), cap = SCR_H - 240;
	if (h > cap) {
		lv_obj_set_height(s, cap);
		h = cap;
	}
	lv_obj_set_pos(s, 0, SCR_H);
	lv_obj_refr_pos(s);
	lv_display_enable_invalidation(NULL, true);
	slide_track(s, true);
	motion_anim(s, msheet_y, SCR_H, SCR_H - h + 44, MOTION_PAGE, msheet_landed);
}

/* ---------- timer: lazy blocks + pictures ---------- */

static void tick(lv_timer_t *t)
{
	(void)t;
	struct wpage *w = top_page();
	if (w && w->prog_on && !prog_timer)	/* a page still loading came back on top */
		prog_timer = lv_timer_create(prog_tick, 100, NULL);
	if (w && w->lazy && !lv_anim_count_running()) {	/* (page_leave asked already; a safety net) */
		lazy_now(NULL);
		return;
	}
	if (!w || !w->doc || !w->col)
		return;
	lv_obj_t *c = w->col;
	int sy = lv_obj_get_scroll_y(c);
	if (w->refresh_at && (long long)lv_tick_get() >= w->refresh_at && !w->loading) {	/* <meta refresh> */
		char u[2048];
		snprintf(u, sizeof(u), "%s", w->doc->refresh);
		w->allow_priv = w->priv;	/* from this page: a public one can't send the phone to the LAN / tailnet */
		snprintf(w->referer, sizeof(w->referer), "%s", w->url);	/* (not go_*: a navigate() may be pending) */
		free(w->post);
		w->post = NULL;
		w->refreshes++;
		page_load(w, u);
		return;
	}
	if (w->tsz && w->tsz != tsize() && !w->loading) {	/* text size changed while it was under another page */
		page_rebuild(w);
		return;
	}
	if (anchor_step(w))
		return;
	if (w->built < w->doc->nb && lv_obj_get_scroll_bottom(c) < 2 * SCR_H) {
		build_more(w, 0);
		return;	/* layout settles first */
	}
	/* pictures near the screen */
	for (int k = 0; k < w->doc->nimgs && img_active < IMG_PARALLEL; k++) {
		struct wimg *im = &w->imgs[k];
		if (im->state != IM_IDLE || !im->box)
			continue;
		lv_area_t a;
		lv_obj_get_coords(im->box, &a);
		lv_area_t ca;
		lv_obj_get_coords(c, &ca);
		int top = a.y1 - ca.y1, bot = a.y2 - ca.y1;
		(void)sy;
		if (bot > -SCR_H && top < 2 * SCR_H)
			img_start(w, k);
	}
}

/* the lock screen over the page: no lazy blocks, pictures or <meta refresh> meanwhile (4 wake-ups a second
 * for nothing seen); unlock = they go on at once */
static void lock_changed(bool locked)
{
	if (prog_timer && locked)
		lv_timer_pause(prog_timer);
	else if (prog_timer)
		lv_timer_resume(prog_timer);
	if (ticker && locked) {
		lv_timer_pause(ticker);
	} else if (ticker) {
		lv_timer_resume(ticker);
		lv_timer_ready(ticker);
	}
}

/* ---------- app ---------- */

/* site icons arrived: the start page and the tab grid show them */
static void icons_arrived(void)
{
	root_build();
	if (grid)
		lv_async_call(card_x_now, NULL);	/* (closes nothing: a refill) */
}

static void app_open(lv_obj_t *root)
{
	app_rt = root;
	store_load();
	wicon_trim();	/* nothing shows a tile now */
	wicon_listen(icons_arrived);
	char cd[400];
	store_path(cd, sizeof(cd), "cookies");
	net_set_cookie_dir(cd);
	nwp = 0;
	nfwd = 0;
	tabs_load();
	ntp_build(root);
	root_build_now(NULL);
	addr_chrome();
	ticker = lv_timer_create(tick, TICK_MS, NULL);
	lock_listen(lock_changed, true);
	const char *start = getenv("ORCHARD_WEB_URL");	/* previews: open straight at a page */
	if (start && *start)
		go_text(start);
	else
		tab_restore(&tabs[cur_tab]);	/* where it was: the tab's page loads again */
	bars_sync();
}

static void app_close(void)
{
	lv_async_call_cancel(navigate_now, NULL);
	lv_async_call_cancel(sheet_close_now, NULL);
	net_cancel(os_req);
	os_req = NULL;
	if (ticker)
		lv_timer_delete(ticker);
	ticker = NULL;
	if (prog_timer)
		lv_timer_delete(prog_timer);
	prog_timer = NULL;
	for (int i = nwp; i >= 1; i--)	/* nav_reset already left them; anything still here goes */
		if (wps[i]) {
			wps[i]->col = NULL;
			page_clear(wps[i]);
			free(wps[i]->post);
			free(wps[i]);
			wps[i] = NULL;
		}
	nwp = 0;
	nfwd = 0;
	sheet = sheet_ta = sheet_box = sheet_x = NULL;
	pressed_span = NULL;
	pressed_sg = NULL;
	free(go_post);
	go_post = NULL;
	find_page = NULL;
	find_bar = find_ta = find_count = NULL;
	if (tbar_timer)
		lv_timer_delete(tbar_timer);
	tbar_timer = NULL;
	tbar_page = NULL;
	tbar = tbar_pct = NULL;
	sel_lab = NULL;
	lv_async_call_cancel(retry_now, NULL);
	lv_async_call_cancel(open_anyway_now, NULL);
	lv_async_call_cancel(find_close_now, NULL);
	lv_async_call_cancel(tab_sync_now, NULL);	/* (the pages left one by one: not a change) */
	lv_async_call_cancel(lazy_now, NULL);
	lv_async_call_cancel(grid_pick_now, NULL);
	lv_async_call_cancel(card_x_now, NULL);
	lv_async_call_cancel(grid_done_now, NULL);
	lv_async_call_cancel(grid_new_now, NULL);
	lv_async_call_cancel(grid_hist_now, NULL);
	lv_async_call_cancel(msheet_pick_now, NULL);
	tabs_save();
	grid = grid_list = msheet = mscrim = NULL;	/* (the shell deletes them with the app) */
	sel_ta = NULL;
	app_rt = NULL;
	root_leave();
}

const app_t app_web = {
	.name = "Web",
	.glyph = G_GLOBE,
	.icon = IC_BLUE,
	.open = app_open,
	.close = app_close,
};
