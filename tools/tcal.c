/* tcal: touchscreen calibration. Draws crosshair targets on /dev/fb0 (Orchard must be stopped),
 * records where each tap lands in raw panel units and prints one line per target:
 *   T <i> <screen x> <screen y> <raw x median> <raw y median> <samples> <raw x spread> <raw y spread>
 * Fit on the PC (bin/tcalfit). Build: see bin/tcal. */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define W 750
#define H 1334
static uint32_t *fb;
static int stride;

static void px(int x, int y, uint32_t c)
{
	if (x >= 0 && x < W && y >= 0 && y < H)
		fb[y * stride + x] = c;
}

static void rect(int x0, int y0, int w, int h, uint32_t c)
{
	for (int y = y0; y < y0 + h; y++)
		for (int x = x0; x < x0 + w; x++)
			px(x, y, c);
}

static void ring(int cx, int cy, int r, int t, uint32_t c)
{
	for (int y = -r - t; y <= r + t; y++)
		for (int x = -r - t; x <= r + t; x++) {
			int d = x * x + y * y;
			if (d >= (r - t) * (r - t) && d <= (r + t) * (r + t))
				px(cx + x, cy + y, c);
		}
}

static void target(int cx, int cy, uint32_t c)
{
	rect(cx - 40, cy - 1, 81, 3, c);
	rect(cx - 1, cy - 40, 3, 81, c);
	ring(cx, cy, 22, 1, c);
}

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int median(int *v, int n)
{
	qsort(v, n, sizeof(*v), cmp);
	return v[n / 2];
}

static long ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
	static const int xs[] = { 50, 375, 700 }, ys[] = { 60, 360, 667, 974, 1274 };
	const int nt = 15;
	int ffd = open("/dev/fb0", O_RDWR), ifd = open(argc > 1 ? argv[1] : "/dev/input/event1", O_RDONLY);
	if (ffd < 0 || ifd < 0) {
		perror("open");
		return 1;
	}
	struct fb_fix_screeninfo fi;
	ioctl(ffd, FBIOGET_FSCREENINFO, &fi);
	stride = fi.line_length / 4;
	fb = mmap(NULL, fi.line_length * H, PROT_READ | PROT_WRITE, MAP_SHARED, ffd, 0);
	if (fb == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	struct input_absinfo ax, ay;
	ioctl(ifd, EVIOCGABS(ABS_X), &ax);
	ioctl(ifd, EVIOCGABS(ABS_Y), &ay);
	printf("ABS X %d..%d fuzz %d res %d | Y %d..%d fuzz %d res %d\n", ax.minimum, ax.maximum, ax.fuzz,
	       ax.resolution, ay.minimum, ay.maximum, ay.fuzz, ay.resolution);
	fflush(stdout);

	static int sx[4096], sy[4096];
	int rx = 0, ry = 0, down = 0, n = 0;
	long up_at = 0;
	for (int i = 0; i < nt; i++) {
		int cx = xs[i % 3], cy = ys[i / 3];
		rect(0, 0, W, H, 0x101418);
		for (int k = 0; k < nt; k++)	/* progress dots */
			rect(W / 2 - nt * 12 + k * 24, H / 2 - 120, 14, 14, k < i ? 0x30c050 : 0x404850);
		target(cx, cy, 0xffffff);
		for (;;) {
			struct input_event ev;
			if (read(ifd, &ev, sizeof(ev)) != sizeof(ev))
				return 1;
			if (ev.type == EV_ABS && ev.code == ABS_X)
				rx = ev.value;
			else if (ev.type == EV_ABS && ev.code == ABS_Y)
				ry = ev.value;
			else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
				if (ev.value && ms() - up_at > 300) {
					down = 1;
					n = 0;
					target(cx, cy, 0xffc030);
				} else if (!ev.value && down) {
					down = 0;
					up_at = ms();
					if (n >= 3)
						break;
					target(cx, cy, 0xffffff);
				}
			} else if (ev.type == EV_SYN && ev.code == SYN_REPORT && down && n < 4096) {
				sx[n] = rx;
				sy[n] = ry;
				n++;
			}
		}
		/* drop the landing and lift-off frames, they wobble */
		int a = n > 6 ? 2 : 0, m = n - 2 * a;
		int lox = 1 << 30, hix = -1, loy = 1 << 30, hiy = -1;
		for (int k = a; k < a + m; k++) {
			lox = sx[k] < lox ? sx[k] : lox, hix = sx[k] > hix ? sx[k] : hix;
			loy = sy[k] < loy ? sy[k] : loy, hiy = sy[k] > hiy ? sy[k] : hiy;
		}
		printf("T %d %d %d %d %d %d %d %d\n", i, cx, cy, median(sx + a, m), median(sy + a, m), n, hix - lox,
		       hiy - loy);
		fflush(stdout);
	}
	rect(0, 0, W, H, 0x103018);
	printf("done\n");
	return 0;
}
