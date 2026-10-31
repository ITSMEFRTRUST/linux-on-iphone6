/* Control Center (iOS 18 grid, solid fills instead of glass, on the blurred wallpaper):
 * connectivity + Now Playing platters, tall brightness / volume sliders, round toggles
 * (lock, haptics, Terminal, Calculator, Timer, Settings, Low Power Mode), live CPU / memory tile, power menu.
 * Pull down from the top zone to open (the panel follows the finger and settles on release);
 * tap empty space, drag it back up or press Home to close. */
#include <stdio.h>
#include <string.h>
#include "orchard.h"

#define U 140	/* grid unit: 4 columns */
#define GAP 30
#define X(c) (50 + (c) * (U + GAP))
#define Y(r) (150 + (r) * (U + GAP))
#define SPAN(n) ((n) * U + ((n) - 1) * GAP)

static lv_obj_t *cc, *c_usb, *c_wifi, *c_bt, *c_bt_dot, *c_dnd, *c_haptic, *c_timer, *c_lpm, *np_title, *np_artist, *np_play, *np_ctl;
static lv_obj_t *br_tile, *br_fill, *vol_tile, *vol_fill, *info;
static bool is_open;
static uint32_t tile, inner;

static void set_y(void *o, int32_t v) { slide_move(o, 0, v); }

/* ---------- pieces ---------- */

static lv_obj_t *circle(lv_obj_t *parent, int x, int y, int d, uint32_t bg, const char *glyph, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *b = ui_icon_button(parent, glyph, d, bg, pal.label);
	lv_obj_set_pos(b, x, y);
	if (cb)
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	return b;
}

/* cc_tick runs every second while Control Center is open: setting a style or a label to the value
 * it already has still redraws the object (27% of the screen, ~9 ms a second), so only changes go
 * through */
static void set_text(lv_obj_t *l, const char *s)
{
	if (strcmp(lv_label_get_text(l), s))
		lv_label_set_text(l, s);
}

static void set_opa(lv_obj_t *o, lv_opa_t v)
{
	if (lv_obj_get_style_opa(o, 0) != v)
		lv_obj_set_style_opa(o, v, 0);
}

static void set_text_opa(lv_obj_t *o, lv_opa_t v)
{
	if (lv_obj_get_style_text_opa(o, 0) != v)
		lv_obj_set_style_text_opa(o, v, 0);
}

static void circle_set(lv_obj_t *b, bool on, uint32_t color, uint32_t off)
{
	lv_color_t bg = C(on ? color : off), fg = C(on ? 0xffffff : pal.label);
	if (!lv_color_eq(lv_obj_get_style_bg_color(b, 0), bg))
		lv_obj_set_style_bg_color(b, bg, 0);
	if (!lv_color_eq(lv_obj_get_style_text_color(lv_obj_get_child(b, 0), 0), fg))
		lv_obj_set_style_text_color(lv_obj_get_child(b, 0), fg, 0);
}

static lv_obj_t *platter(int x, int y, int w, int h)
{
	lv_obj_t *p = box(cc);
	lv_obj_set_pos(p, x, y);
	lv_obj_set_size(p, w, h);
	lv_obj_set_style_radius(p, 60, 0);
	lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(p, C(tile), 0);
	return p;
}

/* ---------- actions ---------- */

static void launch(lv_event_t *e) { shell_launch(lv_event_get_user_data(e)); }

static void lock_now(lv_event_t *e)
{
	(void)e;
	shell_cc_toggle();
	shell_screen_toggle();
}

static void toggle_haptics(lv_event_t *e)
{
	(void)e;
	sys_haptics_enabled = !sys_haptics_enabled;
	prefs_set_int("haptics", sys_haptics_enabled);
	ui_haptic_tap();
	cc_tick();
}

/* Low Power Mode: tap = on/off, hold = Settings > Battery */
static void lpm_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_LONG_PRESSED) {
		ui_haptic_tap();
		settings_open("Battery");
	} else if (code == LV_EVENT_SHORT_CLICKED) {
		power_set_lpm(!power_lpm());
		ui_haptic_tap();
		cc_tick();
	}
}

static void toggle_dnd(lv_event_t *e)
{
	(void)e;
	prefs_set_int("dnd", !prefs_int("dnd", 0));
	ui_haptic_tap();
	cc_tick();
}

/* Wi-Fi: tap = wifiup on/off (shown at once), hold = Settings > Wi-Fi */
static bool wifi_shown;
static uint32_t wifi_tap_at;

static void wifi_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_LONG_PRESSED) {
		ui_haptic_tap();
		settings_open("Wi-Fi");	/* closes Control Center */
	} else if (code == LV_EVENT_SHORT_CLICKED) {
		if (wifi_state() == WIFI_NONE) {
			sys_haptic(30);
			return;
		}
		wifi_shown = wifi_state() == WIFI_OFF;
		wifi_tap_at = lv_tick_get();
		wifi_set_power(wifi_shown);
		ui_haptic_tap();
		cc_tick();
	}
}

/* Bluetooth: tap = power (the switch shows the new state at once), hold = Settings > Bluetooth */
static bool bt_shown;
static uint32_t bt_tap_at;

static void bt_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_LONG_PRESSED) {
		ui_haptic_tap();
		settings_open("Bluetooth");	/* closes Control Center */
	} else if (code == LV_EVENT_SHORT_CLICKED) {
		if (!bt_available()) {
			sys_haptic(30);
			return;
		}
		bt_shown = !bt_powered();
		bt_tap_at = lv_tick_get();
		bt_set_power(bt_shown);
		ui_haptic_tap();
		cc_tick();
	}
}

static void np_btn(lv_event_t *e)
{
	int k = (intptr_t)lv_event_get_user_data(e);
	if (!music_available())
		return;
	if (k < 0)
		music_prev();
	else if (k > 0)
		music_next();
	else
		music_toggle();
	ui_haptic_tap();
	cc_tick();
}

static void power_cb(int idx, void *ud)
{
	(void)ud;
	if (idx == 1)
		sys_reboot();
	else if (idx == 2)
		sys_poweroff();
}

static void power_menu(lv_event_t *e)
{
	(void)e;
	ui_alert("Power", "Restart boots iOS \xe2\x80\x94 Linux needs DFU mode and the PC again.",
		 (const char *[]){ "Cancel", "!Restart", "!Shut Down" }, 3, power_cb, NULL);
}

/* tall sliders: drag relative to where the finger landed, like iOS */
static int drag_y0, drag_v0, last_vol_ms, last_vol;

static int slider_get(lv_obj_t *t) { return t == br_tile ? sys_backlight_get() * 100 / sys_backlight_max() : sys_volume_get(); }

static void slider_fill(lv_obj_t *t, int pct)
{
	lv_obj_t *f = t == br_tile ? br_fill : vol_fill;
	if (lv_obj_get_style_height(f, 0) != SPAN(2) * pct / 100)
		lv_obj_set_height(f, SPAN(2) * pct / 100);
}

static void slider_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *t = lv_event_get_current_target(e);
	lv_point_t p;
	if (t == vol_tile && !sys_volume_available())
		return;
	lv_indev_get_point(lv_indev_active(), &p);
	if (code == LV_EVENT_PRESSED) {
		drag_y0 = p.y;
		drag_v0 = slider_get(t);
		return;
	}
	if (code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED)
		return;
	int v = drag_v0 + (drag_y0 - p.y) * 100 / SPAN(2);
	v = v < 0 ? 0 : v > 100 ? 100 : v;
	slider_fill(t, v);
	if (t == br_tile) {
		v = v < 3 ? 3 : v;
		sys_backlight_set(sys_backlight_max() * v / 100);
	} else if (v != last_vol && (code == LV_EVENT_RELEASED || lv_tick_elaps(last_vol_ms) > 90)) {
		sys_volume_set(v);	/* spawns amixer: throttled */
		last_vol = v;
		last_vol_ms = lv_tick_get();
	}
}

static lv_obj_t *tall_slider(int x, const char *glyph, lv_obj_t **fill)
{
	lv_obj_t *t = platter(x, Y(2), U, SPAN(2));
	lv_obj_set_style_radius(t, 56, 0);
	lv_obj_set_style_clip_corner(t, true, 0);
	lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(t, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* a drag up must not close the panel */
	lv_obj_add_event_cb(t, slider_event, LV_EVENT_ALL, NULL);
	*fill = box(t);
	lv_obj_remove_flag(*fill, LV_OBJ_FLAG_CLICKABLE);	/* a press on the filled part must reach the tile */
	lv_obj_set_width(*fill, U);
	lv_obj_set_style_bg_opa(*fill, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(*fill, C(0xffffff), 0);
	lv_obj_align(*fill, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_t *g = ui_icon_line(t, glyph, font(F_MONO, 48), 0x8e8e93);	/* sun + speaker on one centre line */
	lv_obj_align(g, LV_ALIGN_BOTTOM_MID, 0, -30);
	return t;
}

/* ---------- open / close ---------- */

/* the panel is a slide: moving it shifts its pixels, only the strip it uncovers is drawn */
static int drag_from, finger_y0;
static bool dragged;

static void settled(lv_anim_t *a)
{
	(void)a;
	slide_track(cc, false);
	if (!is_open)
		lv_obj_add_flag(cc, LV_OBJ_FLAG_HIDDEN);
}

static void grab(void)
{
	lv_anim_delete(cc, set_y);
	if (lv_obj_has_flag(cc, LV_OBJ_FLAG_HIDDEN)) {
		lv_display_enable_invalidation(NULL, false);	/* off screen: nothing to draw yet */
		lv_obj_set_y(cc, -SCR_H);
		lv_obj_refr_pos(cc);
		lv_obj_remove_flag(cc, LV_OBJ_FLAG_HIDDEN);
		lv_display_enable_invalidation(NULL, true);
		cc_tick();
	}
	slide_track(cc, true);
}

static void go(bool open, int ms)
{
	grab();
	is_open = open;
	motion_anim(cc, set_y, lv_obj_get_y(cc), open ? 0 : -SCR_H, ms, settled);
	status_refresh();
}

void shell_cc_toggle(void)
{
	if (!cc)
		return;
	if (!is_open)
		ui_haptic_tap();
	go(!is_open, MOTION_PAGE);
}

void cc_drag(int phase, int dy, int vy)
{
	if (!cc)
		return;
	if (phase == 0) {
		grab();
		drag_from = lv_obj_get_y(cc);	/* catches it mid-way if it was still moving */
		return;
	}
	int y = drag_from + dy;
	y = y < -SCR_H ? -SCR_H : y > 0 ? 0 : y;
	if (phase == 1) {
		slide_move(cc, 0, y);
		return;
	}
	/* released: a flick decides, otherwise how far it got; it leaves at the finger's speed */
	bool open = drag_from < 0 ? vy > 200 || (vy > -300 && y > -SCR_H + 160)	/* pulled down: easy to open */
				  : vy > -300 && y > -SCR_H / 4;	/* dragged up from open: a flick or a quarter closes */
	if (open && !is_open)
		ui_haptic_tap();
	go(open, motion_fling_ms(open ? y : SCR_H + y, open ? vy : -vy, MOTION_PAGE));
}

bool shell_cc_open(void) { return is_open; }

static void cc_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *in = lv_indev_active();
	lv_point_t p = { 0, 0 };
	if (!is_open || !in)
		return;
	lv_indev_get_point(in, &p);
	if (code == LV_EVENT_PRESSED) {
		dragged = false;
		finger_y0 = p.y;
	} else if (code == LV_EVENT_PRESSING && lv_event_get_target(e) == cc) {
		/* dragged up from empty space: the panel goes with the finger */
		if (!dragged && p.y - finger_y0 < -16) {
			dragged = true;
			cc_drag(0, 0, 0);
		}
		if (dragged)
			cc_drag(1, p.y - finger_y0, 0);
	} else if (code == LV_EVENT_RELEASED && dragged) {
		lv_point_t v;
		lv_indev_get_vect(in, &v);
		cc_drag(2, p.y - finger_y0, v.y * 60);	/* vect = px per 16 ms read */
	} else if (code == LV_EVENT_CLICKED && lv_event_get_target(e) == cc && !dragged) {
		shell_cc_toggle();
	} else if (code == LV_EVENT_GESTURE && !dragged && lv_indev_get_gesture_dir(in) == LV_DIR_TOP) {
		lv_indev_wait_release(in);
		shell_cc_toggle();
	}
}

/* ---------- live state ---------- */

void cc_tick(void)
{
	if (!cc || lv_obj_has_flag(cc, LV_OBJ_FLAG_HIDDEN))
		return;
	circle_set(c_usb, sys_usbnet_up(), pal.green, inner);
	bool wifi_on = wifi_tap_at && lv_tick_elaps(wifi_tap_at) < 2500 ? wifi_shown : wifi_state() >= WIFI_STARTING;
	circle_set(c_wifi, wifi_on, pal.blue, inner);
	set_text_opa(lv_obj_get_child(c_wifi, 0), wifi_state() != WIFI_NONE ? LV_OPA_COVER : LV_OPA_40);
	bool bt_on = bt_tap_at && lv_tick_elaps(bt_tap_at) < 2500 ? bt_shown : bt_powered();
	circle_set(c_bt, bt_on, pal.blue, inner);
	set_text_opa(lv_obj_get_child(c_bt, 0), bt_available() ? LV_OPA_COVER : LV_OPA_40);
	if (bt_on && bt_connected())	/* a device is connected: small badge */
		lv_obj_remove_flag(c_bt_dot, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(c_bt_dot, LV_OBJ_FLAG_HIDDEN);
	circle_set(c_dnd, prefs_int("dnd", 0), pal.indigo, inner);
	circle_set(c_haptic, sys_haptics_enabled, pal.tint, tile);
	circle_set(c_lpm, power_lpm(), pal.yellow, tile);

	int left;
	char buf[160];
	if (clock_timer_running(&left)) {
		snprintf(buf, sizeof(buf), "%d:%02d", left / 60, left % 60);
		if (lv_obj_get_style_text_font(lv_obj_get_child(c_timer, 0), 0) != font(F_TEXT_SEMI, 34))
			lv_obj_set_style_text_font(lv_obj_get_child(c_timer, 0), font(F_TEXT_SEMI, 34), 0);
		circle_set(c_timer, true, pal.orange, tile);
	} else {
		snprintf(buf, sizeof(buf), G_HOURGLASS);
		if (lv_obj_get_style_text_font(lv_obj_get_child(c_timer, 0), 0) != font(F_MONO, U * 4 / 10))
			lv_obj_set_style_text_font(lv_obj_get_child(c_timer, 0), font(F_MONO, U * 4 / 10), 0);
		circle_set(c_timer, false, pal.orange, tile);
	}
	ui_icon_set(lv_obj_get_child(c_timer, 0), buf);	/* the glyph fitted again, the time a plain label */

	const char *t = music_title(), *ar = music_artist();
	set_text(np_title, t ? t : "Not Playing");
	set_text(np_artist, t && ar ? ar : music_available() ? "Music" : "No songs");
	ui_icon_set(np_play, music_playing() ? G_PAUSE : G_PLAY);
	set_opa(np_ctl, music_available() ? LV_OPA_COVER : LV_OPA_40);

	slider_fill(br_tile, slider_get(br_tile));
	slider_fill(vol_tile, sys_volume_available() ? sys_volume_get() : 0);
	set_opa(vol_tile, sys_volume_available() ? LV_OPA_COVER : LV_OPA_50);

	int total, avail, mhz = sys_cpu_mhz(0);
	long up = sys_uptime_s();
	sys_mem_mb(&total, &avail);
	int n = snprintf(buf, sizeof(buf), "CPU %d%%", sys_cpu_load());
	if (mhz)
		n += snprintf(buf + n, sizeof(buf) - n, " \xc2\xb7 %.1f GHz", mhz / 1000.0);
	snprintf(buf + n, sizeof(buf) - n, "\n%d MB free\nup %ld:%02ld", avail, up / 3600, up / 60 % 60);
	set_text(info, buf);
}

/* ---------- build ---------- */

void cc_create(void)
{
	tile = th.dark ? mix(th.bg, th.fg, 14) : mix(0xffffff, th.fg, 16);
	inner = mix(tile, th.fg, 12);
	cc = box(shell_layer(LY_CC));
	lv_obj_set_size(cc, SCR_W, SCR_H);
	lv_obj_add_flag(cc, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(cc, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_add_event_cb(cc, cc_event, LV_EVENT_ALL, NULL);
	ui_material(cc);
	lv_obj_set_style_clip_corner(cc, false, 0);

	lv_obj_t *pw = circle(cc, SCR_W - 50 - 76, 64, 76, tile, G_POWER, power_menu, NULL);
	(void)pw;
	lv_obj_t *hdr = text(cc, "Control Center", font(F_TEXT_BOLD, 34), pal.label);
	lv_obj_set_pos(hdr, 50, 80);

	/* connectivity */
	lv_obj_t *con = platter(X(0), Y(0), SPAN(2), SPAN(2));
	int d = 116, g = (SPAN(2) - 2 * d) / 3;
	c_usb = circle(con, g, g, d, inner, G_USB, NULL, NULL);
	c_wifi = circle(con, 2 * g + d, g, d, inner, G_WIFI, NULL, NULL);
	lv_obj_add_event_cb(c_wifi, wifi_event, LV_EVENT_ALL, NULL);
	c_bt = circle(con, g, 2 * g + d, d, inner, G_BLUETOOTH, NULL, NULL);
	lv_obj_add_event_cb(c_bt, bt_event, LV_EVENT_ALL, NULL);
	c_bt_dot = box(c_bt);
	lv_obj_set_size(c_bt_dot, 18, 18);
	lv_obj_align(c_bt_dot, LV_ALIGN_TOP_RIGHT, -14, 14);
	lv_obj_set_style_radius(c_bt_dot, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(c_bt_dot, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(c_bt_dot, C(0xffffff), 0);
	lv_obj_add_flag(c_bt_dot, LV_OBJ_FLAG_HIDDEN);
	c_dnd = circle(con, 2 * g + d, 2 * g + d, d, inner, G_MOON, toggle_dnd, NULL);

	/* now playing */
	lv_obj_t *np = platter(X(2), Y(0), SPAN(2), SPAN(2));
	lv_obj_add_flag(np, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(np, launch, LV_EVENT_CLICKED, (void *)&app_music);
	lv_obj_t *art = box(np);
	lv_obj_set_size(art, 88, 88);
	lv_obj_set_pos(art, 28, 28);
	lv_obj_set_style_radius(art, 18, 0);
	lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(art, C(pal.tint), 0);
	lv_obj_set_style_bg_grad_color(art, C(mix(pal.tint, 0x000000, 45)), 0);
	lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
	lv_obj_center(ui_icon(art, G_MUSIC, font(F_MONO, 40), pal.on_tint));
	np_title = text(np, "", font(F_TEXT_SEMI, 28), pal.label);
	lv_obj_set_width(np_title, SPAN(2) - 56);
	lv_label_set_long_mode(np_title, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_pos(np_title, 28, 136);
	np_artist = text(np, "", font(F_TEXT, 26), pal.label2);
	lv_obj_set_width(np_artist, SPAN(2) - 56);
	lv_label_set_long_mode(np_artist, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_pos(np_artist, 28, 174);
	np_ctl = box(np);
	lv_obj_set_size(np_ctl, SPAN(2), 90);
	lv_obj_align(np_ctl, LV_ALIGN_BOTTOM_MID, 0, -6);
	static const char *ng[] = { G_BACKWARD, G_PLAY, G_FORWARD };
	for (int k = -1; k <= 1; k++) {
		lv_obj_t *b = box(np_ctl);
		lv_obj_set_size(b, 96, 90);
		lv_obj_align(b, LV_ALIGN_CENTER, k * 96, 0);
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_PRESSED);
		lv_obj_add_event_cb(b, np_btn, LV_EVENT_CLICKED, (void *)(intptr_t)k);
		lv_obj_t *l = ui_icon(b, ng[k + 1], font(F_MONO, k ? 38 : 48), pal.label);
		lv_obj_center(l);
		if (k == 0)
			np_play = l;
	}

	/* round toggles + shortcuts */
	circle(cc, X(0), Y(2), U, tile, G_LOCK, lock_now, NULL);
	c_haptic = circle(cc, X(1), Y(2), U, tile, G_HAND, toggle_haptics, NULL);
	circle(cc, X(0), Y(3), U, tile, ">_", launch, (void *)&app_terminal);
	circle(cc, X(1), Y(3), U, tile, G_CALC, launch, (void *)&app_calc);
	c_timer = circle(cc, X(0), Y(4), U, tile, G_HOURGLASS, launch, (void *)&app_clock);
	circle(cc, X(1), Y(4), U, tile, G_GEAR, launch, (void *)&app_settings);
	c_lpm = circle(cc, X(0), Y(5), U, tile, G_BATTERY, NULL, NULL);
	lv_obj_add_event_cb(c_lpm, lpm_event, LV_EVENT_ALL, NULL);

	br_tile = tall_slider(X(2), G_SUN_FILL, &br_fill);	/* as Settings: the outline sun looked like the gear */
	vol_tile = tall_slider(X(3), G_VOLUME, &vol_fill);

	/* system info */
	lv_obj_t *it = platter(X(2), Y(4), SPAN(2), U);
	lv_obj_set_style_radius(it, 56, 0);
	lv_obj_add_flag(it, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(it, launch, LV_EVENT_CLICKED, (void *)&app_settings);
	lv_obj_t *chip = ui_icon(it, G_MICROCHIP, font(F_MONO, 34), pal.label2);
	lv_obj_align(chip, LV_ALIGN_LEFT_MID, 26, 0);
	info = text(it, "", font(F_TEXT_SEMI, 20), pal.label);
	lv_obj_set_width(info, SPAN(2) - 96);
	lv_label_set_long_mode(info, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_align(info, LV_ALIGN_LEFT_MID, 78, 0);

	lv_obj_set_y(cc, -SCR_H);
	lv_obj_add_flag(cc, LV_OBJ_FLAG_HIDDEN);
}
