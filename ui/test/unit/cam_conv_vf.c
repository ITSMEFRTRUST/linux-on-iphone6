/* The viewfinder converter of orchard-camint e63f93a (camvf's staged rewrite: row copies, a 16-row gather per pixel,
 * 32-bit maths; 10-11 ms per 750x1000 frame on the phone, 2026-10-21), symbols renamed vf_*: camvf-bench times the
 * camfix converter against it on live frames. */
#include <stdlib.h>
#include <string.h>
#include "cam.h"

typedef struct vf_conv {
	int sw, sh, stride;
	cam_orient_t o;
	int dw, dh;
	int ow, oh;
	bool transpose, flipx, flipy;
	int xa, xb, ya, yb;
	int c0, c1;
	int *srow;
	int *yo, *co;
	uint8_t *stage;
	uint8_t *gy;
	uint16_t *guv;
} vf_conv_t;

vf_conv_t *vf_new(void);
bool vf_conv_init(vf_conv_t *c, int sw, int sh, int stride, cam_orient_t o, int dw, int dh);
void vf_conv_free(vf_conv_t *c);
void vf_convert(vf_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride);
vf_conv_t *vf_new(void) { return calloc(1, sizeof(vf_conv_t)); }

static inline int vf_clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* n gathered samples → n pixels (vectorised: 4 × 32-bit lanes). uv: the CbCr vf_pair of each, Cb in the low byte (the
 * plane's byte order on a little-endian CPU) */
static void vf_yuv_run(bool full, const uint8_t *restrict y, const uint16_t *restrict uv, uint32_t *restrict d, int n)
{
	if (full) {
		for (int i = 0; i < n; i++) {
			int l = y[i] << 16, uu = (uv[i] & 0xff) - 128, vv = (uv[i] >> 8) - 128;
			int r = vf_clamp8((l + vv * 91881 + 32768) >> 16);
			int g = vf_clamp8((l - uu * 22554 - vv * 46802 + 32768) >> 16);
			int b = vf_clamp8((l + uu * 116130 + 32768) >> 16);
			d[i] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
	} else {
		for (int i = 0; i < n; i++) {
			int l = (y[i] - 16) * 76309, uu = (uv[i] & 0xff) - 128, vv = (uv[i] >> 8) - 128;
			int r = vf_clamp8((l + vv * 104597 + 32768) >> 16);
			int g = vf_clamp8((l - uu * 25674 - vv * 53278 + 32768) >> 16);
			int b = vf_clamp8((l + uu * 132201 + 32768) >> 16);
			d[i] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
	}
}

/* the CbCr vf_pair at an even byte offset of a copied row */
static inline uint16_t vf_pair(const uint8_t *row, int o)
{
	uint16_t v;
	memcpy(&v, row + o, 2);
	return v;
}

/* n bytes out of the frame (uncached): 64 bytes per iteration in 16-byte vector loads (aarch64: two ldp q),
 * the fewest bus reads (musl's memcpy moves 16 bytes per ldp of x registers) */
typedef uint8_t vf_v16 __attribute__((vector_size(16), aligned(1)));
static void vf_copy_in(uint8_t *restrict d, const uint8_t *restrict s, size_t n)
{
	for (; n >= 64; n -= 64, s += 64, d += 64) {
		vf_v16 a = *(const vf_v16 *)s, b = *(const vf_v16 *)(s + 16), e = *(const vf_v16 *)(s + 32), f = *(const vf_v16 *)(s + 48);
		*(vf_v16 *)d = a, *(vf_v16 *)(d + 16) = b, *(vf_v16 *)(d + 32) = e, *(vf_v16 *)(d + 48) = f;
	}
	memcpy(d, s, n);
}

void vf_conv_free(vf_conv_t *c)
{
	free(c->srow);
	free(c->yo);
	free(c->co);
	free(c->stage);
	free(c->gy);
	free(c->guv);
	memset(c, 0, sizeof(*c));
}

/* destination i of n → source index in [lo, lo + len) (centre of the destination pixel), optionally flipped
 * within the plane of size full */
static int vf_map1(int i, int n, int lo, int len, bool flip, int full)
{
	int s = lo + (int)(((int64_t)(2 * i + 1) * len) / (2 * n));
	if (s >= lo + len)
		s = lo + len - 1;
	return flip ? full - 1 - s : s;
}

bool vf_conv_init(vf_conv_t *c, int sw, int sh, int stride, cam_orient_t o, int dw, int dh)
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
	int *px = malloc(sizeof(int) * dw), *py = malloc(sizeof(int) * dh);
	if (!px || !py) {
		free(px);
		free(py);
		return false;
	}
	int xa = c->ow, xb = 0, ya = c->oh, yb = 0;
	for (int x = 0; x < dw; x++) {
		int s = px[x] = vf_map1(x, dw, x0, cw, c->flipx, c->ow);
		xa = s < xa ? s : xa;
		xb = s + 1 > xb ? s + 1 : xb;
	}
	for (int y = 0; y < dh; y++) {
		int s = py[y] = vf_map1(y, dh, y0, ch, c->flipy, c->oh);
		ya = s < ya ? s : ya;
		yb = s + 1 > yb ? s + 1 : yb;
	}
	c->xa = xa & ~1, c->xb = (xb + 1) & ~1, c->ya = ya & ~1, c->yb = (yb + 1) & ~1;
	/* source rows ↔ one destination axis, source columns (offsets into a copied row) ↔ the other. Rotated: a
	 * picture column is a source row, a picture row a source column; else the other way round. */
	int *rows = c->transpose ? px : py;
	int *cols = c->transpose ? py : px, ncols = c->transpose ? dh : dw;
	int ca = c->transpose ? c->ya : c->xa, cb = c->transpose ? c->yb : c->xb;
	/* the copied span: 64-byte aligned when the row's stride has room for it (the phone's: stride = width
	 * rounded up to 64) — whole bus bursts */
	c->c0 = ca & ~63;
	c->c1 = (cb + 63) & ~63;
	if (c->c1 > stride)
		c->c0 = ca, c->c1 = cb;
	const int span = c->c1 - c->c0, nstage = c->transpose ? 16 : 1, run = c->transpose ? 16 : dw;
	c->srow = rows;
	c->yo = malloc(sizeof(int) * ncols);
	c->co = malloc(sizeof(int) * ncols);
	c->stage = malloc((size_t)span * nstage * 2);
	c->gy = malloc((size_t)run);
	c->guv = malloc(sizeof(uint16_t) * run);
	if (!c->yo || !c->co || !c->stage || !c->gy || !c->guv) {
		free(cols);
		goto fail;
	}
	for (int i = 0; i < ncols; i++) {
		c->yo[i] = cols[i] - c->c0;
		c->co[i] = (cols[i] & ~1) - c->c0;
	}
	free(cols);
	return true;
fail:
	vf_conv_free(c);
	return false;
}

/* rotated: blocks of K destination columns = K source rows */
static void vf_convert_t(vf_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride)
{
	enum { K = 16 };
	const size_t ss = c->stride;
	const int span = c->c1 - c->c0, dh = c->dh;
	const bool full = c->o.full_range;
	uint8_t *sy = c->stage, *suv = c->stage + (size_t)K * span, *gy = c->gy;
	uint16_t *guv = c->guv;
	const uint8_t *ry[K], *ruv[K];
	for (int x0 = 0; x0 < c->dw; x0 += K) {
		const int n = c->dw - x0 < K ? c->dw - x0 : K;
		int lr = -1, lc = -1;
		for (int k = 0; k < n; k++) {
			int q = c->srow[x0 + k];
			if (q == lr) {
				ry[k] = ry[k - 1];
			} else {
				vf_copy_in(sy + (size_t)k * span, y + q * ss + c->c0, span);
				ry[k] = sy + (size_t)k * span;
				lr = q;
			}
			if (q / 2 == lc) {
				ruv[k] = ruv[k - 1];
			} else {
				vf_copy_in(suv + (size_t)k * span, uv + (q / 2) * ss + c->c0, span);
				ruv[k] = suv + (size_t)k * span;
				lc = q / 2;
			}
		}
		int last = -1;
		for (int j = 0; j < dh; j++) {
			uint32_t *d = dst + (size_t)j * dst_stride + x0;
			const int o = c->yo[j], oc = c->co[j];
			if (o == last) {	/* the same source column as the row above */
				memcpy(d, d - dst_stride, sizeof(uint32_t) * n);
				continue;
			}
			last = o;
			for (int k = 0; k < n; k++) {
				gy[k] = ry[k][o];
				guv[k] = vf_pair(ruv[k], oc);
			}
			if (n == K)	/* (a constant count: unrolled) */
				vf_yuv_run(full, gy, guv, d, K);
			else
				vf_yuv_run(full, gy, guv, d, n);
		}
	}
}

/* not rotated: one source row per destination row */
static void vf_convert_n(vf_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride)
{
	const size_t ss = c->stride;
	const int span = c->c1 - c->c0, dw = c->dw;
	uint8_t *sy = c->stage, *suv = c->stage + span, *gy = c->gy;
	uint16_t *guv = c->guv;
	int last = -1, lc = -1;
	for (int j = 0; j < c->dh; j++) {
		uint32_t *d = dst + (size_t)j * dst_stride;
		const int r = c->srow[j];
		if (r == last) {	/* the same source row as the row above */
			memcpy(d, d - dst_stride, sizeof(uint32_t) * dw);
			continue;
		}
		last = r;
		vf_copy_in(sy, y + r * ss + c->c0, span);
		if (r / 2 != lc) {
			vf_copy_in(suv, uv + (r / 2) * ss + c->c0, span);
			lc = r / 2;
		}
		for (int x = 0; x < dw; x++) {
			gy[x] = sy[c->yo[x]];
			guv[x] = vf_pair(suv, c->co[x]);
		}
		vf_yuv_run(c->o.full_range, gy, guv, d, dw);
	}
}

void vf_convert(vf_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride)
{
	if (c->transpose)
		vf_convert_t(c, y, uv, dst, dst_stride);
	else
		vf_convert_n(c, y, uv, dst, dst_stride);
}

