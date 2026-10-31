/* Orchard's copy of the A8 ISP streaming interface, v1 (BRIEF-CAMERA-STREAM.md; the kernel's own copy is
 * include/uapi/linux/apple-isp-t7000.h in the n61-isp branch — keep the two identical).
 * Misc device /dev/isp0 (root, 0600), present with apple-isp.ko stream=1 after stage 9.
 * Frames: NV12, Y plane stride × height (stride = width rounded up to 64), interleaved CbCr plane right after it
 * (same stride, height / 2 rows); bufsize page-aligned; nbufs buffers mmap'd read-only in one mapping.
 * read() blocks (poll POLLIN) until a frame and returns struct isp_frame; ISP_IOC_RELEASE gives buffer `index`
 * back (never hold more than nbufs - 1). Closing the fd = STOP; the last close powers the sensor down. */
#pragma once
#include <linux/ioctl.h>
#include <linux/types.h>

#define ISP_UAPI_VERSION 1

struct isp_info {
	__u32 version;		/* ISP_UAPI_VERSION */
	__u32 width, height;	/* pixels */
	__u32 stride;		/* bytes per Y (and CbCr) row */
	__u32 nbufs;
	__u32 bufsize;		/* bytes per buffer in the mapping (page-aligned) */
	__u32 sensor_id;
	char sensor_name[32];
};

#define ISP_FRAME_FLAG_NOT_FILLED 0x1	/* fw didn't mark plane 0 filled (the driver drops those: never seen here) */

struct isp_frame {
	__u32 index;		/* buffer: offset index * bufsize in the mapping */
	__u32 seq;
	__u64 timestamp_ns;
	__u32 bytesused;
	__u32 flags;		/* ISP_FRAME_FLAG_* */
};

#define ISP_IOC_INFO _IOR('I', 1, struct isp_info)
#define ISP_IOC_START _IOW('I', 2, __u32)	/* *arg: camera (bits 7:0: 0 = rear, 1 = front) | one ISP_START_* flag */
/* preset choice (drivers before them answer EINVAL): the viewfinder for photos (4:3, rear 1632x1224, front
 * 1376x968), the sensor's full resolution (rear 3264x2448, front 1376x968), or exactly preset index
 * (arg >> ISP_START_PRESET_SHIFT) & 0xff */
#define ISP_START_CAMERA_MASK 0xff
#define ISP_START_PHOTO_VIEW 0x100
#define ISP_START_STILL 0x200
#define ISP_START_PRESET 0x400
#define ISP_START_PRESET_SHIFT 16
#define ISP_IOC_STOP _IO('I', 3)
#define ISP_IOC_RELEASE _IOW('I', 4, __u32)	/* *arg: the frame's index */

/* minor 1 (INFO's version stays 1; an older driver answers ENOTTY): rear-camera autofocus. Window in per-mille of the
 * frame as delivered (sensor orientation, not mirrored); w or h 0 = keep. EAGAIN = no camera streaming. */
struct isp_focus {
	__u32 mode;		/* ISP_FOCUS_* */
	__u32 flags;		/* ISP_FOCUS_F_* */
	__u16 x, y, w, h;	/* window, per-mille */
	__s32 position;		/* in: lens position for MANUAL (0..255); out: lens position after the call, -1 unknown */
	__u32 fw_status;	/* out: first non-zero fw status, 0 = all ok */
	__u32 reserved[2];
};
#define ISP_FOCUS_CONTINUOUS 0	/* window + continuous AF (the driver's default at stream start) */
#define ISP_FOCUS_ONCE 1	/* window + one AF scan, then hold (tap to focus) */
#define ISP_FOCUS_LOCK 2	/* continuous AF off: the lens stays */
#define ISP_FOCUS_MANUAL 3	/* lens to .position */
#define ISP_FOCUS_QUERY 4	/* nothing sent (except F_AE): read the lens position */
#define ISP_FOCUS_F_AE 0x1	/* also meter exposure on the window */
#define ISP_IOC_FOCUS _IOWR('I', 0x20, struct isp_focus)
