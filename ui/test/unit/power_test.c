/* power_calc.h: drain rate, time left, durations */
#include <stdio.h>
#include <string.h>
#include "power_calc.h"

static int fails;
#define EQ(a, b) do { long _a = (a), _b = (b); if (_a != _b) { printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #a, _a, _b); fails++; } } while (0)
#define SEQ(a, b) do { if (strcmp(a, b)) { printf("FAIL %s:%d: \"%s\", want \"%s\"\n", __FILE__, __LINE__, a, b); fails++; } } while (0)

int main(void)
{
	EQ(power_rate_x10(10, 19 * 60), 0);	/* too soon */
	EQ(power_rate_x10(1, 3600), 0);		/* too little */
	EQ(power_rate_x10(10, 3600), 100);	/* 10 %/h */
	EQ(power_rate_x10(3, 2 * 3600), 15);	/* 1.5 %/h */
	EQ(power_minutes_left(50, 100, 0, 0), 300);	/* 50 % at 10 %/h = 5 h */
	EQ(power_minutes_left(50, 0, 900, -300), 180);	/* 900 mAh at 300 mA = 3 h */
	EQ(power_minutes_left(50, 0, 900, 120), -1);	/* charging */
	EQ(power_minutes_left(-1, 100, 900, -300), -1);	/* no gauge */
	EQ(power_minutes_left(80, 15, 0, 0), 3200);	/* 1.5 %/h: 53 h */
	char b[32];
	power_fmt_dur(b, sizeof(b), 30);
	SEQ(b, "< 1 min");
	power_fmt_dur(b, sizeof(b), 12 * 60 + 59);
	SEQ(b, "12 min");
	power_fmt_dur(b, sizeof(b), 3 * 3600 + 5 * 60);
	SEQ(b, "3 h 05 min");
	printf("power_test: %s\n", fails ? "FAILED" : "ok");
	return fails != 0;
}
