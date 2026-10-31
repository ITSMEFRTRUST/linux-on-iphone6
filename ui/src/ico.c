/* Windows .ico reader (see ico.h). Every offset and size read from the file is checked against the buffer
 * before it is used; sizes are computed in 64 bits. Formats: ICONDIR (reserved 0, type 1, count) + 16-byte
 * entries pointing at either a PNG or a headerless BMP (BITMAPINFOHEADER, height doubled for the AND mask,
 * rows bottom-up and padded to 4 bytes, then the 1-bpp AND mask rows). */
#include <stdlib.h>
#include <string.h>
#include "ico.h"

#define ICO_MAX_ENTRIES 64

static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

bool ico_is(const void *data, size_t len)
{
	const uint8_t *p = data;
	return len >= 6 && rd16(p) == 0 && rd16(p + 2) == 1 && rd16(p + 4) >= 1;
}

/* what an entry's bytes hold: kind, size, colour depth (PNG counts as 32); false = unusable */
struct cand { int kind, w, h, bpp; const uint8_t *p; size_t n; };

static bool probe(const uint8_t *p, size_t n, struct cand *c)
{
	c->p = p;
	c->n = n;
	if (n >= 24 && !memcmp(p, "\x89PNG\r\n\x1a\n", 8) && !memcmp(p + 12, "IHDR", 4)) {
		uint32_t w = be32(p + 16), h = be32(p + 20);
		if (!w || !h || w > ICO_MAX_PNG_DIM || h > ICO_MAX_PNG_DIM)
			return false;
		c->kind = ICO_PNG;
		c->w = (int)w;
		c->h = (int)h;
		c->bpp = 32;
		return true;
	}
	if (n < 40)
		return false;
	uint32_t hs = rd32(p);
	int32_t w = (int32_t)rd32(p + 4), h2 = (int32_t)rd32(p + 8);
	uint32_t planes = rd16(p + 12), bpp = rd16(p + 14), comp = rd32(p + 16);
	if (hs < 40 || hs > n || w <= 0 || w > ICO_MAX_DIM || h2 <= 0 || h2 > 2 * ICO_MAX_DIM || planes > 1)
		return false;
	if (!(bpp == 1 || bpp == 4 || bpp == 8 || bpp == 24 || bpp == 32))
		return false;
	if (!(comp == 0 || (comp == 3 && bpp == 32)))	/* BI_RGB, or BI_BITFIELDS with the usual BGRA masks */
		return false;
	int h = h2 >= 2 ? h2 / 2 : h2;	/* the XOR image; the AND mask doubles the height */
	if (h > ICO_MAX_DIM)
		return false;
	c->kind = ICO_RGBA;
	c->w = w;
	c->h = h;
	c->bpp = (int)bpp;
	return true;
}

static bool better(const struct cand *a, const struct cand *b)	/* a over b? */
{
	int da = a->w < a->h ? a->w : a->h, db = b->w < b->h ? b->w : b->h;
	if (da != db)
		return da > db;
	if (a->bpp != b->bpp)
		return a->bpp > b->bpp;
	return a->kind == ICO_PNG && b->kind != ICO_PNG;
}

/* BMP entry → RGBA; NULL = truncated / broken */
static uint8_t *bmp_rgba(const struct cand *c)
{
	const uint8_t *p = c->p;
	size_t n = c->n;
	uint32_t hs = rd32(p), bpp = c->bpp, comp = rd32(p + 16);
	int w = c->w, h = c->h;
	size_t off = hs;
	if (comp == 3) {	/* BI_BITFIELDS: three masks at 40..51 (after a 40-byte header, else inside a longer one) */
		if (n < 52)
			return NULL;
		if (hs < 52)
			off = 52;
		if (rd32(p + 40) != 0x00ff0000 || rd32(p + 44) != 0x0000ff00 || rd32(p + 48) != 0x000000ff)
			return NULL;
	}
	uint32_t pal[256];
	if (bpp <= 8) {
		uint64_t ncol = rd32(p + 32), maxc = 1u << bpp;
		if (!ncol || ncol > maxc)
			ncol = maxc;
		if (off + ncol * 4 > n)
			return NULL;
		memset(pal, 0, sizeof(pal));
		for (uint64_t i = 0; i < ncol; i++)
			pal[i] = rd32(p + off + i * 4);	/* B, G, R, reserved */
		off += ncol * 4;
	}
	uint64_t xrow = ((uint64_t)w * bpp + 31) / 32 * 4, mrow = ((uint64_t)w + 31) / 32 * 4;
	if (off + xrow * h > n)
		return NULL;
	const uint8_t *xor = p + off, *and = NULL;
	if (off + xrow * h + mrow * h <= n)	/* some 32-bpp icons leave the mask out */
		and = xor + xrow * h;
	uint8_t *o = malloc((size_t)w * h * 4);
	if (!o)
		return NULL;
	bool any_alpha = false;
	for (int y = 0; y < h; y++) {
		const uint8_t *r = xor + (uint64_t)(h - 1 - y) * xrow;	/* bottom-up */
		uint8_t *d = o + (size_t)y * w * 4;
		for (int x = 0; x < w; x++, d += 4) {
			uint32_t bgr;
			int a = 255;
			switch (bpp) {
			case 32:
				bgr = rd32(r + x * 4);
				a = bgr >> 24;
				any_alpha |= a != 0;
				break;
			case 24:
				bgr = r[x * 3] | r[x * 3 + 1] << 8 | r[x * 3 + 2] << 16;
				break;
			case 8:
				bgr = pal[r[x]];
				break;
			case 4:
				bgr = pal[r[x / 2] >> (x & 1 ? 0 : 4) & 15];
				break;
			default:
				bgr = pal[r[x / 8] >> (7 - (x & 7)) & 1];
				break;
			}
			d[0] = bgr >> 16;
			d[1] = bgr >> 8;
			d[2] = bgr;
			d[3] = a;
		}
	}
	/* the AND mask (1 = transparent) for icons without an alpha channel, or a 32-bpp one whose alpha is all 0 */
	if (bpp != 32 || !any_alpha)
		for (int y = 0; y < h; y++) {
			const uint8_t *m = and ? and + (uint64_t)(h - 1 - y) * mrow : NULL;
			uint8_t *d = o + (size_t)y * w * 4;
			for (int x = 0; x < w; x++)
				d[x * 4 + 3] = m && (m[x / 8] >> (7 - (x & 7)) & 1) ? 0 : 255;
		}
	return o;
}

int ico_decode(const void *data, size_t len, ico_img_t *out)
{
	const uint8_t *p = data;
	memset(out, 0, sizeof(*out));
	if (!ico_is(data, len))
		return ICO_NONE;
	unsigned count = rd16(p + 4);
	if (count > ICO_MAX_ENTRIES)
		count = ICO_MAX_ENTRIES;
	struct cand cs[ICO_MAX_ENTRIES];
	int nc = 0;
	for (unsigned i = 0; i < count; i++) {
		size_t e = 6 + (size_t)i * 16;
		if (e + 16 > len)
			break;
		uint64_t sz = rd32(p + e + 8), off = rd32(p + e + 12);
		if (off < 6 || off >= len)
			continue;
		if (off + sz > len)	/* a size that overshoots: what is there (BMP checks its own rows) */
			sz = len - off;
		if (probe(p + off, (size_t)sz, &cs[nc]))
			nc++;
	}
	/* best first; a broken BMP gives way to the next one */
	for (int tries = 0; tries < nc; tries++) {
		int b = -1;
		for (int i = 0; i < nc; i++)
			if (cs[i].kind != ICO_NONE && (b < 0 || better(&cs[i], &cs[b])))
				b = i;
		if (b < 0)
			break;
		out->w = cs[b].w;
		out->h = cs[b].h;
		if (cs[b].kind == ICO_PNG) {
			out->png = cs[b].p;
			out->png_len = cs[b].n;
			return out->kind = ICO_PNG;
		}
		if ((out->rgba = bmp_rgba(&cs[b])))
			return out->kind = ICO_RGBA;
		cs[b].kind = ICO_NONE;
	}
	out->w = out->h = 0;
	return ICO_NONE;
}
