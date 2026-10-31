/* camvf-bench (camfix): the viewfinder's camera-side costs, the camfix converter against camint's. Build:
 * ui/test/unit/build-camvf-bench.sh. Usage:
 *   camvf-bench [rear|front] [frames, default 240]    live /dev/isp0 frames (the Camera app must be closed)
 *   camvf-bench synth [rear|front] [frames]            a random frame of the phone's size, no camera (any aarch64 box)
 * Per frame, round robin (every method sees the same light / fps / CPU clock), 750x1000 out, as the app:
 *   A camint's converter (orchard-camint e63f93a, test/unit/cam_conv_vf.c) = what runs on the phone now
 *   B camfix's converter (NEON tbl gather, 16-bit maths, tone curve)
 *   C camfix's converter, scalar path (cam_conv_scalar: what B is without NEON)
 * plus read() itself (with mmap_cached=1 it invalidates the frame's cache lines), the driver timestamp intervals,
 * pickup delay (read time - driver timestamp), cpu0's clock. B = C is checked on every frame. */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "cam.h"
#include "isp_uapi.h"

typedef struct vf_conv vf_conv_t;
vf_conv_t *vf_new(void);
bool vf_conv_init(vf_conv_t *c, int sw, int sh, int stride, cam_orient_t o, int dw, int dh);
void vf_convert(vf_conv_t *c, const uint8_t *y, const uint8_t *uv, uint32_t *dst, int dst_stride);

static double now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static int cur_mhz(void)
{
	FILE *f = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", "r");
	int k = 0;
	if (f) {
		if (fscanf(f, "%d", &k) != 1)
			k = 0;
		fclose(f);
	}
	return k / 1000;
}

typedef struct { double sum, sq, min, max; int n; } acc_t;
static void add(acc_t *a, double v)
{
	if (!a->n || v < a->min)
		a->min = v;
	if (!a->n || v > a->max)
		a->max = v;
	a->sum += v, a->sq += v * v, a->n++;
}
static void show(const char *what, const acc_t *a)
{
	if (!a->n)
		return;
	double m = a->sum / a->n;
	printf("%-52s n %4d  avg %7.2f  sd %6.2f  min %7.2f  max %7.2f\n", what, a->n, m,
	       sqrt(fmax(0, a->sq / a->n - m * m)), a->min, a->max);
}

enum { DW = 750, DH = 1000 };
static vf_conv_t *vc;
static cam_conv_t nc;
static uint32_t *dst, *dst2;
static acc_t m[3];
static int mismatch;

static bool setup(int w, int h, int stride, int cam)
{
	cam_orient_t o = cam_default_orient(w, h, cam);
	vc = vf_new();
	dst = malloc(sizeof(uint32_t) * DW * DH);
	dst2 = malloc(sizeof(uint32_t) * DW * DH);
	if (!vc || !dst || !dst2 || !vf_conv_init(vc, w, h, stride, o, DW, DH) ||
	    !cam_conv_init(&nc, w, h, stride, o, DW, DH)) {
		fprintf(stderr, "converter set-up failed for %dx%d\n", w, h);
		return false;
	}
	printf("camvf-bench: %dx%d stride %d → %dx%d, rot %d mirror %d; camfix gather plan: %s; cpu0 %d MHz\n", w, h,
	       stride, DW, DH, o.rot, o.mirror, nc.tbl ? "NEON tbl" : "scalar (doesn't fit)", cur_mhz());
	return true;
}

static void one(int k, const uint8_t *y, const uint8_t *uv)
{
	double t0 = now_ms();
	if (k == 0)
		vf_convert(vc, y, uv, dst, DW);
	else {
		cam_conv_scalar = k == 2;
		cam_convert(&nc, y, uv, k == 1 ? dst : dst2, DW);
		cam_conv_scalar = false;
	}
	add(&m[k], now_ms() - t0);
	if (k == 2)
		mismatch += memcmp(dst, dst2, sizeof(uint32_t) * DW * DH) != 0;	/* (B ran on the frame before) */
}

static void results(void)
{
	show("A camint converter (ms)", &m[0]);
	show("B camfix converter, NEON (ms)", &m[1]);
	show("C camfix converter, scalar (ms)", &m[2]);
	if (m[0].n && m[1].n)
		printf("  => B is %.2fx A\n", (m[1].sum / m[1].n) / (m[0].sum / m[0].n));
}

static int synth(int cam, int want)
{
	const int w = cam ? 1376 : 1632, h = cam ? 968 : 1224, s = cam ? 1408 : 1664;
	uint8_t *f = malloc((size_t)s * h * 3 / 2);
	srand(1);
	for (size_t i = 0; i < (size_t)s * h * 3 / 2; i++)
		f[i] = (uint8_t)(16 + rand() % 220);
	if (!f || !setup(w, h, s, cam))
		return 1;
	for (int i = 0; i < want * 3; i++) {
		int k = i % 3;
		if (k == 2)	/* C is compared with B on the same frame: B again first (not timed) */
			cam_convert(&nc, f, f + (size_t)s * h, dst, DW);
		one(k, f, f + (size_t)s * h);
	}
	results();
	printf("  B ≠ C on %d of %d frames\n", mismatch, m[2].n);
	return mismatch != 0;
}

int main(int argc, char **argv)
{
	int a = 1;
	bool syn = argc > a && !strcmp(argv[a], "synth");
	a += syn;
	int cam = argc > a && !strcmp(argv[a], "front");
	a += argc > a && (!strcmp(argv[a], "front") || !strcmp(argv[a], "rear"));
	int want = argc > a ? atoi(argv[a]) : 240;
	if (syn)
		return synth(cam, want);
	int fd = open("/dev/isp0", O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		perror("/dev/isp0 (Camera app closed? apple-isp stream=1?)");
		return 1;
	}
	__u32 st = (__u32)cam | ISP_START_PHOTO_VIEW;
	if (ioctl(fd, ISP_IOC_START, &st) < 0 && (st = (__u32)cam, ioctl(fd, ISP_IOC_START, &st) < 0)) {
		perror("START");
		return 1;
	}
	struct isp_info ii = { 0 };
	ioctl(fd, ISP_IOC_INFO, &ii);
	size_t len = (size_t)ii.nbufs * ii.bufsize;
	uint8_t *map = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	printf("sensor %s, %u buffers\n", ii.sensor_name, ii.nbufs);
	if (!setup((int)ii.width, (int)ii.height, (int)ii.stride, cam))
		return 1;
	acc_t iv = { 0 }, pick = { 0 }, mhz = { 0 }, rd = { 0 };
	int got = 0, gaps = 0;
	uint32_t lseq = 0;
	double lts = 0, t_start = now_ms();
	while (got < want && now_ms() - t_start < want * 200.0) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		if (poll(&p, 1, 1000) <= 0)
			continue;
		struct isp_frame fr;
		double t_r = now_ms();
		if (read(fd, &fr, sizeof(fr)) != (ssize_t)sizeof(fr) || fr.index >= ii.nbufs)
			continue;
		double t_read = now_ms(), ts = fr.timestamp_ns / 1e6;
		add(&rd, t_read - t_r);
		add(&pick, t_read - ts);
		if (got) {
			add(&iv, ts - lts);
			gaps += fr.seq - lseq - 1;
		}
		lts = ts, lseq = fr.seq;
		add(&mhz, cur_mhz());
		const uint8_t *y = map + (size_t)fr.index * ii.bufsize, *uv = y + (size_t)ii.stride * ii.height;
		int k = got % 3;
		if (k == 2)	/* C is compared with B on the same frame: B again first (not timed) */
			cam_convert(&nc, y, uv, dst, DW);
		one(k, y, uv);
		__u32 idx = fr.index;
		ioctl(fd, ISP_IOC_RELEASE, &idx);
		got++;
	}
	results();
	printf("  B ≠ C on %d of %d frames\n", mismatch, m[2].n);
	show("read() (incl. the cache invalidate with mmap_cached=1)", &rd);
	show("frame interval, driver timestamps (ms)", &iv);
	printf("  seq gaps (driver drops): %d\n", gaps);
	show("pickup: read() time - driver timestamp (ms)", &pick);
	show("cpu0 MHz at each frame", &mhz);
	ioctl(fd, ISP_IOC_STOP);
	close(fd);
	return 0;
}
