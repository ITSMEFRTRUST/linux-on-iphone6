/* App switcher (Home double-press): cards of recently used apps, newest first, made from the
 * app's last frame (copied off the screen when it was left — the close zoom shrinks the same
 * picture, and reopening shows it while the app is rebuilt); the 60 % card is scaled only when
 * the switcher opens. Tap a card to reopen the app, swipe it up to quit it; tap the background
 * or press Home to leave. */
#include <stdlib.h>
#include <string.h>
#include "orchard.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"

#define CW 450	/* card = 60 % of the screen */
#define CH 800
#define MAXA 8

static struct shot {
	const app_t *app;
	lv_image_dsc_t *full, *img;	/* last frame; the card made from it */
	bool launch, stale;	/* reopening looks like full; img not made from it yet */
} shots[MAXA];
static int nshots;
static lv_obj_t *sw, *row;
static bool card_moved;

static int find(const app_t *a)
{
	for (int i = 0; i < nshots; i++)
		if (shots[i].app == a)
			return i;
	return -1;
}

/* bilinear 750x1334 -> 450x800 in 8-bit fixed point, red+blue and green in one multiply each */
static void scale(const uint32_t *src, uint32_t *out)
{
	static int x0s[CW], wxs[CW];
	for (int x = 0; x < CW; x++) {
		int fx = ((2 * x + 1) * SCR_W * 128) / CW - 128;
		fx = fx < 0 ? 0 : fx;
		x0s[x] = fx >> 8 < SCR_W - 1 ? fx >> 8 : SCR_W - 2;
		wxs[x] = fx >> 8 < SCR_W - 1 ? fx & 255 : 256;
	}
	for (int y = 0; y < CH; y++) {
		int fy = ((2 * y + 1) * SCR_H * 128) / CH - 128;
		fy = fy < 0 ? 0 : fy;
		int y0 = fy >> 8 < SCR_H - 1 ? fy >> 8 : SCR_H - 2, wy = fy >> 8 < SCR_H - 1 ? fy & 255 : 256;
		const uint32_t *r0 = src + y0 * SCR_W, *r1 = r0 + SCR_W;
		for (int x = 0; x < CW; x++) {
			int x0 = x0s[x], wx = wxs[x];
			uint32_t a = r0[x0], b = r0[x0 + 1], c = r1[x0], d = r1[x0 + 1];
			uint32_t rb0 = ((a & 0xff00ff) * (256 - wx) + (b & 0xff00ff) * wx) >> 8 & 0xff00ff;
			uint32_t g0 = ((a & 0xff00) * (256 - wx) + (b & 0xff00) * wx) >> 8 & 0xff00;
			uint32_t rb1 = ((c & 0xff00ff) * (256 - wx) + (d & 0xff00ff) * wx) >> 8 & 0xff00ff;
			uint32_t g1 = ((c & 0xff00) * (256 - wx) + (d & 0xff00) * wx) >> 8 & 0xff00;
			uint32_t rb = (rb0 * (256 - wy) + rb1 * wy) >> 8 & 0xff00ff;
			uint32_t g = (g0 * (256 - wy) + g1 * wy) >> 8 & 0xff00;
			out[y * CW + x] = 0xff000000u | rb | g;
		}
	}
}

static lv_image_dsc_t *image(int w, int h)
{
	lv_image_dsc_t *d = calloc(1, sizeof(*d));
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_XRGB8888;
	d->header.w = w;
	d->header.h = h;
	d->header.stride = w * 4;
	d->data_size = w * h * 4;
	d->data = malloc(d->data_size);
	return d;
}

static void image_free(lv_image_dsc_t *d)
{
	lv_image_cache_drop(d);
	lv_image_header_cache_drop(d);
	free((void *)d->data);
	free(d);
}

/* LVGL caches decoded images by source pointer */
static void image_changed(lv_image_dsc_t *d)
{
	lv_image_cache_drop(d);
	lv_image_header_cache_drop(d);
}

const lv_image_dsc_t *switcher_snapshot(const app_t *a, lv_obj_t *root, bool launch)
{
	int i = find(a);
	struct shot s = { a, NULL, NULL, false, false };
	if (i >= 0) {
		s = shots[i];
		memmove(shots + 1, shots, i * sizeof(shots[0]));
	} else {
		if (nshots == MAXA) {	/* forget the oldest */
			image_free(shots[MAXA - 1].full);
			image_free(shots[MAXA - 1].img);
			nshots--;
		}
		memmove(shots + 1, shots, nshots * sizeof(shots[0]));
		nshots++;
		s.full = image(SCR_W, SCR_H);
		s.img = image(CW, CH);
	}
	/* the screen holds the app's last frame (direct rendering into one full-screen buffer):
	 * copying it is ~1 ms, drawing the app again would be 10-20 */
	lv_draw_buf_t *fb = lv_display_get_buf_active(NULL);
	if (fb && fb->header.w == SCR_W && fb->header.h == SCR_H && lv_color_format_get_size(fb->header.cf) == 4) {
		for (int y = 0; y < SCR_H; y++)
			memcpy((uint8_t *)s.full->data + y * SCR_W * 4, fb->data + y * fb->header.stride, SCR_W * 4);
	} else {
		lv_draw_buf_t *b = lv_snapshot_take(root, LV_COLOR_FORMAT_XRGB8888);
		for (int y = 0; b && y < SCR_H; y++)
			memcpy((uint8_t *)s.full->data + y * SCR_W * 4, b->data + y * b->header.stride, SCR_W * 4);
		if (b)
			lv_draw_buf_destroy(b);
	}
	image_changed(s.full);
	s.launch = launch;
	s.stale = true;
	shots[0] = s;
	return s.full;
}

const lv_image_dsc_t *switcher_picture(const app_t *a, bool any)
{
	int i = find(a);
	return i >= 0 && (any || shots[i].launch) ? shots[i].full : NULL;
}

void switcher_forget(const app_t *a)
{
	int i = find(a);
	if (i < 0)
		return;
	image_free(shots[i].full);
	image_free(shots[i].img);
	memmove(shots + i, shots + i + 1, (nshots - i - 1) * sizeof(shots[0]));
	nshots--;
}

/* ---------- cards ---------- */

static void ty(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }

static void anim_ty(lv_obj_t *o, int from, int to, int ms, lv_anim_completed_cb_t done)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, o);
	lv_anim_set_exec_cb(&a, ty);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_path_cb(&a, motion_path);
	if (done)
		lv_anim_set_completed_cb(&a, done);
	lv_anim_start(&a);
}

static void killed(lv_anim_t *an)
{
	lv_obj_t *card = an->var;
	const app_t *a = lv_obj_get_user_data(card);
	if (a->kill)
		a->kill();
	switcher_forget(a);
	lv_obj_delete(card);
	if (!nshots)
		switcher_show(false);
}

static void card_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *card = lv_event_get_current_target(e);
	lv_indev_t *in = lv_indev_active();
	static int y0;
	lv_point_t p;
	if (!in)
		return;
	lv_indev_get_point(in, &p);
	if (code == LV_EVENT_PRESSED) {
		y0 = p.y;
		card_moved = false;
	} else if (code == LV_EVENT_PRESSING) {
		if (lv_indev_get_scroll_obj(in))
			return;	/* scrolling sideways through the cards */
		int dy = p.y - y0;
		if (dy < -16)
			card_moved = true;
		if (card_moved)
			lv_obj_set_style_translate_y(card, dy < 0 ? dy : dy / 4, 0);
	} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		int t = lv_obj_get_style_translate_y(card, 0);
		if (!card_moved)
			return;
		lv_point_t v;
		lv_indev_get_vect(in, &v);
		if (t < -220 || v.y < -20) {
			lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
			anim_ty(card, t, -SCR_H, motion_fling_ms(SCR_H + t, -v.y * 60, MOTION_PAGE), killed);
		} else {
			anim_ty(card, t, 0, MOTION_SMALL, NULL);
		}
	} else if (code == LV_EVENT_CLICKED && !card_moved) {
		const app_t *a = lv_obj_get_user_data(card);
		lv_obj_t *img = lv_obj_get_child(card, 1);
		shell_open_app(a, img);	/* grows out of the card */
		switcher_show(false);
	}
}

static void bg_clicked(lv_event_t *e)
{
	lv_obj_t *t = lv_event_get_target(e);
	if (t == sw || t == row)
		switcher_show(false);
}

void switcher_show(bool on)
{
	if (!on) {
		if (sw) {
			lv_obj_delete(sw);
			sw = row = NULL;
			status_refresh();
		}
		return;
	}
	if (sw)
		return;
	sw = box(shell_layer(LY_SWITCHER));
	lv_obj_set_size(sw, SCR_W, SCR_H);
	lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(sw, bg_clicked, LV_EVENT_CLICKED, NULL);
	ui_material(sw);
	lv_obj_set_style_clip_corner(sw, false, 0);
	if (!nshots) {
		lv_obj_t *l = text(sw, "No Recent Apps", font(F_TEXT_SEMI, 36), pal.label2);
		lv_obj_center(l);
		return;
	}
	row = box(sw);
	lv_obj_set_size(row, SCR_W, SCR_H);
	lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(row, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_scroll_snap_x(row, LV_SCROLL_SNAP_CENTER);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_hor(row, (SCR_W - CW) / 2, 0);
	lv_obj_set_style_pad_column(row, 36, 0);
	lv_obj_add_event_cb(row, bg_clicked, LV_EVENT_CLICKED, NULL);
	for (int i = 0; i < nshots; i++) {
		if (shots[i].stale) {
			scale((const uint32_t *)shots[i].full->data, (uint32_t *)shots[i].img->data);
			image_changed(shots[i].img);
			shots[i].stale = false;
		}
		lv_obj_t *card = box(row);
		lv_obj_set_size(card, CW, CH + 84);
		lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_user_data(card, (void *)shots[i].app);
		lv_obj_add_event_cb(card, card_event, LV_EVENT_ALL, NULL);
		lv_obj_t *hdr = box(card);
		lv_obj_set_size(hdr, CW, 64);
		lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(hdr, 16, 0);
		lv_obj_remove_flag(hdr, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_t *ic = app_icon(hdr, shots[i].app, 56);
		lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
		text(hdr, shots[i].app->name, font(F_TEXT_SEMI, 30), th.dark ? 0xffffff : pal.label);
		lv_obj_t *img = lv_image_create(card);
		lv_image_set_src(img, shots[i].img);
		lv_obj_set_pos(img, 0, 84);
		lv_obj_set_style_radius(img, 40, 0);	/* lv_image clips its corners to the radius */
		lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
	}
}

bool switcher_visible(void) { return sw != NULL; }

lv_obj_t *switcher_card(const app_t *a)
{
	for (uint32_t i = 0; row && i < lv_obj_get_child_count(row); i++) {
		lv_obj_t *card = lv_obj_get_child(row, i);
		if (lv_obj_get_user_data(card) == a) {
			lv_obj_update_layout(card);
			return lv_obj_get_child(card, 1);
		}
	}
	return NULL;
}
