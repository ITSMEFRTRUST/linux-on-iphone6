#!/bin/sh
# adp-t7000 display off/on stress test, run ON THE PHONE (busybox sh). Stops at the first failure.
#   bin/ph "mkdir -p /tmp/disp"; bin/push disp/stress.sh
#   bin/ph "sh /tmp/disp/stress.sh [N] [MODE] [FLIP]" 600
# N     cycles (default 50)
# MODE  power = debugfs `power` off/on (default; needs the CRTC on, i.e. Orchard has drawn once)
#       key   = Orchard power key (DPMS off/on through Orchard; set deep_off/panel_sleep first)
#       nop   = debugfs `power` nop: header-only param-FIFO packet (nothing visible changes)
#       same  = debugfs `power` same: LAYER_CTL = its current value through the FIFO (nothing visible changes)
#       human = Power presses with real-key timing (orchard.cmd keydown/keyup, hold 60-250 ms) at random phases:
#               inside the 1.2 s grace, while the DPMS off is in flight, double presses, presses during the
#               DPMS on, long dark gaps; ~1 in 5 wakes by Home. After every cycle: Orchard's belief
#               (/tmp/orchard.state) must match the kernel (display_state, touch inhibited, backlight).
#               Needs Orchard orchard-dispfix + adp-t7000 n61-dispfix; set deep_off (and deep_panel) first.
# FLIP  1 = keep Orchard flipping meanwhile: home-screen drags through /tmp/orchard.cmd every 0.4 s (default 1)
#       0 = no synthetic input (separates "races with flips" from "the off/on itself"); use 0 with MODE=key
# After every cycle: the frame counter (ADP 0x1034) must advance, the driver must not report a stall, and
# dmesg must not gain "ADP error" / "stalled" / "timed out" / "skipped" lines. Log: /tmp/disp-stress.log
N=${1:-50}; MODE=${2:-power}; FLIP=${3:-1}
LOG=/tmp/disp-stress.log
mountpoint -q /sys/kernel/debug || mount -t debugfs none /sys/kernel/debug
P=$(ls -d /sys/kernel/debug/dri/*/power 2>/dev/null | head -1)
[ -n "$P" ] || { echo "no adp-t7000 debugfs power file (old module?)"; exit 1; }
PAR=/sys/module/adp_t7000/parameters
CMD=/tmp/orchard.cmd
PAT='adp-t7000.*(ADP error|stalled|timed out|skipped|not sent|not applied|not idle|failed|FAILED|still not on|late layer|no packet-done|did not take)|vblank wait timed out|flip_done timed out|apple-z2.*(failed|not responding)'

msleep() { usleep $(($1 * 1000)) 2>/dev/null || sleep $(awk "BEGIN{print $1/1000}"); }
rnd() { echo $(($1 + RANDOM % ($2 - $1 + 1))); }	# rnd LO HI (busybox ash has $RANDOM)
DS=$(ls -d /sys/class/drm/card*/device/display_state 2>/dev/null | head -1)
TI=$(grep -l Touchscreen /sys/class/input/input*/name 2>/dev/null | head -1); TI=${TI%/name}/inhibited
# human: one press = keydown, hold, keyup (a real key's two events, apart)
press() { echo "keydown $1" > $CMD; msleep $(rnd 60 250); echo "keyup $1" > $CMD; echo "  $(cut -d' ' -f1 /proc/uptime) $1" >> $LOG; }
ostate() { rm -f /tmp/orchard.state; echo state > $CMD; k=0; while [ ! -s /tmp/orchard.state ] && [ $k -lt 20 ]; do msleep 50; k=$((k+1)); done; cat /tmp/orchard.state 2>/dev/null; }
# Orchard's belief vs the kernel, once settled (touch power-up takes ~0.55 s, an on that the kernel reported
# bad is retried for up to ~2.5 s with the backlight held off)
consistent() {
	for try in 1 2 3 4 5 6; do
		o=$(ostate); [ -n "$o" ] || fail "$1: no /tmp/orchard.state (old Orchard?)"
		set -- "$1" $o
		scr=$3 dark=$5 bad=$9 touch=${11} bl=${13}
		k=$(cat $DS 2>/dev/null); inh=$(cat $TI 2>/dev/null)
		msg="orchard [$o] kernel [$k] inhibited $inh"
		if [ "$scr" = on ]; then
			case "$k" in "ok crtc on scanout on panel "a*|"ok crtc on scanout on panel nodsi"*) ;; *) msg="screen on but $msg"; msleep 500; continue ;; esac
			[ "$bad" = 0 ] && [ "$bl" -gt 0 ] && [ "$touch" = on ] && [ "$inh" = 0 ] || { msg="screen on: $msg"; msleep 500; continue; }
			return 0
		fi
		[ "$dark" = 1 ] || { msleep 700; continue; }	# screen off, still in the grace
		case "$k" in "ok crtc off"*) ;; *) msg="dark but $msg"; msleep 500; continue ;; esac
		if [ "$(cat $PAR/deep_off)" = Y ]; then
			case "$k" in *"scanout off"*) ;; *) fail "$1: dark, deep_off, but $msg" ;; esac
			[ "$(cat $PAR/deep_panel 2>/dev/null)" = N ] || case "$k" in *"panel asleep"*|*nodsi*) ;; *) fail "$1: dark but $msg" ;; esac
		fi
		[ "$bl" = 0 ] && [ "$touch" = off ] && [ "$inh" = 1 ] || { msg="dark: $msg"; msleep 500; continue; }
		return 0
	done
	fail "$1: $msg"
}
gap() {	# the phases a human hits: double press, inside the grace, DPMS off in flight, dark
	case $(rnd 0 3) in 0) rnd 120 350 ;; 1) rnd 300 1100 ;; 2) rnd 1100 1450 ;; 3) rnd 1500 4000 ;; esac
}
frame() { sed -n 's/.* frame \([0-9a-f]*\) .*/\1/p' $P; }
errs() { dmesg | grep -cE "$PAT"; }
fail() {
	echo "FAIL at cycle $i: $*" | tee -a $LOG
	cat $P | tee -a $LOG
	[ -n "$DS" ] && cat $DS | tee -a $LOG
	dmesg | grep -E "adp-t7000|vblank|flip_done" | tail -15 | tee -a $LOG
	grep "orchard:" /tmp/orchard.log | tail -12 | tee -a $LOG
	[ -n "$FP" ] && kill $FP 2>/dev/null
	exit 1
}
# the pipe runs: frame counter moves within 100 ms and no stall flag
alive() {
	a=$(frame); msleep 100; b=$(frame)
	[ -n "$a" ] && [ "$a" != "$b" ] || fail "$1: frame counter frozen at $a"
	grep -q STALLED $P && fail "$1: driver reports a stalled pipe"
	[ "$(errs)" = "$E0" ] || fail "$1: new error lines in dmesg"
}

: > $LOG
echo "stress: N=$N MODE=$MODE FLIP=$FLIP deep_off=$(cat $PAR/deep_off) panel_sleep=$(cat $PAR/panel_sleep) $(date)" | tee -a $LOG
cat $P | tee -a $LOG
E0=$(errs)
i=0; alive start

FP=
if [ "$MODE" = human ]; then
	[ -p $CMD ] || { echo "no $CMD (Orchard not running)"; exit 1; }
	[ -n "$DS" ] || { echo "no display_state (needs adp-t7000 from n61-dispfix)"; exit 1; }
	[ -n "$(ostate)" ] || { echo "Orchard has no state command (needs orchard-dispfix)"; exit 1; }
	FLIP=0
fi
if [ "$FLIP" = 1 ] && [ -p $CMD ]; then
	( while :; do echo 'drag 600 700 150 700 300' > $CMD; msleep 400; echo 'drag 150 700 600 700 300' > $CMD; msleep 400; done ) &
	FP=$!
fi
trap '[ -n "$FP" ] && kill $FP 2>/dev/null' EXIT INT TERM HUP

T0=$(date +%s)
i=1
while [ $i -le $N ]; do
	case $MODE in
	power)
		echo off > $P || fail "echo off: error $?"
		grep -q "^scanout off" $P || fail "off: scanout still on"
		msleep 100
		echo on > $P || fail "echo on: error $?"
		grep -q "^scanout on, panel awake" $P || fail "on: not back on"
		;;
	key)
		echo 'key power' > $CMD; msleep 1500
		echo 'key home' > $CMD; msleep 1500
		;;
	human)
		echo "cycle $i" >> $LOG
		for n in $(seq $(rnd 1 4)); do press power; msleep $(gap); done
		msleep 2000; consistent "human after presses"
		o=$(ostate)
		case "$o" in "screen off"*)	# wake it for the next cycle (Home now and then: its own path)
			if [ $(rnd 0 4) = 0 ]; then press home; else press power; fi
			msleep 2000; consistent "human wake" ;;
		esac
		;;
	nop|same)
		echo $MODE > $P || fail "echo $MODE: error $?"
		grep -q "^last packet [0-9a-f]*: 0 " $P || fail "$MODE: packet not reported done"
		;;
	*) echo "unknown MODE $MODE"; exit 2 ;;
	esac
	alive "after on"
	[ $((i % 10)) = 0 ] && echo "cycle $i ok, frame $(frame)" | tee -a $LOG
	i=$((i + 1))
done
echo "PASS: $N cycles in $(( $(date +%s) - T0 )) s" | tee -a $LOG
cat $P | tee -a $LOG
