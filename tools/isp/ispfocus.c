/* ispfocus: one ISP_IOC_FOCUS on /dev/isp0 while another process (ispcat, Orchard) streams.
 *   ispfocus c|once|lock|query [x y w h] [ae]     window in per-mille of the frame (sensor orientation)
 *   ispfocus manual POS                           lens to POS (0..255)
 *   ispfocus watch N                              N x query, 200 ms apart (lens position over time)
 * Build: like ispcat (tools/isp/build.sh), -iquote <kernel>/include/uapi/linux. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "apple-isp-t7000.h"

static int focus(int fd, struct isp_focus *f)
{
	if (ioctl(fd, ISP_IOC_FOCUS, f) < 0) {
		printf("ISP_IOC_FOCUS mode %u: %s\n", f->mode, strerror(errno));
		return 1;
	}
	printf("mode %u window %u,%u %ux%u%s -> lens %d, fw status 0x%04x\n", f->mode, f->x, f->y, f->w, f->h,
	       f->flags & ISP_FOCUS_F_AE ? " +AE" : "", f->position, f->fw_status);
	return 0;
}

int main(int argc, char **argv)
{
	struct isp_focus f = { 0 };
	const char *m = argc > 1 ? argv[1] : "query";
	int fd = open("/dev/isp0", O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("/dev/isp0");
		return 1;
	}
	if (!strcmp(m, "watch")) {
		int n = argc > 2 ? atoi(argv[2]) : 10, r = 0;
		f.mode = ISP_FOCUS_QUERY;
		for (int i = 0; i < n && !r; i++, usleep(200000))
			r = focus(fd, &f);
		return r;
	}
	f.mode = !strcmp(m, "c") ? ISP_FOCUS_CONTINUOUS : !strcmp(m, "once") ? ISP_FOCUS_ONCE :
		 !strcmp(m, "lock") ? ISP_FOCUS_LOCK : !strcmp(m, "manual") ? ISP_FOCUS_MANUAL : ISP_FOCUS_QUERY;
	if (f.mode == ISP_FOCUS_MANUAL && argc > 2)
		f.position = atoi(argv[2]);
	if (f.mode != ISP_FOCUS_MANUAL && argc > 5) {
		f.x = atoi(argv[2]), f.y = atoi(argv[3]), f.w = atoi(argv[4]), f.h = atoi(argv[5]);
		if (argc > 6 && !strcmp(argv[6], "ae"))
			f.flags |= ISP_FOCUS_F_AE;
	}
	return focus(fd, &f);
}
