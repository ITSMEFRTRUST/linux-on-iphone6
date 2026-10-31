#!/bin/sh
# Bring Wi-Fi up at boot (modules + wpa_supplicant + DHCP, unless the user switched it off: /data/wifi/off).
# Everything runs in the background so it never delays Orchard (50-orchard.sh). Waits <= 10 s for the persistent
# /data (40-flash.sh mounts it in the background: asp first, then the PCIe/DART modules, and the saved networks +
# off flag live there). Logs: /tmp/wifi-boot.log, /tmp/wifi.log
[ -x /usr/sbin/wifiup ] || exit 0
(
	for i in $(seq 1 20); do mountpoint -q /data && break; sleep 0.5; done
	exec wifiup boot > /tmp/wifi-boot.log 2>&1
) &
