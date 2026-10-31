/* memo.c: Voice Memos' WAV headers, channel pick, meter, names, durations */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memo.h"

static int fails;
#define EQ(a, b) do { long _a = (long)(a), _b = (long)(b); if (_a != _b) { printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)
#define SEQ(a, b) do { if (strcmp(a, b)) { printf("FAIL %s:%d: \"%s\", want \"%s\"\n", __FILE__, __LINE__, a, b); fails++; } } while (0)

static void t_wav(void)
{
	uint8_t h[MEMO_HDR];
	memo_wav_t w;
	memo_wav_header(h, 48000, 1, 96000);
	EQ(memo_wav_parse(h, sizeof(h), MEMO_HDR + 96000, &w), 1);
	EQ(w.rate, 48000);
	EQ(w.channels, 1);
	EQ(w.bits, 16);
	EQ(w.data_off, 44);
	EQ(w.data_bytes, 96000);
	EQ(memo_wav_ms(&w), 1000);
	/* a recording cut off before its header was finished: size from the file */
	memo_wav_header(h, 48000, 1, 0);
	EQ(memo_wav_parse(h, sizeof(h), MEMO_HDR + 48001, &w), 1);
	EQ(w.data_bytes, 48000);	/* whole frames only */
	EQ(memo_wav_ms(&w), 500);
	/* a header claiming more than is there */
	memo_wav_header(h, 48000, 2, 1000000);
	EQ(memo_wav_parse(h, sizeof(h), MEMO_HDR + 4000, &w), 1);
	EQ(w.data_bytes, 4000);
	/* other chunks before data (LIST), odd-sized one padded */
	uint8_t b[128];
	memo_wav_header(h, 44100, 2, 8);
	memcpy(b, h, 36);
	memcpy(b + 36, "LIST", 4);
	b[40] = 5; b[41] = b[42] = b[43] = 0;
	memset(b + 44, 'x', 6);	/* 5 bytes + 1 pad */
	memcpy(b + 50, h + 36, 8);
	EQ(memo_wav_parse(b, 58, 58 + 8, &w), 1);
	EQ(w.data_off, 58);
	EQ(w.rate, 44100);
	EQ(w.data_bytes, 8);
	/* not ours */
	EQ(memo_wav_parse((const uint8_t *)"RIFF....WAVX", 12, 100, &w), 0);
	EQ(memo_wav_parse(h, 20, 100, &w), 0);	/* truncated */
	memo_wav_header(h, 48000, 1, 10);
	h[34] = 8;	/* 8-bit */
	EQ(memo_wav_parse(h, sizeof(h), 100, &w), 0);
	memo_wav_header(h, 48000, 1, 10);
	h[20] = 3;	/* float */
	EQ(memo_wav_parse(h, sizeof(h), 100, &w), 0);
	/* a huge chunk length must not wrap */
	memo_wav_header(h, 48000, 1, 10);
	memcpy(b, h, 36);
	memcpy(b + 36, "JUNK", 4);
	b[40] = b[41] = b[42] = b[43] = 0xff;
	EQ(memo_wav_parse(b, 44, 1000, &w), 0);
}

static void t_pick(void)
{
	int16_t in[8] = { 1, -100, 2, 200, -3, -32768, 4, 5 }, out[4];
	EQ(memo_pick(in, 4, 2, 0, out), 4);
	EQ(out[0], 1); EQ(out[2], -3); EQ(out[3], 4);
	EQ(memo_pick(in, 4, 2, 1, out), 32768);
	EQ(out[1], 200); EQ(out[2], -32768);
	EQ(memo_pick(in, 4, 2, 7, out), 4);	/* out of range = channel 0 */
	EQ(memo_level(0), 0);
	EQ(memo_level(32767), 100);
	EQ(memo_level(32768), 100);
	EQ(memo_level(60), 0);	/* -54.7 dBFS */
	int l1 = memo_level(1036);	/* -30 dBFS: (24/54)^0.5 = 67 */
	EQ(l1 >= 66 && l1 <= 68, 1);
	int prev = -1;
	for (int p = 1; p <= 32768; p += 97) {	/* never goes down */
		int l = memo_level(p);
		if (l < prev) {
			printf("FAIL memo_level not monotonic at %d\n", p);
			fails++;
			break;
		}
		prev = l;
	}
}

static void t_fmt(void)
{
	char b[64];
	memo_fmt_dur(b, sizeof(b), 0); SEQ(b, "0:00");
	memo_fmt_dur(b, sizeof(b), 5999); SEQ(b, "0:05");
	memo_fmt_dur(b, sizeof(b), 754000); SEQ(b, "12:34");
	memo_fmt_dur(b, sizeof(b), 3723000); SEQ(b, "1:02:03");
	memo_fmt_timer(b, sizeof(b), 5321); SEQ(b, "00:05.32");
	memo_fmt_timer(b, sizeof(b), 754050); SEQ(b, "12:34.05");
	memo_fmt_timer(b, sizeof(b), 3723400); SEQ(b, "1:02:03.40");

	setenv("TZ", "EST5EDT", 1);
	tzset();
	time_t t = 1792637100;	/* 2026-10-21 22:45 EDT */
	memo_default_name(b, sizeof(b), t, false); SEQ(b, "Oct 21, 10:45 PM");
	memo_default_name(b, sizeof(b), t, true); SEQ(b, "Oct 21, 22:45");
	memo_default_name(b, sizeof(b), t - 22 * 3600 - 15 * 60, false); SEQ(b, "Oct 21, 12:30 AM");
	memo_default_name(b, sizeof(b), t - 10 * 3600 - 15 * 60, false); SEQ(b, "Oct 21, 12:30 PM");
	memo_fmt_when(b, sizeof(b), t, t + 60, false); SEQ(b, "Today 10:45 PM");
	memo_fmt_when(b, sizeof(b), t, t + 2 * 3600, true); SEQ(b, "Yesterday 22:45");
	memo_fmt_when(b, sizeof(b), t, t + 3 * 86400, false); SEQ(b, "Oct 21, 2026");

	strcpy(b, "  ../a/b\tc  ");
	EQ(memo_clean_name(b, sizeof(b)), 1); SEQ(b, "abc");
	strcpy(b, " ./ ");
	EQ(memo_clean_name(b, sizeof(b)), 0);
	strcpy(b, "Oct 21, 10:45 PM");
	EQ(memo_clean_name(b, sizeof(b)), 1); SEQ(b, "Oct 21, 10:45 PM");
	char s[11];
	strcpy(s, "ab\xc3\xa9\xc3\xa9xyz");	/* "abééxyz", 9 bytes; room for 6 */
	EQ(memo_clean_name(s, sizeof(s)), 1); SEQ(s, "ab\xc3\xa9\xc3\xa9");
	strcpy(s, "abc\xc3\xa9\xc3\xa9z");	/* the cut lands inside the second é */
	EQ(memo_clean_name(s, sizeof(s)), 1); SEQ(s, "abc\xc3\xa9");
}

int main(void)
{
	t_wav();
	t_pick();
	t_fmt();
	printf("memo_test: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
