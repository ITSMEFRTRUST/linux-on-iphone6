/* Clock — World Clock, Alarms, Stopwatch, Timers (iOS 26 layout, solid fills, floating tab bar).
 * Alarms and the timer run in clock_init()'s background timer, so they ring with the app closed:
 * shell_alarm() (banner, buzz, wakes the screen, breaks through Do Not Disturb) plus a full-screen
 * alert on the shell overlay — Snooze / Stop for alarms, Repeat / Stop for timers; the Power key
 * snoozes (alarm) or stops (timer) like the side button on iOS. Stopwatch + timer count lv_tick
 * (monotonic); alarms follow the wall clock. World clocks use embedded POSIX TZ rules (musl has no
 * zoneinfo on the phone) evaluated here, so the process TZ is never touched.
 * Data under data_dir(): clock-cities, clock-alarms, clock-timers (recent timers). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "orchard.h"

#define SIDE 32
#define TABBAR_TOP (SCR_H - BOTTOM_PAD - 6 - 124)	/* ui_tabbar() geometry */
#define TAB_IW ((SCR_W - 80 - 16) / 4)		/* ui_tabbar() item width for 4 tabs */
#define LIST_PAD (SCR_H - TABBAR_TOP + 40)	/* scroll clearance under the floating tab bar */
#define WHEEL_ROWS 5
#define WHEEL_GAP 22
#define SNOOZE_MIN 9

#define GM_ALARM_PLUS "\xf3\xb0\x80\xa4"	/* md-alarm_plus (empty state) */

static lv_font_t *f_time(void) { return font(F_DISPLAY_LIGHT, 104); }	/* list times */
static lv_font_t *f_suffix(void) { return font(F_DISPLAY_LIGHT, 48); }	/* AM / PM after a list time */
static lv_font_t *f_digits(void) { return font(F_DISPLAY_LIGHT, 140); }	/* stopwatch, timer ring */
static lv_font_t *f_wheel(void) { return font(F_TEXT, 44); }

/* ================================================================ helpers */

/* wall clock; the snap tool simulates time, so follow its tick there (alarms can be tested) */
static time_t wall(void)
{
#ifdef ORCHARD_SNAP
	static time_t t0;
	static uint32_t k0;
	if (!t0) {
		t0 = time(NULL);
		k0 = lv_tick_get();
	}
	return t0 + lv_tick_elaps(k0) / 1000;
#else
	return time(NULL);
#endif
}

static bool clock24(void) { return prefs_int("clock24", 0) != 0; }

/* "7:30" + "AM" (12 h) or "07:30" + "" (24 h) */
static void fmt_hm(char *buf, int n, int hour, int min, char *ampm)
{
	if (clock24()) {
		snprintf(buf, n, "%02d:%02d", hour, min);
		if (ampm)
			ampm[0] = 0;
		return;
	}
	int h = hour % 12 ? hour % 12 : 12;
	snprintf(buf, n, "%d:%02d", h, min);
	if (ampm)
		snprintf(ampm, 3, "%s", hour < 12 ? "AM" : "PM");
}

static void fmt_hm_full(char *buf, int n, int hour, int min)
{
	char ap[4], t[16];
	fmt_hm(t, sizeof(t), hour, min, ap);
	snprintf(buf, n, ap[0] ? "%s %s" : "%s", t, ap);
}

/* timer / recents: "4:59", "1:04:59" */
static void fmt_dur(char *buf, int n, int secs)
{
	if (secs >= 3600)
		snprintf(buf, n, "%d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60);
	else
		snprintf(buf, n, "%d:%02d", secs / 60, secs % 60);
}

/* "1 hr, 30 min", "5 min", "45 sec" */
static void fmt_dur_words(char *buf, int n, int secs)
{
	int h = secs / 3600, m = secs / 60 % 60, s = secs % 60, k = 0;
	buf[0] = 0;
	if (h)
		k += snprintf(buf + k, n - k, "%d hr", h);
	if (m)
		k += snprintf(buf + k, n - k, "%s%d min", k ? ", " : "", m);
	if (s || !k)
		snprintf(buf + k, n - k, "%s%d sec", k ? ", " : "", s);
}

/* stopwatch: "02:31.07" */
static void fmt_sw(char *buf, int n, uint32_t ms)
{
	uint32_t cs = ms / 10, s = cs / 100, m = s / 60;
	if (m >= 100)
		snprintf(buf, n, "%u:%02u:%02u.%02u", m / 60, m % 60, s % 60, cs % 100);
	else
		snprintf(buf, n, "%02u:%02u.%02u", m, s % 60, cs % 100);
}

static void set_text(lv_obj_t *l, const char *s)
{
	if (l && strcmp(lv_label_get_text(l), s))
		lv_label_set_text(l, s);
}

static void anim_x(void *o, int32_t v) { lv_obj_set_x(o, v); }
static void anim_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, v, 0); }

/* data files: written to name.new, then renamed (power loss never leaves half a file) */
static void save_file(const char *name, void (*write)(FILE *f))
{
	char path[320], tmp[330];
	snprintf(path, sizeof(path), "%s/%s", data_dir(), name);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	write(f);
	fclose(f);
	rename(tmp, path);
}

static FILE *open_file(const char *name)
{
	char path[320];
	snprintf(path, sizeof(path), "%s/%s", data_dir(), name);
	return fopen(path, "r");
}

static void chomp(char *s)
{
	size_t n = strlen(s);
	while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
		s[--n] = 0;
}

/* ---------- tabular numbers: fixed-width digit cells (Inter's digits are proportional, so a
 * running clock in a plain label would jiggle); only the cells that change are invalidated ---------- */

typedef struct tnum { char s[24]; lv_font_t *f; int dw; } tnum_t;

static int tn_cw(const tnum_t *t, char c)
{
	return c >= '0' && c <= '9' ? t->dw : lv_font_get_glyph_width(t->f, (uint8_t)c, 0);
}

static int tn_x0(lv_obj_t *o, const tnum_t *t, const lv_area_t *a)
{
	int w = 0;
	for (const char *s = t->s; *s; s++)
		w += tn_cw(t, *s);
	lv_text_align_t al = lv_obj_get_style_text_align(o, 0);
	if (al == LV_TEXT_ALIGN_RIGHT)
		return a->x2 + 1 - w;
	if (al == LV_TEXT_ALIGN_CENTER)
		return a->x1 + (lv_area_get_width(a) - w) / 2;
	return a->x1;
}

static void tn_event(lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_current_target(e);
	tnum_t *t = lv_obj_get_user_data(o);
	if (lv_event_get_code(e) == LV_EVENT_DELETE) {
		free(t);
		lv_obj_set_user_data(o, NULL);
		return;
	}
	if (!t)
		return;
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_draw_label_dsc_t d;
	lv_draw_label_dsc_init(&d);
	lv_obj_init_draw_label_dsc(o, LV_PART_MAIN, &d);
	lv_area_t a;
	lv_obj_get_content_coords(o, &a);
	int x = tn_x0(o, t, &a);
	for (const char *s = t->s; *s; s++) {
		int cw = tn_cw(t, *s);
		lv_point_t p = { x + (cw - lv_font_get_glyph_width(t->f, (uint8_t)*s, 0)) / 2, a.y1 };
		lv_draw_character(layer, &d, &p, (uint8_t)*s);
		x += cw;
	}
}

static lv_obj_t *tnum(lv_obj_t *parent, lv_font_t *f, uint32_t color, int w, lv_text_align_t align)
{
	lv_obj_t *o = box(parent);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
	tnum_t *t = calloc(1, sizeof(*t));
	t->f = f;
	for (char c = '0'; c <= '9'; c++) {
		int g = lv_font_get_glyph_width(f, (uint8_t)c, 0);
		if (g > t->dw)
			t->dw = g;
	}
	lv_obj_set_user_data(o, t);
	lv_obj_set_size(o, w, lv_font_get_line_height(f));
	lv_obj_set_style_text_font(o, f, 0);
	lv_obj_set_style_text_color(o, C(color), 0);
	lv_obj_set_style_text_align(o, align, 0);
	lv_obj_add_event_cb(o, tn_event, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(o, tn_event, LV_EVENT_DELETE, NULL);
	return o;
}

static void tnum_set(lv_obj_t *o, const char *s)
{
	tnum_t *t = o ? lv_obj_get_user_data(o) : NULL;
	if (!t || !strcmp(t->s, s))
		return;
	size_t n = strlen(s);
	bool same = n == strlen(t->s);
	for (size_t i = 0; same && i < n; i++)
		same = tn_cw(t, s[i]) == tn_cw(t, t->s[i]);
	if (same) {
		lv_area_t a;
		lv_obj_get_content_coords(o, &a);
		int x = tn_x0(o, t, &a);
		for (size_t i = 0; i < n; i++) {
			int cw = tn_cw(t, s[i]);
			if (s[i] != t->s[i]) {
				lv_area_t c = { x - 6, a.y1, x + cw + 6, a.y2 };
				lv_obj_invalidate_area(o, &c);
			}
			x += cw;
		}
	} else {
		lv_obj_invalidate(o);
	}
	snprintf(t->s, sizeof(t->s), "%s", s);
}

/* ---------- small widgets ---------- */

/* plain (non-grouped) list row: full width, hairline under it inset to the text, like the Clock app */
static lv_obj_t *plain_row(lv_obj_t *list, int h)
{
	lv_obj_t *r = box(list);
	lv_obj_set_size(r, SCR_W, h);
	lv_obj_set_style_pad_hor(r, SIDE, 0);
	lv_obj_set_style_pad_column(r, 22, 0);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_t *sep = box(r);
	lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_flag(sep, LV_OBJ_FLAG_FLOATING);
	lv_obj_set_size(sep, SCR_W - SIDE, 1);
	lv_obj_set_pos(sep, 0, h - 1);
	lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(sep, C(pal.separator), 0);
	return r;
}

static void row_pressable(lv_obj_t *r, lv_event_cb_t cb, void *ud)
{
	lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* swipe left = Delete, handled by the row */
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(r, C(pal.cell_hi), LV_STATE_PRESSED);
	if (cb)
		lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, ud);
}

/* list container under a large title: hairline on top, rows below */
static lv_obj_t *plain_list(lv_obj_t *col)
{
	lv_obj_t *l = box(col);
	lv_obj_set_size(l, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
	return l;
}

static void list_top_hairline(lv_obj_t *list)
{
	lv_obj_t *h = box(list);
	lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(h, SCR_W, 1);
	lv_obj_set_style_margin_left(h, SIDE, 0);
	lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(h, C(pal.separator), 0);
}

/* time + small AM/PM sharing a baseline */
static lv_obj_t *time_line(lv_obj_t *parent, lv_obj_t **t, lv_obj_t **ap)
{
	lv_obj_t *l = box(parent);
	lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(l, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(l, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_set_style_pad_column(l, 6, 0);
	*t = text(l, "", f_time(), pal.label);
	*ap = text(l, "", f_suffix(), pal.label);
	lv_obj_set_style_pad_bottom(*ap, f_time()->base_line - f_suffix()->base_line - 2, 0);
	return l;
}

static lv_obj_t *minus_btn(lv_obj_t *row, lv_event_cb_t cb, int idx)
{
	lv_obj_t *m = box(row);
	lv_obj_set_size(m, 46, 46);
	lv_obj_set_style_radius(m, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(m, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(m, C(pal.red), 0);
	lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_ext_click_area(m, 26);
	lv_obj_t *bar = box(m);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(bar, 22, 5);
	lv_obj_set_style_radius(bar, 2, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(bar, C(0xffffff), 0);
	lv_obj_center(bar);
	lv_obj_add_event_cb(m, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
	return m;
}

static lv_obj_t *delete_btn(lv_obj_t *row, lv_event_cb_t cb, int idx)
{
	lv_obj_t *b = box(row);
	lv_obj_set_size(b, 176, 96);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, C(pal.red), 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
	lv_obj_set_style_color_filter_opa(b, LV_OPA_30, LV_STATE_PRESSED);
	lv_obj_center(text(b, "Delete", font(F_TEXT_SEMI, 30), 0xffffff));
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
	lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
	return b;
}

/* Edit / Done on the left of the nav bar (nav_button() only does the right side) */
static lv_obj_t *nav_left(const char *label, lv_event_cb_t cb)
{
	lv_obj_t *b = box(nav_page());
	lv_obj_set_size(b, LV_SIZE_CONTENT, NAV_H);
	lv_obj_set_pos(b, 0, STATUS_H);
	lv_obj_set_style_pad_hor(b, SIDE, 0);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_PRESSED);
	text(b, label, font(F_TEXT, 34), pal.tint);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

static void nav_left_set(lv_obj_t *b, const char *label, bool enabled)
{
	if (!b)
		return;
	lv_obj_t *l = lv_obj_get_child(b, 0);
	set_text(l, label);
	lv_obj_set_style_text_font(l, font(strcmp(label, "Done") ? F_TEXT : F_TEXT_SEMI, 34), 0);
	lv_obj_set_style_opa(b, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
	if (enabled)
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	else
		lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
}

/* big round Clock-app button (Start / Stop / Lap ...) with the thin gap ring around it */
static lv_obj_t *round_btn(lv_obj_t *parent, lv_event_cb_t cb)
{
	lv_obj_t *b = box(parent);
	lv_obj_set_size(b, 164, 164);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_outline_width(b, 4, 0);
	lv_obj_set_style_outline_pad(b, 5, 0);
	lv_obj_set_style_outline_opa(b, LV_OPA_COVER, 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
	lv_obj_set_style_color_filter_opa(b, LV_OPA_30, LV_STATE_PRESSED);
	lv_obj_center(text(b, "", font(F_TEXT, 34), pal.label));
	if (cb)
		lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

enum { RB_GREY, RB_GREEN, RB_RED, RB_ORANGE };

static void round_set(lv_obj_t *b, const char *label, int kind, bool enabled)
{
	if (!b)
		return;
	uint32_t fg = kind == RB_GREEN ? pal.green : kind == RB_RED ? pal.red : kind == RB_ORANGE ? pal.orange : pal.label;
	uint32_t bg = kind == RB_GREY ? pal.fill : mix(fg, pal.bg, th.dark ? 76 : 80);
	if (!enabled)
		fg = pal.label3;
	lv_obj_set_style_bg_color(b, C(bg), 0);
	lv_obj_set_style_outline_color(b, C(bg), 0);
	lv_obj_t *l = lv_obj_get_child(b, 0);
	set_text(l, label);
	lv_obj_set_style_text_color(l, C(fg), 0);
	if (enabled)
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	else
		lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
}

/* ---------- iOS-style wheel picker: shared selection band, rows fade out toward the edges ---------- */

static int wheel_row_h(void) { return lv_font_get_line_height(f_wheel()) + WHEEL_GAP; }

static lv_obj_t *picker_box(lv_obj_t *parent, int w)
{
	int rh = wheel_row_h();
	lv_obj_t *p = box(parent);
	lv_obj_remove_flag(p, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(p, w, rh * WHEEL_ROWS);
	lv_obj_t *band = box(p);
	lv_obj_remove_flag(band, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(band, w, rh);
	lv_obj_set_pos(band, 0, rh * (WHEEL_ROWS / 2));
	lv_obj_set_style_radius(band, 22, 0);
	lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(band, C(pal.fill), 0);
	return p;
}

/* after the wheels: fades on top of them (bg = the page colour behind the picker) */
static void picker_fades(lv_obj_t *p, uint32_t bg)
{
	int rh = wheel_row_h(), h = rh * (WHEEL_ROWS / 2);
	for (int i = 0; i < 2; i++) {
		lv_obj_t *f = box(p);
		lv_obj_remove_flag(f, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(f, LV_PCT(100), h);
		lv_obj_set_pos(f, 0, i ? rh * (WHEEL_ROWS - WHEEL_ROWS / 2) : 0);
		lv_obj_set_style_bg_opa(f, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(f, C(bg), 0);
		lv_obj_set_style_bg_grad_color(f, C(bg), 0);
		lv_obj_set_style_bg_grad_dir(f, LV_GRAD_DIR_VER, 0);
		lv_obj_set_style_bg_main_opa(f, i ? LV_OPA_TRANSP : LV_OPA_80, 0);
		lv_obj_set_style_bg_grad_opa(f, i ? LV_OPA_80 : LV_OPA_TRANSP, 0);
	}
}

static lv_obj_t *wheel(lv_obj_t *p, const char *opts, bool infinite, int sel, int x, int w, lv_text_align_t al)
{
	lv_obj_t *r = lv_roller_create(p);
	lv_obj_remove_style_all(r);
	lv_obj_set_style_text_font(r, f_wheel(), 0);
	lv_obj_set_style_text_color(r, C(pal.label), 0);
	lv_obj_set_style_text_line_space(r, WHEEL_GAP, 0);
	lv_obj_set_style_text_align(r, al, 0);
	lv_obj_set_style_text_font(r, f_wheel(), LV_PART_SELECTED);
	lv_obj_set_style_text_color(r, C(pal.label), LV_PART_SELECTED);
	lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, LV_PART_SELECTED);
	lv_obj_set_style_anim_duration(r, 220, 0);
	lv_roller_set_options(r, opts, infinite ? LV_ROLLER_MODE_INFINITE : LV_ROLLER_MODE_NORMAL);
	lv_roller_set_visible_row_count(r, WHEEL_ROWS);
	lv_obj_set_width(r, w);
	lv_obj_set_pos(r, x, 0);
	lv_obj_set_ext_click_area(r, 10);
	lv_roller_set_selected(r, sel, LV_ANIM_OFF);
	return r;
}

/* "0\n1\n...": numbers from..to, zero-padded to width */
static const char *opts_range(char *buf, int n, int from, int to, int width)
{
	int k = 0;
	buf[0] = 0;
	for (int i = from; i <= to && k < n; i++)
		k += snprintf(buf + k, n - k, i == from ? "%0*d" : "\n%0*d", width, i);
	return buf;
}

/* ================================================================ time zones */

typedef struct tzr {
	int std, dst;	/* seconds east of UTC */
	bool has_dst;
	struct { int m, w, d, secs; } r[2];	/* "Mm.w.d/time": DST start, end (local time) */
} tzr_t;

static const char *tz_name(const char *p)
{
	if (*p == '<') {
		while (*p && *p != '>')
			p++;
		return *p ? p + 1 : p;
	}
	while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
		p++;
	return p;
}

static const char *tz_num(const char *p, int *v)
{
	*v = 0;
	while (*p >= '0' && *p <= '9')
		*v = *v * 10 + (*p++ - '0');
	return p;
}

static const char *tz_hms(const char *p, int *out)	/* [+-]hh[:mm[:ss]] → seconds */
{
	int sign = 1, h, m = 0, s = 0;
	if (*p == '+' || *p == '-')
		sign = *p++ == '-' ? -1 : 1;
	p = tz_num(p, &h);
	if (*p == ':') {
		p = tz_num(p + 1, &m);
		if (*p == ':')
			p = tz_num(p + 1, &s);
	}
	*out = sign * (h * 3600 + m * 60 + s);
	return p;
}

static void tz_parse(const char *s, tzr_t *z)
{
	int off;
	memset(z, 0, sizeof(*z));
	const char *p = tz_hms(tz_name(s), &off);
	z->std = z->dst = -off;	/* POSIX offsets count west */
	if (!*p || *p == ',')
		return;
	p = tz_name(p);
	z->has_dst = true;
	z->dst = z->std + 3600;
	if (*p && *p != ',') {
		p = tz_hms(p, &off);
		z->dst = -off;
	}
	for (int i = 0; i < 2; i++) {
		z->r[i].m = i ? 11 : 3;
		z->r[i].w = i ? 1 : 2;
		z->r[i].secs = 7200;
		if (*p != ',')
			continue;
		p++;
		if (*p == 'M') {
			p = tz_num(p + 1, &z->r[i].m);
			if (*p == '.')
				p = tz_num(p + 1, &z->r[i].w);
			if (*p == '.')
				p = tz_num(p + 1, &z->r[i].d);
		}
		if (*p == '/')
			p = tz_hms(p + 1, &z->r[i].secs);
	}
}

static long days_civil(int y, int m, int d)	/* days since 1970-01-01 (proleptic Gregorian) */
{
	y -= m <= 2;
	long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long)doe - 719468;
}

/* the transition "day d of week w of month m, at secs" in year, as local seconds since the epoch */
static long long tz_rule_at(int year, int m, int w, int d, int secs)
{
	long first = days_civil(year, m, 1);
	int wd1 = (int)(((first % 7) + 7 + 4) % 7);	/* 1970-01-01 was a Thursday */
	int day = 1 + (d - wd1 + 7) % 7 + (w - 1) * 7;
	int mdays = (int)(days_civil(m == 12 ? year + 1 : year, m == 12 ? 1 : m + 1, 1) - first);
	while (day > mdays)
		day -= 7;
	return (long long)(first + day - 1) * 86400 + secs;
}

static int tz_offset(const tzr_t *z, time_t t)
{
	if (!z->has_dst)
		return z->std;
	time_t lt = t + z->std;
	struct tm g;
	gmtime_r(&lt, &g);
	int y = g.tm_year + 1900;
	long long start = tz_rule_at(y, z->r[0].m, z->r[0].w, z->r[0].d, z->r[0].secs) - z->std;
	long long end = tz_rule_at(y, z->r[1].m, z->r[1].w, z->r[1].d, z->r[1].secs) - z->dst;
	bool dst = start < end ? (t >= start && t < end) : !(t >= end && t < start);
	return dst ? z->dst : z->std;
}

/* ================================================================ data */

static const struct city { const char *name, *country, *tz; } cities[] = {
	{ "Amsterdam", "Netherlands", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Anchorage", "U.S.A.", "AKST9AKDT,M3.2.0,M11.1.0" },
	{ "Athens", "Greece", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Auckland", "New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3" },
	{ "Baku", "Azerbaijan", "<+04>-4" },
	{ "Bangkok", "Thailand", "<+07>-7" },
	{ "Barcelona", "Spain", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Beijing", "China", "CST-8" },
	{ "Berlin", "Germany", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Boston", "U.S.A.", "EST5EDT,M3.2.0,M11.1.0" },
	{ "Buenos Aires", "Argentina", "<-03>3" },
	{ "Cairo", "Egypt", "EET-2EEST,M4.5.5/0,M10.5.4/24" },
	{ "Chicago", "U.S.A.", "CST6CDT,M3.2.0,M11.1.0" },
	{ "Cupertino", "U.S.A.", "PST8PDT,M3.2.0,M11.1.0" },
	{ "Delhi", "India", "IST-5:30" },
	{ "Denver", "U.S.A.", "MST7MDT,M3.2.0,M11.1.0" },
	{ "Dubai", "United Arab Emirates", "<+04>-4" },
	{ "Dublin", "Ireland", "GMT0IST,M3.5.0/1,M10.5.0" },
	{ "Hong Kong", "China", "HKT-8" },
	{ "Honolulu", "U.S.A.", "HST10" },
	{ "Istanbul", "T\xc3\xbcrkiye", "<+03>-3" },
	{ "Jakarta", "Indonesia", "WIB-7" },
	{ "Jerusalem", "Israel", "IST-2IDT,M3.4.4/26,M10.5.0" },
	{ "Johannesburg", "South Africa", "SAST-2" },
	{ "Kathmandu", "Nepal", "<+0545>-5:45" },
	{ "Kyiv", "Ukraine", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
	{ "Lagos", "Nigeria", "WAT-1" },
	{ "Lisbon", "Portugal", "WET0WEST,M3.5.0/1,M10.5.0" },
	{ "London", "United Kingdom", "GMT0BST,M3.5.0/1,M10.5.0" },
	{ "Los Angeles", "U.S.A.", "PST8PDT,M3.2.0,M11.1.0" },
	{ "Madrid", "Spain", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Mexico City", "Mexico", "CST6" },
	{ "Miami", "U.S.A.", "EST5EDT,M3.2.0,M11.1.0" },
	{ "Moscow", "Russia", "MSK-3" },
	{ "Mumbai", "India", "IST-5:30" },
	{ "Nairobi", "Kenya", "EAT-3" },
	{ "New York", "U.S.A.", "EST5EDT,M3.2.0,M11.1.0" },
	{ "Paris", "France", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Reykjav\xc3\xadk", "Iceland", "GMT0" },
	{ "Rio de Janeiro", "Brazil", "<-03>3" },
	{ "Rome", "Italy", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "San Francisco", "U.S.A.", "PST8PDT,M3.2.0,M11.1.0" },
	{ "S\xc3\xa3o Paulo", "Brazil", "<-03>3" },
	{ "Seoul", "South Korea", "KST-9" },
	{ "Shanghai", "China", "CST-8" },
	{ "Singapore", "Singapore", "<+08>-8" },
	{ "Stockholm", "Sweden", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Sydney", "Australia", "AEST-10AEDT,M10.1.0,M4.1.0/3" },
	{ "Tehran", "Iran", "<+0330>-3:30" },
	{ "Tokyo", "Japan", "JST-9" },
	{ "Toronto", "Canada", "EST5EDT,M3.2.0,M11.1.0" },
	{ "UTC", "Coordinated Universal Time", "UTC0" },
	{ "Vancouver", "Canada", "PST8PDT,M3.2.0,M11.1.0" },
	{ "Vienna", "Austria", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Warsaw", "Poland", "CET-1CEST,M3.5.0,M10.5.0/3" },
	{ "Washington, D.C.", "U.S.A.", "EST5EDT,M3.2.0,M11.1.0" },
	{ "Yerevan", "Armenia", "<+04>-4" },
};
#define NCITIES ((int)(sizeof(cities) / sizeof(cities[0])))
#define MAX_MINE 24
static int mine[MAX_MINE], n_mine;

static void cities_write(FILE *f)
{
	for (int i = 0; i < n_mine; i++)
		fprintf(f, "%s\n", cities[mine[i]].name);
}

static bool city_added(int c)
{
	for (int i = 0; i < n_mine; i++)
		if (mine[i] == c)
			return true;
	return false;
}

static void cities_load(void)
{
	FILE *f = open_file("clock-cities");
	if (!f) {
		static const char *def[] = { "London", "New York", "Tokyo", "Los Angeles" };
		for (unsigned k = 0; k < sizeof(def) / sizeof(def[0]); k++)
			for (int i = 0; i < NCITIES; i++)
				if (!strcmp(cities[i].name, def[k]))
					mine[n_mine++] = i;
		return;
	}
	char line[96];
	while (n_mine < MAX_MINE && fgets(line, sizeof(line), f)) {
		chomp(line);
		for (int i = 0; i < NCITIES; i++)
			if (!strcmp(cities[i].name, line) && !city_added(i))
				mine[n_mine++] = i;
	}
	fclose(f);
}

/* ---------- alarms ---------- */

#define MAX_ALARMS 24
typedef struct alarm {
	int id, hour, min;
	bool on, snooze;
	uint8_t days;		/* bit 0 = Sunday ... bit 6 = Saturday; 0 = rings once */
	char label[40];
	time_t next;		/* next ring while on, 0 = none */
	time_t snoozed;		/* snoozed: rings again then */
} alarm_t;
static alarm_t alarms[MAX_ALARMS];
static int n_alarms, next_id = 1;
static time_t last_check;

static void alarms_write(FILE *f)
{
	for (int i = 0; i < n_alarms; i++) {
		alarm_t *a = &alarms[i];
		fprintf(f, "%02d:%02d %d %d %d %s\n", a->hour, a->min, a->on, a->days, a->snooze, a->label);
	}
}

static int alarm_cmp(const void *x, const void *y)
{
	const alarm_t *a = x, *b = y;
	return (a->hour * 60 + a->min) - (b->hour * 60 + b->min);
}

/* next time this alarm rings strictly after `after` (local time, honours the repeat days) */
static time_t alarm_next(const alarm_t *a, time_t after)
{
	struct tm now;
	localtime_r(&after, &now);
	for (int d = 0; d <= 7; d++) {
		struct tm c = now;
		c.tm_mday += d;
		c.tm_hour = a->hour;
		c.tm_min = a->min;
		c.tm_sec = 0;
		c.tm_isdst = -1;
		time_t t = mktime(&c);
		if (t <= after)
			continue;
		if (a->days && !(a->days & 1 << c.tm_wday))
			continue;
		return t;
	}
	return 0;
}

static void alarms_reschedule(time_t from)
{
	for (int i = 0; i < n_alarms; i++)
		alarms[i].next = alarms[i].on ? alarm_next(&alarms[i], from) : 0;
}

static void alarms_changed(void)
{
	qsort(alarms, n_alarms, sizeof(alarm_t), alarm_cmp);
	alarms_reschedule(wall());
	save_file("clock-alarms", alarms_write);
}

static alarm_t *alarm_by_id(int id)
{
	for (int i = 0; i < n_alarms; i++)
		if (alarms[i].id == id)
			return &alarms[i];
	return NULL;
}

static void alarms_load(void)
{
	FILE *f = open_file("clock-alarms");
	char line[128];
	while (f && n_alarms < MAX_ALARMS && fgets(line, sizeof(line), f)) {
		int h, m, on, days, sn, pos = 0;
		chomp(line);
		if (sscanf(line, "%d:%d %d %d %d %n", &h, &m, &on, &days, &sn, &pos) < 5 || h < 0 || h > 23 || m < 0 || m > 59)
			continue;
		alarm_t *a = &alarms[n_alarms++];
		memset(a, 0, sizeof(*a));
		a->id = next_id++;
		a->hour = h;
		a->min = m;
		a->on = on;
		a->days = days & 0x7f;
		a->snooze = sn;
		snprintf(a->label, sizeof(a->label), "%s", pos ? line + pos : "");
	}
	if (f)
		fclose(f);
	qsort(alarms, n_alarms, sizeof(alarm_t), alarm_cmp);
}

static const char *day_short[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *day_long[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

/* editor value ("Never", "Weekdays", "Mon Wed") or list caption suffix ("every day", "weekdays") */
static void repeat_text(char *buf, int n, uint8_t days, bool caption)
{
	if (!days)
		snprintf(buf, n, "%s", caption ? "" : "Never");
	else if (days == 0x7f)
		snprintf(buf, n, "%s", caption ? "every day" : "Every Day");
	else if (days == 0x3e)
		snprintf(buf, n, "%s", caption ? "weekdays" : "Weekdays");
	else if (days == 0x41)
		snprintf(buf, n, "%s", caption ? "weekends" : "Weekends");
	else {
		int k = 0, cnt = 0, one = 0;
		for (int d = 0; d < 7; d++)
			if (days & 1 << d)
				cnt++, one = d;
		if (cnt == 1) {
			snprintf(buf, n, "every %s", day_long[one]);
			if (!caption)
				buf[0] = 'E';
			return;
		}
		buf[0] = 0;
		for (int i = 1; i <= 7; i++) {	/* Monday first, like iOS */
			int d = i % 7;
			if (days & 1 << d)
				k += snprintf(buf + k, n - k, "%s%s", k ? " " : "", day_short[d]);
		}
	}
}

/* ---------- timer + recents ---------- */

enum { T_IDLE, T_RUN, T_PAUSE };
static struct {
	int state;
	int total;	/* ms */
	uint32_t end;	/* running: lv_tick when it ends */
	int left;	/* paused: ms left */
	char label[40];
} tmr;
static int pick[3] = { 0, 5, 0 };	/* the picker remembers the last duration */

#define MAX_RECENT 6
static struct recent { int secs; char label[40]; } recents[MAX_RECENT];
static int n_recent;

static int tmr_left(void)
{
	if (tmr.state == T_RUN)
		return (int32_t)(tmr.end - lv_tick_get());
	return tmr.state == T_PAUSE ? tmr.left : 0;
}

static void recents_write(FILE *f)
{
	for (int i = 0; i < n_recent; i++)
		fprintf(f, "%d %s\n", recents[i].secs, recents[i].label);
}

static void recents_load(void)
{
	FILE *f = open_file("clock-timers");
	char line[96];
	while (f && n_recent < MAX_RECENT && fgets(line, sizeof(line), f)) {
		int secs, pos = 0;
		chomp(line);
		if (sscanf(line, "%d %n", &secs, &pos) < 1 || secs <= 0 || secs >= 24 * 3600)
			continue;
		recents[n_recent].secs = secs;
		snprintf(recents[n_recent].label, sizeof(recents[0].label), "%s", pos ? line + pos : "");
		n_recent++;
	}
	if (f)
		fclose(f);
}

static void recents_add(int secs, const char *label)
{
	int i;
	for (i = 0; i < n_recent; i++)
		if (recents[i].secs == secs && !strcmp(recents[i].label, label))
			break;
	if (i == n_recent && n_recent < MAX_RECENT)
		n_recent++;
	if (i == n_recent)
		i = n_recent - 1;
	memmove(&recents[1], &recents[0], i * sizeof(recents[0]));
	recents[0].secs = secs;
	snprintf(recents[0].label, sizeof(recents[0].label), "%s", label);
	save_file("clock-timers", recents_write);
}

/* ---------- stopwatch ---------- */

#define MAX_LAPS 999
#define LAP_ROWS 60	/* rows shown (newest first) */
static struct {
	bool run;
	uint32_t start, acc;	/* acc = ms before the current run */
	uint32_t laps[MAX_LAPS];	/* split times (cumulative) */
	int n;
} sw;

static uint32_t sw_elapsed(void) { return sw.acc + (sw.run ? lv_tick_elaps(sw.start) : 0); }

/* ================================================================ app UI state */

static lv_obj_t *root, *host, *tabbar;
static lv_timer_t *ui_tmr;
static int tab = 1;	/* Alarms; remembered while Orchard runs */
static int pill_from = -1;
static void build_tab(void);
static void ui_period(void);
static void label_page(char *buf, int n, lv_obj_t *value, const char *placeholder);
static bool tm_dirty;
static void tm_build_async(void *ud);

static void tab_async(void *ud)
{
	(void)ud;
	if (root)
		build_tab();
}

static void tab_changed(lv_event_t *e)
{
	int t = ui_tabbar_get(lv_event_get_target(e));
	if (t == tab)
		return;
	pill_from = tab * TAB_IW;
	tab = t;
	lv_async_call(tab_async, NULL);	/* the tab bar is rebuilt with the page: not inside its own event */
}

static void tabbar_attach(lv_obj_t *page)
{
	static const char *glyphs[] = { G_GLOBE, G_ALARM, G_STOPWATCH, G_HOURGLASS };
	static const char *labels[] = { "World Clock", "Alarms", "Stopwatch", "Timers" };
	tabbar = ui_tabbar(page, glyphs, labels, 4, tab);
	lv_obj_add_event_cb(tabbar, tab_changed, LV_EVENT_VALUE_CHANGED, NULL);
	if (pill_from >= 0) {	/* carry on the pill slide the old bar started */
		motion_anim(lv_obj_get_child(tabbar, 0), anim_x, pill_from, tab * TAB_IW, MOTION_SMALL + 40, NULL);
		pill_from = -1;
	}
}

/* root page of a list tab: plain background, room to scroll past the tab bar */
static void tab_page(lv_obj_t *col)
{
	lv_obj_set_style_bg_color(nav_page(), C(pal.bg), 0);
	lv_obj_set_style_pad_bottom(col, LIST_PAD, 0);
}

static void empty_note(lv_obj_t *parent, const char *glyph, const char *msg)
{
	lv_obj_t *b = box(parent);
	lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(b, SCR_W, 620);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(b, 24, 0);
	if (glyph)
		ui_icon_line(b, glyph, font(F_MONO, 104), pal.label3);	/* the ink centred, the column as before */
	text(b, msg, font(F_TEXT_SEMI, 36), pal.label2);
}

/* ================================================================ World Clock */

static lv_obj_t *wc_list, *wc_edit, *wc_search, *wc_results, *wc_none;
static bool wc_editing;
static int wc_reveal = -1;
static long wc_stamp = -1;
static struct wc_row { lv_obj_t *row, *cap, *time, *ampm, *right, *del; } wcr[MAX_MINE];
static lv_obj_t *wc_pick_rows[NCITIES];
static void wc_fill(void);

static void wc_update(bool force)
{
	time_t now = wall();
	if (!wc_list || (!force && now / 60 == wc_stamp))
		return;
	wc_stamp = now / 60;
	struct tm lt;
	localtime_r(&now, &lt);
	long today = days_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
	for (int i = 0; i < n_mine; i++) {
		if (!wcr[i].row)
			continue;
		tzr_t z;
		tz_parse(cities[mine[i]].tz, &z);
		int off = tz_offset(&z, now);
		time_t ct = now + off;
		struct tm c;
		gmtime_r(&ct, &c);
		char t[16], ap[4], cap[48], dh[16];
		fmt_hm(t, sizeof(t), c.tm_hour, c.tm_min, ap);
		set_text(wcr[i].time, t);
		set_text(wcr[i].ampm, ap);
		long dd = days_civil(c.tm_year + 1900, c.tm_mon + 1, c.tm_mday) - today;
		int diff = off - (int)lt.tm_gmtoff, ad = diff < 0 ? -diff : diff;
		if (ad % 3600)
			snprintf(dh, sizeof(dh), "%c%d:%02dHRS", diff < 0 ? '-' : '+', ad / 3600, ad % 3600 / 60);
		else
			snprintf(dh, sizeof(dh), "%c%dHR%s", diff < 0 ? '-' : '+', ad / 3600, ad == 3600 ? "" : "S");
		snprintf(cap, sizeof(cap), "%s, %s", dd > 0 ? "Tomorrow" : dd < 0 ? "Yesterday" : "Today", dh);
		set_text(wcr[i].cap, cap);
	}
}

static void wc_show_reveal(int i)
{
	wc_reveal = i;
	for (int k = 0; k < n_mine; k++) {
		if (!wcr[k].row)
			continue;
		bool on = k == i;
		if (on)
			lv_obj_remove_flag(wcr[k].del, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(wcr[k].del, LV_OBJ_FLAG_HIDDEN);
		if (on || wc_editing)
			lv_obj_add_flag(wcr[k].right, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_remove_flag(wcr[k].right, LV_OBJ_FLAG_HIDDEN);
	}
}

static void wc_fill_async(void *ud)
{
	(void)ud;
	if (wc_list)
		wc_fill();
}

static void wc_delete_cb(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= n_mine)
		return;
	memmove(&mine[i], &mine[i + 1], (n_mine - i - 1) * sizeof(mine[0]));
	n_mine--;
	save_file("clock-cities", cities_write);
	wc_reveal = -1;
	if (!n_mine)
		wc_editing = false;
	ui_haptic_tap();
	lv_async_call(wc_fill_async, NULL);	/* the button is in the row being deleted */
}

static void wc_minus_cb(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	wc_show_reveal(wc_reveal == i ? -1 : i);
}

static void wc_row_cb(lv_event_t *e)
{
	if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE)
		return;	/* a swipe, not a tap */
	if (wc_reveal >= 0)
		wc_show_reveal(-1);
}

static void wc_row_gesture(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
	if (d == LV_DIR_LEFT && wc_reveal != i)
		wc_show_reveal(i);
	else if (d == LV_DIR_RIGHT && wc_reveal >= 0)
		wc_show_reveal(-1);
}

static void wc_fill(void)
{
	lv_obj_clean(wc_list);
	memset(wcr, 0, sizeof(wcr));
	nav_left_set(wc_edit, wc_editing ? "Done" : "Edit", n_mine > 0);
	if (!n_mine) {
		empty_note(wc_list, G_GLOBE, "No World Clocks");
		return;
	}
	list_top_hairline(wc_list);
	for (int i = 0; i < n_mine; i++) {
		struct wc_row *w = &wcr[i];
		w->row = plain_row(wc_list, 176);
		row_pressable(w->row, wc_row_cb, NULL);
		lv_obj_set_style_bg_opa(w->row, LV_OPA_TRANSP, LV_STATE_PRESSED);	/* nothing opens on tap */
		lv_obj_add_event_cb(w->row, wc_row_gesture, LV_EVENT_GESTURE, (void *)(intptr_t)i);
		if (wc_editing)
			minus_btn(w->row, wc_minus_cb, i);
		lv_obj_t *col = box(w->row);
		lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_grow(col, 1);
		lv_obj_set_height(col, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(col, 2, 0);
		w->cap = text(col, "", font(F_TEXT, 28), pal.label2);
		lv_obj_t *name = text(col, cities[mine[i]].name, font(F_TEXT, 40), pal.label);
		lv_obj_set_width(name, LV_PCT(100));
		lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
		w->right = time_line(w->row, &w->time, &w->ampm);
		w->del = delete_btn(w->row, wc_delete_cb, i);
	}
	wc_show_reveal(wc_reveal < n_mine ? wc_reveal : -1);
	wc_update(true);
}

static void wc_edit_cb(lv_event_t *e)
{
	(void)e;
	wc_editing = !wc_editing && n_mine > 0;
	wc_reveal = -1;
	wc_fill();
}

/* ---------- Choose a City ---------- */

/* lower-case ASCII fold of a UTF-8 name (ã → a, í → i, ü → u ...) for search */
static void fold(char *out, int n, const char *s)
{
	int k = 0;
	while (*s && k < n - 1) {
		unsigned char c = *s++;
		if (c == 0xc3 && *s) {
			unsigned char d = (unsigned char)*s++ | 0x20;	/* 0x80-0x9f upper → 0xa0-0xbf lower */
			c = d >= 0xa0 && d <= 0xa5 ? 'a' : d == 0xa7 ? 'c' : d >= 0xa8 && d <= 0xab ? 'e' :
			    d >= 0xac && d <= 0xaf ? 'i' : d == 0xb1 ? 'n' : d >= 0xb2 && d <= 0xb6 ? 'o' :
			    d >= 0xb9 && d <= 0xbc ? 'u' : '?';
		} else if (c >= 'A' && c <= 'Z') {
			c += 32;
		}
		out[k++] = c;
	}
	out[k] = 0;
}

static void wc_filter(lv_event_t *e)
{
	(void)e;
	if (!wc_search)
		return;
	char q[64], name[160];
	fold(q, sizeof(q), lv_textarea_get_text(wc_search));
	int shown = 0;
	for (int i = 0; i < NCITIES; i++) {
		char full[160];
		snprintf(full, sizeof(full), "%s, %s", cities[i].name, cities[i].country);
		fold(name, sizeof(name), full);
		bool hit = !q[0] || strstr(name, q);
		if (hit) {
			lv_obj_remove_flag(wc_pick_rows[i], LV_OBJ_FLAG_HIDDEN);
			shown++;
		} else {
			lv_obj_add_flag(wc_pick_rows[i], LV_OBJ_FLAG_HIDDEN);
		}
	}
	if (shown)
		lv_obj_add_flag(wc_none, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_remove_flag(wc_none, LV_OBJ_FLAG_HIDDEN);
}

static void wc_pick_cb(lv_event_t *e)
{
	int c = (intptr_t)lv_event_get_user_data(e);
	if (!city_added(c) && n_mine < MAX_MINE) {
		mine[n_mine++] = c;
		save_file("clock-cities", cities_write);
		wc_editing = false;
		wc_reveal = -1;
		if (wc_list)
			wc_fill();	/* the list lives on the root page, not under this row */
		ui_haptic_tap();
	}
	kbd_hide();
	nav_pop();
}

static void wc_kbd(lv_event_t *e)
{
	lv_obj_t *page = lv_event_get_user_data(e);
	bool up = lv_event_get_code(e) == EV_KBD_SHOWN;
	lv_obj_set_style_pad_bottom(page, up ? kbd_height(KBD_TEXT) + 40 : 140, 0);
}

static void wc_search_click(lv_event_t *e)
{
	(void)e;
	kbd_no_autocap();	/* runs after ui.c's own click handler attached the keyboard */
}

static void wc_pick_leave(void)
{
	kbd_hide();
	wc_search = wc_results = wc_none = NULL;
	memset(wc_pick_rows, 0, sizeof(wc_pick_rows));
}

static void wc_add_cb(lv_event_t *e)
{
	(void)e;
	lv_obj_t *c = nav_push("Add City");
	if (!c)
		return;
	nav_on_leave(wc_pick_leave);
	lv_obj_t *gap = box(c);
	lv_obj_set_size(gap, 1, 12);
	wc_search = ui_search(c, SCR_W - 2 * SIDE, "Search");
	lv_obj_add_event_cb(wc_search, wc_filter, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(wc_search, wc_kbd, EV_KBD_SHOWN, c);
	lv_obj_add_event_cb(wc_search, wc_kbd, EV_KBD_HIDDEN, c);
	lv_obj_add_event_cb(wc_search, wc_search_click, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(lv_obj_get_parent(wc_search), wc_search_click, LV_EVENT_CLICKED, NULL);
	gap = box(c);
	lv_obj_set_size(gap, 1, 20);
	wc_results = plain_list(c);
	list_top_hairline(wc_results);
	for (int i = 0; i < NCITIES; i++) {
		lv_obj_t *r = plain_row(wc_results, 100);
		lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(r, C(pal.cell_hi), LV_STATE_PRESSED);
		lv_obj_add_event_cb(r, wc_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		char full[160];
		snprintf(full, sizeof(full), "%s, %s", cities[i].name, cities[i].country);
		lv_obj_t *l = text(r, full, font(F_TEXT, 34), city_added(i) ? pal.label2 : pal.label);
		lv_obj_set_flex_grow(l, 1);
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
		if (city_added(i))
			ui_icon(r, G_CHECK, font(F_MONO, 32), pal.tint);
		wc_pick_rows[i] = r;
	}
	wc_none = text(c, "No Results", font(F_TEXT_SEMI, 36), pal.label2);
	lv_obj_set_style_pad_top(wc_none, 200, 0);
	lv_obj_add_flag(wc_none, LV_OBJ_FLAG_HIDDEN);
	kbd_attach(wc_search, KBD_TEXT);	/* iOS opens this sheet with the keyboard up */
	kbd_no_autocap();
}

static void wc_build(void)
{
	lv_obj_t *c = nav_root(host, "World Clock");
	tab_page(c);
	wc_edit = nav_left("Edit", wc_edit_cb);
	nav_button(G_PLUS, wc_add_cb, NULL);
	wc_list = plain_list(c);
	wc_fill();
	tabbar_attach(nav_page());
}

/* ================================================================ Alarms */

static lv_obj_t *al_list, *al_edit;
static bool al_editing;
static int al_reveal = -1;
static struct al_row { lv_obj_t *row, *time, *ampm, *cap, *sw, *chev, *del; } alr[MAX_ALARMS];
static void al_fill(void);
static void al_editor(int idx);

static void al_style_row(int i)
{
	struct al_row *r = &alr[i];
	if (!r->row)
		return;
	uint32_t c = alarms[i].on ? pal.label : pal.label2;
	lv_obj_set_style_text_color(r->time, C(c), 0);
	lv_obj_set_style_text_color(r->ampm, C(c), 0);
	lv_obj_set_style_text_color(r->cap, C(c), 0);
}

static void al_show_reveal(int i)
{
	al_reveal = i;
	for (int k = 0; k < n_alarms; k++) {
		struct al_row *r = &alr[k];
		if (!r->row)
			continue;
		bool on = k == i;
		if (on)
			lv_obj_remove_flag(r->del, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(r->del, LV_OBJ_FLAG_HIDDEN);
		if (r->sw) {
			if (on || al_editing)
				lv_obj_add_flag(r->sw, LV_OBJ_FLAG_HIDDEN);
			else
				lv_obj_remove_flag(r->sw, LV_OBJ_FLAG_HIDDEN);
		}
		if (r->chev) {
			if (on)
				lv_obj_add_flag(r->chev, LV_OBJ_FLAG_HIDDEN);
			else
				lv_obj_remove_flag(r->chev, LV_OBJ_FLAG_HIDDEN);
		}
	}
}

static void al_fill_async(void *ud)
{
	(void)ud;
	if (al_list)
		al_fill();
}

static void al_remove(int i)
{
	if (i < 0 || i >= n_alarms)
		return;
	memmove(&alarms[i], &alarms[i + 1], (n_alarms - i - 1) * sizeof(alarm_t));
	n_alarms--;
	alarms_changed();
	al_reveal = -1;
	if (!n_alarms)
		al_editing = false;
}

static void al_delete_cb(lv_event_t *e)
{
	al_remove((intptr_t)lv_event_get_user_data(e));
	ui_haptic_tap();
	lv_async_call(al_fill_async, NULL);	/* the button is in the row being deleted */
}

static void al_minus_cb(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	al_show_reveal(al_reveal == i ? -1 : i);
}

static void al_switch_cb(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= n_alarms)
		return;
	alarms[i].on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
	alarms[i].snoozed = 0;
	alarms_reschedule(wall());
	save_file("clock-alarms", alarms_write);
	al_style_row(i);
}

static void al_row_cb(lv_event_t *e)
{
	if (lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE)
		return;
	if (al_reveal >= 0) {
		al_show_reveal(-1);
		return;
	}
	al_editor((intptr_t)lv_event_get_user_data(e));
}

static void al_row_gesture(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
	if (d == LV_DIR_LEFT && al_reveal != i)
		al_show_reveal(i);
	else if (d == LV_DIR_RIGHT && al_reveal >= 0)
		al_show_reveal(-1);
}

static void al_caption(char *buf, int n, const alarm_t *a)
{
	char rep[48];
	repeat_text(rep, sizeof(rep), a->days, true);
	snprintf(buf, n, rep[0] ? "%s, %s" : "%s", a->label[0] ? a->label : "Alarm", rep);
}

static void al_fill(void)
{
	lv_obj_clean(al_list);
	memset(alr, 0, sizeof(alr));
	nav_left_set(al_edit, al_editing ? "Done" : "Edit", n_alarms > 0);
	if (!n_alarms) {
		empty_note(al_list, GM_ALARM_PLUS, "No Alarms");
		return;
	}
	list_top_hairline(al_list);
	for (int i = 0; i < n_alarms; i++) {
		alarm_t *a = &alarms[i];
		struct al_row *r = &alr[i];
		r->row = plain_row(al_list, 186);
		row_pressable(r->row, al_row_cb, (void *)(intptr_t)i);
		lv_obj_add_event_cb(r->row, al_row_gesture, LV_EVENT_GESTURE, (void *)(intptr_t)i);
		if (al_editing)
			minus_btn(r->row, al_minus_cb, i);
		lv_obj_t *col = box(r->row);
		lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_flex_grow(col, 1);
		lv_obj_set_height(col, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_style_pad_row(col, 0, 0);
		time_line(col, &r->time, &r->ampm);
		char t[16], ap[4], cap[96];
		fmt_hm(t, sizeof(t), a->hour, a->min, ap);
		lv_label_set_text(r->time, t);
		lv_label_set_text(r->ampm, ap);
		al_caption(cap, sizeof(cap), a);
		r->cap = text(col, cap, font(F_TEXT, 30), pal.label);
		lv_obj_set_width(r->cap, LV_PCT(100));
		lv_label_set_long_mode(r->cap, LV_LABEL_LONG_MODE_DOTS);
		if (al_editing) {
			r->chev = ui_icon(r->row, G_CHEVRON_R, font(F_MONO, 28), pal.label3);
		} else {
			r->sw = ui_switch(r->row, a->on);
			lv_obj_add_event_cb(r->sw, al_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
		}
		r->del = delete_btn(r->row, al_delete_cb, i);
		al_style_row(i);
	}
	al_show_reveal(al_reveal < n_alarms ? al_reveal : -1);
}

static void al_edit_cb(lv_event_t *e)
{
	(void)e;
	al_editing = !al_editing && n_alarms > 0;
	al_reveal = -1;
	al_fill();
}

/* ---------- Add / Edit Alarm ---------- */

static alarm_t ed;
static bool ed_new;
static lv_obj_t *ed_h, *ed_m, *ed_ap, *ed_rep, *ed_lbl;
static uint8_t rep_days;
static lv_obj_t *rep_checks[7];

static void ed_leave(void) { ed_h = ed_m = ed_ap = ed_rep = ed_lbl = NULL; }

static void ed_read_wheels(void)
{
	if (!ed_h)
		return;
	int h = lv_roller_get_selected(ed_h), m = lv_roller_get_selected(ed_m);
	if (ed_ap)
		h = (h + 1) % 12 + (lv_roller_get_selected(ed_ap) ? 12 : 0);
	ed.hour = h;
	ed.min = m;
}

static void ed_save_cb(lv_event_t *e)
{
	(void)e;
	ed_read_wheels();
	ed.on = true;
	ed.snoozed = 0;
	alarm_t *a = ed_new ? NULL : alarm_by_id(ed.id);
	if (!a && n_alarms < MAX_ALARMS) {
		a = &alarms[n_alarms++];
		ed.id = next_id++;
	}
	if (a)
		*a = ed;
	alarms_changed();
	al_editing = false;
	al_reveal = -1;
	if (al_list)
		al_fill();
	ui_haptic_tap();
	nav_pop();
}

static void ed_delete_cb(lv_event_t *e)
{
	(void)e;
	alarm_t *a = alarm_by_id(ed.id);
	if (a)
		al_remove((int)(a - alarms));
	if (al_list)
		al_fill();
	nav_pop();
}

static void ed_snooze_cb(lv_event_t *e) { ed.snooze = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED); }

static void rep_toggle(lv_event_t *e)
{
	int d = (intptr_t)lv_event_get_user_data(e);
	rep_days ^= 1 << d;
	if (rep_checks[d])
		lv_obj_set_style_opa(rep_checks[d], rep_days & 1 << d ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

static void rep_leave(void)
{
	ed.days = rep_days;
	char buf[48];
	repeat_text(buf, sizeof(buf), ed.days, false);
	set_text(ed_rep, buf);
	memset(rep_checks, 0, sizeof(rep_checks));
}

static void ed_repeat_cb(lv_event_t *e)
{
	(void)e;
	lv_obj_t *c = nav_push("Repeat");
	if (!c)
		return;
	rep_days = ed.days;
	nav_on_leave(rep_leave);
	lv_obj_t *s = ui_section(c, NULL, NULL);
	for (int d = 0; d < 7; d++) {
		char t[32];
		snprintf(t, sizeof(t), "Every %s", day_long[d]);
		lv_obj_t *r = ui_row(s, NULL, 0, t);
		rep_checks[d] = ui_row_check(r, ed.days & 1 << d);
		ui_row_tap(r, rep_toggle, (void *)(intptr_t)d);
	}
}

static void ed_label_cb(lv_event_t *e)
{
	(void)e;
	label_page(ed.label, sizeof(ed.label), ed_lbl, "Alarm");
}

static void al_editor(int idx)
{
	ed_new = idx < 0 || idx >= n_alarms;
	if (ed_new) {
		time_t now = wall();
		struct tm tm;
		localtime_r(&now, &tm);
		memset(&ed, 0, sizeof(ed));
		ed.hour = tm.tm_hour;
		ed.min = tm.tm_min;
		ed.snooze = true;
	} else {
		ed = alarms[idx];
	}
	lv_obj_t *c = nav_push(ed_new ? "Add Alarm" : "Edit Alarm");
	if (!c)
		return;
	nav_on_leave(ed_leave);
	nav_button("Save", ed_save_cb, NULL);

	static char o12[64], o24[96], o60[200];
	int w = SCR_W - 2 * SIDE;
	lv_obj_t *gap = box(c);
	lv_obj_set_size(gap, 1, 20);
	lv_obj_t *p = picker_box(c, w);
	if (clock24()) {
		int x = (w - 2 * 170) / 2;
		ed_h = wheel(p, opts_range(o24, sizeof(o24), 0, 23, 2), true, ed.hour, x, 150, LV_TEXT_ALIGN_CENTER);
		ed_m = wheel(p, opts_range(o60, sizeof(o60), 0, 59, 2), true, ed.min, x + 170, 150, LV_TEXT_ALIGN_CENTER);
		ed_ap = NULL;
	} else {
		int x = (w - 3 * 150 - 2 * 16) / 2;
		ed_h = wheel(p, opts_range(o12, sizeof(o12), 1, 12, 1), true, (ed.hour + 11) % 12, x, 150, LV_TEXT_ALIGN_CENTER);
		ed_m = wheel(p, opts_range(o60, sizeof(o60), 0, 59, 2), true, ed.min, x + 166, 150, LV_TEXT_ALIGN_CENTER);
		ed_ap = wheel(p, "AM\nPM", false, ed.hour >= 12, x + 332, 150, LV_TEXT_ALIGN_CENTER);
	}
	picker_fades(p, pal.grouped_bg);

	char buf[48];
	lv_obj_t *s = ui_section(c, NULL, NULL);
	lv_obj_t *r = ui_row(s, NULL, 0, "Repeat");
	repeat_text(buf, sizeof(buf), ed.days, false);
	ed_rep = ui_row_value(r, buf);
	ui_row_chevron(r);
	ui_row_tap(r, ed_repeat_cb, NULL);
	r = ui_row(s, NULL, 0, "Label");
	ed_lbl = ui_row_value(r, ed.label[0] ? ed.label : "Alarm");
	ui_row_chevron(r);
	ui_row_tap(r, ed_label_cb, NULL);
	r = ui_row(s, NULL, 0, "Snooze");
	ui_row_switch(r, ed.snooze, ed_snooze_cb, NULL);

	if (!ed_new) {
		s = ui_section(c, NULL, NULL);
		r = ui_row(s, NULL, 0, "Delete Alarm");
		lv_obj_t *l = lv_obj_get_child(lv_obj_get_user_data(r), 0);
		lv_obj_set_style_text_color(l, C(pal.red), 0);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		ui_row_tap(r, ed_delete_cb, NULL);
	}
}

static void al_add_cb(lv_event_t *e)
{
	(void)e;
	if (n_alarms >= MAX_ALARMS) {
		ui_alert("Too Many Alarms", "Delete an alarm before adding a new one.", (const char *[]){ "OK" }, 1, NULL, NULL);
		return;
	}
	al_editor(-1);
}

static void al_build(void)
{
	lv_obj_t *c = nav_root(host, "Alarms");
	tab_page(c);
	al_edit = nav_left("Edit", al_edit_cb);
	nav_button(G_PLUS, al_add_cb, NULL);
	al_list = plain_list(c);
	al_fill();
	tabbar_attach(nav_page());
}

/* ---------- Label page (alarms + timer) ---------- */

static char *lp_buf;
static int lp_n;
static lv_obj_t *lp_ta, *lp_val;
static const char *lp_def;

static void lp_leave(void)
{
	if (lp_ta && lp_buf) {
		snprintf(lp_buf, lp_n, "%s", lv_textarea_get_text(lp_ta));
		chomp(lp_buf);
		while (lp_buf[0] == ' ')
			memmove(lp_buf, lp_buf + 1, strlen(lp_buf));
		set_text(lp_val, lp_buf[0] ? lp_buf : lp_def);
	}
	kbd_hide();
	lp_ta = lp_val = NULL;
	lp_buf = NULL;
	if (tm_dirty) {
		tm_dirty = false;
		lv_async_call(tm_build_async, NULL);
	}
}

static void lp_ready(lv_event_t *e)
{
	(void)e;
	nav_pop();
}

static void label_page(char *buf, int n, lv_obj_t *value, const char *placeholder)
{
	lv_obj_t *c = nav_push("Label");
	if (!c)
		return;
	lp_buf = buf;
	lp_n = n;
	lp_val = value;
	lp_def = placeholder;
	nav_on_leave(lp_leave);
	lv_obj_t *gap = box(c);
	lv_obj_set_size(gap, 1, 36);
	lp_ta = ui_text_field(c, SCR_W - 2 * SIDE, placeholder);
	lv_textarea_set_max_length(lp_ta, n - 8);
	lv_textarea_set_text(lp_ta, buf);
	lv_obj_add_event_cb(lp_ta, lp_ready, LV_EVENT_READY, NULL);
	kbd_attach(lp_ta, KBD_TEXT);
}

/* ================================================================ Stopwatch */

static lv_obj_t *sw_big, *sw_left, *sw_right, *sw_list, *sw_live;

static void sw_fill(void)
{
	if (!sw_list)
		return;
	lv_obj_clean(sw_list);
	sw_live = NULL;
	uint32_t e = sw_elapsed();
	if (!sw.run && !e)
		return;
	list_top_hairline(sw_list);
	int best = -1, worst = -1;
	if (sw.n >= 2) {
		for (int i = 0; i < sw.n; i++) {
			uint32_t d = sw.laps[i] - (i ? sw.laps[i - 1] : 0);
			if (best < 0 || d < sw.laps[best] - (best ? sw.laps[best - 1] : 0))
				best = i;
			if (worst < 0 || d > sw.laps[worst] - (worst ? sw.laps[worst - 1] : 0))
				worst = i;
		}
	}
	char t[32], name[16];
	for (int i = sw.n; i >= 0 && i > sw.n - LAP_ROWS; i--) {
		if (i == sw.n && sw.n >= MAX_LAPS)
			continue;
		uint32_t d = i == sw.n ? e - (sw.n ? sw.laps[sw.n - 1] : 0) : sw.laps[i] - (i ? sw.laps[i - 1] : 0);
		uint32_t c = i == best ? pal.green : i == worst ? pal.red : pal.label;
		lv_obj_t *r = plain_row(sw_list, 92);
		lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
		snprintf(name, sizeof(name), "Lap %d", i + 1);
		lv_obj_t *l = text(r, name, font(F_TEXT, 34), c);
		lv_obj_set_flex_grow(l, 1);
		lv_obj_t *tn = tnum(r, font(F_TEXT, 34), c, 260, LV_TEXT_ALIGN_RIGHT);
		fmt_sw(t, sizeof(t), d);
		tnum_set(tn, t);
		if (i == sw.n)
			sw_live = tn;
	}
}

static void sw_buttons(void)
{
	uint32_t e = sw_elapsed();
	if (sw.run)
		round_set(sw_left, "Lap", RB_GREY, sw.n < MAX_LAPS);
	else
		round_set(sw_left, e ? "Reset" : "Lap", RB_GREY, e > 0);
	round_set(sw_right, sw.run ? "Stop" : "Start", sw.run ? RB_RED : RB_GREEN, true);
}

static void sw_update(void)
{
	if (!sw_big)
		return;
	char t[32];
	uint32_t e = sw_elapsed();
	fmt_sw(t, sizeof(t), e);
	tnum_set(sw_big, t);
	if (sw_live) {
		fmt_sw(t, sizeof(t), e - (sw.n ? sw.laps[sw.n - 1] : 0));
		tnum_set(sw_live, t);
	}
}

static void sw_left_cb(lv_event_t *e)
{
	(void)e;
	if (sw.run) {
		if (sw.n < MAX_LAPS)
			sw.laps[sw.n++] = sw_elapsed();
	} else {
		sw.acc = 0;
		sw.n = 0;
	}
	ui_haptic_tap();
	sw_fill();
	sw_buttons();
	sw_update();
}

static void sw_right_cb(lv_event_t *e)
{
	(void)e;
	bool fresh = !sw.run && !sw_elapsed();
	if (sw.run) {
		sw.acc = sw_elapsed();
		sw.run = false;
	} else {
		sw.start = lv_tick_get();
		sw.run = true;
	}
	ui_haptic_tap();
	if (fresh || !sw_live)
		sw_fill();
	sw_buttons();
	sw_update();
	ui_period();
}

static void sw_build(void)
{
	nav_reset();	/* no navigation stack on this tab: no back swipe */
	lv_obj_t *pg = box(host);
	lv_obj_set_size(pg, SCR_W, SCR_H);
	lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(pg, C(pal.bg), 0);
	lv_obj_remove_flag(pg, LV_OBJ_FLAG_CLICKABLE);

	sw_big = tnum(pg, f_digits(), pal.label, SCR_W, LV_TEXT_ALIGN_CENTER);
	lv_obj_set_pos(sw_big, 0, 250);

	sw_left = round_btn(pg, sw_left_cb);
	lv_obj_set_pos(sw_left, 44, 520);
	sw_right = round_btn(pg, sw_right_cb);
	lv_obj_set_pos(sw_right, SCR_W - 44 - 164, 520);

	sw_list = box(pg);
	lv_obj_set_pos(sw_list, 0, 740);
	lv_obj_set_size(sw_list, SCR_W, SCR_H - 740);
	lv_obj_add_flag(sw_list, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(sw_list, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(sw_list, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(sw_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_bottom(sw_list, SCR_H - TABBAR_TOP + 20, 0);

	sw_fill();
	sw_buttons();
	sw_update();
	tabbar_attach(pg);
}

/* ================================================================ Timers */

static lv_obj_t *tm_wh[3], *tm_unit[3], *tm_arc, *tm_left, *tm_end, *tm_cancel, *tm_go, *tm_lbl;
static void timer_start(int secs, const char *label);

static int tm_picked(void) { return pick[0] * 3600 + pick[1] * 60 + pick[2]; }

static void tm_units(void)
{
	static const char *one[] = { "hour", "min", "sec" }, *many[] = { "hours", "min", "sec" };
	for (int i = 0; i < 3; i++)
		set_text(tm_unit[i], pick[i] == 1 ? one[i] : many[i]);
}

static void tm_buttons(void)
{
	if (tmr.state == T_IDLE) {
		round_set(tm_cancel, "Cancel", RB_GREY, false);
		round_set(tm_go, "Start", RB_GREEN, tm_picked() > 0);
	} else {
		round_set(tm_cancel, "Cancel", RB_GREY, true);
		round_set(tm_go, tmr.state == T_RUN ? "Pause" : "Resume", tmr.state == T_RUN ? RB_ORANGE : RB_GREEN, true);
	}
}

static void tm_end_text(void)
{
	if (!tm_end)
		return;
	time_t at = wall() + (tmr_left() + 999) / 1000;
	struct tm tm;
	localtime_r(&at, &tm);
	char t[24], buf[48];
	fmt_hm_full(t, sizeof(t), tm.tm_hour, tm.tm_min);
	snprintf(buf, sizeof(buf), G_BELL "  %s", t);
	set_text(tm_end, buf);
	lv_obj_set_style_text_color(tm_end, C(tmr.state == T_RUN ? pal.label2 : pal.label3), 0);
}

static void tm_update(void)
{
	if (!tm_arc || tmr.state == T_IDLE)
		return;
	int left = tmr_left();
	if (left < 0)
		left = 0;
	float frac = tmr.total > 0 ? (float)left / tmr.total : 0;
	if (frac > 0.0005f) {
		lv_obj_remove_flag(tm_arc, LV_OBJ_FLAG_HIDDEN);
		lv_arc_set_angles(tm_arc, 0, 360 * frac);
	} else {
		lv_arc_set_angles(tm_arc, 0, 0);
	}
	char t[24];
	fmt_dur(t, sizeof(t), (left + 999) / 1000);
	tnum_set(tm_left, t);
}

static void tm_wheel_cb(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	pick[i] = lv_roller_get_selected(lv_event_get_target(e));
	tm_units();
	tm_buttons();
}

static void tm_cancel_cb(lv_event_t *e)
{
	(void)e;
	tmr.state = T_IDLE;
	ui_haptic_tap();
	lv_async_call(tm_build_async, NULL);
}

static void tm_go_cb(lv_event_t *e)
{
	(void)e;
	if (tmr.state == T_IDLE) {
		if (tm_picked() <= 0)
			return;
		timer_start(tm_picked(), tmr.label);
		lv_async_call(tm_build_async, NULL);
		return;
	}
	if (tmr.state == T_RUN) {
		tmr.left = tmr_left();
		tmr.state = T_PAUSE;
	} else {
		tmr.end = lv_tick_get() + tmr.left;
		tmr.state = T_RUN;
	}
	ui_haptic_tap();
	tm_buttons();
	tm_end_text();
	ui_period();
}

static void tm_recent_cb(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= n_recent)
		return;
	struct recent r = recents[i];
	snprintf(tmr.label, sizeof(tmr.label), "%s", r.label);
	timer_start(r.secs, r.label);
	lv_async_call(tm_build_async, NULL);
}

static void tm_label_cb(lv_event_t *e)
{
	(void)e;
	label_page(tmr.label, sizeof(tmr.label), tm_lbl, "Timer");
}

static void tm_build(void)
{
	lv_obj_t *c = nav_root(host, "Timers");
	tab_page(c);
	int w = SCR_W - 2 * SIDE;
	lv_obj_t *top = box(c);
	lv_obj_remove_flag(top, LV_OBJ_FLAG_CLICKABLE);
	if (tmr.state == T_IDLE) {
		static char oh[80], om[200];
		lv_obj_set_size(top, w, wheel_row_h() * WHEEL_ROWS + 30);
		lv_obj_t *p = picker_box(top, w);
		lv_obj_set_pos(p, 0, 20);
		int cw = w / 3, rh = wheel_row_h();
		for (int i = 0; i < 3; i++) {
			const char *o = i ? opts_range(om, sizeof(om), 0, 59, 1) : opts_range(oh, sizeof(oh), 0, 23, 1);
			tm_wh[i] = wheel(p, o, true, pick[i], i * cw + 6, 104, LV_TEXT_ALIGN_RIGHT);
			lv_obj_add_event_cb(tm_wh[i], tm_wheel_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)i);
		}
		picker_fades(p, pal.bg);
		for (int i = 0; i < 3; i++) {	/* unit labels sit in the band, above the fades */
			tm_unit[i] = text(p, "", font(F_TEXT_SEMI, 30), pal.label);
			lv_obj_remove_flag(tm_unit[i], LV_OBJ_FLAG_CLICKABLE);
			lv_obj_set_pos(tm_unit[i], i * cw + 124, rh * (WHEEL_ROWS / 2) + (rh - lv_font_get_line_height(font(F_TEXT_SEMI, 30))) / 2);
		}
		tm_units();
	} else {
		int d = 560;
		lv_obj_set_size(top, w, d + 60);
		lv_obj_t *ring = box(top);
		lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(ring, d, d);
		lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
		tm_arc = lv_arc_create(ring);
		lv_obj_remove_style_all(tm_arc);
		lv_obj_remove_flag(tm_arc, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(tm_arc, d, d);
		lv_arc_set_rotation(tm_arc, 270);
		lv_arc_set_bg_angles(tm_arc, 0, 360);
		lv_obj_set_style_arc_width(tm_arc, 16, 0);
		lv_obj_set_style_arc_color(tm_arc, C(pal.fill), 0);
		lv_obj_set_style_arc_width(tm_arc, 16, LV_PART_INDICATOR);
		lv_obj_set_style_arc_color(tm_arc, C(pal.orange), LV_PART_INDICATOR);
		lv_obj_set_style_arc_rounded(tm_arc, true, LV_PART_INDICATOR);
		lv_obj_set_style_bg_opa(tm_arc, LV_OPA_TRANSP, LV_PART_KNOB);
		lv_obj_set_style_pad_all(tm_arc, 0, LV_PART_KNOB);
		tm_left = tnum(ring, f_digits(), pal.label, d - 60, LV_TEXT_ALIGN_CENTER);
		lv_obj_align(tm_left, LV_ALIGN_CENTER, 0, -18);
		tm_end = text(ring, "", font(F_TEXT, 30), pal.label2);
		lv_obj_align(tm_end, LV_ALIGN_CENTER, 0, 92);
		tm_end_text();
		tm_update();
	}

	lv_obj_t *btns = box(c);
	lv_obj_remove_flag(btns, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(btns, SCR_W, 200);
	tm_cancel = round_btn(btns, tm_cancel_cb);
	lv_obj_set_pos(tm_cancel, 44, 18);
	tm_go = round_btn(btns, tm_go_cb);
	lv_obj_set_pos(tm_go, SCR_W - 44 - 164, 18);
	tm_buttons();

	lv_obj_t *s = ui_section(c, NULL, NULL);
	lv_obj_t *r = ui_row(s, NULL, 0, "Label");
	tm_lbl = ui_row_value(r, tmr.label[0] ? tmr.label : "Timer");
	ui_row_chevron(r);
	ui_row_tap(r, tm_label_cb, NULL);

	if (n_recent) {
		lv_obj_t *h = text(c, "Recents", font(F_TEXT_BOLD, 34), pal.label);
		lv_obj_set_width(h, SCR_W - 2 * SIDE);
		lv_obj_set_style_pad_top(h, 48, 0);
		lv_obj_set_style_pad_bottom(h, 14, 0);
		lv_obj_t *list = plain_list(c);
		list_top_hairline(list);
		for (int i = 0; i < n_recent; i++) {
			lv_obj_t *row = plain_row(list, 170);
			row_pressable(row, tm_recent_cb, (void *)(intptr_t)i);
			lv_obj_add_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
			lv_obj_t *col = box(row);
			lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_set_flex_grow(col, 1);
			lv_obj_set_height(col, LV_SIZE_CONTENT);
			lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
			char t[24], words[48];
			fmt_dur(t, sizeof(t), recents[i].secs);
			text(col, t, f_time(), pal.label);
			fmt_dur_words(words, sizeof(words), recents[i].secs);
			lv_obj_t *cap = text(col, recents[i].label[0] ? recents[i].label : words, font(F_TEXT, 30), pal.label2);
			lv_obj_set_width(cap, LV_PCT(100));
			lv_label_set_long_mode(cap, LV_LABEL_LONG_MODE_DOTS);
			lv_obj_t *play = ui_icon_button(row, G_PLAY, 84, mix(pal.green, pal.bg, th.dark ? 76 : 80), pal.green);
			lv_obj_remove_flag(play, LV_OBJ_FLAG_CLICKABLE);	/* the whole row starts it */
			lv_obj_set_style_translate_x(lv_obj_get_child(play, 0), 2, 0);	/* the triangle's optical centre */
		}
	}
	tabbar_attach(nav_page());
}

static void tm_build_async(void *ud)
{
	(void)ud;
	if (!root || tab != 3)
		return;
	if (nav_depth() > 0) {	/* the Label page is open: rebuild when it closes */
		tm_dirty = true;
		return;
	}
	build_tab();
}

/* ================================================================ ringing (alarm / timer done) */

enum { RG_ALARM, RG_TIMER };
static struct { lv_obj_t *obj, *time; lv_timer_t *buzz; int kind, id, pulses; bool snooze; } rg;
#define RING_MAX_PULSES 300	/* ~10 minutes of buzzing, then it gives up */

static void ring_gone(lv_anim_t *a) { lv_obj_delete(a->var); }

static void ring_close(void)
{
	if (rg.buzz)
		lv_timer_delete(rg.buzz);
	rg.buzz = NULL;
	if (rg.obj) {
		lv_obj_t *o = rg.obj;
		rg.obj = rg.time = NULL;
		lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
		for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++)
			lv_obj_remove_flag(lv_obj_get_child(o, i), LV_OBJ_FLAG_CLICKABLE);
		motion_anim(o, anim_opa, LV_OPA_COVER, LV_OPA_TRANSP, MOTION_SMALL, ring_gone);
	}
}

static void ring_snooze(void)
{
	alarm_t *a = rg.kind == RG_ALARM ? alarm_by_id(rg.id) : NULL;
	if (a)
		a->snoozed = wall() + SNOOZE_MIN * 60;
	ring_close();
}

static void ring_stop_cb(lv_event_t *e)
{
	(void)e;
	ui_haptic_tap();
	ring_close();
}

static void ring_alt_cb(lv_event_t *e)
{
	(void)e;
	ui_haptic_tap();
	if (rg.kind == RG_ALARM) {
		ring_snooze();
		return;
	}
	int secs = tmr.total / 1000;
	ring_close();
	timer_start(secs, tmr.label);	/* Repeat */
	if (root && tab == 3)
		lv_async_call(tm_build_async, NULL);
}

static void ring_buzz(lv_timer_t *t)
{
	lv_timer_set_period(t, 2000);
	hal_haptic(450);
	lv_display_trigger_activity(NULL);	/* keep auto-lock away while it rings */
	if (++rg.pulses >= RING_MAX_PULSES)
		ring_close();
}

static void ring_clock(void)
{
	if (!rg.time)
		return;
	time_t now = wall();
	struct tm tm;
	localtime_r(&now, &tm);
	char buf[24];
	fmt_hm(buf, sizeof(buf), tm.tm_hour, tm.tm_min, NULL);
	set_text(rg.time, buf);
}

static lv_obj_t *ring_btn(lv_obj_t *parent, const char *label, uint32_t bg, uint32_t fg, int y, lv_event_cb_t cb)
{
	lv_obj_t *b = box(parent);
	lv_obj_set_size(b, SCR_W - 2 * 64, 128);
	lv_obj_align(b, LV_ALIGN_TOP_MID, 0, y);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, C(bg), 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
	lv_obj_set_style_color_filter_opa(b, LV_OPA_30, LV_STATE_PRESSED);
	lv_obj_center(text(b, label, font(F_TEXT_SEMI, 36), fg));
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

static void ring_show(int kind, int id, const char *label, bool snooze)
{
	if (rg.obj || rg.buzz) {	/* a second one while the first still rings: show the newer */
		if (rg.buzz)
			lv_timer_delete(rg.buzz);
		rg.buzz = NULL;
		if (rg.obj)
			lv_obj_delete(rg.obj);
		rg.obj = NULL;
	}
	kbd_hide();	/* input.c hands touches on the keyboard area to the keyboard, even under the overlay */
	rg.kind = kind;
	rg.id = id;
	rg.snooze = snooze;
	rg.pulses = 0;
	lv_obj_t *o = rg.obj = box(shell_overlay());
	lv_obj_set_size(o, SCR_W, SCR_H);
	lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);	/* swallow taps behind it */
	lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(o, C(pal.bg), 0);
	ui_material(o);

	lv_obj_t *h = text(o, kind == RG_ALARM ? G_ALARM "  Alarm" : G_HOURGLASS "  Timer", font(F_TEXT_SEMI, 36), 0xffffff);
	lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 200);
	rg.time = text(o, "", font(F_DISPLAY_BOLD, 200), 0xffffff);
	lv_obj_align(rg.time, LV_ALIGN_TOP_MID, 0, 250);
	ring_clock();
	lv_obj_t *l = text(o, label, font(F_TEXT_SEMI, 36), 0xffffff);
	lv_obj_set_width(l, SCR_W - 120);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_style_opa(l, LV_OPA_80, 0);
	lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 500);

	int y = SCR_H - BOTTOM_PAD - 60 - 128;
	if (kind == RG_TIMER || snooze) {
		ring_btn(o, kind == RG_ALARM ? "Snooze" : "Repeat", pal.orange, 0xffffff, y - 128 - 32, ring_alt_cb);
	}
	ring_btn(o, "Stop", mix(pal.cell, th.fg, 8), pal.label, y, ring_stop_cb);

	motion_anim(o, anim_opa, LV_OPA_TRANSP, LV_OPA_COVER, MOTION_SMALL, NULL);
	rg.buzz = lv_timer_create(ring_buzz, 4000, NULL);	/* shell_alarm() buzzes the first seconds itself */
}

/* ================================================================ background (app open or not) */

static void al_fill_later(void)
{
	if (al_list)
		lv_async_call(al_fill_async, NULL);
}

static void alarm_fire(alarm_t *a, time_t now)
{
	const char *body = a->label[0] ? a->label : "Alarm";
	shell_alarm(&app_clock, "Alarm", body);
	ring_show(RG_ALARM, a->id, body, a->snooze);
	if (!a->days)
		a->on = false;	/* one-time alarms switch themselves off */
	a->next = a->on ? alarm_next(a, now) : 0;
	save_file("clock-alarms", alarms_write);
	al_fill_later();
}

static void timer_start(int secs, const char *label)
{
	tmr.total = secs * 1000;
	tmr.end = lv_tick_get() + tmr.total;
	tmr.state = T_RUN;
	if (label != tmr.label)
		snprintf(tmr.label, sizeof(tmr.label), "%s", label);
	pick[0] = secs / 3600;
	pick[1] = secs / 60 % 60;
	pick[2] = secs % 60;
	recents_add(secs, tmr.label);
	ui_haptic_tap();
	ui_period();
}

static void timer_done(void)
{
	tmr.state = T_IDLE;
	char words[48];
	fmt_dur_words(words, sizeof(words), tmr.total / 1000);
	const char *body = tmr.label[0] ? tmr.label : words;
	shell_alarm(&app_clock, "Timer Done", body);
	ring_show(RG_TIMER, 0, body, false);
	if (root && tab == 3)
		lv_async_call(tm_build_async, NULL);
	ui_period();
}

/* screen off (power.c): no need to look 4 times a second — sleep until the next alarm / snooze / timer end
 * (at most 30 s; the last second at the usual 250 ms) */
static uint32_t dark_period(time_t now)
{
	long s = 30;
	if (tmr.state == T_RUN)
		s = LV_MIN(s, tmr_left() / 1000);
	for (int i = 0; i < n_alarms; i++) {
		if (alarms[i].snoozed)
			s = LV_MIN(s, (long)(alarms[i].snoozed - now));
		if (alarms[i].on && alarms[i].next)
			s = LV_MIN(s, (long)(alarms[i].next - now));
	}
	return s <= 1 ? 250 : (uint32_t)(s - 1) * 1000;
}

static void bg_tick(lv_timer_t *t)
{
	static time_t last_sec;
	if (power_dark())
		lv_timer_set_period(t, dark_period(wall()));	/* power.c puts the old period back on wake */
	else	/* a countdown ends to the quarter second; alarms are whole seconds */
		lv_timer_set_period(t, tmr.state == T_RUN ? 250 : 1000);
	if (tmr.state == T_RUN && tmr_left() <= 0)
		timer_done();
	time_t now = wall();
	if (now == last_sec)
		return;
	last_sec = now;
	if (rg.obj && shell_screen_off()) {	/* Power while it rings: snooze (alarm) / stop (timer) */
		if (rg.kind == RG_ALARM && rg.snooze)
			ring_snooze();
		else
			ring_close();
	}
	if (!last_check || now < last_check || now - last_check > 90)
		alarms_reschedule(now - 1);	/* first run or the clock jumped: no catching up */
	last_check = now;
	for (int i = 0; i < n_alarms; i++) {
		alarm_t *a = &alarms[i];
		if (a->snoozed && a->snoozed <= now) {
			a->snoozed = 0;
			alarm_fire(a, now);
		} else if (a->on && a->next && a->next <= now) {
			alarm_fire(a, now);
		}
	}
	if (now % 60 == 0)
		ring_clock();
}

void clock_init(void)
{
	cities_load();
	alarms_load();
	recents_load();
	alarms_reschedule(wall());
	last_check = wall();
	power_keep(lv_timer_create(bg_tick, 250, NULL), 1000, "clock");	/* alarms + timers ring with the screen off */
}

const char *clock_next_alarm(void)
{
	static char buf[32];
	time_t best = 0;
	for (int i = 0; i < n_alarms; i++) {
		time_t t[2] = { alarms[i].snoozed, alarms[i].on ? alarms[i].next : 0 };
		for (int k = 0; k < 2; k++)
			if (t[k] && (!best || t[k] < best))
				best = t[k];
	}
	if (!best)
		return NULL;
	struct tm tm;
	localtime_r(&best, &tm);
	char t[24];
	fmt_hm_full(t, sizeof(t), tm.tm_hour, tm.tm_min);
	if (best - wall() >= 24 * 3600 - 60)
		snprintf(buf, sizeof(buf), "%s %s", day_short[tm.tm_wday], t);
	else
		snprintf(buf, sizeof(buf), "%s", t);
	return buf;
}

bool clock_timer_running(int *secs_left)
{
	if (tmr.state == T_IDLE)
		return false;
	int ms = tmr_left();
	if (secs_left)
		*secs_left = ms > 0 ? (ms + 999) / 1000 : 0;
	return true;
}

/* ================================================================ app */

static void clear_tab_ptrs(void)
{
	wc_list = wc_edit = wc_search = wc_results = wc_none = NULL;
	memset(wcr, 0, sizeof(wcr));
	memset(wc_pick_rows, 0, sizeof(wc_pick_rows));
	al_list = al_edit = NULL;
	memset(alr, 0, sizeof(alr));
	ed_h = ed_m = ed_ap = ed_rep = ed_lbl = NULL;
	memset(rep_checks, 0, sizeof(rep_checks));
	lp_ta = lp_val = NULL;
	lp_buf = NULL;
	sw_big = sw_left = sw_right = sw_list = sw_live = NULL;
	memset(tm_wh, 0, sizeof(tm_wh));
	memset(tm_unit, 0, sizeof(tm_unit));
	tm_arc = tm_left = tm_end = tm_cancel = tm_go = tm_lbl = NULL;
	tabbar = NULL;
}

static void build_tab(void)
{
	if (tabbar && pill_from >= 0)	/* the old bar's pill is mid-slide: continue from where it is */
		pill_from = lv_obj_get_x(lv_obj_get_child(tabbar, 0));
	kbd_hide();
	nav_reset();	/* leave callbacks run while their objects still exist */
	clear_tab_ptrs();
	if (host)
		lv_obj_delete(host);
	host = box(root);
	lv_obj_set_size(host, SCR_W, SCR_H);
	lv_obj_remove_flag(host, LV_OBJ_FLAG_CLICKABLE);
	wc_stamp = -1;
	switch (tab) {
	case 0: wc_build(); break;
	case 1: al_build(); break;
	case 2: sw_build(); break;
	default: tm_build(); break;
	}
	ui_period();
}

static void ui_period(void)
{
	if (ui_tmr)
		lv_timer_set_period(ui_tmr, tab == 2 && sw.run ? 33 : tab == 3 && tmr.state == T_RUN ? 100 : 500);
}

static void ui_tick(lv_timer_t *t)
{
	(void)t;
	switch (tab) {
	case 0: wc_update(false); break;
	case 2:
		sw_update();
		if (sw.run && sys_charger_online())	/* on the charger: a running stopwatch on screen stays on */
			power_awake_for("clock", 2000);
		break;
	case 3:
		tm_update();
		if (tmr.state == T_RUN && sys_charger_online())
			power_awake_for("clock", 2000);
		if (tmr.state == T_RUN && wall() % 30 == 0)
			tm_end_text();
		break;
	}
}

/* the lock screen over the app (it has its own clock): the labels stop ticking — a running stopwatch drew 30
 * frames a second under it — and its charger hold goes; unlock = they catch up at once */
static void lock_changed(bool locked)
{
	if (ui_tmr && locked) {
		lv_timer_pause(ui_tmr);
	} else if (ui_tmr) {
		lv_timer_resume(ui_tmr);
		lv_timer_ready(ui_tmr);
	}
}

static void app_open(lv_obj_t *r)
{
	root = r;
	host = NULL;
	wc_editing = al_editing = false;
	wc_reveal = al_reveal = -1;
	pill_from = -1;
	ui_tmr = lv_timer_create(ui_tick, 500, NULL);
	lock_listen(lock_changed, true);
	build_tab();
}

static void app_close(void)
{
	if (ui_tmr)
		lv_timer_delete(ui_tmr);
	ui_tmr = NULL;
	clear_tab_ptrs();
	root = host = NULL;
}

const app_t app_clock = {
	.name = "Clock",
	.glyph = G_CLOCK,
	.icon = IC_LIGHT,
	.open = app_open,
	.close = app_close,
};
