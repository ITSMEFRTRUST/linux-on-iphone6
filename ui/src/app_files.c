/* Files: an iOS-style browser for the phone's own filesystem.
 * Browse → Locations (On My iPhone = data_dir(), iPhone Storage = the internal flash's disk info,
 * Music, Root, Temporary, Device = /sys/class) → folder pages (folders first, 150 rows at a time,
 * search, sort, hidden files, live refresh) → a viewer (text up to 64 KB, hex preview of binaries)
 * and Get Info. Deleting (swipe left, long press, viewer trash, Get Info) only exists inside
 * data_dir() and /tmp, always behind a confirmation.
 * Safety: files are read on a worker thread (a stuck /sys or /proc read never freezes the shell),
 * only regular files are ever opened, read-only and non-blocking; device nodes (the internal flash
 * /dev/asp0n1 included), pipes and sockets are never opened at all. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <time.h>
#include <unistd.h>
#include "orchard.h"

#define SIDE 32
#define LIST_W (SCR_W - 2 * SIDE)
#define CELL_R 36
#define RH 116		/* two-line row */
#define RH1 100		/* one-line row */
#define TX 116		/* text column inside a row (icon slot before it) */
#define BATCH 150	/* rows per batch; "Show N More" adds the next one */
#define FIRST 14	/* rows built before the page slides in, the rest follow in chunks */
#define CHUNK 24
#define MAX_ENTS 20000
#define TEXT_CAP (64 * 1024)
#define PSEUDO_CAP 4096	/* /proc + /sys files report size 0 (or a page) */
#define HEX_BYTES 256
#define SWIPE_W 176
#define MAXP 16		/* ui.c's navigation depth */

/* Nerd Font glyphs (Font Awesome range) beyond the shared G_* set */
#define G_MOBILE "\xef\x84\x8b"
#define G_LINUX "\xef\x85\xbc"
#define G_FOLDER_OPEN "\xef\x81\xbc"
#define G_F_TEXT "\xef\x83\xb6"
#define G_F_CODE "\xef\x87\x89"
#define G_F_IMAGE "\xef\x87\x85"
#define G_F_AUDIO "\xef\x87\x87"
#define G_F_VIDEO "\xef\x87\x88"
#define G_F_ARCHIVE "\xef\x87\x86"
#define G_F_PDF "\xef\x87\x81"
#define G_F_BLANK "\xef\x80\x96"
#define G_CUBE "\xef\x86\xb2"
#define G_LINK "\xef\x83\x81"
#define G_UNLINK "\xef\x84\xa7"
#define G_PLUG "\xef\x87\xa6"
#define G_PIPE "\xef\x83\xac"
#define G_EYE "\xef\x81\xae"
#define G_EYE_OFF "\xef\x81\xb0"
#define G_MORE "\xef\x85\x81"
#define G_INFO_C "\xef\x81\x9a"
#define G_SORT_AZ "\xef\x85\x9d"
#define G_SORT_SIZE "\xef\x85\xa1"

static int lum(uint32_t c) { return ((c >> 16 & 0xff) * 299 + (c >> 8 & 0xff) * 587 + (c & 0xff) * 114) / 1000; }
static uint32_t on_color(uint32_t bg) { return lum(bg) > 150 ? mix(th.bg, 0x000000, 40) : 0xffffff; }

/* one line, cut with "…" (LVGL's DOTS mode needs a fixed height, or it wraps) */
static void one_line(lv_obj_t *l, int w)
{
	lv_obj_set_size(l, w, lv_font_get_line_height(lv_obj_get_style_text_font(l, 0)));
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

/* ---------- formatting ---------- */

static void fmt_size(char *b, int n, long long v)
{
	static const char *unit[] = { "KB", "MB", "GB", "TB" };
	if (v < 1000) {
		snprintf(b, n, v == 1 ? "%lld byte" : "%lld bytes", v);
		return;
	}
	double x = v / 1000.0;
	int i = 0;
	while (x >= 999.5 && i < 3) {
		x /= 1000;
		i++;
	}
	if (x < 9.95 && (long long)(x * 10 + 0.5) % 10)
		snprintf(b, n, "%.1f %s", x, unit[i]);
	else
		snprintf(b, n, "%.0f %s", x, unit[i]);
}

static void fmt_count(char *b, int n, long long v)	/* 12,345 */
{
	char t[32];
	int len = snprintf(t, sizeof(t), "%lld", v), k = 0;
	for (int i = 0; i < len && k < n - 1; i++) {
		if (i && (len - i) % 3 == 0 && t[i - 1] != '-' && k < n - 2)
			b[k++] = ',';
		b[k++] = t[i];
	}
	b[k] = 0;
}

static void fmt_items(char *b, int n, int count)
{
	if (count >= 1000)
		snprintf(b, n, "1,000+ items");
	else
		snprintf(b, n, "%d item%s", count, count == 1 ? "" : "s");
}

static void fmt_clock(char *b, int n, const struct tm *tm)
{
	if (prefs_int("clock24", 0)) {
		strftime(b, n, "%H:%M", tm);
		return;
	}
	strftime(b, n, "%I:%M %p", tm);	/* musl has no %l */
	if (b[0] == '0')
		memmove(b, b + 1, strlen(b));
}

/* iOS list style: "Today at 9:41 AM", "Yesterday", "Oct 17", "Oct 17, 2025" (+ time when full) */
static void fmt_date(char *b, int n, time_t t, bool full)
{
	static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	struct tm tm, now, a, c;
	time_t nw = time(NULL);
	localtime_r(&t, &tm);
	localtime_r(&nw, &now);
	char hm[16];
	fmt_clock(hm, sizeof(hm), &tm);
	a = tm;
	c = now;
	a.tm_hour = c.tm_hour = 12;
	a.tm_min = c.tm_min = a.tm_sec = c.tm_sec = 0;
	a.tm_isdst = c.tm_isdst = -1;
	long days = ((long)(mktime(&c) - mktime(&a)) + 43200) / 86400;
	char day[32];
	if (days == 0) {
		snprintf(b, n, "Today at %s", hm);
		return;
	}
	if (days == 1)
		snprintf(day, sizeof(day), "Yesterday");
	else if (tm.tm_year == now.tm_year && days > 0)
		snprintf(day, sizeof(day), "%s %d", mon[tm.tm_mon], tm.tm_mday);
	else
		snprintf(day, sizeof(day), "%s %d, %d", mon[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
	snprintf(b, n, "%s%s%s", day, full ? " at " : "", full ? hm : "");
}

static void fmt_perms(char *b, int n, mode_t m)
{
	char s[10];
	const char *rwx = "rwxrwxrwx";
	for (int i = 0; i < 9; i++)
		s[i] = m & (0400 >> i) ? rwx[i] : '-';
	s[9] = 0;
	snprintf(b, n, "%s (%03o)", s, m & 0777);
}

static void join(char *out, int n, const char *dir, const char *name)
{
	snprintf(out, n, "%s%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

static const char *base_name(const char *path)
{
	const char *s = strrchr(path, '/');
	return s && s[1] ? s + 1 : path;
}

static bool read_line(const char *path, char *buf, int n)
{
	FILE *f = fopen(path, "re");
	if (!f)
		return false;
	bool ok = fgets(buf, n, f) != NULL;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = 0;
	return ok;
}

/* ---------- filesystems ---------- */

static const struct { unsigned long magic; const char *name; bool pseudo; } fstypes[] = {
	{ 0x01021994, "tmpfs", false }, { 0x858458f6, "ramfs", false }, { 0xef53, "ext4", false },
	{ 0x9123683e, "btrfs", false }, { 0x58465342, "XFS", false }, { 0xf2f52010, "F2FS", false },
	{ 0x4d44, "FAT", false }, { 0x2011bab0, "exFAT", false }, { 0x7366746e, "NTFS", false },
	{ 0x73717368, "SquashFS", false }, { 0x794c7630, "OverlayFS", false }, { 0x9660, "ISO 9660", false },
	{ 0x65735546, "FUSE", false }, { 0x6969, "NFS", false }, { 0x2fc12fc1, "ZFS", false },
	{ 0x9fa0, "proc", true }, { 0x62656572, "sysfs", true }, { 0x64626720, "debugfs", true },
	{ 0x74726163, "tracefs", true }, { 0x62656570, "configfs", true }, { 0x73636673, "securityfs", true },
	{ 0x27e0eb, "cgroup", true }, { 0x63677270, "cgroup2", true }, { 0xcafe4a11, "bpf", true },
	{ 0x6165676c, "pstore", true }, { 0xde5e81e4, "efivarfs", true }, { 0x1cd1, "devpts", true },
};

static int fs_index(const char *path)
{
	struct statfs s;
	if (statfs(path, &s) != 0)
		return -1;
	for (unsigned i = 0; i < sizeof(fstypes) / sizeof(fstypes[0]); i++)
		if ((unsigned long)s.f_type == fstypes[i].magic)
			return i;
	return -1;
}

static bool is_pseudo(const char *path)
{
	int i = fs_index(path);
	return i >= 0 && fstypes[i].pseudo;
}

static bool under(const char *canon, const char *root)
{
	size_t n = strlen(root);
	return n && !strncmp(canon, root, n) && (canon[n] == 0 || canon[n] == '/' || root[n - 1] == '/');
}

/* items in this folder may be deleted: it lives inside data_dir() or /tmp */
static bool writable_area(const char *dir)
{
	char cd[PATH_MAX], cr[PATH_MAX];
	if (!realpath(dir, cd))
		return false;
	/* never "everything": a data dir (or /tmp) that resolves to / grants nothing */
	if (realpath(data_dir(), cr) && strcmp(cr, "/") && under(cd, cr))
		return true;
	return realpath("/tmp", cr) && strcmp(cr, "/") && under(cd, cr);
}

static const char *asp_base(void)
{
#ifndef ORCHARD_PHONE
	const char *f = getenv("ORCHARD_FAKE_ASP");	/* previews: a stand-in sysfs dir */
	if (f && *f)
		return f;
#endif
	return "/sys/block/asp0n1";
}

/* the internal flash (read-only driver, loaded by hand): size from sysfs, never the device */
static bool asp_size(long long *bytes)
{
	char p[PATH_MAX], v[64];
	snprintf(p, sizeof(p), "%s/size", asp_base());
	if (!read_line(p, v, sizeof(v)))
		return false;
	*bytes = atoll(v) * 512;
	return true;
}

/* ---------- kinds ---------- */

enum kc { KC_FOLDER, KC_TEXT, KC_CODE, KC_IMAGE, KC_AUDIO, KC_VIDEO, KC_ARCHIVE, KC_PDF, KC_EXEC, KC_LIB, KC_DOC,
	  KC_BROKEN, KC_CHR, KC_BLK, KC_FIFO, KC_SOCK };
struct kind { const char *glyph, *name; uint32_t color; enum kc kc; };

static const struct { const char *ext, *name; enum kc kc; } exts[] = {
	{ "txt", "Plain Text", KC_TEXT }, { "md", "Markdown", KC_TEXT }, { "log", "Log File", KC_TEXT },
	{ "conf", "Configuration", KC_TEXT }, { "cfg", "Configuration", KC_TEXT }, { "ini", "Configuration", KC_TEXT },
	{ "toml", "TOML", KC_TEXT }, { "yaml", "YAML", KC_TEXT }, { "yml", "YAML", KC_TEXT }, { "csv", "CSV", KC_TEXT },
	{ "service", "systemd Unit", KC_TEXT }, { "rules", "Rules", KC_TEXT }, { "list", "List", KC_TEXT },
	{ "json", "JSON", KC_CODE }, { "xml", "XML", KC_CODE }, { "c", "C Source", KC_CODE }, { "h", "C Header", KC_CODE },
	{ "cpp", "C++ Source", KC_CODE }, { "py", "Python Script", KC_CODE }, { "sh", "Shell Script", KC_CODE },
	{ "js", "JavaScript", KC_CODE }, { "html", "HTML", KC_CODE }, { "css", "CSS", KC_CODE }, { "rs", "Rust Source", KC_CODE },
	{ "go", "Go Source", KC_CODE }, { "lua", "Lua Script", KC_CODE }, { "s", "Assembly", KC_CODE },
	{ "dts", "Device Tree Source", KC_CODE }, { "dtsi", "Device Tree Source", KC_CODE }, { "mk", "Makefile", KC_CODE },
	{ "png", "PNG Image", KC_IMAGE }, { "jpg", "JPEG Image", KC_IMAGE }, { "jpeg", "JPEG Image", KC_IMAGE },
	{ "gif", "GIF Image", KC_IMAGE }, { "bmp", "BMP Image", KC_IMAGE }, { "webp", "WebP Image", KC_IMAGE },
	{ "svg", "SVG Image", KC_IMAGE }, { "ppm", "PPM Image", KC_IMAGE }, { "raw", "Raw Image", KC_IMAGE },
	{ "mp3", "MP3 Audio", KC_AUDIO }, { "m4a", "AAC Audio", KC_AUDIO }, { "aac", "AAC Audio", KC_AUDIO },
	{ "flac", "FLAC Audio", KC_AUDIO }, { "wav", "WAV Audio", KC_AUDIO }, { "ogg", "Ogg Audio", KC_AUDIO },
	{ "opus", "Opus Audio", KC_AUDIO }, { "mp4", "MPEG-4 Movie", KC_VIDEO }, { "mkv", "Matroska Video", KC_VIDEO },
	{ "mov", "QuickTime Movie", KC_VIDEO }, { "webm", "WebM Video", KC_VIDEO },
	{ "zip", "ZIP Archive", KC_ARCHIVE }, { "tar", "Tar Archive", KC_ARCHIVE }, { "gz", "Gzip Archive", KC_ARCHIVE },
	{ "tgz", "Gzip Archive", KC_ARCHIVE }, { "xz", "XZ Archive", KC_ARCHIVE }, { "zst", "Zstandard Archive", KC_ARCHIVE },
	{ "bz2", "Bzip2 Archive", KC_ARCHIVE }, { "7z", "7-Zip Archive", KC_ARCHIVE }, { "cpio", "cpio Archive", KC_ARCHIVE },
	{ "lz4", "LZ4 Archive", KC_ARCHIVE }, { "pdf", "PDF Document", KC_PDF },
	{ "ko", "Kernel Module", KC_LIB }, { "so", "Shared Library", KC_LIB }, { "a", "Static Library", KC_LIB },
	{ "o", "Object File", KC_LIB }, { "dtb", "Device Tree", KC_LIB }, { "bin", "Binary", KC_LIB },
	{ "img", "Disk Image", KC_LIB }, { "fw", "Firmware", KC_LIB }, { "efi", "EFI Program", KC_LIB },
};

static int ext_index(const char *name)
{
	const char *dot = strrchr(name, '.');
	if (!dot || dot == name || !dot[1])
		return -1;
	for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++)
		if (!strcasecmp(dot + 1, exts[i].ext))
			return i;
	return -1;
}

static void kind_set(struct kind *k, enum kc kc, const char *name)
{
	static const char *glyphs[] = {
		[KC_FOLDER] = G_FOLDER, [KC_TEXT] = G_F_TEXT, [KC_CODE] = G_F_CODE, [KC_IMAGE] = G_F_IMAGE,
		[KC_AUDIO] = G_F_AUDIO, [KC_VIDEO] = G_F_VIDEO, [KC_ARCHIVE] = G_F_ARCHIVE, [KC_PDF] = G_F_PDF,
		[KC_EXEC] = G_TERMINAL, [KC_LIB] = G_CUBE, [KC_DOC] = G_F_BLANK, [KC_BROKEN] = G_UNLINK,
		[KC_CHR] = G_MICROCHIP, [KC_BLK] = G_HDD, [KC_FIFO] = G_PIPE, [KC_SOCK] = G_PLUG,
	};
	uint32_t colors[] = {
		[KC_FOLDER] = pal.blue, [KC_TEXT] = pal.label2, [KC_CODE] = pal.teal, [KC_IMAGE] = pal.purple,
		[KC_AUDIO] = pal.pink, [KC_VIDEO] = pal.orange, [KC_ARCHIVE] = pal.yellow, [KC_PDF] = pal.red,
		[KC_EXEC] = pal.green, [KC_LIB] = pal.indigo, [KC_DOC] = pal.label2, [KC_BROKEN] = pal.red,
		[KC_CHR] = pal.gray, [KC_BLK] = pal.gray, [KC_FIFO] = pal.gray, [KC_SOCK] = pal.gray,
	};
	k->kc = kc;
	k->glyph = glyphs[kc];
	k->color = colors[kc];
	k->name = name;
}

/* mode = the target's (stat), broken = a symlink pointing nowhere */
static void kind_of(const char *name, mode_t mode, bool broken, struct kind *k)
{
	int i = ext_index(name);
	if (broken)
		kind_set(k, KC_BROKEN, "Broken Link");
	else if (S_ISDIR(mode))
		kind_set(k, KC_FOLDER, "Folder");
	else if (S_ISCHR(mode))
		kind_set(k, KC_CHR, "Character Device");
	else if (S_ISBLK(mode))
		kind_set(k, KC_BLK, "Block Device");
	else if (S_ISFIFO(mode))
		kind_set(k, KC_FIFO, "Pipe");
	else if (S_ISSOCK(mode))
		kind_set(k, KC_SOCK, "Socket");
	else if (i >= 0)
		kind_set(k, exts[i].kc, exts[i].name);
	else if (mode & 0111)
		kind_set(k, KC_EXEC, "Program");
	else
		kind_set(k, KC_DOC, "Document");
}

/* ---------- pages ---------- */

struct ent {
	char *name;
	mode_t mode;	/* the target's type + permissions (the link's own when broken) */
	bool link, broken;
	long long size;
	time_t mtime;
};

enum pk { PK_ROOT, PK_DIR, PK_FILE, PK_INFO, PK_DISK };

struct job {
	int state;	/* 0 reading, 1 done, 2 abandoned (the thread frees it); __atomic builtins only */
	char path[PATH_MAX];
	size_t cap, len;
	unsigned char *buf;
	int err;
	bool more;	/* the file goes on past what was read */
};

struct pg {
	enum pk kind;
	char path[PATH_MAX], title[128];
	lv_obj_t *content;
	struct pg *owner;	/* viewer / info: the folder page listing the item (delete → rescan it) */
	/* folder */
	struct ent *ents;
	int n, *vis, nvis, built, want, err, shown_total;
	bool pseudo, del_ok, truncated;
	char filter[96];
	lv_obj_t *ta, *clear, *list, *more, *note;
	lv_timer_t *build_t, *filter_t;
	struct timespec mt;
	ino_t ino;
	/* viewer */
	struct stat st;
	struct job *job;
	lv_timer_t *poll_t;
	lv_obj_t *wait;
	int polls;
};

static struct pg *stk[MAXP + 1];
static int nstk;
static int gen;		/* bumps on open/close: callbacks from alerts check it */
static int sort_mode;	/* 0 name, 1 date, 2 size */
static bool show_hidden;
static lv_timer_t *watch_t;
static lv_obj_t *swiped;	/* row showing its Delete button */
static bool row_swiped;	/* the current touch became a horizontal swipe: no click */

static bool alive(struct pg *p)
{
	for (int i = 0; i < nstk; i++)
		if (stk[i] == p)
			return true;
	return false;
}

static struct pg *top(void) { return nstk ? stk[nstk - 1] : NULL; }

static void alert_ok(const char *title, const char *msg)
{
	ui_alert(title, msg, (const char *[]){ "OK" }, 1, NULL, NULL);
}

/* ---------- rows ---------- */

static lv_obj_t *list_section(lv_obj_t *content)	/* no clip_corner: long lists must scroll cheaply */
{
	lv_obj_t *s = box(content);
	lv_obj_set_width(s, LIST_W);
	lv_obj_set_height(s, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(s, CELL_R, 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(s, C(pal.cell), 0);
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
	return s;
}

static void row_shift(void *o, int32_t v)
{
	lv_obj_set_style_pad_left(o, -v, 0);
	lv_obj_set_style_pad_right(o, v, 0);
}

static void swipe_close(bool animate)
{
	if (!swiped)
		return;
	lv_obj_t *r = swiped;
	swiped = NULL;
	lv_anim_delete(r, row_shift);
	int v = lv_obj_get_style_pad_right(r, 0);
	if (animate)
		motion_anim(r, row_shift, v, 0, MOTION_SMALL, NULL);
	else
		row_shift(r, 0);
}

static void dir_rebuild(struct pg *p, int sync_rows);
static void ask_delete(struct pg *owner, const char *name, bool from_page);

static void swipe_delete(lv_event_t *e)
{
	lv_obj_t *row = lv_event_get_user_data(e);
	struct pg *p = top();
	if (!p || p->kind != PK_DIR || row != swiped)
		return;
	int idx = (intptr_t)lv_obj_get_user_data(row);
	if (idx >= 0 && idx < p->n)
		ask_delete(p, p->ents[idx].name, false);
}

/* the Delete pill rides in from the right with the row's content (it sits just past the
 * content's right edge; shifting the padding moves both, the row clips it) */
static void swipe_open(lv_obj_t *row)
{
	swipe_close(true);
	lv_obj_t *b = NULL;
	for (int i = lv_obj_get_child_count(row) - 1; i >= 0 && !b; i--)
		if (lv_obj_has_flag(lv_obj_get_child(row, i), LV_OBJ_FLAG_USER_1))
			b = lv_obj_get_child(row, i);
	if (!b) {
		b = box(row);
		lv_obj_add_flag(b, LV_OBJ_FLAG_USER_1);
		lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(b, SWIPE_W - 28, lv_obj_get_height(row) - 24);
		lv_obj_set_pos(b, LIST_W + 12, 12);
		lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(b, C(pal.red), 0);
		lv_obj_set_style_color_filter_dsc(b, &lv_color_filter_shade, LV_STATE_PRESSED);
		lv_obj_set_style_color_filter_opa(b, LV_OPA_30, LV_STATE_PRESSED);
		lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(b, 2, 0);
		ui_icon_line(b, G_TRASH, font(F_MONO, 32), on_color(pal.red));
		text(b, "Delete", font(F_TEXT_SEMI, 24), on_color(pal.red));
		lv_obj_add_event_cb(b, swipe_delete, LV_EVENT_CLICKED, row);
	}
	swiped = row;
	motion_anim(row, row_shift, lv_obj_get_style_pad_right(row, 0), SWIPE_W, MOTION_SMALL, NULL);
}

static void row_pressed_shape(lv_obj_t *row)
{
	lv_obj_t *sec = lv_obj_get_parent(row);
	int i = lv_obj_get_index(row), n = lv_obj_get_child_count(sec);
	lv_obj_set_style_radius(row, i == 0 || i == n - 1 ? CELL_R : 0, LV_STATE_PRESSED);
}

/* list row: plain glyph (iOS Files / Favorites style), title, optional subtitle + chevron */
static lv_obj_t *frow(lv_obj_t *sec, const char *glyph, uint32_t color, const char *title, const char *sub,
		      bool chevron)
{
	lv_obj_t *r = box(sec);
	lv_obj_set_size(r, LIST_W, sub ? RH : RH1);
	lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_remove_flag(r, LV_OBJ_FLAG_GESTURE_BUBBLE);	/* horizontal swipes belong to the row */
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_STATE_PRESSED);
	lv_obj_set_style_bg_color(r, C(pal.cell_hi), LV_STATE_PRESSED);
	if (lv_obj_get_index(r) > 0) {	/* hairline, inset to the text like iOS */
		lv_obj_t *sep = box(r);
		lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(sep, LIST_W - TX, 1);
		lv_obj_set_pos(sep, TX, 0);
		lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(sep, C(pal.separator), 0);
	}
	lv_obj_t *g = text(r, glyph, font(F_MONO, 44), color);
	lv_obj_set_width(g, 72);
	lv_obj_set_style_text_align(g, LV_TEXT_ALIGN_CENTER, 0);
	ui_glyph_fit(g);	/* the ink centred in the 72 px column (folder, doc, image icons differ in width) */
	lv_obj_align(g, LV_ALIGN_LEFT_MID, 26, 0);
	int tw = LIST_W - TX - (chevron ? 76 : 32);
	lv_obj_t *t = text(r, title, font(F_TEXT, 34), pal.label);
	one_line(t, tw);
	if (sub) {
		lv_obj_set_pos(t, TX, 17);
		lv_obj_t *s = text(r, sub, font(F_TEXT, 26), pal.label2);
		one_line(s, tw);
		lv_obj_set_pos(s, TX, 64);
	} else {
		lv_obj_align(t, LV_ALIGN_LEFT_MID, TX, 0);
	}
	if (chevron) {
		lv_obj_t *c = text(r, G_CHEVRON_R, font(F_MONO, 28), pal.label3);
		lv_obj_align(c, LV_ALIGN_RIGHT_MID, -32, 0);
	}
	return r;
}

/* section header in ui_section()'s style, for the hand-built lists */
static void header(lv_obj_t *content, const char *s)
{
	lv_obj_t *h = text(content, s, font(F_TEXT_SEMI, 28), pal.label2);
	lv_obj_set_width(h, LIST_W - 40);
	lv_obj_set_style_pad_top(h, 36, 0);
	lv_obj_set_style_pad_bottom(h, 12, 0);
}

static lv_obj_t *footnote(lv_obj_t *content, const char *s, bool center)
{
	lv_obj_t *f = text(content, s, font(F_TEXT, 26), pal.label2);
	lv_obj_set_width(f, LIST_W - 40);
	lv_label_set_long_mode(f, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_line_space(f, 6, 0);
	lv_obj_set_style_pad_top(f, center ? 28 : 12, 0);
	if (center)
		lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
	return f;
}

/* big centered icon + name + one line under it (Get Info, viewer for non-text, disks) */
static void hero(lv_obj_t *content, const char *glyph, uint32_t color, const char *title, const char *sub)
{
	lv_obj_t *g = ui_icon_line(content, glyph, font(F_MONO, 112), color);	/* ink centred in the line box */
	lv_obj_set_style_pad_top(g, lv_obj_get_style_pad_top(g, 0) + 28, 0);	/* + the gap above it */
	lv_obj_set_height(g, lv_font_get_line_height(font(F_MONO, 112)) + 28);
	lv_obj_t *t = text(content, title, font(F_TEXT_SEMI, 36), pal.label);
	lv_obj_set_width(t, LIST_W - 40);
	lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_top(t, 20, 0);
	if (sub) {
		lv_obj_t *s = text(content, sub, font(F_TEXT, 30), pal.label2);
		lv_obj_set_width(s, LIST_W - 40);
		lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_pad_top(s, 8, 0);
	}
}

/* key + value row; long values wrap right-aligned instead of being cut off */
static lv_obj_t *kv_row(lv_obj_t *sec, const char *k, const char *v)
{
	lv_obj_t *r = ui_row(sec, NULL, 0, k);
	lv_obj_t *l = ui_row_value(r, v);
	lv_point_t sz;
	lv_text_get_size(&sz, v, font(F_TEXT, 34), 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	if (sz.x > 400) {
		lv_obj_set_style_max_width(l, LV_COORD_MAX, 0);
		lv_obj_set_size(l, 420, LV_SIZE_CONTENT);
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
	}
	return r;
}

/* action row: tint (or red) text, no icon */
static lv_obj_t *action_row(lv_obj_t *sec, const char *title, bool red, lv_event_cb_t cb, void *ud)
{
	lv_obj_t *r = ui_row(sec, NULL, 0, title);
	lv_obj_t *t = lv_obj_get_child(lv_obj_get_user_data(r), 0);
	lv_obj_set_style_text_color(t, C(red ? pal.red : pal.tint), 0);
	ui_row_tap(r, cb, ud);
	return r;
}

/* ---------- navigation ---------- */

static void pg_free(struct pg *p)
{
	if (p->build_t)
		lv_timer_delete(p->build_t);
	if (p->filter_t)
		lv_timer_delete(p->filter_t);
	if (p->poll_t)
		lv_timer_delete(p->poll_t);
	if (p->job && __atomic_exchange_n(&p->job->state, 2, __ATOMIC_ACQ_REL) == 1) {	/* finished: ours to free */
		free(p->job->buf);
		free(p->job);
	}
	for (int i = 0; i < p->n; i++)
		free(p->ents[i].name);
	free(p->ents);
	free(p->vis);
	free(p);
}

static void pg_leave(void)
{
	ui_menu_close();
	swiped = NULL;
	if (nstk)
		pg_free(stk[--nstk]);
}

static struct pg *pg_push(enum pk kind, const char *title_arg)
{
	char title[sizeof(((struct pg *)0)->title)];	/* it may live in the page swapped out below */
	snprintf(title, sizeof(title), "%s", title_arg);
	if (kbd_visible())
		kbd_hide();
	swipe_close(false);
	ui_menu_close();
	/* ui.c keeps 16 pages: past that the deepest page is swapped for the new one (nav_push lands
	 * the pop at once, its leave callback frees that page) and back skips it */
	if (nstk >= MAXP || nav_depth() >= MAXP - 1)
		nav_pop();
	lv_obj_t *c = nav_push(title);	/* the bar fits a long title (back label → "Back", then "…") */
	if (!c)
		return NULL;
	struct pg *p = calloc(1, sizeof(*p));
	if (!p)
		return NULL;
	snprintf(p->title, sizeof(p->title), "%s", title);
	p->kind = kind;
	p->content = c;
	stk[nstk++] = p;
	nav_on_leave(pg_leave);
	return p;
}

static void open_dir(const char *path, const char *title);
static void open_file(struct pg *owner, const char *path, const char *name);
static void open_info(struct pg *owner, const char *path, bool can_delete);
static void open_disk(void);

static void open_terminal_at(const char *dir)
{
	char cmd[256];
	int k = snprintf(cmd, sizeof(cmd), "cd '");
	for (const char *s = dir; *s && k < (int)sizeof(cmd) - 8; s++) {
		if (*s == '\'')
			k += snprintf(cmd + k, sizeof(cmd) - k, "'\\''");
		else
			cmd[k++] = *s;
	}
	if (k >= (int)sizeof(cmd) - 8) {
		ui_alert("Path Too Long", "The Terminal can't open a path this long.", (const char *[]){ "OK" }, 1, NULL, NULL);
		return;
	}
	snprintf(cmd + k, sizeof(cmd) - k, "'");
	term_run(cmd);
}

/* ---------- deleting (only inside data_dir() and /tmp) ---------- */

static struct { char path[PATH_MAX], name[NAME_MAX + 1]; struct pg *owner; int gen; bool from_page; } del;

static int rm_one(const char *p, const struct stat *sb, int flag, struct FTW *f)
{
	(void)sb;
	(void)flag;
	(void)f;
	return remove(p) == 0 ? 0 : errno;	/* first failure stops the walk */
}

static int count_cb_n;
static int count_one(const char *p, const struct stat *sb, int flag, struct FTW *f)
{
	(void)p;
	(void)sb;
	(void)flag;
	return f->level > 0 && ++count_cb_n >= 100000;
}

static int remove_path(const char *path)
{
	struct stat st;
	if (lstat(path, &st) != 0)
		return errno;
	if (!S_ISDIR(st.st_mode))	/* files, links (never their targets), dev nodes */
		return unlink(path) == 0 ? 0 : errno;
	/* depth-first, never following links or leaving the filesystem */
	int r = nftw(path, rm_one, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
	return r == 0 ? 0 : r > 0 ? r : errno;
}

static void dir_scan(struct pg *p);
static void dir_filter(struct pg *p);

static void delete_cb(int idx, void *ud)
{
	(void)ud;
	if (idx != 1 || del.gen != gen || !alive(del.owner)) {
		swipe_close(true);
		return;
	}
	/* the item must still be inside the folder that allowed it */
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "%s", del.path);
	char *slash = strrchr(dir, '/');
	if (slash)
		*slash = slash == dir ? (slash[1] = 0, '/') : 0;
	int err = writable_area(dir) ? remove_path(del.path) : EPERM;
	struct pg *o = del.owner;
	swiped = NULL;
	int y = lv_obj_get_scroll_y(o->content);
	dir_scan(o);
	dir_filter(o);
	dir_rebuild(o, o->want);
	lv_obj_scroll_to_y(o->content, y, LV_ANIM_OFF);
	if (err) {
		char t[300];
		snprintf(t, sizeof(t), "Couldn't Delete \xe2\x80\x9c%s\xe2\x80\x9d", del.name);
		alert_ok(t, strerror(err));
	} else {
		ui_haptic_success();
	}
	if (del.from_page && top() != o)
		nav_pop();
}

static void ask_delete(struct pg *owner, const char *name, bool from_page)
{
	if (!owner || !owner->del_ok)
		return;
	join(del.path, sizeof(del.path), owner->path, name);
	snprintf(del.name, sizeof(del.name), "%s", name);
	del.owner = owner;
	del.gen = gen;
	del.from_page = from_page;
	struct stat st;
	char title[320], msg[200];
	snprintf(title, sizeof(title), "Delete \xe2\x80\x9c%s\xe2\x80\x9d?", name);
	if (lstat(del.path, &st) == 0 && S_ISDIR(st.st_mode)) {
		count_cb_n = 0;
		nftw(del.path, count_one, 16, FTW_PHYS | FTW_MOUNT);
		char items[32];
		fmt_items(items, sizeof(items), count_cb_n);
		if (count_cb_n)
			snprintf(msg, sizeof(msg), "This folder and the %s in it will be deleted. You can't undo this.", items);
		else
			snprintf(msg, sizeof(msg), "This empty folder will be deleted. You can't undo this.");
	} else {
		snprintf(msg, sizeof(msg), "This item will be deleted immediately. You can't undo this.");
	}
	ui_alert(title, msg, (const char *[]){ "Cancel", "!Delete" }, 2, delete_cb, NULL);
}

/* ---------- folder pages ---------- */

static int natcmp(const char *a, const char *b)
{
	if (*a == '.')	/* hidden files sort by their name, like ls */
		a++;
	if (*b == '.')
		b++;
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			while (*a == '0' && isdigit((unsigned char)a[1]))
				a++;
			while (*b == '0' && isdigit((unsigned char)b[1]))
				b++;
			const char *as = a, *bs = b;
			while (isdigit((unsigned char)*a))
				a++;
			while (isdigit((unsigned char)*b))
				b++;
			if (a - as != b - bs)
				return a - as < b - bs ? -1 : 1;
			int c = strncmp(as, bs, a - as);
			if (c)
				return c;
			continue;
		}
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb)
			return ca < cb ? -1 : 1;
		a++;
		b++;
	}
	return *a ? 1 : *b ? -1 : 0;
}

static bool ent_dir(const struct ent *e) { return !e->broken && S_ISDIR(e->mode); }

static int ent_cmp(const void *x, const void *y)
{
	const struct ent *a = x, *b = y;
	bool da = ent_dir(a), db = ent_dir(b);
	if (da != db)
		return da ? -1 : 1;
	if (sort_mode == 1 && a->mtime != b->mtime)
		return a->mtime > b->mtime ? -1 : 1;
	if (sort_mode == 2 && !da && a->size != b->size)
		return a->size > b->size ? -1 : 1;
	int c = natcmp(a->name, b->name);
	return c ? c : strcmp(a->name, b->name);
}

static void dir_free_ents(struct pg *p)
{
	for (int i = 0; i < p->n; i++)
		free(p->ents[i].name);
	free(p->ents);
	free(p->vis);
	p->ents = NULL;
	p->vis = NULL;
	p->n = p->nvis = 0;
}

static void dir_scan(struct pg *p)
{
	dir_free_ents(p);
	p->err = 0;
	p->truncated = false;
	struct stat ds;
	if (stat(p->path, &ds) == 0) {
		p->mt = ds.st_mtim;
		p->ino = ds.st_ino;
	}
	DIR *d = opendir(p->path);
	if (!d) {
		p->err = errno;
		return;
	}
	int fd = dirfd(d), cap = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		const char *nm = de->d_name;
		if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2])))
			continue;
		if (p->n >= MAX_ENTS) {
			p->truncated = true;
			break;
		}
		if (p->n == cap) {
			int nc = cap ? cap * 2 : 64;
			struct ent *ne = realloc(p->ents, nc * sizeof(*ne));
			if (!ne)
				break;
			p->ents = ne;
			cap = nc;
		}
		struct ent *e = &p->ents[p->n];
		memset(e, 0, sizeof(*e));
		struct stat st;
		if (fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) != 0) {
			memset(&st, 0, sizeof(st));
			st.st_mode = de->d_type == DT_DIR ? S_IFDIR : S_IFREG;
		}
		if (S_ISLNK(st.st_mode)) {
			struct stat t;
			e->link = true;
			if (fstatat(fd, nm, &t, 0) == 0)
				st = t;
			else
				e->broken = true;
		}
		if (!(e->name = strdup(nm)))
			continue;
		e->mode = st.st_mode;
		e->size = st.st_size;
		e->mtime = st.st_mtime;
		p->n++;
	}
	closedir(d);
	if (p->n > 1)
		qsort(p->ents, p->n, sizeof(*p->ents), ent_cmp);
}

static void dir_filter(struct pg *p)
{
	free(p->vis);
	p->vis = malloc((p->n ? p->n : 1) * sizeof(int));
	p->nvis = p->shown_total = 0;
	if (!p->vis)
		return;
	for (int i = 0; i < p->n; i++) {
		const char *nm = p->ents[i].name;
		if (nm[0] == '.' && !show_hidden)
			continue;
		p->shown_total++;
		if (p->filter[0] && !strcasestr(nm, p->filter))
			continue;
		p->vis[p->nvis++] = i;
	}
}

static int count_items(const char *path)
{
	DIR *d = opendir(path);
	if (!d)
		return -1;
	int n = 0;
	struct dirent *de;
	while (n < 1000 && (de = readdir(d))) {
		const char *s = de->d_name;
		if (s[0] == '.' && (!show_hidden || !s[1] || (s[1] == '.' && !s[2])))
			continue;
		n++;
	}
	closedir(d);
	return n;
}

static void ent_subtitle(struct pg *p, const struct ent *e, char *b, int n)
{
	char date[48] = "", what[64] = "";
	if (!p->pseudo)
		fmt_date(date, sizeof(date), e->mtime, false);
	if (e->broken) {
		snprintf(b, n, "Broken link");
		return;
	}
	if (S_ISDIR(e->mode)) {
		char path[PATH_MAX];
		join(path, sizeof(path), p->path, e->name);
		int c = count_items(path);
		if (c >= 0)
			fmt_items(what, sizeof(what), c);
	} else if (S_ISREG(e->mode)) {
		if (!p->pseudo)
			fmt_size(what, sizeof(what), e->size);
		else
			snprintf(what, sizeof(what), "%s", (e->mode & 0444) && (e->mode & 0222) ? "Read & write"
						 : e->mode & 0222 ? "Write only" : "Read only");
	} else {
		struct kind k;
		kind_of(e->name, e->mode, false, &k);
		snprintf(what, sizeof(what), "%s", k.name);
	}
	snprintf(b, n, "%s%s%s%s", e->link ? "Link \xc2\xb7 " : "", date, date[0] && what[0] ? " \xc2\xb7 " : "", what);
}

static void open_entry(struct pg *p, int idx)
{
	if (idx < 0 || idx >= p->n)
		return;
	struct ent *e = &p->ents[idx];
	char path[PATH_MAX];
	join(path, sizeof(path), p->path, e->name);
	if (ent_dir(e))
		open_dir(path, e->name);
	else
		open_file(p, path, e->name);
}

/* long press on a row: Get Info / Open in Terminal / Delete */
static struct { struct pg *p; int idx; } rmenu;

static void row_menu_pick(int id)
{
	struct pg *p = rmenu.p;
	if (p != top() || rmenu.idx < 0 || rmenu.idx >= p->n)
		return;
	struct ent *e = &p->ents[rmenu.idx];
	char path[PATH_MAX];
	join(path, sizeof(path), p->path, e->name);
	switch (id) {
	case 0:
		open_entry(p, rmenu.idx);
		break;
	case 1:
		open_info(p, path, p->del_ok);
		break;
	case 2:
		open_terminal_at(ent_dir(e) ? path : p->path);
		break;
	case 3:
		ask_delete(p, e->name, false);
		break;
	}
}

static void row_event(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_obj_t *row = lv_event_get_current_target(e);
	struct pg *p = lv_event_get_user_data(e);
	if ((code != LV_EVENT_PRESSED && code != LV_EVENT_GESTURE && code != LV_EVENT_SHORT_CLICKED &&
	     code != LV_EVENT_LONG_PRESSED) || p != top())
		return;
	int idx = (intptr_t)lv_obj_get_user_data(row);
	switch (code) {
	case LV_EVENT_PRESSED:
		row_swiped = false;
		row_pressed_shape(row);
		break;
	case LV_EVENT_GESTURE: {
		lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());
		if (d != LV_DIR_LEFT && d != LV_DIR_RIGHT)
			break;
		row_swiped = true;
		lv_obj_remove_state(row, LV_STATE_PRESSED);
		if (d == LV_DIR_LEFT && p->del_ok && swiped != row)
			swipe_open(row);
		else if (d == LV_DIR_RIGHT)
			swipe_close(true);
		break;
	}
	case LV_EVENT_SHORT_CLICKED:
		if (row_swiped)
			break;
		if (swiped) {	/* first tap just closes an open Delete button */
			swipe_close(true);
			break;
		}
		open_entry(p, idx);
		break;
	case LV_EVENT_LONG_PRESSED: {
		if (swiped || row_swiped || idx < 0 || idx >= p->n)
			break;
		ui_haptic_tap();
		lv_obj_remove_state(row, LV_STATE_PRESSED);
		bool dir = ent_dir(&p->ents[idx]);
		ui_menu_item_t it[] = {
			{ 0, 0, dir ? G_FOLDER_OPEN : G_F_TEXT, "Open" },
			{ 1, 0, G_INFO_C, "Get Info" },
			{ 2, 0, G_TERMINAL, "Open in Terminal" },
			{ 3, MI_RED | MI_GROUP, G_TRASH, "Delete" },
		};
		rmenu.p = p;
		rmenu.idx = idx;
		ui_row_menu(row, it, p->del_ok ? 4 : 3, row_menu_pick);
		break;
	}
	default:
		break;
	}
}

static void more_rows(lv_event_t *e);

static void dir_row(struct pg *p, int vi)
{
	int idx = p->vis[vi];
	struct ent *e = &p->ents[idx];
	struct kind k;
	char sub[200];
	kind_of(e->name, e->mode, e->broken, &k);
	ent_subtitle(p, e, sub, sizeof(sub));
	lv_obj_t *r = frow(p->list, k.glyph, k.color, e->name, sub, ent_dir(e));
	lv_obj_set_user_data(r, (void *)(intptr_t)idx);
	lv_obj_add_event_cb(r, row_event, LV_EVENT_ALL, p);
}

/* builds up to `count` more rows (and the "Show N More" row once the batch is done) */
static void dir_build(struct pg *p, int count)
{
	if (p->more) {
		lv_obj_delete(p->more);
		p->more = NULL;
	}
	int goal = p->want < p->nvis ? p->want : p->nvis;
	while (p->built < goal && count-- > 0)
		dir_row(p, p->built++);
	if (p->built >= goal && p->built < p->nvis) {
		int left = p->nvis - p->built;
		char t[64];
		snprintf(t, sizeof(t), "Show %d More", left < BATCH ? left : BATCH);
		p->more = box(p->list);
		lv_obj_set_size(p->more, LIST_W, RH1);
		lv_obj_add_flag(p->more, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_style_bg_opa(p->more, LV_OPA_COVER, LV_STATE_PRESSED);
		lv_obj_set_style_bg_color(p->more, C(pal.cell_hi), LV_STATE_PRESSED);
		lv_obj_set_style_radius(p->more, CELL_R, LV_STATE_PRESSED);
		lv_obj_t *sep = box(p->more);
		lv_obj_remove_flag(sep, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_set_size(sep, LIST_W - TX, 1);
		lv_obj_set_pos(sep, TX, 0);
		lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(sep, C(pal.separator), 0);
		lv_obj_t *l = text(p->more, t, font(F_TEXT, 34), pal.tint);
		lv_obj_align(l, LV_ALIGN_LEFT_MID, TX, 0);
		char left_s[48];
		fmt_items(left_s, sizeof(left_s), left);
		lv_obj_t *v = text(p->more, left_s, font(F_TEXT, 34), pal.label2);
		lv_obj_align(v, LV_ALIGN_RIGHT_MID, -32, 0);
		lv_obj_t *g = text(p->more, G_LIST, font(F_MONO, 36), pal.tint);
		lv_obj_set_width(g, 72);
		lv_obj_set_style_text_align(g, LV_TEXT_ALIGN_CENTER, 0);
		ui_glyph_fit(g);
		lv_obj_align(g, LV_ALIGN_LEFT_MID, 26, 0);
		lv_obj_add_event_cb(p->more, more_rows, LV_EVENT_CLICKED, p);
	}
}

static void build_tick(lv_timer_t *t)
{
	struct pg *p = lv_timer_get_user_data(t);
	lv_timer_set_period(t, 30);
	dir_build(p, CHUNK);
	int goal = p->want < p->nvis ? p->want : p->nvis;
	if (p->built >= goal) {
		lv_timer_delete(t);
		p->build_t = NULL;
	}
}

static void build_later(struct pg *p, int delay)
{
	int goal = p->want < p->nvis ? p->want : p->nvis;
	if (p->built >= goal || p->build_t)
		return;
	p->build_t = lv_timer_create(build_tick, delay, p);
}

static void more_rows(lv_event_t *e)
{
	struct pg *p = lv_event_get_user_data(e);
	if (p != top())
		return;
	if (swiped) {
		swipe_close(true);
		return;
	}
	ui_haptic_tap();
	p->want += BATCH;
	dir_build(p, CHUNK);
	build_later(p, 30);
}

/* footer ("12 items · 3.2 GB available") or the empty / error state */
static void dir_note(struct pg *p)
{
	if (p->note) {
		lv_obj_delete(p->note);
		p->note = NULL;
	}
	if (p->nvis > 0) {
		lv_obj_remove_flag(p->list, LV_OBJ_FLAG_HIDDEN);
		char s[160], items[48], avail[48] = "";
		if (p->filter[0])
			snprintf(items, sizeof(items), "%d result%s", p->nvis, p->nvis == 1 ? "" : "s");
		else
			fmt_items(items, sizeof(items), p->shown_total);
		struct statvfs v;
		if (!p->pseudo && !p->filter[0] && statvfs(p->path, &v) == 0 && v.f_blocks > 0) {
			char sz[32];
			fmt_size(sz, sizeof(sz), (long long)v.f_bavail * v.f_frsize);
			snprintf(avail, sizeof(avail), " \xc2\xb7 %s available", sz);
		}
		snprintf(s, sizeof(s), "%s%s%s", items, avail, p->truncated ? " (list cut short)" : "");
		p->note = footnote(p->content, s, true);
		return;
	}
	lv_obj_add_flag(p->list, LV_OBJ_FLAG_HIDDEN);
	const char *g = G_FOLDER_OPEN, *t, *m;
	char msg[200];
	if (p->err) {
		lv_obj_add_flag(lv_obj_get_parent(p->ta), LV_OBJ_FLAG_HIDDEN);	/* nothing to search */
		g = G_LOCK;
		t = "Can't Open Folder";
		m = strerror(p->err);
	} else if (p->filter[0]) {
		g = G_SEARCH;
		t = "No Results";
		snprintf(msg, sizeof(msg), "Nothing here matches \xe2\x80\x9c%s\xe2\x80\x9d.", p->filter);
		m = msg;
	} else if (p->n > 0) {
		t = "Only Hidden Items";
		m = "Turn on Show Hidden Files in the top-right menu to see them.";
	} else {
		t = "Empty Folder";
		m = "There's nothing in this folder.";
	}
	p->note = box(p->content);
	lv_obj_set_size(p->note, LIST_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(p->note, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(p->note, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_top(p->note, 150, 0);
	lv_obj_set_style_pad_row(p->note, 14, 0);
	ui_icon_line(p->note, g, font(F_MONO, 112), pal.label3);
	lv_obj_t *tl = text(p->note, t, font(F_TEXT_SEMI, 36), pal.label);
	lv_obj_set_style_pad_top(tl, 14, 0);
	lv_obj_t *ml = text(p->note, m, font(F_TEXT, 30), pal.label2);
	lv_obj_set_width(ml, LIST_W - 80);
	lv_label_set_long_mode(ml, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(ml, LV_TEXT_ALIGN_CENTER, 0);
}

/* rows from scratch: sync_rows now, the rest of the batch in chunks */
static void dir_rebuild(struct pg *p, int sync_rows)
{
	if (p->build_t) {
		lv_timer_delete(p->build_t);
		p->build_t = NULL;
	}
	if (swiped && lv_obj_get_parent(swiped) == p->list)
		swiped = NULL;
	lv_obj_clean(p->list);
	p->more = NULL;
	p->built = 0;
	if (p->want < BATCH)
		p->want = BATCH;
	dir_build(p, sync_rows);
	dir_note(p);
	build_later(p, 30);
}

static void filter_fire(lv_timer_t *t)
{
	struct pg *p = lv_timer_get_user_data(t);
	lv_timer_delete(t);
	p->filter_t = NULL;
	const char *s = lv_textarea_get_text(p->ta);
	while (*s == ' ')
		s++;
	if (!strcmp(s, p->filter))
		return;
	snprintf(p->filter, sizeof(p->filter), "%s", s);
	p->want = BATCH;
	dir_filter(p);
	dir_rebuild(p, 30);
	lv_obj_scroll_to_y(p->content, 0, LV_ANIM_OFF);
}

static void search_event(lv_event_t *e)
{
	struct pg *p = lv_event_get_user_data(e);
	if (!alive(p))
		return;
	switch ((uint32_t)lv_event_get_code(e)) {
	case LV_EVENT_VALUE_CHANGED:
		if (*lv_textarea_get_text(p->ta))
			lv_obj_remove_flag(p->clear, LV_OBJ_FLAG_HIDDEN);
		else
			lv_obj_add_flag(p->clear, LV_OBJ_FLAG_HIDDEN);
		if (p->filter_t)
			lv_timer_reset(p->filter_t);
		else
			p->filter_t = lv_timer_create(filter_fire, 140, p);
		break;
	case EV_KBD_SHOWN:
		kbd_no_autocap();
		swipe_close(true);
		lv_obj_set_style_pad_bottom(p->content, 140 + kbd_height(KBD_TEXT), 0);
		break;
	case EV_KBD_HIDDEN:
		lv_obj_set_style_pad_bottom(p->content, 140, 0);
		break;
	default:
		break;
	}
}

static void search_clear(lv_event_t *e)
{
	struct pg *p = lv_event_get_user_data(e);
	if (alive(p))
		lv_textarea_set_text(p->ta, "");
}

static void content_scroll(lv_event_t *e)
{
	(void)e;
	swipe_close(true);
}

/* "…" menu of a folder: sort, hidden files, info, terminal */
static void dir_menu_pick(int id)
{
	struct pg *p = top();
	if (!p || p->kind != PK_DIR)
		return;
	if (id <= 2) {
		if (sort_mode == id)
			return;
		sort_mode = id;
		prefs_set_int("files_sort", id);
	} else if (id == 3) {
		show_hidden = !show_hidden;
		prefs_set_int("files_hidden", show_hidden);
	} else if (id == 4) {
		open_info(NULL, p->path, false);
		return;
	} else if (id == 5) {
		open_terminal_at(p->path);
		return;
	}
	/* re-sort / re-filter every open folder so going back stays consistent */
	for (int i = 0; i < nstk; i++) {
		struct pg *q = stk[i];
		if (q->kind != PK_DIR)
			continue;
		if (q->n > 1)
			qsort(q->ents, q->n, sizeof(*q->ents), ent_cmp);
		dir_filter(q);
		dir_rebuild(q, q == p ? 30 : 0);
		if (q == p)
			lv_obj_scroll_to_y(q->content, 0, LV_ANIM_OFF);
	}
}

static void dir_menu(lv_event_t *e)
{
	struct pg *p = lv_event_get_user_data(e);
	if (p != top())
		return;
	swipe_close(false);
	if (kbd_visible())
		kbd_hide();
	ui_menu_item_t it[] = {
		{ -1, MI_HEAD, NULL, "Sort By" },
		{ 0, MI_CHECK | (sort_mode == 0 ? MI_ON : 0), G_SORT_AZ, "Name" },
		{ 1, MI_CHECK | (sort_mode == 1 ? MI_ON : 0), G_CLOCK, "Date" },
		{ 2, MI_CHECK | (sort_mode == 2 ? MI_ON : 0), G_SORT_SIZE, "Size" },
		{ 3, MI_GROUP, show_hidden ? G_EYE_OFF : G_EYE, show_hidden ? "Hide Hidden Files" : "Show Hidden Files" },
		{ 4, MI_GROUP, G_INFO_C, "Get Info" },
		{ 5, 0, G_TERMINAL, "Open in Terminal" },
	};
	ui_menu(it, sizeof(it) / sizeof(it[0]), SCR_W - 20, STATUS_H + NAV_H - 6, 0, dir_menu_pick);
}

static void open_dir(const char *path_arg, const char *title)
{
	char path[PATH_MAX];	/* the args may live in a page that pg_push replaces */
	snprintf(path, sizeof(path), "%s", path_arg);
	struct pg *p = pg_push(PK_DIR, title);
	if (!p)
		return;
	snprintf(p->path, sizeof(p->path), "%s", path);
	p->pseudo = is_pseudo(path);
	p->del_ok = writable_area(path);
	p->want = BATCH;
	nav_button(G_MORE, dir_menu, p);

	p->ta = ui_search(p->content, LIST_W, "Search");
	lv_obj_set_style_margin_top(lv_obj_get_parent(p->ta), 8, 0);
	lv_obj_add_event_cb(p->ta, search_event, LV_EVENT_ALL, p);
	p->clear = box(lv_obj_get_parent(p->ta));	/* iOS clear button: only while there is text */
	lv_obj_set_size(p->clear, 38, 38);
	lv_obj_add_flag(p->clear, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_HIDDEN);
	lv_obj_set_ext_click_area(p->clear, 18);
	lv_obj_set_style_radius(p->clear, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_opa(p->clear, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(p->clear, C(pal.label3), 0);
	lv_obj_t *x = ui_icon(p->clear, G_CLOSE, font(F_MONO, 22), pal.fill);
	lv_obj_center(x);
	lv_obj_add_event_cb(p->clear, search_clear, LV_EVENT_CLICKED, p);
	p->list = list_section(p->content);
	lv_obj_set_style_margin_top(p->list, 28, 0);
	lv_obj_add_event_cb(p->content, content_scroll, LV_EVENT_SCROLL_BEGIN, NULL);

	dir_scan(p);
	dir_filter(p);
	dir_build(p, FIRST);
	dir_note(p);
	build_later(p, MOTION_PAGE + 60);	/* after the slide: building never fights it */
}

/* ---------- viewer ---------- */

static void *reader(void *arg)
{
	struct job *j = arg;
	int fd = open(j->path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
	if (fd < 0) {
		j->err = errno;
	} else {
		struct stat st;
		if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {	/* regular files only, ever */
			j->err = EINVAL;
		} else if (!(j->buf = malloc(j->cap + 1))) {
			j->err = ENOMEM;
		} else {
			while (j->len < j->cap) {
				ssize_t r = read(fd, j->buf + j->len, j->cap - j->len);
				if (r < 0 && errno == EINTR)
					continue;
				if (r < 0 && !j->len)
					j->err = errno;
				if (r <= 0)
					break;
				j->len += r;
			}
			if (j->len == j->cap) {
				unsigned char c;
				j->more = read(fd, &c, 1) > 0;
			}
		}
		close(fd);
	}
	if (__atomic_exchange_n(&j->state, 1, __ATOMIC_ACQ_REL) == 2) {	/* the page went away meanwhile */
		free(j->buf);
		free(j);
	}
	return NULL;
}

static int utf8_len(const unsigned char *s, size_t n)	/* valid sequence length, 0 = invalid */
{
	int len = s[0] >= 0xc2 && s[0] <= 0xdf ? 2 : s[0] >= 0xe0 && s[0] <= 0xef ? 3 : s[0] >= 0xf0 && s[0] <= 0xf4 ? 4 : 0;
	if (!len || (size_t)len > n)
		return 0;
	for (int i = 1; i < len; i++)
		if ((s[i] & 0xc0) != 0x80)
			return 0;
	return len;
}

/* text unless more than 2 % junk; NUL-separated strings (/proc/x/cmdline, device tree) count as text */
static bool looks_text(const unsigned char *s, size_t n)
{
	while (n && !s[n - 1])
		n--;
	size_t bad = 0, nul = 0;
	for (size_t i = 0; i < n;) {
		unsigned char c = s[i];
		if (!c) {
			if (i + 1 < n && !s[i + 1])
				return false;
			nul++;
			i++;
		} else if (c < 0x20 || c == 0x7f) {
			bad += !strchr("\t\n\r\f\v\x1b", c);
			i++;
		} else if (c < 0x80) {
			i++;
		} else {
			int l = utf8_len(s + i, n - i);
			bad += !l;
			i += l ? l : 1;
		}
	}
	return bad * 50 <= n && nul * 8 <= n;
}

/* printable copy: tabs → spaces, CRLF → LF, NUL → LF, ANSI colour codes dropped, junk → U+FFFD */
static char *sanitize(const unsigned char *s, size_t n, size_t *out_n)
{
	while (n && !s[n - 1])
		n--;
	char *o = malloc(n * 4 + 1);
	if (!o)
		return NULL;
	size_t k = 0;
	int col = 0;
	for (size_t i = 0; i < n;) {
		unsigned char c = s[i];
		if (c == '\r' && i + 1 < n && s[i + 1] == '\n') {
			i++;
			continue;
		}
		if (c == '\r' || c == 0 || c == '\f' || c == '\v')
			c = '\n';
		if (c == '\n') {
			o[k++] = '\n';
			col = 0;
			i++;
		} else if (c == '\t') {
			do
				o[k++] = ' ';
			while (++col % 4);
			i++;
		} else if (c == 0x1b) {
			i++;
			if (i < n && s[i] == '[') {
				for (i++; i < n && !(s[i] >= 0x40 && s[i] <= 0x7e); i++)
					;
				i++;
			}
		} else if (c < 0x20 || c == 0x7f) {
			memcpy(o + k, "\xef\xbf\xbd", 3);
			k += 3;
			col++;
			i++;
		} else if (c < 0x80) {
			o[k++] = c;
			col++;
			i++;
		} else {
			int l = utf8_len(s + i, n - i);
			if (l) {
				memcpy(o + k, s + i, l);
				k += l;
				i += l;
			} else {
				memcpy(o + k, "\xef\xbf\xbd", 3);
				k += 3;
				i++;
			}
			col++;
		}
	}
	o[k] = 0;
	*out_n = k;
	return o;
}

static lv_obj_t *card(lv_obj_t *content)
{
	lv_obj_t *c = box(content);
	lv_obj_set_size(c, LIST_W, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(c, CELL_R, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(c, C(pal.cell), 0);
	lv_obj_set_style_pad_all(c, 28, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	return c;
}

static lv_obj_t *mono(lv_obj_t *parent, const char *s, uint32_t color)
{
	lv_obj_t *l = text(parent, s, font(F_MONO, 24), color);
	lv_obj_set_style_text_line_space(l, 4, 0);
	return l;
}

/* one label per ~2 KB of lines: only what is on screen is ever drawn */
static void text_card(lv_obj_t *content, const char *t, size_t n)
{
	lv_obj_t *c = card(content);
	lv_obj_set_style_margin_top(c, 20, 0);
	char *chunk = malloc(3200);
	if (!chunk)
		return;
	size_t i = 0;
	while (i < n) {
		size_t end = i + 2048 < n ? i + 2048 : n;
		if (end < n) {
			size_t k = end;
			while (k > i && t[k - 1] != '\n')
				k--;
			if (k > i)
				end = k;
			else
				while (end > i && ((unsigned char)t[end] & 0xc0) == 0x80)
					end--;
		}
		size_t len = end - i;
		if (len && t[i + len - 1] == '\n' && end < n)
			len--;	/* the next label starts on a new line anyway */
		memcpy(chunk, t + i, len);
		chunk[len] = 0;
		lv_obj_t *l = mono(c, chunk, pal.label);
		lv_obj_set_width(l, LV_PCT(100));
		lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
		i = end;
	}
	free(chunk);
}

/* three columns: offsets, bytes, ASCII */
static void hex_card(lv_obj_t *content, const unsigned char *s, size_t n)
{
	char off[32 * 6 + 1], hex[32 * 25 + 1], asc[32 * 9 + 1];
	size_t a = 0, b = 0, c = 0;
	if (n > HEX_BYTES)
		n = HEX_BYTES;
	for (size_t o = 0; o < n; o += 8) {
		a += snprintf(off + a, sizeof(off) - a, "%s%04zx", o ? "\n" : "", o);
		if (o)
			hex[b++] = asc[c++] = '\n';
		for (size_t k = 0; k < 8; k++) {
			if (o + k < n) {
				b += snprintf(hex + b, sizeof(hex) - b, k ? " %02x" : "%02x", s[o + k]);
				asc[c++] = s[o + k] >= 0x20 && s[o + k] < 0x7f ? s[o + k] : '.';
			}
		}
	}
	hex[b] = asc[c] = 0;
	lv_obj_t *cd = card(content);
	lv_obj_set_flex_flow(cd, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(cd, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
	lv_obj_set_style_pad_column(cd, 22, 0);
	mono(cd, off, pal.label3);
	mono(cd, hex, pal.label);
	mono(cd, asc, pal.label2);
}

static void viewer_meta(struct pg *p, const char *kind, char *b, int n)
{
	char sz[32], date[64];
	fmt_size(sz, sizeof(sz), p->st.st_size);
	fmt_date(date, sizeof(date), p->st.st_mtime, true);
	if (is_pseudo(p->path))
		snprintf(b, n, "%s", kind);
	else
		snprintf(b, n, "%s \xc2\xb7 %s \xc2\xb7 %s", kind, sz, date);
}

static void viewer_show(struct pg *p, struct job *j)
{
	if (p->wait) {
		lv_obj_delete(p->wait);
		p->wait = NULL;
	}
	const char *name = base_name(p->path);
	struct kind k;
	kind_of(name, p->st.st_mode, false, &k);
	char meta[200], foot[200];
	if (j->err) {
		hero(p->content, k.glyph, k.color, name, k.name);
		footnote(p->content, j->err == EAGAIN ? "Nothing to read right now." : strerror(j->err), true);
		return;
	}
	if (!j->len) {
		snprintf(meta, sizeof(meta), "%s \xc2\xb7 Empty", k.kc == KC_DOC ? "Document" : k.name);
		hero(p->content, k.glyph, k.color, name, meta);
		return;
	}
	if (looks_text(j->buf, j->len)) {
		size_t tn, len = j->len;
		if (j->more)	/* cut short: end on the last whole line */
			for (size_t i = len; i > len / 2; i--)
				if (j->buf[i - 1] == '\n') {
					len = i;
					break;
				}
		char *t = sanitize(j->buf, len, &tn);
		if (!t)
			return;
		const char *kn = k.kc == KC_TEXT || k.kc == KC_CODE ? k.name : "Plain Text";
		viewer_meta(p, kn, meta, sizeof(meta));
		lv_obj_t *m = footnote(p->content, meta, true);
		lv_obj_set_style_pad_top(m, 8, 0);
		text_card(p->content, t, tn);
		free(t);
		if (j->more) {
			char cap[32], all[32];
			fmt_size(cap, sizeof(cap), j->len);
			fmt_size(all, sizeof(all), p->st.st_size);
			if (p->st.st_size > (off_t)j->len)
				snprintf(foot, sizeof(foot), "Showing the first %s of %s.", cap, all);
			else
				snprintf(foot, sizeof(foot), "Showing the first %s.", cap);
			footnote(p->content, foot, true);
		}
		return;
	}
	const char *kn = k.kc == KC_DOC ? "Binary File" : k.name;
	char sz[32];
	fmt_size(sz, sizeof(sz), p->st.st_size > 0 ? p->st.st_size : (off_t)j->len);
	snprintf(meta, sizeof(meta), "%s \xc2\xb7 %s", kn, sz);
	hero(p->content, k.glyph, k.color, name, meta);
	header(p->content, "Hex preview");
	hex_card(p->content, j->buf, j->len);
	char total[32];
	fmt_count(total, sizeof(total), p->st.st_size > (off_t)j->len ? p->st.st_size : (long long)j->len);
	snprintf(foot, sizeof(foot), "First %d of %s bytes.", (int)(j->len < HEX_BYTES ? j->len : HEX_BYTES), total);
	footnote(p->content, foot, false);
}

static void poll_tick(lv_timer_t *t)
{
	struct pg *p = lv_timer_get_user_data(t);
	struct job *j = p->job;
	if (__atomic_load_n(&j->state, __ATOMIC_ACQUIRE) == 1) {
		lv_timer_delete(t);
		p->poll_t = NULL;
		p->job = NULL;
		viewer_show(p, j);
		free(j->buf);
		free(j);
		return;
	}
	if (++p->polls == 40 && p->wait)	/* ~1.6 s */
		lv_label_set_text(p->wait, "Still reading\xe2\x80\xa6 this file is slow to answer.");
}

static void viewer_info(lv_event_t *e)
{
	struct pg *p = lv_event_get_user_data(e);
	if (p == top())
		open_info(NULL, p->path, false);
}

static void viewer_trash(lv_event_t *e)
{
	struct pg *p = lv_event_get_user_data(e);
	if (p == top() && alive(p->owner))
		ask_delete(p->owner, base_name(p->path), true);
}

static void special_file(struct pg *p, const char *name)
{
	struct kind k;
	kind_of(name, p->st.st_mode, false, &k);
	hero(p->content, k.glyph, k.color, name, k.name);
	lv_obj_t *sec = ui_section(p->content, NULL, NULL);
	if (S_ISCHR(p->st.st_mode) || S_ISBLK(p->st.st_mode)) {
		char v[48];
		snprintf(v, sizeof(v), "%u, %u", major(p->st.st_rdev), minor(p->st.st_rdev));
		kv_row(sec, "Major, Minor", v);
	}
	char perm[32];
	fmt_perms(perm, sizeof(perm), p->st.st_mode);
	kv_row(sec, "Permissions", perm);
	footnote(p->content, "Files never opens device nodes, pipes or sockets \xe2\x80\x94 opening one can have side effects.",
		 false);
}

static void open_file(struct pg *owner, const char *path_arg, const char *name_arg)
{
	char path[PATH_MAX], name[NAME_MAX + 1];	/* the args may live in a page that pg_push replaces */
	snprintf(path, sizeof(path), "%s", path_arg);
	snprintf(name, sizeof(name), "%s", name_arg);
	struct stat st;
	if (stat(path, &st) != 0) {
		alert_ok("Can't Open File", strerror(errno));
		return;
	}
	struct pg *p = pg_push(PK_FILE, name);
	if (!p)
		return;
	snprintf(p->path, sizeof(p->path), "%s", path);
	p->owner = owner && owner != p && alive(owner) ? owner : NULL;	/* gone if it was replaced */
	p->st = st;
	lv_obj_t *info = nav_button(G_INFO_C, viewer_info, p);
	if (p->owner && p->owner->del_ok) {
		lv_obj_t *trash = nav_button(G_TRASH, viewer_trash, p);
		lv_obj_update_layout(info);
		lv_obj_align(trash, LV_ALIGN_RIGHT_MID, -lv_obj_get_width(info) + 24, 0);
	}
	if (!S_ISREG(st.st_mode)) {
		special_file(p, name);
		return;
	}
	struct job *j = calloc(1, sizeof(*j));
	if (!j)
		return;
	snprintf(j->path, sizeof(j->path), "%s", path);
	j->cap = st.st_size <= 0 || is_pseudo(path) ? PSEUDO_CAP : st.st_size > TEXT_CAP ? TEXT_CAP : (size_t)st.st_size;
	p->wait = footnote(p->content, "Loading\xe2\x80\xa6", true);
	lv_obj_set_style_pad_top(p->wait, 200, 0);
	pthread_attr_t at;
	pthread_t tid;
	pthread_attr_init(&at);
	pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
	pthread_attr_setstacksize(&at, 64 << 10);
	if (pthread_create(&tid, &at, reader, j) != 0)
		reader(j);	/* no thread to spare: read in place, the poll picks it up */
	pthread_attr_destroy(&at);
	p->job = j;
	p->poll_t = lv_timer_create(poll_tick, 40, p);
}

/* ---------- Get Info ---------- */

static void info_terminal(lv_event_t *e)
{
	struct pg *p = lv_event_get_user_data(e);
	if (p != top())
		return;
	struct stat st;
	if (stat(p->path, &st) == 0 && S_ISDIR(st.st_mode)) {
		open_terminal_at(p->path);
	} else {
		char dir[PATH_MAX];
		snprintf(dir, sizeof(dir), "%s", p->path);
		char *s = strrchr(dir, '/');
		if (s)
			*s = s == dir ? (s[1] = 0, '/') : 0;
		open_terminal_at(dir);
	}
}

static void info_delete(lv_event_t *e)
{
	struct pg *p = lv_event_get_user_data(e);
	if (p == top() && alive(p->owner))
		ask_delete(p->owner, base_name(p->path), true);
}

/* owner + can_delete: opened from that folder's list, so a Delete row can pop straight back to it */
static void open_info(struct pg *owner, const char *path_arg, bool can_delete)
{
	char path[PATH_MAX];	/* the args may live in a page that pg_push replaces */
	snprintf(path, sizeof(path), "%s", path_arg);
	struct stat ls, st;
	if (lstat(path, &ls) != 0) {
		alert_ok("Can't Get Info", strerror(errno));
		return;
	}
	bool broken = false;
	if (stat(path, &st) != 0) {
		st = ls;
		broken = S_ISLNK(ls.st_mode);
	}
	const char *name = base_name(path);
	struct pg *p = pg_push(PK_INFO, "Info");
	if (!p)
		return;
	snprintf(p->path, sizeof(p->path), "%s", path);
	p->owner = owner && owner != p && alive(owner) ? owner : NULL;	/* gone if it was replaced */
	struct kind k;
	kind_of(name, st.st_mode, broken, &k);
	bool pseudo = is_pseudo(path), dir = S_ISDIR(st.st_mode) && !broken;
	char v[PATH_MAX + 64], sz[32];
	int items = dir ? count_items(path) : -1;
	if (dir && items >= 0)
		fmt_items(sz, sizeof(sz), items);
	else if (S_ISREG(st.st_mode) && !pseudo)
		fmt_size(sz, sizeof(sz), st.st_size);
	else
		sz[0] = 0;
	snprintf(v, sizeof(v), "%s%s%s", k.name, sz[0] ? " \xc2\xb7 " : "", sz);
	hero(p->content, !strcmp(path, "/") ? G_LINUX : k.glyph, k.color, !strcmp(path, "/") ? "/" : name, v);

	lv_obj_t *sec = ui_section(p->content, "Information", NULL);
	if (S_ISLNK(ls.st_mode) && !broken) {
		snprintf(v, sizeof(v), "%s (Link)", k.name);
		kv_row(sec, "Kind", v);
	} else {
		kv_row(sec, "Kind", k.name);
	}
	if (S_ISREG(st.st_mode) && !pseudo) {
		char by[32];
		fmt_size(sz, sizeof(sz), st.st_size);
		fmt_count(by, sizeof(by), st.st_size);
		if (st.st_size >= 1000)
			snprintf(v, sizeof(v), "%s (%s bytes)", sz, by);
		else
			snprintf(v, sizeof(v), "%s", sz);
		kv_row(sec, "Size", v);
	}
	if (dir && items >= 0) {
		fmt_items(v, sizeof(v), items);
		kv_row(sec, "Contents", v);
	}
	if (S_ISLNK(ls.st_mode)) {
		char t[PATH_MAX];
		ssize_t n = readlink(path, t, sizeof(t) - 1);
		if (n >= 0) {
			t[n] = 0;
			kv_row(sec, "Points To", t);
		}
	}
	if (S_ISCHR(st.st_mode) || S_ISBLK(st.st_mode)) {
		snprintf(v, sizeof(v), "%u, %u", major(st.st_rdev), minor(st.st_rdev));
		kv_row(sec, "Major, Minor", v);
	}
	if (!pseudo) {
		fmt_date(v, sizeof(v), st.st_mtime, true);
		kv_row(sec, "Modified", v);
	}
	fmt_perms(v, sizeof(v), st.st_mode);
	kv_row(sec, "Permissions", v);
	struct passwd *pw = getpwuid(st.st_uid);
	if (pw)
		snprintf(v, sizeof(v), "%s", pw->pw_name);
	else
		snprintf(v, sizeof(v), "%u", (unsigned)st.st_uid);
	kv_row(sec, "Owner", v);
	int fi = fs_index(path);
	if (fi >= 0)
		kv_row(sec, "Volume", fstypes[fi].name);
	snprintf(v, sizeof(v), "%s", path);
	char *s = strrchr(v, '/');
	if (s)
		*s = s == v ? (s[1] = 0, '/') : 0;
	if (strcmp(path, "/"))
		kv_row(sec, "Where", v);

	lv_obj_t *act = ui_section(p->content, NULL, NULL);
	action_row(act, "Open in Terminal", false, info_terminal, p);
	if (can_delete && p->owner && p->owner->del_ok && top() == p && nstk >= 2 && stk[nstk - 2] == p->owner)
		action_row(ui_section(p->content, NULL, NULL), dir ? "Delete Folder" : "Delete", true, info_delete, p);
}

/* ---------- iPhone Storage (information only: sysfs, never the block device) ---------- */

static int part_cmp(const void *a, const void *b)
{
	return natcmp(*(char *const *)a, *(char *const *)b);
}

static void open_disk(void)
{
	long long bytes;
	if (!asp_size(&bytes)) {
		ui_alert("iPhone Storage Unavailable",
			 "The internal flash driver isn't loaded. Load its module to see the iPhone's storage here.",
			 (const char *[]){ "OK" }, 1, NULL, NULL);
		return;
	}
	struct pg *p = pg_push(PK_DISK, "iPhone Storage");
	if (!p)
		return;
	const char *base = asp_base(), *dev = base_name(base);
	char cap[32], v[PATH_MAX], path[PATH_MAX];
	fmt_size(cap, sizeof(cap), bytes);
	snprintf(v, sizeof(v), "%s \xc2\xb7 read-only \xc2\xb7 iOS (APFS)", cap);
	hero(p->content, G_HDD, pal.gray, "iPhone Storage", v);

	lv_obj_t *sec = ui_section(p->content, "Disk", NULL);
	char by[32];
	fmt_count(by, sizeof(by), bytes);
	kv_row(sec, "Capacity", cap);
	kv_row(sec, "Bytes", by);
	snprintf(v, sizeof(v), "/dev/%s", dev);
	kv_row(sec, "Device", v);
	kv_row(sec, "Access", "Read-only");
	kv_row(sec, "Format", "iOS (APFS)");
	snprintf(path, sizeof(path), "%s/queue/logical_block_size", base);
	if (read_line(path, v, sizeof(v)) && atoll(v) > 0) {
		fmt_count(by, sizeof(by), atoll(v));
		snprintf(v, sizeof(v), "%s bytes", by);
		kv_row(sec, "Block Size", v);
	}
	snprintf(path, sizeof(path), "%s/device/model", base);
	if (read_line(path, v, sizeof(v)) && v[0])
		kv_row(sec, "Model", v);

	char *parts[16];
	int np = 0;
	size_t dl = strlen(dev);
	DIR *d = opendir(base);
	struct dirent *de;
	while (d && np < 16 && (de = readdir(d)))
		if (!strncmp(de->d_name, dev, dl) && de->d_name[dl] == 'p' && isdigit((unsigned char)de->d_name[dl + 1]))
			if ((parts[np] = strdup(de->d_name)))
				np++;
	if (d)
		closedir(d);
	if (np > 1)
		qsort(parts, np, sizeof(parts[0]), part_cmp);
	if (np) {
		lv_obj_t *ps = ui_section(p->content, "Partitions", NULL);
		for (int i = 0; i < np; i++) {
			int num = atoi(parts[i] + dl + 1);
			snprintf(path, sizeof(path), "%s/%s/size", base, parts[i]);
			char sz[32] = "";
			if (read_line(path, v, sizeof(v)))
				fmt_size(sz, sizeof(sz), atoll(v) * 512);
			lv_obj_t *r = ui_row(ps, NULL, 0, parts[i]);
			ui_row_subtitle(r, num == 1 ? "iOS APFS container" : "Partition");
			ui_row_value(r, sz);
			free(parts[i]);
		}
	}
	footnote(p->content,
		 "Linux sees the iPhone's internal flash through a read-only driver. iOS keeps its files in an "
		 "APFS container that can't be opened here yet, and Files never writes to this disk.",
		 false);
}

/* ---------- Browse (root) ---------- */

enum { L_HOME, L_DISK, L_MUSIC, L_ROOT, L_TMP, L_DEV, L_N };
static const struct { const char *title, *path; } loc_def[L_N] = {
	[L_HOME] = { "On My iPhone", NULL }, [L_DISK] = { "iPhone Storage", NULL }, [L_MUSIC] = { "Music", "/tmp/music" },
	[L_ROOT] = { "Root", "/" }, [L_TMP] = { "Temporary", "/tmp" }, [L_DEV] = { "Device", "/sys/class" },
};
static lv_obj_t *root_box;
static bool root_music, root_disk;	/* what the Locations list was built with */

static const char *loc_path(int i)
{
#ifndef ORCHARD_PHONE
	const char *f = getenv("ORCHARD_FAKE_MUSIC");	/* previews: stand-in for /tmp/music */
	if (i == L_MUSIC && f && *f)
		return f;
#endif
	return i == L_HOME ? data_dir() : loc_def[i].path;
}

static bool is_dir(const char *p)
{
	struct stat st;
	return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void loc_click(lv_event_t *e)
{
	int i = (intptr_t)lv_event_get_user_data(e);
	if (nstk != 1)
		return;
	if (i == L_DISK)
		open_disk();
	else
		open_dir(loc_path(i), loc_def[i].title);
}

static void root_build(void)
{
	lv_obj_clean(root_box);
	static const char *glyph[L_N] = { G_MOBILE, G_HDD, G_MUSIC, G_LINUX, G_CLOCK, G_MICROCHIP };
	long long bytes = 0;
	root_disk = asp_size(&bytes);
	root_music = is_dir(loc_path(L_MUSIC));
	lv_obj_t *sec = ui_section(root_box, "Locations",
				   "The phone keeps its files in RAM: they last until it restarts. "
				   "iPhone Storage is the internal flash, shown read-only.");
	for (int i = 0; i < L_N; i++) {
		char sub[PATH_MAX];
		if (i == L_MUSIC && !root_music)
			continue;
		if (i == L_DISK) {
			if (root_disk) {
				char cap[32];
				fmt_size(cap, sizeof(cap), bytes);
				snprintf(sub, sizeof(sub), "%s \xc2\xb7 read-only \xc2\xb7 iOS (APFS)", cap);
			} else {
				snprintf(sub, sizeof(sub), "Flash driver not loaded");
			}
		} else {
			snprintf(sub, sizeof(sub), "%s", loc_path(i));
		}
		uint32_t color = i == L_DISK && !root_disk ? pal.label3 : pal.blue;
		lv_obj_t *r = frow(sec, glyph[i], color, loc_def[i].title, sub, i != L_DISK || root_disk);
		lv_obj_add_event_cb(r, loc_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);
	}
}

/* every 2 s: a folder on screen that changed is listed again (scroll kept); Music / iPhone
 * Storage come and go on Browse */
static void watch_tick(lv_timer_t *t)
{
	(void)t;
	struct pg *p = top();
	if (!p || ui_menu_shown() || swiped || kbd_visible())
		return;
	if (p->kind == PK_ROOT) {
		long long b;
		if (root_box && (asp_size(&b) != root_disk || is_dir(loc_path(L_MUSIC)) != root_music))
			root_build();
		return;
	}
	if (p->kind != PK_DIR || p->pseudo || p->build_t || p->filter_t)
		return;
	struct stat st;
	if (stat(p->path, &st) != 0 ||
	    (st.st_ino == p->ino && st.st_mtim.tv_sec == p->mt.tv_sec && st.st_mtim.tv_nsec == p->mt.tv_nsec))
		return;
	int y = lv_obj_get_scroll_y(p->content);
	dir_scan(p);
	dir_filter(p);
	dir_rebuild(p, p->want);
	lv_obj_scroll_to_y(p->content, y, LV_ANIM_OFF);
}

static void app_open(lv_obj_t *root)
{
	gen++;
	sort_mode = prefs_int("files_sort", 0);
	if (sort_mode < 0 || sort_mode > 2)
		sort_mode = 0;
	show_hidden = prefs_int("files_hidden", 0);
	lv_obj_t *c = nav_root(root, "Browse");
	struct pg *p = calloc(1, sizeof(*p));
	if (p) {
		p->kind = PK_ROOT;
		p->content = c;
		snprintf(p->title, sizeof(p->title), "Browse");
		stk[nstk++] = p;
		nav_on_leave(pg_leave);
	}
	root_box = box(c);
	lv_obj_set_size(root_box, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(root_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(root_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	root_build();
	watch_t = lv_timer_create(watch_tick, 2000, NULL);
}

static void app_close(void)
{
	gen++;
	ui_menu_close();
	while (nstk)	/* nav_reset already ran the leave callbacks; this is a safety net */
		pg_free(stk[--nstk]);
	if (watch_t)
		lv_timer_delete(watch_t);
	watch_t = NULL;
	root_box = NULL;
	swiped = NULL;
}

const app_t app_files = {
	.name = "Files",
	.glyph = G_FOLDER,
	.icon = IC_BLUE,
	.open = app_open,
	.close = app_close,
};
