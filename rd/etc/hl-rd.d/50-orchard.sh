#!/bin/sh
# Start the Orchard phone shell once the display and touchscreen are up.
# Runs from /init after debug_shell (telnet stays available). Log: /tmp/orchard.log
for i in $(seq 1 60); do
	ls /dev/dri/card* > /dev/null 2>&1 && grep -q Touchscreen /proc/bus/input/devices && break
	sleep 0.5
done
sleep 1
# persistent data on the flash data area (40-flash.sh mounts it in the background): Orchard's data dir
# and BlueZ pairings live there; without it everything stays in RAM as before
for i in $(seq 1 20); do mountpoint -q /data && break; sleep 0.5; done
if mountpoint -q /data; then
	mkdir -p /data/orchard /data/bluetooth /var/lib
	[ -L /var/lib/bluetooth ] || { rm -rf /var/lib/bluetooth; ln -s /data/bluetooth /var/lib/bluetooth; }
	export ORCHARD_DATA=/data/orchard
fi
export TZ=${TZ:-$(cat /data/orchard/TZ 2>/dev/null || echo UTC0)} HOME=/root
mkdir -p /root
cd / && setsid /usr/bin/orchard > /tmp/orchard.log 2>&1 < /dev/null &
