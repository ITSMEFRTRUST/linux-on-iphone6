/* Camera filters (cam_lut.h): 3D LUTs in Adobe/Resolve .cube text (LUT_3D_SIZE, DOMAIN_MIN/MAX,
 * LUT_3D_INPUT_RANGE; red changes fastest) graded into the saved photos.
 * The table is fixed point: each grid point = 3 × int16 in 1/64 of an 8-bit level (0..16320), padded to 4 so a
 * point is 8 bytes (33³ = 287 KB). Per channel, an 8-bit input maps to its lower grid point (as a ready-made table
 * offset) + a 12-bit fraction, so a pixel is three table lookups, a 6-way compare to pick the tetrahedron, four grid
 * points and 9 multiply-adds — the same tetrahedral interpolation as ffmpeg's lut3d=interp=tetrahedral, rounded
 * instead of truncated. A run of identical pixels reuses the last result. Big pictures split over both A8 cores.
 * Bench / PPM tool: -DCAM_LUT_BENCH, built by ui/test/unit/build-camlut-bench.sh. */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cam_lut.h"

const char *data_dir(void);	/* prefs.c */

#define FB 12				/* fraction bits */
#define ONE (1 << FB)
#define TMAX 16320			/* 255 × 64: table units are 1/64 level */
#define MAX_N 129
#define MT_MIN (1u << 20)		/* pixels: below this one thread is quicker than starting a second */

struct cam_lut {
	int n;
	int16_t *t;			/* n³ × { r, g, b, 0 }, red fastest */
	uint32_t off[3][256];		/* per channel: int16 offset of the lower grid point */
	uint16_t frac[3][256];		/* 0..ONE */
	uint32_t step[3];		/* int16 offset of +1 along r / g / b */
};

/* ---------- filters ---------- */

static const char *const labels[CAM_FILTER_N] = { "ORIGINAL", "WARM", "VIVID" };
static const char *const keys[CAM_FILTER_N] = { "original", "warm", "vivid" };
static const char *const files[CAM_FILTER_N] = { NULL, "warm.cube", "vivid.cube" };

static int sel = CAM_FILTER_DEFAULT;	/* (atomics: __atomic builtins, musl has no stdatomic.h) */
static _Thread_local int shot = -1;	/* the filter this thread's last photo got (its thumbnail gets the same) */

const char *cam_filter_label(int f) { return f >= 0 && f < CAM_FILTER_N ? labels[f] : "?"; }
const char *cam_filter_key(int f) { return f >= 0 && f < CAM_FILTER_N ? keys[f] : "?"; }

int cam_filter_from_key(const char *key)
{
	for (int f = 0; key && f < CAM_FILTER_N; f++)
		if (!strcmp(key, keys[f]))
			return f;
	return CAM_FILTER_DEFAULT;
}

void cam_filter_set(int f)
{
	if (f >= 0 && f < CAM_FILTER_N)
		__atomic_store_n(&sel, f, __ATOMIC_RELEASE);
}

int cam_filter_get(void) { return __atomic_load_n(&sel, __ATOMIC_ACQUIRE); }

/* ---------- loading (once per filter, off the UI thread) ---------- */

static pthread_mutex_t load_mu = PTHREAD_MUTEX_INITIALIZER;
static cam_lut_t *tabs[CAM_FILTER_N];
static bool done[CAM_FILTER_N];		/* the load finished, with or without a table (then tabs[] is final) */
static bool started[CAM_FILTER_N];	/* a background load was started */

static double now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

const cam_lut_t *cam_lut_for(int f)
{
	if (f <= CAM_FILTER_ORIGINAL || f >= CAM_FILTER_N)
		return NULL;
	if (__atomic_load_n(&done[f], __ATOMIC_ACQUIRE))
		return tabs[f];
	pthread_mutex_lock(&load_mu);
	if (!done[f]) {
		char path[512], why[160] = "";
		double t0 = now_ms();
		snprintf(path, sizeof(path), "%s/luts/%s", data_dir(), files[f]);
		cam_lut_t *l = cam_lut_load(path, why, sizeof(why));
		if (!l && errno == ENOENT) {
			snprintf(path, sizeof(path), "/usr/share/orchard/luts/%s", files[f]);
			l = cam_lut_load(path, why, sizeof(why));
		}
		if (l)
			fprintf(stderr, "cam: filter %s = %s (%d³, %.0f ms)\n", keys[f], path, l->n, now_ms() - t0);
		else
			fprintf(stderr, "cam: filter %s: %s — photos stay Original\n", keys[f], why);
		__atomic_store_n(&tabs[f], l, __ATOMIC_RELAXED);
		__atomic_store_n(&done[f], true, __ATOMIC_RELEASE);
	}
	pthread_mutex_unlock(&load_mu);
	return tabs[f];
}

static void *preload_main(void *arg)
{
	cam_lut_for((int)(intptr_t)arg);
	return NULL;
}

const cam_lut_t *cam_lut_peek(int f)
{
	if (f <= CAM_FILTER_ORIGINAL || f >= CAM_FILTER_N)
		return NULL;
	if (__atomic_load_n(&done[f], __ATOMIC_ACQUIRE))
		return tabs[f];
	if (!__atomic_exchange_n(&started[f], true, __ATOMIC_ACQ_REL)) {
		pthread_t t;
		pthread_attr_t a;
		pthread_attr_init(&a);
		pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
		if (pthread_create(&t, &a, preload_main, (void *)(intptr_t)f) != 0)
			__atomic_store_n(&started[f], false, __ATOMIC_RELEASE);
		pthread_attr_destroy(&a);
	}
	return NULL;
}

void cam_lut_photo(uint32_t *px, size_t n)
{
	shot = cam_filter_get();
	cam_lut_apply(cam_lut_for(shot), px, n);
}

void cam_lut_thumb(uint32_t *px, size_t n)
{
	cam_lut_apply(cam_lut_for(shot >= 0 ? shot : cam_filter_get()), px, n);
}

/* ---------- parsing ---------- */

/* a decimal number (the common "0.123456" form without strtod — musl's is slow for 108 k of them); NULL = none */
static const char *number(const char *p, double *out)
{
	static const double p10[] = { 1, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14,
				      1e15, 1e16, 1e17, 1e18 };
	while (*p == ' ' || *p == '\t')
		p++;
	const char *s = p;
	bool neg = *p == '-';
	if (*p == '-' || *p == '+')
		p++;
	uint64_t m = 0;
	int nd = 0, sc = 0;
	const char *d0 = p;
	for (; *p >= '0' && *p <= '9'; p++)
		if (nd < 18)
			m = m * 10 + (uint64_t)(*p - '0'), nd += m > 0;
		else
			sc++;
	bool digits = p > d0;
	if (*p == '.') {
		const char *f0 = ++p;
		for (; *p >= '0' && *p <= '9'; p++)
			if (nd < 18)
				m = m * 10 + (uint64_t)(*p - '0'), nd += m > 0, sc--;
		digits |= p > f0;
	}
	if (!digits)
		return NULL;
	if (*p == 'e' || *p == 'E' || sc < -18 || sc > 18) {	/* rare: let libc do it */
		char *e;
		*out = strtod(s, &e);
		return e > s ? e : NULL;
	}
	double v = (double)m;
	v = sc < 0 ? v / p10[-sc] : v * p10[sc];
	*out = neg ? -v : v;
	return p;
}

static bool word(const char **p, const char *w)
{
	size_t n = strlen(w);
	if (strncmp(*p, w, n) || ((*p)[n] != ' ' && (*p)[n] != '\t'))
		return false;
	*p += n;
	return true;
}

/* text[len] must be 0: every scan below stops at a NUL as well as at the line's end (cam_lut_parse: any text) */
static cam_lut_t *parse(const char *text, size_t len, char *why, size_t wn)
{
	int n = 0;
	size_t got = 0, want = 0;
	double dmin[3] = { 0, 0, 0 }, dmax[3] = { 1, 1, 1 };
	cam_lut_t *l = NULL;
	const char *p = text, *end = text + len;
	int line = 0;
	while (p < end) {
		const char *eol = memchr(p, '\n', (size_t)(end - p));
		if (!eol)
			eol = end;
		line++;
		while (p < eol && (*p == ' ' || *p == '\t' || *p == '\r'))
			p++;
		if (p == eol || *p == '#') {
			p = eol + 1;
			continue;
		}
		double v[3];
		const char *q = p;
		if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.') {	/* a grid point */
			if (!l) {
				snprintf(why, wn, "line %d: data before LUT_3D_SIZE", line);
				goto bad;
			}
			for (int k = 0; k < 3; k++)
				if (!(q = number(q, &v[k]))) {
					snprintf(why, wn, "line %d: expected 3 numbers", line);
					goto bad;
				}
			if (got == want) {
				snprintf(why, wn, "more than %zu grid points", want);
				goto bad;
			}
			for (int k = 0; k < 3; k++) {
				double x = v[k] < 0 ? 0 : v[k] > 1 ? 1 : v[k];
				l->t[got * 4 + (size_t)k] = (int16_t)lround(x * TMAX);
			}
			got++;
		} else if (word(&q, "LUT_3D_SIZE")) {
			if (!number(q, &v[0]) || v[0] < 2 || v[0] > MAX_N || l) {
				snprintf(why, wn, "line %d: bad LUT_3D_SIZE", line);
				goto bad;
			}
			n = (int)v[0];
			want = (size_t)n * n * n;
			if (!(l = calloc(1, sizeof(*l))) || !(l->t = calloc(want * 4, sizeof(int16_t)))) {
				snprintf(why, wn, "out of memory");
				goto bad;
			}
			l->n = n;
		} else if (word(&q, "DOMAIN_MIN") || word(&q, "DOMAIN_MAX")) {
			double *d = p[7] == 'M' && p[8] == 'I' ? dmin : dmax;
			for (int k = 0; k < 3; k++)
				if (!(q = number(q, &d[k])) || !isfinite(d[k])) {	/* (±1e999: inf / inf = NaN below) */
					snprintf(why, wn, "line %d: bad DOMAIN", line);
					goto bad;
				}
		} else if (word(&q, "LUT_3D_INPUT_RANGE")) {
			double a, b;
			if (!(q = number(q, &a)) || !number(q, &b) || !isfinite(a) || !isfinite(b)) {
				snprintf(why, wn, "line %d: bad LUT_3D_INPUT_RANGE", line);
				goto bad;
			}
			for (int k = 0; k < 3; k++)
				dmin[k] = a, dmax[k] = b;
		} else if (word(&q, "LUT_1D_SIZE")) {
			snprintf(why, wn, "a 1D LUT (only 3D ones are supported)");
			goto bad;
		}	/* TITLE and anything else: ignored */
		p = eol + 1;
	}
	if (!l || got != want) {
		snprintf(why, wn, l ? "%zu of %zu grid points" : "no LUT_3D_SIZE", got, want);
		goto bad;
	}
	for (int c = 0; c < 3; c++) {
		if (!(dmax[c] > dmin[c])) {
			snprintf(why, wn, "bad DOMAIN_MIN / DOMAIN_MAX");
			goto bad;
		}
		const uint32_t step = c == 0 ? 4 : c == 1 ? 4u * n : 4u * n * n;
		l->step[c] = step;
		for (int x = 0; x < 256; x++) {
			double pos = (x / 255.0 - dmin[c]) / (dmax[c] - dmin[c]) * (n - 1);
			pos = !(pos > 0) ? 0 : pos > n - 1 ? n - 1 : pos;	/* (NaN → 0: (int)NaN is undefined) */
			int i = (int)pos;
			if (i > n - 2)
				i = n - 2;
			l->off[c][x] = (uint32_t)i * step;
			l->frac[c][x] = (uint16_t)lround((pos - i) * ONE);
		}
	}
	return l;
bad:
	cam_lut_free(l);
	return NULL;
}

cam_lut_t *cam_lut_parse(const char *text, size_t len, char *why, size_t wn)
{
	char *z = malloc(len + 1);	/* (a NUL-terminated copy: the scans never read past len) */
	if (!z) {
		snprintf(why, wn, "out of memory");
		return NULL;
	}
	memcpy(z, text, len);
	z[len] = 0;
	cam_lut_t *l = parse(z, len, why, wn);
	free(z);
	return l;
}

cam_lut_t *cam_lut_load(const char *path, char *why, size_t wn)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		int e = errno;
		snprintf(why, wn, "%s: %s", path, strerror(e));
		errno = e;
		return NULL;
	}
	cam_lut_t *l = NULL;
	char *buf = NULL;
	long len = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
	if (len <= 0 || len > (16 << 20) || fseek(f, 0, SEEK_SET) != 0)
		snprintf(why, wn, "%s: not a LUT", path);
	else if (!(buf = malloc((size_t)len + 1)))
		snprintf(why, wn, "out of memory");
	else if (fread(buf, 1, (size_t)len, f) != (size_t)len)
		snprintf(why, wn, "%s: read error", path);
	else {
		buf[len] = 0;
		char w[120] = "";
		if (!(l = parse(buf, (size_t)len, w, sizeof(w))))
			snprintf(why, wn, "%s: %s", path, w);
	}
	free(buf);
	fclose(f);
	errno = l ? 0 : EINVAL;
	return l;
}

void cam_lut_free(cam_lut_t *l)
{
	if (l)
		free(l->t);
	free(l);
}

int cam_lut_size(const cam_lut_t *l) { return l ? l->n : 0; }

/* ---------- applying ---------- */

static void apply_range(const cam_lut_t *l, uint32_t *px, size_t n)
{
	const int16_t *t = l->t;
	const uint32_t sr = l->step[0], sg = l->step[1], sb = l->step[2], s111 = sr + sg + sb;
	uint32_t last_in = ~px[0] & 0xffffff, last_out = 0;	/* (never matches the first pixel) */
	for (size_t i = 0; i < n; i++) {
		const uint32_t p = px[i], in = p & 0xffffff;
		if (in == last_in) {
			px[i] = (p & 0xff000000u) | last_out;
			continue;
		}
		const unsigned r = in >> 16, g = (in >> 8) & 0xff, b = in & 0xff;
		const int16_t *c0 = t + l->off[0][r] + l->off[1][g] + l->off[2][b];
		const int fr = l->frac[0][r], fg = l->frac[1][g], fb = l->frac[2][b];
		uint32_t sa, sab;	/* the tetrahedron: c0 → c0 + sa → c0 + sab → c0 + s111 */
		int f1, f2, f3;		/* its fractions, largest first */
		if (fr > fg) {
			if (fg > fb)
				sa = sr, sab = sr + sg, f1 = fr, f2 = fg, f3 = fb;
			else if (fr > fb)
				sa = sr, sab = sr + sb, f1 = fr, f2 = fb, f3 = fg;
			else
				sa = sb, sab = sr + sb, f1 = fb, f2 = fr, f3 = fg;
		} else {
			if (fb > fg)
				sa = sb, sab = sg + sb, f1 = fb, f2 = fg, f3 = fr;
			else if (fb > fr)
				sa = sg, sab = sg + sb, f1 = fg, f2 = fb, f3 = fr;
			else
				sa = sg, sab = sr + sg, f1 = fg, f2 = fr, f3 = fb;
		}
		const int16_t *ca = c0 + sa, *cb = c0 + sab, *c1 = c0 + s111;
		uint32_t out = 0;
		for (int k = 0; k < 3; k++) {	/* r, g, b: unrolled by the compiler */
			const int v = (c0[k] << FB) + f1 * (ca[k] - c0[k]) + f2 * (cb[k] - ca[k]) + f3 * (c1[k] - cb[k]);
			out = out << 8 | (uint32_t)((v + (1 << (FB + 5))) >> (FB + 6));	/* (a convex mix: 0..255) */
		}
		last_in = in, last_out = out;
		px[i] = (p & 0xff000000u) | out;
	}
}

typedef struct job {
	const cam_lut_t *l;
	uint32_t *px;
	size_t n;
} job_t;

static void *job_main(void *arg)
{
	job_t *j = arg;
	apply_range(j->l, j->px, j->n);
	return NULL;
}

static int threads(void)
{
	static int nt;
	if (!nt) {
		const char *e = getenv("ORCHARD_LUT_THREADS");
		nt = e && atoi(e) >= 1 ? (atoi(e) > 2 ? 2 : atoi(e)) : 2;	/* the A8 has two cores */
	}
	return nt;
}

void cam_lut_apply(const cam_lut_t *l, uint32_t *px, size_t n)
{
	if (!l || !n)
		return;
	if (n >= MT_MIN && threads() > 1) {	/* second half on a helper thread */
		const size_t half = n / 2;
		job_t j = { l, px + half, n - half };
		pthread_t t;
		if (pthread_create(&t, NULL, job_main, &j) == 0) {
			apply_range(l, px, half);
			pthread_join(t, NULL);
			return;
		}
	}
	apply_range(l, px, n);
}

#ifdef CAM_LUT_BENCH
/* camlut-bench CUBE [W H]          time load + apply on a synthetic W×H photo (default 3264×2448 = 8 MP)
 * camlut-bench CUBE in.ppm out.ppm grade a binary PPM (P6, 8-bit) */
const char *data_dir(void)
{
	const char *e = getenv("ORCHARD_DATA");
	return e ? e : "/tmp";
}

static uint32_t *read_ppm(const char *path, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	int mx;
	if (!f || fscanf(f, "P6 %d %d %d", w, h, &mx) != 3 || mx != 255 || fgetc(f) == EOF) {
		if (f)
			fclose(f);
		return NULL;
	}
	size_t n = (size_t)*w * *h;
	uint8_t *rgb = malloc(n * 3);
	uint32_t *px = malloc(n * 4);
	if (!rgb || !px || fread(rgb, 3, n, f) != n) {
		free(rgb), free(px), fclose(f);
		return NULL;
	}
	for (size_t i = 0; i < n; i++)
		px[i] = 0xff000000u | (uint32_t)rgb[3 * i] << 16 | (uint32_t)rgb[3 * i + 1] << 8 | rgb[3 * i + 2];
	free(rgb);
	fclose(f);
	return px;
}

static int write_ppm(const char *path, const uint32_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return 1;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (size_t i = 0; i < (size_t)w * h; i++) {
		uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] };
		fwrite(c, 1, 3, f);
	}
	return fclose(f) != 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s CUBE [W H] | CUBE in.ppm out.ppm\n", argv[0]);
		return 2;
	}
	char why[200];
	double t0 = now_ms();
	cam_lut_t *l = cam_lut_load(argv[1], why, sizeof(why));
	double tl = now_ms() - t0;
	if (!l) {
		fprintf(stderr, "%s\n", why);
		return 1;
	}
	if (argc == 4 && strstr(argv[2], ".ppm")) {
		int w, h;
		uint32_t *px = read_ppm(argv[2], &w, &h);
		if (!px) {
			fprintf(stderr, "can't read %s\n", argv[2]);
			return 1;
		}
		cam_lut_apply(l, px, (size_t)w * h);
		return write_ppm(argv[3], px, w, h);
	}
	int w = argc >= 4 ? atoi(argv[2]) : 3264, h = argc >= 4 ? atoi(argv[3]) : 2448;
	size_t n = (size_t)w * h;
	uint32_t *src = malloc(n * 4), *px = malloc(n * 4);
	if (!src || !px)
		return 1;
	uint32_t s = 2463534242u;
	for (int y = 0; y < h; y++)	/* smooth gradients + sensor-like noise: few repeated pixels */
		for (int x = 0; x < w; x++) {
			s ^= s << 13, s ^= s >> 17, s ^= s << 5;
			int nz = (int)(s & 15) - 8;
			int r = x * 255 / w + nz, g = y * 255 / h + nz / 2, b = (x + y) * 255 / (w + h) - nz;
			r = r < 0 ? 0 : r > 255 ? 255 : r, g = g < 0 ? 0 : g > 255 ? 255 : g, b = b < 0 ? 0 : b > 255 ? 255 : b;
			src[(size_t)y * w + x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
	printf("load+parse %s: %.1f ms (%d^3)\n", argv[1], tl, l->n);
	for (int nt = 1; nt <= 2; nt++) {
		double best = 1e9, first = 0;
		for (int k = 0; k < 4; k++) {
			memcpy(px, src, n * 4);
			double a = now_ms();
			if (nt == 1)
				apply_range(l, px, n);
			else
				cam_lut_apply(l, px, n);
			double d = now_ms() - a;
			if (!k)
				first = d;
			best = d < best ? d : best;
		}
		printf("apply %dx%d (%.1f MP), %d thread(s): best %.1f ms, first %.1f ms, %.1f MP/s\n", w, h, n / 1e6, nt,
		       best, first, n / 1e3 / best);
	}
	uint32_t sum = 0;
	for (size_t i = 0; i < n; i++)
		sum = sum * 31 + px[i];
	printf("checksum %08x\n", sum);
	cam_lut_free(l);
	return 0;
}
#endif
