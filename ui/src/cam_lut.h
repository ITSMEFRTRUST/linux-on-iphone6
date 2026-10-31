/* Camera filters: a 3D LUT (.cube) graded into every saved photo, like iOS's photo filters (cam_lut.c).
 * No LVGL here. Loading parses a .cube once into a compact fixed-point table; applying it is tetrahedral
 * interpolation over XRGB8888 in place. Files: $ORCHARD_DATA/luts/<file> first (tests / overrides), then
 * /usr/share/orchard/luts/<file> (the boot image). A missing or broken file = Original (logged once, never fatal). */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { CAM_FILTER_ORIGINAL, CAM_FILTER_WARM, CAM_FILTER_VIVID, CAM_FILTER_N };
#define CAM_FILTER_DEFAULT CAM_FILTER_WARM

const char *cam_filter_label(int f);		/* "ORIGINAL", "WARM", "VIVID" (the strip, iOS-style caps) */
const char *cam_filter_key(int f);		/* "original", "warm", "vivid" (prefs key camera_filter) */
int cam_filter_from_key(const char *key);	/* unknown / NULL = CAM_FILTER_DEFAULT */
void cam_filter_set(int f);			/* the filter the next photos get (UI thread) */
int cam_filter_get(void);

typedef struct cam_lut cam_lut_t;
/* f's table: loads it on first use — BLOCKS (file read + parse): never call this on the UI thread.
 * NULL = Original, or no usable file */
const cam_lut_t *cam_lut_for(int f);
/* non-blocking: f's table if it is loaded; otherwise NULL and a background load starts (UI thread) */
const cam_lut_t *cam_lut_peek(int f);

/* the save path (cam_save.c): grade a photo with the current filter (remembered per thread), then its thumbnail
 * with the same one */
void cam_lut_photo(uint32_t *px, size_t n);
void cam_lut_thumb(uint32_t *px, size_t n);

/* low level (tests, bench). cam_lut_parse reads exactly len bytes of text (no NUL needed: it parses a terminated
 * copy); NULL + why for a broken cube (non-finite DOMAIN / INPUT_RANGE included) */
cam_lut_t *cam_lut_load(const char *path, char *why, size_t wn);
cam_lut_t *cam_lut_parse(const char *text, size_t len, char *why, size_t wn);
void cam_lut_free(cam_lut_t *l);
int cam_lut_size(const cam_lut_t *l);
/* XRGB8888 in place (the X byte is kept); l == NULL = nothing to do */
void cam_lut_apply(const cam_lut_t *l, uint32_t *px, size_t n);
