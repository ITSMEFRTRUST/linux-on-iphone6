/* Orchard — iOS-style phone shell for Linux on the iPhone 6 (LVGL, software-rendered). */
#pragma once
#include <stdbool.h>
#include "lvgl.h"

#define SCR_W 750
#define SCR_H 1334
#define STATUS_H 40
#define NAV_H 88	/* navigation bar below the status bar */
#define BOTTOM_PAD 16	/* bottom margin under controls (no home indicator: the iPhone 6 has a Home button) */
#define PT(x) ((x) * 2)	/* iOS points → pixels (iPhone 6 is @2x) */

/* ---- fonts (fonts.c) ---- */
enum face { F_TEXT, F_TEXT_MEDIUM, F_TEXT_SEMI, F_TEXT_BOLD, F_DISPLAY_LIGHT, F_DISPLAY_BOLD, F_MONO };
lv_font_t *font(enum face face, int px);

/* ---- theme (theme.c): the PC theme switcher's colors.toml roles ---- */
typedef struct theme {
	bool dark;
	uint32_t accent, bg, fg, bright_fg, soft_fg, muted, lighter_bg, dark_bg;
	uint32_t ansi[16];
} theme_t;
extern theme_t th;
void theme_load(void);
const lv_image_dsc_t *theme_wallpaper(void);		/* theme wallpaper, or the built-in one */
const lv_image_dsc_t *theme_wallpaper_blur(void);	/* blurred + dimmed copy = the "material" behind panels */
int theme_wallpaper_count(void);	/* desktop theme's (if any) + generated ones */
const char *theme_wallpaper_name(int i);
int theme_wallpaper_index(void);
void theme_wallpaper_select(int i);	/* regenerates the material; then shell_wallpaper_changed() */
uint32_t mix(uint32_t a, uint32_t b, int pct);	/* pct% of the way from a to b */
#define C(x) lv_color_hex(x)

/* iOS semantic colours, derived from the theme (ui.c). Use these in apps, never raw hex. */
typedef struct palette {
	uint32_t bg;		/* app background (systemBackground) */
	uint32_t grouped_bg;	/* behind inset-grouped lists */
	uint32_t cell;		/* list cells, cards */
	uint32_t cell_hi;	/* pressed cell */
	uint32_t fill;		/* controls on a cell: unchecked switch, slider track, text field */
	uint32_t separator;
	uint32_t label, label2, label3;	/* primary / secondary / tertiary text */
	uint32_t tint;		/* accent: buttons, links, checked switches */
	uint32_t on_tint;	/* text on a tint-coloured fill */
	uint32_t red, orange, yellow, green, teal, blue, indigo, purple, pink, gray;	/* icon / status colours */
} palette_t;
extern palette_t pal;

/* ---- apps ---- */
enum icon_style { IC_DARK, IC_ACCENT, IC_WARM, IC_MUTED, IC_LIGHT, IC_YELLOW, IC_BLUE, IC_GREEN, IC_RED };

typedef struct app {
	const char *name;
	const char *glyph;	/* drawn with the Nerd Font in the icon */
	enum icon_style icon;	/* icon colours come from the theme */
	void (*open)(lv_obj_t *root);	/* build the UI into root (full screen, under the status bar) */
	void (*close)(void);		/* UI is about to be deleted: drop object pointers, stop timers */
	void (*kill)(void);		/* optional: swiped away in the app switcher (reset state) */
} app_t;

extern const app_t app_terminal, app_settings, app_calc, app_music, app_clock, app_notes, app_files;
extern const app_t app_web, app_weather;	/* app_web.c, app_weather.c: internet apps (net.c) */
extern const app_t app_camera;	/* app_camera.c: the A8 ISP's /dev/isp0 (cam_*.c) */
extern const app_t app_memos;	/* app_memos.c: Voice Memos (the codec's mics on ALSA hw:0,2; memo.c) */
/* Optional "Full Site" renderer hook: none ships with this project. app_web.c declares it weak and only shows its
 * menu items when a build links one in (it would push a page in the open app). */
void web_full_open(const char *url);
/* app_web.c (optional, for a full-site renderer): Full Site's Reader button — called once the Full Site page is
 * gone (the reader page it came from is on top again): show url in the reader */
void web_reader_open(const char *url);

/* ---- shell (shell.c) ---- */
void shell_init(void);
void shell_go_home(void);
void shell_open_app(const app_t *a, lv_obj_t *from);	/* from = icon to zoom out of, NULL = centre */
void shell_launch(const app_t *a);	/* open from anywhere (control center, banners, search) */
bool shell_app_open(void);
const app_t *shell_current_app(void);
void shell_cc_toggle(void);
bool shell_cc_open(void);
void shell_screen_toggle(void);
bool shell_screen_off(void);
bool shell_locked(void);
void shell_unlock(void);
void shell_home_key(bool pressed);	/* acts on release; held ≥ 0.5 s = Search */
void shell_power_key(void);
void shell_switcher_toggle(void);
bool shell_switcher_open(void);
void shell_banner(const app_t *from, const char *title, const char *body);	/* iOS notification banner */
void shell_volume_step(int dir);	/* volume keys: ±1 step + HUD */
void shell_alarm(const app_t *from, const char *title, const char *body);	/* banner + long buzz, wakes the screen */
void shell_wallpaper_changed(void);	/* rebuild home/lock backgrounds after theme_wallpaper_select */
lv_obj_t *shell_overlay(void);	/* layer above apps + keyboard for alerts and sheets */
lv_obj_t *shell_kbd_layer(void);

/* shell internals (shell.c, home.c, lock.c, cc.c, switcher.c) */
enum { LY_KBD, LY_LOCK, LY_CC, LY_STATUS, LY_SWITCHER, LY_BANNER, LY_OVERLAY, LY_COUNT };
lv_obj_t *shell_layer(int ly);
void home_create(lv_obj_t *parent);
void home_tick(void);	/* every second */
lv_obj_t *home_icon_of(const app_t *a);	/* icon object on the home screen / dock (zoom origin) */
void home_reset(void);	/* back to page 1, close search */
void home_wallpaper_changed(void);
lv_obj_t *app_icon(lv_obj_t *parent, const app_t *a, int size);	/* the rounded icon tile */
void lock_create(void);
void lock_show(bool on);	/* instant */
void lock_tick(void);
void lock_notifications_changed(void);
bool lock_visible(void);
void lock_dismiss(void);	/* animated unlock (Home key) */
void lock_wallpaper_changed(void);
void lock_listen(void (*cb)(bool locked), bool on);	/* called when the lock screen comes up (screen off) / goes (unlock):
			 * an open app's foreground polls + UI timers slow down or pause under it, and come back at once */
void cc_create(void);
void cc_tick(void);
void cc_drag(int phase, int dy, int vy);	/* input.c: pull down from the top (0 start, 1 move, 2 end; vy px/s) */
/* the app's last frame, copied off the screen while it is still showing; launch = reopening the app
 * will look the same (root page, no keyboard), so the open zoom can show it at once */
const lv_image_dsc_t *switcher_snapshot(const app_t *a, lv_obj_t *root, bool launch);
const lv_image_dsc_t *switcher_picture(const app_t *a, bool any);	/* full-screen picture, NULL = none (fit) */
void switcher_forget(const app_t *a);
void switcher_show(bool on);
bool switcher_visible(void);
lv_obj_t *switcher_card(const app_t *a);	/* the picture on its card in the open switcher (zoom target) */
void status_set_style(bool on_wallpaper);
void status_refresh(void);	/* re-pick the bar style for what is on screen + update it */
void search_open(void);
struct notif { const app_t *app; char title[64], body[160]; long when; };
int shell_notifications(const struct notif **list);	/* newest first */
void shell_clear_notifications(void);

/* ---- input (input.c): backends feed raw touch frames + Linux key codes ---- */
struct touch { int id, x, y; };
void input_init(void);
void input_touches(const struct touch *t, int n);	/* every touch frame: all fingers currently down */
void input_feed(int x, int y, bool pressed);	/* single-finger convenience (mouse, scripts) */
void input_key(int code, bool pressed);

/* ---- keyboard (kbd.c) ---- */
enum { TK_BKSP = 1, TK_ENTER, TK_TAB, TK_ESC, TK_UP, TK_DOWN, TK_LEFT, TK_RIGHT };
enum kbd_mode { KBD_TEXT, KBD_TERM };
typedef struct text_client {
	void (*text)(const char *utf8, bool ctrl);	/* ctrl = Ctrl latched (terminal row) or Vol− held */
	void (*key)(int tk);
} text_client_t;
int kbd_height(enum kbd_mode mode);
void kbd_show(enum kbd_mode mode, const text_client_t *c);	/* slides up at the bottom of the screen */
void kbd_hide(void);
bool kbd_visible(void);
void kbd_set_context(const char *before_cursor);	/* text before the cursor: suggestions + auto-caps */
void kbd_attach(lv_obj_t *textarea, enum kbd_mode mode);	/* keyboard edits this lv_textarea; again on the
				 * same one = the caret moved (re-reads the text before it: suggestions, caps) */
/* sent to the textarea the keyboard edits (lv_obj_add_event_cb(ta, cb, EV_KBD_SHOWN, ud)): SHOWN = the
 * keyboard is up for it, HIDDEN = the keyboard went away or another field took it. Only kbd.c sends
 * them — unlike LVGL's FOCUSED/DEFOCUSED, which follow every press. An attached textarea is in
 * LV_STATE_FOCUSED (caret shown) exactly while it has the keyboard. ui_init reserves the codes. */
#define EV_KBD_SHOWN ((lv_event_code_t)(LV_EVENT_LAST + 1))
#define EV_KBD_HIDDEN ((lv_event_code_t)(LV_EVENT_LAST + 2))
void kbd_no_autocap(void);	/* after kbd_attach: this field never auto-capitalises (search, commands) */
void kbd_secure(void);	/* after kbd_attach: a secret (automatic for a textarea in password mode): no suggestions,
			 * word model, auto-capitals or pop-ups, nothing typed is remembered */
void kbd_cover(bool on);	/* lock.c: the lock screen is over it — the keyboard and its field stay (unlocking goes
			 * back to typing); meanwhile it is hidden (not drawn), no key is held, no caret blinks, kbd_type / kbd_press
			 * do nothing. kbd_visible() stays true */
void kbd_uncover_show(void);	/* lock.c: the unlock slide starts — the hidden keyboard is there again (no redraw) */
void kbd_ctrl_held(bool held);	/* hardware Vol− = Ctrl */
void kbd_type(const char *utf8);	/* hardware keyboards / test scripts → current client */
void kbd_press(int tk);
bool kbd_hit(int x, int y);	/* input.c: is this point on the keyboard (and not under an overlay)? */
void kbd_touch(int id, int x, int y, int phase);	/* 0 down, 1 move, 2 up */

/* ---- UI kit (ui.c): iOS 26-style components without Liquid Glass ---- */
void ui_init(void);
/* navigation stack for the open app (up to 16 pages): pages slide in from the right, back = chevron or
 * left-edge swipe. The bar keeps the title centred: the back label shrinks from the previous title
 * to "Back" to the chevron alone as the title needs room, then the title is cut short with "…" */
lv_obj_t *nav_root(lv_obj_t *app_root, const char *title);	/* first page; returns its scrolling content column */
lv_obj_t *nav_push(const char *title);	/* returns the new page's content column */
void nav_pop(void);
void nav_land(void);	/* the push in flight lands at once (a page restored in place, no slide) */
void nav_cut(int to);	/* pages above depth `to` go at once, no slide (their leave callbacks run) */
int nav_depth(void);	/* 0 = root page */
lv_obj_t *nav_page(void);	/* current page object (full screen) */
void nav_set_title(lv_obj_t *page, const char *title);	/* page = what nav_page() returned for it */
lv_obj_t *nav_button(const char *label_or_glyph, lv_event_cb_t cb, void *ud);	/* right side of the nav bar */
void nav_on_leave(void (*cb)(void));	/* called when the current page is popped or the app closes */
void nav_lock(bool on);	/* on = no back swipe (a full-screen sheet such as Now Playing covers the pages);
			 * nav_root / nav_reset turn it off */
bool nav_locked(void);	/* nav_lock is on or an alert is up */
void nav_reset(void);
void nav_drag(int phase, int dx, int vx);	/* input.c: interactive back swipe (0 start, 1 move, 2 end; vx px/s) */
bool nav_pristine(void);	/* root page scrolled to the top: the app reopens looking like this */
/* inset-grouped lists */
lv_obj_t *ui_section(lv_obj_t *content, const char *header, const char *footer);
lv_obj_t *ui_row(lv_obj_t *section, const char *glyph, uint32_t icon_color, const char *title);
lv_obj_t *ui_row_value(lv_obj_t *row, const char *value);	/* grey value on the right; returns its label */
void ui_row_chevron(lv_obj_t *row);
void ui_row_tap(lv_obj_t *row, lv_event_cb_t cb, void *ud);	/* row highlights while pressed (CLICKED) */
/* hold (LONG_PRESSED, finger kept still): tick + highlight off, then cb; the release after it
 * is not a tap, so ui_row_tap's cb doesn't follow */
void ui_row_hold(lv_obj_t *row, lv_event_cb_t cb, void *ud);
/* swipe left reveals an action pill at the row's right end (iOS swipe actions; swipe right or a
 * press anywhere else puts it away, a tap on the open row only closes it); cb = pill tapped.
 * Returns the pill. Rows keep their own padding; the row's GESTURE_BUBBLE is dropped. */
lv_obj_t *ui_row_swipe(lv_obj_t *row, const char *glyph, const char *label, uint32_t color, lv_event_cb_t cb,
		       void *ud);
void ui_swipe_close(void);	/* put an open swipe action away */
bool ui_swipe_shown(void);	/* a row is showing its swipe action */
lv_obj_t *ui_row_switch(lv_obj_t *row, bool on, lv_event_cb_t cb, void *ud);
lv_obj_t *ui_row_check(lv_obj_t *row, bool on);	/* tint checkmark (selection lists); returns it */
lv_obj_t *ui_row_slider(lv_obj_t *section, const char *glyph_lo, const char *glyph_hi, int min, int max, int val,
			lv_event_cb_t cb, void *ud);
lv_obj_t *ui_row_subtitle(lv_obj_t *row, const char *sub);	/* second line under the title */
/* ui_row: title (+ subtitle) centred in the 100 px row, one line each, "…" when too long */
/* controls */
lv_obj_t *ui_switch(lv_obj_t *parent, bool on);
lv_obj_t *ui_slider(lv_obj_t *parent, int w, int min, int max, int val);
lv_obj_t *ui_button(lv_obj_t *parent, const char *label, bool filled);	/* capsule button */
lv_obj_t *ui_icon_button(lv_obj_t *parent, const char *glyph, int size, uint32_t bg, uint32_t fg);	/* round */
lv_obj_t *ui_segmented(lv_obj_t *parent, int w, const char **items, int n, int sel);	/* VALUE_CHANGED, ui_segmented_get */
int ui_segmented_get(lv_obj_t *seg);
lv_obj_t *ui_tabbar(lv_obj_t *root, const char **glyphs, const char **labels, int n, int sel);	/* floating capsule */
int ui_tabbar_get(lv_obj_t *tb);
lv_obj_t *ui_search(lv_obj_t *parent, int w, const char *placeholder);	/* returns the lv_textarea */
lv_obj_t *ui_text_field(lv_obj_t *parent, int w, const char *placeholder);
/* one alert at a time: another one raised meanwhile waits its turn; one raised from inside an alert's
 * button callback comes in as that one goes */
void ui_alert(const char *title, const char *msg, const char **buttons, int n, void (*cb)(int idx, void *ud), void *ud);
/* popover menu (iOS 26 context / pull-down menu) on the overlay: a card anchored by its right edge,
 * below y_below if it fits, else above y_above; a press outside closes it, a pick closes it + cb(id) */
enum { MI_HEAD = 1, MI_CHECK = 2, MI_ON = 4, MI_RED = 8, MI_GROUP = 16 };	/* MI_HEAD = grey caption, not
				 * pickable; MI_CHECK = checkmark column (MI_ON = this one ticked); MI_GROUP = gap above */
typedef struct ui_menu_item { int id, flags; const char *glyph, *label; } ui_menu_item_t;
void ui_menu(const ui_menu_item_t *it, int n, int right, int y_below, int y_above, void (*cb)(int id));
void ui_row_menu(lv_obj_t *row, const ui_menu_item_t *it, int n, void (*cb)(int id));	/* under / over a list row */
void ui_menu_close(void);
bool ui_menu_shown(void);
void ui_material(lv_obj_t *obj);	/* blurred-wallpaper background aligned to the screen (for static objects) */
/* a one-glyph Nerd Font label (an icon): its box becomes the ink's box, so centring / aligning / a flex gap work
 * on the icon you see (the icons hang out of their advance and got clipped); a fixed width / height = ink centred
 * in it. Fit again after changing its font; change its glyph with ui_icon_set, never lv_label_set_text */
void ui_glyph_fit(lv_obj_t *label);
lv_font_t *ui_icon_font(const char *glyph, int ink);	/* F_MONO size at which the glyph's larger side is ~ink px */
lv_obj_t *ui_icon(lv_obj_t *parent, const char *glyph, lv_font_t *f, uint32_t color);	/* text() + ui_glyph_fit */
lv_obj_t *ui_icon_line(lv_obj_t *parent, const char *glyph, lv_font_t *f, uint32_t color);	/* box = line box, ink centred */
void ui_icon_set(lv_obj_t *label, const char *glyph);	/* another glyph into a fitted label (fits it again) */
uint32_t ui_on_color(uint32_t bg);	/* readable glyph colour on a coloured fill (list icon tiles) */
void ui_haptic_tap(void);
lv_obj_t *ui_spinner(lv_obj_t *parent, uint32_t ms, uint32_t sweep);	/* every LVGL spinner (never lv_spinner_create):
			 * one made under the lock screen starts stopped; ms / sweep 0 = LVGL's defaults */
void ui_haptic_success(void);

/* motion: one curve and one set of durations for every transition, so the whole shell moves alike.
 * The curve leaves at full speed and settles softly (ease-out quart, close to a critically damped
 * spring): even at ~50 fps the first frame already shows real movement. Gesture releases instead
 * start at the finger's own speed (motion_fling_ms). Nothing waits for an animation to finish:
 * new input finishes or retargets it. */
#define MOTION_ZOOM 250		/* app zooms out of its icon / back into it (ms) */
#define MOTION_PAGE 260		/* nav push / pop, Control Center, lock screen */
#define MOTION_SHEET 220	/* keyboard, banners */
#define MOTION_SMALL 180	/* segmented thumb, alert fade, switcher card snap-back */
int32_t motion_path(const lv_anim_t *a);	/* the curve, for lv_anim_set_path_cb */
void motion_anim(void *var, lv_anim_exec_xcb_t cb, int from, int to, int ms, lv_anim_completed_cb_t done);
int motion_fling_ms(int dist, int v, int max_ms);	/* ms to cover dist px leaving at v px/s (≥ 100) */
/* slides: an object that only moves keeps its pixels — each frame they are shifted in the
 * framebuffer and only what the move uncovers (plus the overlays above it) is drawn again */
void slide_track(lv_obj_t *o, bool on);	/* list bottom to top; off = one full redraw of it */
void slide_move(lv_obj_t *o, int x, int y);	/* lv_obj_set_pos for a tracked object */

/* ---- preferences (prefs.c): key=value file in $ORCHARD_DATA or ~/.orchard ---- */
int prefs_int(const char *key, int def);
void prefs_set_int(const char *key, int v);
const char *prefs_str(const char *key, const char *def);
void prefs_set_str(const char *key, const char *v);
const char *data_dir(void);	/* created on first use */

/* ---- system info / control (sys.c) ---- */
int sys_backlight_max(void);
int sys_backlight_get(void);
void sys_backlight_set(int v);
void sys_cpu_perf(bool on);	/* old switch: performance / schedutil (power.c uses sys_cpufreq) */
/* cpufreq on every policy, written by a helper thread (a write waits for the frequency change): performance, or
 * schedutil between min_khz and max_khz (0 = the hardware's lowest / highest, min -1 = the same as max) */
void sys_cpufreq(bool performance, int min_khz, int max_khz);
void sys_cpufreq_on_exit(void);	/* once: on exit / SIGTERM / SIGINT the governor goes back to plain schedutil */
/* how often the sampler thread reads the charger / battery gauge (s); kick = read both now */
void sys_sampler_pace(int charger_s, int battery_s, bool kick);
void utf8_trim(char *s);	/* cut a trailing half character (after snprintf to a byte limit) */
bool sys_charger_online(void);
int sys_charger_ma(void);
int sys_charger_limit_ma(void);
typedef struct {
	int pct;		/* 0..100, -1 = no fuel gauge */
	int ma;			/* battery current, + charging / - discharging */
	int mv, temp_dc, cycles, full_mah, design_mah;	/* temp in 0.1 °C */
	int power_mw;		/* the gauge's average power (signed like ma), 0 = unknown */
	int charge_mah;		/* charge left, 0 = unknown */
} sys_battery_t;
int sys_battery_pct(void);	/* last sample (no hardware read here), -1 = unknown */
void sys_battery_info(sys_battery_t *b);	/* + asks the sampler for the details for the next few seconds */
long sys_uptime_s(void);
void sys_mem_mb(int *total, int *avail);
const char *sys_kernel(void);
int sys_cpu_mhz(int cpu);
int sys_cpu_count(void);
int sys_cpu_load(void);	/* % busy since the previous call */
bool sys_usbnet_up(void);
bool sys_volume_available(void);
int sys_volume_get(void);	/* 0..100 */
void sys_volume_set(int pct);
void sys_volume_route(const char *mixer_dev, const char *ctl, int pct);	/* bt.c: amixer -D dev 'ctl' (NULL = speaker) */
void sys_haptic(int ms);
extern bool sys_haptics_enabled;
void sys_reboot(void);
void sys_poweroff(void);
void sys_clock_format(char *buf, int n, bool with_ampm);	/* current time, honours the 24-Hour pref */

/* ---- backend (hal_*.c) ---- */
lv_display_t *hal_init(int argc, char **argv);
void hal_run(int argc, char **argv);
void hal_haptic(int ms);
void hal_damage(const lv_area_t *a);	/* pixels changed outside LVGL's rendering: show them with the next frame */
void hal_screen(bool on);	/* power.c: off = touch inhibited + not read (keys still are); on = catch up with the fingers */
bool hal_display(bool on);	/* power.c: display pipe off (DPMS) while the UI sleeps; on before the wake redraw;
				 * false = it didn't switch (on: still dark, try again) */
/* hal_wake (any thread): the UI loop wakes and runs the callback hal_wake_set gave it (one; NULL = none) on its
 * next pass — no timer's period in between (the camera's frames). The phone's loop polls an eventfd for it; sdl /
 * snap never call it back, so whoever uses it keeps a timer too */
void hal_wake_set(void (*cb)(void));
void hal_wake(void);

/* ---- power (power.c): screen off = the UI sleeps, auto-lock, CPU boost on input, Low Power Mode ---- */
void power_init(void);		/* shell_init, after the layers exist */
void power_screen(bool off);	/* shell.c screen_set: backlight is the shell's, the rest is here */
bool power_dark(void);		/* screen off and the UI asleep: background timers keep their own (slow) pace */
bool power_tick(void);		/* shell tick (1 s, 30 s while dark): dims / says when auto-lock is due */
bool power_input(int key);	/* every touch frame (0) and key: undims, raises the CPU for a moment; true = it was
				 * dimmed (a touch that only brightens the screen again, like iOS: not delivered) */
/* auto-lock holds ("awake reasons", logged with ORCHARD_POWERLOG): while any is held the screen neither dims nor
 * locks, and when the last one goes the countdown starts from then. keep_awake = held until released;
 * awake_for = held for the next ms (call it again while the reason lasts: a lease can't get stuck). The token is
 * a string literal ("video", "typing", "terminal"). Music playing is not a reason (iOS). */
void power_keep_awake(const char *token, bool on);
void power_awake_for(const char *token, uint32_t ms);
void power_busy(void);		/* a video plays: keep the CPU up for the next 2 s */
int power_backlight(void);	/* backlight level to restore later (the undimmed one while dimmed) */
void power_backlight_on(int level);	/* wake: backlight on — once the display pipe is really on again */
int power_autolock(void);	/* seconds without a touch before the screen goes off, 0 = never */
/* a background timer that keeps running while the UI sleeps (alarms, music, Bluetooth, Wi-Fi, network jobs);
 * dark_ms = its period while dark, the old one comes back on wake; 0 = it paces itself (power_dark()), nothing
 * is restored. Every kept timer runs right after a wake. Every other LVGL timer is paused while dark. */
void power_keep(lv_timer_t *t, uint32_t dark_ms, const char *name);
bool power_lpm(void);		/* Low Power Mode (pref lowpower) */
void power_set_lpm(bool on);
/* Wi-Fi Sleep (prefs wifisleep 0/1, default 0 = off, + wifisleep_min 1/5/15, default 5): once the screen has been
 * dark that long and nothing needs the network, Wi-Fi goes to sleep (wifi_sleep); the screen coming on wakes it at once.
 * Network holds keep it awake (and wake it if it is asleep) — something that needs the network with the screen off
 * that power.c can't see by itself (it already sees music playing, net.c requests, the charger, and curl / wget /
 * ssh / rsync / apk / git processes): hold = until released, net_for = a lease for the next ms (call it
 * again while the reason lasts). Token = a string literal ("sync"). */
void power_net_hold(const char *token, bool on);
void power_net_for(const char *token, uint32_t ms);
typedef struct {
	bool valid;		/* on battery since the charger went away (false: charging / no gauge) */
	long secs, on_secs, off_secs;	/* since unplugged: total, screen on, screen off */
	int pct, used_pct;
	int rate_x10;		/* average drain, 0.1 %/h (0 = not enough data yet) */
} power_usage_t;
void power_usage(power_usage_t *u);
extern unsigned power_wakeups;	/* main-loop passes (hal), for ORCHARD_POWERLOG */

/* ---- app hooks used by the shell ---- */
void clock_init(void);	/* app_clock.c, called once at startup: alarms + timers run with the app closed */
const char *clock_next_alarm(void);	/* "7:30 AM" or NULL — lock screen / control center */
bool clock_timer_running(int *secs_left);
void music_init(void);	/* app_music.c, called once at startup */
bool music_available(void);	/* mpg123 + at least one song */
bool music_playing(void);
const char *music_title(void);	/* NULL when nothing is loaded */
const char *music_artist(void);
void music_toggle(void);
void music_next(void);
void music_prev(void);
void term_run(const char *cmd);	/* open the Terminal and run cmd in its shell */
void music_output_changed(bool keep_playing);	/* bt.c: ALSA default output moved: restart mpg123 at the same spot */
void settings_open(const char *page);	/* launch Settings at a page ("Bluetooth") */

/* ---- iOS flash usage (apfs.c): the APFS container on the ASP flash, opened read-only ---- */
#define APFS_DEV "/dev/asp0n1p1"
#define APFS_MAXVOL 8
typedef struct apfs_info {
	int ok;			/* 1 parsed, 0 no device (flash driver not loaded), -1 unreadable (err) */
	char err[64];
	uint32_t block_size;
	uint64_t total, free, xid;	/* bytes; checkpoint transaction that was read */
	int free_exact;		/* free from the space manager (else total minus the volumes) */
	int nvol;
	struct apfs_vol { char name[64]; unsigned role; uint64_t used; bool encrypted; } vol[APFS_MAXVOL];
} apfs_info_t;
int apfs_read(const char *dev, apfs_info_t *out);	/* blocking (block reads): call off the LVGL thread */

/* ---- Bluetooth (bt.c): BlueZ through one long-lived bluetoothctl in a worker thread, A2DP audio
 * through BlueALSA (the ALSA default PCM follows the connected headphones/speaker). Everything here
 * runs on the LVGL thread and never blocks; listeners run on it too. Previews: fake devices. ---- */
enum bt_kind { BT_OTHER, BT_HEADPHONES, BT_SPEAKER, BT_COMPUTER, BT_PHONE, BT_INPUT, BT_TV, BT_WATCH };
enum bt_busy { BT_IDLE, BT_PAIRING, BT_CONNECTING, BT_DISCONNECTING, BT_REMOVING };
typedef struct bt_device {
	char addr[18], name[64];
	enum bt_kind kind;
	bool audio;		/* A2DP sink: Music can play through it */
	bool named;		/* has a real name (nameless ones only have their address) */
	bool paired, connected;
	int rssi;		/* dBm, 0 = unknown */
	int battery;		/* %, -1 = unknown (BlueZ's Battery1: only while connected, not every device has it) */
	enum bt_busy busy;	/* operation in flight */
} bt_device_t;
void bt_init(void);		/* main.c, once */
bool bt_available(void);	/* bluetoothctl up + a controller */
const char *bt_status(void);	/* "On" / "Off" / why it isn't available ("Not Installed", ...) */
bool bt_powered(void);
void bt_set_power(bool on);
bool bt_scanning(void);
void bt_scan(bool on);		/* discovery + discoverable as "iPhone 6": only while the Bluetooth page is open */
int bt_devices(bt_device_t *out, int max);	/* snapshot: paired first, then in discovery order */
bool bt_device(const char *addr, bt_device_t *out);
void bt_connect(const char *addr);	/* pair + trust + connect as needed; failures show an alert */
void bt_disconnect(const char *addr);
void bt_forget(const char *addr);	/* unpair + remove */
void bt_rename(const char *addr, const char *name);	/* BlueZ alias (what the lists show); "" = back to its own name */
int bt_connected(void);			/* connected devices */
const char *bt_audio_name(void);	/* device Music plays through, NULL = the speaker */
const char *bt_glyph(enum bt_kind k);
void bt_listen(void (*cb)(void), bool on);	/* change listener on/off */

/* ---- Wi-Fi (wifi.c): wpa_supplicant (wlan0) through its control socket, polled by an LVGL timer — never
 * blocks; DHCP is the boot side's wpa_cli action script. On/off = /usr/sbin/wifiup on|off (child process).
 * Networks are named by their raw SSID bytes (`ssid`); `name` is the same made safe to show. Listeners run
 * on the LVGL thread when anything visible changes. Previews: fake networks (ORCHARD_WIFI=off|none|idle). ---- */
enum wifi_state { WIFI_NONE, WIFI_OFF, WIFI_STARTING, WIFI_ON };	/* NONE = no wifiup / wpa_supplicant */
enum wifi_sec { WIFI_OPEN, WIFI_OWE, WIFI_WEP, WIFI_WPA, WIFI_WPA23, WIFI_SAE, WIFI_EAP };
typedef struct wifi_net {
	char ssid[33];		/* raw bytes: what the calls below take */
	char name[100];		/* for labels (invalid UTF-8 → '?') */
	enum wifi_sec sec;
	int signal;		/* dBm of the strongest access point, 0 = not in range */
	int bars;		/* 1..3, 0 = not in range */
	int freq;		/* MHz */
	bool saved, autojoin, current, joining;
} wifi_net_t;
typedef struct wifi_link {
	bool up;
	char ssid[33], name[100], ip[16], mask[16], router[16], dns[64], bssid[18], mac[18];
	enum wifi_sec sec;
	int rssi, bars, freq, speed;	/* dBm, 1..3, MHz, Mb/s (0 = unknown) */
} wifi_link_t;
void wifi_init(void);		/* main.c, once */
enum wifi_state wifi_state(void);
uint32_t wifi_starting_ms(void);	/* how long it has been WIFI_STARTING (wpa_supplicant not answering) */
const char *wifi_status(void);	/* Settings row: network name / "Not Connected" / "Off" / "Not Available" */
void wifi_set_power(bool on);
/* Wi-Fi Sleep (power.c decides when): on = DISCONNECT + wlan0 down, without touching the user's on/off; true = it
 * went to sleep (false: Wi-Fi is off / starting / joining / asleep already). off = wake: wlan0 up, a scan on the old
 * channel, RECONNECT — never blocks. Asleep and while it reconnects the UI shows Wi-Fi on (that network joining). */
bool wifi_sleep(bool on);
bool wifi_asleep(void);		/* asleep, or woken and not connected yet */
/* the network is away because of Wi-Fi Sleep and will be back: asleep (power_net_for wakes it), or woken ≤ 20 s ago
 * and not online yet — net.c holds a new request back meanwhile (else its DNS lookup fails at once) */
bool wifi_sleep_wait(void);
bool wifi_reconnecting(void);	/* woken from Wi-Fi Sleep, not connected again yet (≤ 20 s): the status bar's fan dimmed */
bool wifi_scanning(void);
void wifi_scan(bool on);	/* rescan every ~10 s while on (the Wi-Fi page is open) */
/* current (or joining) first, then saved networks in range, other networks (by signal), saved out of range */
int wifi_networks(wifi_net_t *out, int max);
bool wifi_network(const char *ssid, wifi_net_t *out);
bool wifi_link(wifi_link_t *out);	/* the connected network + IP / router / DNS; false = not connected */
int wifi_bars(void);		/* status bar: 1..3 while connected, else 0 */
/* join a network in range (or a saved one); password NULL/"" = open or saved. Failures (wrong password:
 * the new network is removed again) show an alert. */
void wifi_join(const char *ssid, const char *password);
void wifi_join_hidden(const char *ssid, enum wifi_sec sec, const char *password);	/* "Other…" */
bool wifi_password_ok(enum wifi_sec sec, const char *pw);	/* long enough for that security (Join button) */
void wifi_forget(const char *ssid);
void wifi_set_autojoin(const char *ssid, bool on);
const char *wifi_sec_name(enum wifi_sec s);	/* "WPA2 Personal" */
int wifi_channel(int freq);
void wifi_listen(void (*cb)(void), bool on);
lv_obj_t *wifi_icon(lv_obj_t *parent, int w);	/* iOS Wi-Fi fan, w px wide, in the object's text colour */
void wifi_icon_set(lv_obj_t *icon, int bars);	/* bars above this level dimmed */

/* ---- Tailscale VPN (tailscale.c): /usr/sbin/tsup on|off|status as child processes, reaped by an LVGL timer —
 * never blocks; status is polled only while a bound label or Settings › VPN shows it. Previews: a fake tailnet
 * (ORCHARD_TS=off|login|none|stopped; =real runs ORCHARD_TSUP or /usr/sbin/tsup). ---- */
enum ts_state { TS_NONE, TS_OFF, TS_STOPPED, TS_STARTING, TS_LOGIN, TS_ON };	/* NONE = not installed */
enum ts_state ts_state(void);
const char *ts_status(void);	/* "Connected" / "Needs Login" / "Connecting…" / "Not Connected" / "Off" / "Not Installed" */
void ts_set_power(bool on);
void ts_bind_label(lv_obj_t *label);	/* keep this label's text = ts_status() until it is deleted */
void ts_settings_page(void);	/* push Settings › VPN */

/* ---- helpers ---- */
static inline lv_obj_t *box(lv_obj_t *parent)
{
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
	return o;
}

static inline lv_obj_t *text(lv_obj_t *parent, const char *s, lv_font_t *f, uint32_t color)
{
	lv_obj_t *l = lv_label_create(parent);
	lv_label_set_text(l, s);
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
	return l;
}

/* Nerd Font glyphs used across the shell (UTF-8) */
#define G_CHEVRON_R "\xef\x81\x94"	/* fa-chevron-right */
#define G_CHEVRON_L "\xef\x81\x93"	/* fa-chevron-left */
#define G_CHECK "\xef\x80\x8c"
#define G_SEARCH "\xef\x80\x82"
#define G_CLOSE "\xef\x80\x8d"
#define G_PLUS "\xef\x81\xa7"
#define G_BOLT "\xef\x83\xa7"
#define G_SUN "\xef\x86\x85"
#define G_SUN_FILL "\xf3\xb0\x96\xa8"	/* md-white-balance-sunny: iOS sun.max.fill (G_SUN reads as a gear) */
#define G_VOLUME "\xef\x80\xa8"
#define G_VOLUME_LO "\xef\x80\xa7"
#define G_LOCK "\xef\x80\xa3"
#define G_TERMINAL "\xef\x84\xa0"
#define G_GEAR "\xef\x80\x93"
#define G_WIFI "\xef\x87\xab"
#define G_BLUETOOTH "\xef\x8a\x94"
#define G_PLANE "\xef\x81\xb2"
#define G_USB "\xef\x8a\x87"
#define G_CLOCK "\xef\x80\x97"
#define G_CALENDAR "\xef\x81\xb3"
#define G_BELL "\xef\x83\xb3"
#define G_BELL_SLASH "\xef\x87\xb6"
#define G_MUSIC "\xef\x80\x81"
#define G_PLAY "\xef\x81\x8b"
#define G_PAUSE "\xef\x81\x8c"
#define G_BACKWARD "\xef\x81\x8a"
#define G_FORWARD "\xef\x81\x8e"
#define G_CALC "\xef\x87\xac"
#define G_FOLDER "\xef\x81\xbb"
#define G_FILE "\xef\x85\x9b"
#define G_NOTE "\xef\x89\x8a"	/* fa-sticky-note */
#define G_PEN "\xef\x81\x80"
#define G_TRASH "\xef\x87\xb8"
#define G_INFO "\xef\x84\xa9"
#define G_BATTERY "\xef\x89\x80"
#define G_HAND "\xef\x82\xa6"
#define G_KEYBOARD "\xef\x84\x9c"
#define G_IMAGE "\xef\x80\xbe"
#define G_CAMERA "\xef\x80\xb0"	/* fa-camera (the Camera app's glyph) */
#define G_GLOBE "\xef\x82\xac"
#define G_POWER "\xef\x80\x91"
#define G_MICROCHIP "\xef\x8b\x9b"
#define G_HDD "\xef\x82\xa0"
#define G_STOPWATCH "\xef\x8b\xb2"
#define G_HOURGLASS "\xef\x89\x94"
#define G_ALARM "\xf3\xb0\x80\xa0"	/* md-alarm */
#define G_MOON "\xef\x86\x86"
#define G_LIST "\xef\x80\xba"
#define G_TEXT_SIZE "\xef\x80\xb4"
#define G_SHARE "\xef\x81\xa4"
#define G_HOME "\xef\x80\x95"
#define G_HEADPHONES "\xef\x80\xa5"
#define G_SHIELD "\xef\x84\xb2"	/* fa-shield (VPN) */
