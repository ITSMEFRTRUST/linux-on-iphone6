/* Keyboard, iOS layout. Built for accuracy on a 4.7" screen:
 *  - every finger is tracked (rollover): a key is committed when its finger lifts, or as soon
 *    as the next finger lands, so fast two-thumb typing never drops letters;
 *  - no dead zones: a tap anywhere on the keyboard goes to the nearest key;
 *  - predictive touch targets (the "keyboard helper" iOS has): near a key border an English /
 *    shell-command word model picks the likelier letter, e.g. "th|" + a tap between e and r → e.
 *    Taps in the middle of a key always win;
 *  - letter pop-ups, slide to correct before lifting, QuickType suggestions, auto-capitals,
 *    double-space = ". ", caps lock = double-tap shift, backspace/arrow repeat.
 * KBD_TERM swaps the suggestion bar for a terminal row (esc tab ctrl ~ | / - arrows).
 * Secure fields (a textarea in password mode, or kbd_secure()): nothing typed reaches the context —
 * no suggestions, word model, auto-capitals or double-space period, no letter pop-ups. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orchard.h"

#define BAR_H 84	/* suggestion bar / terminal row */
#define ROW_P 104	/* key row pitch */
#define KEY_H 80
#define TOP_PAD 8
#define SIDE 6
#define GAP 12
#define KEYS_H (TOP_PAD + 4 * ROW_P)
#define Y_BIAS 6	/* fingers land slightly below what they aim at */
#define ALPHA 0.13f	/* weight of the word model against touch distance */

#define G_SHIFT "\xf3\xb0\x98\xb6"	/* md-apple_keyboard_shift */
#define G_CAPS "\xf3\xb0\x98\xb2"	/* md-apple_keyboard_caps */
#define G_BKSP "\xf3\xb0\xad\x9c"	/* md-backspace_outline */
#define G_ARROW_L "\xef\x81\xa0"
#define G_ARROW_R "\xef\x81\xa1"
#define G_ARROW_U "\xef\x81\xa2"
#define G_ARROW_D "\xef\x81\xa3"

enum kind { K_CHAR, K_SHIFT, K_BKSP, K_LAYER, K_SPACE, K_ENTER, K_TERM, K_CTRL, K_SUGG };
enum layer { L_ALPHA, L_NUM, L_SYM };

struct key { int16_t x, y, w, h; uint8_t kind; int8_t arg; const char *label; };
#define MAXK 48
static struct key keys[MAXK];
static int nkeys;

static lv_obj_t *kb, *pop, *pop_label;
static bool covered;	/* kbd_cover: the lock screen is over it — kb is HIDDEN meanwhile */
static enum kbd_mode mode;
static enum layer layer;
static const text_client_t *client;
static bool shift, caps, ctrl_latch, ctrl_hw, nocap, secure;
static uint32_t last_shift_ms, last_space_ms;
static const char *ret_label = "return";

static struct finger { int id, key, x, y; bool committed; } fing[10];
static int nfing;
static lv_timer_t *repeat;
static int repeat_key;

static char ctx[160];	/* recent text before the cursor */
static char sugg[3][40];

static uint32_t c_bg, c_key, c_sp, c_fg;

/* ---------- word model ---------- */

extern const char words_en[], words_shell[];
static struct word { const char *s; float wt, wm; } *words;
static int nwords;
static double *cum_t, *cum_m;
static float unigram[26];

static int word_cmp(const void *a, const void *b) { return strcmp(((struct word *)a)->s, ((struct word *)b)->s); }

static void add_words(const char *src, bool shell, int *cap)
{
	int rank = 0;
	for (const char *p = src; *p;) {
		const char *e = p;
		while (*e && *e != '\n')
			e++;
		int len = e - p;
		bool ok = len > 0 && len < 32;
		for (int i = 0; ok && i < len; i++)
			ok = p[i] >= 'a' && p[i] <= 'z';
		if (ok) {
			if (nwords == *cap)
				words = realloc(words, (*cap *= 2) * sizeof(*words));
			char *s = malloc(len + 1);
			memcpy(s, p, len);
			s[len] = 0;
			float w = shell ? 1e6f / 45 : 1e6f / (rank + 30);
			words[nwords++] = (struct word){ s, shell ? 0 : w, shell ? w : w * 0.4f };
			rank++;
		}
		p = *e ? e + 1 : e;
	}
}

static void model_init(void)
{
	/* English letter frequencies, for words the list doesn't know */
	static const float f[26] = { 8.2, 1.5, 2.8, 4.3, 12.7, 2.2, 2.0, 6.1, 7.0, 0.15, 0.77, 4.0, 2.4,
				     6.7, 7.5, 1.9, 0.095, 6.0, 6.3, 9.1, 2.8, 0.98, 2.4, 0.15, 2.0, 0.074 };
	for (int i = 0; i < 26; i++)
		unigram[i] = f[i] / 100;
	int cap = 4096;
	words = malloc(cap * sizeof(*words));
	add_words(words_en, false, &cap);
	add_words(words_shell, true, &cap);
	qsort(words, nwords, sizeof(*words), word_cmp);
	int n = 0;	/* merge duplicates (a word in both lists) */
	for (int i = 0; i < nwords; i++) {
		if (n && !strcmp(words[n - 1].s, words[i].s)) {
			words[n - 1].wt += words[i].wt;
			words[n - 1].wm += words[i].wm;
		} else {
			words[n++] = words[i];
		}
	}
	nwords = n;
	cum_t = malloc((n + 1) * sizeof(double));
	cum_m = malloc((n + 1) * sizeof(double));
	cum_t[0] = cum_m[0] = 0;
	for (int i = 0; i < n; i++) {
		cum_t[i + 1] = cum_t[i] + words[i].wt;
		cum_m[i + 1] = cum_m[i] + words[i].wm;
	}
}

/* first index whose word is >= prefix (comparing the first len chars), or > it when upper */
static int bound(const char *p, int len, bool upper)
{
	int lo = 0, hi = nwords;
	while (lo < hi) {
		int mid = (lo + hi) / 2, c = strncmp(words[mid].s, p, len);
		if (c < 0 || (upper && c == 0))
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static double weight(const char *p, int len)
{
	double *cum = mode == KBD_TERM ? cum_m : cum_t;
	return cum[bound(p, len, true)] - cum[bound(p, len, false)];
}

/* lowercase letters at the end of ctx = the word being typed */
static int current_word(char *out, int max)
{
	int n = strlen(ctx), i = n;
	while (i > 0 && ((ctx[i - 1] >= 'a' && ctx[i - 1] <= 'z') || (ctx[i - 1] >= 'A' && ctx[i - 1] <= 'Z')))
		i--;
	int len = n - i < max - 2 ? n - i : max - 2;
	for (int k = 0; k < len; k++)
		out[k] = ctx[i + k] | 0x20;
	out[len] = 0;
	return len;
}

static float logp(char c)
{
	char w[32];
	int len = current_word(w, sizeof(w));
	float u = unigram[c - 'a'];
	double wp = weight(w, len);
	if (wp <= 0)
		return logf(u);
	w[len] = c;
	double wc = weight(w, len + 1);
	float p = (wc + 0.05 * wp * u) / (wp * 1.05);
	float l = logf(p);
	return l < -8 ? -8 : l;
}

static void update_suggestions(void)
{
	char w[32], orig[40], was[3][40];
	int len = current_word(w, sizeof(w));
	memcpy(was, sugg, sizeof(was));
	memset(sugg, 0, sizeof(sugg));
	if (mode != KBD_TEXT || secure || len == 0 || !prefs_int("kbd_suggest", 1))
		goto out;
	int n = strlen(ctx);
	memcpy(orig, ctx + n - len, len);
	orig[len] = 0;
	int lo = bound(w, len, false), hi = bound(w, len, true), best[2] = { -1, -1 };
	for (int i = lo; i < hi; i++) {
		if ((int)strlen(words[i].s) == len)
			continue;
		if (best[0] < 0 || words[i].wt > words[best[0]].wt) {
			best[1] = best[0];
			best[0] = i;
		} else if (best[1] < 0 || words[i].wt > words[best[1]].wt) {
			best[1] = i;
		}
	}
	snprintf(sugg[0], sizeof(sugg[0]), "\xe2\x80\x9c%s\xe2\x80\x9d", orig);
	for (int k = 0; k < 2; k++) {
		if (best[k] < 0)
			continue;
		snprintf(sugg[k + 1], sizeof(sugg[0]), "%s", words[best[k]].s);
		bool all_caps = len > 1;
		for (int i = 0; i < len; i++)
			all_caps &= orig[i] >= 'A' && orig[i] <= 'Z';
		for (char *s = sugg[k + 1]; *s; s++)
			if (all_caps || (s == sugg[k + 1] && orig[0] >= 'A' && orig[0] <= 'Z'))
				*s &= ~0x20;
	}
out:
	if (kb && memcmp(was, sugg, sizeof(was))) {	/* only the bar, only when it changed (was: the whole keyboard per key) */
		lv_area_t a;
		lv_obj_get_coords(kb, &a);
		a.y2 = a.y1 + BAR_H - 1;
		lv_obj_invalidate_area(kb, &a);
	}
}

/* ---------- layout ---------- */

static const char *rows_alpha[3] = { "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const char *num_rows[3][10] = {
	{ "1", "2", "3", "4", "5", "6", "7", "8", "9", "0" },
	{ "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"" },
	{ ".", ",", "?", "!", "'" },
};
static const char *sym_rows[3][10] = {
	{ "[", "]", "{", "}", "#", "%", "^", "*", "+", "=" },
	{ "_", "\\", "|", "~", "<", ">", "`", "\"", "'", "&" },
	{ ".", ",", "?", "!", "@" },
};
static char letters[26][2], letters_up[26][2];

static void add(float x, int y, float w, int h, int kind, int arg, const char *label)
{
	if (nkeys < MAXK)
		keys[nkeys++] = (struct key){ x + 0.5f, y, w + 0.5f, h, kind, arg, label };
}

static void build(void)
{
	float P = (SCR_W - 2 * SIDE) / 10.0f;
	nkeys = 0;
	if (mode == KBD_TERM) {
		static const char *tl[] = { "esc", "tab", "ctrl", "~", "|", "/", "-", G_ARROW_L, G_ARROW_U, G_ARROW_D, G_ARROW_R };
		static const int targ[] = { TK_ESC, TK_TAB, 0, 0, 0, 0, 0, TK_LEFT, TK_UP, TK_DOWN, TK_RIGHT };
		float u = (SCR_W - 2 * SIDE) / 12.2f;
		float x = SIDE;
		for (int i = 0; i < 11; i++) {
			float w = i < 3 ? u * 1.4f : u;
			int kind = i == 2 ? K_CTRL : (i >= 3 && i <= 6) ? K_CHAR : K_TERM;
			add(x + 4, (BAR_H - 64) / 2 + 4, w - 8, 64, kind, targ[i], tl[i]);
			x += w;
		}
	} else {
		float w = (SCR_W - 2 * SIDE) / 3.0f;
		for (int i = 0; i < 3; i++)
			add(SIDE + i * w, 0, w, BAR_H, K_SUGG, i, NULL);
	}
	int y0 = BAR_H + TOP_PAD + (ROW_P - KEY_H) / 2;
	for (int r = 0; r < 3; r++) {
		int y = y0 + r * ROW_P;
		if (layer == L_ALPHA) {
			const char *s = rows_alpha[r];
			int n = strlen(s);
			float x = r == 0 ? SIDE : r == 1 ? SIDE + P / 2 : SIDE + 1.5f * P;
			if (r == 2)
				add(SIDE + GAP / 2, y, 1.5f * P - GAP - 10, KEY_H, K_SHIFT, 0, NULL);
			for (int i = 0; i < n; i++) {
				int c = s[i] - 'a';
				add(x + i * P + GAP / 2, y, P - GAP, KEY_H, K_CHAR, c, shift || caps ? letters_up[c] : letters[c]);
			}
			if (r == 2)
				add(SCR_W - SIDE - 1.5f * P + GAP / 2 + 10, y, 1.5f * P - GAP - 10, KEY_H, K_BKSP, 0, G_BKSP);
		} else {
			const char *(*rows)[10] = layer == L_NUM ? num_rows : sym_rows;
			if (r < 2) {
				for (int i = 0; i < 10; i++)
					add(SIDE + i * P + GAP / 2, y, P - GAP, KEY_H, K_CHAR, -1, rows[r][i]);
			} else {
				add(SIDE + GAP / 2, y, 1.5f * P - GAP - 10, KEY_H, K_LAYER, layer == L_NUM ? L_SYM : L_NUM,
				    layer == L_NUM ? "#+=" : "123");
				float w = 7 * P / 5;
				for (int i = 0; i < 5; i++)
					add(SIDE + 1.5f * P + i * w + GAP / 2, y, w - GAP, KEY_H, K_CHAR, -1, rows[r][i]);
				add(SCR_W - SIDE - 1.5f * P + GAP / 2 + 10, y, 1.5f * P - GAP - 10, KEY_H, K_BKSP, 0, G_BKSP);
			}
		}
	}
	int y = y0 + 3 * ROW_P;
	add(SIDE + GAP / 2, y, 1.5f * P - GAP, KEY_H, K_LAYER, layer == L_ALPHA ? L_NUM : L_ALPHA,
	    layer == L_ALPHA ? "123" : "ABC");
	add(SIDE + 1.5f * P + GAP / 2, y, 6 * P - GAP, KEY_H, K_SPACE, 0, "space");
	add(SIDE + 7.5f * P + GAP / 2, y, 2.5f * P - GAP, KEY_H, K_ENTER, 0, ret_label);
}

/* ---------- hit testing ---------- */

static int nearest(int x, int y, int from, int to)
{
	int best = -1;
	long bestd = 0;
	for (int i = from; i < to; i++) {
		struct key *k = &keys[i];
		int dx = x < k->x ? k->x - x : x > k->x + k->w ? x - k->x - k->w : 0;
		int dy = y < k->y ? k->y - y : y > k->y + k->h ? y - k->y - k->h : 0;
		long d = (long)dx * dx + (long)dy * dy;
		if (best < 0 || d < bestd) {
			best = i;
			bestd = d;
		}
	}
	return best;
}

static int first_row_key(void) { return mode == KBD_TERM ? 11 : 3; }

static int hit(int x, int y, bool predict)
{
	if (y < BAR_H)	/* the bar row is its own strip */
		return nearest(x, y, 0, first_row_key());
	y += Y_BIAS;
	int b = nearest(x, y, first_row_key(), nkeys);
	if (!predict || secure || b < 0 || keys[b].kind != K_CHAR || layer != L_ALPHA || !prefs_int("kbd_predict", 1))
		return b;
	struct key *k = &keys[b];
	float cx = k->x + k->w / 2.f, cy = k->y + k->h / 2.f, P = (SCR_W - 2 * SIDE) / 10.0f;
	if (fabsf(x - cx) < k->w * 0.28f && fabsf(y - cy) < k->h * 0.3f)
		return b;	/* clearly inside: the finger wins */
	int best = b;
	float bestc = 1e9f;
	for (int i = first_row_key(); i < nkeys; i++) {
		struct key *c = &keys[i];
		if (c->kind != K_CHAR || c->arg < 0)
			continue;
		float nx = (x - (c->x + c->w / 2.f)) / P, ny = (y - (c->y + c->h / 2.f)) / ROW_P;
		if (fabsf(nx) > 1.2f || fabsf(ny) > 1.2f)
			continue;
		float cost = nx * nx + ny * ny - ALPHA * logp('a' + c->arg);
		if (cost < bestc) {
			bestc = cost;
			best = i;
		}
	}
	return best;
}

/* ---------- drawing ---------- */

static bool key_down(int i)
{
	for (int f = 0; f < nfing; f++)
		if (fing[f].key == i)
			return true;
	return false;
}

static void draw_text_c(lv_layer_t *layer_, const char *s, lv_font_t *f, uint32_t color, int x, int y, int w, int h)
{
	lv_draw_label_dsc_t d;
	lv_draw_label_dsc_init(&d);
	d.text = s;
	d.font = f;
	d.color = C(color);
	d.align = LV_TEXT_ALIGN_CENTER;
	int lh = lv_font_get_line_height(f);
	lv_area_t a = { x, y + (h - lh) / 2, x + w - 1, y + (h - lh) / 2 + lh - 1 };
	lv_draw_label(layer_, &d, &a);
}

static void rect(lv_layer_t *layer_, int x, int y, int w, int h, int r, uint32_t color, lv_opa_t opa)
{
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_color = C(color);
	d.bg_opa = opa;
	d.radius = r;
	lv_area_t a = { x, y, x + w - 1, y + h - 1 };
	lv_draw_rect(layer_, &d, &a);
}

static void draw_cb(lv_event_t *e)
{
	lv_layer_t *L = lv_event_get_layer(e);
	lv_area_t co;
	lv_obj_get_coords(kb, &co);
	int ox = co.x1, oy = co.y1;
	for (int i = 0; i < nkeys; i++) {
		struct key *k = &keys[i];
		int x = ox + k->x, y = oy + k->y;
		bool down = key_down(i);
		if (k->kind == K_SUGG) {
			if (!sugg[k->arg][0])
				continue;
			if (down)
				rect(L, x + 6, y + 8, k->w - 12, k->h - 16, 18, c_key, LV_OPA_COVER);
			if (k->arg > 0)
				rect(L, x, y + 24, 2, k->h - 48, 0, c_sp, LV_OPA_COVER);
			draw_text_c(L, sugg[k->arg], font(k->arg == 1 ? F_TEXT_SEMI : F_TEXT, 32), c_fg, x + 8, y, k->w - 16, k->h);
			continue;
		}
		bool special = k->kind != K_CHAR && k->kind != K_SPACE;
		if (mode == KBD_TERM && k->y < BAR_H)
			special = true;
		uint32_t bg = special ? c_sp : c_key, fg = c_fg;
		if (down)
			bg = special ? c_key : mix(c_key, c_fg, 18);
		if ((k->kind == K_SHIFT && (shift || caps)) || (k->kind == K_CTRL && (ctrl_latch || ctrl_hw))) {
			bg = k->kind == K_CTRL ? pal.tint : c_fg;
			fg = k->kind == K_CTRL ? pal.on_tint : c_bg;
		}
		rect(L, x, y + 2, k->w, k->h, 16, mix(c_bg, 0x000000, 35), LV_OPA_COVER);	/* key shadow */
		rect(L, x, y, k->w, k->h, 16, bg, LV_OPA_COVER);
		const char *label = k->kind == K_SHIFT ? (caps ? G_CAPS : G_SHIFT) : k->label;
		bool glyph = (unsigned char)label[0] >= 0xe0;
		lv_font_t *f = glyph ? font(F_MONO, k->kind == K_TERM ? 30 : 40)
			       : k->kind == K_CHAR && !(mode == KBD_TERM && k->y < BAR_H) ? font(F_TEXT, 44)
										       : font(F_TEXT, 30);
		draw_text_c(L, label, f, fg, x, y - (k->kind == K_CHAR && layer == L_ALPHA && !shift && !caps ? 3 : 0), k->w,
			    k->h);
	}
}

static void show_pop(int i)
{
	struct key *k = &keys[i];
	if (k->kind != K_CHAR || secure || (mode == KBD_TERM && k->y < BAR_H)) {
		lv_obj_add_flag(pop, LV_OBJ_FLAG_HIDDEN);
		return;
	}
	int w = k->w + 44, h = 132;
	int x = k->x + k->w / 2 - w / 2;
	x = x < 4 ? 4 : x > SCR_W - w - 4 ? SCR_W - w - 4 : x;
	lv_obj_set_pos(pop, x, lv_obj_get_y(kb) + k->y - h + 14);
	lv_label_set_text(pop_label, k->label);
	lv_obj_remove_flag(pop, LV_OBJ_FLAG_HIDDEN);
}

static void invalidate_key(int i)
{
	if (i < 0 || !kb)
		return;
	lv_area_t co, a;
	lv_obj_get_coords(kb, &co);
	a.x1 = co.x1 + keys[i].x - 2;
	a.y1 = co.y1 + keys[i].y - 2;
	a.x2 = a.x1 + keys[i].w + 4;
	a.y2 = a.y1 + keys[i].h + 6;
	lv_obj_invalidate_area(kb, &a);
}

/* ---------- output ---------- */

static void ctx_append(const char *s)
{
	if (secure)
		return;	/* a password never sits in the keyboard's memory */
	int n = strlen(ctx), add_ = strlen(s);
	if (n + add_ >= (int)sizeof(ctx)) {
		int drop = n + add_ - sizeof(ctx) + 40;
		memmove(ctx, ctx + drop, n - drop + 1);
		n -= drop;
	}
	memcpy(ctx + n, s, add_ + 1);
}

static void ctx_pop(void)
{
	int n = strlen(ctx);
	while (n > 0 && (ctx[n - 1] & 0xc0) == 0x80)
		n--;
	if (n > 0)
		n--;
	ctx[n] = 0;
}

/* every typed key (touch, hardware keyboard, scripts): the keyboard is up and text was typed in the last 30 s =
 * an awake reason (no dim, no auto-lock); after that, or when the keyboard goes, the usual countdown */
static void key_activity(void)
{
	lv_display_trigger_activity(NULL);
	power_input(0);
	power_awake_for("typing", 30000);
}

static void emit_text(const char *s)
{
	key_activity();
	if (client && client->text)
		client->text(s, ctrl_latch || ctrl_hw);
	ctx_append(s);
	if (ctrl_latch && kb) {
		ctrl_latch = false;
		lv_obj_invalidate(kb);
	}
}

static void emit_key(int tk)
{
	key_activity();
	if (client && client->key)
		client->key(tk);
	if (tk == TK_BKSP)
		ctx_pop();
	else if (tk == TK_ENTER)
		ctx_append("\n");
}

static void auto_caps(void)
{
	if (!kb || mode != KBD_TEXT || caps || nocap || secure || layer != L_ALPHA || !prefs_int("kbd_autocap", 1))
		return;
	int n = strlen(ctx);
	bool want = n == 0 || ctx[n - 1] == '\n' ||
		    (n >= 2 && ctx[n - 1] == ' ' && (ctx[n - 2] == '.' || ctx[n - 2] == '!' || ctx[n - 2] == '?'));
	if (want != shift) {
		shift = want;
		build();
		lv_obj_invalidate(kb);
	}
}

static void set_layer(int l)
{
	layer = l;
	shift = false;
	build();
	lv_obj_invalidate(kb);
	if (l == L_ALPHA)
		auto_caps();
}

static void commit(int i)
{
	if (i < 0)
		return;
	struct key *k = &keys[i];
	switch (k->kind) {
	case K_CHAR:
		emit_text(k->label);
		if (shift && !caps && layer == L_ALPHA) {
			shift = false;
			build();
			lv_obj_invalidate(kb);
		}
		if (layer != L_ALPHA && k->label[0] == '\'' && mode == KBD_TEXT)
			set_layer(L_ALPHA);	/* like iOS: back to letters after an apostrophe */
		break;
	case K_SPACE: {
		uint32_t now = lv_tick_get();
		int n = strlen(ctx);
		if (mode == KBD_TEXT && !secure && now - last_space_ms < 600 && n >= 2 && ctx[n - 1] == ' ' &&
		    ((ctx[n - 2] | 0x20) >= 'a' && (ctx[n - 2] | 0x20) <= 'z')) {
			emit_key(TK_BKSP);
			emit_text(". ");
			last_space_ms = 0;
		} else {
			emit_text(" ");
			last_space_ms = now;
		}
		if (layer != L_ALPHA && mode == KBD_TEXT)
			set_layer(L_ALPHA);
		break;
	}
	case K_ENTER:
		emit_key(TK_ENTER);
		break;
	case K_SUGG: {
		const char *s = sugg[k->arg];
		if (!s[0])
			return;
		char w[32];
		int len = current_word(w, sizeof(w));
		for (int j = 0; j < len; j++)
			emit_key(TK_BKSP);
		if (k->arg == 0) {	/* keep what was typed: strip the curly quotes */
			char lit[40];
			int sl = strlen(s);
			snprintf(lit, sizeof(lit), "%.*s", sl - 6, s + 3);
			emit_text(lit);
		} else {
			emit_text(s);
		}
		emit_text(" ");
		break;
	}
	default:
		return;
	}
	auto_caps();
	update_suggestions();
}

static void repeat_cb(lv_timer_t *t)
{
	lv_timer_set_period(t, 70);
	if (keys[repeat_key].kind == K_BKSP) {
		emit_key(TK_BKSP);
		update_suggestions();
	} else {
		emit_key(keys[repeat_key].arg);
	}
}

static void stop_repeat(void)
{
	if (repeat) {
		lv_timer_delete(repeat);
		repeat = NULL;
	}
}

/* keys that act on touch-down; returns true when the key is done */
static bool press_now(int i)
{
	struct key *k = &keys[i];
	switch (k->kind) {
	case K_SHIFT: {
		uint32_t now = lv_tick_get();
		if (caps) {
			caps = shift = false;
		} else if (shift && now - last_shift_ms < 350) {
			caps = true;
		} else {
			shift = !shift;
		}
		last_shift_ms = now;
		build();
		lv_obj_invalidate(kb);
		return true;
	}
	case K_BKSP:
	case K_TERM:
		if (k->kind == K_BKSP)
			emit_key(TK_BKSP);
		else
			emit_key(k->arg);
		stop_repeat();
		if (k->kind == K_BKSP || k->arg >= TK_UP) {
			repeat_key = i;
			repeat = lv_timer_create(repeat_cb, 450, NULL);
		}
		if (k->kind == K_BKSP) {
			auto_caps();
			update_suggestions();
		}
		return true;
	case K_LAYER:
		set_layer(k->arg);
		return true;
	case K_CTRL:
		ctrl_latch = !ctrl_latch;
		lv_obj_invalidate(kb);
		return true;
	default:
		return false;
	}
}

/* ---------- touch ---------- */

static struct finger *finger(int id)
{
	for (int f = 0; f < nfing; f++)
		if (fing[f].id == id)
			return &fing[f];
	return NULL;
}

static void refresh_pop(void)
{
	if (!pop)
		return;
	for (int f = nfing - 1; f >= 0; f--)	/* the newest uncommitted finger's key shows */
		if (!fing[f].committed && fing[f].key >= 0) {
			show_pop(fing[f].key);
			return;
		}
	lv_obj_add_flag(pop, LV_OBJ_FLAG_HIDDEN);
}

void kbd_touch(int id, int x, int y, int phase)
{
	if (!kb)
		return;
	int lx = x, ly = y - lv_obj_get_y(kb);
	struct finger *f = finger(id);
	if (phase == 0) {
		/* rollover: the previous fingers' keys are typed now, in order */
		for (int k = 0; k < nfing && kb; k++)
			if (!fing[k].committed) {
				fing[k].committed = true;
				commit(fing[k].key);
				invalidate_key(fing[k].key);
			}
		if (!kb || nfing == (int)(sizeof(fing) / sizeof(fing[0])))
			return;
		int key = hit(lx, ly, true);
		f = &fing[nfing++];
		*f = (struct finger){ id, key, lx, ly, false };
		if (key < 0) {
			f->committed = true;
			return;
		}
		if (prefs_int("kbd_haptic", 1))
			sys_haptic(8);
		f->committed = press_now(key);
		invalidate_key(key);
		refresh_pop();
		return;
	}
	if (!f)
		return;
	if (phase == 1) {
		if (f->committed || keys[f->key].kind == K_SUGG)
			return;
		int key = hit(lx, ly, false);
		if (key >= 0 && key != f->key && key >= first_row_key() && keys[key].kind == keys[f->key].kind) {
			invalidate_key(f->key);	/* slide to correct */
			f->key = key;
			invalidate_key(key);
			refresh_pop();
		}
		return;
	}
	/* up */
	int key = f->key;
	bool was = f->committed;
	*f = fing[--nfing];
	if (repeat && repeat_key == key)
		stop_repeat();
	if (!was)
		commit(key);	/* may hide the keyboard (return in a one-line field) */
	invalidate_key(key);
	refresh_pop();
}

bool kbd_hit(int x, int y)
{
	if (!kb || lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN) || y < lv_obj_get_y(kb) || y >= SCR_H - BOTTOM_PAD)
		return false;
	/* an alert, sheet, Control Center or the switcher drawn over the keyboard takes the touch */
	lv_point_t p = { x, y };
	for (int ly = LY_KBD + 1; ly < LY_COUNT; ly++)
		if (lv_indev_search_obj(shell_layer(ly), &p))
			return false;
	return true;
}

/* ---------- show / hide ---------- */

int kbd_height(enum kbd_mode m)
{
	(void)m;
	return BAR_H + KEYS_H + BOTTOM_PAD;
}

static void set_y(void *o, int32_t v) { slide_move(o, 0, v); }
static void shown(lv_anim_t *a) { slide_track(a->var, false); }

static void make(void)
{
	c_bg = th.dark ? mix(pal.cell, 0x000000, 25) : mix(th.bg, th.fg, 14);
	c_key = th.dark ? mix(pal.cell, th.fg, 24) : 0xffffff;
	c_sp = th.dark ? mix(pal.cell, th.fg, 10) : mix(th.bg, th.fg, 28);
	c_fg = pal.label;
	if (!words)
		model_init();
	for (int i = 0; i < 26; i++) {
		letters[i][0] = 'a' + i;
		letters_up[i][0] = 'A' + i;
	}
	kb = box(shell_kbd_layer());
	lv_obj_set_size(kb, SCR_W, kbd_height(mode));
	lv_obj_set_style_bg_opa(kb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(kb, C(c_bg), 0);
	lv_obj_add_flag(kb, LV_OBJ_FLAG_CLICKABLE);	/* taps never fall through to the app */
	lv_obj_add_event_cb(kb, draw_cb, LV_EVENT_DRAW_MAIN, NULL);

	pop = box(shell_kbd_layer());
	lv_obj_set_size(pop, 106, 132);
	lv_obj_set_style_radius(pop, 26, 0);
	lv_obj_set_style_bg_opa(pop, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(pop, C(mix(c_key, c_fg, 8)), 0);
	lv_obj_set_style_shadow_width(pop, 24, 0);
	lv_obj_set_style_shadow_opa(pop, LV_OPA_50, 0);
	lv_obj_set_style_shadow_color(pop, C(0x000000), 0);
	pop_label = text(pop, "", font(F_TEXT, 68), c_fg);
	lv_obj_align(pop_label, LV_ALIGN_CENTER, 0, -6);
	lv_obj_add_flag(pop, LV_OBJ_FLAG_HIDDEN);
}

void kbd_show(enum kbd_mode m, const text_client_t *c)
{
	client = c;
	if (kb && mode == m)
		return;
	if (kb)
		kbd_hide();
	mode = m;
	layer = L_ALPHA;
	shift = caps = ctrl_latch = false;
	nfing = 0;
	/* built below the screen without invalidating anything, then slides up: its pixels move,
	 * only the rows coming in are drawn */
	lv_display_enable_invalidation(NULL, false);
	make();
	build();
	auto_caps();
	update_suggestions();
	if (covered) {	/* asked for under the lock screen: waits there in place, hidden, for the unlock */
		lv_obj_set_y(kb, SCR_H - kbd_height(m));
		lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
		lv_display_enable_invalidation(NULL, true);
		return;
	}
	lv_obj_set_y(kb, SCR_H);
	lv_obj_refr_size(kb);
	lv_obj_refr_pos(kb);
	lv_display_enable_invalidation(NULL, true);
	slide_track(kb, true);
	motion_anim(kb, set_y, SCR_H, SCR_H - kbd_height(m), MOTION_SHEET, shown);
}

/* textarea binding */
static lv_obj_t *bound_ta;
static bool focus_ours;	/* the FOCUSED / DEFOCUSED on its way comes from here */

/* LVGL focuses whatever is pressed and defocuses the object pressed before: on a textarea the
 * keyboard edits only the keyboard decides (focused = caret shown = keyboard attached) */
static void focus_guard(lv_event_t *e)
{
	if (!focus_ours)
		lv_event_stop_processing(e);
}

/* under the lock screen (kbd_cover): the caret stands still — its blink is an endless animation (60 animation passes
 * a second) nobody sees. A state of its own on top of FOCUSED, so the app's FOCUSED cursor style stays untouched. */
#define CARET_STILL LV_STATE_USER_4
static lv_style_t caret_still;

/* focus + caret, then the keyboard's own event; false = the textarea was deleted meanwhile */
static bool ta_signal(lv_obj_t *ta, bool on)
{
	focus_ours = true;
	if (on)
		lv_obj_add_state(ta, covered ? LV_STATE_FOCUSED | CARET_STILL : LV_STATE_FOCUSED);
	else
		lv_obj_remove_state(ta, LV_STATE_FOCUSED | CARET_STILL);
	lv_result_t r = lv_obj_send_event(ta, on ? LV_EVENT_FOCUSED : LV_EVENT_DEFOCUSED, NULL);
	focus_ours = false;
	/* the caret blinks (an endless animation: 60 animation passes a second) only while focused; LVGL restarts
	 * the blink on FOCUSED but never stops it on DEFOCUSED — its label's STYLE_CHANGED makes it look again */
	if (r == LV_RESULT_OK && !on && lv_obj_check_type(ta, &lv_textarea_class))
		r = lv_obj_send_event(lv_textarea_get_label(ta), LV_EVENT_STYLE_CHANGED, NULL);
	return r == LV_RESULT_OK && lv_obj_send_event(ta, on ? EV_KBD_SHOWN : EV_KBD_HIDDEN, NULL) == LV_RESULT_OK;
}

/* the text before the caret → suggestions + auto-capitals */
static void ta_context(lv_obj_t *ta)
{
	if (secure) {
		kbd_set_context("");
		return;
	}
	const char *t = lv_textarea_get_text(ta);
	char buf[160];
	int pos = lv_textarea_get_cursor_pos(ta), bytes = 0;
	for (int i = 0; t[bytes] && i < pos; i++) {	/* cursor position is in characters */
		bytes++;
		while (((unsigned char)t[bytes] & 0xc0) == 0x80)
			bytes++;
	}
	int start = bytes > (int)sizeof(buf) - 1 ? bytes - (int)sizeof(buf) + 1 : 0;
	while (start < bytes && ((unsigned char)t[start] & 0xc0) == 0x80)
		start++;	/* never half a character */
	snprintf(buf, sizeof(buf), "%.*s", bytes - start, t + start);
	kbd_set_context(buf);
}

static void ta_text(const char *s, bool ctrl) { (void)ctrl; lv_textarea_add_text(bound_ta, s); }

static void ta_key(int tk)
{
	switch (tk) {
	case TK_BKSP: lv_textarea_delete_char(bound_ta); break;
	case TK_LEFT: lv_textarea_cursor_left(bound_ta); ta_context(bound_ta); break;
	case TK_RIGHT: lv_textarea_cursor_right(bound_ta); ta_context(bound_ta); break;
	case TK_ENTER:
		if (lv_textarea_get_one_line(bound_ta)) {
			lv_obj_t *ta = bound_ta;
			kbd_hide();
			lv_obj_send_event(ta, LV_EVENT_READY, NULL);
		} else {
			lv_textarea_add_char(bound_ta, '\n');
		}
		break;
	}
}

static const text_client_t ta_client = { ta_text, ta_key };

static void ta_deleted(lv_event_t *e)
{
	if (lv_event_get_target(e) == bound_ta) {
		bound_ta = NULL;
		if (client == &ta_client)
			kbd_hide();
	}
}

static bool has_cb(lv_obj_t *o, lv_event_cb_t cb)
{
	for (uint32_t i = 0; i < lv_obj_get_event_count(o); i++)
		if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(o, i)) == cb)
			return true;
	return false;
}

void kbd_attach(lv_obj_t *ta, enum kbd_mode m)
{
	if (bound_ta == ta && kb) {	/* already editing it: the caret may have moved */
		ta_context(ta);
		return;
	}
	if (bound_ta) {	/* switching fields: keep the keyboard up, move the focus */
		lv_obj_t *old = bound_ta;
		bound_ta = NULL;
		lv_obj_remove_event_cb(old, ta_deleted);
		ta_signal(old, false);
	}
	if (kb && mode != m)
		kbd_hide();
	nocap = false;
	secure = lv_textarea_get_password_mode(ta);	/* the eye button may show it later: still secret */
	bound_ta = ta;
	lv_obj_add_event_cb(ta, ta_deleted, LV_EVENT_DELETE, NULL);
	if (!has_cb(ta, focus_guard)) {
		static bool styled;
		if (!styled) {
			styled = true;
			lv_style_init(&caret_still);
			lv_style_set_anim_duration(&caret_still, 0);	/* = no blink */
		}
		lv_obj_add_style(ta, &caret_still, LV_PART_CURSOR | LV_STATE_FOCUSED | CARET_STILL);
		lv_obj_remove_flag(ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);
		lv_obj_add_event_cb(ta, focus_guard, LV_EVENT_FOCUSED | LV_EVENT_PREPROCESS, NULL);
		lv_obj_add_event_cb(ta, focus_guard, LV_EVENT_DEFOCUSED | LV_EVENT_PREPROCESS, NULL);
	}
	ret_label = lv_textarea_get_one_line(ta) ? "done" : "return";
	if (kb) {
		client = &ta_client;
		build();
		lv_obj_invalidate(kb);
	} else {
		kbd_show(m, &ta_client);
	}
	ta_context(ta);
	ta_signal(ta, true);
}

void kbd_hide(void)
{
	stop_repeat();
	if (kb) {
		lv_anim_delete(kb, NULL);
		lv_obj_delete(kb);
		lv_obj_delete(pop);
		kb = pop = NULL;
	}
	client = NULL;
	power_keep_awake("typing", false);	/* (no-op unless held: then the countdown starts now) */
	ret_label = "return";
	nfing = 0;
	nocap = false;
	secure = false;
	ctx[0] = 0;
	if (bound_ta) {	/* last: its handler may attach the keyboard again */
		lv_obj_t *ta = bound_ta;
		bound_ta = NULL;
		lv_obj_remove_event_cb(ta, ta_deleted);
		ta_signal(ta, false);
	}
}

/* the lock screen is over the keyboard (from screen-off until it is unlocked): the keyboard, its field and the app
 * stay as they were, so unlocking goes back to typing (iOS). Meanwhile the keyboard is HIDDEN (each wake redraws the
 * whole screen: no keyboard drawn under the lock), nothing on it is pressed, held or repeating, Vol− no longer counts
 * as Ctrl, typing stops holding auto-lock off, and the caret doesn't blink. kbd_visible() stays true (kb exists). */
static void kb_hidden(bool on)
{
	if (!kb || lv_obj_has_flag(kb, LV_OBJ_FLAG_HIDDEN) == on)
		return;
	if (on && lv_anim_delete(kb, set_y)) {	/* covered while still sliding up: it waits where it was going */
		lv_obj_set_y(kb, SCR_H - kbd_height(mode));
		slide_track(kb, false);
	}
	if (on)
		lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

void kbd_cover(bool on)
{
	kb_hidden(on);	/* (again too: a slide-unlock showed it early and was cut short by Power) */
	if (on == covered)
		return;
	covered = on;
	if (on) {
		stop_repeat();
		nfing = 0;	/* a finger still down lifts at wake: unknown by then, it types nothing */
		ctrl_hw = false;	/* (its release goes to the volume while locked) */
		if (pop)
			lv_obj_add_flag(pop, LV_OBJ_FLAG_HIDDEN);
		power_keep_awake("typing", false);
	}
	if (bound_ta) {
		if (on)
			lv_obj_add_state(bound_ta, CARET_STILL);
		else
			lv_obj_remove_state(bound_ta, CARET_STILL);
		if (lv_obj_check_type(bound_ta, &lv_textarea_class))	/* the blink re-reads its style */
			lv_obj_send_event(lv_textarea_get_label(bound_ta), LV_EVENT_STYLE_CHANGED, NULL);
	}
}

/* lock.c lock_dismiss: the unlock slide starts — the keyboard is back (still covered: no keys yet) without a redraw
 * (the lock screen is over all of it), so the strip the slide uncovers draws it */
void kbd_uncover_show(void)
{
	lv_display_enable_invalidation(NULL, false);
	kb_hidden(false);
	lv_display_enable_invalidation(NULL, true);
}

void kbd_no_autocap(void)
{
	nocap = true;
	if (kb && shift && !caps) {
		shift = false;
		build();
		lv_obj_invalidate(kb);
	}
}

void kbd_secure(void)
{
	secure = true;
	nocap = true;
	kbd_set_context("");
	kbd_no_autocap();
	refresh_pop();
}

bool kbd_visible(void) { return kb != NULL; }

void kbd_set_context(const char *before)
{
	if (secure)
		before = "";
	int n = strlen(before);
	snprintf(ctx, sizeof(ctx), "%s", n >= (int)sizeof(ctx) ? before + n - sizeof(ctx) + 1 : before);
	if (kb) {
		auto_caps();
		update_suggestions();
	}
}

void kbd_ctrl_held(bool held)
{
	ctrl_hw = held;
	if (kb)
		lv_obj_invalidate(kb);
}

void kbd_type(const char *s)
{
	if (covered)
		return;	/* nothing reaches a field under the lock screen */
	emit_text(s);
	if (kb) {
		auto_caps();
		update_suggestions();
	}
}

void kbd_press(int tk)
{
	if (!covered)
		emit_key(tk);
}
