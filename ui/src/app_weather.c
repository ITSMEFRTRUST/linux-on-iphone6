/* Weather, iOS-style, from Open-Meteo (api.open-meteo.com, no key): a list of saved cities (search adds
 * one through Open-Meteo's geocoding), and a page per city with the current conditions, the next 24 hours,
 * 7 days and detail tiles. Fetched in °C / km/h and shown in °F (default) or °C — the
 * °F/°C button switches at once. Cities live in prefs (weather_cities), the last answer for each in
 * data_dir()/weather/ so the list shows something offline; data older than 15 min is refreshed. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "json.h"
#include "net.h"
#include "orchard.h"

#define SIDE 32
#define CARD_W (SCR_W - 2 * SIDE)
#define MAX_CITIES 12
#define MAX_RESULTS 10
#define NH 25			/* hours shown: now + 24 */
#define ND 7
#define STALE_S (15 * 60)
#define FC_BYTES (256 * 1024)

#define W_SUNNY "\xf3\xb0\x96\x99"
#define W_NIGHT "\xf3\xb0\x96\x94"
#define W_PCLOUD "\xf3\xb0\x96\x95"
#define W_NPCLOUD "\xf3\xb0\xbc\xb1"
#define W_CLOUDY "\xf3\xb0\x96\x90"
#define W_FOG "\xf3\xb0\x96\x91"
#define W_HAIL "\xf3\xb0\x96\x92"
#define W_LIGHTNING "\xf3\xb0\x96\x93"
#define W_POUR "\xf3\xb0\x96\x96"
#define W_RAIN "\xf3\xb0\x96\x97"
#define W_SNOW "\xf3\xb0\x96\x98"
#define W_WINDY "\xf3\xb0\x96\x9d"
#define W_LRAIN "\xf3\xb0\x99\xbe"
#define W_SRAIN "\xf3\xb0\x99\xbf"
#define W_SUNRISE "\xf3\xb0\x96\x9c"
#define W_SUNSET "\xf3\xb0\x96\x9b"
#define W_HUMID "\xf3\xb0\x96\x8e"
#define W_GAUGE "\xf3\xb0\x8a\x9a"
#define W_UV "\xf3\xb0\x96\xa8"
#define W_THERMO "\xf3\xb0\x94\x8f"
#define W_UMBRELLA "\xf3\xb0\x95\x8a"
#define W_SNOWHEAVY "\xf3\xb0\xbc\xb6"
#define G_CLEAR "\xf3\xb0\x85\x99"
#define DEG "\xc2\xb0"

struct wx {
	bool ok;
	long fetched;		/* time() of the answer */
	int utc_off;		/* city's offset, s */
	double t, feels, hum, wind, wdir, press, uv, precip;
	int code, day;
	int nh;
	double ht[NH];
	int hcode[NH], hpop[NH], hday[NH], hhour[NH];
	int nd;
	double dmax[ND], dmin[ND], duv[ND];
	int dcode[ND], dpop[ND], dwday[ND], rise[ND], set[ND];	/* rise/set: minutes after local midnight */
};

struct city {
	char name[64], region[128];
	double lat, lon;
	struct wx wx;
	net_req_t *req;
	bool failed;
};

static struct city cities[MAX_CITIES];
static int ncity;
static bool loaded;

struct place { char name[64], region[128]; double lat, lon; };
static struct place results[MAX_RESULTS];
static int nresults;
static enum { S_IDLE, S_SEARCHING, S_DONE, S_FAILED } sstate;
static net_req_t *sreq;

/* UI */
static lv_obj_t *list_col, *list_box, *unit_btn, *search_ta, *search_clear, *search_cancel;
static lv_timer_t *search_timer, *refresh_timer;
static int detail = -1;		/* city shown on the pushed page */
static lv_obj_t *detail_col;
static char query[128];

/* ---------- units + formatting ---------- */

static bool fahr(void) { return prefs_int("weather_f", 1); }

static int temp(double c) { return (int)lround(fahr() ? c * 9 / 5 + 32 : c); }

static void fmt_temp(char *b, int n, double c) { snprintf(b, n, "%d" DEG, temp(c)); }

static void fmt_hour(char *b, int n, int h, int m, bool minutes)
{
	if (prefs_int("clock24", 0))
		snprintf(b, n, "%02d:%02d", h, m);
	else if (minutes)
		snprintf(b, n, "%d:%02d %s", (h + 11) % 12 + 1, m, h < 12 ? "AM" : "PM");
	else
		snprintf(b, n, "%d %s", (h + 11) % 12 + 1, h < 12 ? "AM" : "PM");
}

/* WMO weather interpretation codes → glyph, colour, words */
static const char *wx_glyph(int code, bool day)
{
	switch (code) {
	case 0: return day ? W_SUNNY : W_NIGHT;
	case 1: case 2: return day ? W_PCLOUD : W_NPCLOUD;
	case 3: return W_CLOUDY;
	case 45: case 48: return W_FOG;
	case 51: case 53: case 55: case 61: case 63: case 80: case 81: return W_RAIN;
	case 65: case 82: return W_POUR;
	case 56: case 57: case 66: case 67: return W_SRAIN;
	case 71: case 73: case 77: case 85: return W_SNOW;
	case 75: case 86: return W_SNOWHEAVY;
	case 95: return W_LIGHTNING;
	case 96: case 99: return W_HAIL;
	default: return W_CLOUDY;
	}
}

static uint32_t wx_color(int code, bool day)
{
	if (code <= 1)
		return day ? pal.yellow : pal.label2;
	if (code == 2)
		return day ? pal.yellow : pal.label2;
	if (code >= 95)
		return pal.yellow;
	if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82))
		return pal.teal;
	if ((code >= 71 && code <= 77) || code == 85 || code == 86)
		return pal.label;
	return pal.label2;
}

static const char *wx_words(int code)
{
	switch (code) {
	case 0: return "Clear";
	case 1: return "Mostly Clear";
	case 2: return "Partly Cloudy";
	case 3: return "Cloudy";
	case 45: return "Fog";
	case 48: return "Freezing Fog";
	case 51: return "Light Drizzle";
	case 53: return "Drizzle";
	case 55: return "Heavy Drizzle";
	case 56: case 57: return "Freezing Drizzle";
	case 61: return "Light Rain";
	case 63: return "Rain";
	case 65: return "Heavy Rain";
	case 66: case 67: return "Freezing Rain";
	case 71: return "Light Snow";
	case 73: return "Snow";
	case 75: return "Heavy Snow";
	case 77: return "Snow Grains";
	case 80: return "Showers";
	case 81: return "Rain Showers";
	case 82: return "Heavy Showers";
	case 85: case 86: return "Snow Showers";
	case 95: return "Thunderstorms";
	case 96: case 99: return "Thunderstorms and Hail";
	default: return "\xe2\x80\x94";
	}
}

static const char *compass(double deg)
{
	static const char *const d[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
	if (!isfinite(deg))
		return "\xe2\x80\x94";
	double a = fmod(deg, 360);	/* any sign / size (a feed's -720 or 1e300) → [0, 360) */
	if (a < 0)
		a += 360;
	return d[(int)lround(a / 45) % 8];
}

static const char *uv_level(double uv)
{
	return uv < 3 ? "Low" : uv < 6 ? "Moderate" : uv < 8 ? "High" : uv < 11 ? "Very High" : "Extreme";
}

/* ---------- storage ---------- */

static void clean(char *s)
{
	for (; *s; s++)
		if (*s == '|' || *s == ';' || *s == '\n' || *s == '=')
			*s = ' ';
}

static void cities_save(void)
{
	char buf[MAX_CITIES * 220] = "";
	int k = 0;
	for (int i = 0; i < ncity && k < (int)sizeof(buf) - 220; i++)
		k += snprintf(buf + k, sizeof(buf) - k, "%s%s|%s|%.4f|%.4f", i ? ";" : "", cities[i].name,
			      cities[i].region, cities[i].lat, cities[i].lon);
	prefs_set_str("weather_cities", buf);
}

static void cache_path(char *b, int n, const struct city *c)
{
	char d[300];
	snprintf(d, sizeof(d), "%s/weather", data_dir());
	mkdir(d, 0755);
	snprintf(b, n, "%s/%.3f_%.3f.json", d, c->lat, c->lon);
}

static bool wx_parse(const char *body, struct wx *w);

static void cache_load(struct city *c)
{
	char p[400];
	cache_path(p, sizeof(p), c);
	FILE *f = fopen(p, "rb");
	if (!f)
		return;
	char *b = malloc(FC_BYTES + 1);
	size_t n = b ? fread(b, 1, FC_BYTES, f) : 0;
	fclose(f);
	if (!b)
		return;
	b[n] = 0;
	struct stat st;
	if (wx_parse(b, &c->wx) && stat(p, &st) == 0)
		c->wx.fetched = st.st_mtime;
	free(b);
}

static void cache_save(struct city *c, const char *body, size_t len)
{
	char p[400], t[410];
	cache_path(p, sizeof(p), c);
	snprintf(t, sizeof(t), "%s.new", p);
	FILE *f = fopen(t, "wb");
	if (!f)
		return;
	bool ok = fwrite(body, 1, len, f) == len;
	if (fclose(f) == 0 && ok)
		rename(t, p);
	else
		unlink(t);
}

static void cities_load(void)
{
	if (loaded)
		return;
	loaded = true;
	char buf[MAX_CITIES * 220];
	snprintf(buf, sizeof(buf), "%s", prefs_str("weather_cities", ""));
	char *save = NULL;
	for (char *e = strtok_r(buf, ";", &save); e && ncity < MAX_CITIES; e = strtok_r(NULL, ";", &save)) {
		char *f[4] = { 0 };
		int k = 0;
		for (char *p = e; k < 4; k++) {
			f[k] = p;
			p = strchr(p, '|');
			if (!p)
				break;
			*p++ = 0;
		}
		if (!f[3])
			continue;
		struct city *c = &cities[ncity++];
		memset(c, 0, sizeof(*c));
		snprintf(c->name, sizeof(c->name), "%s", f[0]);
		snprintf(c->region, sizeof(c->region), "%s", f[1]);
		c->lat = atof(f[2]);
		c->lon = atof(f[3]);
		cache_load(c);
	}
}

/* ---------- forecast ---------- */

static int hhmm_min(const char *iso)	/* "2026-10-18T06:59" → 419 */
{
	const char *t = iso ? strchr(iso, 'T') : NULL;
	return t ? atoi(t + 1) * 60 + atoi(t + 4) : -1;
}

static int wday_of(const char *date)	/* "2026-10-18" */
{
	struct tm tm = { 0 };
	if (!date || sscanf(date, "%d-%d-%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday) != 3)
		return -1;
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	tm.tm_hour = 12;
	time_t t = timegm(&tm);
	struct tm o;
	gmtime_r(&t, &o);
	return o.tm_wday;
}

static bool wx_parse(const char *body, struct wx *w)
{
	json_t *j = json_parse(body);
	json_t *cur = json_get(j, "current"), *h = json_get(j, "hourly"), *d = json_get(j, "daily");
	if (!cur || !h || !d) {
		json_free(j);
		return false;
	}
	struct wx x = { .ok = true, .fetched = time(NULL) };
	x.utc_off = (int)json_num(json_get(j, "utc_offset_seconds"), 0);
	x.t = json_num(json_get(cur, "temperature_2m"), 0);
	x.feels = json_num(json_get(cur, "apparent_temperature"), x.t);
	x.hum = json_num(json_get(cur, "relative_humidity_2m"), 0);
	x.wind = json_num(json_get(cur, "wind_speed_10m"), 0);
	x.wdir = json_num(json_get(cur, "wind_direction_10m"), 0);
	x.press = json_num(json_get(cur, "pressure_msl"), 0);
	x.uv = json_num(json_get(cur, "uv_index"), -1);
	x.precip = json_num(json_get(cur, "precipitation"), 0);
	x.code = (int)json_num(json_get(cur, "weather_code"), 3);
	x.day = (int)json_num(json_get(cur, "is_day"), 1);
	json_t *ht = json_get(h, "time"), *htt = json_get(h, "temperature_2m"), *hc = json_get(h, "weather_code"),
	       *hp = json_get(h, "precipitation_probability"), *hd = json_get(h, "is_day");
	for (int i = 0; i < json_len(ht) && x.nh < NH; i++, x.nh++) {
		int m = hhmm_min(json_str(json_at(ht, i), NULL));
		x.hhour[x.nh] = m < 0 ? 0 : m / 60;
		x.ht[x.nh] = json_num(json_at(htt, i), 0);
		x.hcode[x.nh] = (int)json_num(json_at(hc, i), 3);
		x.hpop[x.nh] = (int)json_num(json_at(hp, i), 0);
		x.hday[x.nh] = (int)json_num(json_at(hd, i), 1);
	}
	json_t *dt = json_get(d, "time");
	for (int i = 0; i < json_len(dt) && x.nd < ND; i++, x.nd++) {
		x.dwday[x.nd] = wday_of(json_str(json_at(dt, i), NULL));
		x.dmax[x.nd] = json_num(json_at(json_get(d, "temperature_2m_max"), i), 0);
		x.dmin[x.nd] = json_num(json_at(json_get(d, "temperature_2m_min"), i), 0);
		x.dcode[x.nd] = (int)json_num(json_at(json_get(d, "weather_code"), i), 3);
		x.dpop[x.nd] = (int)json_num(json_at(json_get(d, "precipitation_probability_max"), i), 0);
		x.duv[x.nd] = json_num(json_at(json_get(d, "uv_index_max"), i), 0);
		x.rise[x.nd] = hhmm_min(json_str(json_at(json_get(d, "sunrise"), i), NULL));
		x.set[x.nd] = hhmm_min(json_str(json_at(json_get(d, "sunset"), i), NULL));
	}
	json_free(j);
	if (!x.nh || !x.nd)
		return false;
	*w = x;
	return true;
}

static void ui_refresh(void);

static void fc_done(net_res_t *r, void *ud)
{
	struct city *c = ud;
	c->req = NULL;
	struct wx w;
	if (!r->err && r->status == 200 && wx_parse(r->body, &w)) {
		c->wx = w;
		c->failed = false;
		cache_save(c, r->body, r->len);
	} else {
		c->failed = true;
	}
	ui_refresh();
}

static void fc_fetch(struct city *c, bool force)
{
	if (c->req || (!force && c->wx.ok && time(NULL) - c->wx.fetched < STALE_S))
		return;
	char url[700];
	snprintf(url, sizeof(url),
		 "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
		 "&current=temperature_2m,apparent_temperature,relative_humidity_2m,is_day,precipitation,weather_code,"
		 "wind_speed_10m,wind_direction_10m,pressure_msl,uv_index"
		 "&hourly=temperature_2m,weather_code,precipitation_probability,is_day"
		 "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset,"
		 "uv_index_max&timezone=auto&forecast_days=7&forecast_hours=25",
		 c->lat, c->lon);
	c->req = net_get(url, FC_BYTES, 20, fc_done, c);
	if (!c->req)
		c->failed = true;
}

/* ---------- geocoding search ---------- */

static void list_build(void);

static void geo_done(net_res_t *r, void *ud)
{
	(void)ud;
	sreq = NULL;
	nresults = 0;
	json_t *j = r->err || r->status != 200 ? NULL : json_parse(r->body);
	if (!j && r->err) {
		sstate = S_FAILED;
		list_build();
		return;
	}
	json_t *a = json_get(j, "results");
	for (int i = 0; i < json_len(a) && nresults < MAX_RESULTS; i++) {
		json_t *o = json_at(a, i);
		struct place *p = &results[nresults++];
		snprintf(p->name, sizeof(p->name), "%s", json_str(json_get(o, "name"), "?"));
		const char *a1 = json_str(json_get(o, "admin1"), ""), *co = json_str(json_get(o, "country"), "");
		snprintf(p->region, sizeof(p->region), "%s%s%s", a1, *a1 && *co ? ", " : "", co);
		p->lat = json_num(json_get(o, "latitude"), 0);
		p->lon = json_num(json_get(o, "longitude"), 0);
		clean(p->name);
		clean(p->region);
	}
	json_free(j);
	sstate = S_DONE;
	list_build();
}

static void search_tick(lv_timer_t *t)
{
	lv_timer_pause(t);
	net_cancel(sreq);
	sreq = NULL;
	if (!query[0]) {
		sstate = S_IDLE;
		list_build();
		return;
	}
	char enc[300], url[500];
	net_urlencode(query, enc, sizeof(enc));
	snprintf(url, sizeof(url), "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=%d&language=en&format=json",
		 enc, MAX_RESULTS);
	sreq = net_get(url, 64 * 1024, 15, geo_done, NULL);
	sstate = sreq ? S_SEARCHING : S_FAILED;
	list_build();
}

/* ---------- detail page ---------- */

static lv_obj_t *card(lv_obj_t *parent, const char *glyph, const char *title)
{
	lv_obj_t *c = box(parent);
	lv_obj_set_size(c, CARD_W, LV_SIZE_CONTENT);
	lv_obj_set_style_radius(c, 36, 0);
	lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(c, C(pal.cell), 0);
	lv_obj_set_style_pad_all(c, 28, 0);
	lv_obj_set_style_pad_row(c, 14, 0);
	lv_obj_set_style_margin_top(c, 24, 0);
	lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
	lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *h = box(c);
	lv_obj_set_size(h, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(h, 12, 0);
	lv_obj_remove_flag(h, LV_OBJ_FLAG_CLICKABLE);
	ui_icon(h, glyph, font(F_MONO, 28), pal.label2);
	text(h, title, font(F_TEXT_SEMI, 24), pal.label2);
	return c;
}

static void hairline(lv_obj_t *parent)
{
	lv_obj_t *l = box(parent);
	lv_obj_set_size(l, LV_PCT(100), 1);
	lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(l, C(pal.separator), 0);
}

static void hourly(lv_obj_t *col, const struct wx *w)
{
	lv_obj_t *c = card(col, G_CLOCK, "Hourly forecast");
	hairline(c);
	lv_obj_t *row = box(c);
	lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_scroll_dir(row, LV_DIR_HOR);
	lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_GESTURE_BUBBLE);
	for (int i = 0; i < w->nh; i++) {
		lv_obj_t *it = box(row);
		lv_obj_set_size(it, 104, LV_SIZE_CONTENT);
		lv_obj_set_flex_flow(it, LV_FLEX_FLOW_COLUMN);
		lv_obj_set_flex_align(it, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
		lv_obj_set_style_pad_row(it, 10, 0);
		lv_obj_remove_flag(it, LV_OBJ_FLAG_CLICKABLE);
		char b[16];
		if (i == 0)
			snprintf(b, sizeof(b), "Now");
		else
			fmt_hour(b, sizeof(b), w->hhour[i], 0, false);
		text(it, b, font(F_TEXT_SEMI, 26), pal.label);
		lv_obj_t *g = ui_icon_line(it, wx_glyph(w->hcode[i], w->hday[i]), font(F_MONO, 50),
					   wx_color(w->hcode[i], w->hday[i]));	/* one centre line for the row of icons */
		lv_obj_set_style_pad_top(g, lv_obj_get_style_pad_top(g, 0) + 4, 0);
		lv_obj_set_height(g, lv_font_get_line_height(font(F_MONO, 50)) + 4);
		if (w->hpop[i] >= 20)
			snprintf(b, sizeof(b), "%d%%", w->hpop[i] / 10 * 10);
		else
			b[0] = 0;
		text(it, b, font(F_TEXT_SEMI, 22), pal.teal);
		fmt_temp(b, sizeof(b), i == 0 ? w->t : w->ht[i]);
		text(it, b, font(F_TEXT_SEMI, 32), pal.label);
	}
}

static void daily(lv_obj_t *col, const struct wx *w)
{
	static const char *const DAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	lv_obj_t *c = card(col, G_CALENDAR, "7-day forecast");
	double lo = 1e9, hi = -1e9;
	for (int i = 0; i < w->nd; i++) {
		lo = fmin(lo, w->dmin[i]);
		hi = fmax(hi, w->dmax[i]);
	}
	if (hi - lo < 1)
		hi = lo + 1;
	const int BAR = 210;
	for (int i = 0; i < w->nd; i++) {
		hairline(c);
		lv_obj_t *r = box(c);
		lv_obj_set_size(r, LV_PCT(100), 62);
		lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
		lv_obj_t *d = text(r, i == 0 ? "Today" : w->dwday[i] >= 0 ? DAYS[w->dwday[i]] : "", font(F_TEXT_SEMI, 32),
				   pal.label);
		lv_obj_align(d, LV_ALIGN_LEFT_MID, 0, 0);
		lv_obj_t *g = text(r, wx_glyph(w->dcode[i], true), font(F_MONO, 44), wx_color(w->dcode[i], true));
		lv_obj_set_width(g, 56);	/* a column: each day's icon centred in it */
		ui_glyph_fit(g);
		lv_obj_align(g, LV_ALIGN_LEFT_MID, 136, w->dpop[i] >= 30 ? -12 : 0);
		if (w->dpop[i] >= 30) {
			char p[8];
			snprintf(p, sizeof(p), "%d%%", w->dpop[i] / 10 * 10);
			lv_obj_t *pl = text(r, p, font(F_TEXT_SEMI, 20), pal.teal);
			lv_obj_align(pl, LV_ALIGN_LEFT_MID, 138, 22);
		}
		char b[16];
		fmt_temp(b, sizeof(b), w->dmin[i]);
		lv_obj_t *l = text(r, b, font(F_TEXT_SEMI, 32), pal.label2);
		lv_obj_set_width(l, 80);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_align(l, LV_ALIGN_RIGHT_MID, -(80 + 16 + BAR + 16), 0);
		lv_obj_t *track = box(r);
		lv_obj_set_size(track, BAR, 10);
		lv_obj_set_style_radius(track, 5, 0);
		lv_obj_set_style_bg_opa(track, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(track, C(pal.fill), 0);
		lv_obj_align(track, LV_ALIGN_RIGHT_MID, -(80 + 16), 0);
		int x0 = (int)((w->dmin[i] - lo) / (hi - lo) * BAR), x1 = (int)((w->dmax[i] - lo) / (hi - lo) * BAR);
		if (x1 - x0 < 10)
			x1 = x0 + 10;
		lv_obj_t *fill = box(track);
		lv_obj_set_size(fill, x1 - x0, 10);
		lv_obj_set_pos(fill, x0, 0);
		lv_obj_set_style_radius(fill, 5, 0);
		lv_obj_set_style_bg_opa(fill, LV_OPA_COVER, 0);
		lv_obj_set_style_bg_color(fill, C(pal.teal), 0);
		lv_obj_set_style_bg_grad_color(fill, C(pal.orange), 0);
		lv_obj_set_style_bg_grad_dir(fill, LV_GRAD_DIR_HOR, 0);
		if (i == 0) {	/* today: where it is now */
			int xn = (int)((w->t - lo) / (hi - lo) * BAR);
			xn = xn < 0 ? 0 : xn > BAR - 10 ? BAR - 10 : xn;
			lv_obj_t *dot = box(track);
			lv_obj_set_size(dot, 14, 14);
			lv_obj_set_pos(dot, xn - 2, -2);
			lv_obj_set_style_radius(dot, 7, 0);
			lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
			lv_obj_set_style_bg_color(dot, C(pal.label), 0);
			lv_obj_set_style_border_width(dot, 3, 0);
			lv_obj_set_style_border_color(dot, C(pal.cell), 0);
		}
		fmt_temp(b, sizeof(b), w->dmax[i]);
		lv_obj_t *h = text(r, b, font(F_TEXT_SEMI, 32), pal.label);
		lv_obj_set_width(h, 80);
		lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_RIGHT, 0);
		lv_obj_align(h, LV_ALIGN_RIGHT_MID, 0, 0);
	}
}

static lv_obj_t *tile(lv_obj_t *grid, const char *glyph, const char *title, const char *value, const char *note)
{
	lv_obj_t *t = box(grid);
	lv_obj_set_size(t, (CARD_W - 24) / 2, 300);
	lv_obj_set_style_radius(t, 36, 0);
	lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(t, C(pal.cell), 0);
	lv_obj_set_style_pad_all(t, 26, 0);
	lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_t *h = box(t);
	lv_obj_set_size(h, LV_PCT(100), LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(h, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(h, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(h, 10, 0);
	text(h, glyph, font(F_MONO, 28), pal.label2);
	text(h, title, font(F_TEXT_SEMI, 24), pal.label2);
	lv_obj_t *v = text(t, value, font(F_TEXT_MEDIUM, 64), pal.label);
	lv_obj_align(v, LV_ALIGN_TOP_LEFT, 0, 52);
	if (note) {
		lv_obj_t *n = text(t, note, font(F_TEXT, 26), pal.label);
		lv_obj_set_width(n, (CARD_W - 24) / 2 - 52);
		lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
		lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	}
	return t;
}

static void tiles(lv_obj_t *col, const struct wx *w)
{
	lv_obj_t *g = box(col);
	lv_obj_set_size(g, CARD_W, LV_SIZE_CONTENT);
	lv_obj_set_style_margin_top(g, 24, 0);
	lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_style_pad_column(g, 24, 0);
	lv_obj_set_style_pad_row(g, 24, 0);
	lv_obj_remove_flag(g, LV_OBJ_FLAG_CLICKABLE);
	char v[48], n[160];
	fmt_temp(v, sizeof(v), w->feels);
	double diff = w->feels - w->t;
	snprintf(n, sizeof(n), "%s", fabs(diff) < 1.5 ? "Similar to the actual temperature."
				     : diff < 0 ? (w->wind > 15 ? "Wind is making it feel colder." : "Feels colder than it is.")
				     : (w->hum > 60 ? "Humidity is making it feel warmer." : "Feels warmer than it is."));
	tile(g, W_THERMO, "Feels like", v, n);
	snprintf(v, sizeof(v), "%d%%", (int)lround(w->hum));
	snprintf(n, sizeof(n), "%s", w->hum >= 70 ? "The air feels damp." : w->hum <= 30 ? "The air is dry." : "Comfortable humidity.");
	tile(g, W_HUMID, "Humidity", v, n);
	if (fahr())
		snprintf(v, sizeof(v), "%d mph", (int)lround(w->wind / 1.609344));
	else
		snprintf(v, sizeof(v), "%d km/h", (int)lround(w->wind));
	snprintf(n, sizeof(n), "From the %s (%d" DEG ").", compass(w->wdir), (int)lround(w->wdir));
	tile(g, W_WINDY, "Wind", v, n);
	double uv = w->uv >= 0 ? w->uv : w->duv[0];
	snprintf(v, sizeof(v), "%d", (int)lround(uv));
	snprintf(n, sizeof(n), "%s now. Today's high: %d.", uv_level(uv), (int)lround(w->duv[0]));
	tile(g, W_UV, "UV index", v, n);
	/* sunrise or sunset, whichever comes next (local minutes) */
	time_t now = time(NULL) + w->utc_off;
	struct tm tm;
	gmtime_r(&now, &tm);
	int mnow = tm.tm_hour * 60 + tm.tm_min;
	bool next_set = w->rise[0] >= 0 && mnow >= w->rise[0] && mnow < w->set[0];
	int m = next_set ? w->set[0] : (mnow < w->rise[0] ? w->rise[0] : (w->nd > 1 ? w->rise[1] : w->rise[0]));
	char other[24];
	fmt_hour(v, sizeof(v), m / 60, m % 60, true);
	int mo = next_set ? (w->nd > 1 ? w->rise[1] : w->rise[0]) : w->set[0];
	fmt_hour(other, sizeof(other), mo / 60, mo % 60, true);
	snprintf(n, sizeof(n), "%s: %s", next_set ? "Sunrise" : "Sunset", other);
	tile(g, next_set ? W_SUNSET : W_SUNRISE, next_set ? "Sunset" : "Sunrise", v, n);
	if (fahr())
		snprintf(v, sizeof(v), "%.2f", w->press * 0.0295300);
	else
		snprintf(v, sizeof(v), "%d", (int)lround(w->press));
	snprintf(n, sizeof(n), "%s at sea level.", fahr() ? "inHg" : "hPa");
	tile(g, W_GAUGE, "Pressure", v, n);
	snprintf(v, sizeof(v), "%d%%", w->dpop[0]);
	snprintf(n, sizeof(n), "Chance of rain or snow today.");
	tile(g, W_UMBRELLA, "Precipitation", v, n);
	int pm = 0;	/* the next hours' highest chance */
	for (int i = 1; i < w->nh && i < 13; i++)
		pm = w->hpop[i] > pm ? w->hpop[i] : pm;
	snprintf(v, sizeof(v), "%d%%", pm);
	snprintf(n, sizeof(n), "Highest chance in the next 12 hours.");
	tile(g, W_RAIN, "Next 12 hours", v, n);
}

static void detail_build(void)
{
	if (!detail_col || detail < 0 || detail >= ncity)
		return;
	struct city *c = &cities[detail];
	struct wx *w = &c->wx;
	int sy = lv_obj_get_scroll_y(detail_col);
	lv_obj_clean(detail_col);
	lv_obj_t *page = lv_obj_get_parent(detail_col);
	uint32_t sky = w->ok && !w->day ? pal.indigo : pal.blue;
	lv_obj_set_style_bg_color(page, C(mix(pal.bg, sky, th.dark ? 16 : 10)), 0);
	lv_obj_t *hero = box(detail_col);
	lv_obj_set_size(hero, CARD_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(hero, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(hero, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_remove_flag(hero, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_style_pad_top(hero, 10, 0);
	lv_obj_t *nm = text(hero, c->name, font(F_TEXT_MEDIUM, 52), pal.label);
	lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
	if (!w->ok) {
		lv_obj_t *s = text(hero, c->failed ? "Weather isn\xe2\x80\x99t available right now." : "Loading\xe2\x80\xa6",
				   font(F_TEXT, 32), pal.label2);
		lv_obj_set_style_pad_top(s, 200, 0);
		return;
	}
	char b[64];
	fmt_temp(b, sizeof(b), w->t);
	lv_obj_t *t = text(hero, b, font(F_DISPLAY_LIGHT, 190), pal.label);
	lv_obj_set_style_pad_left(t, 40, 0);	/* the degree sign hangs off the centre, as on iOS */
	lv_obj_set_style_margin_top(t, -14, 0);
	text(hero, wx_words(w->code), font(F_TEXT_MEDIUM, 38), pal.label2);
	char hi[16], lo[16];
	fmt_temp(hi, sizeof(hi), w->dmax[0]);
	fmt_temp(lo, sizeof(lo), w->dmin[0]);
	snprintf(b, sizeof(b), "H:%s  L:%s", hi, lo);
	text(hero, b, font(F_TEXT_MEDIUM, 36), pal.label);
	hourly(detail_col, w);
	daily(detail_col, w);
	tiles(detail_col, w);
	char up[32], foot[160];
	time_t ft = w->fetched;
	struct tm tm;
	localtime_r(&ft, &tm);
	fmt_hour(up, sizeof(up), tm.tm_hour, tm.tm_min, true);
	snprintf(foot, sizeof(foot), "Weather data by Open-Meteo.com\n%s %s", c->req ? "Updating\xe2\x80\xa6 Last update" : "Updated", up);
	lv_obj_t *f = text(detail_col, foot, font(F_TEXT, 24), pal.label2);
	lv_obj_set_style_text_align(f, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_pad_top(f, 30, 0);
	lv_obj_update_layout(detail_col);
	lv_obj_scroll_to_y(detail_col, sy, LV_ANIM_OFF);
}

static void detail_leave(void)
{
	detail_col = NULL;
	detail = -1;
}

static void open_city(int i)
{
	if (i < 0 || i >= ncity || detail_col)
		return;
	lv_obj_t *c = nav_push(cities[i].name);
	if (!c)
		return;
	nav_on_leave(detail_leave);
	detail = i;
	detail_col = c;
	nav_set_title(nav_page(), "");
	lv_obj_set_style_pad_bottom(c, 80, 0);
	fc_fetch(&cities[i], false);
	detail_build();
}

/* ---------- list page ---------- */

static void city_click(lv_event_t *e) { open_city((int)(intptr_t)lv_event_get_user_data(e)); }

static int menu_city = -1;

static void city_menu_pick(int id)
{
	int i = menu_city;
	if (i < 0 || i >= ncity)
		return;
	if (id == 1) {
		net_cancel(cities[i].req);
		memmove(cities + i, cities + i + 1, (ncity - i - 1) * sizeof(cities[0]));
		ncity--;
		/* requests in flight point at moved entries: restart them */
		for (int k = i; k < ncity; k++)
			if (cities[k].req) {
				net_cancel(cities[k].req);
				cities[k].req = NULL;
				fc_fetch(&cities[k], true);
			}
		cities_save();
		list_build();
	} else if (id == 2 && i > 0) {
		struct city tmp = cities[i];
		net_cancel(tmp.req);
		tmp.req = NULL;
		memmove(cities + 1, cities, i * sizeof(cities[0]));
		cities[0] = tmp;
		for (int k = 1; k <= i; k++)
			if (cities[k].req) {
				net_cancel(cities[k].req);
				cities[k].req = NULL;
				fc_fetch(&cities[k], true);
			}
		cities_save();
		list_build();
	}
}

static void city_hold(lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target(e);
	menu_city = (int)(intptr_t)lv_event_get_user_data(e);
	ui_haptic_tap();
	static const ui_menu_item_t it[] = { { 2, 0, G_LIST, "Move to Top" }, { 1, MI_RED, G_TRASH, "Remove City" } };
	lv_area_t a;
	lv_obj_get_coords(o, &a);
	ui_menu(it + (menu_city == 0), 2 - (menu_city == 0), SCR_W - SIDE, a.y2 + 12, a.y1 - 12, city_menu_pick);
}

static void city_card(lv_obj_t *parent, int i)
{
	struct city *c = &cities[i];
	struct wx *w = &c->wx;
	lv_obj_t *k = box(parent);
	lv_obj_set_size(k, CARD_W, 210);
	lv_obj_set_style_margin_bottom(k, 20, 0);
	lv_obj_set_style_radius(k, 40, 0);
	lv_obj_set_style_bg_opa(k, LV_OPA_COVER, 0);
	uint32_t sky = w->ok && !w->day ? pal.indigo : pal.blue;
	lv_obj_set_style_bg_color(k, C(mix(pal.cell, sky, 22)), 0);
	lv_obj_set_style_bg_grad_color(k, C(mix(pal.cell, sky, 8)), 0);
	lv_obj_set_style_bg_grad_dir(k, LV_GRAD_DIR_HOR, 0);
	lv_obj_set_style_bg_color(k, C(pal.cell_hi), LV_STATE_PRESSED);
	lv_obj_set_style_bg_grad_color(k, C(pal.cell_hi), LV_STATE_PRESSED);
	lv_obj_set_style_pad_all(k, 28, 0);
	lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_add_event_cb(k, city_click, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
	lv_obj_add_event_cb(k, city_hold, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
	lv_obj_t *n = text(k, c->name, font(F_TEXT_BOLD, 42), pal.label);
	lv_obj_set_width(n, CARD_W - 56 - 200);
	lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, -4);
	char b[96];
	if (w->ok) {
		time_t now = time(NULL) + w->utc_off;
		struct tm tm;
		gmtime_r(&now, &tm);
		fmt_hour(b, sizeof(b), tm.tm_hour, tm.tm_min, true);
	} else {
		snprintf(b, sizeof(b), "%s", c->region);
	}
	lv_obj_t *s = text(k, b, font(F_TEXT_MEDIUM, 26), pal.label);
	lv_obj_set_width(s, CARD_W - 56 - 200);
	lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, 50);
	lv_obj_t *cond = box(k);
	lv_obj_set_size(cond, CARD_W - 56 - 220, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(cond, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(cond, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(cond, 10, 0);
	lv_obj_remove_flag(cond, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_align(cond, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	if (w->ok) {
		ui_icon(cond, wx_glyph(w->code, w->day), font(F_MONO, 32), wx_color(w->code, w->day));
		text(cond, wx_words(w->code), font(F_TEXT_MEDIUM, 28), pal.label2);
	} else {
		text(cond, c->req ? "Loading\xe2\x80\xa6" : c->failed ? "Not available" : "", font(F_TEXT_MEDIUM, 28), pal.label2);
	}
	if (w->ok) {
		fmt_temp(b, sizeof(b), w->t);
		lv_obj_t *t = text(k, b, font(F_DISPLAY_LIGHT, 96), pal.label);
		lv_obj_align(t, LV_ALIGN_TOP_RIGHT, 0, -22);
		char hi[16], lo[16];
		fmt_temp(hi, sizeof(hi), w->dmax[0]);
		fmt_temp(lo, sizeof(lo), w->dmin[0]);
		snprintf(b, sizeof(b), "H:%s  L:%s", hi, lo);
		lv_obj_t *hl = text(k, b, font(F_TEXT_MEDIUM, 28), pal.label);
		lv_obj_align(hl, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
	}
}

/* after the tapped row's event: picking rebuilds the list it is in */
static void result_pick(void *ud)
{
	int r = (int)(intptr_t)ud;
	if (!list_box || r < 0 || r >= nresults)
		return;
	struct place *p = &results[r];
	int found = -1;
	for (int i = 0; i < ncity; i++)
		if (fabs(cities[i].lat - p->lat) < 0.01 && fabs(cities[i].lon - p->lon) < 0.01)
			found = i;
	if (found < 0 && ncity < MAX_CITIES) {
		struct city *c = &cities[ncity];
		memset(c, 0, sizeof(*c));
		snprintf(c->name, sizeof(c->name), "%s", p->name);
		snprintf(c->region, sizeof(c->region), "%s", p->region);
		c->lat = p->lat;
		c->lon = p->lon;
		found = ncity++;
		cities_save();
		fc_fetch(c, true);
	}
	lv_textarea_set_text(search_ta, "");
	kbd_hide();
	list_build();
	if (found >= 0)
		open_city(found);
}

static void result_click(lv_event_t *e) { lv_async_call(result_pick, lv_event_get_user_data(e)); }

static void empty_note(const char *title, const char *hint)
{
	lv_obj_t *t = text(list_box, title, font(F_TEXT_BOLD, 44), pal.label2);
	lv_obj_set_style_pad_top(t, 200, 0);
	lv_obj_t *h = text(list_box, hint, font(F_TEXT, 30), pal.label2);
	lv_obj_set_width(h, CARD_W);
	lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_long_mode(h, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_pad_top(h, 14, 0);
}

static void list_build(void)
{
	if (!list_box)
		return;
	lv_obj_clean(list_box);
	if (query[0]) {
		if (sstate == S_SEARCHING && !nresults) {
			lv_obj_t *s = text(list_box, "Searching\xe2\x80\xa6", font(F_TEXT, 30), pal.label2);
			lv_obj_set_style_pad_top(s, 60, 0);
			return;
		}
		if (sstate == S_FAILED) {
			empty_note("Can\xe2\x80\x99t Search", net_available() ? "Check your internet connection."
								    : "The internet tools (curl) aren\xe2\x80\x99t installed.");
			return;
		}
		if (sstate == S_DONE && !nresults) {
			empty_note("No Results", "No city by that name was found.");
			return;
		}
		lv_obj_t *sec = ui_section(list_box, NULL, NULL);
		for (int i = 0; i < nresults; i++) {
			lv_obj_t *r = ui_row(sec, NULL, 0, results[i].name);
			ui_row_subtitle(r, results[i].region);
			ui_row_tap(r, result_click, (void *)(intptr_t)i);
		}
		return;
	}
	if (!ncity) {
		empty_note("No Cities", "Search for a city to see its weather.");
		return;
	}
	lv_obj_t *gap = box(list_box);
	lv_obj_set_size(gap, 1, 24);
	for (int i = 0; i < ncity; i++)
		city_card(list_box, i);
	lv_obj_t *f = text(list_box, "Hold a city to move or remove it.", font(F_TEXT, 24), pal.label3);
	lv_obj_set_style_pad_top(f, 10, 0);
}

static void ui_refresh(void)
{
	list_build();
	detail_build();
}

static void unit_click(lv_event_t *e)
{
	(void)e;
	prefs_set_int("weather_f", !fahr());
	if (unit_btn)
		lv_label_set_text(lv_obj_get_child(unit_btn, 0), fahr() ? DEG "F" : DEG "C");
	ui_haptic_tap();
	ui_refresh();
}

/* search field (same behaviour as Notes') */
static bool search_active(void) { return search_ta && kbd_visible() && lv_obj_has_state(search_ta, LV_STATE_FOCUSED); }

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
	lv_obj_set_style_pad_bottom(list_col, on ? kbd_height(KBD_TEXT) + 40 : 140, 0);
}

static void search_changed(lv_event_t *e)
{
	(void)e;
	snprintf(query, sizeof(query), "%s", lv_textarea_get_text(search_ta));
	search_chrome();
	if (!query[0]) {
		net_cancel(sreq);
		sreq = NULL;
		sstate = S_IDLE;
		nresults = 0;
		list_build();
		return;
	}
	lv_timer_reset(search_timer);
	lv_timer_resume(search_timer);
}

static void search_focus(lv_event_t *e)
{
	(void)e;
	if (search_active())
		kbd_no_autocap();
	search_chrome();
}

static void search_ready(lv_event_t *e)
{
	(void)e;
	kbd_hide();
	search_tick(search_timer);	/* now, not after the pause */
}

static void search_clear_click(lv_event_t *e)
{
	(void)e;
	lv_textarea_set_text(search_ta, "");
	kbd_attach(search_ta, KBD_TEXT);
	kbd_no_autocap();
}

static void search_cancel_click(lv_event_t *e)
{
	(void)e;
	lv_textarea_set_text(search_ta, "");
	kbd_hide();
	search_chrome();
}

static void refresh_tick(lv_timer_t *t)
{
	(void)t;
	for (int i = 0; i < ncity; i++)
		fc_fetch(&cities[i], false);
}

static void app_open(lv_obj_t *root)
{
	cities_load();
	query[0] = 0;
	sstate = S_IDLE;
	nresults = 0;
	list_col = nav_root(root, "Weather");
	unit_btn = nav_button(fahr() ? DEG "F" : DEG "C", unit_click, NULL);
	lv_obj_t *row = box(list_col);
	lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(row, CARD_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_column(row, 24, 0);
	search_ta = ui_search(row, CARD_W, "Search for a city");
	lv_obj_t *wrap = lv_obj_get_parent(search_ta);
	lv_obj_set_flex_grow(wrap, 1);
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
	list_box = box(list_col);
	lv_obj_remove_flag(list_box, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_set_size(list_box, SCR_W, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(list_box, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(list_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	search_timer = lv_timer_create(search_tick, 500, NULL);
	lv_timer_pause(search_timer);
	refresh_timer = lv_timer_create(refresh_tick, 60 * 1000, NULL);
	search_chrome();
	refresh_tick(NULL);
	list_build();
}

static void app_close(void)
{
	lv_async_call_cancel(result_pick, NULL);
	if (search_timer)
		lv_timer_delete(search_timer);
	if (refresh_timer)
		lv_timer_delete(refresh_timer);
	search_timer = refresh_timer = NULL;
	net_cancel(sreq);
	sreq = NULL;
	list_col = list_box = unit_btn = search_ta = search_clear = search_cancel = NULL;
	detail_col = NULL;
	detail = -1;
	menu_city = -1;
	/* forecasts in flight keep going: their answers land in the cache for next time */
}

const app_t app_weather = {
	.name = "Weather",
	.glyph = W_PCLOUD,
	.icon = IC_BLUE,
	.open = app_open,
	.close = app_close,
};
