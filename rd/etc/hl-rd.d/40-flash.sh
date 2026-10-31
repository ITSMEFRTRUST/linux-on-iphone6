#!/bin/sh
# Load the internal flash driver (ANS1/ASP): /dev/asp0n1 = the iOS APFS container.
# With /etc/flashdata.conf it loads writable inside the data-area extents only (flashdata load) and,
# once the conf has the ext4 UUID, mounts it on /data; otherwise read-only. asp can't be rmmod'ed,
# so this boot-time choice sticks. Backgrounded so a stuck probe can't hold up Orchard. Log: /tmp/asp.log
[ -f /lib/modules/asp.ko ] || exit 0
if [ -x /usr/sbin/flashdata ] && grep -q '^EXTENTS=.' /etc/flashdata.conf 2>/dev/null; then
	if grep -q '^UUID=.' /etc/flashdata.conf; then
		(flashdata mount /data > /tmp/asp.log 2>&1 &)
	else
		(flashdata load > /tmp/asp.log 2>&1 &)
	fi
else
	(insmod /lib/modules/asp.ko > /tmp/asp.log 2>&1 &)
fi
