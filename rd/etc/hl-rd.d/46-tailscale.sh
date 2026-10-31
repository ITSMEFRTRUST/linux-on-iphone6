#!/bin/sh
# Join your tailnet at boot (tsup boot: tailscaled + "tailscale up" once Wi-Fi has a default route), unless Tailscale
# isn't installed on the flash (/data/tailscale) or the user switched it off (/data/tailscale/off). Backgrounded so it
# never delays Orchard; waits <= 20 s for /data (40-flash.sh mounts it in the background). Logs: /tmp/tailscale*.log
# Then "tsup watch" stays behind (one sleep every 30 s): if tailscaled ever dies without restoring /etc/resolv.conf,
# it puts the DHCP DNS servers back so names keep resolving.
[ -x /usr/sbin/tsup ] || exit 0
(
	for i in $(seq 1 40); do mountpoint -q /data && break; sleep 0.5; done
	[ -x /data/tailscale/tailscaled ] || exit 0
	tsup boot > /tmp/tailscale-boot.log 2>&1
	exec tsup watch > /dev/null 2>&1 < /dev/null
) &
