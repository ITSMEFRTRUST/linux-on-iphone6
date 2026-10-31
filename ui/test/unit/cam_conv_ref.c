/* The viewfinder converter as it was before the staged rewrite (orchard-polish 89f7da6 cam_conv.c, symbols renamed
 * ref_*): cam_conv_test checks the new one against it pixel for pixel and times both (`cam_conv_test bench`). */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "cam.h"

typedef struct ref_conv {
	int sw, sh, stride;
	cam_orient_t o;
	int dw, dh;
	int ow, oh;
	bool transpose, flipx, flipy;
	int xa, xb, ya, yb;
	int *xrel, *ymap;
	uint8_t *ty, *tuv;
	uint32_t *row;
} ref_conv_t;

bool ref_conv_init(ref_conv_t *c, int sw, int sh, int stride, cam_orient_t o, int dw, int dh);
void ref_conv_free(ref_conv_t *c);
void ref_convert(ref_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride);

#define TILE 16

static inline int ref_clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }


void ref_conv_free(ref_conv_t *c)
{
	free(c->xrel);
	free(c->ymap);
	free(c->ty);
	free(c->tuv);
	free(c->row);
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

bool ref_conv_init(ref_conv_t *c, int sw, int sh, int stride, cam_orient_t o, int dw, int dh)
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
	c->xrel = malloc(sizeof(int) * dw);
	c->ymap = malloc(sizeof(int) * dh);
	if (!c->xrel || !c->ymap)
		goto fail;
	int xa = c->ow, xb = 0, ya = c->oh, yb = 0;
	for (int x = 0; x < dw; x++) {
		int s = map1(x, dw, x0, cw, c->flipx, c->ow);
		c->xrel[x] = s;
		xa = s < xa ? s : xa;
		xb = s + 1 > xb ? s + 1 : xb;
	}
	for (int y = 0; y < dh; y++) {
		int s = map1(y, dh, y0, ch, c->flipy, c->oh);
		c->ymap[y] = s;
		ya = s < ya ? s : ya;
		yb = s + 1 > yb ? s + 1 : yb;
	}
	c->xa = xa & ~1, c->xb = (xb + 1) & ~1, c->ya = ya & ~1, c->yb = (yb + 1) & ~1;
	for (int x = 0; x < dw; x++)
		c->xrel[x] -= c->xa;
	c->row = malloc(sizeof(uint32_t) * (c->xb - c->xa));
	if (!c->row)
		goto fail;
	if (c->transpose) {
		c->ty = malloc((size_t)c->ow * c->oh);
		c->tuv = malloc((size_t)c->ow * c->oh / 2);
		if (!c->ty || !c->tuv)
			goto fail;
	}
	return true;
fail:
	ref_conv_free(c);
	return false;
}

/* transpose the window [ya, yb) × [xa, xb) of the rotated picture out of the source planes:
 * T[r][q] = S[q][r] — T's rows are the source's columns */
static void transpose_window(ref_conv_t *c, const uint8_t *y, const uint8_t *uv)
{
	const int ss = c->stride, ts = c->ow;
	for (int r0 = c->ya; r0 < c->yb; r0 += TILE)
		for (int q0 = c->xa; q0 < c->xb; q0 += TILE) {
			int r1 = r0 + TILE < c->yb ? r0 + TILE : c->yb, q1 = q0 + TILE < c->xb ? q0 + TILE : c->xb;
			for (int r = r0; r < r1; r++) {
				uint8_t *d = c->ty + (size_t)r * ts;
				const uint8_t *s = y + r;
				for (int q = q0; q < q1; q++)
					d[q] = s[(size_t)q * ss];
			}
		}
	/* CbCr pairs: the half-size plane, 2-byte elements */
	for (int r0 = c->ya / 2; r0 < c->yb / 2; r0 += TILE)
		for (int q0 = c->xa / 2; q0 < c->xb / 2; q0 += TILE) {
			int r1 = r0 + TILE < c->yb / 2 ? r0 + TILE : c->yb / 2;
			int q1 = q0 + TILE < c->xb / 2 ? q0 + TILE : c->xb / 2;
			for (int r = r0; r < r1; r++) {
				uint8_t *d = c->tuv + (size_t)r * ts;
				const uint8_t *s = uv + 2 * r;
				for (int q = q0; q < q1; q++) {
					d[2 * q] = s[(size_t)q * ss];
					d[2 * q + 1] = s[(size_t)q * ss + 1];
				}
			}
		}
}

/* one plane row's window [xa, xb) → c->row (pairs share their CbCr sample) */
static void convert_row(const ref_conv_t *c, const uint8_t *yr, const uint8_t *uvr)
{
	uint32_t *d = c->row;
	const int n = c->xb - c->xa;
	yr += c->xa;
	uvr += c->xa;
	if (c->o.full_range) {
		for (int x = 0; x < n; x += 2) {
			int u = uvr[x] - 128, v = uvr[x + 1] - 128;
			int rr = v * 91881 + 32768, gg = 32768 - u * 22554 - v * 46802, bb = u * 116130 + 32768;
			for (int k = 0; k < 2; k++) {
				int l = yr[x + k] << 16;
				int r = ref_clamp8((l + rr) >> 16), g = ref_clamp8((l + gg) >> 16), b = ref_clamp8((l + bb) >> 16);
				d[x + k] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
			}
		}
	} else {
		for (int x = 0; x < n; x += 2) {
			int u = uvr[x] - 128, v = uvr[x + 1] - 128;
			int rr = v * 104597 + 32768, gg = 32768 - u * 25674 - v * 53278, bb = u * 132201 + 32768;
			for (int k = 0; k < 2; k++) {
				int l = (yr[x + k] - 16) * 76309;
				int r = ref_clamp8((l + rr) >> 16), g = ref_clamp8((l + gg) >> 16), b = ref_clamp8((l + bb) >> 16);
				d[x + k] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
			}
		}
	}
}

void ref_convert(ref_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride)
{
	const uint8_t *py = y, *puv = uv;
	size_t ps = c->stride;
	if (c->transpose) {
		transpose_window(c, y, uv);
		py = c->ty, puv = c->tuv, ps = c->ow;
	}
	const int dw = c->dw, *xr = c->xrel;
	int last = -1;
	for (int j = 0; j < c->dh; j++) {
		uint32_t *d = dst + (size_t)j * dst_stride;
		int r = c->ymap[j];
		if (r == last) {	/* the same source row as the one above: copy it */
			memcpy(d, d - dst_stride, sizeof(uint32_t) * dw);
			continue;
		}
		last = r;
		convert_row(c, py + (size_t)r * ps, puv + (size_t)(r / 2) * ps);
		const uint32_t *s = c->row;
		for (int x = 0; x < dw; x++)
			d[x] = s[xr[x]];
	}
}


ref_conv_t *ref_new(void) { return calloc(1, sizeof(ref_conv_t)); }
