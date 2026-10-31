/* UI kit: iOS 26-style components without Liquid Glass — semantic palette from the theme,
 * navigation stack (slide in/out, interactive edge-swipe back), inset-grouped lists,
 * switches, sliders, segmented controls, floating tab bar, capsule buttons, alerts. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orchard.h"
#include "src/core/lv_obj_draw_private.h"
#include "src/misc/lv_area_private.h"
#include "src/misc/lv_event_private.h"
#include "src/misc/lv_text_private.h"

palette_t pal;

#define ROW_H 100
#define ROW_PAD 14	/* row padding above + below the content */
#define SIDE 32		/* list inset from the screen edge */
#define CELL_R 36	/* inset-grouped corner radius */
#define ROW_ICON 60	/* icon tile in a list row */
#define ROW_GAP 24	/* between a row's items (icon → title → value → chevron) */
#define ROW_INK 36	/* its glyph's larger side (iOS: ~60 % of the tile) */
#define TEXT_IN 32	/* row text inset from the cell's edge (section headers + footers line up with it) */

static int lum(uint32_t c) { return ((c >> 16 & 0xff) * 299 + (c >> 8 & 0xff) * 587 + (c & 0xff) * 114) / 1000; }

/* readable glyph colour on a coloured fill */
static uint32_t on(uint32_t bg) { return lum(bg) > 150 ? mix(th.bg, 0x000000, 40) : 0xffffff; }
uint32_t ui_on_color(uint32_t bg) { return on(bg); }

static void slides_frame(lv_event_t *e);
static void anims_now(lv_event_t *e) { (void)e; lv_anim_refr_now(); }
static void any_press(lv_event_t *e);

void ui_init(void)
{
	/* LVGL's animation timer runs after the refresh timer: step the animations right before each
	 * frame is drawn instead, so a frame never shows positions a period old */
	lv_display_add_event_cb(lv_display_get_default(), anims_now, LV_EVENT_REFR_START, NULL);
	lv_display_add_event_cb(lv_display_get_default(), slides_frame, LV_EVENT_REFR_START, NULL);
	/* the keyboard's events (orchard.h EV_KBD_*) are the first custom codes: reserve them */
	uint32_t shown = lv_event_register_id(), hidden = lv_event_register_id();
	if (shown != EV_KBD_SHOWN || hidden != EV_KBD_HIDDEN)
		fprintf(stderr, "orchard: keyboard event codes %u/%u are taken\n", (unsigned)shown, (unsigned)hidden);
	if (lv_indev_get_next(NULL))	/* input.c's pointer: a press anywhere puts an open swipe action away */
		lv_indev_add_event_cb(lv_indev_get_next(NULL), any_press, LV_EVENT_PRESSED, NULL);
	pal.bg = th.bg;
	if (th.dark) {
		pal.grouped_bg = th.bg;
		pal.cell = lum(th.lighter_bg) > lum(th.bg) ? th.lighter_bg : mix(th.bg, th.fg, 9);
	} else {
		pal.grouped_bg = mix(th.bg, th.fg, 6);
		pal.cell = th.bg;
	}
	pal.cell_hi = mix(pal.cell, th.fg, 12);
	pal.fill = mix(pal.cell, th.fg, 13);
	pal.separator = mix(pal.cell, th.fg, 15);
	pal.label = th.bright_fg;
	pal.label2 = th.muted;
	pal.label3 = mix(th.muted, pal.cell, 45);
	pal.tint = th.accent;
	pal.on_tint = on(th.accent);
	pal.red = th.ansi[1];
	pal.green = th.ansi[2];
	pal.yellow = th.ansi[3];
	pal.blue = th.ansi[4];
	pal.purple = th.ansi[5];
	pal.teal = th.ansi[6];
	pal.orange = mix(th.ansi[1], th.ansi[3], 50);
	pal.indigo = mix(th.ansi[4], th.ansi[5], 50);
	pal.pink = th.ansi[13];
	pal.gray = th.ansi[8];
}

void ui_haptic_tap(void) { sys_haptic(10); }

static void second_buzz(lv_timer_t *t)
{
	sys_haptic(18);
	lv_timer_delete(t);
}

void ui_haptic_success(void)
{
	sys_haptic(18);
	lv_timer_create(second_buzz, 140, NULL);
}

/* ---------- small helpers ---------- */

static void press_fade(lv_obj_t *o)
{
	lv_obj_set_style_opa(o, LV_OPA_50, LV_STATE_PRESSED);
}

static int text_w(const char *s, lv_font_t *f)
{
	lv_point_t sz;
	lv_text_get_size(&sz, s, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	return sz.x;
}

/* a label that stays on one line and ends in "…" when it doesn't fit (LVGL only cuts a label
 * short when its height is fixed: content height wraps instead) */
static void one_line(lv_obj_t *l)
{
	lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

/* Icons are Nerd Font glyphs, and their ink doesn't match their label: most are wider than the advance the
 * label is sized by (fa-wifi is 1.15 em wide, the advance 0.6 em) and sit a pixel or two high. A label centred
 * by its box therefore showed the icon to the right of centre, with whatever hung more than font_h / 4 past
 * the box clipped off (LVGL's label draw margin): the Wi-Fi, USB, speaker and battery icons lost their right
 * side. ui_glyph_fit makes the label's box the ink's box, so centring, alignment and flex gaps all work on
 * the icon you see; a fixed width (a centred column) or height (a line box in a layout) instead gets the ink
 * centred in it. A fitted label remembers what it fitted (FIT_W / FIT_H), so fitting it again after a glyph or font
 * change (ui_icon_set) gives the same result as fitting a fresh label. */
#define FIT_W LV_OBJ_FLAG_USER_4	/* its width / height is the ink's: fit it again from the content */
#define FIT_H LV_OBJ_FLAG_USER_3
void ui_glyph_fit(lv_obj_t *l)
{
	const lv_font_t *f = lv_obj_get_style_text_font(l, 0);
	const char *s = lv_label_get_text(l);
	uint32_t i = 0, letter = s && *s ? lv_text_encoded_next(s, &i) : 0;
	lv_font_glyph_dsc_t g;
	if (!letter || s[i] || !lv_font_get_glyph_dsc(f, &g, letter, 0) || g.box_w < 2 || g.box_h < 2)
		return;	/* one glyph only: text (Control Center's ">_" button) stays a plain label */
	int iw = g.box_w - 1, ih = g.box_h - 1;	/* tiny_ttf's bitmap box is one pixel wider + taller than the ink */
	int top = f->line_height - f->base_line - g.box_h - g.ofs_y;	/* ink top inside the label (lv_draw_label) */
	/* the pen starts at the content box: negative padding puts the ink's top-left corner on the box's
	 * (a translate would move the clip area along with the ink — LVGL clips a label at its bottom edge) */
	int32_t h = lv_obj_get_style_height(l, 0);
	if (h == LV_SIZE_CONTENT || LV_COORD_IS_PCT(h) || lv_obj_has_flag(l, FIT_H)) {
		lv_obj_set_height(l, ih);
		lv_obj_set_style_pad_top(l, -top, 0);
		lv_obj_add_flag(l, FIT_H);
	} else
		lv_obj_set_style_pad_top(l, (h - ih) / 2 - top, 0);
	lv_obj_set_style_pad_bottom(l, 0, 0);
	int32_t w = lv_obj_get_style_width(l, 0);
	if (w == LV_SIZE_CONTENT || LV_COORD_IS_PCT(w) || lv_obj_has_flag(l, FIT_W)) {
		lv_obj_set_width(l, iw);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
		lv_obj_set_style_pad_hor(l, 0, 0);
		lv_obj_set_style_pad_left(l, -g.ofs_x, 0);
		lv_obj_add_flag(l, FIT_W);
	} else {	/* a fixed column: the ink centred in it (it may hang out sideways: labels draw past their box) */
		lv_text_align_t a = lv_obj_get_style_text_align(l, 0);
		int pen = a == LV_TEXT_ALIGN_CENTER ? (w - g.adv_w) / 2 : a == LV_TEXT_ALIGN_RIGHT ? w - g.adv_w : 0;
		lv_obj_set_style_translate_x(l, (w - iw) / 2 - pen - g.ofs_x, 0);
	}
}

/* another glyph into an icon label: set (only when it changes: setting the same text redraws) and fit again.
 * Plain text instead (Control Center's timer shows the time left in its hourglass button) = a plain label again. */
void ui_icon_set(lv_obj_t *l, const char *glyph)
{
	const char *t = lv_label_get_text(l);
	if (t && !strcmp(t, glyph))
		return;
	lv_label_set_text(l, glyph);
	uint32_t i = 0, letter = lv_text_encoded_next(glyph, &i);
	if (letter >= 0xe000 && !glyph[i])
		ui_glyph_fit(l);
	else if (lv_obj_has_flag(l, FIT_W) || lv_obj_has_flag(l, FIT_H)) {
		lv_obj_remove_flag(l, FIT_W | FIT_H);
		lv_obj_set_size(l, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
		lv_obj_set_style_pad_all(l, 0, 0);
		lv_obj_set_style_translate_x(l, 0, 0);
	}
}

/* a fitted icon label */
lv_obj_t *ui_icon(lv_obj_t *parent, const char *glyph, lv_font_t *f, uint32_t color)
{
	lv_obj_t *l = text(parent, glyph, f, color);
	ui_glyph_fit(l);
	return l;
}

/* a fitted icon label that keeps the font's line box (laid out like the text label it was): only the ink moves,
 * to the box's centre — icons in a row or a column share one centre line whatever their shapes */
lv_obj_t *ui_icon_line(lv_obj_t *parent, const char *glyph, lv_font_t *f, uint32_t color)
{
	lv_obj_t *l = text(parent, glyph, f, color);
	lv_obj_set_height(l, lv_font_get_line_height(f));
	ui_glyph_fit(l);
	return l;
}

/* the icon font size at which the larger side of this glyph's ink is about `ink` px, so a tile's icons look
 * alike in size (at one font size fa-info is 12 px wide and fa-wifi 40); even sizes only, 3/4..11/10 of ink */
lv_font_t *ui_icon_font(const char *glyph, int ink)
{
	enum { REF = 40 };
	lv_font_t *ref = font(F_MONO, REF);
	uint32_t i = 0, letter = glyph && *glyph ? lv_text_encoded_next(glyph, &i) : 0;
	lv_font_glyph_dsc_t g;
	int px = ink;
	if (letter && lv_font_get_glyph_dsc(ref, &g, letter, 0) && g.box_w > 1 && g.box_h > 1) {
		int m = (g.box_w > g.box_h ? g.box_w : g.box_h) - 1;
		px = (REF * ink + m / 2) / m;
	}
	if (px < ink * 3 / 4)
		px = ink * 3 / 4;
	if (px > ink * 11 / 10)
		px = ink * 11 / 10;
	return font(F_MONO, (px + 1) & ~1);
}

/* ---------- motion ---------- */

int32_t motion_path(const lv_anim_t *a)
{
	/* ease-out quart: 1 - (1 - t)^4, in 1/1024 steps */
	int64_t t = a->act_time <= 0 ? 0 : a->act_time >= (int32_t)a->duration ? 1024 : (int64_t)a->act_time * 1024 / a->duration;
	int64_t r = 1024 - t, e = 1024 - r * r * r * r / (1024 * 1024 * 1024);
	return a->start_value + (int32_t)((a->end_value - a->start_value) * e / 1024);
}

void motion_anim(void *var, lv_anim_exec_xcb_t cb, int from, int to, int ms, lv_anim_completed_cb_t done)
{
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, var);
	lv_anim_set_exec_cb(&a, cb);
	lv_anim_set_values(&a, from, to);
	lv_anim_set_duration(&a, ms);
	lv_anim_set_path_cb(&a, motion_path);
	if (done)
		lv_anim_set_completed_cb(&a, done);
	lv_anim_start(&a);
}

int motion_fling_ms(int dist, int v, int max_ms)
{
	/* the quart curve leaves at 4 x its average speed: match that to the finger */
	dist = dist < 0 ? -dist : dist;
	int ms = v > 0 ? dist * 4000 / v : max_ms;
	return ms < 100 ? 100 : ms > max_ms ? max_ms : ms;
}

static void anim(void *var, lv_anim_exec_xcb_t cb, int from, int to, int ms, lv_anim_completed_cb_t done)
{
	motion_anim(var, cb, from, to, ms, done);
}

static void set_opa(void *o, int32_t v) { lv_obj_set_style_opa(o, v, 0); }

/* ---------- slides ----------
 * Software rendering can't redraw a whole screen every 16 ms, but a slide doesn't change any
 * pixels, it only moves them: the frame hook below shifts what is already in the framebuffer
 * and invalidates just the strips the move uncovered, the objects drawn above the slide (status
 * bar, banners — and their ghosts that moved along) and soft edges (shadows). Tracked objects
 * are listed bottom to top; each one's pixels are where it was at the last frame, minus what
 * the tracked objects above it covered. */

#define MAX_SLIDES 4
#define MAX_RECTS 32
typedef struct { lv_area_t a[MAX_RECTS]; int n; bool lost; } region_t;
static struct slide { lv_obj_t *obj; lv_area_t at; int ext, r; } slides[MAX_SLIDES];
static int nslides;
static const lv_area_t screen_area = { 0, 0, SCR_W - 1, SCR_H - 1 };

static void reg_add(region_t *r, lv_area_t a)
{
	if (!lv_area_intersect(&a, &a, &screen_area))
		return;
	if (r->n < MAX_RECTS)
		r->a[r->n++] = a;
	else
		r->lost = true;
}

static void reg_sub(region_t *r, const lv_area_t *b)
{
	region_t o = { .n = 0, .lost = r->lost };
	for (int i = 0; i < r->n; i++) {
		lv_area_t a = r->a[i], c;
		if (!lv_area_intersect(&c, &a, b)) {
			reg_add(&o, a);
			continue;
		}
		reg_add(&o, (lv_area_t){ a.x1, a.y1, a.x2, c.y1 - 1 });
		reg_add(&o, (lv_area_t){ a.x1, c.y2 + 1, a.x2, a.y2 });
		reg_add(&o, (lv_area_t){ a.x1, c.y1, c.x1 - 1, c.y2 });
		reg_add(&o, (lv_area_t){ c.x2 + 1, c.y1, a.x2, c.y2 });
	}
	*r = o;
}

static lv_area_t grown(lv_area_t a, int d)
{
	if (a.x1 <= a.x2)
		lv_area_increase(&a, d, d);
	return a;
}

static void inv(const region_t *r)
{
	for (int i = 0; i < r->n; i++)
		lv_obj_invalidate_area(lv_screen_active(), &r->a[i]);
}

/* move one rectangle of the framebuffer by dx, dy (both parts already clipped to the screen) */
static void blit_move(lv_draw_buf_t *fb, const lv_area_t *src, int dx, int dy)
{
	uint32_t stride = fb->header.stride;
	int w = lv_area_get_width(src), h = lv_area_get_height(src);
	uint8_t *s = fb->data + src->y1 * stride + src->x1 * 4, *d = s + dy * (int)stride + dx * 4;
	static uint32_t row[SCR_W];
	if (w == SCR_W && stride == SCR_W * 4 && !dx)
		memmove(d, s, (size_t)h * stride);
	else if (dx)	/* sideways: through a row buffer (musl's memmove goes byte by byte when the
			 * two ends are 4 bytes out of 8-byte alignment, i.e. every odd dx — 10 ms a frame) */
		for (int k = 0; k < h; k++) {
			int y = dy > 0 ? h - 1 - k : k;
			memcpy(row, s + y * stride, w * 4);
			memcpy(d + y * stride, row, w * 4);
		}
	else if (dy > 0)
		for (int y = h - 1; y >= 0; y--)
			memmove(d + y * stride, s + y * stride, w * 4);
	else
		for (int y = 0; y < h; y++)
			memmove(d + y * stride, s + y * stride, w * 4);
	lv_area_t dst = *src;
	lv_area_move(&dst, dx, dy);
	hal_damage(&dst);
}

static bool shown(lv_obj_t *o)
{
	for (; o; o = lv_obj_get_parent(o))
		if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN))
			return false;
	return true;
}

/* layer index (shell_layer order) an object lives in, -1 = the screen (apps, home) */
static int layer_of(lv_obj_t *o)
{
	for (; o && lv_obj_get_parent(o); o = lv_obj_get_parent(o))
		if (lv_obj_get_parent(lv_obj_get_parent(o)) == lv_layer_top())
			return lv_obj_get_index(lv_obj_get_parent(o));
	return -1;
}

static int cmp_down(const void *a, const void *b) { return ((const lv_area_t *)b)->y1 - ((const lv_area_t *)a)->y1; }
static int cmp_up(const void *a, const void *b) { return ((const lv_area_t *)a)->y1 - ((const lv_area_t *)b)->y1; }
static int cmp_right(const void *a, const void *b) { return ((const lv_area_t *)b)->x1 - ((const lv_area_t *)a)->x1; }
static int cmp_left(const void *a, const void *b) { return ((const lv_area_t *)a)->x1 - ((const lv_area_t *)b)->x1; }

static bool reg_meets(const region_t *a, const region_t *b, region_t *both)
{
	bool hit = false;
	for (int i = 0; i < a->n; i++)
		for (int k = 0; k < b->n; k++) {
			lv_area_t c;
			if (lv_area_intersect(&c, &a->a[i], &b->a[k])) {
				hit = true;
				if (both)
					reg_add(both, c);
			}
		}
	return hit;
}

static void slides_frame(lv_event_t *e)
{
	(void)e;
	lv_draw_buf_t *fb = lv_display_get_buf_active(NULL);
	bool direct = fb && fb->header.w == SCR_W && fb->header.h == SCR_H && lv_color_format_get_size(fb->header.cf) == 4;
	direct &= !getenv("ORCHARD_NOSLIDE");	/* debugging: redraw instead, to compare frames against */
	lv_area_t now[MAX_SLIDES];
	int low = LY_COUNT;
	bool moved = false;
	for (int i = 0; i < nslides; i++) {
		lv_obj_get_coords(slides[i].obj, &now[i]);
		if (!shown(slides[i].obj))
			now[i] = (lv_area_t){ -1, -1, -2, -2 };
		moved |= now[i].x1 != slides[i].at.x1 || now[i].y1 != slides[i].at.y1;
		int ly = layer_of(slides[i].obj);
		low = ly < low ? ly : low;
	}
	if (!moved)
		return;

	/* what is drawn above the slides moves along with them: redraw it in place and where its
	 * copy landed. A big overlay (switcher, alert) makes shifting pointless: redraw normally. */
	lv_area_t over[16];
	int nover = 0;
	long over_px = 0;
	for (int ly = low + 1; ly <= LY_COUNT; ly++) {
		lv_obj_t *l = ly < LY_COUNT ? shell_layer(ly) : lv_layer_sys();
		for (uint32_t k = 0; l && k < lv_obj_get_child_count(l); k++) {
			lv_obj_t *c = lv_obj_get_child(l, k);
			lv_area_t a;
			if (!lv_obj_is_visible(c))
				continue;
			lv_obj_get_coords(c, &a);
			a = grown(a, lv_obj_get_ext_draw_size(c));
			if (!lv_area_intersect(&a, &a, &screen_area))
				continue;
			over_px += lv_area_get_size(&a);
			if (nover < 16)
				over[nover++] = a;
			else
				direct = false;
		}
	}
	direct &= over_px < SCR_W * SCR_H / 2;

	/* each slide's pixels: where it was, minus the slides above it (and their soft edges) */
	region_t src[MAX_SLIDES], dst[MAX_SLIDES], redraw = { .n = 0 };
	for (int i = 0; i < nslides; i++) {
		int dx = now[i].x1 - slides[i].at.x1, dy = now[i].y1 - slides[i].at.y1;
		region_t r = { .n = 0 };
		reg_add(&r, slides[i].at);
		for (int j = i + 1; j < nslides; j++) {
			lv_area_t a = grown(slides[j].at, slides[j].ext);
			reg_sub(&r, &a);
		}
		src[i] = (region_t){ .n = 0 };
		dst[i] = (region_t){ .n = 0 };
		direct &= !r.lost;
		for (int k = 0; k < r.n; k++) {
			lv_area_t d = r.a[k], sa;
			lv_area_move(&d, dx, dy);
			if (!lv_area_intersect(&d, &d, &screen_area))
				continue;
			sa = d;
			lv_area_move(&sa, -dx, -dy);
			reg_add(&src[i], sa);
			reg_add(&dst[i], d);
		}
	}
	/* move bottom-up so the upper slide lands on top, unless a lower one would land on pixels an
	 * upper one still has to move: then top-down, and redraw where they overlap */
	bool up = true, down = true;
	for (int i = 0; i < nslides; i++)
		for (int j = i + 1; j < nslides; j++) {
			up &= !reg_meets(&dst[i], &src[j], NULL);
			down &= !reg_meets(&dst[j], &src[i], NULL);
		}
	direct &= up || down;
	for (int n = 0; direct && n < nslides; n++) {
		int i = up ? n : nslides - 1 - n;
		int dx = now[i].x1 - slides[i].at.x1, dy = now[i].y1 - slides[i].at.y1;
		if (!dx && !dy)
			continue;
		region_t *s = &src[i];
		/* pieces of one slide: move the one ahead first */
		qsort(s->a, s->n, sizeof(lv_area_t), dy > 0 ? cmp_down : dy < 0 ? cmp_up : dx > 0 ? cmp_right : cmp_left);
		for (int k = 0; k < s->n; k++) {
			blit_move(fb, &s->a[k], dx, dy);
			dst[i].a[k] = s->a[k];
			lv_area_move(&dst[i].a[k], dx, dy);
			for (int m = 0; m < nover; m++) {	/* the overlay's ghost */
				lv_area_t g;
				if (lv_area_intersect(&g, &over[m], &s->a[k])) {
					lv_area_move(&g, dx, dy);
					reg_add(&redraw, g);
				}
			}
		}
		if (!up)
			for (int j = i + 1; j < nslides; j++)
				reg_meets(&dst[i], &dst[j], &redraw);
	}
	for (int i = 0; i < nslides; i++) {
		struct slide *s = &slides[i];
		region_t out = { .n = 0 };
		/* the slide where no moved pixels landed, plus its soft edge (it blends with what moved
		 * differently below), plus what it uncovered */
		reg_add(&out, now[i]);
		for (int k = 0; direct && k < dst[i].n; k++)
			reg_sub(&out, &dst[i].a[k]);
		if (s->ext) {
			region_t edge = { .n = 0 };
			reg_add(&edge, grown(now[i], s->ext));
			reg_sub(&edge, &now[i]);
			for (int k = 0; k < edge.n; k++)
				reg_add(&out, edge.a[k]);
		}
		if (s->r && now[i].x1 <= now[i].x2) {	/* rounded corners carried what was behind them */
			reg_add(&out, (lv_area_t){ now[i].x1, now[i].y1, now[i].x2, now[i].y1 + s->r - 1 });
			reg_add(&out, (lv_area_t){ now[i].x1, now[i].y2 - s->r + 1, now[i].x2, now[i].y2 });
		}
		region_t gone = { .n = 0 };
		reg_add(&gone, grown(s->at, s->ext));
		reg_sub(&gone, &now[i]);
		for (int k = 0; k < gone.n; k++)
			reg_add(&out, gone.a[k]);
		for (int j = i + 1; j < nslides; j++)
			reg_sub(&out, &now[j]);
		for (int k = 0; k < out.n; k++)
			reg_add(&redraw, out.a[k]);
		redraw.lost |= out.lost;
		s->at = now[i];
	}
	for (int k = 0; k < nover; k++)
		reg_add(&redraw, over[k]);
	if (redraw.lost)
		lv_obj_invalidate(lv_screen_active());
	else
		inv(&redraw);
}

static void slide_forget(lv_obj_t *o)
{
	for (int i = 0; i < nslides; i++)
		if (slides[i].obj == o) {
			memmove(slides + i, slides + i + 1, (nslides - i - 1) * sizeof(slides[0]));
			nslides--;
			return;
		}
}

static void slide_deleted(lv_event_t *e) { slide_forget(lv_event_get_target(e)); }

void slide_track(lv_obj_t *o, bool on)
{
	int i = 0;
	while (i < nslides && slides[i].obj != o)
		i++;
	if (!on) {
		if (i == nslides)
			return;
		lv_obj_remove_event_cb(o, slide_deleted);
		memmove(slides + i, slides + i + 1, (nslides - i - 1) * sizeof(slides[0]));
		nslides--;
		lv_obj_invalidate(o);	/* one honest frame: nothing stale survives the slide */
		return;
	}
	if (i < nslides || nslides == MAX_SLIDES)
		return;
	lv_obj_add_event_cb(o, slide_deleted, LV_EVENT_DELETE, NULL);
	slides[nslides].obj = o;
	lv_obj_get_coords(o, &slides[nslides].at);
	if (!shown(o))
		slides[nslides].at = (lv_area_t){ -1, -1, -2, -2 };
	slides[nslides].ext = lv_obj_get_ext_draw_size(o);
	slides[nslides].r = LV_MIN(lv_obj_get_style_radius(o, 0), lv_obj_get_height(o) / 2);
	nslides++;
}

void slide_move(lv_obj_t *o, int x, int y)
{
	bool tracked = false;
	for (int i = 0; i < nslides; i++)
		tracked |= slides[i].obj == o;
	if (!tracked) {
		lv_obj_set_pos(o, x, y);
		return;
	}
	/* applied now (not at the next layout pass, which would invalidate both places) */
	lv_display_enable_invalidation(NULL, false);
	lv_obj_set_pos(o, x, y);
	lv_obj_refr_pos(o);
	lv_display_enable_invalidation(NULL, true);
	lv_timer_resume(lv_display_get_refr_timer(NULL));	/* nothing was invalidated: wake the refresh */
}

/* ---------- navigation stack ---------- */

#define MAX_PAGES 16	/* a hidden page costs only its objects (a 150-row folder ≈ 1 MB) */
#define BAR_GAP 16	/* nav bar: least space between the title and a button */
static struct page {
	lv_obj_t *obj, *bar, *content, *title, *large, *back, *right, *hair;
	int right_w;
	char name[128];	/* full title (the label may show it cut short) */
	void (*leave)(void);
} pages[MAX_PAGES];
static int depth = -1;
static lv_obj_t *nav_host;
static bool dragging, nav_held;	/* nav_held: nav_lock — a full-screen sheet covers the page */

static void back_clicked(lv_event_t *e)
{
	(void)e;
	nav_pop();
}

/* root page: the small title + bar hairline fade in once the large title scrolls away */
static void content_scrolled(lv_event_t *e)
{
	struct page *p = lv_event_get_user_data(e);
	int y = lv_obj_get_scroll_y(p->content);
	lv_opa_t o = y < 60 ? LV_OPA_TRANSP : y > 100 ? LV_OPA_COVER : (y - 60) * 255 / 40;
	if (p->large)
		lv_obj_set_style_opa(p->title, o, 0);
	lv_obj_set_style_opa(p->hair, y > 4 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

/* the title stays centred over the whole bar. When it needs the room, the back button gives up
 * the previous page's title for "Back", then keeps only its chevron (iOS); only then is the
 * title itself cut short */
static void bar_layout(struct page *p)
{
	lv_font_t *tf = font(F_TEXT_SEMI, 34), *bf = font(F_TEXT, 34);
	int tw = text_w(p->name, tf), room = SCR_W - 2 * (LV_MAX(p->right_w, SIDE) + BAR_GAP);
	if (p->back) {
		lv_obj_t *l = lv_obj_get_child(p->back, 1);
		const char *prev = p[-1].name[0] ? p[-1].name : "Back", *opt[] = { prev, "Back", "" };
		int chev = 2 * 20 + text_w(G_CHEVRON_L, font(F_MONO, 36));
		for (int i = 0; i < 3; i++) {
			int bw = chev + (opt[i][0] ? 10 + text_w(opt[i], bf) : 0);
			room = SCR_W - 2 * (LV_MAX(bw, p->right_w) + BAR_GAP);
			lv_label_set_text(l, opt[i]);
			if (tw <= room)
				break;
		}
		if (lv_label_get_text(l)[0])
			lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
	}
	lv_obj_set_width(p->title, tw <= room ? LV_SIZE_CONTENT : LV_MAX(room, 1));
	one_line(p->title);
	lv_obj_align(p->title, LV_ALIGN_CENTER, 0, 0);
}

static struct page *page_new(const char *title, bool large)
{
	if (depth + 1 >= MAX_PAGES)
		return NULL;
	struct page *p = &pages[++depth];
	memset(p, 0, sizeof(*p));
	snprintf(p->name, sizeof(p->name), "%s", title);
	p->obj = box(nav_host);
	lv_obj_set_size(p->obj, SCR_W, SCR_H);
	lv_obj_set_style_bg_opa(p->obj, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(p->obj, C(pal.grouped_bg), 0);

	p->content = box(p->obj);
	lv_obj_set_pos(p->content, 0, STATUS_H + NAV_H);
	lv_obj_set_size(p->content, SCR_W, SCR_H - STATUS_H - NAV_H);
	lv_obj_add_flag(p->content, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(p->content, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(p->content, LV_SCROLLBAR_MODE_OFF);
	lv_obj_set_flex_flow(p->content, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p->content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(p->content, 0, 0);
	lv_obj_set_style_pad_bottom(p->content, 140, 0);

	p->bar = box(p->obj);
	lv_obj_set_pos(p->bar, 0, STATUS_H);
	lv_obj_set_size(p->bar, SCR_W, NAV_H);
	p->hair = box(p->bar);
	lv_obj_set_size(p->hair, SCR_W, 1);
	lv_obj_align(p->hair, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(p->hair, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(p->hair, C(pal.separator), 0);
	lv_obj_set_style_opa(p->hair, LV_OPA_TRANSP, 0);

	p->title = text(p->bar, title, font(F_TEXT_SEMI, 34), pal.label);
	lv_obj_set_style_text_align(p->title, LV_TEXT_ALIGN_CENTER, 0);

	if (large) {
		lv_obj_set_style_opa(p->title, LV_OPA_TRANSP, 0);
		p->large = text(p->content, title, font(F_TEXT_BOLD, 68), pal.label);
		lv_obj_set_width(p->large, SCR_W - 2 * SIDE);
		lv_obj_set_style_pad_bottom(p->large, 16, 0);
	}
	lv_obj_add_event_cb(p->content, content_scrolled, LV_EVENT_SCROLL, p);

	if (depth > 0) {
		p->back = box(p->bar);
		lv_obj_set_size(p->back, LV_SIZE_CONTENT, NAV_H);
		lv_obj_set_style_pad_hor(p->back, 20, 0);
		lv_obj_set_flex_flow(p->back, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(p->back, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_column(p->back, 10, 0);
		lv_obj_add_flag(p->back, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_ext_click_area(p->back, 10);
		press_fade(p->back);
		lv_obj_add_event_cb(p->back, back_clicked, LV_EVENT_CLICKED, NULL);
		text(p->back, G_CHEVRON_L, font(F_MONO, 36), pal.tint);
		text(p->back, "", font(F_TEXT, 34), pal.tint);	/* the previous title, "Back" or nothing */
	}
	bar_layout(p);
	return p;
}

lv_obj_t *nav_root(lv_obj_t *app_root, const char *title)
{
	nav_reset();
	nav_host = app_root;
	struct page *p = page_new(title, true);
	return p->content;
}

/* Pages slide as one motion: x = top page's left edge (0 = in place, SCR_W = gone), the page
 * under it trails at 30 % (iOS parallax). Both are slides: their pixels move, only the strip
 * coming in and the top page's shadow are drawn. */
static int nav_x, nav_to;
static void (*nav_landed)(void);
static lv_obj_t *nav_shield;	/* over the page being left while it slides: a second tap on its Done
				 * / Back / row can't pop or push again (a double tap popped two pages) */

static void shield_gone(lv_event_t *e) { (void)e; nav_shield = NULL; }

static void shield(lv_obj_t *page)
{
	lv_display_enable_invalidation(NULL, false);	/* transparent: nothing to draw */
	if (nav_shield)
		lv_obj_delete(nav_shield);
	nav_shield = box(page);
	lv_obj_add_event_cb(nav_shield, shield_gone, LV_EVENT_DELETE, NULL);	/* with its page */
	lv_obj_set_size(nav_shield, SCR_W, SCR_H);
	lv_obj_add_flag(nav_shield, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_IGNORE_LAYOUT);
	lv_obj_remove_flag(nav_shield, LV_OBJ_FLAG_GESTURE_BUBBLE);
	lv_display_enable_invalidation(NULL, true);
}

static void unshield(void)
{
	if (!nav_shield)
		return;
	lv_display_enable_invalidation(NULL, false);
	lv_obj_delete(nav_shield);
	nav_shield = NULL;
	lv_display_enable_invalidation(NULL, true);
}

static void nav_place(void *v, int32_t x)
{
	(void)v;
	nav_x = x;
	slide_move(pages[depth].obj, x, 0);
	slide_move(pages[depth - 1].obj, -SCR_W * 3 / 10 + x * 3 / 10, 0);
}

static void nav_track(bool on)
{
	slide_track(pages[depth - 1].obj, on);
	slide_track(pages[depth].obj, on);
}

static void nav_anim_done(lv_anim_t *a)
{
	(void)a;
	void (*f)(void) = nav_landed;
	nav_landed = NULL;
	unshield();
	nav_track(false);
	if (f)
		f();
}

static void nav_go(int to, int ms, void (*landed)(void))
{
	nav_to = to;
	nav_landed = landed;
	motion_anim(&nav_x, nav_place, nav_x, to, ms, nav_anim_done);
}

/* new input never waits for a slide: the running one lands at once */
static void nav_settle(void)
{
	if (!lv_anim_get(&nav_x, nav_place))
		return;
	lv_anim_delete(&nav_x, nav_place);
	nav_place(NULL, nav_to);
	nav_anim_done(NULL);
}

static void hide_prev(void) { lv_obj_add_flag(pages[depth - 1].obj, LV_OBJ_FLAG_HIDDEN); }

lv_obj_t *nav_push(const char *title)
{
	if (!nav_host || depth < 0 || dragging)
		return NULL;
	nav_settle();
	ui_swipe_close();
	/* built off to the right without invalidating anything: the slide draws it strip by strip */
	lv_display_enable_invalidation(NULL, false);
	struct page *p = page_new(title, false);
	if (p) {
		lv_obj_set_style_shadow_width(p->obj, 40, 0);
		lv_obj_set_style_shadow_opa(p->obj, LV_OPA_30, 0);
		lv_obj_set_x(p->obj, SCR_W);
		lv_obj_refr_size(p->obj);
		lv_obj_refr_pos(p->obj);
	}
	lv_display_enable_invalidation(NULL, true);
	if (!p)
		return NULL;
	nav_x = SCR_W;
	nav_track(true);
	shield(pages[depth - 1].obj);
	nav_go(0, MOTION_PAGE, hide_prev);
	return p->content;
}

static void page_drop(void)
{
	struct page *p = &pages[depth];
	if (p->leave)
		p->leave();
	lv_obj_delete(p->obj);
	depth--;
}

/* the page under the top one comes back: visible again, but not redrawn until it is uncovered */
static void show_prev(void)
{
	lv_display_enable_invalidation(NULL, false);
	lv_obj_remove_flag(pages[depth - 1].obj, LV_OBJ_FLAG_HIDDEN);
	lv_display_enable_invalidation(NULL, true);
	nav_track(true);
}

void nav_pop(void)
{
	if (depth < 1 || dragging)
		return;
	nav_settle();
	if (depth < 1)
		return;
	nav_x = lv_obj_get_x(pages[depth].obj);
	show_prev();
	shield(pages[depth].obj);
	nav_go(SCR_W, MOTION_PAGE, page_drop);
}

void nav_drag(int phase, int dx, int vx)
{
	if (phase == 0) {
		nav_settle();
		dragging = depth >= 1 && !nav_locked();
		if (dragging) {
			nav_x = 0;
			show_prev();
		}
		return;
	}
	if (!dragging)
		return;
	dx = dx < 0 ? 0 : dx > SCR_W ? SCR_W : dx;
	if (phase == 1) {
		nav_place(NULL, dx);
		return;
	}
	/* released: a flick decides, otherwise how far it got; leave at the finger's speed */
	dragging = false;
	if (vx > 400 || (dx > SCR_W / 3 && vx > -300)) {
		shield(pages[depth].obj);
		nav_go(SCR_W, motion_fling_ms(SCR_W - dx, vx, MOTION_PAGE), page_drop);
	} else {
		nav_go(0, motion_fling_ms(dx, -vx, MOTION_PAGE), hide_prev);
	}
}

/* the slide in flight (a push) lands at once: a page built in place, e.g. a restored tab, shows without moving */
void nav_land(void) { nav_settle(); }

/* every page above `to` goes at once, no slide (each one's leave callback first); page `to` shows again */
void nav_cut(int to)
{
	if (!nav_host || dragging)
		return;
	nav_settle();
	if (to < 0 || depth <= to)
		return;
	ui_menu_close();
	ui_swipe_close();
	while (depth > to)
		page_drop();
	lv_obj_set_x(pages[depth].obj, 0);	/* it waited 30 % to the left, hidden (nav_place) */
	lv_obj_remove_flag(pages[depth].obj, LV_OBJ_FLAG_HIDDEN);
}

bool nav_pristine(void)
{
	return !nav_host || (depth == 0 && lv_obj_get_scroll_y(pages[0].content) <= 0);
}

int nav_depth(void) { return nav_host ? depth : -1; }
void nav_lock(bool on) { nav_held = on; }
lv_obj_t *nav_page(void) { return depth >= 0 ? pages[depth].obj : NULL; }
void nav_on_leave(void (*cb)(void)) { if (depth >= 0) pages[depth].leave = cb; }

void nav_set_title(lv_obj_t *page, const char *title)
{
	for (int i = 0; i <= depth; i++) {
		struct page *p = &pages[i];
		if (p->obj != page || !strcmp(p->name, title))
			continue;
		snprintf(p->name, sizeof(p->name), "%s", title);
		lv_label_set_text(p->title, title);
		if (p->large)
			lv_label_set_text(p->large, title);
		bar_layout(p);
		if (i < depth)
			bar_layout(p + 1);	/* its back label */
		return;
	}
}

lv_obj_t *nav_button(const char *label, lv_event_cb_t cb, void *ud)
{
	if (depth < 0)
		return NULL;
	struct page *p = &pages[depth];
	lv_obj_t *b = box(p->bar);
	lv_obj_set_size(b, LV_SIZE_CONTENT, NAV_H);
	lv_obj_set_style_pad_hor(b, 32, 0);
	bool icon = strlen(label) <= 4 && (unsigned char)label[0] >= 0xe0;
	lv_font_t *f = font(icon ? F_MONO : F_TEXT_SEMI, 34);
	lv_obj_t *l = text(b, label, f, pal.tint);
	if (icon)
		ui_glyph_fit(l);	/* the ink on the bar's 32 px inset, centred on the bar (the + was 5 px right, 1 px high) */
	lv_obj_center(l);
	lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	press_fade(b);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	p->right = b;
	p->right_w = 2 * 32 + (icon ? lv_obj_get_style_width(l, 0) : text_w(label, f));
	bar_layout(p);
	return b;
}

void nav_reset(void)
{
	ui_menu_close();	/* a menu belongs to the page it was opened on */
	lv_anim_delete(&nav_x, NULL);
	nav_landed = NULL;
	while (depth >= 0) {
		slide_track(pages[depth].obj, false);
		if (pages[depth].leave)
			pages[depth].leave();
		depth--;
	}
	nav_host = NULL;
	dragging = nav_held = false;
}

/* ---------- inset-grouped lists ---------- */

lv_obj_t *ui_section(lv_obj_t *content, const char *header, const char *footer)
{
	if (header) {
		lv_obj_t *h = text(content, header, font(F_TEXT_SEMI, 28), pal.label2);
		lv_obj_set_width(h, SCR_W - 2 * SIDE - 2 * TEXT_IN);
		lv_obj_set_style_pad_top(h, 36, 0);
		lv_obj_set_style_pad_bottom(h, 12, 0);
	} else {
		lv_obj_t *gap = box(content);
		lv_obj_set_size(gap, 1, 36);
	}
	lv_obj_t *s = box(content);
	lv_obj_set_width(s, SCR_W - 2 * SIDE);
	lv_obj_set_height(s, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(s, CELL_R, 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(s, C(pal.cell), 0);
	lv_obj_set_style_clip_corner(s, true, 0);
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
	if (footer) {
		lv_obj_t *f = text(content, footer, font(F_TEXT, 26), pal.label2);
		lv_obj_set_width(f, SCR_W - 2 * SIDE - 2 * TEXT_IN);
		lv_label_set_long_mode(f, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_pad_top(f, 12, 0);
		lv_obj_set_style_text_line_space(f, 6, 0);
	}
	return s;
}

lv_obj_t *ui_row(lv_obj_t *sec, const char *glyph, uint32_t color, const char *title)
{
	lv_obj_t *r = box(sec);
	lv_obj_set_width(r, LV_PCT(100));
	lv_obj_set_height(r, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(r, ROW_H, 0);
	lv_obj_set_style_pad_hor(r, TEXT_IN, 0);
	lv_obj_set_style_pad_ver(r, ROW_PAD, 0);
	lv_obj_set_style_pad_column(r, ROW_GAP, 0);
	lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	if (lv_obj_get_index(r) > 0) {	/* hairline separator, inset to the text like iOS */
		lv_obj_t *sep = box(r);
		lv_obj_add_flag(sep, LV_OBJ_FLAG_FLOATING);
		lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(sep, SCR_W, 1);
		lv_obj_set_pos(sep, glyph ? ROW_ICON + ROW_GAP : 0, -ROW_PAD);	/* starts where the title does */
		lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(sep, C(pal.separator), 0);
	}
	if (glyph) {
		lv_obj_t *ic = box(r);
		lv_obj_remove_flag(ic, LV_OBJ_FLAG_CLICKABLE);	/* taps belong to the row */
		lv_obj_set_size(ic, ROW_ICON, ROW_ICON);
		lv_obj_set_style_radius(ic, (ROW_ICON * 225 + 500) / 1000, 0);	/* 22.5 %, like the app icons */
		lv_obj_set_style_bg_opa(ic, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(ic, C(color), 0);
		lv_obj_t *g = text(ic, glyph, ui_icon_font(glyph, ROW_INK), on(color));
		ui_glyph_fit(g);
		lv_obj_center(g);
	}
	/* the row is content-sized (a subtitle makes it taller) and LVGL puts a content-sized flex
	 * track at the top, not the middle: the text column fills the row's inner height itself and
	 * centres its lines, so the track is that high and everything beside it centres too */
	lv_obj_t *col = box(r);
	lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(col, ROW_H - 2 * ROW_PAD, 0);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_row(col, 4, 0);
	lv_obj_t *t = text(col, title, font(F_TEXT, 34), pal.label);
	lv_obj_set_width(t, LV_PCT(100));
	one_line(t);
	lv_obj_set_user_data(r, col);
	return r;
}

lv_obj_t *ui_row_subtitle(lv_obj_t *row, const char *sub)
{
	lv_obj_t *l = text(lv_obj_get_user_data(row), sub, font(F_TEXT, 26), pal.label2);
	lv_obj_set_width(l, LV_PCT(100));
	one_line(l);
	return l;
}

lv_obj_t *ui_row_value(lv_obj_t *row, const char *value)
{
	lv_obj_t *v = text(row, value, font(F_TEXT, 34), pal.label2);
	lv_obj_set_style_max_width(v, 380, 0);
	one_line(v);
	return v;
}

void ui_row_chevron(lv_obj_t *row)
{
	ui_glyph_fit(text(row, G_CHEVRON_R, font(F_MONO, 28), pal.label3));	/* ends at the text inset */
}

static void row_pressable(lv_obj_t *row)
{
	lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(row, C(pal.cell_hi), LV_STATE_PRESSED);
}

void ui_row_tap(lv_obj_t *row, lv_event_cb_t cb, void *ud)
{
	row_pressable(row);
	lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, ud);
}

/* ---------- row holds + swipe actions ----------
 * One finger at a time, so one record of the touch on a row: a hold is a finger that stayed put
 * for LVGL's long-press time; a swipe is LVGL's horizontal gesture. Either one means the release
 * isn't a tap (CLICKED fires after both): a preprocess handler drops it before the app's. */

#define SWIPE_W 176	/* how far a row slides to show its action */
#define HOLD_SLOP 20	/* px a finger may drift and still be holding */

static struct { lv_obj_t *row; lv_point_t at; bool held, swiped, was_open; } touch;
static lv_obj_t *swiped_row;	/* the row showing its swipe action */

/* the swipe action pill (user flag 2) keeps the row's resting side padding in its user data */
static lv_obj_t *swipe_pill(lv_obj_t *row)
{
	for (int i = (int)lv_obj_get_child_count(row) - 1; i >= 0; i--)
		if (lv_obj_has_flag(lv_obj_get_child(row, i), LV_OBJ_FLAG_USER_2))
			return lv_obj_get_child(row, i);
	return NULL;
}

static int swipe_base(lv_obj_t *row)
{
	lv_obj_t *p = swipe_pill(row);
	return p ? (int)(intptr_t)lv_obj_get_user_data(p) : 0;
}

/* the row's content and the pill just past its right edge shift left together (the row clips) */
static void swipe_shift(void *row, int32_t v)
{
	int base = swipe_base(row);
	lv_obj_set_style_pad_left(row, base - v, 0);
	lv_obj_set_style_pad_right(row, base + v, 0);
}

bool ui_swipe_shown(void) { return swiped_row != NULL; }

void ui_swipe_close(void)
{
	if (!swiped_row)
		return;
	lv_obj_t *r = swiped_row;
	swiped_row = NULL;
	lv_anim_delete(r, swipe_shift);
	anim(r, swipe_shift, lv_obj_get_style_pad_right(r, 0) - swipe_base(r), 0, MOTION_SMALL, NULL);
}

static void swipe_open(lv_obj_t *row)
{
	if (swiped_row != row)
		ui_swipe_close();
	swiped_row = row;
	lv_anim_delete(row, swipe_shift);
	anim(row, swipe_shift, lv_obj_get_style_pad_right(row, 0) - swipe_base(row), SWIPE_W, MOTION_SMALL, NULL);
}

/* indev-level PRESSED (param = the object hit): a press anywhere but on the open row puts its
 * action away; on the app's own page that press does nothing else (iOS), above it (an alert,
 * a sheet) it goes through */
static void any_press(lv_event_t *e)
{
	if (!swiped_row)
		return;
	lv_obj_t *o = lv_event_get_param(e);
	for (lv_obj_t *p = o; p; p = lv_obj_get_parent(p))
		if (p == swiped_row)
			return;
	ui_swipe_close();
	if (o && lv_obj_get_screen(o) == lv_screen_active()) {
		lv_indev_stop_processing(lv_indev_active());
		lv_indev_wait_release(lv_indev_active());
	}
}

static void row_touch(lv_event_t *e)
{
	lv_obj_t *row = lv_event_get_current_target(e);
	lv_indev_t *in = lv_indev_active();
	lv_point_t p = { 0, 0 };
	if (in)
		lv_indev_get_point(in, &p);
	switch (lv_event_get_code(e)) {
	case LV_EVENT_PRESSED:
		touch.row = row;
		touch.at = p;
		touch.held = touch.swiped = false;
		touch.was_open = swiped_row == row;
		break;
	case LV_EVENT_LONG_PRESSED:	/* preprocess: a hold, unless the finger wandered off */
		if (touch.row != row || touch.swiped || touch.was_open || LV_ABS(p.x - touch.at.x) > HOLD_SLOP ||
		    LV_ABS(p.y - touch.at.y) > HOLD_SLOP) {
			lv_event_stop_processing(e);
			break;
		}
		touch.held = true;
		lv_obj_remove_state(row, LV_STATE_PRESSED);
		ui_haptic_tap();
		break;
	case LV_EVENT_GESTURE: {
		lv_dir_t d = in ? lv_indev_get_gesture_dir(in) : LV_DIR_NONE;
		if ((touch.row != row && swiped_row != row) || touch.held || (d != LV_DIR_LEFT && d != LV_DIR_RIGHT))
			break;
		touch.swiped = true;
		lv_obj_remove_state(row, LV_STATE_PRESSED);
		if (d == LV_DIR_LEFT)
			swipe_open(row);
		else
			ui_swipe_close();
		break;
	}
	case LV_EVENT_CLICKED:	/* preprocess: after a hold or a swipe the release isn't a tap */
		if (touch.row != row)
			break;
		if (touch.was_open && !touch.held && !touch.swiped)
			ui_swipe_close();	/* the first tap on an open row only closes it */
		if (touch.held || touch.swiped || touch.was_open)
			lv_event_stop_processing(e);
		touch.row = NULL;
		break;
	case LV_EVENT_DELETE:
		if (swiped_row == row)
			swiped_row = NULL;
		if (touch.row == row)
			touch.row = NULL;
		break;
	default:
		break;
	}
}

/* row_touch for this event code, once */
static void row_on(lv_obj_t *row, uint32_t code)
{
	for (uint32_t i = 0; i < lv_obj_get_event_count(row); i++) {
		lv_event_dsc_t *d = lv_obj_get_event_dsc(row, i);
		if (d->cb == row_touch && d->filter == code)
			return;
	}
	lv_obj_add_event_cb(row, row_touch, code, NULL);
}

void ui_row_hold(lv_obj_t *row, lv_event_cb_t cb, void *ud)
{
	row_pressable(row);
	row_on(row, LV_EVENT_PRESSED);
	row_on(row, LV_EVENT_LONG_PRESSED | LV_EVENT_PREPROCESS);
	row_on(row, LV_EVENT_CLICKED | LV_EVENT_PREPROCESS);
	row_on(row, LV_EVENT_DELETE);
	lv_obj_add_event_cb(row, cb, LV_EVENT_LONG_PRESSED, ud);
}

static void pill_click(lv_event_t *e)
{
	lv_obj_t *row = lv_obj_get_parent(lv_event_get_current_target(e));
	if (row != swiped_row)	/* still sliding in/out under the finger */
		lv_event_stop_processing(e);
}

lv_obj_t *ui_row_swipe(lv_obj_t *row, const char *glyph, const char *label, uint32_t color, lv_event_cb_t cb, void *ud)
{
	int base = lv_obj_get_style_pad_right(row, 0), w = SWIPE_W - 28;
	lv_obj_remove_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* horizontal swipes belong to the row */
	row_on(row, LV_EVENT_PRESSED);
	row_on(row, LV_EVENT_GESTURE);
	row_on(row, LV_EVENT_CLICKED | LV_EVENT_PREPROCESS);
	row_on(row, LV_EVENT_DELETE);
	lv_obj_t *b = box(row);
	lv_obj_add_flag(b, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_USER_2 | LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_user_data(b, (void *)(intptr_t)base);
	lv_obj_set_size(b, w, LV_PCT(100));
	lv_obj_align(b, LV_ALIGN_RIGHT_MID, base + 14 + w, 0);	/* 14 px past the row's edge */
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, C(color), 0);
	lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
	lv_obj_set_style_color_filter_opa(b, LV_OPA_30, LV_STATE_PRESSED);
	lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(b, 2, 0);
	if (glyph)
		ui_glyph_fit(text(b, glyph, font(F_MONO, 32), on(color)));
	text(b, label, font(F_TEXT_SEMI, 24), on(color));
	lv_obj_add_event_cb(b, pill_click, LV_EVENT_CLICKED | LV_EVENT_PREPROCESS, NULL);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	return b;
}

lv_obj_t *ui_row_switch(lv_obj_t *row, bool on_, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *sw = ui_switch(row, on_);
	if (cb)
		lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, ud);
	return sw;
}

lv_obj_t *ui_row_check(lv_obj_t *row, bool on_)
{
	lv_obj_t *c = text(row, G_CHECK, font(F_MONO, 32), pal.tint);
	ui_glyph_fit(c);
	if (!on_)
		lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);
	return c;
}

#define KNOB_OUT 26	/* ui_slider: the knob's radius (12 px track + 2 × 20 px knob padding) */

lv_obj_t *ui_row_slider(lv_obj_t *sec, const char *lo, const char *hi, int min, int max, int val, lv_event_cb_t cb,
			void *ud)
{
	lv_obj_t *r = ui_row(sec, NULL, 0, "");
	lv_obj_t *col = lv_obj_get_user_data(r);
	lv_obj_delete(col);
	lv_obj_set_height(r, ROW_H);	/* fixed: no text column to centre the track around */
	/* the knob is centred on the track's end (it hangs KNOB_OUT past it): keep it off the glyphs, as iOS does
	 * (a gap, not a margin: flex grow doesn't count the grown item's margins) */
	lv_obj_set_style_pad_column(r, ROW_GAP + KNOB_OUT, 0);
	if (lo)
		ui_glyph_fit(text(r, lo, font(F_MONO, 30), pal.label2));
	lv_obj_t *s = ui_slider(r, 10, min, max, val);
	lv_obj_set_flex_grow(s, 1);
	if (hi)
		ui_glyph_fit(text(r, hi, font(F_MONO, 40), pal.label2));
	if (cb)
		lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, ud);
	return s;
}

/* ---------- controls ---------- */

lv_obj_t *ui_switch(lv_obj_t *parent, bool on_)
{
	lv_obj_t *sw = lv_switch_create(parent);
	lv_obj_set_size(sw, 112, 60);
	lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(sw, C(pal.fill), 0);
	lv_obj_set_style_bg_color(sw, C(pal.green), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(sw, C(0xffffff), LV_PART_KNOB);
	lv_obj_set_style_pad_all(sw, -5, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(sw, 10, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(sw, LV_OPA_30, LV_PART_KNOB);
	lv_obj_set_style_anim_duration(sw, MOTION_SMALL, 0);
	if (on_)
		lv_obj_add_state(sw, LV_STATE_CHECKED);
	return sw;
}

lv_obj_t *ui_slider(lv_obj_t *parent, int w, int min, int max, int val)
{
	lv_obj_t *s = lv_slider_create(parent);
	lv_obj_set_size(s, w, 12);
	lv_slider_set_range(s, min, max);
	lv_slider_set_value(s, val, LV_ANIM_OFF);
	lv_obj_set_style_bg_color(s, C(pal.fill), 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(s, C(pal.tint), LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(s, C(0xffffff), LV_PART_KNOB);
	lv_obj_set_style_pad_all(s, 20, LV_PART_KNOB);
	lv_obj_set_style_shadow_width(s, 14, LV_PART_KNOB);
	lv_obj_set_style_shadow_opa(s, LV_OPA_30, LV_PART_KNOB);
	lv_obj_set_ext_click_area(s, 30);
	return s;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *label, bool filled)
{
	lv_obj_t *b = box(parent);
	lv_obj_set_size(b, LV_SIZE_CONTENT, 96);
	lv_obj_set_style_min_width(b, 200, 0);
	lv_obj_set_style_pad_hor(b, 44, 0);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, C(filled ? pal.tint : pal.fill), 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
	lv_obj_set_style_color_filter_opa(b, LV_OPA_30, LV_STATE_PRESSED);
	lv_obj_t *l = text(b, label, font(F_TEXT_SEMI, 34), filled ? pal.on_tint : pal.tint);
	lv_obj_center(l);
	return b;
}

lv_obj_t *ui_icon_button(lv_obj_t *parent, const char *glyph, int size, uint32_t bg, uint32_t fg)
{
	lv_obj_t *b = box(parent);
	lv_obj_set_size(b, size, size);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, C(bg), 0);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
	lv_obj_set_style_color_filter_opa(b, LV_OPA_30, LV_STATE_PRESSED);
	lv_obj_center(ui_icon(b, glyph, font(F_MONO, size * 4 / 10), fg));	/* another font or glyph later: ui_glyph_fit / ui_icon_set */
	return b;
}

/* segmented control: capsule track, sliding thumb; selection in user_data */
static void seg_thumb_x(void *o, int32_t v) { lv_obj_set_x(o, v); }

static void seg_click(lv_event_t *e)
{
	lv_obj_t *seg = lv_event_get_current_target(e);
	lv_point_t p;
	lv_indev_get_point(lv_indev_active(), &p);
	lv_area_t a;
	lv_obj_get_coords(seg, &a);
	int n = lv_obj_get_child_count(seg) - 1;
	int w = lv_area_get_width(&a) - 8;
	int i = (p.x - a.x1 - 4) * n / w;
	i = i < 0 ? 0 : i >= n ? n - 1 : i;
	if (i == (intptr_t)lv_obj_get_user_data(seg))
		return;
	lv_obj_set_user_data(seg, (void *)(intptr_t)i);
	lv_obj_t *thumb = lv_obj_get_child(seg, 0);
	anim(thumb, seg_thumb_x, lv_obj_get_x(thumb), i * w / n, MOTION_SMALL, NULL);
	for (int k = 0; k < n; k++)
		lv_obj_set_style_text_font(lv_obj_get_child(seg, k + 1), font(k == i ? F_TEXT_SEMI : F_TEXT_MEDIUM, 28), 0);
	ui_haptic_tap();
	lv_obj_send_event(seg, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t *ui_segmented(lv_obj_t *parent, int w, const char **items, int n, int sel)
{
	lv_obj_t *seg = box(parent);
	lv_obj_set_size(seg, w, 68);
	lv_obj_set_style_radius(seg, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(seg, C(pal.fill), 0);
	lv_obj_set_style_pad_all(seg, 4, 0);
	lv_obj_add_flag(seg, LV_OBJ_FLAG_CLICKABLE);
	int iw = (w - 8) / n;
	lv_obj_t *thumb = box(seg);
	lv_obj_set_size(thumb, iw, 60);
	lv_obj_set_pos(thumb, sel * iw, 0);
	lv_obj_set_style_radius(thumb, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(thumb, C(th.dark ? mix(pal.fill, th.fg, 28) : 0xffffff), 0);
	lv_obj_set_style_shadow_width(thumb, 8, 0);
	lv_obj_set_style_shadow_opa(thumb, LV_OPA_20, 0);
	for (int i = 0; i < n; i++) {
		lv_obj_t *l = text(seg, items[i], font(i == sel ? F_TEXT_SEMI : F_TEXT_MEDIUM, 28), pal.label);
		lv_obj_set_width(l, iw);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(l, LV_ALIGN_LEFT_MID, i * iw, 0);
	}
	lv_obj_set_user_data(seg, (void *)(intptr_t)sel);
	lv_obj_add_event_cb(seg, seg_click, LV_EVENT_CLICKED, NULL);
	return seg;
}

int ui_segmented_get(lv_obj_t *seg) { return (intptr_t)lv_obj_get_user_data(seg); }

/* floating tab bar (iOS 26 shape, solid fill instead of glass) */
static void tab_style(lv_obj_t *tb, int sel)
{
	int n = lv_obj_get_child_count(tb) - 1;
	for (int i = 0; i < n; i++) {
		lv_obj_t *it = lv_obj_get_child(tb, i + 1);
		uint32_t c = i == sel ? pal.tint : pal.label;
		lv_obj_set_style_text_color(lv_obj_get_child(it, 0), C(c), 0);
		lv_obj_set_style_text_color(lv_obj_get_child(it, 1), C(c), 0);
	}
}

static void tab_click(lv_event_t *e)
{
	lv_obj_t *it = lv_event_get_current_target(e);
	lv_obj_t *tb = lv_obj_get_parent(it);
	int i = lv_obj_get_index(it) - 1;
	if (i == (intptr_t)lv_obj_get_user_data(tb))
		return;
	lv_obj_set_user_data(tb, (void *)(intptr_t)i);
	lv_obj_t *pill = lv_obj_get_child(tb, 0);
	anim(pill, seg_thumb_x, lv_obj_get_x(pill), lv_obj_get_x(it), MOTION_SMALL, NULL);
	tab_style(tb, i);
	ui_haptic_tap();
	lv_obj_send_event(tb, LV_EVENT_VALUE_CHANGED, NULL);
}

lv_obj_t *ui_tabbar(lv_obj_t *root, const char **glyphs, const char **labels, int n, int sel)
{
	int w = SCR_W - 80, h = 124, iw = (w - 16) / n;
	lv_obj_t *tb = box(root);
	lv_obj_set_size(tb, w, h);
	lv_obj_align(tb, LV_ALIGN_BOTTOM_MID, 0, -BOTTOM_PAD - 6);
	lv_obj_set_style_radius(tb, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(tb, C(th.dark ? mix(pal.cell, th.fg, 4) : 0xffffff), 0);
	lv_obj_set_style_border_width(tb, 1, 0);
	lv_obj_set_style_border_color(tb, C(pal.separator), 0);
	lv_obj_set_style_shadow_width(tb, 40, 0);
	lv_obj_set_style_shadow_opa(tb, LV_OPA_40, 0);
	lv_obj_set_style_shadow_color(tb, C(0x000000), 0);
	lv_obj_set_style_pad_all(tb, 8, 0);
	lv_obj_t *pill = box(tb);
	lv_obj_set_size(pill, iw, h - 16);
	lv_obj_set_pos(pill, sel * iw, 0);
	lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(pill, C(pal.fill), 0);
	for (int i = 0; i < n; i++) {
		lv_obj_t *it = box(tb);
		lv_obj_set_size(it, iw, h - 16);
		lv_obj_set_pos(it, i * iw, 0);
		lv_obj_add_flag(it, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(it, tab_click, LV_EVENT_CLICKED, NULL);
		lv_obj_t *g = ui_icon_line(it, glyphs[i], font(F_MONO, 44), pal.label);	/* one centre line for every tab */
		lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 10);
		lv_obj_t *l = text(it, labels[i], font(F_TEXT_SEMI, 21), pal.label);
		lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
	}
	lv_obj_set_user_data(tb, (void *)(intptr_t)sel);
	tab_style(tb, sel);
	return tb;
}

int ui_tabbar_get(lv_obj_t *tb) { return (intptr_t)lv_obj_get_user_data(tb); }

/* ---------- text fields ---------- */

static void field_click(lv_event_t *e)
{
	lv_obj_t *ta = lv_event_get_user_data(e);
	kbd_attach(ta, KBD_TEXT);
}

static lv_obj_t *field(lv_obj_t *parent, int w, const char *placeholder, bool search)
{
	lv_obj_t *wrap = box(parent);
	lv_obj_set_size(wrap, w, 76);
	lv_obj_set_style_radius(wrap, search ? LV_RADIUS_CIRCLE : 20, 0);
	lv_obj_set_style_bg_opa(wrap, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(wrap, C(pal.fill), 0);
	lv_obj_set_style_pad_hor(wrap, 26, 0);
	lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(wrap, 12, 0);
	lv_obj_add_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
	if (search)	/* fit: the gap to the text is measured from the glass itself */
		ui_glyph_fit(text(wrap, G_SEARCH, font(F_MONO, 30), pal.label2));
	lv_obj_t *ta = lv_textarea_create(wrap);
	lv_obj_remove_style_all(ta);
	lv_obj_set_flex_grow(ta, 1);
	lv_obj_set_height(ta, LV_SIZE_CONTENT);
	lv_textarea_set_one_line(ta, true);
	lv_obj_remove_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);	/* focused = keyboard attached (kbd.c), not pressed */
	lv_textarea_set_placeholder_text(ta, placeholder);
	lv_obj_set_style_text_font(ta, font(F_TEXT, 34), 0);
	lv_obj_set_style_text_color(ta, C(pal.label), 0);
	lv_obj_set_style_text_color(ta, C(pal.label2), LV_PART_TEXTAREA_PLACEHOLDER);
	lv_obj_set_style_border_color(ta, C(pal.tint), LV_PART_CURSOR | LV_STATE_FOCUSED);
	lv_obj_set_style_border_width(ta, 3, LV_PART_CURSOR | LV_STATE_FOCUSED);
	lv_obj_set_style_border_side(ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
	lv_obj_set_style_anim_duration(ta, 500, LV_PART_CURSOR | LV_STATE_FOCUSED);	/* blinks only with the keyboard up */
	lv_obj_add_event_cb(wrap, field_click, LV_EVENT_CLICKED, ta);
	lv_obj_add_event_cb(ta, field_click, LV_EVENT_CLICKED, ta);
	return ta;
}

lv_obj_t *ui_search(lv_obj_t *parent, int w, const char *placeholder) { return field(parent, w, placeholder, true); }
lv_obj_t *ui_text_field(lv_obj_t *parent, int w, const char *placeholder) { return field(parent, w, placeholder, false); }

/* ---------- popover menus (iOS 26 context / pull-down menu: rounded card, icons on the right) ---------- */

#define MENU_W 480
static lv_obj_t *menu_dim;
static void (*menu_fn)(int id);

void ui_menu_close(void)
{
	if (!menu_dim)
		return;
	lv_obj_t *d = menu_dim;
	menu_dim = NULL;
	lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
	lv_obj_delete_async(d);
}

bool ui_menu_shown(void) { return menu_dim != NULL; }

static void menu_outside(lv_event_t *e)
{
	(void)e;
	ui_menu_close();
}

static void menu_pick(lv_event_t *e)
{
	int id = (intptr_t)lv_event_get_user_data(e);
	void (*fn)(int) = menu_fn;
	ui_menu_close();
	ui_haptic_tap();
	if (fn)
		fn(id);
}

void ui_menu(const ui_menu_item_t *it, int n, int right, int y_below, int y_above, void (*fn)(int id))
{
	ui_menu_close();
	menu_fn = fn;
	menu_dim = box(shell_overlay());
	lv_obj_set_size(menu_dim, SCR_W, SCR_H);
	lv_obj_add_flag(menu_dim, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(menu_dim, menu_outside, LV_EVENT_PRESSED, NULL);

	uint32_t bg = th.dark ? mix(pal.cell, th.fg, 7) : 0xffffff;
	lv_obj_t *card = box(menu_dim);
	lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);	/* taps between items keep it open */
	lv_obj_set_size(card, MENU_W, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(card, 34, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, C(bg), 0);
	lv_obj_set_style_border_width(card, 1, 0);
	lv_obj_set_style_border_color(card, C(pal.separator), 0);
	lv_obj_set_style_shadow_width(card, 70, 0);
	lv_obj_set_style_shadow_opa(card, LV_OPA_50, 0);
	lv_obj_set_style_shadow_color(card, C(0x000000), 0);
	lv_obj_set_style_clip_corner(card, true, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

	bool checks = false;
	for (int i = 0; i < n; i++)
		checks |= (it[i].flags & MI_CHECK) != 0;
	bool after_break = true;
	for (int i = 0; i < n; i++) {
		if ((it[i].flags & MI_GROUP) && i > 0) {
			lv_obj_t *g = box(card);
			lv_obj_set_size(g, LV_PCT(100), 12);
			lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
			lv_obj_set_style_bg_color(g, C(th.dark ? mix(bg, 0x000000, 45) : mix(bg, th.fg, 9)), 0);
			after_break = true;
		}
		if (it[i].flags & MI_HEAD) {
			lv_obj_t *h = text(card, it[i].label, font(F_TEXT, 26), pal.label2);
			lv_obj_set_width(h, LV_PCT(100));
			lv_obj_set_style_pad_hor(h, 30, 0);
			lv_obj_set_style_pad_top(h, 20, 0);
			lv_obj_set_style_pad_bottom(h, 6, 0);
			after_break = true;
			continue;
		}
		uint32_t fg = it[i].flags & MI_RED ? pal.red : pal.label;
		lv_obj_t *r = box(card);
		lv_obj_set_size(r, LV_PCT(100), 88);
		lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(r, C(mix(bg, th.fg, 12)), LV_STATE_PRESSED);
		if (!after_break) {
			lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
			lv_obj_set_style_border_width(r, 1, 0);
			lv_obj_set_style_border_color(r, C(pal.separator), 0);
		}
		after_break = false;
		int x = 30;
		if (checks) {
			lv_obj_t *c = text(r, G_CHECK, font(F_MONO, 28), pal.label);
			lv_obj_align(c, LV_ALIGN_LEFT_MID, 26, 0);
			ui_glyph_fit(c);
			if (!(it[i].flags & MI_ON))
				lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
			x = 74;
		}
		lv_obj_t *l = text(r, it[i].label, font(F_TEXT, 34), fg);
		lv_obj_set_width(l, MENU_W - x - 90);
		one_line(l);
		lv_obj_align(l, LV_ALIGN_LEFT_MID, x, 0);
		if (it[i].glyph) {
			lv_obj_t *g = text(r, it[i].glyph, font(F_MONO, 32), fg);
			lv_obj_set_width(g, 50);
			lv_obj_set_style_text_align(g, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_align(g, LV_ALIGN_RIGHT_MID, -22, 0);
			ui_glyph_fit(g);	/* one centred column, whatever each icon's width */
		}
		lv_obj_add_event_cb(r, menu_pick, LV_EVENT_CLICKED, (void *)(intptr_t)it[i].id);
	}
	lv_obj_update_layout(card);
	int h = lv_obj_get_height(card), y = y_below;
	if (y + h > SCR_H - 40)
		y = y_above - h;
	if (y < STATUS_H + 8)
		y = STATUS_H + 8;
	int x = right - MENU_W;
	lv_obj_set_pos(card, x < 16 ? 16 : x, y);
	lv_obj_set_style_opa(card, LV_OPA_TRANSP, 0);
	motion_anim(card, set_opa, LV_OPA_TRANSP, LV_OPA_COVER, MOTION_SMALL, NULL);
}

void ui_row_menu(lv_obj_t *row, const ui_menu_item_t *it, int n, void (*fn)(int id))
{
	lv_area_t a;
	lv_obj_get_coords(row, &a);
	ui_menu(it, n, SCR_W - SIDE, a.y2 + 10, a.y1 - 10, fn);
}

/* ---------- alerts ----------
 * One at a time, like iOS: one raised while another is up waits for it (a few are kept); one
 * raised from an alert's own button fades in over it as it goes. */

#define ALERT_QUEUE 4
#define ALERT_BTNS 4
static struct { void (*cb)(int, void *); void *ud; lv_obj_t *dim; } alert;
static struct alert_req {
	char title[96], msg[320], btn[ALERT_BTNS][40];
	int n;
	void (*cb)(int, void *);
	void *ud;
} alert_q[ALERT_QUEUE];
static int alert_nq;

static void alert_show(const char *title, const char *msg, const char **buttons, int n, void (*cb)(int, void *), void *ud);

static void alert_gone(lv_anim_t *a) { lv_obj_delete(a->var); }

static void alert_btn(lv_event_t *e)
{
	lv_obj_t *dim = lv_event_get_current_target(e);
	while (lv_obj_get_parent(dim) && lv_obj_get_parent(dim) != shell_overlay())
		dim = lv_obj_get_parent(dim);
	if (dim != alert.dim)
		return;	/* already on its way out */
	int idx = (intptr_t)lv_event_get_user_data(e);
	void (*cb)(int, void *) = alert.cb;
	void *ud = alert.ud;
	alert.dim = NULL;
	alert.cb = NULL;
	lv_obj_remove_flag(dim, LV_OBJ_FLAG_CLICKABLE);
	anim(dim, set_opa, LV_OPA_COVER, LV_OPA_TRANSP, MOTION_SMALL * 3 / 4, alert_gone);
	if (cb)
		cb(idx, ud);	/* may raise the next alert: it comes in over this one */
	if (!alert.dim && alert_nq) {
		struct alert_req r = alert_q[0];
		memmove(alert_q, alert_q + 1, --alert_nq * sizeof(alert_q[0]));
		const char *b[ALERT_BTNS];
		for (int i = 0; i < r.n; i++)
			b[i] = r.btn[i];
		alert_show(r.title, r.msg[0] ? r.msg : NULL, b, r.n, r.cb, r.ud);
	}
}

void ui_alert(const char *title, const char *msg, const char **buttons, int n, void (*cb)(int, void *), void *ud)
{
	if (!alert.dim) {
		alert_show(title, msg, buttons, n, cb, ud);
		return;
	}
	if (alert_nq == ALERT_QUEUE) {
		fprintf(stderr, "orchard: too many alerts, dropped \"%s\"\n", title);
		return;
	}
	struct alert_req *r = &alert_q[alert_nq++];
	snprintf(r->title, sizeof(r->title), "%s", title);
	snprintf(r->msg, sizeof(r->msg), "%s", msg ? msg : "");
	r->n = n < ALERT_BTNS ? n : ALERT_BTNS;
	for (int i = 0; i < r->n; i++)
		snprintf(r->btn[i], sizeof(r->btn[i]), "%s", buttons[i]);
	r->cb = cb;
	r->ud = ud;
}

static void alert_show(const char *title, const char *msg, const char **buttons, int n, void (*cb)(int, void *), void *ud)
{
	alert.cb = cb;
	alert.ud = ud;
	alert.dim = box(shell_overlay());
	lv_obj_set_size(alert.dim, SCR_W, SCR_H);
	lv_obj_set_style_bg_opa(alert.dim, LV_OPA_50, 0);
	lv_obj_set_style_bg_color(alert.dim, C(0x000000), 0);
	lv_obj_add_flag(alert.dim, LV_OBJ_FLAG_CLICKABLE);	/* swallow taps behind the alert */

	lv_obj_t *card = box(alert.dim);
	lv_obj_set_width(card, 600);
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_center(card);
	lv_obj_set_style_radius(card, 64, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(card, C(th.dark ? mix(pal.cell, th.fg, 6) : 0xffffff), 0);
	lv_obj_set_style_pad_all(card, 40, 0);
	lv_obj_set_style_pad_row(card, 16, 0);
	lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *t = text(card, title, font(F_TEXT_SEMI, 36), pal.label);
	lv_obj_set_width(t, LV_PCT(100));
	lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
	if (msg) {
		lv_obj_t *m = text(card, msg, font(F_TEXT, 30), pal.label2);
		lv_obj_set_width(m, LV_PCT(100));
		lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_line_space(m, 6, 0);
	}
	lv_obj_t *row = box(card);
	lv_obj_set_width(row, LV_PCT(100));
	lv_obj_set_height(row, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_top(row, 20, 0);
	lv_obj_set_style_pad_gap(row, 16, 0);
	lv_obj_set_flex_flow(row, n == 2 ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
	for (int i = 0; i < n; i++) {
		const char *label = buttons[i];
		bool destructive = label[0] == '!', primary = i == n - 1;
		if (destructive)
			label++;
		lv_obj_t *b = box(row);
		lv_obj_set_height(b, 92);
		if (n == 2)
			lv_obj_set_flex_grow(b, 1);
		else
			lv_obj_set_width(b, LV_PCT(100));
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		uint32_t bg = destructive ? pal.red : primary ? pal.tint : pal.fill;
		lv_obj_set_style_bg_color(b, C(bg), 0);
		lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
		lv_obj_set_style_color_filter_opa(b, LV_OPA_30, LV_STATE_PRESSED);
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(b, alert_btn, LV_EVENT_CLICKED, (void *)(intptr_t)i);
		lv_obj_t *l = text(b, label, font(F_TEXT_SEMI, 32), destructive || primary ? on(bg) : pal.label);
		lv_obj_center(l);
	}
	anim(alert.dim, set_opa, LV_OPA_TRANSP, LV_OPA_COVER, MOTION_SMALL, NULL);
	ui_haptic_tap();
}

/* the page under a sheet or an alert stays put: no back swipe */
bool nav_locked(void) { return nav_held || alert.dim; }

/* ---------- material ---------- */

void ui_material(lv_obj_t *obj)
{
	lv_obj_update_layout(obj);
	lv_area_t a;
	lv_obj_get_coords(obj, &a);
	lv_obj_t *img = lv_image_create(obj);
	lv_image_set_src(img, theme_wallpaper_blur());
	lv_obj_add_flag(img, LV_OBJ_FLAG_FLOATING);
	lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_pos(img, -a.x1 - lv_obj_get_style_pad_left(obj, 0) - lv_obj_get_style_border_width(obj, 0),
		       -a.y1 - lv_obj_get_style_pad_top(obj, 0) - lv_obj_get_style_border_width(obj, 0));
	lv_obj_move_to_index(img, 0);
	lv_obj_set_style_clip_corner(obj, true, 0);
}

/* an LVGL spinner ("Loading…"): every one goes through here. Made while the lock screen is up (a slow answer lands
 * meanwhile) it starts stopped — it would draw 60 frames a second nobody sees; lock.c starts every spinner again on
 * unlock. ms / sweep (lv_spinner_set_anim_params): 0 = LVGL's own. */
lv_obj_t *ui_spinner(lv_obj_t *parent, uint32_t ms, uint32_t sweep)
{
	lv_obj_t *s = lv_spinner_create(parent);
	if (ms)
		lv_spinner_set_anim_params(s, ms, sweep);
	if (lock_visible() && lv_obj_get_screen(s) == lv_screen_active())	/* below the lock (not on a layer above it) */
		lv_anim_delete(s, NULL);
	return s;
}
