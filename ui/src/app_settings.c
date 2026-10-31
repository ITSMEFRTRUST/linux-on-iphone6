/* Settings (iOS 26 layout, no Liquid Glass): search, device card, Wi-Fi / Bluetooth / USB Network,
 * General (About, iPhone Storage, Date & Time, Keyboard, Restart / Shut Down), Display & Brightness
 * (Auto-Lock), Wallpaper, Sounds & Haptics, Focus, Battery. Bluetooth is real (bt.c): power switch,
 * My Devices (tap = connect, hold = menu, swipe left = Forget, (i) → Disconnect / Rename / Forget,
 * battery level when BlueZ has one), Other Devices found while the page is open. Wi-Fi is real too
 * (wifi.c): switch, the current network under it, My Networks / Other Networks (tap = join, password
 * page for secured ones, hold = menu, swipe left = Forget, (i) → IP / router / signal, Auto-Join,
 * Forget), "Other…" for hidden networks, Known Networks (saved ones, in range or not). */
#include <ctype.h>
#include <stdarg.h>
#include <dirent.h>
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include "orchard.h"
#include "src/misc/lv_timer_private.h"	/* lock_changed: finds the spinners by their callback */
#include "power_calc.h"

#define SIDE 32
#define W_CELL (SCR_W - 2 * SIDE)
#define W_NOTE (W_CELL - 64)	/* footers: lined up with the rows' text, like ui_section's */
#define VERSION "Orchard 0.2"
#define G_LINUX "\xef\x85\xbc"
#define G_INFO_O "\xf3\xb0\x8b\xbd"	/* md-information-outline */
#define G_BT_LINK "\xf3\xb0\x82\xb1"	/* md-bluetooth-connect */
#define G_BT_OFF "\xf3\xb0\x82\xb2"	/* md-bluetooth-off */
#define FLASH "/sys/block/asp0n1"	/* iOS user area (n2..n10 = firmware / NVRAM / syscfg) */

/* ---------- small helpers ---------- */

static lv_obj_t *col_box(lv_obj_t *parent)
{
	lv_obj_t *b = box(parent);
	lv_obj_set_size(b, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	return b;
}

static lv_obj_t *row_title(lv_obj_t *row) { return lv_obj_get_child(lv_obj_get_user_data(row), 0); }

static lv_obj_t *value_row(lv_obj_t *sec, const char *title, const char *value)
{
	lv_obj_t *r = ui_row(sec, NULL, 0, title);
	return ui_row_value(r, value);
}

/* a row whose title is an action ("Disconnect", "Restart") */
static lv_obj_t *action_row(lv_obj_t *sec, const char *title, uint32_t color, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *r = ui_row(sec, NULL, 0, title);
	lv_obj_set_style_text_color(row_title(r), C(color), 0);
	ui_row_tap(r, cb, ud);
	return r;
}

/* iOS activity indicator: 8 spokes, the bright one steps round 12 times a second. An lv_spinner
 * redraws at the full frame rate for as long as a scan runs (the Wi-Fi / Bluetooth pages: 60 fps
 * the whole time they are open) — this is a 12 fps timer that only invalidates the spinner. */
#define SPOKES 8
static void spin_draw(lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_current_target(e);
	lv_layer_t *L = lv_event_get_layer(e);
	lv_area_t co;
	lv_obj_get_coords(o, &co);
	int size = lv_area_get_width(&co), phase = (int)(intptr_t)lv_obj_get_user_data(o);
	float cx = co.x1 + size / 2.f, cy = co.y1 + size / 2.f, r0 = size * 0.24f, r1 = size * 0.46f;
	lv_draw_line_dsc_t d;
	lv_draw_line_dsc_init(&d);
	d.color = C(pal.label2);
	d.width = size / 9 + 2;
	d.round_start = d.round_end = 1;
	for (int i = 0; i < SPOKES; i++) {
		float a = (i * 2 * 3.14159265f) / SPOKES - 3.14159265f / 2;
		int age = (phase - i + SPOKES) % SPOKES;	/* 0 = the bright one, then fading behind it */
		d.opa = LV_OPA_COVER - age * (LV_OPA_COVER - 50) / (SPOKES - 1);
		d.p1.x = cx + r0 * cosf(a);
		d.p1.y = cy + r0 * sinf(a);
		d.p2.x = cx + r1 * cosf(a);
		d.p2.y = cy + r1 * sinf(a);
		lv_draw_line(L, &d);
	}
}

static void spin_tick(lv_timer_t *t)
{
	lv_obj_t *o = lv_timer_get_user_data(t);
	if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
		return;
	lv_obj_set_user_data(o, (void *)(((intptr_t)lv_obj_get_user_data(o) + 1) % SPOKES));
	lv_obj_invalidate(o);
}

static void spin_gone(lv_event_t *e) { lv_timer_delete(lv_event_get_user_data(e)); }

static lv_obj_t *spinner(lv_obj_t *parent, int size)
{
	lv_obj_t *s = box(parent);
	lv_obj_set_size(s, size, size);
	lv_obj_remove_flag(s, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_pad_all(s, 0, 0);
	lv_obj_add_event_cb(s, spin_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_timer_t *t = lv_timer_create(spin_tick, 83, s);
	lv_obj_add_event_cb(s, spin_gone, LV_EVENT_DELETE, t);
	return s;
}

static lv_obj_t *tile(lv_obj_t *parent, const char *glyph, uint32_t color, int size)
{
	lv_obj_t *t = box(parent);
	lv_obj_set_size(t, size, size);
	lv_obj_set_style_radius(t, size * 225 / 1000, 0);
	lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(t, C(color), 0);
	lv_obj_t *g = text(t, glyph, ui_icon_font(glyph, size * 60 / 100), ui_on_color(color));	/* as the list's tiles */
	ui_glyph_fit(g);
	lv_obj_center(g);
	return t;
}

/* iOS "hero" header inside a cell: big icon, title, short description */
static lv_obj_t *hero(lv_obj_t *sec, const char *glyph, uint32_t color, const char *title, const char *desc)
{
	lv_obj_t *h = box(sec);
	lv_obj_set_size(h, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_all(h, 36, 0);
	lv_obj_set_style_pad_row(h, 14, 0);
	lv_obj_set_flex_flow(h, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	tile(h, glyph, color, 116);
	lv_obj_t *t = text(h, title, font(F_TEXT_BOLD, 44), pal.label);
	lv_obj_set_style_pad_top(t, 8, 0);
	if (desc) {
		lv_obj_t *d = text(h, desc, font(F_TEXT, 28), pal.label2);
		lv_obj_set_width(d, LV_PCT(100));
		lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(d, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_line_space(d, 6, 0);
	}
	return h;
}

static void fmt_bytes(char *b, int n, double bytes)
{
	if (bytes >= 1e9)
		snprintf(b, n, bytes >= 1e10 ? "%.0f GB" : "%.1f GB", bytes / 1e9);
	else
		snprintf(b, n, "%.0f MB", bytes / 1e6);
}

static void pref_switch_cb(lv_event_t *e)
{
	const char *key = lv_event_get_user_data(e);
	bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	prefs_set_int(key, on);
	if (!strcmp(key, "haptics"))
		sys_haptics_enabled = on;
	ui_haptic_tap();
}

static lv_obj_t *pref_row(lv_obj_t *sec, const char *glyph, uint32_t color, const char *title, const char *key, int def)
{
	lv_obj_t *r = ui_row(sec, glyph, color, title);
	ui_row_switch(r, prefs_int(key, def), pref_switch_cb, (void *)key);
	return r;
}

/* ---------- page table (search + navigation) ---------- */

static void pg_wifi(void), pg_bluetooth(void), pg_usb(void), pg_general(void), pg_about(void), pg_storage(void),
	pg_datetime(void), pg_keyboard(void), pg_display(void), pg_autolock(void), pg_wallpaper(void),
	pg_sounds(void), pg_focus(void), pg_battery(void), pg_wifisleep(void), pg_camera(void);

static const struct pdef {
	const char *title, *parent, *keys, *glyph;
	int color;	/* index into the palette list in color_of() */
	void (*fn)(void);
} pdefs[] = {
	{ "Wi-Fi", NULL, "wifi wlan wireless internet network wpa password ssid router", G_WIFI, 0, pg_wifi },
	{ "Bluetooth", NULL, "bt headphones speaker pair audio airpods", G_BLUETOOTH, 0, pg_bluetooth },
	{ "USB Network", NULL, "usb telnet ip network ethernet pc", G_USB, 1, pg_usb },
	{ "VPN", NULL, "vpn tailscale tailnet magicdns remote", G_SHIELD, 0, ts_settings_page },
	{ "General", NULL, "settings", G_GEAR, 2, pg_general },
	{ "About", "General", "version kernel model chip cpu memory ram uptime name", G_INFO, 2, pg_about },
	{ "iPhone Storage", "General", "storage disk flash space capacity ios apfs", G_HDD, 2, pg_storage },
	{ "Date & Time", "General", "clock 24-hour time zone", G_CLOCK, 0, pg_datetime },
	{ "Keyboard", "General", "typing predictive suggestions capitalization", G_KEYBOARD, 2, pg_keyboard },
	{ "Display & Brightness", NULL, "brightness screen backlight", G_SUN_FILL, 0, pg_display },
	{ "Auto-Lock", "Display & Brightness", "lock sleep screen timeout", G_LOCK, 0, pg_autolock },
	{ "Wallpaper", NULL, "background theme", G_IMAGE, 3, pg_wallpaper },
	{ "Sounds & Haptics", NULL, "volume vibration haptic sound speaker output", G_VOLUME, 4, pg_sounds },
	{ "Focus", NULL, "do not disturb dnd silence notifications", G_MOON, 5, pg_focus },
	{ "Battery", NULL, "charging power charger current level percentage health low power mode saver drain time left usage", G_BATTERY, 1, pg_battery },
	{ "Wi-Fi Sleep", "Battery", "wifi wi-fi sleep offline tailnet tailscale screen off turn off after battery", G_WIFI, 0, pg_battery },
	{ "Camera", NULL, "camera photo photos selfie front mirror flip", G_CAMERA, 2, pg_camera },
	{ "Mirror Front Camera", "Camera", "camera selfie front mirror flip photo", G_CAMERA, 2, pg_camera },
};
#define NPDEF ((int)(sizeof(pdefs) / sizeof(pdefs[0])))

static uint32_t color_of(int i)
{
	switch (i) {
	case 0: return pal.blue;
	case 1: return pal.green;
	case 3: return pal.teal;
	case 4: return pal.red;
	case 5: return pal.indigo;
	default: return pal.gray;
	}
}

static void open_cb(lv_event_t *e)
{
	const struct pdef *p = &pdefs[(intptr_t)lv_event_get_user_data(e)];
	kbd_hide();
	if (p->fn)
		p->fn();
}

static lv_obj_t *nav_row(lv_obj_t *sec, int idx, const char *value)
{
	const struct pdef *p = &pdefs[idx];
	lv_obj_t *r = ui_row(sec, p->glyph, color_of(p->color), p->title);
	lv_obj_t *v = value ? ui_row_value(r, value) : NULL;
	ui_row_chevron(r);
	ui_row_tap(r, open_cb, (void *)(intptr_t)idx);
	return v;
}

static int pidx(const char *title)
{
	for (int i = 0; i < NPDEF; i++)
		if (!strcmp(pdefs[i].title, title))
			return i;
	return -1;
}

/* =============================== root =============================== */

static lv_obj_t *root_col, *main_box, *res_box, *search_ta, *v_wifi, *v_bt, *v_usb;
static lv_timer_t *root_timer;
static const char *pending_page;
static uint32_t pending_at;

static const char *bt_value(void)
{
	if (!bt_available())
		return bt_status();
	if (!bt_powered())
		return "Off";
	const char *a = bt_audio_name();
	return a ? a : "On";
}

/* a label set to the text it already has is still redrawn: only changes */
static void set_text(lv_obj_t *l, const char *s)
{
	if (l && strcmp(lv_label_get_text(l), s))
		lv_label_set_text(l, s);
}

static void root_refresh(void)
{
	set_text(v_wifi, wifi_status());
	set_text(v_bt, bt_value());
	set_text(v_usb, sys_usbnet_up() ? "Connected" : "Off");
}

static void root_tick(lv_timer_t *t)
{
	(void)t;
	root_refresh();
}

static void card_cb(lv_event_t *e)
{
	(void)e;
	pg_about();
}

static void build_main(lv_obj_t *c)
{
	char buf[96];
	/* device card (the Apple Account slot) */
	lv_obj_t *s = ui_section(c, NULL, NULL);
	lv_obj_t *r = ui_row(s, NULL, 0, "iPhone 6");
	lv_obj_set_style_pad_ver(r, 22, 0);
	lv_obj_t *av = box(r);
	lv_obj_set_size(av, 118, 118);
	lv_obj_set_style_radius(av, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(av, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(av, C(mix(pal.gray, 0xffffff, 20)), 0);
	lv_obj_set_style_bg_grad_color(av, C(mix(pal.gray, 0x000000, 30)), 0);
	lv_obj_set_style_bg_grad_dir(av, LV_GRAD_DIR_VER, 0);
	lv_obj_t *tux = text(av, G_LINUX, font(F_MONO, 66), 0xffffff);
	lv_obj_center(tux);
	ui_glyph_fit(tux);
	lv_obj_move_to_index(av, 0);
	lv_obj_set_style_text_font(row_title(r), font(F_TEXT_SEMI, 40), 0);
	snprintf(buf, sizeof(buf), "Linux %s \xc2\xb7 Orchard", sys_kernel());
	ui_row_subtitle(r, buf);
	ui_row_chevron(r);
	ui_row_tap(r, card_cb, NULL);

	s = ui_section(c, NULL, NULL);
	v_wifi = nav_row(s, pidx("Wi-Fi"), wifi_status());
	v_bt = nav_row(s, pidx("Bluetooth"), bt_value());
	v_usb = nav_row(s, pidx("USB Network"), sys_usbnet_up() ? "Connected" : "Off");
	ts_bind_label(nav_row(s, pidx("VPN"), ts_status()));

	s = ui_section(c, NULL, NULL);
	static const char *grp[] = { "General", "Display & Brightness", "Wallpaper", "Sounds & Haptics", "Focus", "Battery" };
	for (int i = 0; i < 6; i++)
		nav_row(s, pidx(grp[i]), NULL);
	s = ui_section(c, NULL, NULL);	/* apps (iOS: below the system groups) */
	nav_row(s, pidx("Camera"), NULL);

	lv_obj_t *v = text(c, VERSION, font(F_TEXT, 26), pal.label3);
	lv_obj_set_style_pad_top(v, 40, 0);
}

static bool matches(const struct pdef *p, const char *q)
{
	return strcasestr(p->title, q) || strcasestr(p->keys, q) || (p->parent && strcasestr(p->parent, q));
}

static void search_changed(lv_event_t *e)
{
	(void)e;
	const char *q = lv_textarea_get_text(search_ta);
	while (*q == ' ')
		q++;
	if (!*q) {
		lv_obj_remove_flag(main_box, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(res_box, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	lv_obj_add_flag(main_box, LV_OBJ_FLAG_HIDDEN);
	lv_obj_remove_flag(res_box, LV_OBJ_FLAG_HIDDEN);
	lv_obj_clean(res_box);
	lv_obj_t *sec = NULL;
	for (int i = 0; i < NPDEF; i++) {
		const struct pdef *p = &pdefs[i];
		if (!matches(p, q))
			continue;
		if (!sec)
			sec = ui_section(res_box, NULL, NULL);
		lv_obj_t *r = ui_row(sec, p->glyph, color_of(p->color), p->title);
		ui_row_subtitle(r, p->parent ? p->parent : "Settings");
		if (p->fn) {
			ui_row_chevron(r);
			ui_row_tap(r, open_cb, (void *)(intptr_t)i);
		} else {
			ui_row_value(r, "No driver");
			lv_obj_set_style_opa(r, LV_OPA_50, 0);
		}
	}
	if (!sec) {
		char b[120];
		snprintf(b, sizeof(b), "No Results for \xe2\x80\x9c%s\xe2\x80\x9d", q);
		lv_obj_t *l = text(res_box, b, font(F_TEXT_SEMI, 36), pal.label2);
		lv_obj_set_style_pad_top(l, 160, 0);
	}
}

static void search_click(lv_event_t *e)
{
	(void)e;
	kbd_no_autocap();	/* runs after ui.c's handler attached the keyboard */
}

static void search_focus(lv_event_t *e)
{
	bool on = lv_event_get_code(e) == EV_KBD_SHOWN;
	if (root_col)
		lv_obj_set_style_pad_bottom(root_col, on ? kbd_height(KBD_TEXT) + 60 : 140, 0);
}

static void search_ready(lv_event_t *e)
{
	(void)e;
	kbd_hide();
}

static void root_bt_changed(void) { root_refresh(); }

static void lock_changed(bool locked);

static void app_open(lv_obj_t *root)
{
	lock_listen(lock_changed, true);
	lv_obj_t *c = nav_root(root, "Settings");
	root_col = c;
	search_ta = ui_search(c, W_CELL, "Search");
	lv_obj_t *wrap = lv_obj_get_parent(search_ta);
	lv_obj_set_style_margin_bottom(wrap, 4, 0);
	lv_obj_add_event_cb(search_ta, search_click, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(wrap, search_click, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(search_ta, search_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(search_ta, search_focus, EV_KBD_SHOWN, NULL);
	lv_obj_add_event_cb(search_ta, search_focus, EV_KBD_HIDDEN, NULL);
	lv_obj_add_event_cb(search_ta, search_ready, LV_EVENT_READY, NULL);
	main_box = col_box(c);
	res_box = col_box(c);
	lv_obj_add_flag(res_box, LV_OBJ_FLAG_HIDDEN);
	build_main(main_box);
	bt_listen(root_bt_changed, true);
	wifi_listen(root_bt_changed, true);
	root_timer = lv_timer_create(root_tick, 2000, NULL);
	if (pending_page && lv_tick_elaps(pending_at) < 3000) {
		int i = pidx(pending_page);
		if (i >= 0 && pdefs[i].fn)
			pdefs[i].fn();
	}
	pending_page = NULL;
}

/* =============================== Bluetooth =============================== */

#define MAXROWS 32
static lv_obj_t *bt_page, *bt_sw, *bt_foot, *bt_box;
static lv_timer_t *bt_timer;
static bool bt_dirty;
static char row_addr[MAXROWS][18];
static uint32_t sw_at;
static bool sw_want;

static void bt_rebuild(void);

static void bt_sw_cb(lv_event_t *e)
{
	sw_want = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	sw_at = lv_tick_get();
	bt_set_power(sw_want);
	ui_haptic_tap();
}

static void pg_device(const char *addr);
static void pg_rename(const char *addr);
static void ask_forget(const char *addr);

static void bt_row_cb(lv_event_t *e)
{
	const char *a = row_addr[(intptr_t)lv_event_get_user_data(e)];
	bt_device_t d;
	if (!bt_device(a, &d) || d.connected || d.busy)
		return;
	ui_haptic_tap();
	bt_connect(a);
}

static void bt_info_cb(lv_event_t *e)
{
	pg_device(row_addr[(intptr_t)lv_event_get_user_data(e)]);
}

/* hold a device: Connect / Disconnect, Rename, Device Info, Forget */
static char menu_addr[18];

static void bt_menu_pick(int id)
{
	bt_device_t d;
	if (!bt_device(menu_addr, &d))
		return;
	switch (id) {
	case 0:
		if (d.busy)
			break;
		if (d.connected)
			bt_disconnect(menu_addr);
		else
			bt_connect(menu_addr);
		break;
	case 1:
		pg_rename(menu_addr);
		break;
	case 2:
		pg_device(menu_addr);
		break;
	case 3:
		ask_forget(menu_addr);
		break;
	}
}

static void bt_hold_cb(lv_event_t *e)
{
	const char *a = row_addr[(intptr_t)lv_event_get_user_data(e)];
	bt_device_t d;
	if (!bt_device(a, &d))
		return;
	snprintf(menu_addr, sizeof(menu_addr), "%s", a);
	ui_menu_item_t it[] = {
		{ 0, 0, d.connected ? G_BT_OFF : G_BT_LINK, d.connected ? "Disconnect" : "Connect" },
		{ 1, 0, G_PEN, "Rename" },
		{ 2, 0, G_INFO_O, "Device Info" },
		{ 3, MI_RED | MI_GROUP, G_TRASH, "Forget This Device" },
	};
	ui_row_menu(lv_event_get_current_target(e), it, 4, bt_menu_pick);
}

static void bt_swipe_cb(lv_event_t *e)
{
	ui_swipe_close();
	ask_forget(row_addr[(intptr_t)lv_event_get_user_data(e)]);
}

/* "▮ 80%" in front of the connection state */
static void battery_chip(lv_obj_t *row, int pct)
{
	static const char *g[] = { "\xef\x89\x84", "\xef\x89\x83", "\xef\x89\x82", "\xef\x89\x81", G_BATTERY };
	char b[8];
	lv_obj_t *c = box(row);
	lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(c, 6, 0);
	ui_glyph_fit(text(c, g[(pct + 12) / 25], font(F_MONO, 24), pct <= 20 ? pal.red : pal.label2));
	snprintf(b, sizeof(b), "%d%%", pct);
	text(c, b, font(F_TEXT, 28), pal.label2);
}

static const char *busy_text(enum bt_busy b)
{
	switch (b) {
	case BT_PAIRING: return "Pairing\xe2\x80\xa6";
	case BT_CONNECTING: return "Connecting\xe2\x80\xa6";
	case BT_DISCONNECTING: return "Disconnecting\xe2\x80\xa6";
	case BT_REMOVING: return "Forgetting\xe2\x80\xa6";
	default: return "";
	}
}

static lv_obj_t *device_row(lv_obj_t *sec, const bt_device_t *d, int slot, bool mine)
{
	lv_obj_t *r = ui_row(sec, NULL, 0, d->name);
	lv_label_set_max_lines(row_title(r), 1);
	if (!mine) {	/* strangers get a type glyph so a speaker stands out (My Devices: names only, like iOS) */
		lv_obj_t *g = text(r, bt_glyph(d->kind), font(F_MONO, 38), pal.label2);
		lv_obj_set_width(g, 48);
		lv_obj_set_style_text_align(g, LV_TEXT_ALIGN_CENTER, 0);
		ui_glyph_fit(g);
		lv_obj_move_to_index(g, lv_obj_get_index(lv_obj_get_user_data(r)));
		if (lv_obj_get_index(r) > 0)	/* separator: inset to the name, past the glyph */
			lv_obj_set_x(lv_obj_get_child(r, 0), 72);
	}
	if (d->busy) {
		spinner(r, 36);
		if (mine)
			ui_row_value(r, busy_text(d->busy));
	} else if (mine) {
		if (d->connected && d->battery >= 0)
			battery_chip(r, d->battery);
		ui_row_value(r, d->connected ? "Connected" : "Not Connected");
	}
	if (mine) {	/* (i): the whole row height and out to the cell's edge, so a thumb can't miss it */
		lv_obj_t *ib = box(r);
		lv_obj_set_size(ib, 100, 72);
		lv_obj_set_style_margin_right(ib, -32, 0);
		lv_obj_set_style_margin_left(ib, -16, 0);
		lv_obj_add_flag(ib, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(ib, 14);
		lv_obj_set_style_opa(ib, LV_OPA_50, LV_STATE_PRESSED);
		lv_obj_t *ig = text(ib, G_INFO_O, font(F_MONO, 46), pal.tint);
		lv_obj_center(ig);
		ui_glyph_fit(ig);	/* its right edge then meets the text inset, like a switch's */
		lv_obj_add_event_cb(ib, bt_info_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);
	}
	ui_row_tap(r, bt_row_cb, (void *)(intptr_t)slot);
	if (mine) {
		ui_row_hold(r, bt_hold_cb, (void *)(intptr_t)slot);
		ui_row_swipe(r, G_TRASH, "Forget", pal.red, bt_swipe_cb, (void *)(intptr_t)slot);
	}
	return r;
}

/* spinner right after the "Other devices" header text, like iOS */
static lv_obj_t *header_spinner(lv_obj_t *sec)
{
	lv_obj_t *h = lv_obj_get_child(lv_obj_get_parent(sec), lv_obj_get_index(sec) - 1);
	if (!h || !lv_obj_check_type(h, &lv_label_class))
		return NULL;
	lv_obj_update_layout(h);
	lv_point_t p;
	lv_label_get_letter_pos(h, strlen(lv_label_get_text(h)), &p);
	lv_obj_t *s = spinner(h, 30);
	lv_obj_add_flag(s, LV_OBJ_FLAG_FLOATING);
	lv_obj_set_pos(s, p.x + 16, p.y + 2);
	return s;
}

static const char *bt_footer(void)
{
	static char b[200];
	const char *st = bt_status();
	if (bt_powered())
		return "This iPhone is discoverable as \xe2\x80\x9ciPhone 6\xe2\x80\x9d while Bluetooth Settings is open.";
	if (bt_available())
		return NULL;
	if (!strcmp(st, "Not Installed"))
		return "BlueZ isn't on the phone yet \xe2\x80\x94 bin/audioboot installs it a minute after boot.";
	if (!strcmp(st, "No Bluetooth controller"))
		return "No Bluetooth controller (hci0): the BCM4345 didn't come up.";
	snprintf(b, sizeof(b), "Bluetooth is starting\xe2\x80\xa6");
	return b;
}

static void bt_rebuild(void)
{
	if (!bt_box)
		return;
	bt_dirty = false;
	bool on = lv_tick_elaps(sw_at) < 3000 ? sw_want : bt_powered();
	if (on != lv_obj_has_state(bt_sw, LV_STATE_CHECKED)) {
		if (on)
			lv_obj_add_state(bt_sw, LV_STATE_CHECKED);
		else
			lv_obj_remove_state(bt_sw, LV_STATE_CHECKED);
	}
	if (bt_available())
		lv_obj_remove_state(bt_sw, LV_STATE_DISABLED);
	else
		lv_obj_add_state(bt_sw, LV_STATE_DISABLED);
	lv_obj_set_style_opa(bt_sw, bt_available() ? LV_OPA_COVER : LV_OPA_40, 0);
	const char *f = bt_footer();
	if (f) {
		lv_label_set_text(bt_foot, f);
		lv_obj_remove_flag(bt_foot, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(bt_foot, LV_OBJ_FLAG_HIDDEN);
	}

	lv_obj_clean(bt_box);
	if (!bt_powered())
		return;
	bt_device_t d[MAXROWS];
	int n = bt_devices(d, MAXROWS), slot = 0;
	lv_obj_t *mine = NULL, *other = NULL;
	for (int i = 0; i < n; i++)
		if (d[i].paired) {
			if (!mine)
				mine = ui_section(bt_box, "My devices", NULL);
			snprintf(row_addr[slot], sizeof(row_addr[0]), "%s", d[i].addr);
			device_row(mine, &d[i], slot++, true);
		}
	int others = 0;
	for (int i = 0; i < n; i++)
		others += !d[i].paired && d[i].named;
	other = ui_section(bt_box, "Other devices",
			   others ? NULL : "Put your speaker or headphones in pairing mode and it shows up here.");
	if (bt_scanning() || !others)
		header_spinner(other);
	for (int i = 0; i < n && slot < MAXROWS; i++)
		if (!d[i].paired && d[i].named) {
			snprintf(row_addr[slot], sizeof(row_addr[0]), "%s", d[i].addr);
			device_row(other, &d[i], slot++, false);
		}
}

static bool dp_dirty;
static void dp_rebuild(void);

static void bt_changed(void) { bt_dirty = dp_dirty = true; }

/* rebuilding the rows under a finger would eat the tap: wait for the release */
static bool finger_down(void)
{
	for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
		if (lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED)
			return true;
	return false;
}

static void bt_tick(lv_timer_t *t)
{
	(void)t;
	if (finger_down() || ui_swipe_shown() || ui_menu_shown())
		return;	/* the rows stay put while a swipe action or a menu is open on one */
	if (bt_dirty || (lv_tick_elaps(sw_at) >= 3000 && lv_tick_elaps(sw_at) < 3400))
		bt_rebuild();
	if (dp_dirty)
		dp_rebuild();
}

static void bt_leave(void)
{
	bt_scan(false);
	bt_listen(bt_changed, false);
	if (bt_timer)
		lv_timer_delete(bt_timer);
	bt_timer = NULL;
	bt_page = bt_sw = bt_foot = bt_box = NULL;
}

static void pg_bluetooth(void)
{
	if (bt_page)
		return;
	lv_obj_t *c = nav_push("Bluetooth");
	if (!c)
		return;
	bt_page = nav_page();
	lv_obj_t *s = ui_section(c, NULL, NULL);
	hero(s, G_BLUETOOTH, pal.blue, "Bluetooth",
	     "Connect speakers and headphones to play Music through them. Pair a new one below while it's in "
	     "pairing mode.");
	lv_obj_t *r = ui_row(s, NULL, 0, "Bluetooth");
	bt_sw = ui_row_switch(r, bt_powered(), bt_sw_cb, NULL);
	bt_foot = text(c, "", font(F_TEXT, 26), pal.label2);
	lv_obj_set_width(bt_foot, W_NOTE);
	lv_label_set_long_mode(bt_foot, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_pad_top(bt_foot, 12, 0);
	lv_obj_set_style_text_line_space(bt_foot, 6, 0);
	bt_box = col_box(c);
	sw_at = lv_tick_get() - 10000;
	bt_rebuild();
	bt_listen(bt_changed, true);
	bt_timer = lv_timer_create(bt_tick, 300, NULL);
	bt_scan(true);
	nav_on_leave(bt_leave);
}

/* ---------- one device ---------- */

static char dp_addr[18];
static lv_obj_t *dp_page, *dp_box;

static const char *kind_name(enum bt_kind k)
{
	static const char *n[] = { "Accessory", "Headphones", "Speaker", "Computer", "Phone", "Keyboard / Mouse", "TV",
				   "Watch" };
	return n[k];
}

static void dp_connect_cb(lv_event_t *e)
{
	(void)e;
	ui_haptic_tap();
	bt_connect(dp_addr);
}

static void dp_disconnect_cb(lv_event_t *e)
{
	(void)e;
	ui_haptic_tap();
	bt_disconnect(dp_addr);
}

static char forget_addr[18];

static void forget_cb(int idx, void *ud)
{
	(void)ud;
	if (idx != 1)
		return;
	bt_forget(forget_addr);
	if (dp_page && nav_page() == dp_page && !strcmp(dp_addr, forget_addr))
		nav_pop();
}

static void ask_forget(const char *addr)
{
	bt_device_t d;
	char t[120];
	if (!bt_device(addr, &d))
		return;
	snprintf(forget_addr, sizeof(forget_addr), "%s", addr);
	snprintf(t, sizeof(t), "Forget \xe2\x80\x9c%s\xe2\x80\x9d?", d.name);
	ui_alert(t, "It will need to be paired again before you can use it.",
		 (const char *[]){ "Cancel", "!Forget Device" }, 2, forget_cb, NULL);
}

static void dp_forget_cb(lv_event_t *e)
{
	(void)e;
	ask_forget(dp_addr);
}

static void dp_rename_cb(lv_event_t *e)
{
	(void)e;
	pg_rename(dp_addr);
}

static void dp_rebuild(void)
{
	bt_device_t d;
	dp_dirty = false;
	if (!dp_box)
		return;
	if (!bt_device(dp_addr, &d)) {	/* forgotten / gone */
		if (nav_page() == dp_page)
			nav_pop();
		return;
	}
	lv_obj_clean(dp_box);
	nav_set_title(dp_page, d.name);	/* renamed */
	const char *audio = bt_audio_name();
	bool here = audio && d.connected && !strcmp(audio, d.name);
	lv_obj_t *s = ui_section(dp_box, NULL, NULL);
	hero(s, bt_glyph(d.kind), d.connected ? pal.blue : pal.gray, d.name,
	     d.busy ? busy_text(d.busy) : d.connected ? (here ? "Connected \xc2\xb7 Music plays here" : "Connected")
						    : "Not Connected");

	s = ui_section(dp_box, NULL, NULL);
	lv_obj_t *r;
	if (d.connected)
		r = action_row(s, "Disconnect", pal.tint, dp_disconnect_cb, NULL);
	else
		r = action_row(s, "Connect", pal.tint, dp_connect_cb, NULL);
	if (d.busy) {
		lv_obj_set_style_opa(r, LV_OPA_40, 0);
		lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
		spinner(r, 36);
	}
	action_row(s, "Forget This Device", pal.red, dp_forget_cb, NULL);

	s = ui_section(dp_box, NULL,
		       d.audio ? "While it's connected, Music and every other sound play through it. Disconnecting "
				 "pauses Music and switches back to the iPhone speaker."
			       : NULL);
	r = ui_row(s, NULL, 0, "Name");
	ui_row_value(r, d.name);
	ui_row_chevron(r);
	ui_row_tap(r, dp_rename_cb, NULL);
	value_row(s, "Device Type", kind_name(d.kind));
	if (d.connected && d.battery >= 0) {
		char b[8];
		snprintf(b, sizeof(b), "%d%%", d.battery);
		value_row(s, "Battery", b);
	}
	value_row(s, "Address", d.addr);
	value_row(s, "Audio", d.audio ? (here ? "In Use" : "Speaker / Headphones") : "No");
	if (d.rssi) {
		char b[24];
		snprintf(b, sizeof(b), "%d dBm", d.rssi);
		value_row(s, "Signal", b);
	}
}

static void dp_leave(void) { dp_page = dp_box = NULL; }

/* ---------- rename (the BlueZ alias) ---------- */

static char rn_addr[18], rn_was[64];
static lv_obj_t *rn_ta;

static void rn_done(lv_event_t *e)
{
	(void)e;
	if (rn_ta)
		nav_pop();	/* rn_leave saves */
}

/* back, Done or the keyboard's "done" all keep the new name (iOS); empty = the device's own name */
static void rn_leave(void)
{
	if (!rn_ta)
		return;
	char t[64];
	const char *s = lv_textarea_get_text(rn_ta);
	while (*s == ' ')
		s++;
	snprintf(t, sizeof(t), "%s", s);
	utf8_trim(t);	/* 63 bytes: never half a character */
	for (int n = strlen(t); n > 0 && t[n - 1] == ' '; n--)
		t[n - 1] = 0;
	if (strcmp(t, rn_was))
		bt_rename(rn_addr, t);
	rn_ta = NULL;
}

static void pg_rename(const char *addr)
{
	bt_device_t d;
	if (!bt_device(addr, &d))
		return;
	lv_obj_t *c = nav_push("Name");
	if (!c)
		return;
	snprintf(rn_addr, sizeof(rn_addr), "%s", addr);
	snprintf(rn_was, sizeof(rn_was), "%s", d.name);
	lv_obj_t *gap = box(c);
	lv_obj_set_size(gap, 1, 36);
	rn_ta = ui_text_field(c, W_CELL, d.name);
	lv_obj_set_style_pad_hor(lv_obj_get_parent(rn_ta), 32, 0);	/* the text lines up with the note under it */
	lv_textarea_set_text(rn_ta, d.name);
	lv_textarea_set_max_length(rn_ta, 48);
	lv_obj_add_event_cb(rn_ta, rn_done, LV_EVENT_READY, NULL);
	lv_obj_t *f = text(c, "Shown in Bluetooth, Control Center and Music. Leave it empty for the name the device gives "
			      "itself.", font(F_TEXT, 26), pal.label2);
	lv_obj_set_width(f, W_NOTE);
	lv_label_set_long_mode(f, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_pad_top(f, 12, 0);
	lv_obj_set_style_text_line_space(f, 6, 0);
	nav_button("Done", rn_done, NULL);
	nav_on_leave(rn_leave);
	kbd_attach(rn_ta, KBD_TEXT);
}

static void pg_device(const char *addr)
{
	bt_device_t d;
	if (!bt_device(addr, &d))
		return;
	lv_obj_t *c = nav_push(d.name);
	if (!c)
		return;
	snprintf(dp_addr, sizeof(dp_addr), "%s", addr);
	dp_page = nav_page();
	dp_box = col_box(c);
	dp_rebuild();	/* later changes: bt_tick (the Bluetooth page underneath keeps listening) */
	nav_on_leave(dp_leave);
}

/* =============================== Wi-Fi =============================== */

#define G_EYE "\xef\x81\xae"
#define G_EYE_SLASH "\xef\x81\xb0"
#define WMAX 40
enum { NR_TOP, NR_MINE, NR_OTHER, NR_KNOWN };

static lv_obj_t *wf_page, *wf_sw, *wf_top, *wf_cur, *wf_foot, *wf_box, *wf_spin;
static lv_timer_t *wf_timer;
static bool wf_dirty, kn_dirty, np_dirty;
static char wf_ssid[WMAX][33], kn_ssid[WMAX][33];	/* row → network (the rows' user data points here) */
static uint32_t wf_sw_at;
static bool wf_sw_want;

static void pg_net(const char *ssid);
static void pg_password(const char *ssid);
static void pg_other(void);
static void pg_known(void);
static void wf_ask_forget(const char *ssid);

static void wf_sw_cb(lv_event_t *e)
{
	wf_sw_want = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	wf_sw_at = lv_tick_get();
	wifi_set_power(wf_sw_want);
	ui_haptic_tap();
	wf_dirty = true;
}

/* tap a network: join it (saved / open), or ask for its password first */
static void wf_open_net(const char *ssid)
{
	wifi_net_t n;
	if (!wifi_network(ssid, &n) || n.joining)
		return;
	if (n.current || !n.bars) {
		pg_net(ssid);
		return;
	}
	if (n.saved || n.sec == WIFI_OPEN || n.sec == WIFI_OWE || n.sec == WIFI_EAP) {
		ui_haptic_tap();
		wifi_join(ssid, NULL);
		return;
	}
	pg_password(ssid);
}

static void wf_tap_cb(lv_event_t *e) { wf_open_net(lv_event_get_user_data(e)); }
static void wf_info_cb(lv_event_t *e) { pg_net(lv_event_get_user_data(e)); }

static void wf_swipe_cb(lv_event_t *e)
{
	ui_swipe_close();
	wf_ask_forget(lv_event_get_user_data(e));
}

/* hold a network: Join, Auto-Join, Network Info, Forget */
static char wf_menu_ssid[33];

static void wf_menu_pick(int id)
{
	wifi_net_t n;
	if (!wifi_network(wf_menu_ssid, &n))
		return;
	switch (id) {
	case 0: wf_open_net(wf_menu_ssid); break;
	case 1: wifi_set_autojoin(wf_menu_ssid, !n.autojoin); break;
	case 2: pg_net(wf_menu_ssid); break;
	case 3: wf_ask_forget(wf_menu_ssid); break;
	}
}

static void wf_hold_cb(lv_event_t *e)
{
	const char *ssid = lv_event_get_user_data(e);
	wifi_net_t n;
	if (!wifi_network(ssid, &n))
		return;
	snprintf(wf_menu_ssid, sizeof(wf_menu_ssid), "%s", ssid);
	ui_menu_item_t it[4];
	int k = 0;
	if (!n.current && !n.joining && n.bars)
		it[k++] = (ui_menu_item_t){ 0, 0, G_WIFI, "Join" };
	if (n.saved)
		it[k++] = (ui_menu_item_t){ 1, MI_CHECK | (n.autojoin ? MI_ON : 0), NULL, "Auto-Join" };
	it[k++] = (ui_menu_item_t){ 2, 0, G_INFO_O, "Network Info" };
	if (n.saved)
		it[k++] = (ui_menu_item_t){ 3, MI_RED | MI_GROUP, G_TRASH, "Forget Network" };
	ui_row_menu(lv_event_get_current_target(e), it, k, wf_menu_pick);
}

/* (i): the whole row height and out to the cell's edge, like the Bluetooth rows */
static void info_button(lv_obj_t *r, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *ib = box(r);
	lv_obj_set_size(ib, 100, 72);
	lv_obj_set_style_margin_right(ib, -32, 0);
	lv_obj_set_style_margin_left(ib, -16, 0);
	lv_obj_add_flag(ib, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(ib, 14);
	lv_obj_set_style_opa(ib, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_t *ig = text(ib, G_INFO_O, font(F_MONO, 46), pal.tint);
	lv_obj_center(ig);
	ui_glyph_fit(ig);
	lv_obj_add_event_cb(ib, cb, LV_EVENT_CLICKED, ud);
}

/* name … lock, bars, (i); the current network has a checkmark in front (a spinner while joining) */
static lv_obj_t *net_row(lv_obj_t *sec, const wifi_net_t *n, char *slot, int kind)
{
	snprintf(slot, 33, "%s", n->ssid);
	lv_obj_t *r = ui_row(sec, NULL, 0, n->name);
	lv_label_set_max_lines(row_title(r), 1);
	if (kind == NR_TOP) {
		lv_obj_t *lead = box(r);
		lv_obj_set_size(lead, 40, 40);
		lv_obj_remove_flag(lead, LV_OBJ_FLAG_CLICKABLE);
		if (n->joining) {
			lv_obj_center(spinner(lead, 34));
		} else {
			lv_obj_t *ck = text(lead, G_CHECK, font(F_MONO, 30), pal.tint);
			lv_obj_center(ck);
			ui_glyph_fit(ck);
		}
		lv_obj_move_to_index(lead, lv_obj_get_index(lv_obj_get_user_data(r)));
		lv_obj_set_style_margin_right(lead, -6, 0);
	}
	if (kind == NR_KNOWN) {
		if (n->joining)
			spinner(r, 36);
		ui_row_value(r, !n->autojoin ? "Auto-Join Off" : n->current ? "Connected" : n->bars ? "" : "Not in Range");
	} else {
		lv_obj_t *g = box(r);
		lv_obj_set_size(g, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(g, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(g, 16, 0);
		if (n->sec != WIFI_OPEN && n->sec != WIFI_OWE)
			ui_glyph_fit(text(g, G_LOCK, font(F_MONO, 26), pal.label));
		lv_obj_t *ic = wifi_icon(g, 40);
		lv_obj_set_style_text_color(ic, C(pal.label), 0);
		wifi_icon_set(ic, n->bars);
	}
	info_button(r, wf_info_cb, slot);
	ui_row_tap(r, wf_tap_cb, slot);
	ui_row_hold(r, wf_hold_cb, slot);
	if (n->saved)
		ui_row_swipe(r, G_TRASH, "Forget", pal.red, wf_swipe_cb, slot);
	return r;
}

static void other_cb(lv_event_t *e)
{
	(void)e;
	pg_other();
}

static void known_cb(lv_event_t *e)
{
	(void)e;
	pg_known();
}

static const char *wf_footer(void)
{
	switch (wifi_state()) {
	case WIFI_NONE:
		return "Wi-Fi isn't available: there's no /usr/sbin/wifiup on the phone (an older boot image).";
	case WIFI_STARTING:
		return wifi_starting_ms() < 20000 ? "Wi-Fi is starting\xe2\x80\xa6"
						 : "wpa_supplicant isn't answering (no /var/run/wpa_supplicant/wlan0). Turn Wi-Fi "
						   "off and on again to restart it.";
	default:
		return NULL;
	}
}

static void wf_footer_sync(void)
{
	const char *f = wf_footer();
	if (f && strcmp(lv_label_get_text(wf_foot), f))
		lv_label_set_text(wf_foot, f);
	if (!f != lv_obj_has_flag(wf_foot, LV_OBJ_FLAG_HIDDEN)) {
		if (f)
			lv_obj_remove_flag(wf_foot, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(wf_foot, LV_OBJ_FLAG_HIDDEN);
	}
}

static void wf_rebuild(void)
{
	if (!wf_box)
		return;
	wf_dirty = false;
	enum wifi_state s = wifi_state();
	bool on = lv_tick_elaps(wf_sw_at) < 3000 ? wf_sw_want : s >= WIFI_STARTING;
	if (on != lv_obj_has_state(wf_sw, LV_STATE_CHECKED)) {
		if (on)
			lv_obj_add_state(wf_sw, LV_STATE_CHECKED);
		else
			lv_obj_remove_state(wf_sw, LV_STATE_CHECKED);
	}
	if (s != WIFI_NONE)
		lv_obj_remove_state(wf_sw, LV_STATE_DISABLED);
	else
		lv_obj_add_state(wf_sw, LV_STATE_DISABLED);
	lv_obj_set_style_opa(wf_sw, s != WIFI_NONE ? LV_OPA_COVER : LV_OPA_40, 0);
	wf_footer_sync();

	if (wf_cur)
		lv_obj_delete(wf_cur);
	wf_cur = wf_spin = NULL;
	lv_obj_clean(wf_box);
	if (s != WIFI_ON)
		return;
	wifi_net_t n[WMAX];
	int cnt = wifi_networks(n, WMAX), slot = 0, top = -1, mine = 0, saved = 0;
	for (int i = 0; i < cnt; i++)	/* the one being joined, else the current one, sits under the switch */
		if (n[i].joining || (n[i].current && top < 0))
			top = i;
	if (top >= 0)
		wf_cur = net_row(wf_top, &n[top], wf_ssid[slot++], NR_TOP);
	for (int i = 0; i < cnt; i++) {
		mine += i != top && n[i].saved && n[i].bars;
		saved += n[i].saved;
	}
	if (mine) {
		lv_obj_t *sec = ui_section(wf_box, "My networks", NULL);
		for (int i = 0; i < cnt && slot < WMAX; i++)
			if (i != top && n[i].saved && n[i].bars)
				net_row(sec, &n[i], wf_ssid[slot++], NR_MINE);
	}
	lv_obj_t *sec = ui_section(wf_box, mine ? "Other networks" : "Networks", NULL);
	wf_spin = header_spinner(sec);
	if (wf_spin && !wifi_scanning())
		lv_obj_add_flag(wf_spin, LV_OBJ_FLAG_HIDDEN);
	for (int i = 0; i < cnt && slot < WMAX; i++)
		if (i != top && !n[i].saved && n[i].bars)
			net_row(sec, &n[i], wf_ssid[slot++], NR_OTHER);
	lv_obj_t *r = ui_row(sec, NULL, 0, "Other\xe2\x80\xa6");
	ui_row_tap(r, other_cb, NULL);
	if (saved) {
		char b[8];
		sec = ui_section(wf_box, NULL, "Networks the iPhone has joined. It joins them again by itself when they're in "
					     "range, unless Auto-Join is off.");
		r = ui_row(sec, NULL, 0, "Known Networks");
		snprintf(b, sizeof(b), "%d", saved);
		ui_row_value(r, b);
		ui_row_chevron(r);
		ui_row_tap(r, known_cb, NULL);
	}
}

/* ---------- one network ((i)) ---------- */

static char np_ssid[33];
static lv_obj_t *np_page, *np_box, *np_ip, *np_mask, *np_router, *np_dns, *np_sig, *np_speed;
static int np_shape;
static uint32_t np_at;

static int shape_of(const wifi_net_t *n)
{
	return n->current | n->saved << 1 | n->autojoin << 2 | n->joining << 3 | (n->bars > 0) << 4;
}

static void np_join_cb(lv_event_t *e)
{
	(void)e;
	wf_open_net(np_ssid);
}

static void np_forget_cb(lv_event_t *e)
{
	(void)e;
	wf_ask_forget(np_ssid);
}

static void np_auto_cb(lv_event_t *e)
{
	wifi_set_autojoin(np_ssid, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	ui_haptic_tap();
}

static void band(char *b, int n, int freq)
{
	int ch = wifi_channel(freq);
	const char *g = freq >= 5925 ? "6 GHz" : freq >= 4900 ? "5 GHz" : "2.4 GHz";
	if (ch)
		snprintf(b, n, "%s \xc2\xb7 Channel %d", g, ch);
	else
		snprintf(b, n, "%s", freq ? g : "");
}

static void set_value(lv_obj_t *l, const char *v)
{
	if (l && strcmp(lv_label_get_text(l), v))
		lv_label_set_text(l, v);
}

/* the live numbers, every 2 s while the page shows */
static void np_values(void)
{
	wifi_link_t l;
	char b[32];
	if (!np_box || !wifi_link(&l) || strcmp(l.ssid, np_ssid))
		return;
	set_value(np_ip, l.ip[0] ? l.ip : "Obtaining\xe2\x80\xa6");
	set_value(np_mask, l.mask);
	set_value(np_router, l.router);
	set_value(np_dns, l.dns);
	snprintf(b, sizeof(b), "%d dBm", l.rssi);
	set_value(np_sig, b);
	if (l.speed)
		snprintf(b, sizeof(b), "%d Mb/s", l.speed);
	else
		b[0] = 0;
	set_value(np_speed, b);
}

static void np_rebuild(void)
{
	wifi_net_t n;
	wifi_link_t l;
	char b[64];
	np_dirty = false;
	if (!np_box)
		return;
	if (!wifi_network(np_ssid, &n)) {	/* forgotten and out of range / Wi-Fi off */
		if (nav_page() == np_page)
			nav_pop();
		return;
	}
	np_shape = shape_of(&n);
	lv_obj_clean(np_box);
	np_ip = np_mask = np_router = np_dns = np_sig = np_speed = NULL;
	bool linked = n.current && wifi_link(&l);

	if ((!n.current && n.bars) || n.saved) {
		lv_obj_t *s = ui_section(np_box, NULL, NULL);
		if (!n.current && n.bars) {
			lv_obj_t *r = action_row(s, "Join This Network", pal.tint, np_join_cb, NULL);
			if (n.joining) {
				lv_obj_set_style_opa(r, LV_OPA_40, 0);
				lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
				spinner(r, 36);
			}
		}
		if (n.saved)
			action_row(s, "Forget This Network", pal.tint, np_forget_cb, NULL);
	}
	if (n.saved) {
		lv_obj_t *s = ui_section(np_box, NULL, "Joins this network by itself whenever it's in range.");
		lv_obj_t *r = ui_row(s, NULL, 0, "Auto-Join");
		ui_row_switch(r, n.autojoin, np_auto_cb, NULL);
	}
	if (linked) {
		lv_obj_t *s = ui_section(np_box, "IPv4 address", NULL);
		value_row(s, "Configure IP", "Automatic");
		np_ip = value_row(s, "IP Address", "");
		np_mask = value_row(s, "Subnet Mask", "");
		np_router = value_row(s, "Router", "");
		s = ui_section(np_box, "DNS", NULL);
		np_dns = value_row(s, "Servers", "");
	}
	lv_obj_t *s = ui_section(np_box, linked ? "Wi-Fi" : NULL, NULL);
	value_row(s, "Security", wifi_sec_name(n.sec));
	if (linked) {
		np_sig = value_row(s, "Signal", "");
		band(b, sizeof(b), l.freq);
		value_row(s, "Band", b);
		np_speed = value_row(s, "Link Speed", "");
		value_row(s, "Access Point", l.bssid);
		value_row(s, "Wi-Fi Address", l.mac[0] ? l.mac : "\xe2\x80\x94");
		np_values();
	} else {
		if (n.bars)
			snprintf(b, sizeof(b), "%d dBm", n.signal);
		value_row(s, "Signal", n.bars ? b : "Not in Range");
		if (n.freq) {
			band(b, sizeof(b), n.freq);
			value_row(s, "Band", b);
		}
	}
	np_at = lv_tick_get();
}

static void np_leave(void) { np_page = np_box = np_ip = np_mask = np_router = np_dns = np_sig = np_speed = NULL; }

static void pg_net(const char *ssid)
{
	wifi_net_t n;
	if (!wifi_network(ssid, &n))
		return;
	lv_obj_t *c = nav_push(n.name);
	if (!c)
		return;
	snprintf(np_ssid, sizeof(np_ssid), "%s", ssid);
	np_page = nav_page();
	np_box = col_box(c);
	np_rebuild();	/* later changes: wf_tick (the Wi-Fi page underneath keeps listening) */
	nav_on_leave(np_leave);
}

/* ---------- forget ---------- */

static char wf_forget_ssid[33];

static void wf_forget_cb(int idx, void *ud)
{
	(void)ud;
	if (idx != 1)
		return;
	wifi_forget(wf_forget_ssid);
	if (np_page && nav_page() == np_page && !strcmp(np_ssid, wf_forget_ssid))
		nav_pop();
}

static void wf_ask_forget(const char *ssid)
{
	wifi_net_t n;
	char t[160];
	if (!wifi_network(ssid, &n) || !n.saved)
		return;
	snprintf(wf_forget_ssid, sizeof(wf_forget_ssid), "%s", ssid);
	snprintf(t, sizeof(t), "Forget Wi-Fi Network \xe2\x80\x9c%s\xe2\x80\x9d?", n.name);
	ui_alert(t, "Your iPhone will no longer join this Wi-Fi network. Joining it again needs its password.",
		 (const char *[]){ "Cancel", "!Forget" }, 2, wf_forget_cb, NULL);
}

/* ---------- Known Networks ---------- */

static lv_obj_t *kn_box;

static void kn_rebuild(void)
{
	kn_dirty = false;
	if (!kn_box)
		return;
	lv_obj_clean(kn_box);
	wifi_net_t n[WMAX];
	int cnt = wifi_networks(n, WMAX), slot = 0;
	lv_obj_t *sec = NULL;
	for (int i = 0; i < cnt; i++) {	/* by name, like iOS */
		for (int j = i + 1; j < cnt; j++)
			if (strcasecmp(n[j].name, n[i].name) < 0) {
				wifi_net_t t = n[i];
				n[i] = n[j];
				n[j] = t;
			}
		if (!n[i].saved)
			continue;
		if (!sec)
			sec = ui_section(kn_box, NULL, "Swipe left on a network to forget it.");
		net_row(sec, &n[i], kn_ssid[slot++], NR_KNOWN);
	}
	if (!sec) {
		lv_obj_t *l = text(kn_box, "No Known Networks", font(F_TEXT_SEMI, 36), pal.label2);
		lv_obj_set_style_pad_top(l, 160, 0);
	}
}

static void kn_leave(void) { kn_box = NULL; }

static void pg_known(void)
{
	lv_obj_t *c = nav_push("Known Networks");
	if (!c)
		return;
	kn_box = col_box(c);
	kn_rebuild();
	nav_on_leave(kn_leave);
}

/* ---------- password / Other… ---------- */

static char pw_ssid[33];
static enum wifi_sec pw_sec;
static bool pw_other;
static lv_obj_t *pw_ta, *pw_name, *pw_join, *pw_box, *pw_secval;

static bool pw_valid(void)
{
	if (pw_other) {
		const char *nm = pw_name ? lv_textarea_get_text(pw_name) : "";
		if (!*nm || strlen(nm) > 32)
			return false;
	}
	return wifi_password_ok(pw_sec, pw_ta ? lv_textarea_get_text(pw_ta) : "");
}

static void pw_update(lv_event_t *e)
{
	(void)e;
	if (pw_join)
		lv_obj_set_style_opa(pw_join, pw_valid() ? LV_OPA_COVER : LV_OPA_40, 0);
}

static void pw_go(lv_event_t *e)
{
	(void)e;
	if (!pw_valid())
		return;
	char pw[72], nm[33];
	snprintf(pw, sizeof(pw), "%s", pw_sec == WIFI_OPEN || pw_sec == WIFI_OWE ? "" : lv_textarea_get_text(pw_ta));
	ui_haptic_tap();
	if (pw_other) {
		snprintf(nm, sizeof(nm), "%s", lv_textarea_get_text(pw_name));
		wifi_join_hidden(nm, pw_sec, pw);
	} else {
		wifi_join(pw_ssid, pw);
	}
	memset(pw, 0, sizeof(pw));
	nav_pop();
}

static void pw_ready(lv_event_t *e)
{
	if (lv_event_get_target(e) == pw_name && pw_ta && !lv_obj_has_flag(pw_box, LV_OBJ_FLAG_HIDDEN)) {
		kbd_attach(pw_ta, KBD_TEXT);	/* Name → Password */
		kbd_secure();	/* also while the eye shows it */
		return;
	}
	pw_go(e);
}

static void eye_cb(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_user_data(e);
	bool hide = !lv_textarea_get_password_mode(ta);
	lv_textarea_set_password_mode(ta, hide);
	ui_icon_set(lv_obj_get_child(lv_event_get_current_target(e), 0), hide ? G_EYE_SLASH : G_EYE);
}

static void field_tap(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_user_data(e);
	kbd_attach(ta, KBD_TEXT);
	kbd_no_autocap();
	if (ta == pw_ta)
		kbd_secure();	/* also while the eye shows it */
}

/* "Password  ••••••  (eye)": a label and a bare text field in one list row */
static lv_obj_t *field_row(lv_obj_t *sec, const char *title, const char *placeholder, bool secret)
{
	lv_obj_t *r = ui_row(sec, NULL, 0, title);
	lv_obj_t *col = lv_obj_get_user_data(r);
	lv_obj_set_flex_grow(col, 0);
	lv_obj_set_width(col, 160);
	lv_obj_t *ta = ui_text_field(r, 100, placeholder);
	lv_obj_t *wrap = lv_obj_get_parent(ta);
	lv_obj_set_flex_grow(wrap, 1);
	lv_obj_set_height(wrap, 64);
	lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
	lv_obj_set_style_pad_hor(wrap, 0, 0);
	lv_obj_add_event_cb(wrap, field_tap, LV_EVENT_CLICKED, ta);	/* after ui.c's attach: no auto-capitals */
	lv_obj_add_event_cb(ta, field_tap, LV_EVENT_CLICKED, ta);
	lv_obj_add_event_cb(ta, pw_update, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(ta, pw_ready, LV_EVENT_READY, NULL);
	ui_row_tap(r, field_tap, ta);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_STATE_PRESSED);	/* a field row doesn't flash */
	lv_textarea_set_max_length(ta, secret ? 64 : 32);
	if (secret) {
		lv_textarea_set_password_mode(ta, true);
		lv_textarea_set_password_show_time(ta, 900);
		lv_obj_t *eye = box(r);
		lv_obj_set_size(eye, 80, 72);
		lv_obj_set_style_margin_right(eye, -20, 0);
		lv_obj_add_flag(eye, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_opa(eye, LV_OPA_50, LV_STATE_PRESSED);
		lv_obj_t *eg = text(eye, G_EYE_SLASH, font(F_MONO, 32), pal.label2);
		lv_obj_center(eg);
		ui_glyph_fit(eg);
		lv_obj_add_event_cb(eye, eye_cb, LV_EVENT_CLICKED, ta);
	}
	return ta;
}

static lv_obj_t *caption(lv_obj_t *c, const char *s)
{
	lv_obj_t *l = text(c, s, font(F_TEXT_SEMI, 32), pal.label);
	lv_obj_set_width(l, W_CELL - 40);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_line_space(l, 6, 0);
	lv_obj_set_style_pad_top(l, 44, 0);
	return l;
}

static void pw_leave(void)
{
	pw_ta = pw_name = pw_join = pw_box = pw_secval = NULL;
	pw_other = false;
}

static void pg_password(const char *ssid)
{
	wifi_net_t n;
	char t[160];
	if (!wifi_network(ssid, &n))
		return;
	lv_obj_t *c = nav_push("Enter Password");
	if (!c)
		return;
	snprintf(pw_ssid, sizeof(pw_ssid), "%s", ssid);
	pw_sec = n.sec;
	pw_other = false;
	pw_name = NULL;
	snprintf(t, sizeof(t), "Enter the password for \xe2\x80\x9c%s\xe2\x80\x9d", n.name);
	caption(c, t);
	pw_box = ui_section(c, NULL, n.sec == WIFI_WEP ? "WEP keys are 5 or 13 characters (or 10 or 26 hex digits)." : NULL);
	pw_ta = field_row(pw_box, "Password", "", true);
	pw_join = nav_button("Join", pw_go, NULL);
	pw_update(NULL);
	nav_on_leave(pw_leave);
	kbd_attach(pw_ta, KBD_TEXT);
	kbd_secure();
}

static const struct { enum wifi_sec sec; const char *name, *item; } other_secs[] = {
	{ WIFI_OPEN, "None", "None" }, { WIFI_WEP, "WEP", "WEP" }, { WIFI_WPA, "WPA2/WPA3 Personal", "WPA2/WPA3" },
	{ WIFI_SAE, "WPA3 Personal", "WPA3" },
};

static const char *other_sec_name(enum wifi_sec s)
{
	for (int i = 0; i < 4; i++)
		if (other_secs[i].sec == s)
			return other_secs[i].name;
	return "";
}

static void sec_pick(int id)
{
	pw_sec = id;
	if (pw_secval)
		lv_label_set_text(pw_secval, other_sec_name(pw_sec));
	bool none = pw_sec == WIFI_OPEN;
	lv_obj_t *gap = lv_obj_get_child(lv_obj_get_parent(pw_box), lv_obj_get_index(pw_box) - 1);
	if (none) {
		lv_obj_add_flag(pw_box, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(gap, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_remove_flag(pw_box, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(gap, LV_OBJ_FLAG_HIDDEN);
	}
	pw_update(NULL);
}

static void sec_menu_cb(lv_event_t *e)
{
	ui_menu_item_t it[4];
	for (int i = 0; i < 4; i++)
		it[i] = (ui_menu_item_t){ other_secs[i].sec, MI_CHECK | (other_secs[i].sec == pw_sec ? MI_ON : 0), NULL,
					  other_secs[i].item };
	kbd_hide();
	ui_row_menu(lv_event_get_current_target(e), it, 4, sec_pick);
}

static void pg_other(void)
{
	lv_obj_t *c = nav_push("Other Network");
	if (!c)
		return;
	pw_other = true;
	pw_sec = WIFI_WPA;
	pw_ssid[0] = 0;
	caption(c, "Enter the name and security type of the network.");
	lv_obj_t *s = ui_section(c, NULL, NULL);
	pw_name = field_row(s, "Name", "Network Name", false);
	lv_obj_t *r = ui_row(s, NULL, 0, "Security");
	pw_secval = ui_row_value(r, other_sec_name(pw_sec));
	ui_row_chevron(r);
	ui_row_tap(r, sec_menu_cb, NULL);
	pw_box = ui_section(c, NULL, "Hidden networks don't show up in the list: the iPhone asks for this one by name.");
	pw_ta = field_row(pw_box, "Password", "", true);
	pw_join = nav_button("Join", pw_go, NULL);
	pw_update(NULL);
	nav_on_leave(pw_leave);
	kbd_attach(pw_name, KBD_TEXT);
	kbd_no_autocap();
}

/* ---------- the page ---------- */

static void wf_changed(void) { wf_dirty = kn_dirty = np_dirty = true; }

static void wf_tick(lv_timer_t *t)
{
	(void)t;
	if (wf_spin && wifi_scanning() == lv_obj_has_flag(wf_spin, LV_OBJ_FLAG_HIDDEN)) {
		if (wifi_scanning())
			lv_obj_remove_flag(wf_spin, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(wf_spin, LV_OBJ_FLAG_HIDDEN);
	}
	if (wf_foot)
		wf_footer_sync();
	if (finger_down() || ui_swipe_shown() || ui_menu_shown())
		return;	/* the rows stay put while a swipe action or a menu is open on one */
	if (wf_dirty || (lv_tick_elaps(wf_sw_at) >= 3000 && lv_tick_elaps(wf_sw_at) < 3400))
		wf_rebuild();
	if (kn_dirty)
		kn_rebuild();
	if (np_box) {
		wifi_net_t n;
		if (np_dirty && (!wifi_network(np_ssid, &n) || shape_of(&n) != np_shape))
			np_rebuild();
		np_dirty = false;
		if (lv_tick_elaps(np_at) >= 2000) {
			np_at = lv_tick_get();
			np_values();
		}
	}
}

static void wf_leave(void)
{
	wifi_scan(false);
	wifi_listen(wf_changed, false);
	if (wf_timer)
		lv_timer_delete(wf_timer);
	wf_timer = NULL;
	wf_page = wf_sw = wf_top = wf_cur = wf_foot = wf_box = wf_spin = NULL;
}

static void pg_wifi(void)
{
	if (wf_page)
		return;
	lv_obj_t *c = nav_push("Wi-Fi");
	if (!c)
		return;
	wf_page = nav_page();
	wf_top = ui_section(c, NULL, NULL);
	hero(wf_top, G_WIFI, pal.blue, "Wi-Fi",
	     "Connect to Wi-Fi, see the networks around you and choose which ones the iPhone joins by itself.");
	lv_obj_t *r = ui_row(wf_top, NULL, 0, "Wi-Fi");
	wf_sw = ui_row_switch(r, wifi_state() >= WIFI_STARTING, wf_sw_cb, NULL);
	wf_foot = text(c, "", font(F_TEXT, 26), pal.label2);
	lv_obj_set_width(wf_foot, W_NOTE);
	lv_label_set_long_mode(wf_foot, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_pad_top(wf_foot, 12, 0);
	lv_obj_set_style_text_line_space(wf_foot, 6, 0);
	wf_box = col_box(c);
	wf_sw_at = lv_tick_get() - 10000;
	wf_rebuild();
	wifi_listen(wf_changed, true);
	wf_timer = lv_timer_create(wf_tick, 300, NULL);
	wifi_scan(true);
	nav_on_leave(wf_leave);
}

/* =============================== USB Network =============================== */

static void pg_usb(void)
{
	lv_obj_t *c = nav_push("USB Network");
	if (!c)
		return;
	lv_obj_t *s = ui_section(c, NULL, NULL);
	hero(s, G_USB, pal.green, "USB Network", "The phone and the PC talk over the Lightning cable (usb0).");
	s = ui_section(c, NULL, "From the PC: telnet 172.16.43.1");
	value_row(s, "Status", sys_usbnet_up() ? "Connected" : "Not Connected");
	value_row(s, "iPhone", "172.16.43.1");
	value_row(s, "PC", "172.16.43.2");
}

/* =============================== General =============================== */

static void power_cb(int idx, void *ud)
{
	if (idx != 1)
		return;
	if ((intptr_t)ud)
		sys_poweroff();
	else
		sys_reboot();
}

static void restart_cb(lv_event_t *e)
{
	(void)e;
	ui_alert("Restart?", "Restarting boots iOS \xe2\x80\x94 Linux needs DFU mode and the PC again.",
		 (const char *[]){ "Cancel", "!Restart" }, 2, power_cb, (void *)0);
}

static void shutdown_cb(lv_event_t *e)
{
	(void)e;
	ui_alert("Shut Down?", "Linux needs DFU mode and the PC to start again.",
		 (const char *[]){ "Cancel", "!Shut Down" }, 2, power_cb, (void *)1);
}

static void pg_general(void)
{
	lv_obj_t *c = nav_push("General");
	if (!c)
		return;
	lv_obj_t *s = ui_section(c, NULL, NULL);
	nav_row(s, pidx("About"), NULL);
	s = ui_section(c, NULL, NULL);
	nav_row(s, pidx("iPhone Storage"), NULL);
	s = ui_section(c, NULL, NULL);
	nav_row(s, pidx("Date & Time"), NULL);
	nav_row(s, pidx("Keyboard"), NULL);
	s = ui_section(c, NULL, NULL);
	action_row(s, "Restart", pal.red, restart_cb, NULL);
	action_row(s, "Shut Down", pal.red, shutdown_cb, NULL);
}

/* ---------- About ---------- */

static lv_obj_t *ab_cpu, *ab_up, *ab_mem;
static lv_timer_t *ab_timer;
static double flash_capacity(void);

static void ab_tick(lv_timer_t *t)
{
	(void)t;
	long up = sys_uptime_s();
	int mhz = sys_cpu_mhz(0);
	if (mhz)
		lv_label_set_text_fmt(ab_cpu, "%d cores \xc2\xb7 %.2f GHz", sys_cpu_count(), mhz / 1000.0);
	else
		lv_label_set_text_fmt(ab_cpu, "%d cores", sys_cpu_count());
	int total, avail;
	sys_mem_mb(&total, &avail);
	lv_label_set_text_fmt(ab_mem, "%d MB", avail);
	if (up >= 86400)
		lv_label_set_text_fmt(ab_up, "%ldd %ldh %02ldm", up / 86400, up / 3600 % 24, up / 60 % 60);
	else
		lv_label_set_text_fmt(ab_up, "%ldh %02ldm %02lds", up / 3600, up / 60 % 60, up % 60);
}

static void ab_leave(void)
{
	if (ab_timer)
		lv_timer_delete(ab_timer);
	ab_timer = NULL;
	ab_cpu = ab_up = ab_mem = NULL;
}

static void pg_about(void)
{
	lv_obj_t *c = nav_push("About");
	if (!c)
		return;
	char b[96];
	int total, avail;
	sys_mem_mb(&total, &avail);
	lv_obj_t *s = ui_section(c, NULL, NULL);
	value_row(s, "Name", "iPhone 6");
	value_row(s, "Linux Version", sys_kernel());
	value_row(s, "Shell", VERSION);
	value_row(s, "Model Name", "iPhone 6");
	value_row(s, "Model Number", "N61AP");

	s = ui_section(c, NULL, NULL);
	value_row(s, "Chip", "Apple A8 (T7000)");
	ab_cpu = value_row(s, "CPU", "");
	snprintf(b, sizeof(b), "%d MB", total);
	value_row(s, "Memory", b);	/* RAM: Linux and everything it writes live here */
	ab_mem = value_row(s, "Memory Free", "");
	double cap = flash_capacity();
	if (cap > 0) {
		fmt_bytes(b, sizeof(b), cap);
		value_row(s, "Capacity", b);
	}
	FILE *bf = fopen("/tmp/bt-address", "r");	/* written by tools/btcal (the phone's own BMac) */
	if (bf) {
		char ba[32] = "";
		if (fgets(ba, sizeof(ba), bf) && (ba[strcspn(ba, "\n")] = 0, ba[0]))
			value_row(s, "Bluetooth", ba);
		fclose(bf);
	}
	ab_up = value_row(s, "Uptime", "");
	ab_tick(NULL);
	ab_timer = lv_timer_create(ab_tick, 1000, NULL);
	nav_on_leave(ab_leave);
}

/* ---------- iPhone Storage: the iOS flash (APFS container, read-only) by what uses it ---------- */

#define G_MOBILE "\xef\x84\x8b"	/* fa-mobile */
#define G_SIGNAL "\xef\x80\x92"	/* fa-signal */
#define G_DB "\xef\x87\x80"	/* fa-database */

static apfs_info_t fl;	/* filled by the reader thread; read once fl_state is 2 */
static int fl_state;	/* 0 not read, 1 reading, 2 done (cached for later visits when it worked) */
static lv_obj_t *st_box;
static lv_timer_t *st_timer;

static void *fl_reader(void *arg)
{
	static apfs_info_t t;
	apfs_read(arg, &t);
	fl = t;
	__atomic_store_n(&fl_state, 2, __ATOMIC_RELEASE);
	return NULL;
}

/* previews: ORCHARD_FLASH=demo = what a real phone reported (2026-10-18); another path = an image to parse */
static void fl_demo(void)
{
	static const apfs_info_t demo = {
		.ok = 1, .block_size = 4096, .total = 15999954944ULL, .free = 8118239232ULL, .xid = 0x18def,
		.free_exact = 1, .nvol = 3,
		.vol = { { "System", 0, 4527169536ULL, false }, { "Data", 0, 3311181824ULL, true },
			 { "Baseband Data", 0, 806912ULL, false } },
	};
	fl = demo;
	fl_state = 2;
}

static void fl_start(void)
{
	static char path[256];
	const char *e = getenv("ORCHARD_FLASH");
	if (__atomic_load_n(&fl_state, __ATOMIC_ACQUIRE) == 1)
		return;
	if (e && !strcmp(e, "demo")) {
		fl_demo();
		return;
	}
	snprintf(path, sizeof(path), "%s", e && *e ? e : APFS_DEV);
	fl_state = 1;
	pthread_attr_t at;
	pthread_t tid;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&tid, &at, fl_reader, path)) {
		memset(&fl, 0, sizeof(fl));
		fl.ok = -1;
		snprintf(fl.err, sizeof(fl.err), "no reader thread");
		fl_state = 2;
	}
	pthread_attr_destroy(&at);
}

/* About › Capacity: the container once read, else the raw size of the flash namespace */
static double flash_capacity(void)
{
	if (__atomic_load_n(&fl_state, __ATOMIC_ACQUIRE) == 2 && fl.ok == 1)
		return fl.total;
	const char *e = getenv("ORCHARD_FLASH");
	if (e && !strcmp(e, "demo"))
		return 15999954944.0;
	FILE *f = fopen(FLASH "/size", "r");
	long long sectors = 0;
	if (f) {
		if (fscanf(f, "%lld", &sectors) != 1)
			sectors = 0;
		fclose(f);
	}
	return sectors * 512.0;
}

struct cat {
	const char *name, *glyph, *sub;
	uint32_t color;
	double bytes;
};

/* volumes → what iOS would call them; plus the container's own bookkeeping */
static int fl_cats(struct cat *c, int max, char subs[][96])
{
	int n = 0;
	double vols = 0;
	for (int i = 0; i < fl.nvol && n < max - 1; i++) {
		const struct apfs_vol *v = &fl.vol[i];
		size_t l = strlen(v->name);
		struct cat *k = &c[n];
		k->bytes = v->used;
		vols += v->used;
		if (v->role == 0x1 || !strcmp(v->name, "System") || (l > 2 && !strcmp(v->name + l - 2, "OS"))) {
			*k = (struct cat){ "iOS System", G_MOBILE, "The iOS 12 system volume", pal.gray, v->used };
		} else if (v->role == 0x40 || !strcmp(v->name, "Data")) {
			*k = (struct cat){ "iOS Data", G_LOCK,
					   "Apps, photos, messages \xe2\x80\x94 encrypted, not readable by Linux", pal.blue,
					   v->used };
		} else if (v->role == 0x80 || strstr(v->name, "Baseband")) {
			*k = (struct cat){ "Cellular firmware", G_SIGNAL, "The modem's own volume", pal.green, v->used };
		} else {
			snprintf(subs[n], sizeof(subs[0]), "APFS volume%s", v->encrypted ? " \xc2\xb7 encrypted" : "");
			*k = (struct cat){ v->name, G_HDD, subs[n], pal.purple, v->used };
		}
		n++;
	}
	/* a "Linux" category (a file carved out inside iOS Data) slots in here once it exists */
	double meta = (double)fl.total - fl.free - vols;
	if (fl.free_exact && meta > 0 && n < max)
		c[n++] = (struct cat){ "File System", G_DB, "APFS bookkeeping: object map, checkpoints, space manager",
				       pal.teal, meta };
	return n;
}

static void st_card(lv_obj_t *sec, const struct cat *c, int n)
{
	char b[96], tot[24], used[24];
	fmt_bytes(tot, sizeof(tot), fl.total);
	fmt_bytes(used, sizeof(used), (double)fl.total - fl.free);
	lv_obj_t *card = box(sec);
	lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_pad_all(card, 32, 0);
	lv_obj_set_style_pad_row(card, 6, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	text(card, tot, font(F_DISPLAY_BOLD, 72), pal.label);
	snprintf(b, sizeof(b), "%s%s of %s Used", fl.free_exact ? "" : "About ", used, tot);
	text(card, b, font(F_TEXT, 30), pal.label2);

	int bw = W_CELL - 64;
	lv_obj_t *bar = box(card);
	lv_obj_set_size(bar, bw, 34);
	lv_obj_set_style_margin_top(bar, 22, 0);
	lv_obj_set_style_radius(bar, 9, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, C(pal.fill), 0);	/* the track = free space */
	lv_obj_set_style_clip_corner(bar, true, 0);
	lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(bar, 3, 0);
	for (int i = 0; i < n; i++) {
		if (c[i].bytes < fl.total * 0.0025)	/* slivers would only exaggerate */
			continue;
		int w = (int)(bw * c[i].bytes / fl.total);
		lv_obj_t *seg = box(bar);
		lv_obj_set_size(seg, w < 6 ? 6 : w, LV_PCT(100));
		lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(seg, C(c[i].color), 0);
	}

	lv_obj_t *leg = box(card);
	lv_obj_set_size(leg, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_style_margin_top(leg, 16, 0);
	lv_obj_set_flex_flow(leg, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(leg, 28, 0);
	lv_obj_set_style_pad_row(leg, 10, 0);
	for (int i = 0; i <= n; i++) {
		if (i < n && c[i].bytes < fl.total * 0.0025)
			continue;
		lv_obj_t *it = box(leg);
		lv_obj_set_size(it, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(it, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(it, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(it, 10, 0);
		lv_obj_t *dot = box(it);
		lv_obj_set_size(dot, 18, 18);
		lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(dot, C(i < n ? c[i].color : pal.fill), 0);
		text(it, i < n ? c[i].name : "Free", font(F_TEXT, 26), pal.label2);
	}
}

static void st_build(void)
{
	char b[96];
	lv_obj_clean(st_box);
	if (fl.ok != 1) {	/* nothing cached: the next visit tries again (the module may get loaded) */
		snprintf(b, sizeof(b), "The flash couldn't be read (%s).", fl.err);
		lv_obj_t *s = ui_section(st_box, NULL,
					 fl.ok == 0 ? "Flash driver not loaded, so iOS's storage can't be read. Linux itself "
						      "runs from RAM \xe2\x80\x94 see General \xe2\x80\xba About \xe2\x80\xba Memory."
						    : b);
		lv_obj_t *r = ui_row(s, G_HDD, pal.gray, "iPhone Storage");
		ui_row_value(r, "Not Available");
		fl_state = 0;
		return;
	}
	struct cat c[APFS_MAXVOL + 2];
	char subs[APFS_MAXVOL + 2][96];
	int n = fl_cats(c, APFS_MAXVOL + 2, subs);
	st_card(ui_section(st_box, NULL, NULL), c, n);

	lv_obj_t *s = ui_section(st_box, NULL,
				 "Read from the iOS flash (the APFS container on asp0n1p1), opened read-only. Linux runs "
				 "from RAM and never writes here, so iOS stays intact.");
	for (int i = 0; i <= n; i++) {
		bool free_row = i == n;
		lv_obj_t *r = ui_row(s, free_row ? G_HDD : c[i].glyph, free_row ? pal.fill : c[i].color,
				   free_row ? "Free" : c[i].name);
		ui_row_subtitle(r, free_row ? (fl.free_exact ? "Counted by the APFS space manager"
							     : "Estimated: capacity minus the volumes")
					    : c[i].sub);
		fmt_bytes(b, sizeof(b), free_row ? (double)fl.free : c[i].bytes);
		ui_row_value(r, b);
	}
}

static void st_tick(lv_timer_t *t)
{
	if (!st_box || __atomic_load_n(&fl_state, __ATOMIC_ACQUIRE) != 2)
		return;
	lv_timer_delete(t);
	st_timer = NULL;
	st_build();
}

static void st_leave(void)
{
	if (st_timer)
		lv_timer_delete(st_timer);
	st_timer = NULL;
	st_box = NULL;
}

static void pg_storage(void)
{
	lv_obj_t *c = nav_push("iPhone Storage");
	if (!c)
		return;
	st_box = col_box(c);
	if (__atomic_load_n(&fl_state, __ATOMIC_ACQUIRE) == 2 && fl.ok == 1) {	/* cached: iOS isn't running */
		st_build();
	} else {
		lv_obj_t *s = ui_section(st_box, NULL, NULL);
		lv_obj_t *r = ui_row(s, NULL, 0, "Calculating\xe2\x80\xa6");
		lv_obj_set_style_text_color(row_title(r), C(pal.label2), 0);
		spinner(r, 36);
		fl_start();
		st_timer = lv_timer_create(st_tick, 150, NULL);
	}
	nav_on_leave(st_leave);
}

/* ---------- Date & Time ---------- */

static void pg_datetime(void)
{
	lv_obj_t *c = nav_push("Date & Time");
	if (!c)
		return;
	char b[64];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	lv_obj_t *s = ui_section(c, NULL, NULL);
	pref_row(s, NULL, 0, "24-Hour Time", "clock24", 0);
	s = ui_section(c, NULL, "The time zone comes from the boot script (TZ).");
	const char *tz = getenv("TZ");
	value_row(s, "Time Zone", tz && *tz ? tz : "UTC");
	strftime(b, sizeof(b), "%a, %b %d, %Y", &tm);
	value_row(s, "Date", b);
}

/* ---------- Keyboard ---------- */

static void pg_keyboard(void)
{
	lv_obj_t *c = nav_push("Keyboard");
	if (!c)
		return;
	lv_obj_t *s = ui_section(c, "All keyboards", NULL);
	pref_row(s, NULL, 0, "Auto-Capitalization", "kbd_autocap", 1);
	pref_row(s, NULL, 0, "Suggestions", "kbd_suggest", 1);
	s = ui_section(c, NULL, "Keys you're likely to type next get a slightly bigger touch area, so a tap near a key "
				"border lands on the letter the word needs. A tap in the middle of a key always wins.");
	pref_row(s, NULL, 0, "Predictive Touch", "kbd_predict", 1);
	s = ui_section(c, NULL, NULL);
	pref_row(s, NULL, 0, "Keyboard Feedback", "kbd_haptic", 1);
}

/* ---------- Camera ---------- */

static void pg_camera(void)
{
	lv_obj_t *c = nav_push("Camera");
	if (!c)
		return;
	lv_obj_t *s = ui_section(c, "Composition", "Saves front camera photos as you see them in the viewfinder. "
				 "Off: they're flipped back to how others see you.");
	pref_row(s, NULL, 0, "Mirror Front Camera", "camera_mirror", 1);	/* (cam_dev_mirror, app_camera.c) */
}

/* =============================== Display & Brightness =============================== */

static lv_obj_t *v_autolock;

static const char *autolock_name(int s)
{
	switch (s) {
	case 30: return "30 Seconds";
	case 60: return "1 Minute";
	case 120: return "2 Minutes";
	case 300: return "5 Minutes";
	case 0: return "Never";
	default: return "Custom";
	}
}

static void bright_cb(lv_event_t *e)
{
	int v = lv_slider_get_value(lv_event_get_target(e));
	sys_backlight_set(v);
}

static void disp_leave(void) { v_autolock = NULL; }

static void autolock_open_cb(lv_event_t *e)
{
	(void)e;
	pg_autolock();
}

static void pg_display(void)
{
	lv_obj_t *c = nav_push("Display & Brightness");
	if (!c)
		return;
	int max = sys_backlight_max(), lo = max / 40 > 20 ? max / 40 : 20;
	int cur = sys_backlight_get();
	lv_obj_t *s = ui_section(c, "Brightness", NULL);
	lv_obj_t *sl = ui_row_slider(s, G_SUN_FILL, G_SUN_FILL, lo, max, cur < lo ? lo : cur, bright_cb, NULL);
	lv_obj_remove_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);
	s = ui_section(c, NULL, "Orchard starts on the Lock Screen after the phone boots.");
	lv_obj_t *r = ui_row(s, NULL, 0, "Auto-Lock");
	v_autolock = ui_row_value(r, autolock_name(power_autolock()));
	ui_row_chevron(r);
	ui_row_tap(r, autolock_open_cb, NULL);
	pref_row(s, NULL, 0, "Start Locked", "start_locked", 1);
	nav_on_leave(disp_leave);
}

static const int al_secs[] = { 30, 60, 120, 300, 0 };
static lv_obj_t *al_check[5];

static void al_cb(lv_event_t *e)
{
	int k = (intptr_t)lv_event_get_user_data(e);
	prefs_set_int("autolock", al_secs[k]);
	for (int i = 0; i < 5; i++)
		if (al_check[i])
			lv_obj_set_style_opa(al_check[i], i == k ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	if (v_autolock)
		lv_label_set_text(v_autolock, autolock_name(al_secs[k]));
	ui_haptic_tap();
}

static void al_leave(void)
{
	for (int i = 0; i < 5; i++)
		al_check[i] = NULL;
}

static void pg_autolock(void)
{
	lv_obj_t *c = nav_push("Auto-Lock");
	if (!c)
		return;
	int cur = power_autolock();
	lv_obj_t *s = ui_section(c, NULL, power_lpm() ? "Low Power Mode keeps Auto-Lock at 30 seconds or less."
					     : "The screen dims, then turns off and locks after this long without a touch. "
					       "Woken on the Lock Screen, it goes off again after 10 seconds.");
	for (int i = 0; i < 5; i++) {
		lv_obj_t *r = ui_row(s, NULL, 0, autolock_name(al_secs[i]));
		al_check[i] = ui_row_check(r, cur == al_secs[i]);
		ui_row_tap(r, al_cb, (void *)(intptr_t)i);
	}
	nav_on_leave(al_leave);
}

/* =============================== Wallpaper =============================== */

#define WP_MAX 12
static lv_obj_t *wp_img, *wp_check[WP_MAX];

static void wp_cb(lv_event_t *e)
{
	int k = (intptr_t)lv_event_get_user_data(e);
	if (k == theme_wallpaper_index())
		return;
	theme_wallpaper_select(k);
	prefs_set_int("wallpaper", k);
	shell_wallpaper_changed();
	if (wp_img)
		lv_image_set_src(wp_img, theme_wallpaper());
	for (int i = 0; i < WP_MAX; i++)
		if (wp_check[i])
			lv_obj_set_style_opa(wp_check[i], i == k ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	ui_haptic_tap();
}

static void wp_leave(void)
{
	wp_img = NULL;
	for (int i = 0; i < WP_MAX; i++)
		wp_check[i] = NULL;
}

static void pg_wallpaper(void)
{
	lv_obj_t *c = nav_push("Wallpaper");
	if (!c)
		return;
	/* preview: the lock screen look, scaled */
	lv_obj_t *pv = box(c);
	lv_obj_set_size(pv, 300, 534);
	lv_obj_set_style_margin_top(pv, 36, 0);
	lv_obj_set_style_radius(pv, 40, 0);
	lv_obj_set_style_clip_corner(pv, true, 0);
	lv_obj_set_style_border_width(pv, 2, 0);
	lv_obj_set_style_border_color(pv, C(pal.separator), 0);
	wp_img = lv_image_create(pv);
	lv_image_set_src(wp_img, theme_wallpaper());
	lv_obj_set_size(wp_img, 300, 534);
	lv_image_set_inner_align(wp_img, LV_IMAGE_ALIGN_COVER);
	char b[32];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	strftime(b, sizeof(b), "%A, %B %d", &tm);
	lv_obj_t *d = text(pv, b, font(F_TEXT_SEMI, 18), 0xffffff);
	lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 50);
	sys_clock_format(b, sizeof(b), false);
	lv_obj_t *t = text(pv, b, font(F_DISPLAY_BOLD, 84), 0xffffff);
	lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 72);

	int n = theme_wallpaper_count(), cur = theme_wallpaper_index();
	lv_obj_t *s = ui_section(c, "Wallpapers", "The first one comes from the PC's desktop theme when it has a wallpaper.");
	for (int i = 0; i < n && i < WP_MAX; i++) {
		lv_obj_t *r = ui_row(s, NULL, 0, theme_wallpaper_name(i));
		wp_check[i] = ui_row_check(r, i == cur);
		ui_row_tap(r, wp_cb, (void *)(intptr_t)i);
	}
	nav_on_leave(wp_leave);
}

/* =============================== Sounds & Haptics =============================== */

static uint32_t vol_ms;
static lv_obj_t *snd_out;

static void vol_cb(lv_event_t *e)
{
	lv_obj_t *sl = lv_event_get_target(e);
	bool done = lv_event_get_code(e) == LV_EVENT_RELEASED;
	if (done || lv_tick_elaps(vol_ms) > 90) {	/* each set spawns amixer */
		sys_volume_set(lv_slider_get_value(sl));
		vol_ms = lv_tick_get();
	}
}

static void snd_bt_changed(void)
{
	if (snd_out) {
		const char *a = bt_audio_name();
		lv_label_set_text(snd_out, a ? a : "iPhone Speaker");
	}
}

static void snd_leave(void)
{
	bt_listen(snd_bt_changed, false);
	snd_out = NULL;
}

static void pg_sounds(void)
{
	lv_obj_t *c = nav_push("Sounds & Haptics");
	if (!c)
		return;
	bool ok = sys_volume_available();
	lv_obj_t *s = ui_section(c, "Volume", ok ? NULL : "No mixer: amixer isn't on the phone yet (bin/audioboot installs it).");
	lv_obj_t *sl = ui_row_slider(s, G_VOLUME_LO, G_VOLUME, 0, 100, ok ? sys_volume_get() : 0, vol_cb, NULL);
	lv_obj_add_event_cb(sl, vol_cb, LV_EVENT_RELEASED, NULL);
	lv_obj_remove_flag(sl, LV_OBJ_FLAG_GESTURE_BUBBLE);
	if (!ok) {
		lv_obj_add_state(sl, LV_STATE_DISABLED);
		lv_obj_set_style_opa(lv_obj_get_parent(sl), LV_OPA_40, 0);
	}
	lv_obj_t *r = ui_row(s, NULL, 0, "Output");
	const char *a = bt_audio_name();
	snd_out = ui_row_value(r, a ? a : "iPhone Speaker");

	s = ui_section(c, "Haptics", "Taps on buttons and keys, alarms always buzz.");
	pref_row(s, NULL, 0, "System Haptics", "haptics", 1);
	pref_row(s, NULL, 0, "Keyboard Feedback", "kbd_haptic", 1);
	bt_listen(snd_bt_changed, true);
	nav_on_leave(snd_leave);
}

/* =============================== Focus =============================== */

static void pg_focus(void)
{
	lv_obj_t *c = nav_push("Focus");
	if (!c)
		return;
	lv_obj_t *s = ui_section(c, NULL, "Banners stay silent and go straight to the Lock Screen. Alarms and timers still "
				       "ring.");
	pref_row(s, G_MOON, pal.indigo, "Do Not Disturb", "dnd", 0);
}

/* =============================== Battery =============================== */

static lv_obj_t *bat_lvl, *bat_state, *bat_health, *bat_cyc, *bat_v, *bat_i, *bat_t, *bat_src, *bat_cur, *bat_lim,
	*bat_draw, *bat_left, *bat_left_sub, *use_time, *use_on, *use_off, *use_used, *bat_lpm, *bat_ws_row, *bat_ws_val;
static lv_timer_t *bat_timer;

static void set_fmt(lv_obj_t *l, const char *fmt, ...)
{
	char buf[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	set_text(l, buf);
}

/* everything comes from the sampler thread's last values (sys.c): no hardware read here */
static void bat_tick(lv_timer_t *t)
{
	(void)t;
	sys_battery_t b;
	sys_battery_info(&b);
	power_usage_t u;
	power_usage(&u);
	bool chg = sys_charger_online();
	char d[32];
	if (b.pct >= 0) {
		set_fmt(bat_lvl, "%d%%", b.pct);
		set_text(bat_state, chg ? (b.pct == 100 ? "Charged" : "Charging") : "On Battery");
		if (b.design_mah > 0 && b.full_mah > 0)
			set_fmt(bat_health, "%d%%", LV_MIN(100, b.full_mah * 100 / b.design_mah));
		set_fmt(bat_cyc, "%d", b.cycles);
		set_fmt(bat_v, "%d.%02d V", b.mv / 1000, b.mv % 1000 / 10);
		set_fmt(bat_i, "%d mA", b.ma);
		set_fmt(bat_t, "%d.%d \xc2\xb0" "C", b.temp_dc / 10, LV_ABS(b.temp_dc % 10));
		int mw = b.power_mw ? b.power_mw : b.ma * b.mv / 1000;
		if (chg)
			set_text(bat_draw, "\xe2\x80\x94");
		else
			set_fmt(bat_draw, "%d mA \xc2\xb7 %s%d.%02d W", -b.ma, mw > 0 ? "-" : "", LV_ABS(mw) / 1000,
				LV_ABS(mw) % 1000 / 10);	/* (draw: discharging is positive here) */
		long m = chg ? -1 : power_minutes_left(b.pct, u.rate_x10, b.charge_mah, b.ma);
		if (m >= 0) {
			power_fmt_dur(d, sizeof(d), m * 60);
			set_fmt(bat_left, "\xe2\x89\x88 %s", d);
		} else {
			set_text(bat_left, "\xe2\x80\x94");
		}
		set_text(bat_left_sub, chg ? "while on the charger: none" : u.rate_x10 ? "at the average since unplugged"
							     : "at the present draw");
	}
	if (u.valid) {
		power_fmt_dur(d, sizeof(d), u.secs);
		set_text(use_time, d);
		power_fmt_dur(d, sizeof(d), u.on_secs);
		set_text(use_on, d);
		power_fmt_dur(d, sizeof(d), u.off_secs);
		set_text(use_off, d);
		if (u.rate_x10)
			set_fmt(use_used, "%d%% \xc2\xb7 %d.%d%%/h", u.used_pct, u.rate_x10 / 10, u.rate_x10 % 10);
		else
			set_fmt(use_used, "%d%%", u.used_pct);
	} else {
		set_text(use_time, chg ? "Charging" : "\xe2\x80\x94");
		set_text(use_on, "\xe2\x80\x94");
		set_text(use_off, "\xe2\x80\x94");
		set_text(use_used, "\xe2\x80\x94");
	}
	set_text(bat_src, chg ? "USB" : "Battery");
	set_fmt(bat_cur, "%d mA", sys_charger_ma());
	set_fmt(bat_lim, "%d mA", sys_charger_limit_ma());
	if (lv_obj_has_state(bat_lpm, LV_STATE_CHECKED) != power_lpm()) {	/* (turned off by itself at 80 %) */
		if (power_lpm())
			lv_obj_add_state(bat_lpm, LV_STATE_CHECKED);
		else
			lv_obj_remove_state(bat_lpm, LV_STATE_CHECKED);
	}
}

static void lpm_cb(lv_event_t *e)
{
	power_set_lpm(lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	ui_haptic_tap();
}

/* Wi-Fi Sleep (power.c): the switch + "Turn Off After" (1 / 5 / 15 minutes, a picker like Auto-Lock's) */
static const int ws_mins[] = { 1, 5, 15 };
static lv_obj_t *ws_check[3];

static const char *ws_name(int m) { return m == 1 ? "1 Minute" : m == 15 ? "15 Minutes" : "5 Minutes"; }

static void ws_row_sync(void)
{
	if (!bat_ws_row)
		return;
	bool on = prefs_int("wifisleep", 0);
	lv_obj_set_style_opa(bat_ws_row, on ? LV_OPA_COVER : LV_OPA_40, 0);
	if (on)
		lv_obj_add_flag(bat_ws_row, LV_OBJ_FLAG_CLICKABLE);
	else
		lv_obj_remove_flag(bat_ws_row, LV_OBJ_FLAG_CLICKABLE);
	set_text(bat_ws_val, ws_name(prefs_int("wifisleep_min", 5)));
}

static void ws_switch_cb(lv_event_t *e)
{
	prefs_set_int("wifisleep", lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
	ws_row_sync();
	ui_haptic_tap();
}

static void ws_pick_cb(lv_event_t *e)
{
	int k = (intptr_t)lv_event_get_user_data(e);
	prefs_set_int("wifisleep_min", ws_mins[k]);
	for (int i = 0; i < 3; i++)
		if (ws_check[i])
			lv_obj_set_style_opa(ws_check[i], i == k ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	ws_row_sync();
	ui_haptic_tap();
}

static void ws_leave(void)
{
	for (int i = 0; i < 3; i++)
		ws_check[i] = NULL;
}

static void pg_wifisleep(void)
{
	lv_obj_t *c = nav_push("Turn Off After");
	if (!c)
		return;
	int cur = prefs_int("wifisleep_min", 5);
	lv_obj_t *s = ui_section(c, NULL, "Counted from when the screen goes off. Waking the phone brings Wi-Fi back at once.");
	for (int i = 0; i < 3; i++) {
		lv_obj_t *r = ui_row(s, NULL, 0, ws_name(ws_mins[i]));
		ws_check[i] = ui_row_check(r, cur == ws_mins[i] || (i == 1 && cur != 1 && cur != 15));
		ui_row_tap(r, ws_pick_cb, (void *)(intptr_t)i);
	}
	nav_on_leave(ws_leave);
}

static void ws_open_cb(lv_event_t *e)
{
	(void)e;
	pg_wifisleep();
}

static void bat_leave(void)
{
	if (bat_timer)
		lv_timer_delete(bat_timer);
	bat_timer = NULL;
	bat_lvl = bat_state = bat_health = bat_cyc = bat_v = bat_i = bat_t = bat_src = bat_cur = bat_lim = NULL;
	bat_draw = bat_left = bat_left_sub = use_time = use_on = use_off = use_used = bat_lpm = bat_ws_row = bat_ws_val = NULL;
}

static void pg_battery(void)
{
	lv_obj_t *c = nav_push("Battery");
	if (!c)
		return;
	bool gauge = sys_battery_pct() >= 0;
	lv_obj_t *s = ui_section(c, NULL, "Low Power Mode lowers the CPU's top speed, keeps Wi-Fi in power save, checks "
					   "the charger and Wi-Fi less often, drops the home clock's second hand and locks "
					   "after 30 seconds. It turns off by itself once the phone has charged to 80%.");
	lv_obj_t *r = ui_row(s, G_BATTERY, pal.yellow, "Low Power Mode");
	bat_lpm = ui_row_switch(r, power_lpm(), lpm_cb, NULL);
	s = ui_section(c, NULL, "Wi-Fi turns off once the screen has been off this long and comes back when you wake the "
				"phone. Saves battery; meanwhile the phone is offline, also on the tailnet. Playing music, "
				"downloads and charging keep Wi-Fi on.");
	r = ui_row(s, G_WIFI, pal.blue, "Wi-Fi Sleep");
	ui_row_switch(r, prefs_int("wifisleep", 0), ws_switch_cb, NULL);
	bat_ws_row = ui_row(s, NULL, 0, "Turn Off After");
	bat_ws_val = ui_row_value(bat_ws_row, "");
	ui_row_chevron(bat_ws_row);
	ui_row_tap(bat_ws_row, ws_open_cb, NULL);
	ws_row_sync();
	s = ui_section(c, "Battery", gauge ? "Current is negative while the phone runs on the battery. "
					   "Maximum Capacity is the full charge now against the design capacity."
					 : "The battery gauge isn't reachable with this kernel, so the level is "
					   "unknown (the status bar draws it full).");
	bat_lvl = value_row(s, "Battery Level", "\xe2\x80\x94");
	bat_state = value_row(s, "State", "");
	bat_draw = value_row(s, "Draw", "\xe2\x80\x94");
	r = ui_row(s, NULL, 0, "Time Left");
	bat_left_sub = ui_row_subtitle(r, "");
	bat_left = ui_row_value(r, "\xe2\x80\x94");
	bat_health = value_row(s, "Maximum Capacity", "\xe2\x80\x94");
	bat_cyc = value_row(s, "Cycle Count", "\xe2\x80\x94");
	bat_v = value_row(s, "Voltage", "\xe2\x80\x94");
	bat_i = value_row(s, "Current", "\xe2\x80\x94");
	bat_t = value_row(s, "Temperature", "\xe2\x80\x94");
	s = ui_section(c, "Since unplugged", "Counted while Orchard runs on the battery; the average needs 20 minutes and "
					     "2% to say anything.");
	use_time = value_row(s, "Time", "\xe2\x80\x94");
	use_on = value_row(s, "Screen On", "\xe2\x80\x94");
	use_off = value_row(s, "Screen Off", "\xe2\x80\x94");
	use_used = value_row(s, "Used", "\xe2\x80\x94");
	s = ui_section(c, "Charging", NULL);
	bat_src = value_row(s, "Power Source", "");
	bat_cur = value_row(s, "Charge Current", "");
	bat_lim = value_row(s, "Input Limit", "");
	bat_tick(NULL);
	bat_timer = lv_timer_create(bat_tick, 2000, NULL);
	nav_on_leave(bat_leave);
}

/* =============================== app =============================== */

void settings_open(const char *page)
{
	bool already = shell_current_app() == &app_settings && root_col;
	pending_page = page;
	pending_at = lv_tick_get();
	shell_launch(&app_settings);	/* closes Control Center / the switcher first */
	if (already) {
		pending_page = NULL;
		if ((strcmp(page, "Bluetooth") || !bt_page) && (strcmp(page, "Wi-Fi") || !wf_page)) {
			int i = pidx(page);
			if (i >= 0 && pdefs[i].fn)
				pdefs[i].fn();
		}
	}
}

/* the lock screen over Settings: its pages' timers (row rebuilds, uptime, battery, spinners) pause — the
 * Bluetooth page drew its spinner 12 times a second under it; unlock = they run at once (the lists rebuild) */
static void lock_changed(bool locked)
{
	lv_timer_t *ts[] = { root_timer, bt_timer, wf_timer, ab_timer, st_timer, bat_timer };
	for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t))
		if (t->timer_cb == spin_tick)
			locked ? lv_timer_pause(t) : lv_timer_resume(t);
	for (size_t i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
		if (!ts[i])
			continue;
		if (locked) {
			lv_timer_pause(ts[i]);
		} else {
			lv_timer_resume(ts[i]);
			lv_timer_ready(ts[i]);
		}
	}
	if (!locked && bt_timer)
		bt_dirty = true;	/* (a rebuild due meanwhile, 3 s after a switch, isn't lost) */
	if (!locked && wf_timer)
		wf_dirty = true;
}

static void app_close(void)
{
	bt_listen(root_bt_changed, false);
	wifi_listen(root_bt_changed, false);
	if (root_timer)
		lv_timer_delete(root_timer);
	root_timer = NULL;
	root_col = main_box = res_box = search_ta = v_wifi = v_bt = v_usb = NULL;
}

const app_t app_settings = {
	.name = "Settings",
	.glyph = G_GEAR,
	.icon = IC_MUTED,
	.open = app_open,
	.close = app_close,
};
