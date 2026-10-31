/* Terminal: libvterm screen on a pty running /bin/sh, iOS-style dark keyboard with a
 * terminal key row (esc/tab/ctrl/arrows; kbd.c), drag to scroll back. The shell keeps running
 * while the app is closed, like an iOS app in the background. */
#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vterm.h"
#include "orchard.h"

#define PAD 10
#define TERM_PX 24
#define SB_LINES 1000
#define DEF_FG th.fg
#define DEF_BG th.bg

static VTerm *vt;
static VTermScreen *vs;
static int pty = -1, rows, cols, cw, ch;
static pid_t child;
static bool dead;
static VTermPos cursor;
static bool cursor_vis = true;
static lv_obj_t *term;	/* NULL while the app is closed */
static lv_timer_t *poll_timer;
static int scroll, drag_acc;	/* lines scrolled back into history */

static VTermScreenCell *sb;	/* scrollback ring, SB_LINES x cols */
static int sb_head, sb_count;

/* ---------- vterm callbacks ---------- */

static bool stale;	/* output came in under the lock screen: one redraw on unlock instead of one per poll */

static void invalidate_rows(int r0, int r1)
{
	lv_area_t a;
	if (!term)
		return;
	if (shell_locked()) {
		stale = true;
		return;
	}
	lv_obj_get_coords(term, &a);
	int top = a.y1;
	a.y1 = top + r0 * ch;
	a.y2 = top + r1 * ch - 1;
	lv_obj_invalidate_area(term, &a);
}

static int cb_damage(VTermRect r, void *u)
{
	(void)u;
	invalidate_rows(r.start_row + scroll, r.end_row + scroll);
	return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *u)
{
	(void)u;
	cursor = pos;
	cursor_vis = visible;
	invalidate_rows(old.row + scroll, old.row + scroll + 1);
	invalidate_rows(pos.row + scroll, pos.row + scroll + 1);
	return 1;
}

static int cb_settermprop(VTermProp p, VTermValue *v, void *u)
{
	(void)u;
	if (p == VTERM_PROP_CURSORVISIBLE)
		cursor_vis = v->boolean;
	return 1;
}

static int cb_bell(void *u)
{
	(void)u;
	sys_haptic(30);
	return 1;
}

static int cb_sb_pushline(int n, const VTermScreenCell *cells, void *u)
{
	(void)u;
	VTermScreenCell *line = sb + sb_head * cols;
	memset(line, 0, cols * sizeof(*line));
	memcpy(line, cells, (n < cols ? n : cols) * sizeof(*line));
	sb_head = (sb_head + 1) % SB_LINES;
	if (sb_count < SB_LINES)
		sb_count++;
	return 1;
}

static const VTermScreenCallbacks cbs = {
	.damage = cb_damage,
	.movecursor = cb_movecursor,
	.settermprop = cb_settermprop,
	.bell = cb_bell,
	.sb_pushline = cb_sb_pushline,
};

/* the pty is polled every 10 ms while output flows or the user types, every 100 ms after 2 s of
 * quiet (the session lives on in the background: it polled 100 times a second forever), every second
 * when quiet with the app closed, whenever the screen is off and while the lock screen is up (lit, over
 * the open Terminal it was 10 / 100 a second; unlock = back at once, lock_changed). The timer paces
 * itself (pace(), every tick): power.c doesn't restore an old period on wake. The 2 s are wall time
 * (they were 200 polls: after a dark spell the fast pace went on for 2 s at 100 a second). */
#define POLL_FAST 10
#define POLL_IDLE 100
#define POLL_BG 1000	/* quiet and the app closed, screen off, locked */
/* a pty holds only ~12 KiB (pty.c caps its buffer at 8 KiB, + n_tty's 4 KiB): read once a second, a job printing
 * faster than that waits on it — a flood got 20 KiB/s through (PC test: 100 ms = 176 KiB/s). Reading more per tick
 * can't help (there is no more), so a tick that finds it full brings the next one at the fast pace, until a tick
 * finds less (same flood: 1.6 MB/s); a quiet or trickling job stays at one wake-up a second */
#define PTY_FULL 8192
#define POLL_DARK_FULL 100	/* screen off: the full-pty fast pace is 100 ms (10 ms was 75 wake-ups a second for a flood
				 * in the pocket); the job still drains (~10x the 1 s pace), never stalls for good */
static bool pty_full;
static uint32_t out_at;	/* last output / key */
static uint32_t cur_period = POLL_FAST;

static void pace(void)
{
	uint32_t want = power_dark() ? (pty_full ? POLL_DARK_FULL : POLL_BG)
		      : shell_locked() ? (pty_full ? POLL_FAST : POLL_BG)
		      : lv_tick_elaps(out_at) < 2000 ? POLL_FAST : term ? POLL_IDLE : POLL_BG;
	if (poll_timer && want != cur_period) {
		cur_period = want;
		lv_timer_set_period(poll_timer, want);
	}
}

static void poll_fast(void)
{
	out_at = lv_tick_get();
	if (poll_timer && cur_period != POLL_FAST && !power_dark() && !shell_locked()) {
		pace();
		lv_timer_ready(poll_timer);	/* the echo of what was just typed: on the next pass */
	}
}

static void out_cb(const char *s, size_t len, void *u)
{
	(void)u;
	poll_fast();	/* the echo comes right back */
	while (len > 0 && pty >= 0) {
		ssize_t n = write(pty, s, len);
		if (n <= 0)
			break;
		s += n;
		len -= n;
	}
}

/* ---------- session ---------- */

static void poll_cb(lv_timer_t *t);

static void lock_changed(bool locked)
{
	pace();
	if (!locked && poll_timer && !power_dark())
		lv_timer_ready(poll_timer);	/* what came in during the last second shows at once */
	if (!locked && stale && term)
		lv_obj_invalidate(term);
	if (!locked)
		stale = false;
}

static void session_start(void)
{
	free(sb);
	sb = calloc(SB_LINES * cols, sizeof(*sb));
	sb_head = sb_count = scroll = 0;
	cursor = (VTermPos){ 0, 0 };	/* a restart redraws everything: the dead session's cursor must not survive it */
	cursor_vis = true;

	vt = vterm_new(rows, cols);
	vterm_set_utf8(vt, 1);
	vs = vterm_obtain_screen(vt);
	vterm_screen_set_callbacks(vs, &cbs, NULL);
	vterm_screen_set_damage_merge(vs, VTERM_DAMAGE_SCROLL);
	vterm_screen_enable_altscreen(vs, 1);
	vterm_screen_reset(vs, 1);
	vterm_output_set_callback(vt, out_cb, NULL);
	/* the theme's 16 ANSI colours (same as Ghostty on the PC); bold = bright */
	VTermState *st = vterm_obtain_state(vt);
	for (int i = 0; i < 16; i++) {
		VTermColor c;
		uint32_t p = th.ansi[i];
		vterm_color_rgb(&c, p >> 16, p >> 8 & 0xff, p & 0xff);
		vterm_state_set_palette_color(st, i, &c);
	}
	vterm_state_set_bold_highbright(st, 1);

	struct winsize ws = { .ws_row = rows, .ws_col = cols, .ws_xpixel = cols * cw, .ws_ypixel = rows * ch };
	child = forkpty(&pty, NULL, NULL, &ws);
	if (child == 0) {
		setenv("TERM", "xterm-256color", 1);
		setenv("PS1", "\\[\\e[1;36m\\]iphone6\\[\\e[0m\\]:\\w\\$ ", 1);
		if (!getenv("HOME"))
			setenv("HOME", "/", 1);
		if (chdir(getenv("HOME")) < 0 && chdir("/") < 0)
			_exit(126);
		execl("/bin/sh", "sh", (char *)NULL);
		_exit(127);
	}
	fcntl(pty, F_SETFL, O_NONBLOCK);
	dead = false;
	if (!poll_timer) {
		poll_timer = lv_timer_create(poll_cb, POLL_FAST, NULL);
		power_keep(poll_timer, 0, "terminal");	/* screen off: a job's output still drains (no stall on a full pty);
						 * it paces itself (pace) */
		lock_listen(lock_changed, true);
	} else {
		out_at = lv_tick_get();
		lv_timer_resume(poll_timer);	/* paused while the session was dead (poll_cb) */
		pace();
		lv_timer_ready(poll_timer);
	}
}

static void session_end(void)
{
	if (pty >= 0)
		close(pty);
	pty = -1;
	vterm_free(vt);
	vt = NULL;
	vs = NULL;
}

static void poll_cb(lv_timer_t *t)
{
	(void)t;
	char buf[16384];
	ssize_t n = 0;
	int total = 0;
	if (dead || pty < 0) {
		lv_timer_pause(t);	/* nothing to read (it polled 100 times a second after the shell exited / the
					 * app was killed): session_start resumes it */
		return;
	}
	while (total < 256 * 1024 && (n = read(pty, buf, sizeof(buf))) > 0) {
		vterm_input_write(vt, buf, n);
		total += n;
	}
	if (total && term && shell_current_app() == &app_terminal && !shell_locked())
		power_awake_for("terminal", 3000);	/* a command printing in front of you: no auto-lock */
	if (total)
		poll_fast();
	pty_full = total >= PTY_FULL;
	pace();
	if (total) {
		vterm_screen_flush_damage(vs);
		if (scroll) {
			scroll = 0;
			if (term && shell_locked())
				stale = true;
			else if (term)
				lv_obj_invalidate(term);
		}
	}
	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR) || waitpid(child, NULL, WNOHANG) == child) {
		const char *msg = "\r\n\x1b[2m[process exited \xe2\x80\x94 tap to restart]\x1b[0m";
		vterm_input_write(vt, msg, strlen(msg));
		vterm_screen_flush_damage(vs);
		dead = true;
		lv_timer_pause(t);
	}
}

/* ---------- drawing ---------- */

static void get_cell(int row, int col, VTermScreenCell *c)
{
	int line = row - scroll;
	if (line >= 0) {
		vterm_screen_get_cell(vs, (VTermPos){ line, col }, c);
		return;
	}
	int k = -line - 1;	/* 0 = newest history line */
	if (k >= sb_count) {
		memset(c, 0, sizeof(*c));
		c->width = 1;
		c->fg.type = VTERM_COLOR_DEFAULT_FG;
		c->bg.type = VTERM_COLOR_DEFAULT_BG;
		return;
	}
	*c = sb[((sb_head - 1 - k + SB_LINES) % SB_LINES) * cols + col];
	if (!c->width)
		c->width = 1;
}

static uint32_t rgb(VTermColor col, uint32_t def)
{
	if (VTERM_COLOR_IS_DEFAULT_FG(&col) || VTERM_COLOR_IS_DEFAULT_BG(&col))
		return def;
	vterm_screen_convert_color_to_rgb(vs, &col);
	return col.rgb.red << 16 | col.rgb.green << 8 | col.rgb.blue;
}

static void cell_colors(const VTermScreenCell *c, uint32_t *fg, uint32_t *bg)
{
	*fg = rgb(c->fg, DEF_FG);
	*bg = rgb(c->bg, DEF_BG);
	if (c->attrs.reverse) {
		uint32_t t = *fg;
		*fg = *bg;
		*bg = t;
	}
}

static int put_utf8(char *o, uint32_t cp)
{
	if (cp < 0x80) { o[0] = cp; return 1; }
	if (cp < 0x800) { o[0] = 0xc0 | cp >> 6; o[1] = 0x80 | (cp & 0x3f); return 2; }
	if (cp < 0x10000) { o[0] = 0xe0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 0x3f); o[2] = 0x80 | (cp & 0x3f); return 3; }
	o[0] = 0xf0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 0x3f); o[2] = 0x80 | (cp >> 6 & 0x3f); o[3] = 0x80 | (cp & 0x3f);
	return 4;
}

static void draw_text(lv_layer_t *layer, const char *s, int x, int y, uint32_t color)
{
	lv_draw_label_dsc_t d;
	lv_draw_label_dsc_init(&d);
	d.text = s;
	d.text_local = 1;
	d.font = font(F_MONO, TERM_PX);
	d.color = lv_color_hex(color);
	lv_area_t a = { x, y, x + SCR_W, y + ch - 1 };
	lv_draw_label(layer, &d, &a);
}

static void fill(lv_layer_t *layer, int x, int y, int w, int h, uint32_t color, lv_opa_t opa)
{
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_color = lv_color_hex(color);
	d.bg_opa = opa;
	lv_area_t a = { x, y, x + w - 1, y + h - 1 };
	lv_draw_rect(layer, &d, &a);
}

static void draw_cb(lv_event_t *e)
{
	lv_layer_t *layer = lv_event_get_layer(e);
	lv_area_t co;
	VTermScreenCell c;
	char buf[1024];
	if (!vs)
		return;
	lv_obj_get_coords(term, &co);
	int x0 = co.x1 + PAD, y0 = co.y1;
	for (int r = 0; r < rows; r++) {
		int y = y0 + r * ch;
		for (int col = 0; col < cols;) {
			uint32_t fg, bg, f2, b2;
			get_cell(r, col, &c);
			cell_colors(&c, &fg, &bg);
			int c0 = col, n = 0;
			bool ink = false;
			while (col < cols) {
				get_cell(r, col, &c);
				cell_colors(&c, &f2, &b2);
				if (f2 != fg || b2 != bg || n > (int)sizeof(buf) - 32)
					break;
				if (c.chars[0] && c.chars[0] != ' ') {
					ink = true;
					for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && c.chars[i]; i++)
						n += put_utf8(buf + n, c.chars[i]);
				} else {
					buf[n++] = ' ';
				}
				col += c.width > 0 ? c.width : 1;
			}
			buf[n] = 0;
			if (bg != DEF_BG)
				fill(layer, x0 + c0 * cw, y, (col - c0) * cw, ch, bg, LV_OPA_COVER);
			if (ink)
				draw_text(layer, buf, x0 + c0 * cw, y, fg);
		}
	}
	if (cursor_vis && !dead && scroll == 0) {
		int x = x0 + cursor.col * cw, y = y0 + cursor.row * ch;
		fill(layer, x, y, cw, ch, th.accent, LV_OPA_COVER);
		vterm_screen_get_cell(vs, cursor, &c);
		if (c.chars[0] && c.chars[0] != ' ') {
			buf[put_utf8(buf, c.chars[0])] = 0;
			draw_text(layer, buf, x, y, th.bg);
		}
	}
}

static const text_client_t client;	/* (the keyboard's, below) */

static void term_touch(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_CLICKED && dead) {
		session_end();
		session_start();
		lv_obj_invalidate(term);
	} else if (code == LV_EVENT_CLICKED && !kbd_visible()) {
		kbd_show(KBD_TERM, &client);	/* a tap brings the keyboard back, whatever took it away */
	} else if (code == LV_EVENT_PRESSED) {
		drag_acc = 0;
	} else if (code == LV_EVENT_PRESSING) {
		lv_point_t v;
		lv_indev_get_vect(lv_indev_active(), &v);
		drag_acc += v.y;
		int lines = drag_acc / ch;
		if (lines) {
			drag_acc -= lines * ch;
			int s = scroll + lines;
			s = s < 0 ? 0 : s > sb_count ? sb_count : s;
			if (s != scroll) {
				scroll = s;
				lv_obj_invalidate(term);
			}
		}
	}
}

/* ---------- input ---------- */

static void back_to_bottom(void)
{
	if (scroll) {
		scroll = 0;
		lv_obj_invalidate(term);
	}
}

static void send_text(const char *s, bool ctrl)
{
	if (!vt || dead)
		return;
	back_to_bottom();
	while (*s) {
		uint32_t cp = (unsigned char)*s++;
		int more = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : cp >= 0xc0 ? 1 : 0;
		if (more)
			cp &= 0x3f >> more;
		while (more-- && *s)
			cp = cp << 6 | (*s++ & 0x3f);
		vterm_keyboard_unichar(vt, cp, ctrl ? VTERM_MOD_CTRL : VTERM_MOD_NONE);
	}
}

static void send_key(int tk)
{
	static const VTermKey map[] = {
		[TK_BKSP] = VTERM_KEY_BACKSPACE, [TK_ENTER] = VTERM_KEY_ENTER, [TK_TAB] = VTERM_KEY_TAB,
		[TK_ESC] = VTERM_KEY_ESCAPE, [TK_UP] = VTERM_KEY_UP, [TK_DOWN] = VTERM_KEY_DOWN,
		[TK_LEFT] = VTERM_KEY_LEFT, [TK_RIGHT] = VTERM_KEY_RIGHT,
	};
	if (!vt || dead || tk <= 0 || tk >= (int)(sizeof(map) / sizeof(map[0])))
		return;
	back_to_bottom();
	vterm_keyboard_key(vt, map[tk], VTERM_MOD_NONE);
}

static const text_client_t client = { send_text, send_key };

/* the shell keeps running while the app is closed: commands typed elsewhere (Spotlight) go
 * straight to it, or wait for the session to start */
static char pending[256];

void term_run(const char *cmd)
{
	snprintf(pending, sizeof(pending), "%s", cmd);
	if (vt && !dead) {
		send_text(pending, false);
		send_key(TK_ENTER);
		pending[0] = 0;
	}
	shell_launch(&app_terminal);
}

/* ---------- app ---------- */

static void app_open(lv_obj_t *root)
{
	int kb_h = kbd_height(KBD_TERM);
	if (!cw) {
		cw = lv_font_get_glyph_width(font(F_MONO, TERM_PX), 'M', 0);
		ch = lv_font_get_line_height(font(F_MONO, TERM_PX));
		cols = (SCR_W - 2 * PAD) / cw;
		rows = (SCR_H - STATUS_H - kb_h) / ch;
	}
	if (!vt)
		session_start();
	if (pending[0]) {
		send_text(pending, false);
		send_key(TK_ENTER);
		pending[0] = 0;
	}

	term = box(root);
	lv_obj_set_pos(term, 0, STATUS_H);
	lv_obj_set_size(term, SCR_W, SCR_H - STATUS_H - kb_h);
	lv_obj_add_flag(term, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(term, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_add_event_cb(term, term_touch, LV_EVENT_ALL, NULL);
	kbd_show(KBD_TERM, &client);
	poll_fast();	/* from the 1 s background pace */
}

static void app_close(void)
{
	term = NULL;
}

/* swiped away in the app switcher: end the shell */
static void app_kill(void)
{
	if (!vt)
		return;
	if (child > 0 && !dead)
		kill(child, SIGHUP);
	if (child > 0)
		waitpid(child, NULL, 0);
	child = 0;
	session_end();
	dead = true;
}

const app_t app_terminal = {
	.name = "Terminal",
	.glyph = ">_",
	.icon = IC_DARK,
	.open = app_open,
	.close = app_close,
	.kill = app_kill,
};
