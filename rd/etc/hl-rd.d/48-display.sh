#!/bin/sh
# This project's display-pipe driver (adp-t7000, kernel branch n61-disp, standalone mode = no DT node needed):
# replaces simpledrm, gives Orchard tear-free page flips. If it refuses to load (unexpected iBoot state)
# simpledrm stays and Orchard runs as before. Skip it: `noadp` on the kernel command line, or create
# /data/orchard/no-adp, or delete the module. Log: /tmp/adp.log
[ -f /lib/modules/adp-t7000.ko ] || exit 0
grep -qw noadp /proc/cmdline && { echo "adp: skipped (noadp on the command line)" > /tmp/adp.log; exit 0; }
# /data is mounted by 40-flash.sh in the background: when it is going to be (flashdata configured with a UUID),
# wait for it — at most 10 s, and only while flashdata is still running — so the no-adp switch is seen this boot
if [ -x /usr/sbin/flashdata ] && grep -q '^EXTENTS=.' /etc/flashdata.conf 2>/dev/null &&
   grep -q '^UUID=.' /etc/flashdata.conf; then
	for i in $(seq 1 40); do
		mountpoint -q /data && break
		[ $i -gt 2 ] && ! grep -qsx flashdata /proc/[0-9]*/comm && break	# gave up without mounting
		usleep 250000 2>/dev/null || sleep 0.25
	done
fi
[ -e /data/orchard/no-adp ] && { echo "adp: skipped (/data/orchard/no-adp)" > /tmp/adp.log; exit 0; }
insmod /lib/modules/adp-t7000.ko standalone=1 > /tmp/adp.log 2>&1
dmesg | grep adp-t7000 >> /tmp/adp.log
