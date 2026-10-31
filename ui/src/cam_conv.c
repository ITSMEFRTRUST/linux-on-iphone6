/* NV12 → XRGB8888 for the Camera viewfinder, thumbnails and saved photos (see cam.h). BT.601, video or full range,
 * nearest neighbour, plus the base tone (cam_conv_set_tone: an S-curve on the luma + a chroma gain, on by default:
 * the ISP's output looks flat / washed out next to iOS's).
 *
 * Cost model (phone, 2026-10-21: the camvf converter took 10-11 ms per 750 × 1000 frame from a cached mapping, the
 * same for the rear 1632 × 1224 and the front 1376 × 968 source, so it was per output pixel, ~19 cycles each: a
 * scalar two-load gather from 16 rows per pixel, a non-inlined 16-pixel colour call with spills, 32-bit maths).
 * Now every source row the picture uses is converted along the row, 16 output samples at a time:
 *   - the 16 Y samples come out of one 32-byte window of the row with one NEON tbl, the 16 CbCr pairs out of a
 *     64-byte window with two (index tables made once in cam_conv_init: nearest neighbour = fixed positions);
 *   - the tone curve is a 256-entry byte table (4 tbl / tbx), the colour maths is 16-bit, 8 lanes, saturating;
 *   - vst4 writes the 16 pixels; rotated pictures (90 / 270: the landscape sensors in the portrait viewfinder) go
 *     in blocks of CAM_CONV_K source rows (= destination columns) into a small row buffer that is then transposed
 *     into the destination 4 × 4 pixels at a time.
 * A window that doesn't fit (shrinking more than 2×: thumbnails) or a build without NEON takes the scalar path,
 * which computes exactly the same numbers (cam_conv_test compares them on aarch64).
 *
 * The source (the ISP's buffer: write-combined, or cacheable with mmap_cached) is only read row by row in 64-byte
 * loads (copy_in), and only the rows the picture uses. */
#include <stdlib.h>
#include <string.h>
#include "cam.h"

#if defined(__aarch64__) && defined(__ARM_NEON) && __has_include(<arm_neon.h>)
#include <arm_neon.h>
#define CONV_NEON 1
#else
#define CONV_NEON 0
#ifdef __aarch64__
#warning "cam_conv.c without NEON (no arm_neon.h): the viewfinder converts on the slow scalar path"
#endif
#endif

#define CHUNK 16	/* output samples per NEON step */
#define JRUN 256	/* rotated: destination rows converted per pass (K × JRUN pixels = 16 KB, stays in L1) */
#define PAD 64		/* bytes after each staged row (the 32 / 64-byte windows may reach past its end) */

bool cam_conv_scalar;

/* ---------- tone + colour ---------- */

/* contrast % (S-curve strength, 0 = none) and saturation % (100 = as the ISP gives it). Default 30 / 120 (the plain
 * ISP picture looked a bit washed out); ORCHARD_CAMERA_TONE=contrast,saturation overrides, "0,100" = the plain ISP picture. */
static int tone_contrast = -1, tone_sat = 120;

void cam_conv_set_tone(int contrast, int sat)
{
	tone_contrast = contrast < 0 ? 0 : contrast > 100 ? 100 : contrast;
	tone_sat = sat < 0 ? 0 : sat > 200 ? 200 : sat;
}

static void tone_get(int *contrast, int *sat)
{
	if (tone_contrast < 0) {	/* first use */
		const char *e = getenv("ORCHARD_CAMERA_TONE");
		int c = 30, s = 120;
		if (e && *e) {
			char *end;
			c = (int)strtol(e, &end, 10);
			s = *end == ',' ? atoi(end + 1) : 100;
		}
		cam_conv_set_tone(c, s);
	}
	*contrast = tone_contrast, *sat = tone_sat;
}

typedef struct coefs {
	uint8_t lut[256];	/* Y → luma 0..255 (range expansion + the S-curve) */
	int16_t cr, cg1, cg2, cb;	/* chroma → RGB, 6-bit fixed point (BT.601, range, saturation) */
} coefs_t;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void coefs_make(coefs_t *k, bool full)
{
	int contrast, sat;
	tone_get(&contrast, &sat);
	const double a = contrast / 100.0, s = sat / 100.0;
	for (int y = 0; y < 256; y++) {
		double x = full ? y / 255.0 : (y - 16) / 219.0;
		x = x < 0 ? 0 : x > 1 ? 1 : x;
		x += a * (x * x * (3 - 2 * x) - x);	/* smoothstep blend: mid slope 1 + a / 2, ends 1 - a */
		k->lut[y] = (uint8_t)clampi((int)(x * 255 + 0.5), 0, 255);
	}
	/* R = Y' + cr·V', G = Y' - cg1·U' - cg2·V', B = Y' + cb·U'; video range scales chroma by 255/224 */
	const double r = full ? 1.402 : 1.596, g1 = full ? 0.344136 : 0.391762, g2 = full ? 0.714136 : 0.812968,
		     b = full ? 1.772 : 2.017232;
	k->cr = (int16_t)clampi((int)(r * s * 64 + 0.5), 0, 255);
	k->cg1 = (int16_t)clampi((int)(g1 * s * 64 + 0.5), 0, 255);
	k->cg2 = (int16_t)clampi((int)(g2 * s * 64 + 0.5), 0, 255);
	k->cb = (int16_t)clampi((int)(b * s * 64 + 0.5), 0, 255);
}

static inline int sat16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }
static inline int out8(int v) { return clampi((v + 32) >> 6, 0, 255); }	/* = NEON sqrshrun #6 */

/* one pixel, exactly the NEON lanes' arithmetic */
static inline uint32_t px(const coefs_t *k, int y, int u, int v)
{
	const int l = k->lut[y] << 6;
	u -= 128, v -= 128;
	const int r = sat16(l + (int16_t)(v * k->cr));
	const int g = sat16(l - (int16_t)(u * k->cg1 + v * k->cg2));
	const int b = sat16(l + (int16_t)(u * k->cb));
	return 0xff000000u | (uint32_t)out8(r) << 16 | (uint32_t)out8(g) << 8 | (uint32_t)out8(b);
}

uint32_t cam_yuv_px(int y, int u, int v, bool full)
{
	coefs_t k;
	coefs_make(&k, full);
	return px(&k, y, u, v);
}

/* ---------- source rows ---------- */

/* n bytes out of the frame: 64 bytes per iteration in 16-byte vector loads (aarch64: two ldp q), the fewest bus
 * reads from a write-combined mapping (musl's memcpy moves 16 bytes per ldp of x registers) */
typedef uint8_t v16 __attribute__((vector_size(16), aligned(1)));
static void copy_in(uint8_t *restrict d, const uint8_t *restrict s, size_t n)
{
	for (; n >= 64; n -= 64, s += 64, d += 64) {
		v16 a = *(const v16 *)s, b = *(const v16 *)(s + 16), e = *(const v16 *)(s + 32), f = *(const v16 *)(s + 48);
		*(v16 *)d = a, *(v16 *)(d + 16) = b, *(v16 *)(d + 32) = e, *(v16 *)(d + 48) = f;
	}
	memcpy(d, s, n);
}

/* samples [j0, j1) of one copied source row (yr: Y, cr: its CbCr row) → out[0 .. j1 - j0) */
static void row_px(const cam_conv_t *c, const uint8_t *yr, const uint8_t *cr, uint32_t *out, int j0, int j1)
{
	const coefs_t *k = c->k;
	int j = j0;
#if CONV_NEON
	if (c->tbl && !cam_conv_scalar && j0 % CHUNK == 0 && j1 - j0 >= CHUNK) {
		const uint8x16x4_t L0 = vld1q_u8_x4(k->lut), L1 = vld1q_u8_x4(k->lut + 64), L2 = vld1q_u8_x4(k->lut + 128),
				   L3 = vld1q_u8_x4(k->lut + 192);
		const int16x8_t kr = vdupq_n_s16(k->cr), kg1 = vdupq_n_s16(k->cg1), kg2 = vdupq_n_s16(k->cg2),
				kb = vdupq_n_s16(k->cb);
		const uint8x16_t c64 = vdupq_n_u8(64), c128 = vdupq_n_u8(128), c192 = vdupq_n_u8(192);
		const uint8x8_t h128 = vdup_n_u8(128);
		for (; j + CHUNK <= j1; j += CHUNK, out += CHUNK) {
			const int q = j / CHUNK;
			const uint8x16_t yv = vqtbl2q_u8(vld1q_u8_x2(yr + c->ybase[q]), vld1q_u8(c->yidx + (size_t)q * 16));
			uint8x16_t t = vqtbl4q_u8(L0, yv);
			t = vqtbx4q_u8(t, L1, vsubq_u8(yv, c64));
			t = vqtbx4q_u8(t, L2, vsubq_u8(yv, c128));
			t = vqtbx4q_u8(t, L3, vsubq_u8(yv, c192));
			const uint8x16x4_t w = vld1q_u8_x4(cr + c->cbase[q]);
			const uint8x16_t p0 = vqtbl4q_u8(w, vld1q_u8(c->cidx + (size_t)q * 32)),
					 p1 = vqtbl4q_u8(w, vld1q_u8(c->cidx + (size_t)q * 32 + 16));
			const uint8x16_t U = vuzp1q_u8(p0, p1), V = vuzp2q_u8(p0, p1);
			uint8x16x4_t o;
			uint8x8_t rr[2], gg[2], bb[2];
			for (int h = 0; h < 2; h++) {
				const uint8x8_t th = h ? vget_high_u8(t) : vget_low_u8(t);
				const int16x8_t l = vreinterpretq_s16_u16(vshll_n_u8(th, 6));
				const int16x8_t u = vreinterpretq_s16_u16(vsubl_u8(h ? vget_high_u8(U) : vget_low_u8(U), h128));
				const int16x8_t v = vreinterpretq_s16_u16(vsubl_u8(h ? vget_high_u8(V) : vget_low_u8(V), h128));
				rr[h] = vqrshrun_n_s16(vqaddq_s16(l, vmulq_s16(v, kr)), 6);
				gg[h] = vqrshrun_n_s16(vqsubq_s16(l, vmlaq_s16(vmulq_s16(u, kg1), v, kg2)), 6);
				bb[h] = vqrshrun_n_s16(vqaddq_s16(l, vmulq_s16(u, kb)), 6);
			}
			o.val[0] = vcombine_u8(bb[0], bb[1]);
			o.val[1] = vcombine_u8(gg[0], gg[1]);
			o.val[2] = vcombine_u8(rr[0], rr[1]);
			o.val[3] = vdupq_n_u8(0xff);
			vst4q_u8((uint8_t *)out, o);
		}
	}
#endif
	for (; j < j1; j++, out++) {
		const uint8_t *p = cr + c->co[j];
		*out = px(k, yr[c->yo[j]], p[0], p[1]);
	}
}

/* ---------- set-up ---------- */

void cam_conv_free(cam_conv_t *c)
{
	free(c->srow);
	free(c->yo);
	free(c->co);
	free(c->stage);
	free(c->tmp);
	free(c->ybase);
	free(c->cbase);
	free(c->yidx);
	free(c->cidx);
	free(c->k);
	memset(c, 0, sizeof(*c));
}

/* destination i of n → source index in [lo, lo + len) (centre of the destination pixel), optionally flipped
 * within the plane of size full */
static int map1(int i, int n, int lo, int len, bool flip, int full)
{
	int s = lo + (int)(((int64_t)(2 * i + 1) * len) / (2 * n));
	if (s >= lo + len)
		s = lo + len - 1;
	return flip ? full - 1 - s : s;
}

/* the NEON gather plan: per CHUNK samples a window start + the samples' byte positions in it (Y: 32-byte window,
 * CbCr pairs: 64-byte). false = some chunk doesn't fit (shrinking > ~2×): scalar */
static bool plan(cam_conv_t *c, int n)
{
	const int nq = n / CHUNK;
	if (!nq)
		return false;
	c->ybase = malloc(sizeof(int) * nq);
	c->cbase = malloc(sizeof(int) * nq);
	c->yidx = malloc((size_t)nq * 16);
	c->cidx = malloc((size_t)nq * 32);
	if (!c->ybase || !c->cbase || !c->yidx || !c->cidx)
		return false;
	for (int q = 0; q < nq; q++) {
		int ylo = 1 << 30, yhi = -1, clo = 1 << 30, chi = -1;
		for (int i = q * CHUNK; i < (q + 1) * CHUNK; i++) {
			ylo = c->yo[i] < ylo ? c->yo[i] : ylo;
			yhi = c->yo[i] > yhi ? c->yo[i] : yhi;
			clo = c->co[i] < clo ? c->co[i] : clo;
			chi = c->co[i] + 1 > chi ? c->co[i] + 1 : chi;
		}
		if (yhi - ylo >= 32 || chi - clo >= 64)
			return false;
		c->ybase[q] = ylo, c->cbase[q] = clo;
		for (int i = 0; i < CHUNK; i++) {
			c->yidx[q * 16 + i] = (uint8_t)(c->yo[q * CHUNK + i] - ylo);
			c->cidx[q * 32 + 2 * i] = (uint8_t)(c->co[q * CHUNK + i] - clo);
			c->cidx[q * 32 + 2 * i + 1] = (uint8_t)(c->co[q * CHUNK + i] - clo + 1);
		}
	}
	return true;
}

bool cam_conv_init(cam_conv_t *c, int sw, int sh, int stride, cam_orient_t o, int dw, int dh)
{
	memset(c, 0, sizeof(*c));
	if (sw < 2 || sh < 2 || (sw | sh) & 1 || stride < sw || dw < 1 || dh < 1)
		return false;
	c->sw = sw, c->sh = sh, c->stride = stride, c->o = o, c->dw = dw, c->dh = dh;
	int rot = ((o.rot % 360) + 360) % 360;
	c->transpose = rot == 90 || rot == 270;
	c->flipx = rot == 90 || rot == 180;
	c->flipy = rot == 180 || rot == 270;
	if (o.mirror)
		c->flipx = !c->flipx;
	c->ow = c->transpose ? sh : sw;
	c->oh = c->transpose ? sw : sh;
	/* cover: the source window with the box's aspect, centred */
	int cw = c->ow, ch = c->oh;
	if ((int64_t)dw * c->oh >= (int64_t)dh * c->ow)
		ch = (int)((int64_t)c->ow * dh / dw);
	else
		cw = (int)((int64_t)c->oh * dw / dh);
	if (cw < 1)
		cw = 1;
	if (ch < 1)
		ch = 1;
	int x0 = (c->ow - cw) / 2, y0 = (c->oh - ch) / 2;
	/* the rotated picture's column / row of each destination column / row */
	int *px_ = malloc(sizeof(int) * dw), *py = malloc(sizeof(int) * dh);
	if (!px_ || !py) {
		free(px_);
		free(py);
		return false;
	}
	int xa = c->ow, xb = 0, ya = c->oh, yb = 0;
	for (int x = 0; x < dw; x++) {
		int s = px_[x] = map1(x, dw, x0, cw, c->flipx, c->ow);
		xa = s < xa ? s : xa;
		xb = s + 1 > xb ? s + 1 : xb;
	}
	for (int y = 0; y < dh; y++) {
		int s = py[y] = map1(y, dh, y0, ch, c->flipy, c->oh);
		ya = s < ya ? s : ya;
		yb = s + 1 > yb ? s + 1 : yb;
	}
	c->xa = xa & ~1, c->xb = (xb + 1) & ~1, c->ya = ya & ~1, c->yb = (yb + 1) & ~1;
	/* source rows ↔ one destination axis, source columns (offsets into a copied row) ↔ the other. Rotated: a
	 * picture column is a source row, a picture row a source column; else the other way round. */
	int *rows = c->transpose ? px_ : py;
	int *cols = c->transpose ? py : px_, ncols = c->transpose ? dh : dw;
	int ca = c->transpose ? c->ya : c->xa, cb = c->transpose ? c->yb : c->xb;
	/* the copied span: 64-byte aligned when the row's stride has room for it (the phone's: stride = width
	 * rounded up to 64) — whole bus bursts */
	c->c0 = ca & ~63;
	c->c1 = (cb + 63) & ~63;
	if (c->c1 > stride)
		c->c0 = ca, c->c1 = cb;
	const int span = c->c1 - c->c0, nstage = c->transpose ? CAM_CONV_K : 1;
	c->srow = rows;
	c->yo = malloc(sizeof(int) * ncols);
	c->co = malloc(sizeof(int) * ncols);
	c->stage = calloc((size_t)(span + PAD) * nstage * 2, 1);
	c->k = malloc(sizeof(coefs_t));
	if (c->transpose)
		c->tmp = malloc(sizeof(uint32_t) * CAM_CONV_K * JRUN);
	if (!c->yo || !c->co || !c->stage || !c->k || (c->transpose && !c->tmp)) {
		free(cols);
		goto fail;
	}
	for (int i = 0; i < ncols; i++) {
		c->yo[i] = cols[i] - c->c0;
		c->co[i] = (cols[i] & ~1) - c->c0;
	}
	free(cols);
	coefs_make(c->k, o.full_range);
	c->tbl = plan(c, ncols);
	return true;
fail:
	cam_conv_free(c);
	return false;
}

/* ---------- conversion ---------- */

/* tmp (n rows of JRUN pixels) → destination columns x0 .. x0 + n - 1, rows j0 .. j0 + m - 1 */
static void transpose_out(const uint32_t *tmp, int n, uint32_t *dst, size_t ds, int x0, int j0, int m)
{
	int j = 0;
#if CONV_NEON
	if (n == CAM_CONV_K && !cam_conv_scalar)
		for (; j + 4 <= m; j += 4) {
			uint32_t *d = dst + (size_t)(j0 + j) * ds + x0;
			for (int k = 0; k < CAM_CONV_K; k += 4) {
				const uint32_t *t = tmp + (size_t)k * JRUN + j;
				const uint32x4_t a = vld1q_u32(t), b = vld1q_u32(t + JRUN), e = vld1q_u32(t + 2 * JRUN),
						 f = vld1q_u32(t + 3 * JRUN);
				const uint32x4_t ab0 = vtrn1q_u32(a, b), ab1 = vtrn2q_u32(a, b), ef0 = vtrn1q_u32(e, f),
						 ef1 = vtrn2q_u32(e, f);
				const uint64x2_t r0 = vtrn1q_u64(vreinterpretq_u64_u32(ab0), vreinterpretq_u64_u32(ef0)),
						 r1 = vtrn1q_u64(vreinterpretq_u64_u32(ab1), vreinterpretq_u64_u32(ef1)),
						 r2 = vtrn2q_u64(vreinterpretq_u64_u32(ab0), vreinterpretq_u64_u32(ef0)),
						 r3 = vtrn2q_u64(vreinterpretq_u64_u32(ab1), vreinterpretq_u64_u32(ef1));
				vst1q_u32(d + k, vreinterpretq_u32_u64(r0));
				vst1q_u32(d + ds + k, vreinterpretq_u32_u64(r1));
				vst1q_u32(d + 2 * ds + k, vreinterpretq_u32_u64(r2));
				vst1q_u32(d + 3 * ds + k, vreinterpretq_u32_u64(r3));
			}
		}
#endif
	for (; j < m; j++) {
		uint32_t *d = dst + (size_t)(j0 + j) * ds + x0;
		for (int k = 0; k < n; k++)
			d[k] = tmp[(size_t)k * JRUN + j];
	}
}

/* rotated: blocks of K destination columns = K source rows */
static void convert_t(cam_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride)
{
	enum { K = CAM_CONV_K };
	const size_t ss = c->stride, row = (size_t)(c->c1 - c->c0) + PAD;
	const int span = c->c1 - c->c0, dh = c->dh;
	uint8_t *sy = c->stage, *suv = c->stage + (size_t)K * row;
	const uint8_t *ry[K], *ruv[K];
	for (int x0 = 0; x0 < c->dw; x0 += K) {
		const int n = c->dw - x0 < K ? c->dw - x0 : K;
		int lr = -1, lc = -1;
		for (int k = 0; k < n; k++) {
			int q = c->srow[x0 + k];
			if (q == lr) {
				ry[k] = ry[k - 1];
			} else {
				copy_in(sy + k * row, y + q * ss + c->c0, span);
				ry[k] = sy + k * row;
				lr = q;
			}
			if (q / 2 == lc) {
				ruv[k] = ruv[k - 1];
			} else {
				copy_in(suv + k * row, uv + (q / 2) * ss + c->c0, span);
				ruv[k] = suv + k * row;
				lc = q / 2;
			}
		}
		for (int j0 = 0; j0 < dh; j0 += JRUN) {
			const int m = dh - j0 < JRUN ? dh - j0 : JRUN;
			for (int k = 0; k < n; k++)
				if (k && ry[k] == ry[k - 1] && ruv[k] == ruv[k - 1])	/* (enlarging: the same source row) */
					memcpy(c->tmp + (size_t)k * JRUN, c->tmp + (size_t)(k - 1) * JRUN, sizeof(uint32_t) * m);
				else
					row_px(c, ry[k], ruv[k], c->tmp + (size_t)k * JRUN, j0, j0 + m);
			transpose_out(c->tmp, n, dst, (size_t)dst_stride, x0, j0, m);
		}
	}
}

/* not rotated: one source row per destination row */
static void convert_n(cam_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride)
{
	const size_t ss = c->stride;
	const int span = c->c1 - c->c0, dw = c->dw;
	uint8_t *sy = c->stage, *suv = c->stage + span + PAD;
	int last = -1, lc = -1;
	for (int j = 0; j < c->dh; j++) {
		uint32_t *d = dst + (size_t)j * dst_stride;
		const int r = c->srow[j];
		if (r == last) {	/* the same source row as the row above */
			memcpy(d, d - dst_stride, sizeof(uint32_t) * dw);
			continue;
		}
		last = r;
		copy_in(sy, y + r * ss + c->c0, span);
		if (r / 2 != lc) {
			copy_in(suv, uv + (r / 2) * ss + c->c0, span);
			lc = r / 2;
		}
		row_px(c, sy, suv, d, 0, dw);
	}
}

void cam_convert(cam_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride)
{
	if (c->transpose)
		convert_t(c, y, uv, dst, dst_stride);
	else
		convert_n(c, y, uv, dst, dst_stride);
}

cam_orient_t cam_default_orient(int w, int h, int camera)
{
	cam_orient_t o = { w > h ? 90 : 0, camera == 1, false };
	const char *e = getenv("ORCHARD_CAMERA_ROT");
	if (e && *e)
		o.rot = atoi(e);
	e = getenv("ORCHARD_CAMERA_RANGE");
	if (e && *e)
		o.full_range = !strcmp(e, "full");
	return o;
}

void cam_orient_src_xy(cam_orient_t o, int w, int h, int x, int y, int *sx, int *sy)
{
	int rot = ((o.rot % 360) + 360) % 360;
	if (o.mirror)
		x = (rot == 90 || rot == 270 ? h : w) - 1 - x;
	switch (rot) {
	case 90: *sx = y, *sy = h - 1 - x; break;
	case 180: *sx = w - 1 - x, *sy = h - 1 - y; break;
	case 270: *sx = w - 1 - y, *sy = x; break;
	default: *sx = x, *sy = y; break;
	}
}
