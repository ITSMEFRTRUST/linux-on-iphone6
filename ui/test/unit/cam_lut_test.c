/* cam_lut.c: identity / linear / hand-checked cubes, a double-precision tetrahedral reference, the .cube parser's
 * edge cases, the filter lookup ($ORCHARD_DATA/luts, missing file = Original) and, when ffmpeg is installed, the
 * result against ffmpeg's lut3d=interp=tetrahedral (max 2 levels apart; ffmpeg truncates, we round).
 * Usage: cam_lut_test WORKDIR [extra .cube files to compare with ffmpeg] */
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cam_lut.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

const char *data_dir(void)
{
	const char *e = getenv("ORCHARD_DATA");
	return e ? e : "/nonexistent";
}

typedef void (*fn_t)(double r, double g, double b, double out[3]);

/* a .cube text of size n from fn (red fastest), into a malloc'd string */
static char *cube_text(int n, fn_t fn, const char *extra)
{
	size_t cap = (size_t)n * n * n * 40 + 512, len = 0;
	char *s = malloc(cap);
	len += (size_t)snprintf(s + len, cap - len, "# test\r\nTITLE \"t\"\r\n%sLUT_3D_SIZE %d\r\n\r\n", extra, n);
	for (int b = 0; b < n; b++)
		for (int g = 0; g < n; g++)
			for (int r = 0; r < n; r++) {
				double o[3];
				fn(r / (n - 1.0), g / (n - 1.0), b / (n - 1.0), o);
				len += (size_t)snprintf(s + len, cap - len, "%.6f %.6f %.6f\r\n", o[0], o[1], o[2]);
			}
	return s;
}

static cam_lut_t *cube(int n, fn_t fn)
{
	char *s = cube_text(n, fn, ""), why[120] = "";
	cam_lut_t *l = cam_lut_parse(s, strlen(s), why, sizeof(why));
	CHECK(l, "parse: %s", why);
	free(s);
	return l;
}

static void f_identity(double r, double g, double b, double o[3]) { o[0] = r, o[1] = g, o[2] = b; }
static void f_swap(double r, double g, double b, double o[3]) { o[0] = g, o[1] = b, o[2] = 1 - r; }
/* smooth, non-linear, crosstalk between channels (a "look") */
static void f_look(double r, double g, double b, double o[3])
{
	double l = 0.3 * r + 0.59 * g + 0.11 * b;
	o[0] = pow(0.8 * r + 0.2 * l, 0.8);
	o[1] = 0.05 + 0.9 * pow(0.7 * g + 0.3 * l, 1.1);
	o[2] = 1 - pow(1 - (0.85 * b + 0.15 * r), 1.3);
}

static uint32_t px(int r, int g, int b) { return 0x5a000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b; }
static int ch(uint32_t p, int k) { return (int)(p >> (16 - 8 * k)) & 0xff; }

/* the reference: tetrahedral interpolation in doubles over the (unquantised) function's grid */
static void ref(int n, fn_t fn, int r, int g, int b, double out[3])
{
	double pos[3] = { r * (n - 1) / 255.0, g * (n - 1) / 255.0, b * (n - 1) / 255.0 }, f[3];
	int i[3];
	for (int k = 0; k < 3; k++) {
		i[k] = (int)pos[k];
		if (i[k] > n - 2)
			i[k] = n - 2;
		f[k] = pos[k] - i[k];
	}
	int ord[3] = { 0, 1, 2 };	/* axes by fraction, largest first */
	for (int a = 0; a < 3; a++)
		for (int c = a + 1; c < 3; c++)
			if (f[ord[c]] > f[ord[a]]) {
				int t = ord[a];
				ord[a] = ord[c], ord[c] = t;
			}
	int at[3] = { i[0], i[1], i[2] };
	double prev[3], cur[3], w = 1;
	fn(at[0] / (n - 1.0), at[1] / (n - 1.0), at[2] / (n - 1.0), prev);
	for (int k = 0; k < 3; k++)
		out[k] = 0;
	for (int s = 0; s < 3; s++) {	/* corner s gets weight f[ord[s-1]] - f[ord[s]] */
		double wk = w - f[ord[s]];
		for (int k = 0; k < 3; k++)
			out[k] += wk * prev[k];
		w = f[ord[s]];
		at[ord[s]]++;
		fn(at[0] / (n - 1.0), at[1] / (n - 1.0), at[2] / (n - 1.0), cur);
		memcpy(prev, cur, sizeof(cur));
	}
	for (int k = 0; k < 3; k++)
		out[k] = (out[k] + w * prev[k]) * 255;
}

static void test_identity(int n)
{
	cam_lut_t *l = cube(n, f_identity);
	if (!l)
		return;
	int bad = 0;
	for (int r = 0; r < 256; r++)
		for (int g = 0; g < 256; g += 5)
			for (int b = 0; b < 256; b += 3) {
				uint32_t p = px(r, g, b), q = p;
				cam_lut_apply(l, &q, 1);
				bad += q != p;
			}
	uint32_t edge[] = { px(255, 255, 255), px(0, 0, 0), px(255, 0, 255), px(1, 254, 128) }, e2[4];
	memcpy(e2, edge, sizeof(edge));
	cam_lut_apply(l, e2, 4);
	CHECK(!bad && !memcmp(edge, e2, sizeof(edge)), "identity %d³: %d pixels changed", n, bad);
	cam_lut_free(l);
}

static void test_swap(void)
{
	cam_lut_t *l = cube(9, f_swap);
	if (!l)
		return;
	int bad = 0;
	for (int i = 0; i < 200000; i++) {
		int r = rand() & 255, g = rand() & 255, b = rand() & 255;
		uint32_t q = px(r, g, b);
		cam_lut_apply(l, &q, 1);
		bad += q != px(g, b, 255 - r);
	}
	CHECK(!bad, "swap: %d of 200000 wrong", bad);
	cam_lut_free(l);
}

/* hand-checked: a 2³ cube, corners chosen freely */
static const double corner[8][3] = { { .1, .2, .3 }, { .9, .1, .0 }, { .0, .8, .1 }, { .7, .7, .2 },
				     { .2, .0, .9 }, { .6, .3, .8 }, { .1, .9, .7 }, { 1, 1, 1 } };
static void f_corners(double r, double g, double b, double o[3])
{
	const double *c = corner[(int)lround(r) | (int)lround(g) << 1 | (int)lround(b) << 2];
	o[0] = c[0], o[1] = c[1], o[2] = c[2];
}

static void test_known(void)
{
	cam_lut_t *l = cube(2, f_corners);
	if (!l)
		return;
	/* corners exactly; the grey diagonal = c000 → c111 only; (255, 128, 0) = halfway c100 → c110 */
	struct { int in[3]; int out[3]; } k[] = {
		{ { 0, 0, 0 }, { 26, 51, 77 } },	/* .1 .2 .3 × 255 = 25.5 → 26 (round half up), 51, 76.5 → 77 */
		{ { 255, 0, 0 }, { 230, 26, 0 } },
		{ { 255, 255, 0 }, { 179, 179, 51 } },
		{ { 255, 255, 255 }, { 255, 255, 255 } },
		{ { 51, 51, 51 }, { 71, 92, 112 } },	/* 0.8 c000 + 0.2 c111: .28 .36 .44 × 255 */
		{ { 255, 128, 0 }, { 204, 102, 26 } },	/* c100 + 128/255 (c110 - c100): .7996 .4012 .1004 */
	};
	for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
		uint32_t q = px(k[i].in[0], k[i].in[1], k[i].in[2]);
		cam_lut_apply(l, &q, 1);
		double rf[3];
		ref(2, f_corners, k[i].in[0], k[i].in[1], k[i].in[2], rf);
		for (int c = 0; c < 3; c++)
			CHECK(abs(ch(q, c) - k[i].out[c]) <= 1 && fabs(ch(q, c) - rf[c]) <= 0.51,
			      "known %d %d %d ch %d: got %d, want %d (ref %.2f)", k[i].in[0], k[i].in[1], k[i].in[2], c,
			      ch(q, c), k[i].out[c], rf[c]);
	}
	uint32_t q = 0x12000000u;	/* the X byte survives */
	cam_lut_apply(l, &q, 1);
	CHECK(q >> 24 == 0x12, "X byte lost");
	cam_lut_free(l);
}

/* every pixel within 0.5 + quantisation of the double reference, for a few sizes */
static void test_vs_ref(int n)
{
	cam_lut_t *l = cube(n, f_look);
	if (!l)
		return;
	double worst = 0;
	for (int i = 0; i < 300000; i++) {
		int r = rand() & 255, g = rand() & 255, b = rand() & 255;
		uint32_t q = px(r, g, b);
		cam_lut_apply(l, &q, 1);
		double rf[3];
		ref(n, f_look, r, g, b, rf);
		for (int c = 0; c < 3; c++)
			worst = fmax(worst, fabs(ch(q, c) - rf[c]));
	}
	CHECK(worst <= 0.6, "vs reference %d³: max error %.3f levels", n, worst);
	printf("cam_lut: %d³ look vs double reference: max %.3f levels\n", n, worst);
	cam_lut_free(l);
}

static void test_parse(void)
{
	char why[120];
	struct { const char *txt; bool ok; } c[] = {
		{ "", false },
		{ "0 0 0\n", false },
		{ "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n", false },	/* 7 of 8 */
		{ "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n1 1 1\n", false },
		{ "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n", false },
		{ "LUT_3D_SIZE 1\n0 0 0\n", false },
		{ "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 x\n1 1 1\n", false },
		{ "LUT_3D_SIZE 2\nDOMAIN_MIN 0 0 0\nDOMAIN_MAX 0 1 1\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n", false },
		{ "TITLE \"x\"\nLUT_3D_SIZE 2\n# c\n  0 0 0\n1e0 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1.0 1 1", true },
	};
	for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
		cam_lut_t *l = cam_lut_parse(c[i].txt, strlen(c[i].txt), why, sizeof(why));
		CHECK(!!l == c[i].ok, "parse case %zu: %s", i, l ? "accepted" : why);
		cam_lut_free(l);
	}
	/* a non-finite DOMAIN / INPUT_RANGE (±1e999 = ±inf: inf / inf = NaN, (int)NaN = UB) is refused; huge finite is ok */
#define CUBE2 "LUT_3D_SIZE 2\n0 0 0\n1 0 0\n0 1 0\n1 1 0\n0 0 1\n1 0 1\n0 1 1\n1 1 1\n"
	struct { const char *txt; bool ok; } d[] = {
		{ "DOMAIN_MIN -1e999 0 0\n" CUBE2, false },
		{ "DOMAIN_MAX 1 1e999 1\n" CUBE2, false },
		{ "DOMAIN_MIN -1e999 -1e999 -1e999\nDOMAIN_MAX 1e999 1e999 1e999\n" CUBE2, false },
		{ "LUT_3D_INPUT_RANGE 0 1e999\n" CUBE2, false },
		{ "LUT_3D_INPUT_RANGE -1e999 1\n" CUBE2, false },
		{ "DOMAIN_MIN -1e300 -1e300 -1e300\nDOMAIN_MAX 1e300 1e300 1e300\n" CUBE2, true },
		{ "LUT_3D_INPUT_RANGE -1e308 1e308\n" CUBE2, true },
	};
	for (size_t i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
		cam_lut_t *l = cam_lut_parse(d[i].txt, strlen(d[i].txt), why, sizeof(why));
		CHECK(!!l == d[i].ok, "domain case %zu: %s", i, l ? "accepted" : why);
		if (l) {	/* (every input lands in the cube: no crash, no UB) */
			uint32_t q[3] = { px(0, 0, 0), px(255, 255, 255), px(1, 128, 254) };
			cam_lut_apply(l, q, 3);
		}
		cam_lut_free(l);
	}
	/* exactly len bytes are read: no NUL after them (ASan: an overread = a crash here) ... */
	const char *t = CUBE2 "1 1 1";	/* (a 9th point after len) */
	size_t tl = strlen(CUBE2) - 1;	/* (without the last newline: the text ends in a digit) */
	char *nz = malloc(tl);
	memcpy(nz, t, tl);
	cam_lut_t *lz = cam_lut_parse(nz, tl, why, sizeof(why));
	CHECK(lz && cam_lut_size(lz) == 2, "unterminated text: %s", lz ? "ok" : why);
	cam_lut_free(lz);
	free(nz);
	/* ... and nothing after them counts (the 9th point is past len) */
	lz = cam_lut_parse(t, strlen(CUBE2), why, sizeof(why));
	CHECK(lz, "text past len was read: %s", why);
	cam_lut_free(lz);
	lz = cam_lut_parse(t, strlen(t), why, sizeof(why));
	CHECK(!lz, "9 points in a 2³ cube accepted");
	cam_lut_free(lz);
	/* keywords cut off by len */
	lz = cam_lut_parse("LUT_3D_SIZE 2", 8, why, sizeof(why));
	CHECK(!lz, "a cut-off LUT_3D_SIZE accepted");
	cam_lut_free(lz);
#undef CUBE2
	/* DOMAIN 0..0.5: input 128 already at the top of the cube (identity → 255) */
	char *s = cube_text(5, f_identity, "DOMAIN_MIN 0 0 0\nDOMAIN_MAX 0.5 0.5 0.5\n");
	cam_lut_t *l = cam_lut_parse(s, strlen(s), why, sizeof(why));
	free(s);
	CHECK(l, "domain: %s", why);
	uint32_t q = px(200, 64, 0);
	cam_lut_apply(l, &q, 1);
	CHECK(ch(q, 0) == 255 && abs(ch(q, 1) - 128) <= 1 && ch(q, 2) == 0, "domain: %06x", q & 0xffffff);
	cam_lut_free(l);
	cam_lut_apply(NULL, &q, 1);	/* Original: nothing, no crash */
	errno = 0;
	CHECK(!cam_lut_load("/nonexistent/x.cube", why, sizeof(why)) && errno == ENOENT, "missing file");
}

static void write_file(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");
	fputs(s, f);
	fclose(f);
}

static void test_filters(const char *dir)
{
	CHECK(cam_filter_from_key("vivid") == CAM_FILTER_VIVID && cam_filter_from_key("original") == 0 &&
	      cam_filter_from_key("bogus") == CAM_FILTER_WARM && cam_filter_from_key(NULL) == CAM_FILTER_WARM,
	      "filter keys");
	CHECK(!strcmp(cam_filter_label(CAM_FILTER_WARM), "WARM"), "label");
	char d[600], p[700];
	snprintf(d, sizeof(d), "%s/data", dir);
	mkdir(d, 0755);
	snprintf(p, sizeof(p), "%s/luts", d);
	mkdir(p, 0755);
	setenv("ORCHARD_DATA", d, 1);
	snprintf(p, sizeof(p), "%s/luts/warm.cube", d);
	char *s = cube_text(5, f_swap, "");
	write_file(p, s);
	free(s);
	snprintf(p, sizeof(p), "%s/luts/vivid.cube", d);
	remove(p);	/* Vivid missing → Original */
	CHECK(!cam_lut_for(CAM_FILTER_ORIGINAL), "original has a table");
	/* peek: NULL at first, the table once the background load is done */
	const cam_lut_t *t = NULL;
	for (int i = 0; i < 400 && !(t = cam_lut_peek(CAM_FILTER_WARM)); i++)
		usleep(5000);
	CHECK(t && cam_lut_size(t) == 5 && cam_lut_for(CAM_FILTER_WARM) == t, "warm from $ORCHARD_DATA/luts");
	cam_filter_set(CAM_FILTER_VIVID);
	uint32_t a = px(10, 20, 30), th = a;
	cam_lut_photo(&a, 1);
	CHECK(a == px(10, 20, 30), "missing LUT must leave the photo alone: %06x", a & 0xffffff);
	cam_filter_set(CAM_FILTER_WARM);
	a = px(10, 20, 30);
	cam_lut_photo(&a, 1);
	cam_filter_set(CAM_FILTER_ORIGINAL);	/* changed after the shot: the thumbnail still matches the photo */
	cam_lut_thumb(&th, 1);
	CHECK(a == px(20, 30, 245) && th == a, "warm photo %06x thumb %06x", a & 0xffffff, th & 0xffffff);
	CHECK(cam_filter_get() == CAM_FILTER_ORIGINAL, "filter get");
	/* a big picture goes through the two-thread path: same result as pixel by pixel */
	size_t n = (1u << 20) + 77;
	uint32_t *big = malloc(n * 4), *one = malloc(n * 4);
	for (size_t i = 0; i < n; i++)
		big[i] = one[i] = px(rand() & 255, rand() & 255, (int)(i & 255));
	cam_lut_apply(t, big, n);
	for (size_t i = 0; i < n; i += 4096)
		cam_lut_apply(t, one + i, n - i < 4096 ? n - i : 4096);
	CHECK(!memcmp(big, one, n * 4), "threaded apply differs");
	free(big), free(one);
}

/* ---------- vs ffmpeg ---------- */

static int read_ppm(const char *path, uint8_t **rgb, int *w, int *h)
{
	FILE *f = fopen(path, "rb");
	int mx;
	if (!f || fscanf(f, "P6 %d %d %d", w, h, &mx) != 3 || mx != 255 || fgetc(f) == EOF) {
		if (f)
			fclose(f);
		return -1;
	}
	size_t n = (size_t)*w * *h * 3;
	*rgb = malloc(n);
	int ok = fread(*rgb, 1, n, f) == n;
	fclose(f);
	return ok ? 0 : -1;
}

static void vs_ffmpeg(const char *dir, const char *cube_path, const char *name)
{
	char why[200], in[700], out[700], cmd[2400];
	cam_lut_t *l = cam_lut_load(cube_path, why, sizeof(why));
	CHECK(l, "%s", why);
	if (!l)
		return;
	const int w = 512, h = 512;
	snprintf(in, sizeof(in), "%s/lut_in.ppm", dir);
	snprintf(out, sizeof(out), "%s/lut_ff.ppm", dir);
	FILE *f = fopen(in, "wb");
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	uint32_t *p = malloc(sizeof(uint32_t) * w * h);
	for (int y = 0; y < h; y++)	/* top half: smooth ramps (all 256 levels per channel), bottom: random */
		for (int x = 0; x < w; x++) {
			int r, g, b;
			if (y < h / 2)
				r = x / 2, g = (y * 2) & 255, b = (x + y) & 255;
			else
				r = rand() & 255, g = rand() & 255, b = rand() & 255;
			p[y * w + x] = px(r, g, b);
			fputc(r, f), fputc(g, f), fputc(b, f);
		}
	fclose(f);
	snprintf(cmd, sizeof(cmd),
		 "ffmpeg -nostdin -v error -y -i '%s' -vf \"lut3d=file='%s':interp=tetrahedral\" -pix_fmt rgb24 '%s'", in,
		 cube_path, out);
	CHECK(system(cmd) == 0, "ffmpeg failed: %s", cmd);
	uint8_t *ff = NULL;
	int fw, fh;
	if (read_ppm(out, &ff, &fw, &fh) == 0 && fw == w && fh == h) {
		cam_lut_apply(l, p, (size_t)w * h);
		int worst = 0;
		long sum = 0, over1 = 0;
		for (int i = 0; i < w * h; i++)
			for (int c = 0; c < 3; c++) {
				int d = abs(ch(p[i], c) - ff[i * 3 + c]);
				worst = d > worst ? d : worst, sum += d, over1 += d > 1;
			}
		printf("cam_lut: %s vs ffmpeg lut3d tetrahedral: max %d, mean %.3f levels, %ld of %d values > 1\n", name,
		       worst, sum / (3.0 * w * h), over1, 3 * w * h);
		CHECK(worst <= 2, "%s vs ffmpeg: max abs error %d", name, worst);
	} else {
		CHECK(0, "can't read ffmpeg's output");
	}
	free(ff);
	free(p);
	cam_lut_free(l);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "/tmp";
	srand(1);
	test_identity(33);
	test_identity(17);
	test_identity(2);
	test_swap();
	test_known();
	test_vs_ref(2);
	test_vs_ref(17);
	test_vs_ref(33);
	test_parse();
	test_filters(dir);
	if (system("command -v ffmpeg > /dev/null 2>&1") == 0) {
		char path[700];
		snprintf(path, sizeof(path), "%s/look33.cube", dir);
		char *s = cube_text(33, f_look, "");
		write_file(path, s);
		free(s);
		vs_ffmpeg(dir, path, "generated 33³ look");
		for (int i = 2; i < argc; i++)
			if (access(argv[i], R_OK) == 0)
				vs_ffmpeg(dir, argv[i], strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i]);
	} else {
		printf("cam_lut: ffmpeg comparison skipped (no ffmpeg)\n");
	}
	if (fails)
		fprintf(stderr, "cam_lut_test: %d failure(s)\n", fails);
	else
		printf("cam_lut_test ok\n");
	return fails != 0;
}
