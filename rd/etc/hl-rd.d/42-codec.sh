#!/bin/sh
# CS42L67 headset codec (kernel branch n61-codec: mics + headphone jack, driver re/CODEC-DRIVER.md). With that DT the
# whole sound card waits for this module — the speaker too (hw:0,0 = speaker, hw:0,1 = headset codec) — so load it
# early, before Orchard / mpg123 / BlueZ want the card. Loaded in the background with a 5 s guard: a codec probe that
# hangs can never block the boot. Skip it: `nocodec` on the kernel command line, or create /data/orchard/no-codec
# (seen when /data mounts within ~3 s), or delete the module. Log: /tmp/codec.log
M=/lib/modules/snd-soc-cs42l67.ko
[ -f $M ] || exit 0
grep -qw nocodec /proc/cmdline && { echo "codec: skipped (nocodec on the command line)" > /tmp/codec.log; exit 0; }
(
	# /data is mounted by 40-flash.sh in the background: wait at most 3 s for it (only while flashdata still runs)
	# so the no-codec switch works; past that the module loads anyway (the speaker needs the card)
	if [ -x /usr/sbin/flashdata ] && grep -q '^UUID=.' /etc/flashdata.conf 2>/dev/null; then
		for i in $(seq 1 12); do
			mountpoint -q /data && break
			[ $i -gt 2 ] && ! grep -qsx flashdata /proc/[0-9]*/comm && break
			usleep 250000 2>/dev/null || sleep 0.25
		done
	fi
	[ -e /data/orchard/no-codec ] && { echo "codec: skipped (/data/orchard/no-codec)"; exit 0; }
	echo "codec: insmod $M"
	timeout 5 insmod $M
	echo "codec: insmod rc=$?"
	sleep 1
	dmesg | grep -iE 'cs42l67|mca|asoc|snd' | tail -20
	echo "--- /proc/asound/cards"
	cat /proc/asound/cards
) > /tmp/codec.log 2>&1 < /dev/null &
exit 0
