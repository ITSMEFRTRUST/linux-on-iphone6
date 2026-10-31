/* Calculator in the iOS layout: AC ± % ÷ / digits / orange operators, chained evaluation. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orchard.h"

#define BTN 150
#define GAP 30

static lv_obj_t *disp, *op_btn[4];
static char entry[32];
static double acc;
static int pending;	/* 0 none, else '+', '-', '*', '/' */
static bool fresh, err;

enum { K_FN, K_NUM, K_OP };	/* key kinds → theme colours */
static const struct { const char *t; int kind, span; } keys[] = {
	{ "AC", K_FN, 1 }, { "\xc2\xb1", K_FN, 1 }, { "%", K_FN, 1 }, { "\xc3\xb7", K_OP, 1 },
	{ "7", K_NUM, 1 }, { "8", K_NUM, 1 }, { "9", K_NUM, 1 }, { "\xc3\x97", K_OP, 1 },
	{ "4", K_NUM, 1 }, { "5", K_NUM, 1 }, { "6", K_NUM, 1 }, { "\xe2\x88\x92", K_OP, 1 },
	{ "1", K_NUM, 1 }, { "2", K_NUM, 1 }, { "3", K_NUM, 1 }, { "+", K_OP, 1 },
	{ "0", K_NUM, 2 }, { ".", K_NUM, 1 }, { "=", K_OP, 1 },
};

static void show(double v)
{
	if (!isfinite(v)) {
		err = true;
		lv_label_set_text(disp, "Error");
		return;
	}
	snprintf(entry, sizeof(entry), "%.9g", v);
	lv_label_set_text(disp, entry);
}

static double cur(void) { return entry[0] ? strtod(entry, NULL) : 0; }

static double apply(double a, int op, double b)
{
	switch (op) {
	case '+': return a + b;
	case '-': return a - b;
	case '*': return a * b;
	case '/': return a / b;
	}
	return b;
}

/* key colours (iOS 26: dark grey digits, lighter grey functions, orange operators) */
static uint32_t k_num(void) { return th.dark ? mix(pal.cell, th.fg, 12) : pal.fill; }
static uint32_t k_fn(void) { return th.dark ? mix(pal.cell, th.fg, 32) : mix(pal.fill, th.fg, 14); }
static uint32_t k_op(void) { return pal.orange; }
static uint32_t ink_on(uint32_t bg)
{
	int l = ((bg >> 16 & 0xff) * 299 + (bg >> 8 & 0xff) * 587 + (bg & 0xff) * 114) / 1000;
	return l > 165 ? 0x1c1c1e : 0xffffff;
}

static void highlight(int op)
{
	static const int ops[] = { '/', '*', '-', '+' };
	for (int i = 0; i < 4; i++) {
		bool on = ops[i] == op;
		lv_obj_set_style_bg_color(op_btn[i], C(on ? 0xffffff : k_op()), 0);
		lv_obj_set_style_text_color(lv_obj_get_child(op_btn[i], 0), C(on ? k_op() : ink_on(k_op())), 0);
	}
}

static void press(lv_event_t *e)
{
	const char *t = lv_event_get_user_data(e);
	sys_haptic(8);
	if (err && strcmp(t, "AC"))
		return;
	if (!strcmp(t, "AC")) {
		entry[0] = 0;
		acc = 0;
		pending = 0;
		err = false;
		lv_label_set_text(disp, "0");
		highlight(0);
		return;
	}
	int op = !strcmp(t, "+") ? '+' : !strcmp(t, "\xe2\x88\x92") ? '-' : !strcmp(t, "\xc3\x97") ? '*' :
		 !strcmp(t, "\xc3\xb7") ? '/' : 0;
	if (op || !strcmp(t, "=")) {
		if (!fresh || !pending)
			acc = pending ? apply(acc, pending, cur()) : cur();
		pending = op;
		fresh = true;
		show(acc);
		highlight(op);
		return;
	}
	if (!strcmp(t, "\xc2\xb1") || !strcmp(t, "%")) {
		show(t[0] == '%' ? cur() / 100 : -cur());
		return;
	}
	/* digit or dot */
	if (fresh) {
		entry[0] = 0;
		fresh = false;
		highlight(0);
	}
	if (t[0] == '.' && strchr(entry, '.'))
		return;
	if (strlen(entry) >= 9)
		return;
	if (!strcmp(entry, "0") && t[0] != '.')
		entry[0] = 0;
	if (!entry[0] && t[0] == '.')
		strcpy(entry, "0");
	strcat(entry, t);
	lv_label_set_text(disp, entry);
}

static void app_open(lv_obj_t *root)
{
	int top = SCR_H - BOTTOM_PAD - 5 * (BTN + GAP);
	disp = text(root, entry[0] ? entry : "0", font(F_DISPLAY_LIGHT, 150), pal.label);
	lv_obj_set_width(disp, SCR_W - 2 * GAP - 20);
	lv_obj_set_style_text_align(disp, LV_TEXT_ALIGN_RIGHT, 0);
	lv_label_set_long_mode(disp, LV_LABEL_LONG_MODE_CLIP);
	lv_obj_set_pos(disp, GAP, top - 190);

	int col = 0, r = 0, opi = 0;
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		lv_obj_t *b = box(root);
		int w = keys[i].span * BTN + (keys[i].span - 1) * GAP;
		lv_obj_set_pos(b, GAP + col * (BTN + GAP), top + r * (BTN + GAP));
		lv_obj_set_size(b, w, BTN);
		lv_obj_set_style_radius(b, BTN / 2, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		uint32_t bg = keys[i].kind == K_OP ? k_op() : keys[i].kind == K_FN ? k_fn() : k_num();
		lv_obj_set_style_bg_color(b, C(bg), 0);
		lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
		lv_obj_set_style_color_filter_opa(b, LV_OPA_20, LV_STATE_PRESSED);
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(b, press, LV_EVENT_CLICKED, (void *)keys[i].t);
		lv_obj_t *l = text(b, keys[i].t, font(keys[i].kind == K_NUM ? F_TEXT : F_TEXT_MEDIUM, 64),
				   keys[i].kind == K_NUM ? pal.label : ink_on(bg));
		if (keys[i].span == 2)
			lv_obj_align(l, LV_ALIGN_LEFT_MID, 56, 0);
		else
			lv_obj_center(l);
		if (keys[i].kind == K_OP && opi < 4)	/* ÷ × − + (not =): they light up while pending */
			op_btn[opi++] = b;
		col += keys[i].span;
		if (col == 4) {
			col = 0;
			r++;
		}
	}
	highlight(fresh ? pending : 0);
}

static void app_close(void)
{
	disp = NULL;
	memset(op_btn, 0, sizeof(op_btn));
}

const app_t app_calc = {
	.name = "Calculator",
	.glyph = "\xef\x87\xac",
	.icon = IC_WARM,
	.open = app_open,
	.close = app_close,
};
