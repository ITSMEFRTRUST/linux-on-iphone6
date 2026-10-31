/* cam_conv.c: known colours, every rotation / mirror against a per-pixel reference, cover-crop bounds.
 * `cam_conv_test bench` (build with -O2, no sanitizers) times the viewfinder conversions at the phone's size and,
 * for calibration, a planar yuv_blit loop at 750×1294 (7 ms on the phone, DESIGN.md). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cam.h"

/* cam_conv_ref.c: the converter before the staged rewrite */
typedef struct ref_conv ref_conv_t;
ref_conv_t *ref_new(void);
bool ref_conv_init(ref_conv_t *c, int sw, int sh, int stride, cam_orient_t o, int dw, int dh);
void ref_conv_free(ref_conv_t *c);
void ref_convert(ref_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride);

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static int ch(uint32_t p, int s) { return (int)(p >> s & 0xff); }
static int near(uint32_t p, int r, int g, int b)
{
	return abs(ch(p, 16) - r) <= 2 && abs(ch(p, 8) - g) <= 2 && abs(ch(p, 0) - b) <= 2;
}

static void colours(void)
{
	cam_conv_set_tone(0, 100);	/* (plain BT.601: the reference colours) */
	struct { int y, u, v; bool full; int r, g, b; const char *name; } t[] = {
		{ 16, 128, 128, false, 0, 0, 0, "video black" },
		{ 235, 128, 128, false, 255, 255, 255, "video white" },
		{ 126, 128, 128, false, 128, 128, 128, "video grey" },
		{ 81, 90, 240, false, 255, 0, 0, "video red" },
		{ 145, 54, 34, false, 0, 255, 0, "video green" },
		{ 41, 240, 110, false, 0, 0, 255, "video blue" },
		{ 0, 128, 128, true, 0, 0, 0, "full black" },
		{ 255, 128, 128, true, 255, 255, 255, "full white" },
		{ 76, 85, 255, true, 254, 0, 0, "full red" },
		{ 150, 44, 21, true, 0, 255, 1, "full green" },
		{ 29, 255, 107, true, 0, 0, 254, "full blue" },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		uint32_t p = cam_yuv_px(t[i].y, t[i].u, t[i].v, t[i].full);
		CHECK(near(p, t[i].r, t[i].g, t[i].b), "%s: got %d,%d,%d", t[i].name, ch(p, 16), ch(p, 8), ch(p, 0));
		CHECK(p >> 24 == 0xff, "%s: alpha", t[i].name);
	}
	/* through the converter: a 4×2 frame of two 2×2 blocks, red | blue */
	uint8_t f[64 * 2 + 64] = { 0 };
	for (int r = 0; r < 2; r++)
		for (int x = 0; x < 4; x++)
			f[r * 64 + x] = x < 2 ? 81 : 41;
	uint8_t *uv = f + 64 * 2;
	uv[0] = 90, uv[1] = 240, uv[2] = 240, uv[3] = 110;
	cam_conv_t c;
	uint32_t d[8];
	CHECK(cam_conv_init(&c, 4, 2, 64, (cam_orient_t){ 0 }, 4, 2), "init 4x2");
	cam_convert(&c, f, uv, d, 4);
	CHECK(near(d[0], 255, 0, 0) && near(d[5], 255, 0, 0), "converter red");
	CHECK(near(d[3], 0, 0, 255) && near(d[6], 0, 0, 255), "converter blue");
	cam_conv_free(&c);
}

/* rotated picture pixel (x, y) → source pixel, from the definition of a clockwise rotation + mirror */
static void ref_xy(int rot, bool mirror, int W, int H, int x, int y, int *sx, int *sy)
{
	int ow = rot == 90 || rot == 270 ? H : W;
	if (mirror)
		x = ow - 1 - x;
	switch (rot) {
	case 0: *sx = x, *sy = y; break;
	case 90: *sx = y, *sy = H - 1 - x; break;
	case 180: *sx = W - 1 - x, *sy = H - 1 - y; break;
	default: *sx = W - 1 - y, *sy = x; break;
	}
}

static void orientations(void)
{
	const int W = 70, H = 38, S = 128;	/* not tile multiples: the edges of the transpose */
	uint8_t *f = malloc(S * H * 3 / 2);
	srand(7);
	for (int i = 0; i < S * H * 3 / 2; i++)
		f[i] = (uint8_t)rand();
	const uint8_t *uv = f + S * H;
	static const int rots[] = { 0, 90, 180, 270 };
	for (int ri = 0; ri < 4; ri++)
		for (int m = 0; m < 2; m++) {
			int rot = rots[ri], ow = rot % 180 ? H : W, oh = rot % 180 ? W : H;
			cam_conv_t c;
			uint32_t *d = malloc(sizeof(uint32_t) * ow * oh);
			CHECK(cam_conv_init(&c, W, H, S, (cam_orient_t){ rot, m, m }, ow, oh), "init rot %d", rot);
			cam_convert(&c, f, uv, d, ow);
			int bad = 0;
			for (int y = 0; y < oh; y++)
				for (int x = 0; x < ow; x++) {
					int sx, sy;
					ref_xy(rot, m, W, H, x, y, &sx, &sy);
					const uint8_t *p = uv + (sy / 2) * S + (sx & ~1);
					uint32_t want = cam_yuv_px(f[sy * S + sx], p[0], p[1], m);
					bad += d[y * ow + x] != want;
				}
			CHECK(!bad, "rot %d mirror %d: %d pixels differ", rot, m, bad);
			cam_conv_free(&c);
			free(d);
		}
	free(f);
}

/* cover crop: a 16:9 landscape sensor into the 3:4 viewfinder; odd / tiny / enlarged boxes stay in bounds (ASan) */
static void cover(void)
{
	const int W = 960, H = 540, S = 960;
	uint8_t *f = calloc(S * H * 3 / 2, 1);
	static const int box[][2] = { { 750, 1000 }, { 92, 92 }, { 1, 1 }, { 3, 1333 }, { 1501, 7 }, { 2000, 3000 } };
	for (int r = 0; r < 4; r++)
		for (unsigned b = 0; b < sizeof(box) / sizeof(box[0]); b++) {
			cam_conv_t c;
			int dw = box[b][0], dh = box[b][1];
			uint32_t *d = malloc(sizeof(uint32_t) * dw * dh);
			CHECK(cam_conv_init(&c, W, H, S, (cam_orient_t){ r * 90, r & 1, 0 }, dw, dh), "init %dx%d", dw, dh);
			CHECK(c.xa >= 0 && c.xb <= c.ow && c.ya >= 0 && c.yb <= c.oh, "window %dx%d", dw, dh);
			cam_convert(&c, f, f + S * H, d, dw);
			cam_conv_free(&c);
			free(d);
		}
	cam_conv_t c;
	cam_conv_init(&c, W, H, S, (cam_orient_t){ 90, 0, 0 }, 750, 1000);
	CHECK(c.xb - c.xa == 540 && c.yb - c.ya == 720, "960x540 rot 90 → 750x1000: window %dx%d (540x720)",
	      c.xb - c.xa, c.yb - c.ya);
	cam_conv_free(&c);
	free(f);
}

/* the default tone: black and white stay, mid-grey stays, shadows darker / highlights brighter (S-curve), a
 * monotonic curve, colours stronger; the NEON path = the scalar one with it too */
static void tone(void)
{
	cam_conv_set_tone(30, 120);
	CHECK(cam_yuv_px(16, 128, 128, false) == 0xff000000u, "black stays black");
	CHECK(cam_yuv_px(235, 128, 128, false) == 0xffffffffu, "white stays white");
	CHECK(abs(ch(cam_yuv_px(126, 128, 128, false), 8) - 128) <= 1, "mid-grey stays");
	CHECK(ch(cam_yuv_px(71, 128, 128, false), 8) < 64, "a quarter grey gets darker: %d", ch(cam_yuv_px(71, 128, 128, false), 8));
	CHECK(ch(cam_yuv_px(180, 128, 128, false), 8) > 191, "three-quarter grey gets brighter: %d",
	      ch(cam_yuv_px(180, 128, 128, false), 8));
	int last = -1, mono = 1;
	for (int y = 0; y < 256; y++) {
		int g = ch(cam_yuv_px(y, 128, 128, false), 8);
		mono &= g >= last;
		last = g;
	}
	CHECK(mono, "the curve is monotonic");
	uint32_t a = cam_yuv_px(110, 100, 170, false);	/* a muted orange: more saturated */
	cam_conv_set_tone(30, 100);
	uint32_t b = cam_yuv_px(110, 100, 170, false);
	CHECK(ch(a, 16) - ch(a, 0) > ch(b, 16) - ch(b, 0), "saturation 120 %% > 100 %%");
	cam_conv_set_tone(30, 120);
	const int W = 1632, H = 1224, S = 1664;
	uint8_t *f = malloc((size_t)S * H * 3 / 2);
	srand(5);
	for (size_t i = 0; i < (size_t)S * H * 3 / 2; i++)
		f[i] = (uint8_t)rand();
	for (int m = 0; m < 2; m++) {
		cam_conv_t c;
		uint32_t *d = malloc(sizeof(uint32_t) * 750 * 1000), *e = malloc(sizeof(uint32_t) * 750 * 1000);
		CHECK(cam_conv_init(&c, W, H, S, (cam_orient_t){ 90, m, 0 }, 750, 1000), "init");
		cam_convert(&c, f, f + (size_t)S * H, e, 750);
		cam_conv_scalar = true;
		cam_convert(&c, f, f + (size_t)S * H, d, 750);
		cam_conv_scalar = false;
		CHECK(!memcmp(d, e, sizeof(uint32_t) * 750 * 1000), "tone: NEON ≠ scalar (mirror %d)", m);
		cam_conv_free(&c);
		free(d);
		free(e);
	}
	free(f);
}

/* the converter ≈ the old one (±2 levels, plain tone): random frames, every orientation / mirror / range, boxes that
 * shrink, enlarge (repeated rows / columns), odd sizes, strides with and without 64-byte room */
static void same_as_ref(void)
{
	cam_conv_set_tone(0, 100);
	static const struct { int w, h, s; } src[] = {
		{ 70, 38, 128 }, { 64, 48, 64 }, { 70, 38, 70 }, { 1632, 1224, 1664 }, { 1376, 968, 1408 }, { 960, 540, 960 },
		{ 6, 4, 6 },
	};
	static const int box[][2] = { { 750, 1000 }, { 92, 92 }, { 1, 1 }, { 33, 17 }, { 200, 61 }, { 17, 300 } };
	ref_conv_t *r = ref_new();
	srand(11);
	for (unsigned si = 0; si < sizeof(src) / sizeof(src[0]); si++) {
		const int W = src[si].w, H = src[si].h, S = src[si].s;
		uint8_t *f = malloc((size_t)S * H * 3 / 2);
		for (size_t i = 0; i < (size_t)S * H * 3 / 2; i++)	/* (legal video-range values: the new maths clamps
									 * Y < 16 / > 235 before adding the chroma) */
			f[i] = (uint8_t)(16 + rand() % (i < (size_t)S * H ? 220 : 225));
		for (unsigned b = 0; b < sizeof(box) / sizeof(box[0]); b++)
			for (int rot = 0; rot < 360; rot += 90)
				for (int m = 0; m < 4; m++) {
					if (W > 200 && (b > 1 || m & 2))	/* (big frames: the phone's boxes) */
						continue;
					const int dw = box[b][0], dh = box[b][1];
					cam_orient_t o = { rot, m & 1, m >> 1 };
					cam_conv_t c;
					uint32_t *d = malloc(sizeof(uint32_t) * dw * dh), *e = malloc(sizeof(uint32_t) * dw * dh);
					CHECK(cam_conv_init(&c, W, H, S, o, dw, dh) && ref_conv_init(r, W, H, S, o, dw, dh),
					      "init %dx%d → %dx%d", W, H, dw, dh);
					cam_convert(&c, f, f + (size_t)S * H, d, dw);
					ref_convert(r, f, f + (size_t)S * H, e, dw);
					int bad = 0, worst = 0;	/* (16-bit maths: ±1-2 levels from the old 16.16 one) */
					for (int i = 0; i < dw * dh; i++)
						for (int sh = 0; sh < 32; sh += 8) {
							int dd = abs(ch(d[i], sh) - ch(e[i], sh));
							bad += dd > 2;
							worst = dd > worst ? dd : worst;
						}
					CHECK(!bad, "%dx%d (stride %d) rot %d mirror %d full %d → %dx%d: %d channels differ from the old "
					      "converter by > 2 (worst %d)", W, H, S, rot, m & 1, m >> 1, dw, dh, bad, worst);
					/* the NEON path = the scalar one, bit for bit (aarch64; elsewhere both are scalar) */
					cam_convert(&c, f, f + (size_t)S * H, e, dw);
					cam_conv_scalar = true;
					cam_convert(&c, f, f + (size_t)S * H, d, dw);
					cam_conv_scalar = false;
					bad = 0;
					for (int i = 0; i < dw * dh; i++)
						bad += d[i] != e[i];
					CHECK(!bad, "%dx%d rot %d mirror %d full %d → %dx%d: NEON ≠ scalar in %d pixels", W, H, rot,
					      m & 1, m >> 1, dw, dh, bad);
					if (si == 3 && b == 0 && rot == 90 && m == 0)
						printf("cam_conv_test: 1632x1224 → 750x1000 rot 90: gather plan %s\n", c.tbl ? "fits (NEON tbl on aarch64)" : "doesn't fit (scalar)");
					cam_conv_free(&c);
					ref_conv_free(r);
					free(d);
					free(e);
				}
		free(f);
	}
	free(r);
}

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

/* a planar 4:2:0 blit (no scaling), the loop the phone timed at 7 ms for 750×1294 */
static void yt_blit(const uint8_t *py, const uint8_t *pcb, const uint8_t *pcr, int fw, int fh, uint32_t *dst)
{
	for (int y = 0; y + 1 < fh; y += 2) {
		const uint8_t *y0 = py + (size_t)y * fw, *y1 = y0 + fw;
		const uint8_t *cb = pcb + (size_t)(y / 2) * (fw / 2), *cr = pcr + (size_t)(y / 2) * (fw / 2);
		uint32_t *d0 = dst + (size_t)y * fw, *d1 = d0 + fw;
		for (int x = 0; x + 1 < fw; x += 2) {
			int u = cb[x >> 1] - 128, v = cr[x >> 1] - 128;
			int rr = (v * 104597) >> 16, gg = (u * 25674 + v * 53278) >> 16, bb = (u * 132201) >> 16;
#define YPX(d, yy) do { int l = (((int)(yy) - 16) * 76309) >> 16, r_ = l + rr, g_ = l - gg, b_ = l + bb; \
	r_ = r_ < 0 ? 0 : r_ > 255 ? 255 : r_; g_ = g_ < 0 ? 0 : g_ > 255 ? 255 : g_; b_ = b_ < 0 ? 0 : b_ > 255 ? 255 : b_; \
	(d) = 0xff000000u | (uint32_t)r_ << 16 | (uint32_t)g_ << 8 | (uint32_t)b_; } while (0)
			YPX(d0[x], y0[x]); YPX(d0[x + 1], y0[x + 1]); YPX(d1[x], y1[x]); YPX(d1[x + 1], y1[x + 1]);
#undef YPX
		}
	}
}

static void bench(void)
{
	enum { N = 200, DW = 750, DH = 1000 };
	uint32_t *d = malloc(sizeof(uint32_t) * 750 * 1334);
	uint8_t *f = malloc(1024 * 1088 * 3 / 2);
	for (int i = 0; i < 1024 * 1088 * 3 / 2; i++)
		f[i] = (uint8_t)(i * 7 + (i >> 9));
	double t0 = now_ms();
	for (int k = 0; k < N; k++)
		yt_blit(f, f + 750 * 1294, f + 750 * 1294 * 5 / 4, 750, 1294, d);
	double yt = (now_ms() - t0) / N;
	printf("bench: yuv_blit 750x1294 (phone: 7 ms) %.2f ms  → phone/PC factor %.1f\n", yt, 7.0 / yt);
	static const struct { int w, h, s, rot; } m[] = {
		{ 960, 540, 960, 90 }, { 640, 480, 640, 90 }, { 1920, 1080, 1920, 90 }, { 540, 960, 576, 0 },
	};
	for (unsigned i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
		cam_conv_t c;
		cam_conv_init(&c, m[i].w, m[i].h, m[i].s, (cam_orient_t){ m[i].rot, 0, 0 }, DW, DH);
		t0 = now_ms();
		for (int k = 0; k < N; k++)
			cam_convert(&c, f, f + m[i].s * m[i].h, d, DW);
		double ms = (now_ms() - t0) / N;
		printf("bench: %dx%d rot %d → %dx%d: %.2f ms per frame on this PC, ≈ %.1f ms on the phone "
		       "(%.0f %% of a core at 30 fps)\n", m[i].w, m[i].h, m[i].rot, DW, DH, ms, ms * 7.0 / yt,
		       ms * 7.0 / yt * 30 / 10);
		cam_conv_free(&c);
	}
	/* the phone's presets (4:3 PHOTO_VIEW): old converter vs this one, and what each reads from the frame */
	static const struct { int w, h, s; bool mirror; const char *what; } p[] = {
		{ 1632, 1224, 1664, false, "rear PHOTO_VIEW" }, { 1376, 968, 1408, true, "front PHOTO_VIEW" },
		{ 1000, 750, 1024, false, "ISP-scaled 1000x750 (kernel patch 3)" },
	};
	uint8_t *g = malloc(1664 * 1224 * 3 / 2);
	for (int i = 0; i < 1664 * 1224 * 3 / 2; i++)
		g[i] = (uint8_t)(i * 13 + (i >> 11));
	ref_conv_t *r = ref_new();
	for (unsigned i = 0; i < sizeof(p) / sizeof(p[0]); i++) {
		cam_orient_t o = { 90, p[i].mirror, false };
		cam_conv_t c;
		cam_conv_init(&c, p[i].w, p[i].h, p[i].s, o, DW, DH);
		ref_conv_init(r, p[i].w, p[i].h, p[i].s, o, DW, DH);
		const uint8_t *uv = g + (size_t)p[i].s * p[i].h;
		double best_old = 1e9, best_new = 1e9;
		for (int rep = 0; rep < 5; rep++) {
			t0 = now_ms();
			for (int k = 0; k < N / 4; k++)
				ref_convert(r, g, uv, d, DW);
			double a = (now_ms() - t0) / (N / 4);
			t0 = now_ms();
			for (int k = 0; k < N / 4; k++)
				cam_convert(&c, g, uv, d, DW);
			double b = (now_ms() - t0) / (N / 4);
			best_old = a < best_old ? a : best_old;
			best_new = b < best_new ? b : best_new;
		}
		/* frame reads: old = the transposed window byte by byte; new = the copied rows (64-byte loads) */
		long old_rd = (long)(c.xb - c.xa) * (c.yb - c.ya) * 3 / 2, rows = 0, crows = 0, lr = -1, lc = -1;
		for (int x = 0; x < DW; x++) {	/* (as convert_t: per block of K) */
			if (x % CAM_CONV_K == 0)
				lr = lc = -1;
			rows += c.srow[x] != lr;
			crows += c.srow[x] / 2 != lc;
			lr = c.srow[x], lc = c.srow[x] / 2;
		}
		long new_rd = (rows + crows) * (long)(c.c1 - c.c0);
		printf("bench: %s %dx%d → %dx%d: old %.2f ms, new %.2f ms on this PC (≈ %.1f / %.1f ms on the phone if "
		       "cached); frame reads old %ld × 1 B, new %.2f MB in %ld × 64 B\n", p[i].what, p[i].w, p[i].h, DW, DH,
		       best_old, best_new, best_old * 7.0 / yt, best_new * 7.0 / yt, old_rd, new_rd / 1e6, new_rd / 64);
		cam_conv_free(&c);
		ref_conv_free(r);
	}
	free(r);
	free(g);
	free(d);
	free(f);
}

int main(int argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "bench")) {
		bench();
		return 0;
	}
	colours();
	orientations();
	cover();
	same_as_ref();
	tone();
	printf(fails ? "cam_conv_test: %d FAILED\n" : "cam_conv_test: ok\n", fails);
	return fails != 0;
}
