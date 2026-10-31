/* Voice Memos' pure parts (memo.c, no LVGL): WAV headers, the capture's channel pick + level, names,
 * durations. test/unit/memo_test.c */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define MEMO_RATE 48000
#define MEMO_HDR 44	/* the header memo_wav_header writes */

typedef struct memo_wav {
	uint32_t rate;
	int channels, bits;
	uint32_t data_off, data_bytes;	/* where the samples are; bytes = whole frames only */
} memo_wav_t;

/* canonical 44-byte PCM WAV header (16-bit) for data_bytes of samples */
void memo_wav_header(uint8_t h[MEMO_HDR], uint32_t rate, int channels, uint32_t data_bytes);
/* parse the first n bytes of a file of file_size bytes: RIFF/WAVE, a PCM fmt chunk and the data chunk (other
 * chunks skipped). A data size of 0 or past the end (a recording that never got its final header) = the rest of
 * the file. false = not a 16-bit PCM WAV we can play */
bool memo_wav_parse(const uint8_t *buf, size_t n, uint64_t file_size, memo_wav_t *out);
uint32_t memo_wav_ms(const memo_wav_t *w);	/* its length */

/* channel `pick` of `frames` interleaved S16 frames of ch_in channels into out (mono); returns the largest
 * |sample| seen */
int memo_pick(const int16_t *in, int frames, int ch_in, int pick, int16_t *out);
/* meter 0..100 for a peak sample: -54 dBFS..0 dBFS, square-root shaped like iOS' bars */
int memo_level(int peak);

/* "0:05", "12:34", "1:02:03" (list rows, the scrubber) */
void memo_fmt_dur(char *buf, size_t n, uint32_t ms);
/* the recording timer: "00:05.32", "12:34.05", "1:02:03.40" */
void memo_fmt_timer(char *buf, size_t n, uint32_t ms);
/* a new recording's name: "Oct 21, 10:45 PM" / "Oct 21, 22:45" (h24) */
void memo_default_name(char *buf, size_t n, time_t t, bool h24);
/* a row's second line from the file's time: "Today 10:45 PM", "Yesterday 22:45", "Oct 19, 2026" (now = today) */
void memo_fmt_when(char *buf, size_t n, time_t t, time_t now, bool h24);
/* a user-typed name made safe as a file name in place: no '/', control characters or leading dots, spaces
 * trimmed, at most n - 5 bytes (room for ".wav", never half a UTF-8 character); false = nothing left */
bool memo_clean_name(char *s, size_t n);
