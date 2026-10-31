#!/bin/sh
# Camera (A8 ISP, kernel driver apple-isp, branch n61-isp; RE notes re/CAMERA-WEDGE.md): load it so the Orchard Camera app
# finds /dev/isp0. stream=1 + keep_on=0 = the ISP is powered off while /dev/isp0 is closed and boots again on open, so an
# idle camera costs no battery. Loaded in the background with a 10 s guard: an ISP boot that hangs can never block the boot.
# The firmware (lib/firmware/apple/isp-t7000.bin) and Apple's tuning setfiles are local blobs from var/extra-rd.
# Skip it: `noisp` on the kernel command line, or create /data/orchard/no-isp (seen once /data is mounted). Log: /tmp/isp.log
M=/lib/modules/apple-isp.ko
[ -f $M ] || exit 0
grep -qw noisp /proc/cmdline && { echo "isp: skipped (noisp on the command line)" > /tmp/isp.log; exit 0; }
(
	# after the codec hook: the sound card comes first, the camera is only needed once someone opens it
	sleep 2
	[ -e /data/orchard/no-isp ] && { echo "isp: skipped (/data/orchard/no-isp)"; exit 0; }
	echo "isp: insmod $M"
	timeout 10 insmod $M stage=9 stream=1 irq=1 keep_on=0
	echo "isp: insmod rc=$?"
	dmesg | grep -i 'apple-isp' | tail -5
	ls -l /dev/isp0
) > /tmp/isp.log 2>&1 < /dev/null &
exit 0
