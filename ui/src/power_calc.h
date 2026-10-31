/* Battery arithmetic for Settings › Battery (pure: test/unit/power_test.c). */
#pragma once
#include <stdio.h>

/* average drain since unplugged in 0.1 %/h; 0 = not enough to say yet (< 20 min or < 2 % used) */
static inline int power_rate_x10(int used_pct, long secs)
{
	if (secs < 20 * 60 || used_pct < 2)
		return 0;
	return (int)(used_pct * 36000L / secs);
}

/* minutes left: at the average since unplugged when there is one, else at the present draw (ma < 0 =
 * discharging, charge_mah left); -1 = no estimate */
static inline long power_minutes_left(int pct, int rate_x10, int charge_mah, int ma)
{
	if (pct < 0)
		return -1;
	if (rate_x10 > 0)
		return pct * 600L / rate_x10;
	if (ma < 0 && charge_mah > 0)
		return charge_mah * 60L / -ma;
	return -1;
}

/* "3 h 05 min", "12 min", "< 1 min" */
static inline void power_fmt_dur(char *buf, int n, long secs)
{
	long m = secs / 60;
	if (secs < 60)
		snprintf(buf, n, "< 1 min");
	else if (m < 60)
		snprintf(buf, n, "%ld min", m);
	else
		snprintf(buf, n, "%ld h %02ld min", m / 60, m % 60);
}
