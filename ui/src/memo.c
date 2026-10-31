/* Voice Memos' pure parts: see memo.h */
#include "memo.h"
#include <stdio.h>
#include <string.h>

static void le16(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t rd16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void memo_wav_header(uint8_t h[MEMO_HDR], uint32_t rate, int channels, uint32_t data_bytes)
{
	memcpy(h, "RIFF", 4);
	le32(h + 4, 36 + data_bytes);
	memcpy(h + 8, "WAVEfmt ", 8);
	le32(h + 16, 16);
	le16(h + 20, 1);	/* PCM */
	le16(h + 22, channels);
	le32(h + 24, rate);
	le32(h + 28, rate * channels * 2);
	le16(h + 32, channels * 2);
	le16(h + 34, 16);
	memcpy(h + 36, "data", 4);
	le32(h + 40, data_bytes);
}

bool memo_wav_parse(const uint8_t *buf, size_t n, uint64_t file_size, memo_wav_t *out)
{
	memset(out, 0, sizeof(*out));
	if (n < 12 || memcmp(buf, "RIFF", 4) || memcmp(buf + 8, "WAVE", 4))
		return false;
	bool fmt = false;
	size_t p = 12;
	while (p + 8 <= n) {
		uint32_t len = rd32(buf + p + 4);
		if (!memcmp(buf + p, "fmt ", 4)) {
			if (len < 16 || p + 8 + 16 > n)
				return false;
			const uint8_t *f = buf + p + 8;
			uint32_t tag = rd16(f);
			if (tag == 0xfffe && len >= 40 && p + 8 + 26 <= n)	/* WAVE_FORMAT_EXTENSIBLE: the sub-format */
				tag = rd16(f + 24);
			if (tag != 1)
				return false;
			out->channels = rd16(f + 2);
			out->rate = rd32(f + 4);
			out->bits = rd16(f + 14);
			fmt = true;
		} else if (!memcmp(buf + p, "data", 4)) {
			if (!fmt || out->bits != 16 || out->channels < 1 || out->channels > 8 || out->rate < 8000 ||
			    out->rate > 192000)
				return false;
			out->data_off = p + 8;
			uint64_t rest = file_size > out->data_off ? file_size - out->data_off : 0;
			uint64_t bytes = len == 0 || len > rest ? rest : len;
			uint32_t frame = out->channels * 2;
			if (bytes > 0xffffffffu)
				bytes = 0xffffffffu;
			out->data_bytes = (uint32_t)(bytes / frame * frame);
			return true;
		}
		uint64_t next = (uint64_t)p + 8 + len + (len & 1);	/* chunks are word-aligned */
		if (next > n)
			return false;	/* (the data chunk must be in the bytes read) */
		p = (size_t)next;
	}
	return false;
}

uint32_t memo_wav_ms(const memo_wav_t *w)
{
	if (!w->rate || !w->channels)
		return 0;
	return (uint32_t)((uint64_t)w->data_bytes / (w->channels * 2) * 1000 / w->rate);
}

int memo_pick(const int16_t *in, int frames, int ch_in, int pick, int16_t *out)
{
	int peak = 0;
	if (pick < 0 || pick >= ch_in)
		pick = 0;
	for (int i = 0; i < frames; i++) {
		int16_t s = in[i * ch_in + pick];
		out[i] = s;
		int a = s < 0 ? -s : s;
		if (a > peak)
			peak = a;
	}
	return peak;
}

int memo_level(int peak)
{
	/* 20*log10(peak/32768) in 0.5 dB steps without libm: compare against the thresholds */
	if (peak <= 0)
		return 0;
	double db = 0, v = peak / 32768.0;
	while (v < 0.5) {	/* -6.02 dB per halving */
		v *= 2;
		db -= 6.0206;
	}
	/* the rest (0.5..1): 20*log10(v) ≈ 8.6859 * ln v, ln v ≈ 2 * atanh((v-1)/(v+1)) */
	double z = (v - 1) / (v + 1);
	db += 8.6859 * 2 * (z + z * z * z / 3);
	if (db <= -54)
		return 0;
	if (db >= 0)
		return 100;
	double x = (db + 54) / 54;	/* 0..1, then a square-root-ish curve: quiet speech still moves the bars */
	double r = x;
	for (int i = 0; i < 8; i++)
		r = (r + x / r) / 2;
	int l = (int)(r * 100 + 0.5);
	return l < 0 ? 0 : l > 100 ? 100 : l;
}

void memo_fmt_dur(char *buf, size_t n, uint32_t ms)
{
	uint32_t s = ms / 1000;
	if (s >= 3600)
		snprintf(buf, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
	else
		snprintf(buf, n, "%u:%02u", s / 60, s % 60);
}

void memo_fmt_timer(char *buf, size_t n, uint32_t ms)
{
	uint32_t s = ms / 1000, cs = ms % 1000 / 10;
	if (s >= 3600)
		snprintf(buf, n, "%u:%02u:%02u.%02u", s / 3600, s / 60 % 60, s % 60, cs);
	else
		snprintf(buf, n, "%02u:%02u.%02u", s / 60, s % 60, cs);
}

static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

static void fmt_clock(char *buf, size_t n, const struct tm *tm, bool h24)
{
	if (h24)
		snprintf(buf, n, "%02d:%02d", tm->tm_hour, tm->tm_min);
	else
		snprintf(buf, n, "%d:%02d %s", (tm->tm_hour + 11) % 12 + 1, tm->tm_min, tm->tm_hour < 12 ? "AM" : "PM");
}

void memo_default_name(char *buf, size_t n, time_t t, bool h24)
{
	struct tm tm;
	char c[16];
	localtime_r(&t, &tm);
	fmt_clock(c, sizeof(c), &tm, h24);
	snprintf(buf, n, "%s %d, %s", mon[tm.tm_mon], tm.tm_mday, c);
}

void memo_fmt_when(char *buf, size_t n, time_t t, time_t now, bool h24)
{
	struct tm tm, today;
	char c[16];
	localtime_r(&t, &tm);
	localtime_r(&now, &today);
	fmt_clock(c, sizeof(c), &tm, h24);
	struct tm y = today;	/* yesterday: mktime normalises day 0 */
	y.tm_mday -= 1;
	y.tm_isdst = -1;
	time_t yt = mktime(&y);
	struct tm yd;
	localtime_r(&yt, &yd);
	if (tm.tm_year == today.tm_year && tm.tm_yday == today.tm_yday)
		snprintf(buf, n, "Today %s", c);
	else if (tm.tm_year == yd.tm_year && tm.tm_yday == yd.tm_yday)
		snprintf(buf, n, "Yesterday %s", c);
	else
		snprintf(buf, n, "%s %d, %d", mon[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
}

bool memo_clean_name(char *s, size_t n)
{
	if (n < 6)
		return false;
	size_t w = 0;
	for (size_t r = 0; s[r]; r++) {
		unsigned char ch = s[r];
		if (ch == '/' || ch < 0x20 || ch == 0x7f)
			continue;
		if (w == 0 && (ch == '.' || ch == ' '))
			continue;
		s[w++] = ch;
	}
	s[w] = 0;
	size_t max = n - 5;	/* + ".wav" + NUL */
	if (w > max) {
		w = max;
		while (w > 0 && ((unsigned char)s[w] & 0xc0) == 0x80)	/* s[w] continues a character: cut before it */
			w--;
		s[w] = 0;
	}
	while (w > 0 && s[w - 1] == ' ')
		s[--w] = 0;
	return w > 0;
}
