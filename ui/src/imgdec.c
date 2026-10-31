/* Image decoding for the Web app: JPEG (baseline, LVGL's bundled TJpgDec) and PNG (LVGL's bundled
 * lodepng), scaled to fit a box with an area-average filter (or bilinear when enlarging), into a
 * malloc'd lv_image_dsc_t. Touches no LVGL state: runs on the net_bg worker thread. Both decoders are
 * compiled here (they're off in lv_conf.h, so the LVGL build doesn't carry them or their image decoders). */
#pragma GCC diagnostic ignored "-Wunused-function"
#define LV_USE_LODEPNG 1
#define LV_USE_TJPGD 1
#define LODEPNG_NO_COMPILE_ENCODER
#define LODEPNG_NO_COMPILE_DISK
#define LODEPNG_MAX_ALLOC (48 << 20)
#include "../third_party/lvgl/src/libs/lodepng/lodepng.c"
#include "../third_party/lvgl/src/libs/tjpgd/tjpgd.c"
/* 16-bit-per-channel PNGs: LVGL's lodepng refuses them (error 116), stb_image reads them as 8-bit */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MAX_DIMENSIONS 8192
#include "../third_party/lvgl/src/libs/gltf/stb_image/stb_image.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include "imgdec.h"
#include "ico.c"	/* favicons (.ico): its own file so test/unit/ico_fuzz.c can fuzz it alone */

#define MAX_SRC_PIXELS (4096 * 4096)	/* JPEG is streamed; PNG decodes whole (4 B/px) under LODEPNG_MAX_ALLOC */

/* area-average accumulator: sw×sh source pixels → aw×ah */
typedef struct acc {
	int sw, sh, aw, ah;
	uint32_t *sum;	/* b, g, r, a, n per accumulator pixel */
	int *xmap;	/* source x → accumulator x */
} acc_t;

static bool acc_init(acc_t *a, int sw, int sh, int aw, int ah)
{
	a->sw = sw;
	a->sh = sh;
	a->aw = aw;
	a->ah = ah;
	a->sum = calloc((size_t)aw * ah * 5, sizeof(uint32_t));
	a->xmap = malloc(sw * sizeof(int));
	if (!a->sum || !a->xmap)
		return false;
	for (int x = 0; x < sw; x++)
		a->xmap[x] = (int)((int64_t)x * aw / sw);
	return true;
}

static void acc_free(acc_t *a)
{
	free(a->sum);
	free(a->xmap);
}

static inline void acc_px(acc_t *a, int x, int y, int b, int g, int r, int al)
{
	int ty = (int)((int64_t)y * a->ah / a->sh);
	uint32_t *s = a->sum + ((size_t)ty * a->aw + a->xmap[x]) * 5;
	s[0] += b;
	s[1] += g;
	s[2] += r;
	s[3] += al;
	s[4]++;
}

/* accumulator → BGRA (aw×ah) */
static uint8_t *acc_out(acc_t *a, bool *alpha)
{
	uint8_t *o = malloc((size_t)a->aw * a->ah * 4);
	if (!o)
		return NULL;
	*alpha = false;
	for (size_t i = 0; i < (size_t)a->aw * a->ah; i++) {
		uint32_t *s = a->sum + i * 5, n = s[4] ? s[4] : 1;
		o[i * 4] = s[0] / n;
		o[i * 4 + 1] = s[1] / n;
		o[i * 4 + 2] = s[2] / n;
		o[i * 4 + 3] = s[4] ? s[3] / n : 0;
		if (o[i * 4 + 3] != 255)
			*alpha = true;
	}
	return o;
}

/* bilinear enlarge of BGRA */
static uint8_t *enlarge(const uint8_t *src, int sw, int sh, int tw, int th)
{
	uint8_t *o = malloc((size_t)tw * th * 4);
	if (!o)
		return NULL;
	for (int y = 0; y < th; y++) {
		int fy = (int)(((int64_t)y * 2 + 1) * sh * 128 / (2 * th)) - 64;
		if (fy < 0)
			fy = 0;
		int y0 = fy >> 7, wy = fy & 127, y1 = y0 + 1 < sh ? y0 + 1 : y0;
		for (int x = 0; x < tw; x++) {
			int fx = (int)(((int64_t)x * 2 + 1) * sw * 128 / (2 * tw)) - 64;
			if (fx < 0)
				fx = 0;
			int x0 = fx >> 7, wx = fx & 127, x1 = x0 + 1 < sw ? x0 + 1 : x0;
			const uint8_t *p00 = src + ((size_t)y0 * sw + x0) * 4, *p01 = src + ((size_t)y0 * sw + x1) * 4;
			const uint8_t *p10 = src + ((size_t)y1 * sw + x0) * 4, *p11 = src + ((size_t)y1 * sw + x1) * 4;
			uint8_t *d = o + ((size_t)y * tw + x) * 4;
			for (int c = 0; c < 4; c++) {
				int top = p00[c] * (128 - wx) + p01[c] * wx, bot = p10[c] * (128 - wx) + p11[c] * wx;
				d[c] = (top * (128 - wy) + bot * wy) >> 14;
			}
		}
	}
	return o;
}

/* target size: CSS pixels shown at @2x (scale 2; 1 = pixel for pixel), fitted into max_w × max_h, never below 1 */
static void fit(int sw, int sh, int max_w, int max_h, int scale, int *tw, int *th)
{
	int64_t w = (int64_t)sw * scale, h = (int64_t)sh * scale;
	if (w > max_w) {
		h = h * max_w / w;
		w = max_w;
	}
	if (h > max_h) {
		w = w * max_h / h;
		h = max_h;
	}
	*tw = w < 1 ? 1 : (int)w;
	*th = h < 1 ? 1 : (int)h;
}

static lv_image_dsc_t *finish(uint8_t *px, int w, int h, bool alpha)
{
	lv_image_dsc_t *d = calloc(1, sizeof(*d));
	if (!d) {
		free(px);
		return NULL;
	}
	if (!alpha)	/* XRGB: the 4th byte is ignored, but keep it opaque anyway */
		for (size_t i = 0; i < (size_t)w * h; i++)
			px[i * 4 + 3] = 255;
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = alpha ? LV_COLOR_FORMAT_ARGB8888 : LV_COLOR_FORMAT_XRGB8888;
	d->header.w = w;
	d->header.h = h;
	d->header.stride = w * 4;
	d->data_size = (uint32_t)w * h * 4;
	d->data = px;
	return d;
}

/* the accumulated (≤ target) picture, enlarged to tw×th when the source was smaller */
static lv_image_dsc_t *from_acc(acc_t *a, int tw, int th)
{
	bool alpha;
	uint8_t *o = acc_out(a, &alpha);
	if (!o)
		return NULL;
	if (a->aw != tw || a->ah != th) {
		uint8_t *e = enlarge(o, a->aw, a->ah, tw, th);
		free(o);
		if (!e)
			return NULL;
		o = e;
	}
	return finish(o, tw, th, alpha);
}

/* ---------- JPEG ---------- */

typedef struct jsrc { const uint8_t *p; size_t n, pos; acc_t acc; } jsrc_t;

static size_t j_in(JDEC *jd, uint8_t *buf, size_t n)
{
	jsrc_t *s = jd->device;
	size_t left = s->n - s->pos;
	if (n > left)
		n = left;
	if (buf)
		memcpy(buf, s->p + s->pos, n);
	s->pos += n;
	return n;
}

static int j_out(JDEC *jd, void *bitmap, JRECT *r)
{
	jsrc_t *s = jd->device;
	const uint8_t *px = bitmap;	/* B, G, R */
	for (int y = r->top; y <= r->bottom; y++)
		for (int x = r->left; x <= r->right; x++, px += 3)
			if (x < s->acc.sw && y < s->acc.sh)
				acc_px(&s->acc, x, y, px[0], px[1], px[2], 255);
	return 1;
}

static lv_image_dsc_t *jpeg(const uint8_t *data, size_t len, int max_w, int max_h, int scale, int *nat, const char **err)
{
	JDEC jd;
	jsrc_t s = { data, len, 0, { 0 } };
	size_t pool_n = 32768;
	void *pool = malloc(pool_n);
	if (!pool)
		return NULL;
	JRESULT r = jd_prepare(&jd, j_in, pool, pool_n, &s);
	if (r != JDR_OK) {
		*err = r == JDR_FMT3 ? "progressive or unusual JPEG" : "bad JPEG";
		free(pool);
		return NULL;
	}
	int sw = jd.width, sh = jd.height, tw, th;
	if ((int64_t)sw * sh > MAX_SRC_PIXELS || !sw || !sh) {
		*err = "image too large";
		free(pool);
		return NULL;
	}
	*nat = sw > sh ? sw : sh;
	fit(sw, sh, max_w, max_h, scale, &tw, &th);
	if (!acc_init(&s.acc, sw, sh, tw < sw ? tw : sw, th < sh ? th : sh)) {
		acc_free(&s.acc);
		free(pool);
		return NULL;
	}
	r = jd_decomp(&jd, j_out, 0);
	free(pool);
	lv_image_dsc_t *d = r == JDR_OK ? from_acc(&s.acc, tw, th) : NULL;
	if (r != JDR_OK)
		*err = "JPEG decode failed";
	acc_free(&s.acc);
	return d;
}

/* unscaled, straight into a caller's XRGB8888 buffer (e.g. a full-site renderer's frames, when one is built
 * without libjpeg-turbo). Any thread: no shared state. */
typedef struct jraw { const uint8_t *p; size_t n, pos; uint32_t *dst; int stride, w, h; } jraw_t;

static size_t jr_in(JDEC *jd, uint8_t *buf, size_t n)
{
	jraw_t *s = jd->device;
	size_t left = s->n - s->pos;
	if (n > left)
		n = left;
	if (buf)
		memcpy(buf, s->p + s->pos, n);
	s->pos += n;
	return n;
}

static int jr_out(JDEC *jd, void *bitmap, JRECT *r)
{
	jraw_t *s = jd->device;
	const uint8_t *px = bitmap;	/* B, G, R */
	int rw = r->right - r->left + 1;
	for (int y = r->top; y <= r->bottom; y++, px += rw * 3) {
		if (y >= s->h)
			continue;
		uint32_t *d = s->dst + (size_t)y * s->stride;
		for (int x = r->left, i = 0; x <= r->right && x < s->w; x++, i += 3)
			d[x] = 0xff000000u | (uint32_t)px[i + 2] << 16 | (uint32_t)px[i + 1] << 8 | px[i];
	}
	return 1;
}

bool img_jpeg_xrgb(const void *data, size_t len, uint32_t *dst, int stride, int max_w, int max_h, int *w, int *h)
{
	JDEC jd;
	jraw_t s = { data, len, 0, dst, stride, 0, 0 };
	size_t pool_n = 32768;
	void *pool = malloc(pool_n);
	if (!pool)
		return false;
	bool ok = jd_prepare(&jd, jr_in, pool, pool_n, &s) == JDR_OK && jd.width && jd.height && jd.width <= max_w &&
		  jd.height <= max_h && jd.width <= stride;
	if (ok) {
		s.w = *w = jd.width, s.h = *h = jd.height;
		ok = jd_decomp(&jd, jr_out, 0) == JDR_OK;
	}
	free(pool);
	return ok;
}

/* ---------- PNG ---------- */

/* RGBA rows (straight alpha) → fitted picture */
static lv_image_dsc_t *from_rgba(const uint8_t *pix, unsigned sw, unsigned sh, size_t stride, int max_w, int max_h,
				 int scale)
{
	int tw, th;
	fit(sw, sh, max_w, max_h, scale, &tw, &th);
	acc_t a = { 0 };
	lv_image_dsc_t *d = NULL;
	if (acc_init(&a, sw, sh, (int)sw > tw ? tw : (int)sw, (int)sh > th ? th : (int)sh)) {
		for (unsigned y = 0; y < sh; y++) {
			const uint8_t *p = pix + (size_t)y * stride;
			for (unsigned x = 0; x < sw; x++, p += 4)	/* premultiply-free average: weight by alpha */
				acc_px(&a, x, y, p[2] * p[3] / 255, p[1] * p[3] / 255, p[0] * p[3] / 255, p[3]);
		}
		/* un-weight colour by the averaged alpha */
		for (size_t i = 0; i < (size_t)a.aw * a.ah; i++) {
			uint32_t *s = a.sum + i * 5;
			if (s[3])	/* straight colour = Σ(c·a) / Σa; acc_out divides by n */
				for (int c = 0; c < 3; c++) {
					uint64_t v = (uint64_t)s[c] * 255 * s[4] / s[3];
					s[c] = v > 255ull * s[4] ? 255 * s[4] : (uint32_t)v;
				}
		}
		d = from_acc(&a, tw, th);
	}
	acc_free(&a);
	return d;
}

static lv_image_dsc_t *png(const uint8_t *data, size_t len, int max_w, int max_h, int scale, int *nat, const char **err)
{
	unsigned sw, sh;
	LodePNGState st;
	lodepng_state_init(&st);
	unsigned ierr = lodepng_inspect(&sw, &sh, &st, data, len);
	lodepng_state_cleanup(&st);
	bool deep = ierr == 116;	/* > 8 bits per channel: stb_image instead */
	if (ierr && !deep) {
		*err = "bad PNG";
		return NULL;
	}
	if ((int64_t)sw * sh > 2400 * 2400 || !sw || !sh) {
		*err = "image too large";
		return NULL;
	}
	/* RGBA bytes, rows `stride` apart. LVGL's lodepng hands back an lv_draw_buf_t from its image-cache
	 * allocator: plain malloc underneath (LV_STDLIB_CLIB), so it is fine on this thread */
	lv_draw_buf_t *buf = NULL;
	unsigned char *pix;
	size_t stride;
	if (deep) {
		int iw, ih, n;
		pix = len <= INT_MAX ? stbi_load_from_memory(data, (int)len, &iw, &ih, &n, 4) : NULL;
		if (!pix || (unsigned)iw != sw || (unsigned)ih != sh) {
			stbi_image_free(pix);
			*err = "PNG decode failed";
			return NULL;
		}
		stride = (size_t)sw * 4;
	} else {
		unsigned char *out = NULL;
		if (lodepng_decode32(&out, &sw, &sh, data, len) || !out) {
			if (out)
				lv_draw_buf_destroy((lv_draw_buf_t *)out);
			*err = "PNG decode failed";
			return NULL;
		}
		buf = (lv_draw_buf_t *)out;
		pix = buf->data;
		stride = buf->header.stride;
	}
	*nat = sw > sh ? sw : sh;
	lv_image_dsc_t *d = from_rgba(pix, sw, sh, stride, max_w, max_h, scale);
	if (buf)
		lv_draw_buf_destroy(buf);
	else
		stbi_image_free(pix);
	return d;
}

static lv_image_dsc_t *decode(const void *data, size_t len, int max_w, int max_h, int scale, int *nat, const char **err)
{
	const uint8_t *p = data;
	*nat = 0;
	*err = "unsupported image format";
	if (len > 3 && p[0] == 0xff && p[1] == 0xd8 && p[2] == 0xff)
		return jpeg(p, len, max_w, max_h, scale, nat, err);
	if (len > 8 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8))
		return png(p, len, max_w, max_h, scale, nat, err);
	if (ico_is(p, len)) {
		ico_img_t ic;
		int k = ico_decode(p, len, &ic);
		if (k == ICO_PNG)
			return png(ic.png, ic.png_len, max_w, max_h, scale, nat, err);
		if (k == ICO_RGBA) {
			*nat = ic.w > ic.h ? ic.w : ic.h;
			lv_image_dsc_t *d = from_rgba(ic.rgba, ic.w, ic.h, (size_t)ic.w * 4, max_w, max_h, scale);
			free(ic.rgba);
			return d;
		}
		*err = "bad icon";
	}
	return NULL;
}

lv_image_dsc_t *img_decode(const void *data, size_t len, int max_w, int max_h, const char **err)
{
	const char *dummy;
	int nat;
	return decode(data, len, max_w, max_h, 2, &nat, err ? err : &dummy);
}

lv_image_dsc_t *img_decode_1x(const void *data, size_t len, int max, int *natural, const char **err)
{
	const char *dummy;
	int nat;
	lv_image_dsc_t *d = decode(data, len, max, max, 1, &nat, err ? err : &dummy);
	if (natural)
		*natural = nat;
	return d;
}

void img_free(lv_image_dsc_t *d)
{
	if (!d)
		return;
	free((void *)d->data);
	free(d);
}
