/* Camera internals (cam_conv.c, cam_src.c, cam_dev.c, cam_save.c; the UI is app_camera.c). No LVGL in here except
 * cam_dev's frame hand-off: everything runs on the camera's worker threads. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- cam_conv.c: NV12 → XRGB8888, oriented + scaled to cover a box (centre crop), nearest neighbour, with the base
 * tone (an S-curve on the luma + a chroma gain: cam_conv_set_tone). The source (the ISP's buffer) is only read row by
 * row in 64-byte loads, and only the rows the picture uses; each used source row is then converted along the row,
 * 16 samples per NEON step (tbl gathers from 32 / 64-byte windows, 16-bit maths), into the destination row, or for
 * rotated (90 / 270) pictures in blocks of CAM_CONV_K source rows (= destination columns) into a row buffer that is
 * transposed into the destination. Scalar fallback with the same numbers (no NEON, or shrinking > 2×). ---- */
typedef struct cam_orient {
	int rot;		/* clockwise: 0, 90, 180, 270 (sensor → portrait screen) */
	bool mirror;		/* left-right flip after rotating (front camera, like iOS) */
	bool full_range;	/* Y 0..255 (JPEG / "420f"); false = video range 16..235 ("420v", BT.601) */
} cam_orient_t;

#define CAM_CONV_K 16
typedef struct cam_conv {
	int sw, sh, stride;	/* source as delivered (stride = bytes per Y / CbCr row) */
	cam_orient_t o;
	int dw, dh;		/* destination box */
	int ow, oh;		/* source size after rotating */
	bool transpose, flipx, flipy;
	int xa, xb, ya, yb;	/* visible window in rotated-picture coordinates, even-aligned */
	int c0, c1;		/* source columns copied out of each source row used (the window, 64-byte aligned) */
	int *srow;		/* the source row of each destination column (rotated) / row (not rotated) */
	int *yo, *co;		/* the other axis: offset of its Y / CbCr pair sample in a copied row */
	uint8_t *stage;		/* copied rows (+ 64 bytes each): K (rotated) / 1 Y rows, then as many CbCr rows */
	uint32_t *tmp;		/* rotated: K converted source rows of up to 256 samples, before the transpose */
	bool tbl;		/* the NEON gather plan fits every 16-sample chunk */
	int *ybase, *cbase;	/* per chunk: Y window / CbCr window start in the copied row */
	uint8_t *yidx, *cidx;	/* per chunk: 16 Y / 32 CbCr byte positions in those windows */
	struct coefs *k;	/* tone LUT + chroma coefficients (cam_conv.c) */
} cam_conv_t;

/* base tone for conversions set up from now on: contrast % (S-curve strength; 0 = none), saturation % (100 = the
 * ISP's). Default 30 / 120, or ORCHARD_CAMERA_TONE=contrast,saturation ("0,100" = the plain ISP picture) */
void cam_conv_set_tone(int contrast, int sat);
extern bool cam_conv_scalar;	/* tests: no NEON */
bool cam_conv_init(cam_conv_t *c, int sw, int sh, int stride, cam_orient_t o, int dw, int dh);
void cam_conv_free(cam_conv_t *c);
/* dst: dh rows of dst_stride pixels */
void cam_convert(cam_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride);
uint32_t cam_yuv_px(int y, int u, int v, bool full_range);	/* one pixel, with the current tone (tests) */
/* the orientation the app uses for a w × h source: landscape sensors turn 90° clockwise into portrait, the front
 * camera is mirrored; ORCHARD_CAMERA_ROT=0|90|180|270 and ORCHARD_CAMERA_RANGE=full|video override */
cam_orient_t cam_default_orient(int w, int h, int camera);
/* oriented picture pixel (x, y) → source pixel (the fake source draws its text through this, upright) */
void cam_orient_src_xy(cam_orient_t o, int w, int h, int x, int y, int *sx, int *sy);

/* ---- cam_src.c: frame sources behind one interface — /dev/isp0 (isp_uapi.h) or the fake one (animated test
 * frames, ORCHARD_CAMERA=fake; default in the sdl / snap builds). Blocking calls: camera thread only. ---- */
typedef struct cam_info {
	int w, h, stride, nbufs;
	char sensor[40];
} cam_info_t;
typedef struct cam_frame {
	int index;
	uint32_t seq;
	uint64_t ts_ns;
	const uint8_t *y, *uv;
} cam_frame_t;
typedef struct cam_src cam_src_t;
/* NULL + why (for the "Camera Unavailable" screen) */
cam_src_t *cam_src_open(char *why, size_t n);
const cam_info_t *cam_src_info(cam_src_t *s);
enum cam_mode { CAM_MODE_VIEW, CAM_MODE_STILL };	/* 4:3 viewfinder preset / full-resolution preset */
/* camera 0 rear, 1 front; a driver without the preset flags streams its default preset for both modes */
bool cam_src_start(cam_src_t *s, int camera, enum cam_mode mode, char *why, size_t n);
/* after a START: CAM_MODE_STILL gives a bigger picture than the viewfinder's (the driver took the preset flags) —
 * false for the fake source and for a driver before the flags (then a photo is just the next frame). Asked afresh
 * after every cam_src_open */
bool cam_src_has_presets(cam_src_t *s);
void cam_src_stop(cam_src_t *s);
/* 1 = frame, 0 = nothing within timeout_ms, -1 = error (why) */
int cam_src_next(cam_src_t *s, cam_frame_t *f, int timeout_ms, char *why, size_t n);
void cam_src_release(cam_src_t *s, const cam_frame_t *f);
void cam_src_close(cam_src_t *s);
bool cam_src_fake(void);	/* which one cam_src_open will use */

/* ---- cam_save.c: photos in $ORCHARD_DATA/Photos (IMG_0001.JPG …; .PNG from a build without libjpeg) ---- */
/* one frame, oriented, as photo number num; path = the file written. Any thread. false + why */
bool cam_save_photo(char *path, size_t pn, int num, const uint8_t *nv12, int w, int h, int stride, cam_orient_t o,
		    char *why, size_t n);
const char *cam_photos_dir(void);	/* created on first use (first call from the UI thread) */
int cam_photos_list(char ***paths);	/* full paths, oldest → newest; free with cam_photos_free */
void cam_photos_free(char **paths, int n);
int cam_photos_next_number(void);	/* after the highest IMG_<n> there */
/* thumbnails: Photos/.thumbs/IMG_<n>.bgra, CAM_THUMB² XRGB8888 (written with the photo, removed with it) */
bool cam_thumb_write(const char *photo, const uint32_t *px);
bool cam_thumb_read(const char *photo, uint32_t *px);
bool cam_photo_delete(const char *photo);	/* the photo + its thumbnail */

/* ---- cam_dev.c: the camera worker thread. The UI only says what it wants and picks finished frames up; nothing
 * here blocks the caller (open / ioctl / read all happen on the worker, which sleeps while the camera is off). ---- */
enum cam_state { CAM_OFF, CAM_STARTING, CAM_RUNNING, CAM_ERROR };
/* viewfinder on (camera 0 rear / 1 front, frames dw × dh XRGB8888) or off (STOP + close: the sensor powers down;
 * a full-resolution still under way is finished and saved first, a tap not picked up yet is dropped).
 * Before turning it off or changing dw / dh the UI must stop showing cam_dev_frame()'s buffers: they are freed. */
void cam_dev_run(bool on, int camera, int dw, int dh);
void cam_dev_retry(void);	/* after CAM_ERROR: open + start again */
/* fn (or NULL) is called on the camera thread each time a finished frame is ready for cam_dev_frame (hal_wake) */
void cam_dev_notify(void (*fn)(void));
enum cam_state cam_dev_state(char *why, size_t n, cam_info_t *info);	/* why / info may be NULL */
/* newest finished frame, NULL = none since the last call; the buffer stays untouched until the next call that
 * returns non-NULL (triple buffering) */
const uint32_t *cam_dev_frame(void);
/* take a photo (saved on its own thread): the next frame, or with a driver that has the presets a full-resolution
 * still (cam_dev.c take_still: up to a few seconds). false = not taken (not running, or a photo is still being
 * taken: every tap until then would be the same photo) */
bool cam_dev_capture(void);
/* a full-resolution still is being taken (from the tap until the viewfinder is back): the shutter and the
 * last-photo thumbnail are greyed */
bool cam_dev_busy(void);
/* front photos: true = saved as the (mirrored) viewfinder shows them, false = un-mirrored (what the others see).
 * Settings › Camera › Mirror Front Camera (pref camera_mirror, default 1); read when a photo is taken */
void cam_dev_mirror(bool on);
#define CAM_THUMB 92	/* px, square */
/* the photo's outcome, once: 1 saved (path + a CAM_THUMB² cover thumbnail of it), -1 failed (why), 0 nothing yet */
int cam_dev_capture_result(char *path, size_t pn, uint32_t *thumb, char *why, size_t wn);

/* ---- cam_focus.c: tap to focus. The UI posts a request (the newest wins); cam_dev's worker sends it between two
 * frames through cam_src_focus. Rear camera: AF window + exposure window at the tap, one AF scan (POINT, LOCK) or
 * continuous AF (CONTINUOUS, the driver's default at stream start); front camera (fixed focus): exposure only. ---- */
enum cam_focus_mode { CAM_FOCUS_CONTINUOUS, CAM_FOCUS_POINT, CAM_FOCUS_LOCK };
void cam_focus_request(enum cam_focus_mode m, int x, int y);	/* UI thread; (x, y) in the dw × dh viewfinder box */
/* camera thread: the viewfinder was STARTed again after a photo. A held lens (a tap's scan, AF lock) is held again
 * (CAF off on the same window), unless a newer request is waiting. A driver that replays the request itself at START
 * (apple-isp n61-isp-fix) already did; then this is a no-op for the lens */
void cam_focus_restarted(int camera);
/* camera thread, once per loop: sends the pending request to the camera that streams (camera -1 = none) */
void cam_focus_service(cam_src_t *s, int camera, const cam_info_t *in, cam_orient_t o, int dw, int dh);
/* box point (x, y) → window {x, y, w, h} in ‰ of the sw × sh source (sensor orientation), size ‰ of its short side */
void cam_focus_window(cam_orient_t o, int sw, int sh, int dw, int dh, int x, int y, int size, int win[4]);
/* cam_src.c: one ISP_IOC_FOCUS (mode = ISP_FOCUS_*, win NULL = keep, ae = meter there too); 0 or an errno
 * (ENOTTY = driver without focus control); *lens = the lens position after it (-1 unknown). Camera thread. */
int cam_src_focus(cam_src_t *s, int mode, const int win[4], bool ae, int *lens);
