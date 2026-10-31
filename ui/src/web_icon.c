/* Site icons for the Web app (see web_icon.h).
 *
 * Discovery: wicon_page() queues the page's candidates (its apple-touch-icon, its best non-SVG rel=icon, then
 * /apple-touch-icon.png and /favicon.ico on its origin); one site is looked up at a time, one candidate after
 * the other, each a net.c GET with the page's own allow_private (so a public page's icon can never reach the
 * LAN / tailnet), no cookies, ≤ ICON_MAX_BYTES, ICON_TIMEOUT s. The first one that decodes (PNG / JPEG /
 * ICO through imgdec.c, on the net_bg worker) is stored pixel for pixel, shrunk to ≤ WICON_BIG px. A site with
 * no icon gets an empty marker file, so it isn't asked again for a week either; network errors store nothing.
 *
 * Cache: <data>/web/icons/<host>.bgra = 16-byte header ("OFI1", u16 w, h, natural size, flags) + w × h BGRA
 * (straight alpha); w = h = 0 = no icon. The file's mtime is when it was looked up; older than ICON_FRESH
 * = looked up again on the next visit (the old icon shows meanwhile). Past ICON_CACHE_CAP bytes the oldest
 * files go.
 *
 * Tiles: wicon_tile() misses are batched into one net_bg job that reads the files and composes both tile
 * sizes there: a big, full-bleed icon (apple-touch-icon) fills the tile, others sit centred on a light tile —
 * icons under 48 px at their own size (16 px ones doubled pixel for pixel), not blown up. Corners are rounded
 * into the pixels (no clip_corner while scrolling). Composed tiles stay in memory (≤ MEM_KEEP sites kept over
 * an app close); replaced ones are freed only on the next app open, when no lv_image can still show them. */
#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "imgdec.h"
#include "net.h"
#include "orchard.h"
#include "web_icon.h"

#define ICON_MAX_BYTES (256 << 10)
#define ICON_TIMEOUT 15
#define ICON_FRESH (7 * 86400)
#define ICON_CACHE_CAP (6 << 20)
#define ICON_SMALL_PX 48	/* below this an icon is drawn at its own size, not scaled to the tile */
#define MAX_ENT 320
#define MEM_KEEP 64
#define MAX_DISC 8		/* sites waiting to be looked up */
#define MAX_CAND 4
#define LOAD_BATCH 24
#define KEY_N 104
#define HDR 16
#define F_FULL 1		/* opaque up to its edges: fills the tile */

static const int tsizes[2] = { WICON_BIG, WICON_SMALL };

enum { E_UNKNOWN, E_LOADING, E_READY, E_ABSENT };
struct ent { char key[KEY_N]; int state; lv_image_dsc_t *t[2]; uint32_t bg; uint32_t used; };
static struct ent ents[MAX_ENT];
static int nent;
static uint32_t clock_n;
static lv_image_dsc_t **grave;	/* replaced tiles: freed by wicon_trim */
static int ngrave, capgrave;
static void (*listener)(void);

static void bury(lv_image_dsc_t *d)
{
	if (!d)
		return;
	if (ngrave == capgrave) {
		int nc = capgrave ? capgrave * 2 : 16;
		lv_image_dsc_t **g = realloc(grave, nc * sizeof(*g));
		if (!g)
			return;	/* leak one tile rather than free one that may show */
		grave = g;
		capgrave = nc;
	}
	grave[ngrave++] = d;
}

/* "https://en.m.wikipedia.org/…" → the cache name ("127.0.0.1:8000" → "127.0.0.1_8000"); "" = no usable host */
static const char *url_key(const char *url, char *out)
{
	char h[256];
	net_host(url, h, sizeof(h));
	int k = 0;
	for (const char *p = h; *p && k < KEY_N - 1; p++) {
		char c = *p;
		bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || (c == '.' && k > 0);
		out[k++] = ok ? c : '_';
	}
	/* an explicit port that isn't the scheme's own is another site (a router's :8080, test servers) */
	const char *au = strstr(url, "://");
	au = au ? au + 3 : url;
	const char *end = au + strcspn(au, "/?#"), *col = NULL;
	for (const char *q = au; q < end; q++)
		if (*q == ':')
			col = q;
		else if (*q == ']' || *q == '@')
			col = NULL;
	int port = col ? atoi(col + 1) : 0;
	if (k && port > 0 && port != 80 && port != 443 && k < KEY_N - 8)
		k += snprintf(out + k, KEY_N - k, "_%d", port);
	out[k] = 0;
	return out;
}

static const char *icon_dir(void)
{
	static char d[400];
	if (!d[0]) {
		snprintf(d, sizeof(d), "%s/web", data_dir());
		mkdir(d, 0755);
		snprintf(d, sizeof(d), "%s/web/icons", data_dir());
	}
	mkdir(d, 0700);
	return d;
}

static uint32_t tile_bg(void)	/* the light tile behind icons that don't fill it */
{
	return th.dark ? mix(pal.label, 0xffffff, 55) : mix(pal.cell, pal.label, 7);
}

static struct ent *ent_find(const char *key)
{
	for (int i = 0; i < nent; i++)
		if (!strcmp(ents[i].key, key))
			return &ents[i];
	return NULL;
}

static struct ent *ent_get(const char *key)
{
	struct ent *e = ent_find(key);
	if (e || nent == MAX_ENT)
		return e;
	e = &ents[nent++];
	memset(e, 0, sizeof(*e));
	snprintf(e->key, sizeof(e->key), "%s", key);
	return e;
}

static void ent_drop_tiles(struct ent *e)
{
	for (int s = 0; s < 2; s++) {
		bury(e->t[s]);
		e->t[s] = NULL;
	}
}

/* ---------- pixels (worker thread) ---------- */

/* sw×sh BGRA → dw×dh: whole-number enlargements pixel for pixel (crisp), other enlargements bilinear, shrinking
 * an area average weighted by alpha */
static void resample(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh)
{
	if (dw >= sw && dh >= sh && dw % sw == 0 && dh % sh == 0) {
		for (int y = 0; y < dh; y++)
			for (int x = 0; x < dw; x++)
				memcpy(dst + ((size_t)y * dw + x) * 4, src + ((size_t)(y * sh / dh) * sw + x * sw / dw) * 4, 4);
		return;
	}
	if (dw > sw || dh > sh) {
		for (int y = 0; y < dh; y++) {
			int fy = (int)(((int64_t)y * 2 + 1) * sh * 128 / (2 * dh)) - 64;
			fy = fy < 0 ? 0 : fy;
			int y0 = fy >> 7, wy = fy & 127, y1 = y0 + 1 < sh ? y0 + 1 : y0;
			for (int x = 0; x < dw; x++) {
				int fx = (int)(((int64_t)x * 2 + 1) * sw * 128 / (2 * dw)) - 64;
				fx = fx < 0 ? 0 : fx;
				int x0 = fx >> 7, wx = fx & 127, x1 = x0 + 1 < sw ? x0 + 1 : x0;
				const uint8_t *p[4] = { src + ((size_t)y0 * sw + x0) * 4, src + ((size_t)y0 * sw + x1) * 4,
							src + ((size_t)y1 * sw + x0) * 4, src + ((size_t)y1 * sw + x1) * 4 };
				int wt[4] = { (128 - wx) * (128 - wy), wx * (128 - wy), (128 - wx) * wy, wx * wy };
				int64_t a = 0, c[3] = { 0, 0, 0 };
				for (int k = 0; k < 4; k++) {
					int64_t wa = (int64_t)wt[k] * p[k][3];
					a += wa;
					for (int ch = 0; ch < 3; ch++)
						c[ch] += wa * p[k][ch];
				}
				uint8_t *d = dst + ((size_t)y * dw + x) * 4;
				for (int ch = 0; ch < 3; ch++)
					d[ch] = a ? (uint8_t)(c[ch] / a) : 0;
				d[3] = (uint8_t)(a >> 14);
			}
		}
		return;
	}
	for (int y = 0; y < dh; y++) {
		int ya = y * sh / dh, yb = (y + 1) * sh / dh;
		yb = yb > ya ? yb : ya + 1;
		for (int x = 0; x < dw; x++) {
			int xa = x * sw / dw, xb = (x + 1) * sw / dw;
			xb = xb > xa ? xb : xa + 1;
			uint64_t a = 0, c[3] = { 0, 0, 0 }, n = 0;
			for (int yy = ya; yy < yb; yy++)
				for (int xx = xa; xx < xb; xx++) {
					const uint8_t *p = src + ((size_t)yy * sw + xx) * 4;
					a += p[3];
					for (int ch = 0; ch < 3; ch++)
						c[ch] += (uint64_t)p[ch] * p[3];
					n++;
				}
			uint8_t *d = dst + ((size_t)y * dw + x) * 4;
			for (int ch = 0; ch < 3; ch++)
				d[ch] = a ? (uint8_t)(c[ch] / a) : 0;
			d[3] = (uint8_t)(a / n);
		}
	}
}

/* the tile: S×S ARGB8888, rounded like the monogram tiles (radius S·27 %), anti-aliased see-through corners */
static lv_image_dsc_t *compose(const uint8_t *src, int sw, int sh, int nat, int flags, int S, uint32_t bg)
{
	uint8_t *px = malloc((size_t)S * S * 4);
	lv_image_dsc_t *d = calloc(1, sizeof(*d));
	if (!px || !d) {
		free(px);
		free(d);
		return NULL;
	}
	int r = S * 27 / 100;
	if (flags & F_FULL) {
		resample(src, sw, sh, px, S, S);
		for (size_t i = 0; i < (size_t)S * S; i++)	/* a full-bleed icon is opaque: no see-through holes */
			if (px[i * 4 + 3] < 255) {
				int a = px[i * 4 + 3];
				for (int ch = 0; ch < 3; ch++)
					px[i * 4 + ch] = (px[i * 4 + ch] * a + (bg >> (8 * ch) & 0xff) * (255 - a)) / 255;
				px[i * 4 + 3] = 255;
			}
	} else {
		for (size_t i = 0; i < (size_t)S * S; i++) {
			px[i * 4] = bg & 0xff;
			px[i * 4 + 1] = bg >> 8 & 0xff;
			px[i * 4 + 2] = bg >> 16 & 0xff;
			px[i * 4 + 3] = 255;
		}
		int box = S * 60 / 100, want;
		if (nat >= ICON_SMALL_PX)
			want = box;
		else	/* small icons at their own size; 16-ish ones doubled (as a browser shows them at @2x) */
			want = (nat < 32 ? nat * 2 : nat) < box ? (nat < 32 ? nat * 2 : nat) : box;
		int m = sw > sh ? sw : sh;
		if (nat < ICON_SMALL_PX && m * 2 <= want)	/* whole-number factor: crisp */
			want = want / m * m;
		int iw = sw * want / m, ih = sh * want / m;
		iw = iw < 1 ? 1 : iw;
		ih = ih < 1 ? 1 : ih;
		uint8_t *ic = malloc((size_t)iw * ih * 4);
		if (ic) {
			resample(src, sw, sh, ic, iw, ih);
			int ox = (S - iw) / 2, oy = (S - ih) / 2;
			for (int y = 0; y < ih; y++)
				for (int x = 0; x < iw; x++) {
					const uint8_t *s = ic + ((size_t)y * iw + x) * 4;
					uint8_t *q = px + ((size_t)(oy + y) * S + ox + x) * 4;
					for (int ch = 0; ch < 3; ch++)
						q[ch] = (s[ch] * s[3] + q[ch] * (255 - s[3])) / 255;
				}
			free(ic);
		}
	}
	for (int y = 0; y < r; y++)	/* corner coverage, 4×4 samples per pixel (as app_web.c's img_round) */
		for (int x = 0; x < r; x++) {
			int in = 0;
			for (int sy = 0; sy < 4; sy++)
				for (int sx = 0; sx < 4; sx++) {
					int dx = 8 * r - (8 * x + 2 * sx + 1), dy = 8 * r - (8 * y + 2 * sy + 1);
					in += dx * dx + dy * dy <= 64 * r * r;
				}
			if (in == 16)
				continue;
			int xs[2] = { x, S - 1 - x }, ys[2] = { y, S - 1 - y };
			for (int a = 0; a < 2; a++)
				for (int c = 0; c < 2; c++)
					px[((size_t)ys[a] * S + xs[c]) * 4 + 3] = px[((size_t)ys[a] * S + xs[c]) * 4 + 3] * in / 16;
		}
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_ARGB8888;
	d->header.w = S;
	d->header.h = S;
	d->header.stride = S * 4;
	d->data_size = (uint32_t)S * S * 4;
	d->data = px;
	return d;
}

static void put16(uint8_t *p, unsigned v) { p[0] = v & 0xff; p[1] = v >> 8 & 0xff; }
static unsigned get16(const uint8_t *p) { return p[0] | p[1] << 8; }

/* a cache file → pixels (malloc'd) + header; false = missing / broken, *none = the "no icon" marker */
static bool read_icon(const char *path, uint8_t **px, int *w, int *h, int *nat, int *flags, bool *none)
{
	*px = NULL;
	*none = false;
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	uint8_t hd[HDR];
	bool ok = fread(hd, 1, HDR, f) == HDR && !memcmp(hd, "OFI1", 4);
	if (ok) {
		*w = get16(hd + 4);
		*h = get16(hd + 6);
		*nat = get16(hd + 8);
		*flags = get16(hd + 10);
		if (!*w && !*h)
			*none = true;
		else if (*w < 1 || *h < 1 || *w > 256 || *h > 256)
			ok = false;
		else if ((*px = malloc((size_t)*w * *h * 4)))
			ok = fread(*px, 1, (size_t)*w * *h * 4, f) == (size_t)*w * *h * 4;
		else
			ok = false;
	}
	fclose(f);
	if (!ok) {
		free(*px);
		*px = NULL;
		*none = false;
	}
	return ok;
}

static void write_icon(const char *path, const uint8_t *px, int w, int h, int nat, int flags)
{
	char tmp[512];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return;
	uint8_t hd[HDR] = { 'O', 'F', 'I', '1' };
	put16(hd + 4, w);
	put16(hd + 6, h);
	put16(hd + 8, nat);
	put16(hd + 10, flags);
	bool ok = fwrite(hd, 1, HDR, f) == HDR && (!px || fwrite(px, 1, (size_t)w * h * 4, f) == (size_t)w * h * 4);
	if (fclose(f) == 0 && ok)
		rename(tmp, path);
	else
		unlink(tmp);
}

struct cfile { char name[KEY_N + 8]; off_t size; time_t mtime; };
static int cfile_cmp(const void *a, const void *b)
{
	const struct cfile *x = a, *y = b;
	return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

/* over the cap: the oldest files go until it is down to 3/4 */
static void prune(const char *dir)
{
	DIR *dd = opendir(dir);
	if (!dd)
		return;
	struct cfile *fs = NULL;
	int n = 0, cap = 0;
	long long total = 0;
	struct dirent *de;
	while ((de = readdir(dd))) {
		size_t l = strlen(de->d_name);
		if (l < 6 || l >= KEY_N + 8 || strcmp(de->d_name + l - 5, ".bgra"))
			continue;
		char p[600];
		struct stat st;
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		if (stat(p, &st))
			continue;
		if (n == cap) {
			int nc = cap ? cap * 2 : 64;
			struct cfile *g = realloc(fs, nc * sizeof(*g));
			if (!g)
				break;
			fs = g;
			cap = nc;
		}
		snprintf(fs[n].name, sizeof(fs[n].name), "%s", de->d_name);
		fs[n].size = st.st_size;
		fs[n].mtime = st.st_mtime;
		total += st.st_size;
		n++;
	}
	closedir(dd);
	if (total > ICON_CACHE_CAP) {
		qsort(fs, n, sizeof(*fs), cfile_cmp);
		for (int i = 0; i < n && total > ICON_CACHE_CAP * 3LL / 4; i++) {
			char p[600];
			snprintf(p, sizeof(p), "%s/%s", dir, fs[i].name);
			if (!unlink(p))
				total -= fs[i].size;
		}
	}
	free(fs);
}

/* ---------- reading tiles from the cache ---------- */

struct litem { char key[KEY_N]; lv_image_dsc_t *t[2]; bool absent; };
struct ljob { char dir[400]; uint32_t bg; int n; struct litem it[LOAD_BATCH]; };
static char lq[MAX_ENT][KEY_N];	/* keys waiting for a load job */
static int nlq;
static bool load_busy, load_sched;

static void tiles_from(struct litem *it, const uint8_t *px, int w, int h, int nat, int flags, uint32_t bg)
{
	for (int s = 0; s < 2; s++)
		it->t[s] = compose(px, w, h, nat, flags, tsizes[s], bg);
}

static void ljob_work(void *a)
{
	struct ljob *j = a;
	for (int i = 0; i < j->n; i++) {
		struct litem *it = &j->it[i];
		char p[600];
		snprintf(p, sizeof(p), "%s/%s.bgra", j->dir, it->key);
		uint8_t *px;
		int w, h, nat, flags;
		bool none;
		if (read_icon(p, &px, &w, &h, &nat, &flags, &none) && !none)
			tiles_from(it, px, w, h, nat, flags, j->bg);
		it->absent = !it->t[0] || !it->t[1];
		free(px);
	}
}

static void load_flush(void *ud);

static void ljob_done(void *a)
{
	struct ljob *j = a;
	bool any = false;
	for (int i = 0; i < j->n; i++) {
		struct litem *it = &j->it[i];
		struct ent *e = ent_find(it->key);
		if (e && e->state == E_LOADING) {
			if (it->absent) {
				e->state = E_ABSENT;
			} else {
				ent_drop_tiles(e);
				e->t[0] = it->t[0];
				e->t[1] = it->t[1];
				e->bg = j->bg;
				e->state = E_READY;
				it->t[0] = it->t[1] = NULL;
				any = true;
			}
		}
		for (int s = 0; s < 2; s++)	/* nobody showed these */
			img_free(it->t[s]);
	}
	free(j);
	load_busy = false;
	if (nlq)
		load_flush(NULL);
	if (any && listener)
		listener();
}

static void load_flush(void *ud)
{
	(void)ud;
	load_sched = false;
	if (load_busy || !nlq)
		return;
	struct ljob *j = calloc(1, sizeof(*j));
	if (!j)
		return;
	snprintf(j->dir, sizeof(j->dir), "%s", icon_dir());
	j->bg = tile_bg();
	while (nlq && j->n < LOAD_BATCH) {
		nlq--;
		snprintf(j->it[j->n++].key, KEY_N, "%s", lq[nlq]);
	}
	load_busy = true;
	net_bg(ljob_work, ljob_done, j);
}

static void load_queue(struct ent *e)
{
	e->state = E_LOADING;
	if (nlq == MAX_ENT) {
		e->state = E_UNKNOWN;
		return;
	}
	snprintf(lq[nlq++], KEY_N, "%s", e->key);
	if (!load_sched && !load_busy) {
		load_sched = true;
		lv_async_call(load_flush, NULL);	/* once the page that asked is built: one job for all its tiles */
	}
}

const lv_image_dsc_t *wicon_tile(const char *url, int size)
{
	int s = size == WICON_BIG ? 0 : size == WICON_SMALL ? 1 : -1;
	char key[KEY_N];
	if (s < 0 || !*url_key(url, key))
		return NULL;
	struct ent *e = ent_get(key);
	if (!e)
		return NULL;
	e->used = ++clock_n;
	if (e->state == E_READY && e->bg != tile_bg()) {	/* the theme changed: compose them again */
		ent_drop_tiles(e);
		e->state = E_UNKNOWN;
	}
	if (e->state == E_UNKNOWN)
		load_queue(e);
	return e->state == E_READY ? e->t[s] : NULL;
}

void wicon_listen(void (*cb)(void)) { listener = cb; }

/* ---------- looking icons up ---------- */

struct disc { char key[KEY_N]; char cand[MAX_CAND][1024]; int nc, next; char referer[1024]; bool priv, desktop, neterr; };
static struct disc dq[MAX_DISC];	/* [0] is being looked up while disc_busy */
static int ndq;
static bool disc_busy;
static net_req_t *disc_req;

struct djob { char key[KEY_N], dir[400]; char *data; size_t len; uint32_t bg; bool none, touch_only;
	      bool ok; lv_image_dsc_t *t[2]; int nat, w, h; };

static void disc_next(void);

/* opaque along the middle of all four edges: an icon drawn to fill its square (apple-touch-icon style) */
static bool full_bleed(const uint8_t *px, int w, int h)
{
	const int pts[4][2] = { { w / 2, 0 }, { w / 2, h - 1 }, { 0, h / 2 }, { w - 1, h / 2 } };
	for (int i = 0; i < 4; i++)
		if (px[((size_t)pts[i][1] * w + pts[i][0]) * 4 + 3] < 250)
			return false;
	return w * 10 >= h * 9 && h * 10 >= w * 9;
}

static void djob_work(void *a)
{
	struct djob *j = a;
	char p[600];
	snprintf(p, sizeof(p), "%s/%s.bgra", j->dir, j->key);
	if (j->touch_only) {	/* nothing new found, the old icon (if any) stays: asked again in a week */
		if (utimes(p, NULL) && errno == ENOENT)
			write_icon(p, NULL, 0, 0, 0, 0);
		return;
	}
	const char *err;
	int nat = 0;
	lv_image_dsc_t *d = img_decode_1x(j->data, j->len, WICON_BIG, &nat, &err);
	free(j->data);
	j->data = NULL;
	if (!d || d->header.cf == LV_COLOR_FORMAT_RGB565)
		goto out;
	int w = d->header.w, h = d->header.h;
	uint8_t *px = (uint8_t *)d->data;
	if (d->header.stride != (uint32_t)w * 4)
		goto out;
	if (d->header.cf == LV_COLOR_FORMAT_XRGB8888)
		for (size_t i = 0; i < (size_t)w * h; i++)
			px[i * 4 + 3] = 255;
	int flags = nat >= 64 && full_bleed(px, w, h) ? F_FULL : 0;
	write_icon(p, px, w, h, nat, flags);
	prune(j->dir);
	struct litem it = { { 0 } };
	tiles_from(&it, px, w, h, nat, flags, j->bg);
	j->t[0] = it.t[0];
	j->t[1] = it.t[1];
	j->ok = j->t[0] && j->t[1];
	j->nat = nat;
	j->w = w;
	j->h = h;
out:
	img_free(d);
}

static void djob_done(void *a)
{
	struct djob *j = a;
	if (j->touch_only) {
		free(j);
		disc_busy = false;
		disc_next();
		return;
	}
	if (!j->ok) {	/* not an image we read: the next candidate */
		for (int s = 0; s < 2; s++)
			img_free(j->t[s]);
		free(j);
		dq[0].next++;
		disc_busy = false;
		disc_next();
		return;
	}
	fprintf(stderr, "wicon: %s %dx%d (%d px) from %s\n", j->key, j->w, j->h, j->nat, dq[0].cand[dq[0].next]);
	struct ent *e = ent_get(j->key);
	if (e) {
		ent_drop_tiles(e);
		e->t[0] = j->t[0];
		e->t[1] = j->t[1];
		e->bg = j->bg;
		e->state = E_READY;
	} else {
		img_free(j->t[0]);
		img_free(j->t[1]);
	}
	free(j);
	memmove(dq, dq + 1, (ndq - 1) * sizeof(dq[0]));	/* this site is done */
	ndq--;
	disc_busy = false;
	disc_next();
	if (e && listener)
		listener();
}

static void disc_fetched(net_res_t *r, void *ud)
{
	(void)ud;
	disc_req = NULL;
	struct disc *q = &dq[0];
	if (r->err || r->status >= 400 || r->truncated || !r->len || !strncmp(r->ctype, "text/", 5)) {
		if (r->err && r->err != NET_EPRIVATE && r->err != NET_EBLOCKED)
			q->neterr = true;
		q->next++;
		disc_busy = false;
		disc_next();
		return;
	}
	struct djob *j = calloc(1, sizeof(*j));
	if (!j) {
		q->next++;
		disc_busy = false;
		disc_next();
		return;
	}
	snprintf(j->key, sizeof(j->key), "%s", q->key);
	snprintf(j->dir, sizeof(j->dir), "%s", icon_dir());
	j->bg = tile_bg();
	j->len = r->len;
	j->data = net_take(r);
	net_bg(djob_work, djob_done, j);	/* disc_busy stays: djob_done goes on */
}

/* start (or go on with) the site at the head of the queue */
static void disc_next(void)
{
	while (!disc_busy && ndq) {
		struct disc *q = &dq[0];
		if (q->next >= q->nc) {	/* none of them: remember that (unless the network was the problem) */
			bool neterr = q->neterr;
			struct djob *j = neterr ? NULL : calloc(1, sizeof(*j));
			if (j) {
				snprintf(j->key, sizeof(j->key), "%s", q->key);
				snprintf(j->dir, sizeof(j->dir), "%s", icon_dir());
				j->touch_only = true;
			}
			memmove(dq, dq + 1, (ndq - 1) * sizeof(dq[0]));
			ndq--;
			if (j) {
				disc_busy = true;
				net_bg(djob_work, djob_done, j);
				return;
			}
			continue;
		}
		net_opts_t o = { .url = q->cand[q->next], .max_bytes = ICON_MAX_BYTES, .timeout_s = ICON_TIMEOUT,
				 .referer = q->referer, .desktop = q->desktop, .cookies = NET_CK_NONE,
				 .allow_private = q->priv };
		disc_req = net_fetch(&o, disc_fetched, NULL);
		if (disc_req)
			disc_busy = true;
		else
			q->next++;
	}
}

static void add_cand(struct disc *q, const char *url)
{
	if (!url || !*url || q->nc == MAX_CAND || strlen(url) >= sizeof(q->cand[0]))
		return;
	if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8))
		return;
	for (int i = 0; i < q->nc; i++)
		if (!strcmp(q->cand[i], url))
			return;
	snprintf(q->cand[q->nc++], sizeof(q->cand[0]), "%s", url);
}

void wicon_page(const char *url, const html_doc_t *d, bool priv, bool desktop)
{
	char key[KEY_N], p[600];
	if (!url || !*url_key(url, key))
		return;
	for (int i = 0; i < ndq; i++)
		if (!strcmp(dq[i].key, key))
			return;
	snprintf(p, sizeof(p), "%s/%s.bgra", icon_dir(), key);
	struct stat st;
	if (!stat(p, &st) && time(NULL) - st.st_mtime < ICON_FRESH && st.st_mtime <= time(NULL) + 86400)
		return;
	if (ndq == MAX_DISC)
		return;
	struct disc *q = &dq[ndq];
	memset(q, 0, sizeof(*q));
	snprintf(q->key, sizeof(q->key), "%s", key);
	snprintf(q->referer, sizeof(q->referer), "%.*s", (int)strcspn(url, "#"), url);
	q->priv = priv;
	q->desktop = desktop;
	char abs[2048];
	if (d) {
		add_cand(q, d->touch_icon);
		add_cand(q, d->icon);
	}
	if (net_resolve(url, "/apple-touch-icon.png", abs, sizeof(abs)))
		add_cand(q, abs);
	if (net_resolve(url, "/favicon.ico", abs, sizeof(abs)))
		add_cand(q, abs);
	if (!q->nc)
		return;
	ndq++;
	disc_next();
}

/* ---------- housekeeping ---------- */

static bool kept(const char *name, const char *const *keep, int n)
{
	char k[KEY_N];
	for (int i = 0; i < n; i++) {
		url_key(keep[i], k);
		size_t l = strlen(k);
		if (*k && !strncmp(name, k, l) && !strcmp(name + l, ".bgra"))
			return true;
	}
	return false;
}

void wicon_clear(const char *const *keep_urls, int n)
{
	const char *dir = icon_dir();
	DIR *dd = opendir(dir);
	if (dd) {
		struct dirent *de;
		while ((de = readdir(dd)))
			if (de->d_name[0] != '.' && !kept(de->d_name, keep_urls, n)) {
				char p[600];
				snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
				unlink(p);
			}
		closedir(dd);
	}
	for (int i = 0; i < nent; i++) {
		char nm[KEY_N + 8];
		snprintf(nm, sizeof(nm), "%s.bgra", ents[i].key);
		if (!kept(nm, keep_urls, n)) {
			ent_drop_tiles(&ents[i]);
			if (ents[i].state != E_LOADING)	/* a load in flight finds the file gone */
				ents[i].state = E_UNKNOWN;
		}
	}
	/* a lookup in flight still stores what it finds: it is for a page being visited right now */
}

static int used_cmp(const void *a, const void *b)
{
	const struct ent *x = a, *y = b;
	return x->used > y->used ? -1 : x->used < y->used;
}

void wicon_trim(void)
{
	for (int i = 0; i < ngrave; i++)
		img_free(grave[i]);
	ngrave = 0;
	if (nent <= MEM_KEEP || load_busy || nlq)	/* (a load job in flight refers to entries by key: fine, but keep it simple) */
		return;
	qsort(ents, nent, sizeof(ents[0]), used_cmp);	/* most recently used first */
	for (int i = MEM_KEEP; i < nent; i++)
		for (int s = 0; s < 2; s++)
			img_free(ents[i].t[s]);
	nent = MEM_KEEP;
}
