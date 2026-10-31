#!/bin/sh
# Display / key wedge capture, run ON THE PHONE (busybox sh). Read-only except `echo w > sysrq-trigger`
# (blocked-task list into dmesg) and an Orchard `state` command.
#   bin/ph "mkdir -p /tmp/disp"; bin/push disp/wedge.sh
#   bin/ph "sh /tmp/disp/wedge.sh snap" 60       # everything, into /tmp/wedge-HHMMSS/ (prints the dir)
#   bin/ph "sh /tmp/disp/wedge.sh keys 15" 40    # 15 s: every gpio-keys event + IRQ edges + pin samples,
#                                                 # while you press (and hold) Power / Home
# Pull: var/codec3-tools/pull3.sh /tmp/wedge-HHMMSS/<file> var/dispfix-<file>.txt (or tar it first).
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
D=$(ls -d /sys/kernel/debug/dri/*/power 2>/dev/null | head -1); D=${D%/power}
DS=$(ls -d /sys/class/drm/card*/device/display_state 2>/dev/null | head -1)
EV=$(awk '/^N:/{n=$0} /^H:/{if (n ~ /gpio-keys/) for (i = 1; i <= NF; i++) if ($i ~ /^event/) print $i}' /proc/bus/input/devices | head -1)
TI=$(grep -l Touchscreen /sys/class/input/input*/name 2>/dev/null | head -1); TI=${TI%/name}

irqs() { grep -E "Power Button|Home Button|apple-z2|adp-t7000" /proc/interrupts | awk '{n = $NF; if (n == "Button") n = $(NF - 1); printf "%s=%s ", n, $2 + $3} END {print ""}'; }

case "$1" in
snap)
	O=${2:-/tmp/wedge-$(date +%H%M%S)}; mkdir -p $O
	{ date; cat /proc/uptime /proc/loadavg; } > $O/time
	{ for f in power panel stall; do echo "== $f"; cat $D/$f; done
	  echo "== display_state"; cat $DS
	  echo "== params"; for p in deep_off deep_panel panel_sleep; do echo "$p=$(cat /sys/module/adp_t7000/parameters/$p 2>/dev/null)"; done
	  echo "== backlight"; cat /sys/class/backlight/*/brightness /sys/class/backlight/*/actual_brightness
	  echo "== touch inhibited"; cat $TI/inhibited; } > $O/adp 2>&1
	irqs > $O/irq; sleep 2; irqs >> $O/irq; cat /proc/interrupts > $O/interrupts
	cat /sys/kernel/debug/gpio > $O/gpio 2>&1
	grep -E "^name|gpio-keys|spi" /sys/kernel/debug/wakeup_sources > $O/wakeup 2>&1
	# Orchard: does its main loop still answer, and where does each thread sleep
	rm -f /tmp/orchard.state; [ -p /tmp/orchard.cmd ] && echo state > /tmp/orchard.cmd
	k=0; while [ ! -s /tmp/orchard.state ] && [ $k -lt 20 ]; do usleep 100000; k=$((k + 1)); done
	cat /tmp/orchard.state > $O/orchard-state 2>/dev/null || echo "NO ANSWER in 2 s: Orchard's main loop is stuck" > $O/orchard-state
	P=$(pidof orchard)
	for t in /proc/$P/task/*; do
		echo "== $(cat $t/comm) tid ${t##*/} state $(cut -d' ' -f3 $t/stat) wchan $(cat $t/wchan)"
		cat $t/stack 2>/dev/null
	done > $O/orchard-tasks 2>&1
	tail -80 /tmp/orchard.log > $O/orchard-log
	echo w > /proc/sysrq-trigger; usleep 500000
	dmesg | tail -300 > $O/dmesg
	top -bn1 | head -15 > $O/top 2>&1
	echo "$O"; cat $O/orchard-state; head -12 $O/adp; cat $O/irq
	;;
keys)
	S=${2:-15}; [ -n "$EV" ] || { echo "no gpio-keys event device"; exit 1; }
	echo "keys: /dev/input/$EV for $S s, IRQs before: $(irqs)"
	grep gpio-keys /sys/kernel/debug/wakeup_sources
	# raw input_event: 8 B sec, 8 B usec, 2 B type, 2 B code, 4 B value (aarch64, little endian).
	# cat writes every read at once; a hexdump killed by timeout would lose its stdio buffer (= no KEY lines)
	R=/tmp/wedge-keys.bin; rm -f $R
	timeout $S cat /dev/input/$EV > $R &
	HP=$!
	# the pins while held: Power = gpio-33, Home = gpio-32 ("lo" = pressed, active low)
	end=$(($(cut -d. -f1 /proc/uptime) + S)); pl=0; ph=0; hl=0; hh=0
	while [ $(cut -d. -f1 /proc/uptime) -lt $end ]; do
		# "in hi|lo" of gpio-32 (Home) then gpio-33 (Power), debugfs order
		set -- $(grep -E "gpio-3[23] " /sys/kernel/debug/gpio | awk '{for (i = 1; i <= NF; i++) if ($i == "in") printf "%s ", $(i + 1)}')
		[ "$2" = lo ] && pl=$((pl + 1)) || ph=$((ph + 1))
		[ "$1" = lo ] && hl=$((hl + 1)) || hh=$((hh + 1))
		usleep 20000
	done
	wait $HP
	hexdump -v -e '24/1 "%02x " "\n"' $R | awk '
		function h(s,  i, v) { v = 0; for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; return v }
		{ sec = h($4 $3 $2 $1); us = h($11 $10 $9); t = h($18 $17); c = h($20 $19); v = h($24 $23 $22 $21)
		  if (t == 1) printf "%d.%06d KEY %d %s\n", sec, us, c, v == 1 ? "down" : v == 0 ? "up" : "repeat"
		  else if (t == 0 && c == 3) printf "%d.%06d SYN_DROPPED\n", sec, us }'
	echo "($(($(wc -c < $R) / 24)) input events, $R)"
	echo "pin samples: Power lo $pl hi $ph, Home lo $hl hi $hh"
	echo "IRQs after: $(irqs)"
	grep gpio-keys /sys/kernel/debug/wakeup_sources
	grep "orchard: key" /tmp/orchard.log | tail -10
	;;
*) echo "usage: $0 snap [DIR] | keys [SECS]"; exit 2 ;;
esac
