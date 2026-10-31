/* imgdec.c — JPEG (baseline) / PNG / ICO → scaled lv_image_dsc_t, for the net_bg worker thread. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "lvgl.h"

/* decoded at @2x of its own size, fitted into max_w × max_h; NULL + *err on failure. Free with img_free once no
 * lv_image shows it (Orchard runs without LVGL's image caches). */
lv_image_dsc_t *img_decode(const void *data, size_t len, int max_w, int max_h, const char **err);
/* favicons: pixel for pixel (never enlarged), shrunk to fit max × max; *natural = the source's larger side */
lv_image_dsc_t *img_decode_1x(const void *data, size_t len, int max, int *natural, const char **err);
void img_free(lv_image_dsc_t *d);
/* baseline JPEG → XRGB8888 rows of `stride` px at its own size (no scaling); false = broken, or bigger than
 * max_w × max_h (or the stride). Any thread. */
bool img_jpeg_xrgb(const void *data, size_t len, uint32_t *dst, int stride, int max_w, int max_h, int *w, int *h);
