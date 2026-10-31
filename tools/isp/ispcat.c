// SPDX-License-Identifier: GPL-2.0-only
/*
 * ispcat: /dev/isp0 stream test (apple-isp.ko stream=1).
 *   ispcat [-c camera] [-n frames] [-t timeout_s] [-o file] [-k hold] [-p pause_s]
 * Starts camera (0 rear, 1 front), receives N frames, prints fps, writes the last frame
 * to file (default /tmp/frame.nv12; NV12 stride x height*3/2, see the INFO line).
 * -k hold: keep up to `hold` buffers before releasing (tests the recycling).
 * -p pause: after START (and after the frames), wait pause_s before STOP / close, so a
 *    hang can be pinned to START, streaming, STOP or the close-time power-off.
 * -n 0: START + INFO only, no read().
 * Every wait is bounded (poll timeout), so a silent fw cannot hang it.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "apple-isp-t7000.h"

static int kmsg = -1;

/* progress line on stdout and in the kernel log (console), so the last step before a hang is visible */
static void say(const char *what)
{
	char b[160];
	int n = snprintf(b, sizeof(b), "ispcat: %s\n", what);

	fputs(b, stdout);
	if (kmsg >= 0 && n > 0)
		(void)!write(kmsg, b, n);
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int save(const char *path, const uint8_t *p, size_t len)
{
	FILE *f = fopen(path, "wb");

	if (!f || fwrite(p, 1, len, f) != len) {
		perror(path);
		if (f)
			fclose(f);
		return -1;
	}
	fclose(f);
	return 0;
}

int main(int argc, char **argv)
{
	const char *out = "/tmp/frame.nv12";
	unsigned int cam = 0, want = 30, tmo = 10, hold = 0, got = 0, i, pause = 0;
	uint32_t held[8];
	unsigned int nheld = 0;
	struct isp_info info;
	struct isp_frame fr;
	struct pollfd pfd;
	uint8_t *map;
	double t0, t1 = 0;
	int fd, opt, ret = 1, ymean = 0;

	setvbuf(stdout, NULL, _IONBF, 0);	/* every line out before a possible hang */
	while ((opt = getopt(argc, argv, "c:n:t:o:k:p:a")) != -1) {
		switch (opt) {
		case 'c': cam = strtoul(optarg, NULL, 0); break;	/* camera | ISP_START_* flags */
		case 'a': ymean = 1; break;
		case 'n': want = atoi(optarg); break;
		case 't': tmo = atoi(optarg); break;
		case 'o': out = optarg; break;
		case 'k': hold = atoi(optarg); break;
		case 'p': pause = atoi(optarg); break;
		default:
			fprintf(stderr, "usage: %s [-c cam|flags] [-a (mean Y of every frame)] [-n frames] [-t timeout_s] [-o file] [-k hold] [-p pause_s]\n",
				argv[0]);
			return 2;
		}
	}
	kmsg = open("/dev/kmsg", O_WRONLY);
	say("open /dev/isp0");
	fd = open("/dev/isp0", O_RDWR);		/* boots the ISP: 1-2 s */
	if (fd < 0) {
		perror("/dev/isp0");
		return 1;
	}
	say((cam & 0xff) ? "START camera 1" : "START camera 0");
	if (ioctl(fd, ISP_IOC_START, &cam)) {
		perror("ISP_IOC_START (dmesg has the failing command)");
		goto out_close;
	}
	if (ioctl(fd, ISP_IOC_INFO, &info) || !info.nbufs) {
		perror("ISP_IOC_INFO");
		goto out_stop;
	}
	printf("INFO v%u: %ux%u stride %u, %u buffers of %u bytes, sensor 0x%x '%s'\n",
	       info.version, info.width, info.height, info.stride, info.nbufs, info.bufsize,
	       info.sensor_id, info.sensor_name);
	if (hold >= info.nbufs)
		hold = info.nbufs - 1;
	map = mmap(NULL, (size_t)info.nbufs * info.bufsize, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		perror("mmap");
		goto out_stop;
	}
	if (pause) {
		say("START ok, pausing");
		sleep(pause);
	}
	pfd.fd = fd;
	pfd.events = POLLIN;
	t0 = now();
	while (got < want) {
		int r = poll(&pfd, 1, tmo * 1000);

		if (r == 0) {
			fprintf(stderr, "no frame for %u s after %u frame(s)\n", tmo, got);
			break;
		}
		if (r < 0 || (pfd.revents & (POLLERR | POLLHUP))) {
			fprintf(stderr, "poll: %s (revents 0x%x)\n", r < 0 ? strerror(errno) : "stream stopped",
				pfd.revents);
			break;
		}
		if (read(fd, &fr, sizeof(fr)) != sizeof(fr)) {
			perror("read");
			break;
		}
		if (!got) {
			t1 = now();
			say("first frame");
		}
		got++;
		if (got <= 3 || got == want)
			printf("frame %u: seq %u buffer %u %u bytes flags 0x%x ts %llu\n", got, fr.seq,
			       fr.index, fr.bytesused, fr.flags, (unsigned long long)fr.timestamp_ns);
		if (ymean && fr.index < info.nbufs) {	/* AE convergence: centre half of the picture */
			const uint8_t *y = map + (size_t)fr.index * info.bufsize;
			unsigned long long sum = 0, cnt = 0;
			for (unsigned int r = info.height / 4; r < info.height * 3 / 4; r += 4)
				for (unsigned int c = info.width / 4; c < info.width * 3 / 4; c += 4, cnt++)
					sum += y[(size_t)r * info.stride + c];
			printf("frame %u: +%.0f ms mean Y %.1f\n", got, (now() - t1) * 1e3, cnt ? (double)sum / cnt : 0.0);
		}
		if (got == want && fr.index < info.nbufs)
			save(out, map + (size_t)fr.index * info.bufsize, fr.bytesused);
		held[nheld++] = fr.index;
		while (nheld > hold) {
			ioctl(fd, ISP_IOC_RELEASE, &held[0]);
			memmove(held, held + 1, --nheld * sizeof(held[0]));
		}
	}
	for (i = 0; i < nheld; i++)
		ioctl(fd, ISP_IOC_RELEASE, &held[i]);
	if (got > 1)
		printf("%u frame(s), first after %.2f s, %.2f fps\n", got, t1 - t0,
		       (got - 1) / (now() - t1));
	if (want && got == want)
		printf("wrote %s (%ux%u NV12, stride %u)\n", out, info.width, info.height, info.stride);
	ret = got == want ? 0 : 1;
	if (pause && want) {
		say("frames done, pausing before STOP");
		sleep(pause);
	}
	munmap(map, (size_t)info.nbufs * info.bufsize);
out_stop:
	say("STOP");
	ioctl(fd, ISP_IOC_STOP);
	if (pause) {
		say("STOP ok, pausing before close");
		sleep(pause);
	}
out_close:
	say("close (power-off unless keep_on=1)");
	close(fd);
	say("closed");			/* last close powers sensor + ISP down (keep_on=0) */
	return ret;
}
