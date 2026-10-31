#!/bin/sh
# Inbound firewall (/etc/firewall.nft, nft from tools/net-user.tgz) - loaded synchronously BEFORE Wi-Fi comes up
# (45-wifi.sh), so the LAN never sees the root telnet on 172.16.43.1:23 or any other listener. Takes milliseconds.
# Rollback on a running phone: nft flush ruleset. Log: /tmp/firewall.log
if [ -x /usr/sbin/nft ] && [ -f /etc/firewall.nft ]; then
	nft -f /etc/firewall.nft > /tmp/firewall.log 2>&1 && echo "firewall loaded" >> /tmp/firewall.log
else
	echo "no nft or /etc/firewall.nft: NO FIREWALL" > /tmp/firewall.log
fi
