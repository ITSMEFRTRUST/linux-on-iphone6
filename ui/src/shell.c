/* Shell core: layers, status bar, app launch/close, hardware keys (Home = home, double-press =
 * app switcher, hold = Search; there is no swipe-up home gesture), screen on/off + lock +
 * auto-lock (power.c decides when, and puts the UI to sleep while the screen is off), notification banners,
 * volume HUD. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "orchard.h"
#include "src/core/lv_obj_event_private.h"
#include "src/misc/lv_area_private.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"

#define ICON 120

static lv_obj_t *layers[LY_COUNT];
static lv_obj_t *status, *st_time, *st_left, *st_power, *st_pct, *st_wifi, *batt, *batt_fill, *batt_nub;
static lv_obj_t *app_root, *blackout;
static const app_t *cur;
static bool screen_off, on_wallpaper = true;
static int saved_backlight;
static uint32_t last_home;
static lv_timer_t *home_hold;
static bool home_held;

/* ---------- layers (children of lv_layer_top, in z order) ---------- */

lv_obj_t *shell_layer(int ly) { return layers[ly]; }
lv_obj_t *shell_kbd_layer(void) { return layers[LY_KBD]; }
lv_obj_t *shell_overlay(void) { return layers[LY_OVERLAY]; }

static void make_layers(void)
{
	for (int i = 0; i < LY_COUNT; i++) {
		layers[i] = box(lv_layer_top());
		lv_obj_set_size(layers[i], SCR_W, SCR_H);
		lv_obj_remove_flag(layers[i], LV_OBJ_FLAG_CLICKABLE);
	}
}

/* ---------- status bar ---------- */

/* yellow in Low Power Mode, green while charging, red at 20 % and below (like iOS), else the bar's text colour */
static uint32_t batt_color(uint32_t c)
{
	int pct = sys_battery_pct();
	return power_lpm() ? pal.yellow : sys_charger_online() ? pal.green : pct >= 0 && pct <= 20 ? pal.red : c;
}

void status_set_style(bool wall)
{
	on_wallpaper = wall;
	uint32_t c = wall ? 0xffffff : pal.label;
	lv_obj_set_style_text_color(st_time, C(c), 0);
	lv_obj_set_style_text_color(st_left, C(c), 0);
	lv_obj_set_style_text_color(st_power, C(c), 0);
	lv_obj_set_style_text_color(st_pct, C(c), 0);
	lv_obj_set_style_text_color(st_wifi, C(c), 0);
	lv_obj_invalidate(st_wifi);
	lv_obj_set_style_border_color(batt, C(c), 0);
	lv_obj_set_style_bg_color(batt_fill, C(batt_color(c)), 0);
	lv_obj_set_style_bg_color(batt_nub, C(c), 0);
}

static void status_tick(void);

void status_refresh(void)
{
	if (!status)
		return;
	status_set_style(shell_cc_open() ? th.dark : lock_visible() || !cur);
	status_tick();
}

static void status_wifi_changed(void) { status_tick(); }	/* the bars come and go at once */

static void make_status_bar(void)
{
	status = box(layers[LY_STATUS]);
	lv_obj_set_size(status, SCR_W, STATUS_H);
	st_left = text(status, "Linux", font(F_TEXT_SEMI, 26), 0xffffff);
	lv_obj_align(st_left, LV_ALIGN_LEFT_MID, 18, 0);
	st_time = text(status, "", font(F_TEXT_SEMI, 26), 0xffffff);
	lv_obj_align(st_time, LV_ALIGN_CENTER, 0, 0);
	/* battery glyph, filled to the gauge's percentage (full while it is unknown) */
	batt = box(status);
	lv_obj_set_size(batt, 50, 24);
	lv_obj_align(batt, LV_ALIGN_RIGHT_MID, -24, 0);
	lv_obj_set_style_radius(batt, 7, 0);
	lv_obj_set_style_border_width(batt, 2, 0);
	lv_obj_set_style_border_opa(batt, LV_OPA_50, 0);
	lv_obj_set_style_pad_all(batt, 3, 0);
	batt_fill = box(batt);
	lv_obj_set_size(batt_fill, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_radius(batt_fill, 4, 0);
	lv_obj_set_style_bg_opa(batt_fill, LV_OPA_COVER, 0);
	batt_nub = box(status);
	lv_obj_set_size(batt_nub, 4, 9);
	lv_obj_set_style_radius(batt_nub, 2, 0);
	lv_obj_set_style_bg_opa(batt_nub, LV_OPA_50, 0);
	lv_obj_align_to(batt_nub, batt, LV_ALIGN_OUT_RIGHT_MID, 1, 0);
	st_power = text(status, "", font(F_TEXT_SEMI, 26), 0xffffff);
	st_pct = text(status, "", font(F_TEXT_SEMI, 26), 0xffffff);
	st_wifi = wifi_icon(status, 32);	/* Wi-Fi bars left of the battery while connected, like iOS */
	lv_obj_add_flag(st_wifi, LV_OBJ_FLAG_HIDDEN);
	wifi_listen(status_wifi_changed, true);
	status_set_style(true);
}

/* the status bar ticks every second: a label set to the text it has, a flag set again or a colour set again
 * still redraws it (and what is under it), so only changes go through */
static void set_text(lv_obj_t *l, const char *s)
{
	if (strcmp(lv_label_get_text(l), s))
		lv_label_set_text(l, s);
}

static void set_hidden(lv_obj_t *o, bool hide)
{
	if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hide)
		return;
	if (hide)
		lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static void set_bg(lv_obj_t *o, uint32_t c)
{
	if (!lv_color_eq(lv_obj_get_style_bg_color(o, 0), C(c)))
		lv_obj_set_style_bg_color(o, C(c), 0);
}

static void status_tick(void)
{
	char buf[32];
	sys_clock_format(buf, sizeof(buf), !prefs_int("clock24", 0));
	/* the lock screen shows a big clock, so the bar drops its own (like iOS) */
	set_text(st_time, lock_visible() ? "" : buf);
	const char *bt = bt_audio_name() ? "  " G_HEADPHONES : bt_connected() ? "  " G_BLUETOOTH : "";
	snprintf(buf, sizeof(buf), "Linux%s%s%s", sys_usbnet_up() ? "  " G_USB : "", bt, prefs_int("dnd", 0) ? "  " G_MOON : "");
	set_text(st_left, buf);
	bool chg = sys_charger_online();
	int pct = sys_battery_pct();
	set_text(st_power, chg ? G_BOLT : "");
	/* (aligning again restyles = redraws, even to the same place: only when the row changed) */
	static char laid[24];
	char lay[24];
	int bars = wifi_bars();
	bool fan = bars > 0 || wifi_reconnecting();	/* back from Wi-Fi Sleep, not connected yet: the fan dimmed */
	snprintf(lay, sizeof(lay), "%d%d%d%s", chg, fan, on_wallpaper, lv_label_get_text(st_pct));
	bool relayout = strcmp(lay, laid);
	if (relayout)
		lv_obj_align_to(st_power, batt, LV_ALIGN_OUT_LEFT_MID, -10, 0);
	if (pct >= 0)
		snprintf(buf, sizeof(buf), "%d%%", pct);
	else
		buf[0] = 0;
	set_text(st_pct, buf);
	snprintf(lay, sizeof(lay), "%d%d%d%s", chg, fan, on_wallpaper, buf);
	relayout |= strcmp(lay, laid) != 0;
	snprintf(laid, sizeof(laid), "%s", lay);
	if (relayout)
		lv_obj_align_to(st_pct, chg ? st_power : batt, LV_ALIGN_OUT_LEFT_MID, chg ? -6 : -10, 0);
	/* 40 px inside the outline (50 - 2 x 2 border - 2 x 3 padding); a sliver stays at 0 % */
	int32_t w = pct < 0 ? LV_PCT(100) : LV_MAX(4, 40 * pct / 100);
	if (lv_obj_get_style_width(batt_fill, 0) != w)
		lv_obj_set_width(batt_fill, w);
	if (fan) {
		wifi_icon_set(st_wifi, bars);
		if (relayout)
			lv_obj_align_to(st_wifi, pct >= 0 ? st_pct : chg ? st_power : batt, LV_ALIGN_OUT_LEFT_MID, -12, 0);
		set_hidden(st_wifi, false);
	} else {
		set_hidden(st_wifi, true);
	}
	set_bg(batt_fill, batt_color(on_wallpaper ? 0xffffff : pal.label));
}

/* ---------- app launch / close: the app's picture zooms out of its icon and back into it ----------
 * The picture is the app's last frame (copied off the screen when it was left, like iOS), or on
 * its first launch the app built off screen and drawn once. Each frame it is scaled into a
 * card-sized scratch image (nearest pixel, a plain copy loop — LVGL's own transform costs ~8x
 * more), which one full-screen object that never moves draws 1:1. Only the new card and the
 * strip it left are invalidated, in pieces, and the card's middle reports itself opaque, so
 * LVGL draws just the picture there instead of the home screen under it too. The live app is
 * built (if it wasn't) when the picture fills the screen, so it takes over without a change. */

static lv_obj_t *zoom;
static const lv_image_dsc_t *pic;	/* the picture; NULL = plain card */
static lv_image_dsc_t pic_first, frame;	/* first launch: the app drawn off screen; this frame's card */
static lv_draw_buf_t *first_frame;
static const app_t *zoom_app;
static lv_area_t from_area, to_area, card;	/* icon, full screen (or where a zoom was caught), card now */
static int32_t zoom_t;	/* 0 = icon … 1024 = full screen */
static int zoom_dir, from_r, to_r, card_r;	/* dir: +1 opening, -1 closing, 0 idle */
static bool built, zoom_in_switcher;	/* cur's UI exists; shrinking into a switcher card */
static lv_obj_t *zoom_target;	/* that card's picture, hidden until the zoom lands on it */

static void inv(lv_area_t a)
{
	if (a.x1 <= a.x2 && a.y1 <= a.y2)
		lv_obj_invalidate_area(lv_screen_active(), &a);
}

static lv_area_t card_mid(void)
{
	lv_area_t m = card;
	m.y1 += card_r;
	m.y2 -= card_r;
	return m;
}

/* the picture scaled to the card's width and centred on it (an icon-shaped card crops the middle) */
static void zoom_scale(void)
{
	int w = lv_area_get_width(&card), h = lv_area_get_height(&card);
	if (!pic || w <= 0 || h <= 0 || w > SCR_W || h > SCR_H || (w == SCR_W && h == SCR_H))
		return;	/* (full screen: the picture is drawn itself) */
	static int xs[SCR_W];
	int step = SCR_W * 65536 / w, top = LV_MAX((SCR_H * w / SCR_W - h) / 2, 0), stride = pic->header.stride / 4;
	for (int x = 0; x < w; x++)
		xs[x] = LV_MIN((x * step + step / 2) >> 16, SCR_W - 1);
	uint32_t *out = (uint32_t *)frame.data;
	for (int y = 0; y < h; y++, out += w) {
		const uint32_t *row = (const uint32_t *)pic->data + LV_MIN(((y + top) * step + step / 2) >> 16, SCR_H - 1) * stride;
		for (int x = 0; x < w; x++)
			out[x] = row[xs[x]];
	}
	frame.header.w = w;
	frame.header.h = h;
	frame.header.stride = w * 4;
	frame.data_size = w * h * 4;
	lv_image_cache_drop(&frame);	/* LVGL caches images by source pointer */
	lv_image_header_cache_drop(&frame);
}

static void zoom_set(void *v, int32_t t)
{
	(void)v;
	lv_area_t old = card;
	zoom_t = t;
	card.x1 = from_area.x1 + (to_area.x1 - from_area.x1) * t / 1024;
	card.y1 = from_area.y1 + (to_area.y1 - from_area.y1) * t / 1024;
	card.x2 = from_area.x2 + (to_area.x2 - from_area.x2) * t / 1024;
	card.y2 = from_area.y2 + (to_area.y2 - from_area.y2) * t / 1024;
	card_r = from_r + (to_r - from_r) * t / 1024;
	zoom_scale();
	/* redraw old ∪ new card, cut so that the opaque middle is an area of its own */
	lv_area_t u = card, m = card_mid();
	if (old.x1 <= old.x2) {
		u.x1 = LV_MIN(u.x1, old.x1);
		u.y1 = LV_MIN(u.y1, old.y1);
		u.x2 = LV_MAX(u.x2, old.x2);
		u.y2 = LV_MAX(u.y2, old.y2);
	}
	inv((lv_area_t){ u.x1, u.y1, u.x2, m.y1 - 1 });
	inv((lv_area_t){ u.x1, m.y2 + 1, u.x2, u.y2 });
	inv((lv_area_t){ u.x1, m.y1, m.x1 - 1, m.y2 });
	inv((lv_area_t){ m.x2 + 1, m.y1, u.x2, m.y2 });
	inv(m);
	/* status bar: wallpaper style while the home screen shows around the card */
	if (on_wallpaper != (t < 700) && !zoom_in_switcher)
		status_set_style(t < 700);
}

static void zoom_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_area_t m = card_mid();
	if (code == LV_EVENT_COVER_CHECK) {
		lv_cover_check_info_t *info = lv_event_get_param(e);
		info->res = lv_area_is_in(info->area, &m, 0) ? LV_COVER_RES_COVER : LV_COVER_RES_NOT_COVER;
		return;
	}
	if (code != LV_EVENT_DRAW_MAIN)
		return;
	lv_layer_t *layer = lv_event_get_layer(e);
	if (!pic) {
		lv_draw_rect_dsc_t r;
		lv_draw_rect_dsc_init(&r);
		r.bg_color = C(pal.bg);
		r.radius = card_r;
		lv_draw_rect(layer, &r, &card);
		return;
	}
	lv_draw_image_dsc_t d;
	lv_draw_image_dsc_init(&d);
	d.src = lv_area_get_width(&card) == SCR_W && lv_area_get_height(&card) == SCR_H ? (const void *)pic : &frame;
	d.image_area = card;
	d.clip_radius = card_r;
	lv_draw_image(layer, &d, &card);
}

/* shown / hidden without invalidating the whole screen: zoom_set invalidates what changes */
static void zoom_show(bool on)
{
	lv_display_enable_invalidation(NULL, false);
	if (on) {
		lv_obj_remove_flag(zoom, LV_OBJ_FLAG_HIDDEN);
		lv_obj_move_foreground(zoom);
	} else {
		lv_obj_add_flag(zoom, LV_OBJ_FLAG_HIDDEN);
		if (zoom_in_switcher)	/* back under the apps, aimed at the full screen again */
			lv_obj_set_parent(zoom, lv_screen_active());
		zoom_in_switcher = false;
		to_area = (lv_area_t){ 0, 0, SCR_W - 1, SCR_H - 1 };
		to_r = 0;
	}
	lv_display_enable_invalidation(NULL, true);
	if (!on && zoom_target) {	/* the switcher card it landed on */
		lv_obj_remove_flag(zoom_target, LV_OBJ_FLAG_HIDDEN);
		zoom_target = NULL;
	}
	if (!on)
		inv(card);	/* the home screen under the last card */
}

static void origin(lv_obj_t *from)
{
	if (from) {
		lv_obj_get_coords(from, &from_area);
		from_r = lv_obj_get_style_radius(from, 0);
	} else {	/* zoom out of the middle of the screen */
		from_area = (lv_area_t){ SCR_W / 2 - ICON / 2, SCR_H / 2 - ICON / 2, SCR_W / 2 + ICON / 2 - 1,
					 SCR_H / 2 + ICON / 2 - 1 };
		from_r = ICON * 225 / 1000;
	}
}

static void zoom_landed(lv_anim_t *a);

static void zoom_go(int dir, int ms)
{
	if (!zoom_dir) {
		zoom_show(true);
		card = (lv_area_t){ 0, 0, -1, -1 };
		zoom_set(NULL, zoom_t);
	}
	zoom_dir = dir;
	lv_anim_delete(&zoom_t, zoom_set);
	motion_anim(&zoom_t, zoom_set, zoom_t, dir > 0 ? 1024 : 0, ms, zoom_landed);
}

/* the app takes over from its picture: built now if it wasn't (the screen still shows the full
 * picture meanwhile), then drawn once for real */
static void opened(void)
{
	lv_obj_remove_flag(app_root, LV_OBJ_FLAG_HIDDEN);
	if (!built)
		cur->open(app_root);
	built = true;
	status_set_style(false);
}

static void zoom_landed(lv_anim_t *a)
{
	(void)a;
	int dir = zoom_dir;
	zoom_dir = 0;
	zoom_show(false);
	if (dir > 0)
		opened();
	else
		zoom_app = NULL;
}

/* the zoom in flight lands at once (new input never waits for one) */
static void zoom_finish(void)
{
	if (!zoom_dir)
		return;
	lv_anim_delete(&zoom_t, zoom_set);
	zoom_set(NULL, zoom_dir > 0 ? 1024 : 0);
	zoom_landed(NULL);
}

/* the live app sits hidden under the picture until the zoom lands */
static void root_new(const app_t *a)
{
	cur = a;
	built = false;
	lv_display_enable_invalidation(NULL, false);
	app_root = box(lv_screen_active());
	lv_obj_set_size(app_root, SCR_W, SCR_H);
	lv_obj_set_style_bg_opa(app_root, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(app_root, C(pal.bg), 0);
	lv_obj_add_flag(app_root, LV_OBJ_FLAG_HIDDEN);
	lv_display_enable_invalidation(NULL, true);
}

void shell_open_app(const app_t *a, lv_obj_t *from)
{
	if (zoom_in_switcher)
		zoom_finish();	/* a card tapped while the app still shrinks into it */
	if (zoom_dir < 0 && a == zoom_app && !cur) {
		/* tapped again while it was still shrinking: grow it back from there */
		root_new(a);
		zoom_go(1, MOTION_ZOOM * (1024 - zoom_t) / 1024 + 60);
		return;
	}
	if (zoom_dir < 0)
		zoom_finish();	/* another app while the last one shrinks: it lands at once */
	if (cur)
		return;
	zoom_app = a;
	origin(from);
	root_new(a);
	pic = switcher_picture(a, switcher_visible());	/* from a switcher card: what the card shows */
	if (!pic) {
		/* no picture that matches a fresh start: build the app now and draw it once */
		a->open(app_root);
		built = true;
		lv_obj_update_layout(app_root);
		if (!first_frame)
			first_frame = lv_draw_buf_create(SCR_W, SCR_H, LV_COLOR_FORMAT_XRGB8888, 0);
		if (first_frame && lv_snapshot_take_to_draw_buf(app_root, LV_COLOR_FORMAT_XRGB8888, first_frame) == LV_RESULT_OK) {
			pic_first.header = first_frame->header;
			pic_first.data = first_frame->data;
			pic_first.data_size = first_frame->data_size;
			lv_image_cache_drop(&pic_first);
			lv_image_header_cache_drop(&pic_first);
			pic = &pic_first;
		}
	}
	zoom_t = 0;
	zoom_go(1, MOTION_ZOOM);
}

/* the app's UI goes away (its picture stays with the switcher) */
static void app_teardown(void)
{
	if (built) {
		kbd_hide();
		nav_reset();
		cur->close();
	}
	lv_obj_delete(app_root);
	app_root = NULL;
	cur = NULL;
	built = false;
}

/* reopening will look like this: root page at the top, no keyboard */
static bool launchable(void) { return !kbd_visible() && nav_pristine(); }

void shell_go_home(void)
{
	if (zoom_dir > 0) {	/* Home while it opens: shrink back from where it got to */
		app_teardown();
		zoom_go(-1, MOTION_ZOOM * zoom_t / 1024 + 60);
		return;
	}
	if (!cur || zoom_dir)
		return;
	/* the picture is copied straight off the screen: the shrink starts on this very frame */
	pic = switcher_snapshot(cur, app_root, launchable());
	zoom_app = cur;
	origin(home_icon_of(cur));
	app_teardown();
	zoom_t = 1024;
	zoom_go(-1, MOTION_ZOOM);
}

/* close without animation (switching straight to another app, the switcher, Search) */
static void close_now(void)
{
	bool shown = zoom_dir == 0;	/* still opening: it was never on screen, keep its old picture */
	if (zoom_dir) {
		lv_anim_delete(&zoom_t, zoom_set);
		zoom_dir = 0;
		zoom_show(false);
		zoom_app = NULL;
	}
	if (!cur)
		return;
	if (shown)
		switcher_snapshot(cur, app_root, launchable());
	app_teardown();
	status_refresh();
}

void shell_launch(const app_t *a)
{
	if (switcher_visible())
		switcher_show(false);
	if (shell_cc_open())
		shell_cc_toggle();
	if (lock_visible())
		lock_show(false);
	if (cur == a)
		return;
	close_now();
	shell_open_app(a, home_icon_of(a));
}

bool shell_app_open(void) { return cur != NULL; }
const app_t *shell_current_app(void) { return cur; }

/* ---------- screen, lock, keys ---------- */

static void banner_hide(void);

static void tick(lv_timer_t *t);

static void screen_set(bool off)
{
	if (off == screen_off)
		return;
	if (off) {
		saved_backlight = power_backlight();	/* (the undimmed level when auto-lock dimmed it first) */
		sys_backlight_set(0);
		lv_obj_remove_flag(blackout, LV_OBJ_FLAG_HIDDEN);
		if (shell_cc_open())
			shell_cc_toggle();
		/* the keyboard stays: under the lock screen (lock_show → kbd_cover), unlocking goes back to typing — the
		 * Terminal only shows it when it opens */
		ui_menu_close();	/* menus sit above the lock layer */
		ui_swipe_close();
		banner_hide();	/* it is on the lock screen's list now */
		lock_show(true);
		power_screen(true);	/* the UI goes to sleep once the black frame is up */
	} else {
		power_screen(false);	/* (+ the CPU: power.c) */
		power_backlight_on(saved_backlight > 0 ? saved_backlight : sys_backlight_max() / 2);
		lv_obj_add_flag(blackout, LV_OBJ_FLAG_HIDDEN);
		lv_display_trigger_activity(NULL);
	}
	screen_off = off;
	if (!off)
		tick(NULL);
}

void shell_screen_toggle(void) { screen_set(!screen_off); }
bool shell_screen_off(void) { return screen_off; }
bool shell_locked(void) { return lock_visible(); }

void shell_unlock(void) { lock_dismiss(); }

void shell_power_key(void) { screen_set(!screen_off); }

/* Home held: Search from anywhere (the slot Siri has on iOS) */
static void home_long(lv_timer_t *t)
{
	(void)t;
	home_hold = NULL;
	if (screen_off || lock_visible())
		return;	/* the release does the normal press */
	home_held = true;
	if (switcher_visible())
		switcher_show(false);
	if (shell_cc_open())
		shell_cc_toggle();
	close_now();
	status_refresh();
	home_reset();
	search_open();
	ui_haptic_tap();
}

void shell_home_key(bool pressed)
{
	if (pressed) {
		home_held = false;
		if (home_hold)
			lv_timer_delete(home_hold);
		home_hold = lv_timer_create(home_long, 500, NULL);
		lv_timer_set_repeat_count(home_hold, 1);
		return;
	}
	if (home_hold) {
		lv_timer_delete(home_hold);
		home_hold = NULL;
	}
	if (home_held)
		return;
	uint32_t now = lv_tick_get();
	bool twice = last_home && now - last_home < 400;
	last_home = now;
	if (screen_off) {
		screen_set(false);
		return;
	}
	if (lock_visible()) {
		shell_unlock();
		return;
	}
	if (twice) {
		shell_switcher_toggle();
		return;
	}
	if (switcher_visible()) {
		switcher_show(false);
		return;
	}
	if (shell_cc_open()) {
		shell_cc_toggle();
		return;
	}
	if (cur)
		shell_go_home();
	else
		home_reset();
}

/* the switcher's card went away (switcher closed) while the app was shrinking into it: land
 * right after (not inside the switcher's deletion) */
static void land_async(void *p)
{
	(void)p;
	if (zoom_in_switcher)
		zoom_finish();
}

static void card_gone(lv_event_t *e)
{
	(void)e;
	zoom_target = NULL;
	lv_async_call(land_async, NULL);
}

/* switcher from an app (or a close zoom still running): the app shrinks into its card */
static void into_switcher(void)
{
	const app_t *a = cur ? cur : zoom_app;
	lv_area_t at = { 0, 0, SCR_W - 1, SCR_H - 1 };
	int at_r = 0;
	if (zoom_dir > 0 || (!cur && zoom_dir == 0)) {
		close_now();	/* still opening (never on screen) or nothing open */
		switcher_show(true);
		return;
	}
	if (zoom_dir < 0) {	/* caught mid-way home: carry on from there */
		lv_anim_delete(&zoom_t, zoom_set);
		at = card;
		at_r = card_r;
	} else {
		pic = switcher_snapshot(cur, app_root, launchable());
		app_teardown();
		status_refresh();
	}
	switcher_show(true);
	lv_obj_t *img = pic ? switcher_card(a) : NULL;
	if (!img) {
		zoom_dir = 0;
		zoom_show(false);
		return;
	}
	lv_obj_get_coords(img, &from_area);
	from_r = lv_obj_get_style_radius(img, 0);
	to_area = at;
	to_r = at_r;
	lv_obj_add_event_cb(img, card_gone, LV_EVENT_DELETE, NULL);
	lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
	zoom_target = img;
	/* drawn above the switcher until it lands on the card */
	lv_display_enable_invalidation(NULL, false);
	lv_obj_set_parent(zoom, shell_layer(LY_SWITCHER));
	lv_display_enable_invalidation(NULL, true);
	zoom_in_switcher = true;
	zoom_app = a;
	zoom_t = 1024;
	zoom_dir = 0;
	zoom_go(-1, MOTION_ZOOM);
}

void shell_switcher_toggle(void)
{
	if (shell_cc_open())
		shell_cc_toggle();
	if (!switcher_visible()) {
		into_switcher();
		return;
	}
	switcher_show(!switcher_visible());
}

bool shell_switcher_open(void) { return switcher_visible(); }

/* ---------- notifications + banners ---------- */

#define MAX_NOTIF 20
static struct notif notifs[MAX_NOTIF];
static int nnotif;
static lv_obj_t *banner;
static lv_timer_t *banner_timer;
static const app_t *banner_app;

int shell_notifications(const struct notif **list)
{
	*list = notifs;
	return nnotif;
}

void shell_clear_notifications(void)
{
	nnotif = 0;
	lock_notifications_changed();
}

/* banners are slides: moving shifts their pixels, only what they uncover is drawn */
static void set_y(void *o, int32_t v) { slide_move(o, lv_obj_get_x(o), v); }

static void banner_landed(lv_anim_t *a) { slide_track(a->var, false); }

static void banner_gone(lv_anim_t *a)
{
	lv_obj_delete(a->var);
	if (a->var == banner)
		banner = NULL;
}

static void banner_hide(void)
{
	if (!banner)
		return;
	if (banner_timer) {
		lv_timer_delete(banner_timer);
		banner_timer = NULL;
	}
	slide_track(banner, true);
	motion_anim(banner, set_y, lv_obj_get_y(banner), -260, MOTION_SHEET, banner_gone);
	lv_obj_remove_flag(banner, LV_OBJ_FLAG_CLICKABLE);
	banner = NULL;
}

static void banner_timeout(lv_timer_t *t)
{
	(void)t;
	banner_timer = NULL;
	banner_hide();
}

static void banner_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_CLICKED) {
		const app_t *a = banner_app;
		banner_hide();
		if (a && !screen_off)
			shell_launch(a);
	} else if (code == LV_EVENT_GESTURE && lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
		banner_hide();
	}
}

static void banner_pop(const app_t *from, const char *title, const char *body);

void shell_banner(const app_t *from, const char *title, const char *body)
{
	memmove(notifs + 1, notifs, sizeof(notifs[0]) * (MAX_NOTIF - 1));
	notifs[0].app = from;
	snprintf(notifs[0].title, sizeof(notifs[0].title), "%s", title);
	snprintf(notifs[0].body, sizeof(notifs[0].body), "%s", body ? body : "");
	notifs[0].when = time(NULL);
	if (nnotif < MAX_NOTIF)
		nnotif++;
	lock_notifications_changed();
	if (lock_visible() || screen_off || prefs_int("dnd", 0))
		return;	/* shows on the lock screen instead */
	banner_pop(from, title, body);
}

static void banner_pop(const app_t *from, const char *title, const char *body)
{
	if (banner)
		banner_hide();
	banner_app = from;
	banner = box(layers[LY_BANNER]);
	lv_obj_set_size(banner, SCR_W - 32, LV_SIZE_CONTENT);
	lv_obj_set_x(banner, 16);
	lv_obj_set_style_radius(banner, 44, 0);
	lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(banner, C(th.dark ? mix(pal.cell, th.fg, 8) : 0xffffff), 0);
	lv_obj_set_style_shadow_width(banner, 40, 0);
	lv_obj_set_style_shadow_opa(banner, LV_OPA_50, 0);
	lv_obj_set_style_shadow_color(banner, C(0x000000), 0);
	lv_obj_set_style_pad_all(banner, 24, 0);
	lv_obj_set_style_pad_column(banner, 22, 0);
	lv_obj_set_flex_flow(banner, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(banner, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(banner, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* swipe up reaches the banner itself */
	lv_obj_add_event_cb(banner, banner_event, LV_EVENT_ALL, NULL);
	if (from)
		app_icon(banner, from, 76);
	lv_obj_t *col = box(banner);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	lv_obj_t *t = text(col, title, font(F_TEXT_SEMI, 30), pal.label);
	lv_obj_set_width(t, LV_PCT(100));
	lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
	if (body && *body) {
		lv_obj_t *b = text(col, body, font(F_TEXT, 28), pal.label);
		lv_obj_set_width(b, LV_PCT(100));
		lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
	}
	lv_obj_t *now = text(banner, "now", font(F_TEXT, 24), pal.label2);
	lv_obj_add_flag(now, LV_OBJ_FLAG_FLOATING);
	lv_obj_align(now, LV_ALIGN_TOP_RIGHT, 0, -4);

	lv_obj_set_y(banner, -260);
	lv_obj_update_layout(banner);
	slide_track(banner, true);
	motion_anim(banner, set_y, -260, 20, MOTION_SHEET + 60, banner_landed);
	banner_timer = lv_timer_create(banner_timeout, 4500, NULL);
	lv_timer_set_repeat_count(banner_timer, 1);
	ui_haptic_tap();
}

/* alarms: long buzz pattern, wake the screen */
static int buzz_left;
static void buzz_cb(lv_timer_t *t)
{
	hal_haptic(400);
	if (--buzz_left <= 0)
		lv_timer_delete(t);
}

void shell_alarm(const app_t *from, const char *title, const char *body)
{
	if (screen_off)
		screen_set(false);
	shell_banner(from, title, body);
	if (!lock_visible() && prefs_int("dnd", 0))
		banner_pop(from, title, body);	/* alarms break through Do Not Disturb */
	buzz_left = 4;
	buzz_cb(lv_timer_create(buzz_cb, 900, NULL));
}

/* ---------- volume HUD (left edge, next to the iPhone 6 volume buttons) ---------- */

static lv_obj_t *vol_hud, *vol_fill;
static lv_timer_t *vol_timer;

static void vol_hide(lv_timer_t *t)
{
	(void)t;
	vol_timer = NULL;
	lv_obj_add_flag(vol_hud, LV_OBJ_FLAG_HIDDEN);
}

void shell_volume_step(int dir)
{
	int v = sys_volume_get() + dir * 7;
	v = v < 0 ? 0 : v > 100 ? 100 : v;
	sys_volume_set(v);
	if (!vol_hud) {
		vol_hud = box(layers[LY_BANNER]);
		lv_obj_set_size(vol_hud, 30, 300);
		lv_obj_set_pos(vol_hud, 14, 300);
		lv_obj_set_style_radius(vol_hud, 15, 0);
		lv_obj_set_style_bg_opa(vol_hud, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(vol_hud, C(mix(pal.cell, th.fg, 10)), 0);
		lv_obj_set_style_clip_corner(vol_hud, true, 0);
		lv_obj_set_style_shadow_width(vol_hud, 20, 0);
		lv_obj_set_style_shadow_opa(vol_hud, LV_OPA_40, 0);
		vol_fill = box(vol_hud);
		lv_obj_set_width(vol_fill, 30);
		lv_obj_set_style_bg_opa(vol_fill, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(vol_fill, C(th.bright_fg), 0);
	}
	lv_obj_set_height(vol_fill, 300 * v / 100);
	lv_obj_align(vol_fill, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_remove_flag(vol_hud, LV_OBJ_FLAG_HIDDEN);
	if (vol_timer)
		lv_timer_reset(vol_timer);
	else {
		vol_timer = lv_timer_create(vol_hide, 1500, NULL);
		lv_timer_set_repeat_count(vol_timer, 1);
	}
	ui_haptic_tap();
}

/* ---------- ticks ---------- */

static void poweroff_now(lv_timer_t *t)
{
	(void)t;
	sys_poweroff();
}

static void low_battery_cb(int idx, void *ud)
{
	(void)ud;
	if (idx == 1)
		power_set_lpm(true);
}

/* Low battery (iOS-like): an alert at 20 % and at 10 % (once each until the charger is plugged in
 * again), and at 3 % a clean shutdown (sync, /data unmounted) before the battery is flat. The gauge
 * is sampled every 30 s, so 3 % has to hold for two samples. */
static void battery_watch(void)
{
	static int warned = 101;	/* lowest level warned about since the charger was last in */
	static uint32_t empty_since;
	static bool shutting_down;
	int pct = sys_battery_pct();
	if (pct < 0 || shutting_down)
		return;
	if (sys_charger_online()) {
		warned = 101;
		empty_since = 0;
		return;
	}
	if (pct <= 3) {
		if (!empty_since)
			empty_since = lv_tick_get() | 1;
		if (lv_tick_elaps(empty_since) < 65000)
			return;
		shutting_down = true;
		screen_set(false);
		ui_alert("Shutting Down", "The battery is empty. Connect a charger, then start the phone again.", NULL, 0,
			 NULL, NULL);
		lv_timer_set_repeat_count(lv_timer_create(poweroff_now, 4000, NULL), 1);
		return;
	}
	empty_since = 0;
	int level = pct <= 10 ? 10 : pct <= 20 ? 20 : 101;
	if (level >= warned)
		return;
	warned = level;
	char msg[160];
	static const char *ok[] = { "Close", "Low Power Mode" };
	bool offer = !power_lpm();	/* like iOS: the alert offers Low Power Mode */
	snprintf(msg, sizeof(msg), "%d%% of battery remaining.%s", pct,
		 offer ? " Low Power Mode lowers the CPU's top speed and background activity." : "");
	screen_set(false);
	ui_alert("Low Battery", msg, ok, offer ? 2 : 1, low_battery_cb, NULL);
}

static void tick(lv_timer_t *t)
{
	(void)t;
	battery_watch();
	bool lock = power_tick();	/* auto-lock due; while dark it keeps the UI asleep */
	if (screen_off)
		return;	/* nothing to show: screen_set() catches up when it comes back on */
	status_tick();
	home_tick();
	lock_tick();
	cc_tick();
	if (lock)
		screen_set(true);
}

void shell_wallpaper_changed(void)
{
	/* home and lock screen hold image objects pointing at the wallpaper buffers */
	home_wallpaper_changed();
	lock_wallpaper_changed();
	lv_obj_invalidate(lv_screen_active());
	lv_obj_invalidate(lv_layer_top());
}

void shell_init(void)
{
	ui_init();
	theme_wallpaper_select(prefs_int("wallpaper", 0));
	sys_haptics_enabled = prefs_int("haptics", 1);
	lv_obj_set_style_bg_color(lv_screen_active(), C(0x000000), 0);
	lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);
	make_layers();
	home_create(lv_screen_active());
	zoom = box(lv_screen_active());
	lv_obj_set_size(zoom, SCR_W, SCR_H);
	to_area = (lv_area_t){ 0, 0, SCR_W - 1, SCR_H - 1 };
	frame.header.magic = LV_IMAGE_HEADER_MAGIC;
	frame.header.cf = LV_COLOR_FORMAT_XRGB8888;
	frame.data = malloc(SCR_W * SCR_H * 4);
	lv_obj_remove_flag(zoom, LV_OBJ_FLAG_CLICKABLE);	/* taps reach the home screen while it shrinks */
	lv_obj_add_flag(zoom, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_event_cb(zoom, zoom_event, LV_EVENT_COVER_CHECK, NULL);
	lv_obj_add_event_cb(zoom, zoom_event, LV_EVENT_DRAW_MAIN, NULL);
	make_status_bar();
	lock_create();
	cc_create();
	blackout = box(lv_layer_sys());
	lv_obj_set_size(blackout, SCR_W, SCR_H);
	lv_obj_set_style_bg_opa(blackout, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(blackout, lv_color_black(), 0);
	lv_obj_add_flag(blackout, LV_OBJ_FLAG_HIDDEN);
	lv_obj_add_flag(blackout, LV_OBJ_FLAG_CLICKABLE);
	if (prefs_int("start_locked", 1))
		lock_show(true);
	power_init();
	tick(NULL);
	power_keep(lv_timer_create(tick, 1000, NULL), 30000, "shell");	/* dark: the low-battery watch */
}
