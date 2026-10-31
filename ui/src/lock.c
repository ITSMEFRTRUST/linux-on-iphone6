/* Lock screen: wallpaper, date + big clock, next alarm, Now Playing, notifications, charging
 * line, Terminal / Music quick buttons. Press Home to unlock (the iPhone 6 way — no swipe);
 * a tap on empty space nudges the "Press Home to open" hint. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "orchard.h"
#include "src/widgets/spinner/lv_spinner_private.h"	/* spinners(): its duration + sweep */

#define CARD_W (SCR_W - 32)

static lv_obj_t *lock, *wall, *l_date, *l_time, *l_alarm, *l_charge, *l_hint, *cards, *np_play;
static char np_last[96];
static bool np_last_playing;

static uint32_t ink(void) { return th.dark ? 0xffffff : 0x1c1c1e; }

/* unlocking slides it up: its pixels move, only the strip it uncovers is drawn */
static void set_y(void *o, int32_t v) { slide_move(o, 0, v); }

static void gone(lv_anim_t *a)
{
	(void)a;
	lock_show(false);
}

void lock_dismiss(void)
{
	if (!lock_visible())
		return;
	kbd_uncover_show();	/* the keyboard (hidden under it) is there in the strip the slide uncovers */
	slide_track(lock, true);
	motion_anim(lock, set_y, lv_obj_get_y(lock), -SCR_H, MOTION_PAGE, gone);
}

static void hint_exec(void *o, int32_t v) { lv_obj_set_style_translate_y(o, -v, 0); }

/* tap on empty space: the hint hops, like iOS does when the phone wants the Home button */
static void lock_clicked(lv_event_t *e)
{
	(void)e;
	lv_anim_t a;
	lv_anim_init(&a);
	lv_anim_set_var(&a, l_hint);
	lv_anim_set_exec_cb(&a, hint_exec);
	lv_anim_set_values(&a, 0, 18);
	lv_anim_set_duration(&a, 120);
	lv_anim_set_reverse_duration(&a, 220);
	lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_repeat_count(&a, 2);
	lv_anim_start(&a);
}

/* ---------- cards ---------- */

static lv_obj_t *mcard(lv_obj_t *parent)
{
	lv_obj_t *c = box(parent);
	lv_obj_set_size(c, CARD_W, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(c, 44, 0);
	lv_obj_set_style_pad_all(c, 24, 0);
	lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
	return c;
}

static void notif_clicked(lv_event_t *e)
{
	const app_t *a = lv_event_get_user_data(e);
	lock_show(false);
	if (a)
		shell_launch(a);
}

static void clear_clicked(lv_event_t *e)
{
	(void)e;
	shell_clear_notifications();
}

static void np_btn(lv_event_t *e)
{
	int k = (intptr_t)lv_event_get_user_data(e);
	if (k < 0)
		music_prev();
	else if (k > 0)
		music_next();
	else
		music_toggle();
	ui_haptic_tap();
	lock_tick();
}

static void np_open(lv_event_t *e)
{
	(void)e;
	lock_show(false);
	shell_launch(&app_music);
}

static void now_playing(void)
{
	lv_obj_t *c = mcard(cards);
	lv_obj_set_height(c, 236);
	lv_obj_add_event_cb(c, np_open, LV_EVENT_CLICKED, NULL);
	lv_obj_t *art = box(c);
	lv_obj_set_size(art, 96, 96);
	lv_obj_set_style_radius(art, 20, 0);
	lv_obj_set_style_bg_opa(art, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(art, C(pal.tint), 0);
	lv_obj_set_style_bg_grad_color(art, C(mix(pal.tint, 0x000000, 45)), 0);
	lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
	lv_obj_center(ui_icon(art, G_MUSIC, font(F_MONO, 44), pal.on_tint));
	lv_obj_t *t = text(c, music_title(), font(F_TEXT_SEMI, 32), ink());
	lv_obj_set_width(t, CARD_W - 48 - 120);
	lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_pos(t, 120, 10);
	const char *ar = music_artist();
	lv_obj_t *s = text(c, ar && *ar ? ar : "Music", font(F_TEXT, 28), ink());
	lv_obj_set_style_text_opa(s, LV_OPA_60, 0);
	lv_obj_set_width(s, CARD_W - 48 - 120);
	lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_pos(s, 120, 54);
	static const char *g[] = { G_BACKWARD, G_PAUSE, G_FORWARD };
	for (int k = -1; k <= 1; k++) {
		lv_obj_t *b = box(c);
		lv_obj_set_size(b, 120, 80);
		lv_obj_align(b, LV_ALIGN_BOTTOM_MID, k * 170, 0);
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_PRESSED);
		lv_obj_add_event_cb(b, np_btn, LV_EVENT_CLICKED, (void *)(intptr_t)k);
		lv_obj_t *l = ui_icon(b, k == 0 && !music_playing() ? G_PLAY : g[k + 1], font(F_MONO, k ? 44 : 56), ink());
		lv_obj_center(l);
		if (k == 0)
			np_play = l;
	}
}

static void ago(char *buf, int n, long when)
{
	long d = time(NULL) - when;
	if (d < 60)
		snprintf(buf, n, "now");
	else if (d < 3600)
		snprintf(buf, n, "%ldm ago", d / 60);
	else
		snprintf(buf, n, "%ldh ago", d / 3600);
}

static void notification(const struct notif *nt)
{
	lv_obj_t *c = mcard(cards);
	lv_obj_set_style_pad_column(c, 22, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_add_event_cb(c, notif_clicked, LV_EVENT_CLICKED, (void *)nt->app);
	if (nt->app)
		app_icon(c, nt->app, 72);
	lv_obj_t *col = box(c);
	lv_obj_set_flex_grow(col, 1);
	lv_obj_set_height(col, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 2, 0);
	lv_obj_t *t = text(col, nt->title, font(F_TEXT_SEMI, 30), ink());
	lv_obj_set_width(t, CARD_W - 48 - 94 - 120);
	lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
	if (nt->body[0]) {
		lv_obj_t *b = text(col, nt->body, font(F_TEXT, 28), ink());
		lv_obj_set_width(b, LV_PCT(100));
		lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
	}
	char when[16];
	ago(when, sizeof(when), nt->when);
	lv_obj_t *w = text(c, when, font(F_TEXT, 24), ink());
	lv_obj_set_style_text_opa(w, LV_OPA_60, 0);
	lv_obj_add_flag(w, LV_OBJ_FLAG_FLOATING);
	lv_obj_align(w, LV_ALIGN_TOP_RIGHT, 0, 2);
}

static void rebuild(void)
{
	if (!cards)
		return;
	lv_obj_clean(cards);
	np_play = NULL;
	if (music_title())
		now_playing();
	const struct notif *list;
	int n = shell_notifications(&list);
	if (n) {
		lv_obj_t *hdr = box(cards);
		lv_obj_set_size(hdr, CARD_W, 64);
		lv_obj_t *clr = box(hdr);
		lv_obj_set_size(clr, LV_SIZE_CONTENT, 56);
		lv_obj_set_style_pad_hor(clr, 26, 0);
		lv_obj_set_style_radius(clr, LV_RADIUS_CIRCLE, 0);
		lv_obj_align(clr, LV_ALIGN_RIGHT_MID, 0, 0);
		lv_obj_add_flag(clr, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_add_event_cb(clr, clear_clicked, LV_EVENT_CLICKED, NULL);
		lv_obj_t *l = text(clr, G_CLOSE "  Clear", font(F_TEXT_SEMI, 24), 0xffffff);
		lv_obj_center(l);
		lv_obj_update_layout(cards);
		ui_material(clr);
	}
	int shown = music_title() ? 2 : 3;
	for (int i = 0; i < n && i < shown; i++)
		notification(&list[i]);
	if (n > shown) {
		char more[32];
		snprintf(more, sizeof(more), "+%d more", n - shown);
		lv_obj_t *m = text(cards, more, font(F_TEXT_SEMI, 26), 0xffffff);
		lv_obj_set_style_text_opa(m, LV_OPA_70, 0);
	}
	/* material needs the final positions: lay out first, then blur behind each card */
	lv_obj_update_layout(cards);
	for (uint32_t i = 0; i < lv_obj_get_child_count(cards); i++) {
		lv_obj_t *c = lv_obj_get_child(cards, i);
		if (lv_obj_get_style_radius(c, 0) == 44)
			ui_material(c);
	}
}

void lock_notifications_changed(void)
{
	if (lock_visible() && lv_obj_get_y(lock) == 0)
		rebuild();
}

/* ---------- quick buttons ---------- */

static void quick_clicked(lv_event_t *e)
{
	const app_t *a = lv_event_get_user_data(e);
	lock_show(false);
	shell_launch(a);
}


/* the info lines get a soft dark copy 2 px down-right (LVGL has no text shadow): readable on any wallpaper */
static lv_obj_t *shadowed(lv_obj_t *l)
{
	lv_obj_t *sh = lv_label_create(lock);
	lv_obj_remove_style_all(sh);
	lv_obj_remove_flag(sh, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_text_font(sh, lv_obj_get_style_text_font(l, 0), 0);
	lv_obj_set_style_text_color(sh, C(0x000000), 0);
	lv_obj_set_style_text_opa(sh, LV_OPA_80, 0);
	lv_obj_set_style_text_align(sh, lv_obj_get_style_text_align(l, 0), 0);
	lv_label_set_long_mode(sh, lv_label_get_long_mode(l));
	lv_obj_set_size(sh, lv_obj_get_style_width(l, 0), lv_obj_get_style_height(l, 0));
	lv_obj_set_pos(sh, lv_obj_get_style_x(l, 0) + 2, lv_obj_get_style_y(l, 0) + 2);
	lv_label_set_text(sh, lv_label_get_text(l));
	if (lv_obj_has_flag(l, LV_OBJ_FLAG_HIDDEN))
		lv_obj_add_flag(sh, LV_OBJ_FLAG_HIDDEN);
	lv_obj_move_to_index(sh, lv_obj_get_index(l));	/* just below it */
	lv_obj_set_user_data(l, sh);
	return l;
}

static void sh_text(lv_obj_t *l, const char *t)
{
	/* a label cut short with "…" holds the cut text, so compare with what was set (every second otherwise) */
	static struct { lv_obj_t *l; char t[256]; } last[8];
	int k = 0;
	while (k < 8 && last[k].l && last[k].l != l)
		k++;
	if (k < 8 && last[k].l == l && !strcmp(last[k].t, t) && lv_label_get_text(l)[0])
		return;
	if (k < 8) {
		last[k].l = l;
		snprintf(last[k].t, sizeof(last[k].t), "%s", t);
	}
	lv_label_set_text(l, t);
	lv_label_set_text(lv_obj_get_user_data(l), t);
}

static void sh_show(lv_obj_t *l, bool on)
{
	lv_obj_t *sh = lv_obj_get_user_data(l);
	if (lv_obj_has_flag(l, LV_OBJ_FLAG_HIDDEN) != on)
		return;	/* (showing a shown object again still redraws it, every second)*/
	if (on) {
		lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(sh, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(sh, LV_OBJ_FLAG_HIDDEN);
	}
}

static void quick(const app_t *a, const char *glyph, int x)
{
	lv_obj_t *b = box(lock);
	lv_obj_set_size(b, 100, 100);
	lv_obj_set_pos(b, x, SCR_H - 196);
	lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
	ui_material(b);
	lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
	lv_obj_set_style_color_filter_opa(b, LV_OPA_30, LV_STATE_PRESSED);
	lv_obj_add_event_cb(b, quick_clicked, LV_EVENT_CLICKED, (void *)a);
	lv_obj_center(ui_icon(b, glyph, font(F_MONO, 38), 0xffffff));
}

/* ---------- public ---------- */

void lock_tick(void)
{
	if (!lock_visible())
		return;
	char buf[48];
	sys_clock_format(buf, sizeof(buf), false);
	if (strcmp(buf, lv_label_get_text(l_time)))
		lv_label_set_text(l_time, buf);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	char d[32];
	strftime(d, sizeof(d), "%A, %B", &tm);
	snprintf(buf, sizeof(buf), "%s %d", d, tm.tm_mday);
	sh_text(l_date, buf);

	const char *al = clock_next_alarm();
	if (al) {
		snprintf(buf, sizeof(buf), G_ALARM "  %s", al);
		sh_text(l_alarm, buf);
		sh_show(l_alarm, true);
	} else {
		sh_show(l_alarm, false);
	}
	int y = al ? 430 : 392;
	int cy = y > 468 ? y + 4 : 470;	/* three lines: the cards move down a line */
	if (lv_obj_get_y(cards) != cy) {
		lv_obj_set_y(cards, cy);
		lock_notifications_changed();	/* the cards' material is aligned to where they sit */
	}
	if (sys_charger_online()) {
		int ma = sys_charger_ma(), pct = sys_battery_pct();
		char p[16] = "";
		if (pct >= 0)
			snprintf(p, sizeof(p), " \xc2\xb7 %d%%", pct);
		if (pct == 100)
			snprintf(buf, sizeof(buf), G_BOLT "  Charged%s", p);
		else if (ma > 0)
			snprintf(buf, sizeof(buf), G_BOLT "  Charging%s \xc2\xb7 %d mA", p, ma);
		else
			snprintf(buf, sizeof(buf), G_BOLT "  Charging%s", p);
	} else {
		buf[0] = 0;
	}
	if (strcmp(buf, lv_label_get_text(l_charge)))
		lv_label_set_text(l_charge, buf);

	/* Now Playing card follows the player */
	const char *t = music_title();
	bool playing = music_playing();
	bool new_song = strcmp(t ? t : "", np_last) != 0;
	if (new_song || playing != np_last_playing) {
		snprintf(np_last, sizeof(np_last), "%s", t ? t : "");
		np_last_playing = playing;
		if (new_song || !np_play)
			lock_notifications_changed();
		else
			ui_icon_set(np_play, playing ? G_PAUSE : G_PLAY);
	}
}

/* the open app's foreground timers follow the lock screen: nothing under it is seen, so they slow down / pause
 * (the Terminal reads its pty once a second, Clock's stopwatch stops ticking …) and are back at once on unlock */
#define MAXLL 16	/* 10 in use with every app opened once (2026-10) */
static void (*lock_ls[MAXLL])(bool locked);

/* LVGL spinners under it (Music / Web "Loading…" on a slow network) drew 60 frames a second under the
 * lock screen: their animation stops meanwhile and starts again on unlock (from the top: nobody saw where it was) */
static lv_obj_tree_walk_res_t spin_walk(lv_obj_t *o, void *ud)
{
	if (lv_obj_check_type(o, &lv_spinner_class)) {
		lv_spinner_t *sp = (lv_spinner_t *)o;
		if (ud)
			lv_anim_delete(o, NULL);
		else
			lv_spinner_set_anim_params(o, sp->duration, sp->angle);
	}
	return LV_OBJ_TREE_WALK_NEXT;
}

static void spinners(bool stop)
{
	lv_obj_tree_walk(lv_screen_active(), spin_walk, stop ? (void *)1 : NULL);	/* home + the app: all below the lock */
}

/* a listener that comes while the lock screen is already up (an app opened under it, a timer made late) hears it
 * at once — it would wait for the unlock otherwise, running at full pace under the lock meanwhile */
void lock_listen(void (*cb)(bool locked), bool on)
{
	for (int i = 0; i < MAXLL; i++)
		if (lock_ls[i] == cb)
			lock_ls[i] = NULL;
	if (!on || !cb)
		return;
	for (int i = 0; i < MAXLL; i++)
		if (!lock_ls[i]) {
			lock_ls[i] = cb;
			if (lock_visible())
				cb(true);
			return;
		}
	fprintf(stderr, "lock: lock_listen: all %d slots taken, a listener is dropped (raise MAXLL)\n", MAXLL);
}

void lock_show(bool on)
{
	if (!lock)
		return;
	bool was = lock_visible();
	lv_anim_delete(lock, set_y);
	slide_track(lock, false);
	lv_obj_set_y(lock, 0);
	kbd_cover(on);	/* an app's keyboard waits under the lock screen (hidden: not drawn at every wake) */
	if (on) {
		if (switcher_visible())
			switcher_show(false);
		lv_obj_remove_flag(lock, LV_OBJ_FLAG_HIDDEN);
		lock_tick();
		rebuild();
	} else {
		lv_obj_add_flag(lock, LV_OBJ_FLAG_HIDDEN);
	}
	status_refresh();
	if (on != was)
		spinners(on);
	if (on != was)
		for (int i = 0; i < MAXLL; i++)
			if (lock_ls[i])
				lock_ls[i](on);
}

bool lock_visible(void) { return lock && !lv_obj_has_flag(lock, LV_OBJ_FLAG_HIDDEN); }

void lock_wallpaper_changed(void)
{
	lv_image_set_src(wall, theme_wallpaper());
	if (lock_visible())
		rebuild();
}

void lock_create(void)
{
	lock = box(shell_layer(LY_LOCK));
	lv_obj_set_size(lock, SCR_W, SCR_H);
	lv_obj_add_flag(lock, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(lock, lock_clicked, LV_EVENT_CLICKED, NULL);
	wall = lv_image_create(lock);
	lv_image_set_src(wall, theme_wallpaper());

	lv_obj_t *g = ui_icon_line(lock, G_LOCK, font(F_MONO, 30), 0xffffff);
	lv_obj_align(g, LV_ALIGN_TOP_MID, 0, 56);
	l_date = text(lock, "", font(F_TEXT_BOLD, 36), 0xffffff);
	lv_obj_set_width(l_date, SCR_W);
	lv_obj_set_style_text_align(l_date, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(l_date, 0, 104);
	shadowed(l_date);
	l_time = text(lock, "", font(F_DISPLAY_BOLD, 200), 0xffffff);
	lv_obj_set_width(l_time, SCR_W);
	lv_obj_set_style_text_align(l_time, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(l_time, 0, 138);
	l_alarm = text(lock, "", font(F_TEXT_BOLD, 30), 0xffffff);
	lv_obj_set_width(l_alarm, SCR_W);
	lv_obj_set_style_text_align(l_alarm, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(l_alarm, 0, 392);
	shadowed(l_alarm);

	cards = box(lock);
	lv_obj_set_pos(cards, 16, 470);
	lv_obj_set_size(cards, CARD_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cards, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(cards, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(cards, 16, 0);
	lv_obj_remove_flag(cards, LV_OBJ_FLAG_CLICKABLE);

	quick(&app_terminal, ">_", 56);
	quick(&app_music, G_MUSIC, SCR_W - 156);
	l_charge = text(lock, "", font(F_TEXT_SEMI, 26), 0xffffff);
	lv_obj_set_width(l_charge, SCR_W - 340);
	lv_obj_set_style_text_align(l_charge, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(l_charge, 170, SCR_H - 160);
	l_hint = text(lock, "Press Home to open", font(F_TEXT_MEDIUM, 28), 0xffffff);
	lv_obj_set_style_text_opa(l_hint, LV_OPA_70, 0);
	lv_obj_align(l_hint, LV_ALIGN_BOTTOM_MID, 0, -30);
	lv_obj_add_flag(lock, LV_OBJ_FLAG_HIDDEN);
}
