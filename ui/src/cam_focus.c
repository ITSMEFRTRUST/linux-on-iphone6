/* Tap to focus (cam.h): the UI posts the newest request, the camera thread sends it between two frames through
 * ISP_IOC_FOCUS (cam_src_focus). Rear camera: autofocus (continuous by default — the driver turns it on at
 * stream start — a one-shot scan on a tap, lock on a long press) and exposure metered on the same window. The front
 * camera's lens is fixed-focus: a tap only moves the exposure window there. A driver without the ioctl (ENOTTY)
 * switches this off for the rest of the run. No LVGL in here. */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "cam.h"
#include "isp_uapi.h"

#define WIN_PERMILLE 250	/* the window's side: a quarter of the frame's short side (iOS's tap box is ~ 1/4) */

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
	bool pending;
	enum cam_focus_mode m;
	int x, y;
} req;
static bool unsupported;	/* camera thread only */
static struct {		/* camera thread only: the last request sent (cam_focus_restarted holds it again) */
	bool have;
	int camera;
	enum cam_focus_mode m;
	int win[4];
} sent;
static bool replay;	/* camera thread only: hold the lens again at the next service */

void cam_focus_request(enum cam_focus_mode m, int x, int y)
{
	pthread_mutex_lock(&mu);
	req.pending = true;
	req.m = m;
	req.x = x;
	req.y = y;
	pthread_mutex_unlock(&mu);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void cam_focus_window(cam_orient_t o, int sw, int sh, int dw, int dh, int x, int y, int size, int win[4])
{
	int rot = ((o.rot % 360) + 360) % 360;
	bool tr = rot == 90 || rot == 270;
	int ow = tr ? sh : sw, oh = tr ? sw : sh;
	/* the visible window of the oriented picture: cam_conv_init's cover crop */
	int cw = ow, ch = oh;
	if ((int64_t)dw * oh >= (int64_t)dh * ow)
		ch = (int)((int64_t)ow * dh / dw);
	else
		cw = (int)((int64_t)oh * dw / dh);
	int px = clampi((ow - cw) / 2 + (int)((int64_t)clampi(x, 0, dw - 1) * cw / dw), 0, ow - 1);
	int py = clampi((oh - ch) / 2 + (int)((int64_t)clampi(y, 0, dh - 1) * ch / dh), 0, oh - 1);
	int sx, sy;
	cam_orient_src_xy(o, sw, sh, px, py, &sx, &sy);
	/* a square in the scene: size ‰ of the short side, in ‰ of each axis */
	int side = (int)((int64_t)size * (sw < sh ? sw : sh) / 1000);
	int w = clampi((int)((int64_t)side * 1000 / sw), 1, 1000), h = clampi((int)((int64_t)side * 1000 / sh), 1, 1000);
	int cx = (int)((int64_t)sx * 1000 / sw), cy = (int)((int64_t)sy * 1000 / sh);
	win[0] = clampi(cx - w / 2, 0, 1000 - w);
	win[1] = clampi(cy - h / 2, 0, 1000 - h);
	win[2] = w;
	win[3] = h;
}

void cam_focus_service(cam_src_t *s, int camera, const cam_info_t *in, cam_orient_t o, int dw, int dh)
{
	pthread_mutex_lock(&mu);
	bool go = req.pending;
	enum cam_focus_mode m = req.m;
	int x = req.x, y = req.y;
	req.pending = false;
	pthread_mutex_unlock(&mu);
	if (go)
		replay = false;	/* (a newer request wins) */
	if (replay) {	/* after a photo's re-START: the tapped / locked lens stays where it was */
		replay = false;
		if (!unsupported && s && sent.have && sent.camera == camera && camera == 0 &&
		    sent.m != CAM_FOCUS_CONTINUOUS) {
			int lens = -1, e = cam_src_focus(s, ISP_FOCUS_LOCK, sent.win, false, &lens);
			if (getenv("ORCHARD_CAMERA_STATS") || e)
				fprintf(stderr, "cam: focus held again after the photo: %s, lens %d\n", e ? "failed" : "ok",
					lens);
		}
	}
	if (!go || unsupported || !s || camera < 0 || in->w <= 0 || in->h <= 0)
		return;
	int win[4], lens = -1;
	cam_focus_window(o, in->w, in->h, dw, dh, x, y, WIN_PERMILLE, win);
	int mode = m == CAM_FOCUS_CONTINUOUS ? ISP_FOCUS_CONTINUOUS : ISP_FOCUS_ONCE;
	if (camera == 1)
		mode = ISP_FOCUS_QUERY;	/* fixed-focus front lens: exposure only */
	int e = cam_src_focus(s, mode, win, true, &lens);
	if (!e) {
		sent.have = true;
		sent.camera = camera;
		sent.m = m;
		for (int i = 0; i < 4; i++)
			sent.win[i] = win[i];
	}
	if (e == ENOTTY) {
		unsupported = true;
		fprintf(stderr, "cam: the camera driver has no focus control (ISP_IOC_FOCUS): tap to focus is off\n");
		return;
	}
	if (getenv("ORCHARD_CAMERA_STATS") || e)
		fprintf(stderr, "cam: focus %s at %d,%d → window %d,%d %dx%d ‰: %s, lens %d\n",
			m == CAM_FOCUS_CONTINUOUS ? "continuous" : m == CAM_FOCUS_LOCK ? "lock" : "point", x, y, win[0],
			win[1], win[2], win[3], e ? "failed" : "ok", lens);
}

void cam_focus_restarted(int camera)
{
	replay = sent.have && sent.camera == camera;
}
