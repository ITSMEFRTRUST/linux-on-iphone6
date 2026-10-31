/* Camera: iOS-style photo camera on the A8 ISP (/dev/isp0, BRIEF-CAMERA-STREAM.md v1).
 * Top bar (flash placeholder + filters: Original / Warm / Vivid, cam_lut.c), the 3:4 viewfinder (sensor frames turned upright and cover-scaled to
 * 750 × 1000 by the camera thread, cam_dev.c / cam_conv.c; this file only swaps finished frames into one lv_image
 * and invalidates it), the mode strip (PHOTO; VIDEO greyed), shutter, last-photo thumbnail, front /
 * rear flip. Shutter = a photo saved as a JPEG in $ORCHARD_DATA/Photos (cam_save.c) + a black blink + a tap: a
 * full-resolution still when the driver has the presets (cam_dev.c take_still; the shutter and the last-photo
 * thumbnail are greyed and take no taps until the viewfinder is back; closing the camera meanwhile still saves it),
 * else the next frame.
 * The thumbnail opens a minimal viewer (swipe between photos, delete).
 * Battery: the camera runs only while its viewfinder is on screen — screen off, locked, the app switcher, the viewer
 * or closing the app = STOP + close (the sensor powers down); a running viewfinder holds auto-lock ("camera"
 * lease). Nothing here blocks: open / ioctl / read live on the camera thread; errors show "Camera Unavailable"
 * with the reason and Try Again.
 * The camera UI is always dark, as on iOS: white on black whatever the theme (the theme's yellow marks the mode).
 * Previews: ORCHARD_CAMERA=fake (default in sdl / snap) = animated test frames through the same path; =none /
 * nostream / fail / stall = the error screens. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "orchard.h"
#include "cam.h"
#include "cam_lut.h"
#include "imgdec.h"

void lv_image_cache_drop(const void *src);	/* LVGL, only in its private headers */

#define TOP_H 120		/* status bar + flash / settings */
#define VF_Y TOP_H
#define VF_W SCR_W
#define VF_H 1000		/* 3:4, like iOS's photo mode */
#define BOT_Y (VF_Y + VF_H)
#define SHUT_D 132
#define SHUT_CY (SCR_H - BOTTOM_PAD - SHUT_D / 2 - 6)
#define SIDE_D 92		/* thumbnail / flip button */
#define RUN_MS 15		/* frame pick-up while the viewfinder runs */
#define IDLE_MS 100		/* otherwise (starting, error, viewer) */
#define LOCKED_MS 1000		/* the lock screen is over it (the camera is off): a photo's thumbnail, nothing else */

#define WHITE 0xffffff
#define BLACK 0x000000
#define GREY 0x8e8e93
#define DARK 0x2c2c2e

#define G_FLASH_OFF "\xf3\xb0\x89\x83"	/* md-flash-off */
#define G_CAM_OFF "\xf3\xb0\x97\x9f"	/* md-camera-off */
#define G_FLIP "\xef\x80\xa1"		/* fa-refresh: iOS's flip arrows */
#define G_DELETE "\xf3\xb0\x86\xb4"	/* md-delete */

static lv_obj_t *root, *vf_img, *blink, *msg, *msg_title, *msg_text, *msg_btn, *shutter, *thumb_box, *thumb_img;
static lv_obj_t *flip_btn;
static lv_timer_t *tm;
static uint32_t tm_period;
static lv_image_dsc_t vf_dsc, thumb_dsc;
static uint32_t thumb_px[CAM_THUMB * CAM_THUMB];
static bool have_thumb;
static int camera;		/* 0 rear, 1 front (kept while closed) */
static bool running;		/* we asked cam_dev for the viewfinder */
static bool showing;		/* vf_img shows a cam_dev buffer */
static bool shutter_ready;	/* the shutter as drawn: white + takes taps, or greyed + disabled */
static bool thumb_busy;		/* the last-photo thumbnail as drawn: greyed + disabled while a still is under way */
static enum cam_state shown_state = CAM_OFF;
static uint32_t starting_at, awake_at;
static bool cc_was_open;
static char last_why[160];

static void viewer_open(void);
static bool viewer_up(void);
static void viewer_close(void);
static void viewer_tick(void);

/* ---------- viewfinder ---------- */

static void vf_detach(void)	/* stop showing cam_dev's buffers (before they can be freed) */
{
	if (!vf_img)
		return;
	if (showing) {
		lv_image_set_src(vf_img, NULL);
		lv_obj_add_flag(vf_img, LV_OBJ_FLAG_HIDDEN);
	}
	showing = false;
}

static void vf_show(const uint32_t *px)
{
	vf_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	vf_dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
	vf_dsc.header.w = VF_W;
	vf_dsc.header.h = VF_H;
	vf_dsc.header.stride = VF_W * 4;
	vf_dsc.data = (const uint8_t *)px;
	vf_dsc.data_size = VF_W * VF_H * 4;
	lv_image_cache_drop(&vf_dsc);
	if (lv_image_get_src(vf_img) != &vf_dsc)
		lv_image_set_src(vf_img, &vf_dsc);
	else
		lv_obj_invalidate(vf_img);
	lv_timer_ready(lv_display_get_refr_timer(NULL));	/* drawn on this pass, not up to 16 ms later */
	if (lv_obj_has_flag(vf_img, LV_OBJ_FLAG_HIDDEN))
		lv_obj_remove_flag(vf_img, LV_OBJ_FLAG_HIDDEN);
	showing = true;
}

/* should the camera run now: our viewfinder is what's on screen */
static bool wanted(void)
{
	return root && shell_current_app() == &app_camera && !shell_screen_off() && !shell_locked() &&
	       !shell_switcher_open() && !power_dark() && !viewer_up();
}

static void focus_reset(void);

static void cam_run(bool on)
{
	if (on == running)
		return;
	if (!on) {
		vf_detach();
		if (awake_at) {
			power_keep_awake("camera", false);	/* the auto-lock countdown starts now */
			awake_at = 0;
		}
	}
	running = on;
	if (on)	/* (off closed the camera: the driver forgot any tap / AF lock, so the UI does too) */
		focus_reset();
	cam_dev_run(on, camera, VF_W, VF_H);
	if (on)
		starting_at = lv_tick_get();
}

/* ---------- tap to focus (cam_focus.c sends it): iOS's yellow square ----------
 * Tap: focus + exposure on that point (one AF scan), the square shrinks in, pulses, fades; a few seconds later AF
 * goes back to continuous on the same point. Long press (rear): the same scan, then the focus stays put ("AF LOCK")
 * until the next tap or a camera flip. The front lens is fixed-focus: there a tap only moves the exposure point. */
#define FOCUS_BOX 150		/* px, the square at rest */
#define FOCUS_FADE_MS 1500	/* after the tap: the square fades */
#define FOCUS_REVERT_MS 4000	/* after the tap: continuous AF again (on that point) */
static lv_obj_t *focus_sq, *focus_lock;
static lv_timer_t *focus_tm;
static uint32_t focus_at;	/* lv_tick of the last tap, 0 = none */
static int focus_x, focus_y;	/* that tap in viewfinder coordinates */
static bool focus_locked, focus_fading, focus_revert;

static void focus_place(int side)
{
	int x = focus_x - side / 2, y = focus_y - side / 2;
	x = x < 0 ? 0 : x > VF_W - side ? VF_W - side : x;
	y = y < 0 ? 0 : y > VF_H - side ? VF_H - side : y;
	lv_obj_set_size(focus_sq, side, side);
	lv_obj_set_pos(focus_sq, x, VF_Y + y);
}
static void focus_size_cb(void *o, int32_t v) { (void)o; focus_place(v); }
static void focus_opa_cb(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }
static void focus_hidden(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }
static void focus_pulse_up(lv_anim_t *a) { motion_anim(a->var, focus_opa_cb, LV_OPA_40, LV_OPA_COVER, 140, NULL); }
static void focus_pulse(lv_anim_t *a) { motion_anim(a->var, focus_opa_cb, LV_OPA_COVER, LV_OPA_40, 140, focus_pulse_up); }

static void focus_reset(void)	/* camera flip / open: continuous AF on the centre, no square */
{
	focus_at = 0;
	focus_locked = focus_fading = focus_revert = false;
	if (focus_sq)
		lv_obj_add_flag(focus_sq, LV_OBJ_FLAG_HIDDEN);
	if (focus_lock)
		lv_obj_add_flag(focus_lock, LV_OBJ_FLAG_HIDDEN);
	cam_focus_request(CAM_FOCUS_CONTINUOUS, VF_W / 2, VF_H / 2);
}

static void focus_tap(lv_event_t *e)
{
	if (!showing || cam_dev_state(NULL, 0, NULL) != CAM_RUNNING)
		return;
	lv_point_t p;
	lv_area_t a;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_obj_get_coords(vf_img, &a);
	focus_x = p.x - a.x1, focus_y = p.y - a.y1;
	bool lock = lv_event_get_code(e) == LV_EVENT_LONG_PRESSED && camera == 0;
	cam_focus_request(lock ? CAM_FOCUS_LOCK : CAM_FOCUS_POINT, focus_x, focus_y);
	focus_at = lv_tick_get();
	if (!focus_at)
		focus_at = 1;
	focus_locked = lock;
	focus_revert = !lock && camera == 0;
	focus_fading = false;
	ui_haptic_tap();
	lv_obj_remove_flag(focus_sq, LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_style_opa(focus_sq, LV_OPA_COVER, 0);
	lv_obj_move_foreground(focus_sq);
	focus_place(FOCUS_BOX * 3 / 2);
	motion_anim(focus_sq, focus_size_cb, FOCUS_BOX * 3 / 2, FOCUS_BOX, 220, focus_pulse);
	if (lock)
		lv_obj_remove_flag(focus_lock, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(focus_lock, LV_OBJ_FLAG_HIDDEN);
}

static void focus_timer(lv_timer_t *t)
{
	(void)t;
	if (!focus_at)
		return;
	uint32_t el = lv_tick_elaps(focus_at);
	if (!focus_locked && !focus_fading && el > FOCUS_FADE_MS) {
		focus_fading = true;
		motion_anim(focus_sq, focus_opa_cb, LV_OPA_COVER, LV_OPA_TRANSP, 350, focus_hidden);
	}
	if (focus_revert && el > FOCUS_REVERT_MS) {
		focus_revert = false;
		cam_focus_request(CAM_FOCUS_CONTINUOUS, focus_x, focus_y);
	}
}

static void focus_build(void)
{
	lv_obj_add_flag(vf_img, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(vf_img, focus_tap, LV_EVENT_SHORT_CLICKED, NULL);
	lv_obj_add_event_cb(vf_img, focus_tap, LV_EVENT_LONG_PRESSED, NULL);
	focus_sq = box(root);
	lv_obj_set_style_bg_opa(focus_sq, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_color(focus_sq, C(pal.yellow), 0);
	lv_obj_set_style_border_width(focus_sq, 3, 0);
	lv_obj_set_style_border_opa(focus_sq, LV_OPA_COVER, 0);
	lv_obj_remove_flag(focus_sq, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(focus_sq, LV_OBJ_FLAG_HIDDEN);
	focus_lock = box(root);
	lv_obj_set_style_bg_color(focus_lock, C(pal.yellow), 0);
	lv_obj_set_style_bg_opa(focus_lock, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(focus_lock, 8, 0);
	lv_obj_set_style_pad_hor(focus_lock, 14, 0);
	lv_obj_set_style_pad_ver(focus_lock, 6, 0);
	lv_obj_set_size(focus_lock, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	text(focus_lock, "AF LOCK", font(F_TEXT_SEMI, 24), BLACK);
	lv_obj_align(focus_lock, LV_ALIGN_TOP_MID, 0, VF_Y + 24);
	lv_obj_add_flag(focus_lock, LV_OBJ_FLAG_HIDDEN);
	focus_tm = lv_timer_create(focus_timer, 100, NULL);
	focus_reset();
}

static void focus_close(void)
{
	if (focus_tm)
		lv_timer_delete(focus_tm);
	focus_tm = NULL;
	focus_sq = focus_lock = NULL;
	focus_at = 0;
}

/* ---------- thumbnail ---------- */

static void thumb_set(const uint32_t *px)
{
	if (px && px != thumb_px)
		memcpy(thumb_px, px, sizeof(thumb_px));
	have_thumb = px != NULL;
	if (!thumb_img)
		return;
	thumb_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
	thumb_dsc.header.cf = LV_COLOR_FORMAT_XRGB8888;
	thumb_dsc.header.w = thumb_dsc.header.h = CAM_THUMB;
	thumb_dsc.header.stride = CAM_THUMB * 4;
	thumb_dsc.data = (const uint8_t *)thumb_px;
	thumb_dsc.data_size = sizeof(thumb_px);
	lv_image_cache_drop(&thumb_dsc);
	if (have_thumb) {
		lv_image_set_src(thumb_img, &thumb_dsc);
		lv_obj_invalidate(thumb_img);
		lv_obj_remove_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_image_set_src(thumb_img, NULL);
		lv_obj_add_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
	}
}

/* the newest photo's thumbnail (its sidecar), or none */
static void thumb_load_last(void)
{
	char **v;
	int n = cam_photos_list(&v);
	bool ok = n > 0 && cam_thumb_read(v[n - 1], thumb_px);
	if (n > 0 && !ok)	/* a photo without a sidecar: a grey tile still opens the viewer */
		for (int i = 0; i < CAM_THUMB * CAM_THUMB; i++)
			thumb_px[i] = 0xff000000u | DARK;
	thumb_set(n > 0 ? thumb_px : NULL);
	cam_photos_free(v, n);
}

/* ---------- the message over the viewfinder (starting / unavailable) ---------- */

static void retry_cb(lv_event_t *e)
{
	(void)e;
	ui_haptic_tap();
	cam_dev_retry();
	starting_at = lv_tick_get();
}

/* NULL title = hidden; btn = Try Again */
static void msg_show(const char *glyph_on, const char *title, const char *body, bool btn)
{
	if (!msg)
		return;
	if (!title) {
		if (!lv_obj_has_flag(msg, LV_OBJ_FLAG_HIDDEN))
			lv_obj_add_flag(msg, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_t *g = lv_obj_get_child(msg, 0);
	if (glyph_on)
		lv_obj_remove_flag(g, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
	if (strcmp(lv_label_get_text(msg_title), title))
		lv_label_set_text(msg_title, title);
	if (strcmp(lv_label_get_text(msg_text), body ? body : ""))
		lv_label_set_text(msg_text, body ? body : "");
	if (btn)
		lv_obj_remove_flag(msg_btn, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(msg_btn, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(msg, LV_OBJ_FLAG_HIDDEN);
}

static void build_msg(void)
{
	msg = box(root);
	lv_obj_set_pos(msg, 0, VF_Y);
	lv_obj_set_size(msg, VF_W, VF_H);
	lv_obj_set_flex_flow(msg, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(msg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(msg, 24, 0);
	ui_icon_line(msg, G_CAM_OFF, font(F_MONO, 120), GREY);
	msg_title = text(msg, "", font(F_TEXT_SEMI, 40), WHITE);
	msg_text = text(msg, "", font(F_TEXT, 30), GREY);
	lv_obj_set_width(msg_text, 680);
	lv_label_set_long_mode(msg_text, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_align(msg_text, LV_TEXT_ALIGN_CENTER, 0);
	msg_btn = ui_button(msg, "Try Again", true);
	lv_obj_add_event_cb(msg_btn, retry_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_add_flag(msg, LV_OBJ_FLAG_HIDDEN);
}

/* ---------- controls ---------- */

static void blink_opa(void *o, int32_t v) { lv_obj_set_style_bg_opa(o, (lv_opa_t)v, 0); }
static void blink_done(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

static void shutter_cb(lv_event_t *e)
{
	(void)e;
	/* not taken (starting, or a still under way: the shutter is greyed until it is done) = no blink, no tap */
	cam_dev_mirror(prefs_int("camera_mirror", 1));	/* Settings › Camera › Mirror Front Camera */
	if (cam_dev_state(NULL, 0, NULL) != CAM_RUNNING || !running || !cam_dev_capture())
		return;
	ui_haptic_tap();
	/* iOS's blink: the viewfinder goes black and comes back */
	lv_obj_remove_flag(blink, LV_OBJ_FLAG_HIDDEN);
	motion_anim(blink, blink_opa, LV_OPA_COVER, LV_OPA_TRANSP, 260, blink_done);
}

static void flip_cb(lv_event_t *e)
{
	(void)e;
	ui_haptic_tap();
	camera = !camera;
	focus_reset();
	if (running) {	/* same size: cam_dev restarts the sensor, the last frame stays up meanwhile */
		cam_dev_run(true, camera, VF_W, VF_H);
		starting_at = lv_tick_get();
	}
}

static void thumb_cb(lv_event_t *e)
{
	(void)e;
	if (have_thumb && !cam_dev_busy())	/* (mid-still it would open on the photo before the one being taken) */
		viewer_open();
}

/* ---------- filters (cam_lut.c): the top-right button (iOS's three rings, yellow while a filter is on) opens a
 * strip of live previews over the bottom of the viewfinder; the choice is pref camera_filter (default Warm) and
 * grades the saved photos on the save thread. The viewfinder itself stays ungraded (cam_dev.c's frames are
 * untouched); only the three small previews are graded here, from the shown frame every FS_MS. ---------- */

#define FS_H 236		/* the strip */
#define FS_Y (BOT_Y - FS_H)
#define FS_PV 150		/* a preview tile, px (the viewfinder's centre square, every 5th pixel) */
#define FS_MS 400		/* preview refresh while the strip is up: 2 × 22 k pixels, ~1.3 ms each on the A8 */

static lv_obj_t *fl_btn, *fl_ring[3], *fs, *fs_tile[CAM_FILTER_N], *fs_img[CAM_FILTER_N], *fs_name[CAM_FILTER_N];
static lv_image_dsc_t fs_dsc[CAM_FILTER_N];
static uint32_t *fs_px;		/* CAM_FILTER_N previews of FS_PV² (only while the strip is up) */
static uint32_t fs_at;

static void filter_show(void)
{
	const int cur = cam_filter_get();
	for (int i = 0; i < 3; i++)
		lv_obj_set_style_border_color(fl_ring[i], C(cur != CAM_FILTER_ORIGINAL ? pal.yellow : WHITE), 0);
	for (int f = 0; fs && f < CAM_FILTER_N; f++) {
		lv_obj_set_style_border_width(fs_tile[f], f == cur ? 4 : 0, 0);
		lv_obj_set_style_text_color(fs_name[f], C(f == cur ? pal.yellow : WHITE), 0);
	}
}

static void fs_refresh(void)
{
	fs_at = lv_tick_get();
	if (!fs_px || !showing || !vf_dsc.data)
		return;
	const uint32_t *src = (const uint32_t *)vf_dsc.data;	/* (untouched until the next cam_dev_frame) */
	const int step = VF_W / FS_PV, y0 = (VF_H - VF_W) / 2;
	for (int y = 0; y < FS_PV; y++)
		for (int x = 0; x < FS_PV; x++)
			fs_px[y * FS_PV + x] = src[(size_t)(y0 + y * step) * VF_W + (size_t)x * step];
	for (int f = 1; f < CAM_FILTER_N; f++) {
		uint32_t *d = fs_px + (size_t)f * FS_PV * FS_PV;
		memcpy(d, fs_px, sizeof(uint32_t) * FS_PV * FS_PV);
		cam_lut_apply(cam_lut_peek(f), d, FS_PV * FS_PV);	/* not loaded yet (or no file) = as is */
	}
	for (int f = 0; f < CAM_FILTER_N; f++) {
		lv_image_cache_drop(&fs_dsc[f]);
		lv_obj_invalidate(fs_img[f]);
	}
}

static void fs_tile_cb(lv_event_t *e)
{
	const int f = (int)(intptr_t)lv_event_get_user_data(e);
	if (f == cam_filter_get())
		return;
	ui_haptic_tap();
	cam_filter_set(f);
	prefs_set_str("camera_filter", cam_filter_key(f));
	cam_lut_peek(f);	/* loads in the background now, not at the shutter */
	filter_show();
}

static void fs_close(void)
{
	if (!fs)
		return;
	lv_obj_delete(fs);
	fs = NULL;
	for (int f = 0; f < CAM_FILTER_N; f++)
		lv_image_cache_drop(&fs_dsc[f]);
	free(fs_px);
	fs_px = NULL;
}

static void fs_open(void)
{
	const size_t n = (size_t)FS_PV * FS_PV;
	if (fs || !(fs_px = malloc(sizeof(uint32_t) * n * CAM_FILTER_N)))
		return;
	for (size_t i = 0; i < n * CAM_FILTER_N; i++)
		fs_px[i] = 0xff000000u | DARK;
	fs = box(root);
	lv_obj_set_pos(fs, 0, FS_Y);
	lv_obj_set_size(fs, SCR_W, FS_H);
	lv_obj_set_style_bg_opa(fs, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(fs, C(BLACK), 0);
	for (int f = 0; f < CAM_FILTER_N; f++) {
		const int cx = SCR_W / 2 + (f - CAM_FILTER_N / 2) * (FS_PV + 64);
		fs_tile[f] = box(fs);
		lv_obj_set_size(fs_tile[f], FS_PV, FS_PV);
		lv_obj_set_pos(fs_tile[f], cx - FS_PV / 2, 20);
		lv_obj_set_style_border_color(fs_tile[f], C(pal.yellow), 0);
		lv_obj_set_style_border_post(fs_tile[f], true, 0);	/* over the picture */
		lv_obj_set_ext_click_area(fs_tile[f], 24);
		lv_obj_add_event_cb(fs_tile[f], fs_tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)f);
		fs_dsc[f] = (lv_image_dsc_t){ .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_XRGB8888,
							  .w = FS_PV, .h = FS_PV, .stride = FS_PV * 4 },
					      .data = (const uint8_t *)(fs_px + n * f), .data_size = (uint32_t)(n * 4) };
		fs_img[f] = lv_image_create(fs_tile[f]);
		lv_image_set_src(fs_img[f], &fs_dsc[f]);
		fs_name[f] = text(fs, cam_filter_label(f), font(F_TEXT_SEMI, 22), WHITE);
		lv_obj_set_style_text_letter_space(fs_name[f], 1, 0);
		lv_obj_update_layout(fs_name[f]);
		lv_obj_set_pos(fs_name[f], cx - lv_obj_get_width(fs_name[f]) / 2, 20 + FS_PV + 16);
	}
	filter_show();
	fs_refresh();
}

static void fl_cb(lv_event_t *e)
{
	(void)e;
	ui_haptic_tap();
	if (fs)
		fs_close();
	else
		fs_open();
}

static void build_filters(void)
{
	static const int8_t at[3][2] = { { -8, -7 }, { 8, -7 }, { 0, 8 } };	/* three overlapping rings */
	fl_btn = box(root);
	lv_obj_set_size(fl_btn, 88, 88);
	lv_obj_set_pos(fl_btn, SCR_W - 64 - 44, STATUS_H + 40 - 44);
	lv_obj_set_ext_click_area(fl_btn, 12);
	lv_obj_add_event_cb(fl_btn, fl_cb, LV_EVENT_CLICKED, NULL);
	for (int i = 0; i < 3; i++) {
		fl_ring[i] = box(fl_btn);
		lv_obj_remove_flag(fl_ring[i], LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(fl_ring[i], 30, 30);
		lv_obj_set_style_radius(fl_ring[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_border_width(fl_ring[i], 3, 0);
		lv_obj_align(fl_ring[i], LV_ALIGN_CENTER, at[i][0], at[i][1]);
	}
	filter_show();
}

static lv_obj_t *label_at(lv_obj_t *p, const char *s, lv_font_t *f, uint32_t c, int cx, int cy)
{
	lv_obj_t *l = text(p, s, f, c);
	if ((unsigned char)s[0] >= 0xee)	/* a Nerd Font icon (private use area): centre its ink, not its advance */
		ui_glyph_fit(l);
	lv_obj_update_layout(l);
	lv_obj_set_pos(l, cx - lv_obj_get_width(l) / 2, cy - lv_obj_get_height(l) / 2);
	return l;
}

static void build_chrome(void)
{
	/* top: flash (a placeholder until the ISP has one) + filters */
	label_at(root, G_FLASH_OFF, font(F_MONO, 44), WHITE, 64, STATUS_H + 40);
	build_filters();
	/* mode strip: PHOTO under the shutter, VIDEO greyed out to its left */
	lv_obj_t *ph = label_at(root, "PHOTO", font(F_TEXT_SEMI, 26), pal.yellow, SCR_W / 2, BOT_Y + 30);
	lv_obj_set_style_text_letter_space(ph, 2, 0);
	lv_obj_t *vd = label_at(root, "VIDEO", font(F_TEXT_SEMI, 26), GREY, SCR_W / 2 - 150, BOT_Y + 30);
	lv_obj_set_style_text_letter_space(vd, 2, 0);
	lv_obj_set_style_text_opa(vd, LV_OPA_50, 0);
	/* shutter: white ring, white disc that greys while pressed */
	lv_obj_t *ring = box(root);
	lv_obj_set_size(ring, SHUT_D, SHUT_D);
	lv_obj_set_pos(ring, SCR_W / 2 - SHUT_D / 2, SHUT_CY - SHUT_D / 2);
	lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_border_width(ring, 7, 0);
	lv_obj_set_style_border_color(ring, C(WHITE), 0);
	shutter = box(ring);
	lv_obj_set_size(shutter, SHUT_D - 26, SHUT_D - 26);
	lv_obj_center(shutter);
	lv_obj_set_style_radius(shutter, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(shutter, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(shutter, C(WHITE), 0);
	lv_obj_set_style_bg_color(shutter, C(0xc7c7cc), LV_STATE_PRESSED);
	lv_obj_add_flag(shutter, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(shutter, 20);
	lv_obj_add_event_cb(shutter, shutter_cb, LV_EVENT_CLICKED, NULL);
	/* last photo */
	thumb_box = box(root);
	lv_obj_set_size(thumb_box, SIDE_D, SIDE_D);
	lv_obj_set_pos(thumb_box, 110 - SIDE_D / 2, SHUT_CY - SIDE_D / 2);
	lv_obj_set_style_radius(thumb_box, 14, 0);
	lv_obj_set_style_clip_corner(thumb_box, true, 0);
	lv_obj_set_style_bg_opa(thumb_box, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(thumb_box, C(BLACK), 0);
	lv_obj_set_style_border_width(thumb_box, 2, 0);
	lv_obj_set_style_border_color(thumb_box, C(DARK), 0);
	lv_obj_add_flag(thumb_box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(thumb_box, 16);
	lv_obj_add_event_cb(thumb_box, thumb_cb, LV_EVENT_CLICKED, NULL);
	thumb_img = lv_image_create(thumb_box);
	lv_obj_center(thumb_img);
	lv_obj_add_flag(thumb_img, LV_OBJ_FLAG_HIDDEN);
	/* front / rear */
	flip_btn = ui_icon_button(root, G_FLIP, SIDE_D, DARK, WHITE);
	lv_obj_set_pos(flip_btn, SCR_W - 110 - SIDE_D / 2, SHUT_CY - SIDE_D / 2);
	lv_obj_set_ext_click_area(flip_btn, 16);
	lv_obj_add_event_cb(flip_btn, flip_cb, LV_EVENT_CLICKED, NULL);
}

/* ---------- the timer: run / stop the camera, pick frames up, photos ---------- */

static void tick(lv_timer_t *t)
{
	(void)t;
	if (!root)
		return;
	bool want = wanted();
	cam_run(want);
	/* a finished photo: its thumbnail (cam_dev made it from the frame) */
	char path[256], why[160];
	int res = cam_dev_capture_result(path, sizeof(path), thumb_px, why, sizeof(why));
	if (res > 0) {
		cam_thumb_read(path, thumb_px);	/* its sidecar: graded like the photo (cam_lut.c) */
		thumb_set(thumb_px);
	}
	else if (res < 0)
		ui_alert("Couldn't Save Photo", why, (const char *[]){ "OK" }, 1, NULL, NULL);
	if (viewer_up())
		viewer_tick();
	/* Control Center put the status bar back to the app style: ours is white on black */
	bool cc = shell_cc_open();
	if (cc_was_open && !cc)
		status_set_style(true);
	cc_was_open = cc;

	enum cam_state st = running ? cam_dev_state(why, sizeof(why), NULL) : CAM_OFF;
	if (running) {
		const uint32_t *f = cam_dev_frame();
		if (f)
			vf_show(f);
	}
	if (fs && lv_tick_elaps(fs_at) >= FS_MS)
		fs_refresh();
	if (st == CAM_RUNNING && lv_tick_elaps(awake_at) > 1000) {	/* looking through it isn't idle */
		awake_at = lv_tick_get() | 1;
		power_awake_for("camera", 3000);	/* (a lease: whatever stops this tick, the hold runs out) */
		if (!getenv("ORCHARD_CAMERA_NOBOOST"))
			power_busy();	/* CPU at its top (2 s, renewed): convert + draw at full speed, no DVFS jitter */
	}
	bool err_changed = st == CAM_ERROR && strcmp(why, last_why);
	if (st != shown_state || err_changed || (st == CAM_STARTING && lv_tick_elaps(starting_at) > 600)) {
		if (st == CAM_ERROR) {
			snprintf(last_why, sizeof(last_why), "%s", why);
			vf_detach();
			msg_show(G_CAM_OFF, "Camera Unavailable", why, true);
		} else if (st == CAM_STARTING && lv_tick_elaps(starting_at) > 600 && !showing) {
			msg_show(NULL, "Starting Camera…", NULL, false);	/* (only if it takes a while) */
		} else if (st != CAM_STARTING || showing) {
			msg_show(NULL, NULL, NULL, false);
		}
		if (st != CAM_ERROR)
			last_why[0] = 0;
		shown_state = st;
	}
	/* the shutter takes taps only while the viewfinder runs and no full-resolution still is under way (from the tap
	 * until the viewfinder is back, cam_dev_busy): otherwise greyed and disabled (no pressed look either). The
	 * last-photo thumbnail likewise while busy: the viewer would open on the photo before the one being taken */
	const bool busy = running && cam_dev_busy(), ready = st == CAM_RUNNING && !busy;
	if (ready != shutter_ready) {
		shutter_ready = ready;
		lv_obj_set_style_opa(shutter, ready ? LV_OPA_COVER : LV_OPA_40, 0);
		lv_obj_set_state(shutter, LV_STATE_DISABLED, !ready);
	}
	if (busy != thumb_busy) {
		thumb_busy = busy;
		lv_obj_set_style_opa(thumb_box, busy ? LV_OPA_40 : LV_OPA_COVER, 0);
		lv_obj_set_state(thumb_box, LV_STATE_DISABLED, busy);
	}
	uint32_t period = shell_locked() ? LOCKED_MS : st == CAM_RUNNING ? RUN_MS : IDLE_MS;
	if (period != tm_period)
		lv_timer_set_period(tm, tm_period = period);
}

/* ---------- photo viewer: opens on the newest; as in Photos, swipe right = older, left = newer ---------- */

static lv_obj_t *vw, *vw_img, *vw_count, *vw_date;
static char **vw_list;
static int vw_n, vw_i;
static lv_image_dsc_t *vw_dsc;	/* the picture shown */
/* decoding happens on a thread per request; only the newest request's picture is kept */
static pthread_mutex_t dmu = PTHREAD_MUTEX_INITIALIZER;
static unsigned d_want;
static lv_image_dsc_t *d_res;
static bool d_ready;
static char d_err[96];

typedef struct djob {
	char path[512];
	unsigned gen;
} djob_t;

static void *decode_main(void *arg)
{
	djob_t *j = arg;
	const char *err = "Can't read the photo.";
	lv_image_dsc_t *d = NULL;
	FILE *f = fopen(j->path, "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long len = ftell(f);
		fseek(f, 0, SEEK_SET);
		uint8_t *buf = len > 0 && len < (64 << 20) ? malloc((size_t)len) : NULL;
		if (buf && fread(buf, 1, (size_t)len, f) == (size_t)len)
			d = img_decode(buf, (size_t)len, SCR_W, SCR_H, &err);
		free(buf);
		fclose(f);
	}
	pthread_mutex_lock(&dmu);
	if (j->gen == d_want) {
		if (d_ready && d_res)
			img_free(d_res);
		d_res = d;
		d_ready = true;
		snprintf(d_err, sizeof(d_err), "%s", d ? "" : err ? err : "Can't show the photo.");
	} else if (d) {
		img_free(d);
	}
	pthread_mutex_unlock(&dmu);
	free(j);
	return NULL;
}

static void decode_start(const char *path)
{
	djob_t *j = malloc(sizeof(*j));
	if (!j)
		return;
	snprintf(j->path, sizeof(j->path), "%s", path);
	pthread_mutex_lock(&dmu);
	j->gen = ++d_want;
	pthread_mutex_unlock(&dmu);
	pthread_t t;
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&t, &a, decode_main, j) != 0)
		free(j);
	pthread_attr_destroy(&a);
}

static void decode_cancel(void)
{
	pthread_mutex_lock(&dmu);
	d_want++;
	if (d_ready && d_res)
		img_free(d_res);
	d_res = NULL;
	d_ready = false;
	pthread_mutex_unlock(&dmu);
}

static bool viewer_up(void) { return vw != NULL; }

static void vw_label_set(lv_obj_t *l, const char *s)
{
	if (strcmp(lv_label_get_text(l), s))
		lv_label_set_text(l, s);
}

static void viewer_show(int i)
{
	if (i < 0 || i >= vw_n)
		return;
	vw_i = i;
	char s[64];
	snprintf(s, sizeof(s), "%d of %d", i + 1, vw_n);
	vw_label_set(vw_count, s);
	struct stat st;
	s[0] = 0;
	if (stat(vw_list[i], &st) == 0) {
		struct tm tmv;
		localtime_r(&st.st_mtime, &tmv);
		strftime(s, sizeof(s), prefs_int("clock24", 0) ? "%b %-d, %Y  %H:%M" : "%b %-d, %Y  %-I:%M %p", &tmv);
	}
	vw_label_set(vw_date, s);
	decode_start(vw_list[i]);	/* the old picture stays until the new one is ready */
}

static void viewer_tick(void)
{
	pthread_mutex_lock(&dmu);
	lv_image_dsc_t *d = d_ready ? d_res : NULL;
	bool ready = d_ready;
	d_res = NULL;
	d_ready = false;
	pthread_mutex_unlock(&dmu);
	if (!ready)
		return;
	lv_image_set_src(vw_img, d);	/* (NULL: a photo that can't be decoded shows black) */
	if (vw_dsc)
		img_free(vw_dsc);
	vw_dsc = d;
}

static void viewer_reload(int want)
{
	cam_photos_free(vw_list, vw_n);
	vw_n = cam_photos_list(&vw_list);
	if (!vw_n) {
		viewer_close();
		return;
	}
	viewer_show(want < 0 ? 0 : want >= vw_n ? vw_n - 1 : want);
}

static void delete_cb(int idx, void *ud)
{
	(void)ud;
	if (idx != 1 || !vw || vw_i >= vw_n)
		return;
	if (!cam_photo_delete(vw_list[vw_i]))
		ui_alert("Couldn't Delete Photo", NULL, (const char *[]){ "OK" }, 1, NULL, NULL);
	else
		ui_haptic_success();
	thumb_load_last();
	int was = vw_i;
	viewer_reload(was);
}

static void trash_cb(lv_event_t *e)
{
	(void)e;
	ui_alert("Delete Photo?", "This photo will be deleted from this iPhone.", (const char *[]){ "Cancel", "!Delete" },
		 2, delete_cb, NULL);
}

static void back_cb(lv_event_t *e)
{
	(void)e;
	viewer_close();
}

static void swipe_cb(lv_event_t *e)
{
	(void)e;
	lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
	if (d == LV_DIR_RIGHT && vw_i > 0)
		viewer_show(vw_i - 1);
	else if (d == LV_DIR_LEFT && vw_i + 1 < vw_n)
		viewer_show(vw_i + 1);
}

static lv_obj_t *bar(lv_obj_t *p, int y, int h)
{
	lv_obj_t *b = box(p);
	lv_obj_set_pos(b, 0, y);
	lv_obj_set_size(b, SCR_W, h);
	lv_obj_set_style_bg_opa(b, LV_OPA_70, 0);
	lv_obj_set_style_bg_color(b, C(BLACK), 0);
	return b;
}

static void viewer_open(void)
{
	if (vw)
		return;
	vw_n = cam_photos_list(&vw_list);
	if (!vw_n) {
		thumb_load_last();
		return;
	}
	cam_run(false);	/* the sensor rests while the photos are up */
	vw = box(root);
	lv_obj_set_size(vw, SCR_W, SCR_H);
	lv_obj_set_style_bg_opa(vw, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(vw, C(BLACK), 0);
	lv_obj_add_flag(vw, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(vw, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(vw, swipe_cb, LV_EVENT_GESTURE, NULL);
	vw_img = lv_image_create(vw);
	lv_obj_center(vw_img);
	/* top: back + date + position */
	lv_obj_t *top = bar(vw, 0, STATUS_H + NAV_H);
	lv_obj_t *back = box(top);
	lv_obj_set_size(back, 120, NAV_H);
	lv_obj_set_pos(back, 0, STATUS_H);
	lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
	lv_obj_t *chev = ui_icon(back, G_CHEVRON_L, font(F_MONO, 40), WHITE);
	lv_obj_align(chev, LV_ALIGN_LEFT_MID, 28, 0);
	vw_date = text(top, "", font(F_TEXT_SEMI, 30), WHITE);
	lv_obj_align(vw_date, LV_ALIGN_TOP_MID, 0, STATUS_H + 8);
	vw_count = text(top, "", font(F_TEXT, 24), GREY);
	lv_obj_align(vw_count, LV_ALIGN_TOP_MID, 0, STATUS_H + 48);
	/* bottom: delete */
	lv_obj_t *bot = bar(vw, SCR_H - 120, 120);
	lv_obj_t *trash = ui_icon_button(bot, G_DELETE, 88, BLACK, WHITE);
	lv_obj_align(trash, LV_ALIGN_RIGHT_MID, -24, -BOTTOM_PAD / 2);
	lv_obj_add_event_cb(trash, trash_cb, LV_EVENT_CLICKED, NULL);
	viewer_show(vw_n - 1);
	lv_timer_set_period(tm, tm_period = IDLE_MS);
}

static void viewer_close(void)
{
	if (!vw)
		return;
	decode_cancel();
	lv_obj_delete(vw);
	vw = vw_img = vw_count = vw_date = NULL;
	if (vw_dsc)
		img_free(vw_dsc);
	vw_dsc = NULL;
	cam_photos_free(vw_list, vw_n);
	vw_list = NULL;
	vw_n = vw_i = 0;
}

/* ---------- app ---------- */

/* the camera thread finished a frame (hal_wake): pick it up on this pass (the timer stays as the fallback) */
static void frame_wake(void)
{
	if (tm && running)
		lv_timer_ready(tm);
}

/* lock: the next pass stops the camera and slows down; unlock: it starts again at once */
static void lock_changed(bool locked)
{
	(void)locked;
	if (tm)
		lv_timer_ready(tm);
}

static void cam_open(lv_obj_t *r)
{
	root = r;
	lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(root, C(BLACK), 0);
	cam_photos_dir();	/* (made here, on the UI thread) */
	vf_img = lv_image_create(root);
	lv_obj_set_pos(vf_img, 0, VF_Y);
	lv_obj_set_size(vf_img, VF_W, VF_H);
	lv_obj_add_flag(vf_img, LV_OBJ_FLAG_HIDDEN);
	blink = box(root);
	lv_obj_set_pos(blink, 0, VF_Y);
	lv_obj_set_size(blink, VF_W, VF_H);
	lv_obj_set_style_bg_color(blink, C(BLACK), 0);
	lv_obj_add_flag(blink, LV_OBJ_FLAG_HIDDEN);
	focus_build();
	build_msg();
	cam_filter_set(cam_filter_from_key(prefs_str("camera_filter", NULL)));
	cam_lut_peek(cam_filter_get());	/* loads in the background now, not at the first shutter */
	build_chrome();
	thumb_load_last();
	status_set_style(true);
	cc_was_open = false;
	shown_state = CAM_OFF;
	shutter_ready = true;	/* (as built: the first tick greys it until the viewfinder runs) */
	thumb_busy = false;
	last_why[0] = 0;
	showing = running = false;
	tm = lv_timer_create(tick, tm_period = RUN_MS, NULL);
	hal_wake_set(frame_wake);
	cam_dev_notify(hal_wake);
	lock_listen(lock_changed, true);
	tick(tm);	/* the camera starts with the app (the first frame comes ~ one frame time later) */
}

static void cam_close(void)
{
	viewer_close();
	fs_close();
	cam_run(false);	/* STOP + close on the camera thread: the sensor powers down */
	cam_dev_notify(NULL);
	hal_wake_set(NULL);
	if (tm)
		lv_timer_delete(tm);
	tm = NULL;
	focus_close();
	root = vf_img = blink = msg = msg_title = msg_text = msg_btn = shutter = thumb_box = thumb_img = flip_btn = fl_btn = NULL;
}

const app_t app_camera = { .name = "Camera", .glyph = "\xef\x80\xb0", .icon = IC_DARK, .open = cam_open,
			   .close = cam_close };
