#!/bin/sh
# Network time: the PMIC RTC gives the clock at boot to the whole second; busybox ntpd (client only, no -l: nothing
# listens) trims it and keeps it from drifting once Wi-Fi has a default route (waits <= 120 s, then starts anyway:
# ntpd keeps retrying its peers). Backgrounded; its chatter goes nowhere (check by hand: ntpd -d -n -q -w -p pool.ntp.org)
(
	for i in $(seq 1 240); do ip route | grep -q '^default' && break; sleep 0.5; done
	exec ntpd -n -p 0.pool.ntp.org -p 1.pool.ntp.org -p time.cloudflare.com > /dev/null 2>&1 < /dev/null
) &
