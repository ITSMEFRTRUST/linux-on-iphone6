/* Notes, iOS-style: the list is grouped by date (Today, Yesterday, Previous 7 days, months, years)
 * with a search field that filters on the whole text; a note opens in a full-page editor that
 * types through the shared keyboard. Notes are plain text files, data_dir()/notes/<id>.txt —
 * the first line is the title, the file's mtime is "last edited". Saving is automatic: 1 s after
 * the last keystroke, when the keyboard goes away and when the page is left; empty notes are
 * discarded. */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "orchard.h"

#define SIDE 32			/* list inset (ui.c) */
#define TOP (STATUS_H + NAV_H)	/* page content starts here */
#define BTN 84			/* round toolbar buttons */
#define TB_H (BTN + 30 + BOTTOM_PAD)	/* bottom toolbar, fade included */
#define ED_PAD 36		/* editor text inset */
#define DATE_H 72		/* editor: date line above the text */
#define MAX_NOTES 500
#define MAX_ROWS 150		/* DESIGN.md: keep lists to ~150 rows */
#define MAX_BYTES (512 * 1024)	/* bigger files are cut when read */
#define SAVE_MS 1000

#define G_COMPOSE "\xf3\xb0\xa4\x8c"	/* md-square_edit_outline */
#define G_BIN "\xf3\xb0\xa9\xba"	/* md-trash_can_outline */
#define G_CLEAR "\xf3\xb0\x85\x99"	/* md-close_circle */

struct note { char id[24]; time_t mtime; char *text; };
static struct note notes[MAX_NOTES];
static int nnotes;

/* list page */
static lv_obj_t *list_col, *list_box, *count_lbl, *search_ta, *search_clear, *search_cancel;
static char query[128];
static lv_timer_t *filter_timer, *save_timer;
static char del_id[24];

/* editor page */
static lv_obj_t *ed_page, *ed_ta, *ed_date, *ed_done, *ed_tb;
static char ed_id[24];
static bool ed_loading, ed_dirty, ed_deleted;
static enum { ED_PUSHED, ED_SLIDING_IN, ED_SHOWN, ED_LEAVING } ed_phase;

/* ---------- small helpers ---------- */

static bool blank(const char *s)
{
	for (; *s; s++)
		if (!isspace((unsigned char)*s))
			return false;
	return true;
}

/* cut s to at most len bytes without splitting a UTF-8 sequence */
static void utf8_cut(char *s, int len)
{
	int j = len - 1;
	while (j >= 0 && ((unsigned char)s[j] & 0xc0) == 0x80)
		j--;
	if (j >= 0) {
		unsigned char c = s[j];
		int need = c < 0xc0 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2;
		if (len - j < need)
			len = j;
	}
	s[len < 0 ? 0 : len] = 0;
}

/* title = first line with text; preview = what follows, on one line */
static void summary(const char *t, char *title, int tn, char *prev, int pn)
{
	while (*t && isspace((unsigned char)*t))
		t++;
	const char *e = strchr(t, '\n');
	if (!e)
		e = t + strlen(t);
	int n = e - t;
	while (n > 0 && isspace((unsigned char)t[n - 1]))
		n--;
	if (n > tn - 1)
		n = tn - 1;
	memcpy(title, t, n);
	utf8_cut(title, n);
	if (!title[0])
		snprintf(title, tn, "New Note");
	int k = 0;
	bool gap = false;
	for (const char *p = e; *p && k < pn - 2; p++) {
		if (isspace((unsigned char)*p)) {
			gap = k > 0;
			continue;
		}
		if (gap)
			prev[k++] = ' ';
		gap = false;
		prev[k++] = *p;
	}
	utf8_cut(prev, k);
}

/* ---------- dates ---------- */

static const char *const MONTHS[] = { "January", "February", "March", "April", "May", "June", "July",
				      "August", "September", "October", "November", "December" };
static const char *const WDAYS[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static time_t day_start(time_t t)
{
	struct tm tm;
	localtime_r(&t, &tm);
	tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
	tm.tm_isdst = -1;
	return mktime(&tm);
}

static void fmt_clock(char *b, int n, const struct tm *tm)
{
	if (prefs_int("clock24", 0))
		snprintf(b, n, "%02d:%02d", tm->tm_hour, tm->tm_min);
	else
		snprintf(b, n, "%d:%02d %s", (tm->tm_hour + 11) % 12 + 1, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
}

/* list rows: 2:32 PM / Yesterday / Tuesday / 9/14/26 */
static void fmt_short(char *b, int n, time_t t)
{
	time_t today = day_start(time(NULL)), yday = day_start(today - 12 * 3600);
	struct tm tm;
	localtime_r(&t, &tm);
	if (t >= today)
		fmt_clock(b, n, &tm);
	else if (t >= yday)
		snprintf(b, n, "Yesterday");
	else if (t >= today - 6 * 86400)
		snprintf(b, n, "%s", WDAYS[tm.tm_wday]);
	else
		snprintf(b, n, "%d/%d/%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year % 100);
}

/* editor: October 18, 2026 at 2:32 PM */
static void fmt_long(char *b, int n, time_t t)
{
	struct tm tm;
	char c[16];
	localtime_r(&t, &tm);
	fmt_clock(c, sizeof(c), &tm);
	snprintf(b, n, "%s %d, %d at %s", MONTHS[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900, c);
}

/* list sections, like iOS */
static void fmt_group(char *b, int n, time_t t)
{
	time_t now = time(NULL), today = day_start(now), yday = day_start(today - 12 * 3600);
	struct tm tm, tn;
	localtime_r(&t, &tm);
	localtime_r(&now, &tn);
	if (t >= today)
		snprintf(b, n, "Today");
	else if (t >= yday)
		snprintf(b, n, "Yesterday");
	else if (t >= today - 7 * 86400)
		snprintf(b, n, "Previous 7 days");
	else if (t >= today - 30 * 86400)
		snprintf(b, n, "Previous 30 days");
	else if (tm.tm_year == tn.tm_year)
		snprintf(b, n, "%s", MONTHS[tm.tm_mon]);
	else
		snprintf(b, n, "%d", tm.tm_year + 1900);
}

/* ---------- storage ---------- */

static const char *notes_dir(void)
{
	static char d[300];
	snprintf(d, sizeof(d), "%s/notes", data_dir());
	return d;
}

static void note_path(char *b, int n, const char *id) { snprintf(b, n, "%s/%s.txt", notes_dir(), id); }

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char *buf = malloc(MAX_BYTES + 1);
	size_t n = buf ? fread(buf, 1, MAX_BYTES, f) : 0;
	fclose(f);
	if (!buf)
		return NULL;
	buf[n] = 0;
	utf8_cut(buf, strlen(buf));
	char *r = realloc(buf, strlen(buf) + 1);
	return r ? r : buf;
}

static bool write_file(const char *id, const char *text)
{
	char path[400], tmp[410];
	mkdir(notes_dir(), 0755);
	note_path(path, sizeof(path), id);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	FILE *f = fopen(tmp, "wb");
	if (!f)
		return false;
	size_t n = strlen(text);
	bool ok = fwrite(text, 1, n, f) == n;
	ok = (fclose(f) == 0) && ok;
	if (!ok || rename(tmp, path)) {
		unlink(tmp);
		return false;
	}
	return true;
}

static int by_mtime(const void *a, const void *b)
{
	const struct note *x = a, *y = b;
	if (x->mtime != y->mtime)
		return x->mtime < y->mtime ? 1 : -1;
	return strcmp(y->id, x->id);
}

static int note_find(const char *id)
{
	for (int i = 0; i < nnotes; i++)
		if (!strcmp(notes[i].id, id))
			return i;
	return -1;
}

static void note_set(const char *id, const char *text, time_t mtime)
{
	int i = note_find(id);
	if (i < 0) {
		if (nnotes == MAX_NOTES)
			return;
		i = nnotes++;
		snprintf(notes[i].id, sizeof(notes[i].id), "%s", id);
		notes[i].text = NULL;
	}
	free(notes[i].text);
	notes[i].text = strdup(text);
	notes[i].mtime = mtime;
	if (!notes[i].text) {	/* out of memory: drop it from the list, the file is still there */
		notes[i] = notes[--nnotes];
		return;
	}
	qsort(notes, nnotes, sizeof(notes[0]), by_mtime);
}

static void note_delete(const char *id)
{
	char path[400];
	note_path(path, sizeof(path), id);
	unlink(path);
	int i = note_find(id);
	if (i < 0)
		return;
	free(notes[i].text);
	memmove(notes + i, notes + i + 1, (nnotes - i - 1) * sizeof(notes[0]));
	nnotes--;
}

static void new_id(char *out, int n)
{
	char path[400];
	for (long long t = time(NULL);; t++) {
		snprintf(out, n, "%lld", t);
		note_path(path, sizeof(path), out);
		if (note_find(out) < 0 && access(path, F_OK) != 0 && strcmp(out, ed_id))
			return;
	}
}

static const char welcome[] =
	"Welcome to Orchard\n"
	"Notes saves as you type \xe2\x80\x94 there is no Save button.\n"
	"\n"
	"\xe2\x80\xa2 Tap the compose button at the bottom right to start a new note. Its first line "
	"becomes the title.\n"
	"\xe2\x80\xa2 Tap anywhere in a note to put the cursor there and type. Done hides the keyboard.\n"
	"\xe2\x80\xa2 Search, at the top of the list, looks inside every note.\n"
	"\xe2\x80\xa2 To delete a note, hold it in the list, or tap the trash button inside it.\n"
	"\xe2\x80\xa2 Swipe in from the left edge to go back.\n";

static void notes_load(void)
{
	for (int i = 0; i < nnotes; i++)
		free(notes[i].text);
	nnotes = 0;
	struct stat st;
	if (stat(notes_dir(), &st) != 0) {	/* first run */
		mkdir(notes_dir(), 0755);
		char id[24];
		snprintf(id, sizeof(id), "%lld", (long long)time(NULL));
		write_file(id, welcome);
	}
	DIR *d = opendir(notes_dir());
	struct dirent *e;
	while (d && (e = readdir(d)) && nnotes < MAX_NOTES) {
		int len = strlen(e->d_name);
		if (e->d_name[0] == '.' || len < 5 || len - 4 >= (int)sizeof(notes[0].id) ||
		    strcmp(e->d_name + len - 4, ".txt"))
			continue;
		char path[400];
		snprintf(path, sizeof(path), "%s/%s", notes_dir(), e->d_name);
		if (stat(path, &st) || !S_ISREG(st.st_mode))
			continue;
		char *t = read_file(path);
		if (!t)
			continue;
		if (blank(t)) {	/* empty notes are discarded */
			free(t);
			unlink(path);
			continue;
		}
		struct note *n = &notes[nnotes++];
		snprintf(n->id, sizeof(n->id), "%.*s", len - 4, e->d_name);
		n->mtime = st.st_mtime;
		n->text = t;
	}
	if (d)
		closedir(d);
	qsort(notes, nnotes, sizeof(notes[0]), by_mtime);
}

/* ---------- shared bits ---------- */

/* bottom toolbar: fades in over the content, then solid (no Liquid Glass) */
static lv_obj_t *toolbar(lv_obj_t *page, uint32_t bg)
{
	lv_obj_t *tb = box(page);
	lv_obj_set_size(tb, SCR_W, TB_H);
	lv_obj_align(tb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(tb, C(bg), 0);
	lv_obj_set_style_bg_grad_color(tb, C(bg), 0);
	lv_obj_set_style_bg_grad_dir(tb, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_main_opa(tb, LV_OPA_TRANSP, 0);
	lv_obj_set_style_bg_grad_opa(tb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_main_stop(tb, 0, 0);
	lv_obj_set_style_bg_grad_stop(tb, 70, 0);
	lv_obj_set_style_pad_hor(tb, SIDE, 0);
	lv_obj_set_style_pad_bottom(tb, BOTTOM_PAD + 6, 0);
	return tb;
}

static lv_obj_t *tool_button(lv_obj_t *tb, const char *glyph, uint32_t page_bg, lv_align_t align, lv_event_cb_t cb)
{
	uint32_t bg = page_bg == pal.cell ? pal.fill : pal.cell;
	lv_obj_t *b = ui_icon_button(tb, glyph, BTN, bg, pal.tint);
	lv_obj_set_style_text_font(lv_obj_get_child(b, 0), font(F_MONO, 44), 0);
	ui_glyph_fit(lv_obj_get_child(b, 0));	/* fitted again in its font */
	lv_obj_align(b, align, 0, 0);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	return b;
}

/* text before the cursor → keyboard suggestions + auto-capitals */
static void kbd_context(lv_obj_t *ta)
{
	const char *t = lv_textarea_get_text(ta);
	int pos = lv_textarea_get_cursor_pos(ta), b = 0;
	for (int i = 0; t[b] && i < pos; i++) {	/* cursor position is in characters */
		b++;
		while (((unsigned char)t[b] & 0xc0) == 0x80)
			b++;
	}
	char buf[160];
	int start = b > (int)sizeof(buf) - 1 ? b - (int)sizeof(buf) + 1 : 0;
	while (start < b && ((unsigned char)t[start] & 0xc0) == 0x80)
		start++;
	snprintf(buf, sizeof(buf), "%.*s", b - start, t + start);
	kbd_set_context(buf);
}

/* ---------- list ---------- */

static void editor_open(int idx);

static void row_click(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (i >= 0 && i < nnotes)
		editor_open(i);
}

static void del_confirmed(int idx, void *ud);

static void row_hold(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (i < 0 || i >= nnotes)
		return;
	char title[48], prev[8], msg[96];
	summary(notes[i].text, title, sizeof(title), prev, sizeof(prev));
	snprintf(msg, sizeof(msg), "\xe2\x80\x9c%s\xe2\x80\x9d will be deleted.", title);
	snprintf(del_id, sizeof(del_id), "%s", notes[i].id);
	ui_alert("Delete Note?", msg, (const char *[]){ "Cancel", "!Delete" }, 2, del_confirmed, NULL);
}

static void note_row(lv_obj_t *sec, int i)
{
	char title[128], prev[160], date[32];
	summary(notes[i].text, title, sizeof(title), prev, sizeof(prev));
	fmt_short(date, sizeof(date), notes[i].mtime);
	lv_obj_t *r = ui_row(sec, NULL, 0, title);
	lv_obj_t *col = lv_obj_get_user_data(r);
	lv_obj_t *t = lv_obj_get_child(col, 0);
	lv_obj_set_style_text_font(t, font(F_TEXT_SEMI, 34), 0);
	lv_obj_set_height(t, lv_font_get_line_height(font(F_TEXT_SEMI, 34)));	/* one line, then … */
	lv_obj_t *line = box(col);
	lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(line, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(line, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(line, 14, 0);
	text(line, date, font(F_TEXT, 28), mix(pal.label, pal.label2, 45));
	lv_obj_t *p = text(line, prev[0] ? prev : "No additional text", font(F_TEXT, 28), pal.label2);
	lv_obj_set_flex_grow(p, 1);
	lv_obj_set_height(p, lv_font_get_line_height(font(F_TEXT, 28)));	/* one line, then … */
	lv_label_set_long_mode(p, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(r, C(pal.cell_hi), LV_STATE_PRESSED);
	/* SHORT_CLICKED: a hold opens the delete prompt instead (CLICKED would also follow it) */
	lv_obj_add_event_cb(r, row_click, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
	lv_obj_add_event_cb(r, row_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
}

static lv_obj_t *group_section(const char *title)
{
	lv_obj_t *s = ui_section(list_box, title, NULL);
	lv_obj_t *h = lv_obj_get_child(list_box, lv_obj_get_index(s) - 1);
	if (h && lv_obj_check_type(h, &lv_label_class)) {	/* iOS Notes: bold date headers */
		lv_obj_set_style_text_font(h, font(F_TEXT_BOLD, 40), 0);
		lv_obj_set_style_text_color(h, C(pal.label), 0);
		lv_obj_set_style_pad_top(h, lv_obj_get_index(h) == 0 ? 30 : 44, 0);
		lv_obj_set_style_pad_bottom(h, 14, 0);
	}
	return s;
}

static void empty_state(const char *title, const char *hint)
{
	lv_obj_t *t = text(list_box, title, font(F_TEXT_BOLD, 44), pal.label2);
	lv_obj_set_style_pad_top(t, 260, 0);
	lv_obj_t *h = text(list_box, hint, font(F_TEXT, 30), pal.label2);
	lv_obj_set_width(h, SCR_W - 2 * SIDE);
	lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_pad_top(h, 14, 0);
}

static void list_build(void)
{
	if (!list_box)
		return;
	lv_obj_clean(list_box);
	char buf[96];
	snprintf(buf, sizeof(buf), "%d Note%s", nnotes, nnotes == 1 ? "" : "s");
	lv_label_set_text(count_lbl, nnotes ? buf : "");
	int shown = 0, hits = 0;
	char group[32] = "", g[32];
	lv_obj_t *sec = NULL;
	for (int i = 0; i < nnotes; i++) {
		if (query[0] && !strcasestr(notes[i].text, query))
			continue;
		hits++;
		if (shown == MAX_ROWS)
			continue;
		fmt_group(g, sizeof(g), notes[i].mtime);
		if (!sec || strcmp(g, group)) {
			snprintf(group, sizeof(group), "%s", g);
			sec = group_section(g);
		}
		note_row(sec, i);
		shown++;
	}
	if (!nnotes) {
		empty_state("No Notes", "Tap the compose button to write one.");
	} else if (!hits) {
		empty_state("No Results", "Check the spelling or try a new search.");
	} else if (hits > shown) {
		snprintf(buf, sizeof(buf), "Showing the newest %d of %d. Search to find the others.", shown, hits);
		lv_obj_t *f = text(list_box, buf, font(F_TEXT, 26), pal.label2);
		lv_obj_set_style_pad_top(f, 20, 0);
	}
}

static void del_confirmed(int idx, void *ud)
{
	(void)ud;
	if (idx != 1 || !del_id[0])
		return;
	note_delete(del_id);
	del_id[0] = 0;
	list_build();
}

static void compose_click(lv_event_t *e)
{
	(void)e;
	editor_open(-1);
}

/* search: filters as you type (rebuilt after a short pause), Cancel ends it */
static void filter_tick(lv_timer_t *t)
{
	lv_timer_pause(t);
	list_build();
	if (list_col)
		lv_obj_scroll_to_y(list_col, 0, LV_ANIM_OFF);
}

static bool search_active(void) { return kbd_visible() && lv_obj_has_state(search_ta, LV_STATE_FOCUSED); }

static void search_chrome(void)
{
	bool on = search_active();
	if (on || query[0])
		lv_obj_remove_flag(search_cancel, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(search_cancel, LV_OBJ_FLAG_HIDDEN);
	if (query[0])
		lv_obj_remove_flag(search_clear, LV_OBJ_FLAG_HIDDEN);
	else
		lv_obj_add_flag(search_clear, LV_OBJ_FLAG_HIDDEN);
	/* rows can scroll up out from under the keyboard */
	lv_obj_set_style_pad_bottom(list_col, on ? kbd_height(KBD_TEXT) + 40 : TB_H + 40, 0);
}

static void search_changed(lv_event_t *e)
{
	(void)e;
	snprintf(query, sizeof(query), "%s", lv_textarea_get_text(search_ta));
	search_chrome();
	lv_timer_reset(filter_timer);
	lv_timer_resume(filter_timer);
}

static void search_focus(lv_event_t *e)
{
	(void)e;
	if (search_active())
		kbd_no_autocap();	/* EV_KBD_SHOWN comes from kbd_attach, after the keyboard is up */
	search_chrome();
}

static void search_ready(lv_event_t *e)
{
	(void)e;
	kbd_hide();
}

static void search_clear_click(lv_event_t *e)
{
	(void)e;
	lv_textarea_set_text(search_ta, "");
	kbd_attach(search_ta, KBD_TEXT);
}

static void search_cancel_click(lv_event_t *e)
{
	(void)e;
	lv_textarea_set_text(search_ta, "");
	kbd_hide();
	search_chrome();
}

static void build_search(lv_obj_t *c)
{
	lv_obj_t *row = box(c);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(row, SCR_W - 2 * SIDE, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 24, 0);
	search_ta = ui_search(row, SCR_W - 2 * SIDE, "Search");
	lv_obj_t *wrap = lv_obj_get_parent(search_ta);
	lv_obj_set_flex_grow(wrap, 1);
	lv_obj_remove_flag(search_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);	/* focus = keyboard attached, nothing else */
	search_clear = ui_icon(wrap, G_CLEAR, font(F_MONO, 36), pal.label3);
	lv_obj_add_flag(search_clear, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_ext_click_area(search_clear, 20);
	lv_obj_add_event_cb(search_clear, search_clear_click, LV_EVENT_CLICKED, NULL);
	search_cancel = text(row, "Cancel", font(F_TEXT, 34), pal.tint);
	lv_obj_add_flag(search_cancel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_ext_click_area(search_cancel, 20);
	lv_obj_set_style_opa(search_cancel, LV_OPA_50, LV_STATE_PRESSED);
	lv_obj_add_event_cb(search_cancel, search_cancel_click, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(search_ta, search_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(search_ta, search_focus, EV_KBD_SHOWN, NULL);
	lv_obj_add_event_cb(search_ta, search_focus, EV_KBD_HIDDEN, NULL);
	lv_obj_add_event_cb(search_ta, search_ready, LV_EVENT_READY, NULL);
}

/* ---------- editor ---------- */

static bool ed_editing(void) { return ed_ta && kbd_visible() && lv_obj_has_state(ed_ta, LV_STATE_FOCUSED); }

static void ed_set_date(time_t t)
{
	char b[64];
	fmt_long(b, sizeof(b), t);
	lv_label_set_text(ed_date, b);
}

static void ed_save(void)
{
	if (!ed_ta || ed_deleted || !ed_dirty)
		return;
	ed_dirty = false;
	lv_timer_pause(save_timer);
	const char *t = lv_textarea_get_text(ed_ta);
	int i = note_find(ed_id);
	if (blank(t)) {	/* emptied: an empty note isn't kept */
		if (i < 0)
			return;
		note_delete(ed_id);
	} else {
		if (i >= 0 && !strcmp(notes[i].text, t))
			return;
		if (!write_file(ed_id, t)) {
			ui_alert("Couldn\xe2\x80\x99t Save Note", "The notes folder can\xe2\x80\x99t be written to.",
				 (const char *[]){ "OK" }, 1, NULL, NULL);
			return;
		}
		time_t now = time(NULL);
		note_set(ed_id, t, now);
		ed_set_date(now);
	}
	list_build();	/* the list page is hidden under the editor: rebuilt while nobody looks */
}

static void save_tick(lv_timer_t *t)
{
	(void)t;
	ed_save();
}

static void ed_layout(void)
{
	if (!ed_ta)
		return;
	bool on = ed_editing();
	/* the text runs under the faded toolbar; with the keyboard up it ends above it, and the
	 * textarea keeps the caret in view when its size changes */
	lv_obj_set_style_pad_bottom(ed_ta, on ? 32 : TB_H + 24, 0);
	lv_obj_set_height(ed_ta, SCR_H - TOP - (on ? kbd_height(KBD_TEXT) : 0));
	if (on) {
		lv_obj_remove_flag(ed_done, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(ed_tb, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_obj_add_flag(ed_done, LV_OBJ_FLAG_HIDDEN);
		lv_obj_remove_flag(ed_tb, LV_OBJ_FLAG_HIDDEN);
	}
}

static void ed_focus(lv_event_t *e)
{
	(void)e;
	ed_layout();
	if (!ed_editing())
		ed_save();
}

static void ed_changed(lv_event_t *e)
{
	(void)e;
	if (ed_loading)
		return;
	ed_dirty = true;
	lv_timer_reset(save_timer);
	lv_timer_resume(save_timer);
}

/* tap = put the caret where the finger was (also when only scrolling would have moved it) */
static void ed_click(lv_event_t *e)
{
	(void)e;
	lv_point_t p;
	lv_indev_t *in = lv_indev_active();
	if (in) {
		lv_indev_get_point(in, &p);
		lv_obj_t *label = lv_textarea_get_label(ed_ta);
		lv_area_t a;
		lv_obj_get_coords(label, &a);
		lv_point_t rel = { p.x - a.x1, p.y - a.y1 };
		uint32_t pos;
		if (rel.y < 0)
			pos = 0;
		else if (rel.y >= lv_area_get_height(&a))
			pos = LV_TEXTAREA_CURSOR_LAST;
		else
			pos = lv_label_get_letter_on(label, &rel, true);
		lv_textarea_set_cursor_pos(ed_ta, pos);
	}
	if (ed_editing())
		kbd_context(ed_ta);	/* already attached: kbd_attach() would keep the old context */
	else
		kbd_attach(ed_ta, KBD_TEXT);
}

static void ed_done_click(lv_event_t *e)
{
	(void)e;
	kbd_hide();
}

static void ed_load(const char *id, const char *text, time_t t)
{
	snprintf(ed_id, sizeof(ed_id), "%s", id);
	ed_dirty = ed_deleted = false;
	ed_loading = true;
	lv_textarea_set_text(ed_ta, text);
	lv_textarea_set_cursor_pos(ed_ta, 0);
	ed_loading = false;
	lv_obj_update_layout(ed_ta);
	lv_obj_scroll_to_y(ed_ta, 0, LV_ANIM_OFF);
	ed_set_date(t);
}

static void ed_trash_confirmed(int idx, void *ud)
{
	(void)ud;
	if (idx != 1 || !ed_ta)
		return;
	ed_deleted = true;
	lv_timer_pause(save_timer);
	note_delete(ed_id);
	list_build();
	nav_pop();
}

static void ed_trash_click(lv_event_t *e)
{
	(void)e;
	if (blank(lv_textarea_get_text(ed_ta))) {	/* nothing to lose */
		ed_deleted = true;
		note_delete(ed_id);
		list_build();
		nav_pop();
		return;
	}
	ui_alert("Delete Note?", "This note will be deleted.", (const char *[]){ "Cancel", "!Delete" }, 2,
		 ed_trash_confirmed, NULL);
}

/* compose inside a note: keep this one, start the next in the same page */
static void ed_compose_click(lv_event_t *e)
{
	(void)e;
	ed_save();
	char id[24];
	new_id(id, sizeof(id));
	ed_load(id, "", time(NULL));
	kbd_attach(ed_ta, KBD_TEXT);
}

static void ed_going(void *ud);

static void editor_leave(void)
{
	lv_async_call_cancel(ed_going, NULL);
	if (ed_ta && lv_obj_has_state(ed_ta, LV_STATE_FOCUSED))
		kbd_hide();
	ed_save();
	lv_timer_pause(save_timer);
	ed_page = ed_ta = ed_date = ed_done = ed_tb = NULL;
	ed_phase = ED_PUSHED;
	ed_id[0] = 0;
}

/* the editor started sliding away (back button or edge swipe): put the keyboard away and save
 * now, so the list it uncovers is already up to date */
static void ed_going(void *ud)
{
	(void)ud;
	if (!ed_ta)
		return;
	if (lv_obj_has_state(ed_ta, LV_STATE_FOCUSED))
		kbd_hide();
	ed_save();
}

static void root_child_changed(lv_event_t *e)
{
	if (!ed_page || lv_event_get_param(e) != ed_page)
		return;
	/* coords lag the style until the next layout: a new page reads x = 0 before it is off
	 * to the right, so only a landing after sliding in counts as shown */
	int x = lv_obj_get_x(ed_page);
	if (x > 0 && ed_phase == ED_PUSHED) {
		ed_phase = ED_SLIDING_IN;
	} else if (x == 0 && (ed_phase == ED_SLIDING_IN || ed_phase == ED_LEAVING)) {
		ed_phase = ED_SHOWN;	/* landed, or an edge swipe was given up */
	} else if (x > 0 && ed_phase == ED_SHOWN) {
		ed_phase = ED_LEAVING;
		lv_async_call(ed_going, NULL);	/* we are inside the slide (invalidation is off) */
	}
}

static void editor_open(int idx)
{
	if (ed_page && ed_phase != ED_LEAVING)
		return;	/* a double tap; a leaving editor is finished off by nav_push() */
	if (kbd_visible())
		kbd_hide();	/* the search keyboard */
	lv_obj_t *c = nav_push("");
	if (!c)
		return;
	ed_page = nav_page();
	ed_phase = ED_PUSHED;
	nav_on_leave(editor_leave);
	lv_obj_set_style_bg_color(ed_page, C(pal.bg), 0);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_style_pad_bottom(c, 0, 0);

	ed_ta = lv_textarea_create(c);
	lv_obj_remove_style_all(ed_ta);
	lv_obj_remove_flag(ed_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);	/* focused = keyboard attached (kbd.c) */
	lv_obj_set_width(ed_ta, SCR_W);
	lv_obj_set_style_pad_hor(ed_ta, ED_PAD, 0);
	lv_obj_set_style_pad_top(ed_ta, DATE_H, 0);
	lv_obj_set_style_text_font(ed_ta, font(F_TEXT, 34), 0);
	lv_obj_set_style_text_color(ed_ta, C(pal.label), 0);
	lv_obj_set_style_text_line_space(ed_ta, 8, 0);
	lv_obj_set_style_border_color(ed_ta, C(pal.tint), LV_PART_CURSOR | LV_STATE_FOCUSED);
	lv_obj_set_style_border_width(ed_ta, 4, LV_PART_CURSOR | LV_STATE_FOCUSED);
	lv_obj_set_style_border_side(ed_ta, LV_BORDER_SIDE_LEFT, LV_PART_CURSOR | LV_STATE_FOCUSED);
	lv_obj_set_style_anim_duration(ed_ta, 500, LV_PART_CURSOR | LV_STATE_FOCUSED);	/* blinks only with the keyboard up */
	lv_obj_set_style_width(ed_ta, 6, LV_PART_SCROLLBAR);
	lv_obj_set_style_radius(ed_ta, 3, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_opa(ed_ta, LV_OPA_COVER, LV_PART_SCROLLBAR);
	lv_obj_set_style_bg_color(ed_ta, C(pal.label3), LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_right(ed_ta, 6, LV_PART_SCROLLBAR);
	lv_obj_set_style_pad_ver(ed_ta, 10, LV_PART_SCROLLBAR);
	lv_obj_set_scrollbar_mode(ed_ta, LV_SCROLLBAR_MODE_ACTIVE);
	lv_obj_set_scroll_dir(ed_ta, LV_DIR_VER);
	lv_textarea_set_cursor_click_pos(ed_ta, false);	/* ed_click places it, so scrolling doesn't */
	lv_obj_remove_flag(ed_ta, LV_OBJ_FLAG_CLICK_FOCUSABLE);	/* focus = keyboard attached */

	/* the date sits in the textarea's top padding, so it scrolls away with the text */
	ed_date = text(ed_ta, "", font(F_TEXT, 26), pal.label2);
	lv_obj_set_width(ed_date, SCR_W - 2 * ED_PAD);
	lv_obj_set_style_text_align(ed_date, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(ed_date, 0, -DATE_H + 6);

	ed_done = nav_button("Done", ed_done_click, NULL);
	ed_tb = toolbar(ed_page, pal.bg);
	tool_button(ed_tb, G_BIN, pal.bg, LV_ALIGN_BOTTOM_LEFT, ed_trash_click);
	tool_button(ed_tb, G_COMPOSE, pal.bg, LV_ALIGN_BOTTOM_RIGHT, ed_compose_click);

	if (idx >= 0) {
		ed_load(notes[idx].id, notes[idx].text, notes[idx].mtime);
	} else {
		char id[24];
		new_id(id, sizeof(id));
		ed_load(id, "", time(NULL));
	}
	lv_obj_add_event_cb(ed_ta, ed_click, LV_EVENT_CLICKED, NULL);
	lv_obj_add_event_cb(ed_ta, ed_changed, LV_EVENT_VALUE_CHANGED, NULL);
	lv_obj_add_event_cb(ed_ta, ed_focus, EV_KBD_SHOWN, NULL);
	lv_obj_add_event_cb(ed_ta, ed_focus, EV_KBD_HIDDEN, NULL);
	ed_layout();
	if (idx < 0)
		kbd_attach(ed_ta, KBD_TEXT);	/* a new note starts typing right away */
}

/* ---------- app ---------- */

static void app_open(lv_obj_t *root)
{
	notes_load();
	query[0] = 0;
	save_timer = lv_timer_create(save_tick, SAVE_MS, NULL);
	lv_timer_pause(save_timer);
	filter_timer = lv_timer_create(filter_tick, 120, NULL);
	lv_timer_pause(filter_timer);

	list_col = nav_root(root, "Notes");
	lv_obj_t *page = nav_page();
	build_search(list_col);
	list_box = box(list_col);
	lv_obj_remove_flag(list_box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(list_box, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	lv_obj_t *tb = toolbar(page, pal.grouped_bg);
	count_lbl = text(tb, "", font(F_TEXT, 24), pal.label2);
	lv_obj_align(count_lbl, LV_ALIGN_BOTTOM_MID, 0, -(BTN - 29) / 2);
	tool_button(tb, G_COMPOSE, pal.grouped_bg, LV_ALIGN_BOTTOM_RIGHT, compose_click);

	search_chrome();
	list_build();
	lv_obj_add_event_cb(root, root_child_changed, LV_EVENT_CHILD_CHANGED, NULL);
}

static void app_close(void)
{
	lv_async_call_cancel(ed_going, NULL);
	if (save_timer)
		lv_timer_delete(save_timer);
	if (filter_timer)
		lv_timer_delete(filter_timer);
	save_timer = filter_timer = NULL;
	list_col = list_box = count_lbl = search_ta = search_clear = search_cancel = NULL;
	ed_page = ed_ta = ed_date = ed_done = ed_tb = NULL;
	ed_phase = ED_PUSHED;
	ed_dirty = false;
	del_id[0] = 0;
}

const app_t app_notes = {
	.name = "Notes",
	.glyph = G_NOTE,
	.icon = IC_YELLOW,
	.open = app_open,
	.close = app_close,
};
