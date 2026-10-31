/* TTF fonts embedded with .incbin (assets.S), rasterised at runtime by LVGL tiny_ttf. */
#include "orchard.h"

#define ASSET(n) extern const unsigned char n[], n##_end[]
ASSET(ttf_inter);
ASSET(ttf_inter_medium);
ASSET(ttf_inter_semi);
ASSET(ttf_inter_bold);
ASSET(ttf_inter_display_light);
ASSET(ttf_inter_display_bold);
ASSET(ttf_mono);

static const struct { const unsigned char *start, *end; } faces[] = {
	[F_TEXT] = { ttf_inter, ttf_inter_end },
	[F_TEXT_MEDIUM] = { ttf_inter_medium, ttf_inter_medium_end },
	[F_TEXT_SEMI] = { ttf_inter_semi, ttf_inter_semi_end },
	[F_TEXT_BOLD] = { ttf_inter_bold, ttf_inter_bold_end },
	[F_DISPLAY_LIGHT] = { ttf_inter_display_light, ttf_inter_display_light_end },
	[F_DISPLAY_BOLD] = { ttf_inter_display_bold, ttf_inter_display_bold_end },
	[F_MONO] = { ttf_mono, ttf_mono_end },
};

#define MAX_FONTS 192
static struct { enum face face; int px; lv_font_t *f; } cache[MAX_FONTS];
static int ncache;

/* One instance per (face, size); Inter falls back to the Nerd Font for symbols/icons. */
lv_font_t *font(enum face face, int px)
{
	for (int i = 0; i < ncache; i++)
		if (cache[i].face == face && cache[i].px == px)
			return cache[i].f;
	lv_font_t *f = lv_tiny_ttf_create_data(faces[face].start, faces[face].end - faces[face].start, px);
	if (face != F_MONO)
		f->fallback = font(F_MONO, px);
	if (ncache < MAX_FONTS)
		cache[ncache++] = (typeof(cache[0])){ face, px, f };
	return f;
}
