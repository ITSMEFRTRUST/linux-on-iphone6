/* Home screen: wallpaper, widgets (analog clock, month calendar), app grid, Search pill, dock
 * on material, and the Spotlight sheet (apps, arithmetic, "Run in Terminal"). Tap the pill or
 * hold Home to search (a pull down from the top is Control Center, so there is no swipe for it). */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "orchard.h"

#define ICON 120
#define COL_X(c) (54 + (c) * 174)	/* iOS @2x grid: 4 columns of 60 pt icons, 27 pt gaps */
#define ROW_Y(r) (84 + (r) * 176)
#define WIDGET 294	/* small widget = 2 x 2 icons */
#define DOCK_H 176
#define DOCK_Y (SCR_H - 16 - DOCK_H)
#define FIELD_H 76

static lv_obj_t *home, *wall, *clock_face, *cal_face;
static lv_obj_t *sheet, *sheet_ta, *results;
static int cal_mday = -1;

static const app_t *grid_apps[] = { &app_clock, &app_notes, &app_calc, &app_web, &app_weather, &app_camera, &app_memos };
static const app_t *dock_apps[] = { &app_terminal, &app_music, &app_files, &app_settings };
static const app_t *all_apps[] = { &app_terminal, &app_music, &app_files, &app_settings,
				   &app_clock, &app_notes, &app_calc, &app_web, &app_weather, &app_camera, &app_memos };
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static lv_obj_t *grid_icons[N(grid_apps)], *dock_icons[N(dock_apps)];

static int lum(uint32_t c) { return ((c >> 16 & 0xff) * 299 + (c >> 8 & 0xff) * 587 + (c & 0xff) * 114) / 1000; }

/* ---------- icons ----------
 * Drawn icons: assets/icons/gen.py → SVG → raw BGRA at 120 and 64 px (Makefile, assets.S), shown as an
 * lv_image stretched to the size asked (the 64 px one below 70). An app without art falls back to the glyph tile. */

extern const uint8_t icon_clock_120[], icon_clock_64[];
extern const uint8_t icon_notes_120[], icon_notes_64[];
extern const uint8_t icon_calculator_120[], icon_calculator_64[];
extern const uint8_t icon_web_120[], icon_web_64[];
extern const uint8_t icon_weather_120[], icon_weather_64[];
extern const uint8_t icon_camera_120[], icon_camera_64[];
extern const uint8_t icon_voicememos_120[], icon_voicememos_64[];
extern const uint8_t icon_terminal_120[], icon_terminal_64[];
extern const uint8_t icon_music_120[], icon_music_64[];
extern const uint8_t icon_files_120[], icon_files_64[];
extern const uint8_t icon_settings_120[], icon_settings_64[];
static const struct { const app_t *app; const uint8_t *big, *small; } icon_art[] = {
	{ &app_clock, icon_clock_120, icon_clock_64 },
	{ &app_notes, icon_notes_120, icon_notes_64 },
	{ &app_calc, icon_calculator_120, icon_calculator_64 },
	{ &app_web, icon_web_120, icon_web_64 },
	{ &app_weather, icon_weather_120, icon_weather_64 },
	{ &app_camera, icon_camera_120, icon_camera_64 },
	{ &app_memos, icon_voicememos_120, icon_voicememos_64 },
	{ &app_terminal, icon_terminal_120, icon_terminal_64 },
	{ &app_music, icon_music_120, icon_music_64 },
	{ &app_files, icon_files_120, icon_files_64 },
	{ &app_settings, icon_settings_120, icon_settings_64 },
};

static lv_image_dsc_t icon_dsc[sizeof(icon_art) / sizeof(icon_art[0])][2];

static const lv_image_dsc_t *icon_image(const app_t *a, int size)
{
	for (unsigned i = 0; i < sizeof(icon_art) / sizeof(icon_art[0]); i++) {
		if (icon_art[i].app != a)
			continue;
		int k = size < 70, px = k ? 64 : 120;	/* (scaled down, never up) */
		lv_image_dsc_t *d = &icon_dsc[i][k];
		if (!d->data) {
			d->header.magic = LV_IMAGE_HEADER_MAGIC;
			d->header.cf = LV_COLOR_FORMAT_ARGB8888;
			d->header.w = d->header.h = px;
			d->header.stride = px * 4;
			d->data_size = px * px * 4;
			d->data = k ? icon_art[i].small : icon_art[i].big;
		}
		return d;
	}
	return NULL;
}

lv_obj_t *app_icon(lv_obj_t *parent, const app_t *a, int size)
{
	const lv_image_dsc_t *art = icon_image(a, size);
	if (art) {
		lv_obj_t *ic = lv_image_create(parent);
		lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);
		lv_image_set_src(ic, art);
		lv_obj_set_size(ic, size, size);
		lv_image_set_inner_align(ic, LV_IMAGE_ALIGN_STRETCH);
		lv_image_set_antialias(ic, true);
		lv_obj_set_style_image_recolor(ic, C(0x000000), LV_STATE_PRESSED);	/* (pressable: shade the art) */
		lv_obj_set_style_image_recolor_opa(ic, LV_OPA_30, LV_STATE_PRESSED);
		return ic;
	}
	uint32_t top;
	switch (a->icon) {
	case IC_DARK: top = mix(th.dark_bg, 0x000000, 30); break;
	case IC_ACCENT: top = pal.tint; break;
	case IC_WARM: top = pal.orange; break;
	case IC_LIGHT: top = 0xf4f4f6; break;
	case IC_YELLOW: top = pal.yellow; break;
	case IC_BLUE: top = pal.blue; break;
	case IC_GREEN: top = pal.green; break;
	case IC_RED: top = pal.red; break;
	default: top = pal.gray; break;
	}
	uint32_t ink = a->icon == IC_DARK ? pal.green : lum(top) > 165 ? 0x1c1c1e : 0xffffff;
	lv_obj_t *ic = box(parent);
	lv_obj_set_size(ic, size, size);
	lv_obj_set_style_radius(ic, size * 225 / 1000, 0);	/* iOS squircle ≈ 22.5 % */
	lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(ic, C(mix(top, 0xffffff, 10)), 0);
	lv_obj_set_style_bg_grad_color(ic, C(mix(top, 0x000000, a->icon == IC_DARK ? 40 : 18)), 0);
	lv_obj_set_style_bg_grad_dir(ic, LV_GRAD_DIR_VER, 0);
	if (a->icon == IC_DARK) {	/* keep a dark icon visible on a dark dock */
		lv_obj_set_style_border_width(ic, size >= 100 ? 2 : 1, 0);
		lv_obj_set_style_border_color(ic, C(th.muted), 0);
		lv_obj_set_style_border_opa(ic, LV_OPA_40, 0);
	}
	bool term = a == &app_terminal;
	lv_obj_t *g = text(ic, a->glyph, font(F_MONO, size * (term ? 44 : 64) / 120), ink);
	if (term)
		lv_obj_align(g, LV_ALIGN_TOP_LEFT, size / 9, size / 14);
	else {
		ui_glyph_fit(g);
		lv_obj_center(g);
	}
	return ic;
}

static void icon_clicked(lv_event_t *e)
{
	ui_haptic_tap();
	shell_open_app(lv_event_get_user_data(e), lv_event_get_current_target(e));
}

static void pressable(lv_obj_t *o)
{
	lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_color_filter_dsc(o, &lv_color_filter_shade, LV_STATE_PRESSED);
	lv_obj_set_style_color_filter_opa(o, LV_OPA_40, LV_STATE_PRESSED);
}

/* white label with a soft dark copy behind it, readable on bright wallpapers */
static void wall_label(lv_obj_t *parent, const char *s, lv_font_t *f, int x, int y, int w)
{
	for (int k = 0; k < 2; k++) {
		lv_obj_t *l = text(parent, s, f, k ? 0xffffff : 0x000000);
		lv_obj_set_width(l, w);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_pos(l, x, y + (k ? 0 : 2));
		if (!k)
			lv_obj_set_style_text_opa(l, LV_OPA_50, 0);
	}
}

static lv_obj_t *home_app(lv_obj_t *parent, const app_t *a, int x, int y, bool label)
{
	lv_obj_t *ic = app_icon(parent, a, ICON);
	lv_obj_set_pos(ic, x, y);
	pressable(ic);
	lv_obj_add_event_cb(ic, icon_clicked, LV_EVENT_CLICKED, (void *)a);
	if (label)
		wall_label(parent, a->name, font(F_TEXT, 24), x - 25, y + ICON + 10, ICON + 50);
	return ic;
}

lv_obj_t *home_icon_of(const app_t *a)
{
	for (int i = 0; i < N(dock_apps); i++)
		if (dock_apps[i] == a)
			return dock_icons[i];
	for (int i = 0; i < N(grid_apps); i++)
		if (grid_apps[i] == a)
			return grid_icons[i];
	return NULL;
}

/* ---------- widgets ---------- */

static uint32_t widget_ink(void) { return th.dark ? 0xffffff : 0x1c1c1e; }

static void line(lv_layer_t *L, float x1, float y1, float x2, float y2, int w, uint32_t color, lv_opa_t opa)
{
	lv_draw_line_dsc_t d;
	lv_draw_line_dsc_init(&d);
	d.p1.x = x1;
	d.p1.y = y1;
	d.p2.x = x2;
	d.p2.y = y2;
	d.width = w;
	d.color = C(color);
	d.opa = opa;
	d.round_start = d.round_end = 1;
	lv_draw_line(L, &d);
}

static void disc(lv_layer_t *L, int cx, int cy, int r, uint32_t color)
{
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_color = C(color);
	d.radius = LV_RADIUS_CIRCLE;
	lv_area_t a = { cx - r, cy - r, cx + r - 1, cy + r - 1 };
	lv_draw_rect(L, &d, &a);
}

static void label_at(lv_layer_t *L, const char *s, lv_font_t *f, uint32_t color, int cx, int cy)
{
	lv_draw_label_dsc_t d;
	lv_draw_label_dsc_init(&d);
	d.text = s;
	d.text_local = 1;
	d.font = f;
	d.color = C(color);
	d.align = LV_TEXT_ALIGN_CENTER;
	int h = lv_font_get_line_height(f);
	lv_area_t a = { cx - 40, cy - h / 2, cx + 39, cy - h / 2 + h - 1 };
	lv_draw_label(L, &d, &a);
}

/* angle in degrees clockwise from 12 o'clock */
static void hand(lv_layer_t *L, int cx, int cy, float deg, float len, float tail, int w, uint32_t color)
{
	float s = sinf(deg * (float)M_PI / 180), c = cosf(deg * (float)M_PI / 180);
	line(L, cx - s * tail, cy + c * tail, cx + s * len, cy - c * len, w, color, LV_OPA_COVER);
}

static void clock_draw(lv_event_t *e)
{
	lv_layer_t *L = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(clock_face, &a);
	int cx = (a.x1 + a.x2 + 1) / 2, cy = (a.y1 + a.y2 + 1) / 2, R = lv_area_get_width(&a) / 2 - 14;
	uint32_t ink = widget_ink();
	disc(L, cx, cy, R, th.dark ? mix(th.dark_bg, 0x000000, 55) : 0xffffff);
	for (int i = 0; i < 60; i++) {
		if (i % 5 == 0)
			continue;
		float s = sinf(i * 6 * (float)M_PI / 180), c = cosf(i * 6 * (float)M_PI / 180);
		line(L, cx + s * (R - 9), cy - c * (R - 9), cx + s * (R - 15), cy - c * (R - 15), 2, ink, LV_OPA_40);
	}
	for (int h = 1; h <= 12; h++) {
		char n[4];
		snprintf(n, sizeof(n), "%d", h);
		float s = sinf(h * 30 * (float)M_PI / 180), c = cosf(h * 30 * (float)M_PI / 180);
		label_at(L, n, font(F_TEXT_SEMI, 28), ink, cx + s * (R - 36), cy - c * (R - 36));
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	float sec = power_lpm() ? 0 : tm.tm_sec, min = tm.tm_min + sec / 60, hr = tm.tm_hour % 12 + min / 60;
	hand(L, cx, cy, hr * 30, R * 0.5f, 0, 9, ink);
	hand(L, cx, cy, min * 6, R * 0.8f, 0, 7, ink);
	disc(L, cx, cy, 8, ink);
	if (power_lpm())
		return;
	hand(L, cx, cy, sec * 6, R * 0.88f, R * 0.2f, 3, pal.orange);
	disc(L, cx, cy, 6, pal.orange);
	disc(L, cx, cy, 2, th.dark ? 0x000000 : 0xffffff);
}

static void cal_draw(lv_event_t *e)
{
	lv_layer_t *L = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(cal_face, &a);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char buf[32];
	strftime(buf, sizeof(buf), "%B", &tm);
	for (char *p = buf; *p; p++)
		*p = toupper((unsigned char)*p);

	lv_draw_label_dsc_t d;
	lv_draw_label_dsc_init(&d);
	d.text = buf;
	d.text_local = 1;
	d.font = font(F_TEXT_BOLD, 24);
	d.color = C(pal.red);
	lv_area_t t = { a.x1 + 22, a.y1 + 18, a.x2 - 22, a.y1 + 50 };
	lv_draw_label(L, &d, &t);

	int cw = (lv_area_get_width(&a) - 24) / 7, x0 = a.x1 + 12 + cw / 2;
	static const char *wd = "SMTWTFS";
	for (int i = 0; i < 7; i++) {
		char s[2] = { wd[i], 0 };
		label_at(L, s, font(F_TEXT_SEMI, 18), pal.label2, x0 + i * cw, a.y1 + 70);
	}
	struct tm last = tm;	/* day 0 of next month = last day of this one */
	last.tm_mon++;
	last.tm_mday = 0;
	last.tm_isdst = -1;
	mktime(&last);
	int first = ((tm.tm_wday - (tm.tm_mday - 1)) % 7 + 7) % 7;
	for (int day = 1; day <= last.tm_mday; day++) {
		int idx = first + day - 1, x = x0 + idx % 7 * cw, y = a.y1 + 104 + idx / 7 * 32;
		char s[4];
		snprintf(s, sizeof(s), "%d", day);
		if (day == tm.tm_mday) {
			disc(L, x, y, 16, pal.red);
			label_at(L, s, font(F_TEXT_BOLD, 20), 0xffffff, x, y);
		} else {
			label_at(L, s, font(F_TEXT_SEMI, 20), widget_ink(), x, y);
		}
	}
}

static lv_obj_t *widget(int x, int y, const char *name, lv_event_cb_t draw)
{
	lv_obj_t *card = box(home);
	lv_obj_set_pos(card, x, y);
	lv_obj_set_size(card, WIDGET, WIDGET);
	lv_obj_set_style_radius(card, 44, 0);
	ui_material(card);
	lv_obj_t *face = box(card);	/* drawn above the material image */
	lv_obj_set_size(face, WIDGET, WIDGET);
	lv_obj_remove_flag(face, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(face, draw, LV_EVENT_DRAW_MAIN, NULL);
	wall_label(home, name, font(F_TEXT, 24), x, y + WIDGET + 10, WIDGET);
	return face;
}

void home_tick(void)
{
	if (!home || shell_app_open() || lock_visible())
		return;	/* covered: the close animation redraws them fresh */
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	static int drawn_min = -1;
	static bool drawn_lpm;
	if (!power_lpm() || tm.tm_min != drawn_min || !drawn_lpm) {	/* Low Power Mode: no second hand, a redraw a minute */
		drawn_min = tm.tm_min;
		drawn_lpm = power_lpm();
		lv_obj_invalidate(clock_face);
	}
	if (tm.tm_mday != cal_mday) {
		cal_mday = tm.tm_mday;
		lv_obj_invalidate(cal_face);
	}
}

/* ---------- Spotlight ---------- */

/* tiny calculator: + - * x / ^ % ( ) */
static const char *ep;
static bool eok;
static double expr(void);
static void ws(void)
{
	while (*ep == ' ')
		ep++;
}
static double atom(void)
{
	ws();
	if (*ep == '(') {
		ep++;
		double v = expr();
		ws();
		if (*ep == ')')
			ep++;
		else
			eok = false;
		return v;
	}
	if (*ep == '-') {
		ep++;
		return -atom();
	}
	char *end;
	double v = strtod(ep, &end);
	if (end == ep)
		eok = false;
	ep = end;
	return v;
}
static double power(void)
{
	double b = atom();
	ws();
	if (*ep == '^') {
		ep++;
		return pow(b, power());
	}
	return b;
}
static double term(void)
{
	double v = power();
	for (;;) {
		ws();
		char c = *ep;
		if (c == '*' || c == 'x' || c == '/') {
			ep++;
			double r = power();
			v = c == '/' ? v / r : v * r;
		} else if (c == '%') {
			ep++;
			v /= 100;
		} else {
			return v;
		}
	}
}
static double expr(void)
{
	double v = term();
	for (;;) {
		ws();
		if (*ep == '+') {
			ep++;
			v += term();
		} else if (*ep == '-') {
			ep++;
			v -= term();
		} else {
			return v;
		}
	}
}
static bool calc(const char *q, double *out)
{
	bool op = false, digit = false;
	for (const char *p = q; *p; p++) {
		if (isdigit((unsigned char)*p))
			digit = true;
		else if (strchr("+-*/x^%", *p))
			op = true;
		else if (!strchr(" .()", *p))
			return false;
	}
	if (!op || !digit || strstr(q, "0x"))
		return false;
	ep = q;
	eok = true;
	double v = expr();
	ws();
	if (!eok || *ep || !isfinite(v))
		return false;
	*out = v;
	return true;
}

static void search_close(void)
{
	if (!sheet)
		return;
	lv_obj_t *s = sheet;
	sheet = sheet_ta = results = NULL;
	kbd_hide();
	lv_obj_delete(s);
}

void home_reset(void) { search_close(); }

static void result_app(lv_event_t *e)
{
	const app_t *a = lv_event_get_user_data(e);
	kbd_hide();
	shell_open_app(a, lv_event_get_current_target(e));	/* takes the icon's coords now */
	search_close();
}

static void result_term(lv_event_t *e)
{
	(void)e;
	char cmd[256];
	snprintf(cmd, sizeof(cmd), "%s", lv_textarea_get_text(sheet_ta));
	search_close();
	term_run(cmd);
}

static void result_calc(lv_event_t *e)
{
	(void)e;
	search_close();
	shell_launch(&app_calc);
}

static int app_rank(const app_t *a, const char *q)
{
	if (!*q)
		return 1;
	if (!strncasecmp(a->name, q, strlen(q)))
		return 2;
	return strcasestr(a->name, q) ? 1 : 0;
}

static lv_obj_t *card(lv_obj_t *parent)
{
	lv_obj_t *c = box(parent);
	lv_obj_set_size(c, LV_PCT(100), 100);
	lv_obj_set_style_radius(c, 34, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_70, 0);
	lv_obj_set_style_bg_color(c, C(th.dark ? mix(pal.cell, 0x000000, 30) : 0xffffff), 0);
	lv_obj_set_style_bg_color(c, C(pal.cell_hi), LV_STATE_PRESSED);
	lv_obj_set_style_pad_hor(c, 22, 0);
	lv_obj_set_style_pad_column(c, 22, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	return c;
}

static void rebuild(void)
{
	lv_obj_clean(results);
	const char *q = lv_textarea_get_text(sheet_ta);
	while (*q == ' ')
		q++;
	const app_t *hits[N(all_apps)];
	int n = 0;
	for (int rank = 2; rank >= 1; rank--)
		for (int i = 0; i < N(all_apps); i++)
			if (app_rank(all_apps[i], q) == rank && n < 4 && (*q || i < 4))
				hits[n++] = all_apps[i];

	text(results, *q ? "Applications" : "Suggestions", font(F_TEXT_BOLD, 30), pal.label);
	if (n) {
		lv_obj_t *row = box(results);
		lv_obj_set_size(row, LV_PCT(100), ICON + 50);
		for (int i = 0; i < n; i++) {
			lv_obj_t *ic = app_icon(row, hits[i], ICON);
			lv_obj_set_pos(ic, 6 + i * 174, 0);
			pressable(ic);
			lv_obj_add_event_cb(ic, result_app, LV_EVENT_CLICKED, (void *)hits[i]);
			lv_obj_t *l = text(row, hits[i]->name, font(F_TEXT, 24), pal.label);
			lv_obj_set_width(l, ICON + 50);
			lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_set_pos(l, 6 + i * 174 - 25, ICON + 10);
		}
	} else {
		text(results, "No apps", font(F_TEXT, 28), pal.label2);
	}
	if (!*q)
		return;
	double v;
	if (calc(q, &v)) {
		char s[64];
		if (fabs(v) < 1e15 && v == floor(v))
			snprintf(s, sizeof(s), "= %.0f", v);
		else
			snprintf(s, sizeof(s), "= %.10g", v);
		lv_obj_t *c = card(results);
		app_icon(c, &app_calc, 60);
		text(c, s, font(F_DISPLAY_LIGHT, 48), pal.label);
		lv_obj_add_event_cb(c, result_calc, LV_EVENT_CLICKED, NULL);
	}
	lv_obj_t *c = card(results);
	app_icon(c, &app_terminal, 60);
	char s[300];
	snprintf(s, sizeof(s), "Run \xe2\x80\x9c%s\xe2\x80\x9d", q);
	lv_obj_t *l = text(c, s, font(F_TEXT_SEMI, 32), pal.label);
	lv_obj_set_flex_grow(l, 1);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	text(c, "Terminal", font(F_TEXT, 26), pal.label2);
	lv_obj_add_event_cb(c, result_term, LV_EVENT_CLICKED, NULL);
}

static void sheet_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_VALUE_CHANGED) {
		rebuild();
	} else if (code == LV_EVENT_READY) {	/* "done": open the top app hit */
		lv_obj_t *row = lv_obj_get_child(results, 1);
		lv_obj_t *ic = row ? lv_obj_get_child(row, 0) : NULL;
		if (ic && lv_obj_has_flag(ic, LV_OBJ_FLAG_CLICKABLE) && *lv_textarea_get_text(sheet_ta))
			lv_obj_send_event(ic, LV_EVENT_CLICKED, NULL);
	}
}

static void sheet_close_clicked(lv_event_t *e)
{
	(void)e;
	search_close();
}

void search_open(void)
{
	if (sheet || shell_app_open())
		return;
	sheet = box(home);
	lv_obj_set_size(sheet, SCR_W, SCR_H);
	lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
	ui_material(sheet);
	lv_obj_set_style_clip_corner(sheet, false, 0);

	int fy = SCR_H - kbd_height(KBD_TEXT) - FIELD_H - 20;
	results = box(sheet);
	lv_obj_set_pos(results, 0, STATUS_H + 24);
	lv_obj_set_size(results, SCR_W, fy - STATUS_H - 40);
	lv_obj_set_style_pad_hor(results, 32, 0);
	lv_obj_set_style_pad_row(results, 22, 0);
	lv_obj_set_flex_flow(results, LV_FLEX_FLOW_COLUMN);

	sheet_ta = ui_search(sheet, SCR_W - 64 - FIELD_H - 18, "Search");
	lv_obj_set_pos(lv_obj_get_parent(sheet_ta), 32, fy);
	lv_obj_add_event_cb(sheet_ta, sheet_event, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(sheet_ta, sheet_event, LV_EVENT_READY, NULL);
	lv_obj_t *x = ui_icon_button(sheet, G_CLOSE, FIELD_H, pal.fill, pal.label);
	lv_obj_set_pos(x, SCR_W - 32 - FIELD_H, fy);
	lv_obj_add_event_cb(x, sheet_close_clicked, LV_EVENT_CLICKED, NULL);
	rebuild();
	kbd_attach(sheet_ta, KBD_TEXT);
	kbd_no_autocap();	/* app names and shell commands */
}

/* ---------- build ---------- */

static void pill_clicked(lv_event_t *e)
{
	(void)e;
	search_open();
}

void home_wallpaper_changed(void)
{
	lv_image_set_src(wall, theme_wallpaper());
	lv_obj_invalidate(home);
}

void home_create(lv_obj_t *parent)
{
	home = box(parent);
	lv_obj_set_size(home, SCR_W, SCR_H);
	lv_obj_remove_flag(home, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* gestures stop here, not at the screen */
	wall = lv_image_create(home);
	lv_image_set_src(wall, theme_wallpaper());

	clock_face = widget(COL_X(0), ROW_Y(0), "Clock", clock_draw);
	cal_face = widget(COL_X(2), ROW_Y(0), "Calendar", cal_draw);
	for (int i = 0; i < N(grid_apps); i++)
		grid_icons[i] = home_app(home, grid_apps[i], COL_X(i % 4), ROW_Y(2 + i / 4), true);

	lv_obj_t *pill = box(home);
	lv_obj_set_size(pill, 160, 56);
	lv_obj_set_pos(pill, (SCR_W - 160) / 2, DOCK_Y - 28 - 56);
	lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
	ui_material(pill);
	pressable(pill);
	lv_obj_add_event_cb(pill, pill_clicked, LV_EVENT_CLICKED, NULL);
	lv_obj_t *pl = text(pill, G_SEARCH "  Search", font(F_TEXT_SEMI, 24), 0xffffff);
	lv_obj_center(pl);

	lv_obj_t *dock = box(home);
	lv_obj_set_pos(dock, 16, DOCK_Y);
	lv_obj_set_size(dock, SCR_W - 32, DOCK_H);
	lv_obj_set_style_radius(dock, 56, 0);
	ui_material(dock);
	int gap = (SCR_W - 32 - N(dock_apps) * ICON) / (N(dock_apps) + 1);
	for (int i = 0; i < N(dock_apps); i++)
		dock_icons[i] = home_app(dock, dock_apps[i], gap + i * (ICON + gap), (DOCK_H - ICON) / 2, false);
}
