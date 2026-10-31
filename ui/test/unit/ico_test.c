/* ico.c: the best entry is picked, BMP depths decode right (palette, AND mask, alpha), hostile files are refused */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ico.h"
static int fails;
#define CHK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t buf[1 << 16];
static size_t n;
static void p8(int v) { buf[n++] = v; }
static void p16(int v) { p8(v & 255); p8(v >> 8 & 255); }
static void p32(uint32_t v) { p16(v & 0xffff); p16(v >> 16); }
static void at32(size_t o, uint32_t v) { for (int i = 0; i < 4; i++) buf[o + i] = v >> (8 * i); }

/* ICONDIR with k entries; returns the offset of entry i's size field via ent[] */
static size_t ent[8];
static void dir(int k) { n = 0; p16(0); p16(1); p16(k); for (int i = 0; i < k; i++) { ent[i] = n; for (int j = 0; j < 16; j++) p8(0); } }
static void begin(int i) { at32(ent[i] + 12, (uint32_t)n); }
static void end(int i, size_t start) { at32(ent[i] + 8, (uint32_t)(n - start)); }

/* BITMAPINFOHEADER (+ palette) for a w×h icon at bpp */
static void bih(int w, int h, int bpp, int ncol)
{
	p32(40); p32(w); p32(2 * h); p16(1); p16(bpp); p32(0); p32(0); p32(0); p32(0); p32(ncol); p32(0);
}
static void pad(size_t row_start) { while ((n - row_start) % 4) p8(0); }

static void bmp32(int i, int w, int h, uint32_t argb, int with_mask, int zero_alpha)
{
	size_t s = n;
	begin(i);
	bih(w, h, 32, 0);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			p32(zero_alpha ? argb & 0xffffff : argb);
	if (with_mask)
		for (int y = 0; y < h; y++) {	/* bottom-up: y = 0 is the bottom row; left half transparent */
			size_t r = n;
			for (int b = 0; b < (w + 7) / 8; b++) {
				int v = 0;
				for (int k = 0; k < 8; k++)
					if (b * 8 + k < w / 2)
						v |= 0x80 >> k;
				p8(v);
			}
			pad(r);
		}
	end(i, s);
}

int main(void)
{
	ico_img_t im;
	/* 1. picks the 32 px 32-bpp BMP over 16 px ones; colours + alpha straight through */
	dir(3);
	bmp32(0, 16, 16, 0xff0000ff, 1, 0);
	bmp32(1, 32, 32, 0x80ff8000, 1, 0);
	bmp32(2, 16, 16, 0xff00ff00, 1, 0);
	CHK(ico_is(buf, n));
	CHK(ico_decode(buf, n, &im) == ICO_RGBA && im.w == 32 && im.h == 32);
	if (im.rgba) {
		CHK(im.rgba[0] == 0xff && im.rgba[1] == 0x80 && im.rgba[2] == 0x00 && im.rgba[3] == 0x80);	/* R G B A */
		free(im.rgba);
	}
	/* 2. a 32-bpp icon whose alpha is all 0 uses its AND mask */
	dir(1);
	bmp32(0, 16, 16, 0xff112233, 1, 1);
	CHK(ico_decode(buf, n, &im) == ICO_RGBA);
	if (im.rgba) {
		CHK(im.rgba[3] == 0 && im.rgba[(15) * 4 + 3] == 255);	/* left half transparent, right opaque */
		CHK(im.rgba[0] == 0x11 && im.rgba[1] == 0x22 && im.rgba[2] == 0x33);
		free(im.rgba);
	}
	/* 3. 8-bpp paletted, bottom-up rows: the top row is palette entry 2, the rest entry 1 */
	dir(1);
	size_t s = n;
	begin(0);
	bih(4, 4, 8, 3);
	p32(0x000000); p32(0xff0000); p32(0x00ff00);	/* 0xRRGGBB (stored B G R x): black, red, green */
	for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) p8(y == 3 ? 2 : 1);
	for (int y = 0; y < 4; y++) p32(0);	/* mask: all opaque */
	end(0, s);
	CHK(ico_decode(buf, n, &im) == ICO_RGBA && im.w == 4 && im.h == 4);
	if (im.rgba) {
		CHK(im.rgba[0] == 0 && im.rgba[1] == 0xff && im.rgba[3] == 255);	/* top-left green */
		CHK(im.rgba[4 * 4] == 0xff && im.rgba[4 * 4 + 1] == 0);		/* second row red */
		free(im.rgba);
	}
	/* 4. 4-bpp and 1-bpp nibbles / bits in the right order */
	dir(1);
	s = n;
	begin(0);
	bih(2, 1, 4, 16);
	for (int i = 0; i < 16; i++) p32(i == 5 ? 0xff0000 : i == 10 ? 0x0000ff : 0);
	p8(0x5a); p8(0); p8(0); p8(0);	/* left = 5 (red), right = 10 (blue) */
	p32(0);
	end(0, s);
	CHK(ico_decode(buf, n, &im) == ICO_RGBA);
	if (im.rgba) {
		CHK(im.rgba[0] == 0xff && im.rgba[2] == 0 && im.rgba[6] == 0xff && im.rgba[4] == 0);
		free(im.rgba);
	}
	dir(1);
	s = n;
	begin(0);
	bih(8, 1, 1, 2);
	p32(0); p32(0xffffff);
	p8(0x80); p8(0); p8(0); p8(0);	/* only x = 0 white */
	p32(0);
	end(0, s);
	CHK(ico_decode(buf, n, &im) == ICO_RGBA);
	if (im.rgba) {
		CHK(im.rgba[0] == 0xff && im.rgba[4] == 0);
		free(im.rgba);
	}
	/* 5. an embedded PNG (only its IHDR is looked at) wins at the same size over a BMP, at a bigger size anyway */
	dir(2);
	bmp32(0, 32, 32, 0xff000000, 1, 0);
	s = n;
	begin(1);
	const uint8_t png[] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n', 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 64, 0, 0, 0, 64, 8, 6, 0, 0, 0 };
	memcpy(buf + n, png, sizeof(png));
	n += sizeof(png);
	end(1, s);
	CHK(ico_decode(buf, n, &im) == ICO_PNG && im.w == 64 && im.png == buf + s && im.png_len == sizeof(png));
	/* 6. hostile: truncated rows, offsets past the end, absurd sizes, too-big BMPs, bad counts — refused, no reads past n */
	dir(1);
	bmp32(0, 32, 32, 0xffffffff, 0, 0);
	size_t full = n;
	for (size_t cut = 0; cut < full; cut += 7) {
		uint8_t *c = malloc(cut ? cut : 1);
		memcpy(c, buf, cut);
		int k = ico_decode(c, cut, &im);
		free(im.rgba);
		CHK(k == ICO_NONE || cut >= 6 + 16 + 40 + 32 * 32 * 4);
		free(c);
	}
	dir(1);
	at32(ent[0] + 8, 0xffffffff);
	at32(ent[0] + 12, 0xfffffff0);
	CHK(ico_decode(buf, n, &im) == ICO_NONE);
	dir(1);
	s = n;
	begin(0);
	bih(4096, 4096, 32, 0);
	end(0, s);
	CHK(ico_decode(buf, n, &im) == ICO_NONE);
	dir(1);
	s = n;
	begin(0);
	bih(16, 16, 8, 0x7fffffff);	/* palette count bigger than the depth allows: 256, then the rows are missing */
	end(0, s);
	CHK(ico_decode(buf, n, &im) == ICO_NONE);
	n = 0; p16(0); p16(1); p16(0);
	CHK(!ico_is(buf, n) && ico_decode(buf, n, &im) == ICO_NONE);
	n = 0; p16(0); p16(2); p16(1);	/* a cursor */
	CHK(!ico_is(buf, n));
	if (!fails)
		printf("ico_test ok\n");
	return fails != 0;
}
