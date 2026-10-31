/* ico.c — Windows .ico reader for favicons (pure C, no LVGL, any thread; fuzzed: test/unit/ico_fuzz.c).
 * Picks the best image in the directory (largest, PNG or deeper BMP on a tie) and hands it back either as the
 * embedded PNG's bytes or as BMP pixels decoded to RGBA (1 / 4 / 8 / 24 / 32 bpp, AND mask applied). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ICO_MAX_DIM 256		/* BMP entries; embedded PNGs may be up to ICO_MAX_PNG_DIM */
#define ICO_MAX_PNG_DIM 1024

enum { ICO_NONE, ICO_PNG, ICO_RGBA };
typedef struct ico_img {
	int kind;		/* ICO_* */
	const uint8_t *png;	/* ICO_PNG: the PNG inside the buffer passed in */
	size_t png_len;
	uint8_t *rgba;		/* ICO_RGBA: w × h × 4 (R, G, B, A, straight alpha, top row first), malloc'd */
	int w, h;		/* the chosen image's size (ICO_PNG: from its IHDR) */
} ico_img_t;

bool ico_is(const void *data, size_t len);	/* starts like an icon directory */
int ico_decode(const void *data, size_t len, ico_img_t *out);	/* → out->kind; free(out->rgba) afterwards */
