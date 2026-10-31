/* Theme = a desktop theme-switcher format: colors.toml (Omarchy keys) + wallpaper.raw
 * (BGRX 750x1334, made on the PC by `bin/orc theme`). Looked up in $ORCHARD_THEME,
 * /tmp/orchard-theme, /etc/orchard; built-in iOS dark palette when none is found.
 * Also owns the wallpapers: the theme's, generated gradient ones, and the blurred
 * "material" copy that panels (dock, control center, widgets) sit on. */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "orchard.h"
#include "src/misc/cache/instance/lv_image_cache.h"

theme_t th = {
	.dark = true,
	.accent = 0x0a84ff,
	.bg = 0x000000,
	.fg = 0xe5e5ea,
	.bright_fg = 0xffffff,
	.soft_fg = 0xd1d1d6,
	.muted = 0x8e8e93,
	.lighter_bg = 0x1c1c1e,
	.dark_bg = 0x000000,
	.ansi = {
		0x1c1c1e, 0xff453a, 0x32d74b, 0xffd60a, 0x0a84ff, 0xbf5af2, 0x64d2ff, 0xd1d1d6,
		0x636366, 0xff6961, 0x66e07a, 0xffe066, 0x5ac8fa, 0xda8fff, 0x99e6ff, 0xffffff,
	},
};

static lv_image_dsc_t theme_wp, gen_wp, blur_wp;
static const lv_image_dsc_t *cur_wp;
static int cur_idx = -1;
static const char *dir;

uint32_t mix(uint32_t a, uint32_t b, int pct)
{
	uint32_t r = 0;
	for (int s = 0; s < 24; s += 8) {
		int ca = a >> s & 0xff, cb = b >> s & 0xff;
		r |= (uint32_t)(ca + (cb - ca) * pct / 100) << s;
	}
	return r;
}

static void set(const char *key, uint32_t v)
{
	static const struct { const char *k; size_t off; } keys[] = {
		{ "accent", offsetof(theme_t, accent) },
		{ "background", offsetof(theme_t, bg) }, { "bg", offsetof(theme_t, bg) },
		{ "foreground", offsetof(theme_t, fg) }, { "fg", offsetof(theme_t, fg) },
		{ "bright_fg", offsetof(theme_t, bright_fg) }, { "soft_fg", offsetof(theme_t, soft_fg) },
		{ "muted", offsetof(theme_t, muted) }, { "lighter_bg", offsetof(theme_t, lighter_bg) },
		{ "dark_bg", offsetof(theme_t, dark_bg) },
	};
	int n;
	if (sscanf(key, "color%d", &n) == 1 && n >= 0 && n < 16) {
		th.ansi[n] = v;
		return;
	}
	for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (!strcmp(key, keys[i].k))
			*(uint32_t *)((char *)&th + keys[i].off) = v;
}

static bool load_colors(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[256], key[64], val[64];
	if (!f)
		return false;
	bool have_light = false, have_bright = false, have_soft = false, have_dark = false;
	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, " %63[a-z_0-9] = \"%63[^\"]\"", key, val) != 2)
			continue;
		if (!strcmp(key, "mode")) {
			th.dark = strcmp(val, "light") != 0;
		} else if (val[0] == '#' && strlen(val) == 7) {
			set(key, strtoul(val + 1, NULL, 16));
			have_light |= !strcmp(key, "lighter_bg");
			have_bright |= !strcmp(key, "bright_fg");
			have_soft |= !strcmp(key, "soft_fg");
			have_dark |= !strcmp(key, "dark_bg");
		}
	}
	fclose(f);
	/* older themes only carry the Ghostty keys: derive the semantic roles */
	if (!have_light)
		th.lighter_bg = mix(th.bg, th.fg, 10);
	if (!have_bright)
		th.bright_fg = th.ansi[15];
	if (!have_soft)
		th.soft_fg = mix(th.fg, th.bg, 15);
	if (!have_dark)
		th.dark_bg = mix(th.bg, 0x000000, 30);
	return true;
}

static void dsc_init(lv_image_dsc_t *d, void *px)
{
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_XRGB8888;
	d->header.w = SCR_W;
	d->header.h = SCR_H;
	d->header.stride = SCR_W * 4;
	d->data_size = SCR_W * SCR_H * 4;
	d->data = px;
}

static bool load_wallpaper(const char *path)
{
	FILE *f = fopen(path, "rb");
	size_t size = SCR_W * SCR_H * 4;
	if (!f)
		return false;
	uint8_t *px = malloc(size);
	bool ok = fread(px, 1, size, f) == size;
	fclose(f);
	if (!ok) {
		free(px);
		return false;
	}
	dsc_init(&theme_wp, px);
	return true;
}

/* ---------- generated wallpapers: soft colour blobs over a dark gradient ---------- */

static const struct gen {
	const char *name;
	struct { float x, y, r; uint32_t c; } blob[4];
	uint32_t top, bottom;
} gens[] = {
	{ "Orchard", { { 120, 260, 430, 0xff3b80 }, { 680, 620, 400, 0xff9433 }, { 200, 1080, 470, 0x26b3ff }, { 720, 1300, 380, 0x8052ff } },
	  0x171a47, 0x0a0d1f },
	{ "Dusk", { { 600, 200, 480, 0xff6a3d }, { 100, 700, 420, 0xd9387a }, { 650, 1150, 450, 0x5b3cc4 }, { 80, 1300, 300, 0x1e90ff } },
	  0x2a1030, 0x0b0612 },
	{ "Lagoon", { { 150, 300, 450, 0x14d6c1 }, { 650, 700, 420, 0x2f7bff }, { 250, 1150, 430, 0x7fe36b }, { 700, 1320, 300, 0x1aa1a8 } },
	  0x07262b, 0x020c10 },
	{ "Ember", { { 375, 250, 500, 0xffb020 }, { 80, 900, 420, 0xff4d2e }, { 700, 1000, 380, 0xc21f5b }, { 375, 1400, 420, 0x3d0f45 } },
	  0x2b1405, 0x0d0603 },
};
#define NGEN ((int)(sizeof(gens) / sizeof(gens[0])))

static void generate(const struct gen *g)
{
	uint32_t *px = gen_wp.data ? (uint32_t *)gen_wp.data : malloc(SCR_W * SCR_H * 4);
	uint32_t seed = 1;
	for (int y = 0; y < SCR_H; y++) {
		float t = (float)y / SCR_H;
		uint32_t base = mix(g->top, g->bottom, t * 100);
		float br = (base >> 16 & 0xff) / 255.f, bg = (base >> 8 & 0xff) / 255.f, bb = (base & 0xff) / 255.f;
		for (int x = 0; x < SCR_W; x++) {
			float r = br, gg = bg, b = bb;
			for (int i = 0; i < 4; i++) {
				float dx = x - g->blob[i].x, dy = y - g->blob[i].y;
				float w = expf(-(dx * dx + dy * dy) / (g->blob[i].r * g->blob[i].r)) * 0.85f;
				uint32_t c = g->blob[i].c;
				/* screen blend */
				r = 1 - (1 - r) * (1 - (c >> 16 & 0xff) / 255.f * w);
				gg = 1 - (1 - gg) * (1 - (c >> 8 & 0xff) / 255.f * w);
				b = 1 - (1 - b) * (1 - (c & 0xff) / 255.f * w);
			}
			seed = seed * 1103515245 + 12345;
			float n = ((seed >> 16) & 0xff) / 255.0f - 0.5f;	/* dither against banding */
			int R = r * 255 + n, G = gg * 255 + n, B = b * 255 + n;
			R = R < 0 ? 0 : R > 255 ? 255 : R;
			G = G < 0 ? 0 : G > 255 ? 255 : G;
			B = B < 0 ? 0 : B > 255 ? 255 : B;
			px[y * SCR_W + x] = 0xff000000u | R << 16 | G << 8 | B;
		}
	}
	dsc_init(&gen_wp, px);
}

/* ---------- material: 1/4-scale box blur (3 passes ≈ gaussian), tinted, scaled back up ---------- */

#define BS 4
#define BW ((SCR_W + BS - 1) / BS)
#define BH ((SCR_H + BS - 1) / BS)

static void blur_pass(float *img, float *tmp, int r)
{
	for (int y = 0; y < BH; y++)	/* horizontal */
		for (int c = 0; c < 3; c++) {
			float acc = 0;
			for (int x = -r; x <= r; x++)
				acc += img[(y * BW + (x < 0 ? 0 : x >= BW ? BW - 1 : x)) * 3 + c];
			for (int x = 0; x < BW; x++) {
				tmp[(y * BW + x) * 3 + c] = acc / (2 * r + 1);
				int out = x - r < 0 ? 0 : x - r, in = x + r + 1 >= BW ? BW - 1 : x + r + 1;
				acc += img[(y * BW + in) * 3 + c] - img[(y * BW + out) * 3 + c];
			}
		}
	for (int x = 0; x < BW; x++)	/* vertical */
		for (int c = 0; c < 3; c++) {
			float acc = 0;
			for (int y = -r; y <= r; y++)
				acc += tmp[((y < 0 ? 0 : y >= BH ? BH - 1 : y) * BW + x) * 3 + c];
			for (int y = 0; y < BH; y++) {
				img[(y * BW + x) * 3 + c] = acc / (2 * r + 1);
				int out = y - r < 0 ? 0 : y - r, in = y + r + 1 >= BH ? BH - 1 : y + r + 1;
				acc += tmp[(in * BW + x) * 3 + c] - tmp[(out * BW + x) * 3 + c];
			}
		}
}

static void make_blur(const lv_image_dsc_t *src)
{
	float *img = calloc(BW * BH * 3, sizeof(float)), *tmp = malloc(BW * BH * 3 * sizeof(float));
	const uint32_t *s = (const uint32_t *)src->data;
	for (int y = 0; y < SCR_H; y++)
		for (int x = 0; x < SCR_W; x++) {
			uint32_t p = s[y * SCR_W + x];
			float *d = img + ((y / BS) * BW + x / BS) * 3;
			d[0] += (p >> 16 & 0xff) / (float)(BS * BS);
			d[1] += (p >> 8 & 0xff) / (float)(BS * BS);
			d[2] += (p & 0xff) / (float)(BS * BS);
		}
	for (int i = 0; i < 3; i++)
		blur_pass(img, tmp, 7);
	/* iOS "thin material": more saturation, then pulled towards the theme background */
	float tr = (th.bg >> 16 & 0xff), tg = (th.bg >> 8 & 0xff), tb = (th.bg & 0xff), k = th.dark ? 0.42f : 0.55f;
	if (!th.dark)
		tr = tg = tb = 255;
	for (int i = 0; i < BW * BH; i++) {
		float *p = img + i * 3, l = 0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2];
		for (int c = 0; c < 3; c++) {
			float v = l + (p[c] - l) * 1.5f;
			v = v < 0 ? 0 : v > 255 ? 255 : v;
			p[c] = v + ((c == 0 ? tr : c == 1 ? tg : tb) - v) * k;
		}
	}
	uint32_t *px = blur_wp.data ? (uint32_t *)blur_wp.data : malloc(SCR_W * SCR_H * 4);
	for (int y = 0; y < SCR_H; y++) {
		float fy = (y + 0.5f) / BS - 0.5f;
		int y0 = fy < 0 ? 0 : (int)fy, y1 = y0 + 1 < BH ? y0 + 1 : y0;
		float wy = fy - y0;
		wy = wy < 0 ? 0 : wy;
		for (int x = 0; x < SCR_W; x++) {
			float fx = (x + 0.5f) / BS - 0.5f;
			int x0 = fx < 0 ? 0 : (int)fx, x1 = x0 + 1 < BW ? x0 + 1 : x0;
			float wx = fx - x0;
			wx = wx < 0 ? 0 : wx;
			uint32_t o = 0xff000000u;
			for (int c = 0; c < 3; c++) {
				float a = img[(y0 * BW + x0) * 3 + c] * (1 - wx) + img[(y0 * BW + x1) * 3 + c] * wx;
				float b = img[(y1 * BW + x0) * 3 + c] * (1 - wx) + img[(y1 * BW + x1) * 3 + c] * wx;
				int v = a * (1 - wy) + b * wy + 0.5f;
				o |= (uint32_t)(v > 255 ? 255 : v) << (16 - 8 * c);
			}
			px[y * SCR_W + x] = o;
		}
	}
	dsc_init(&blur_wp, px);
	free(img);
	free(tmp);
}

/* ---------- public ---------- */

int theme_wallpaper_count(void) { return NGEN + (theme_wp.data ? 1 : 0); }

const char *theme_wallpaper_name(int i)
{
	if (theme_wp.data && i == 0)
		return "Desktop theme";
	return gens[i - (theme_wp.data ? 1 : 0)].name;
}

int theme_wallpaper_index(void) { return cur_idx; }

void theme_wallpaper_select(int i)
{
	if (i < 0 || i >= theme_wallpaper_count())
		i = 0;
	if (theme_wp.data && i == 0) {
		cur_wp = &theme_wp;
	} else {
		generate(&gens[i - (theme_wp.data ? 1 : 0)]);
		cur_wp = &gen_wp;
	}
	cur_idx = i;
	make_blur(cur_wp);
	/* LVGL caches decoded images by source pointer: the pixels changed under the same pointer */
	lv_image_cache_drop(&gen_wp);
	lv_image_cache_drop(&blur_wp);
}

void theme_load(void)
{
	const char *dirs[] = { getenv("ORCHARD_THEME"), "/tmp/orchard-theme", "/etc/orchard" };
	char p[300];
	for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		if (!dirs[i])
			continue;
		snprintf(p, sizeof(p), "%s/colors.toml", dirs[i]);
		if (load_colors(p)) {
			dir = dirs[i];
			break;
		}
	}
	if (dir) {
		snprintf(p, sizeof(p), "%s/wallpaper.raw", dir);
		load_wallpaper(p);
	}
}

const lv_image_dsc_t *theme_wallpaper(void) { return cur_wp; }
const lv_image_dsc_t *theme_wallpaper_blur(void) { return &blur_wp; }
