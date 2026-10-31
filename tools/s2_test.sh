#!/bin/sh
# s2_test.sh MODE [SECS] - suspend-to-idle (s2idle) test runner for N61 (kernel n61-suspend). Run DETACHED,
# the shell that writes /sys/power/state blocks until resume:
#   setsid sh /tmp/s2_test.sh freezer > /tmp/s2_log 2>&1 < /dev/null &
# then read /tmp/s2_log (and dmesg) once the phone answers again. Markers also go to /dev/kmsg (netconsole).
# MODE:
#   info          state only (mem_sleep, params, wakeup sources), no suspend
#   freezer       pm_test=freezer: freeze tasks, 5 s, thaw. No device touched.
#   devices       pm_test=devices: + device suspend/resume (not noirq), 5 s.
#   platform      pm_test=platform: + noirq/late phases, 5 s, no s2idle loop.
#   real [SECS]   real s2idle; wake timer SECS (default 20) as safety net; Power/Home wake earlier.
#   meas SECS     battery-only (charger limit 0) real s2idle of SECS; charge_now before/after -> mA.
#   awake SECS    same accounting without suspending (baseline for meas, same screen/radio state).
# processors/core pm_test levels are rejected by the kernel for s2idle (-EAGAIN); not offered.
# Restores pm_test=none, wake timer 0 and the charger input limit on exit/HUP/INT/TERM.
P=/sys/power; W=/sys/module/s2idle_wake_timer/parameters; B=/sys/class/power_supply/battery
C=/sys/class/power_supply/sn2400-charger
MODE=$1; SECS=${2:-20}
mark() { echo "s2_test: $*"; echo "s2_test: $*" > /dev/kmsg; }
LIM=$(cat $C/input_current_limit 2>/dev/null); [ "${LIM:-0}" -ge 500000 ] || LIM=500000
final() {
	echo none > $P/pm_test 2>/dev/null; [ -d $W ] && echo 0 > $W/wake_after_ms
	i=0; while [ $i -lt 10 ]; do echo $LIM > $C/input_current_limit; sleep 2
		[ "$(cat $C/input_current_limit)" -ge "$LIM" ] && break; i=$((i+1)); done
	mark "restored pm_test=$(cat $P/pm_test | tr ' ' '\n' | grep '\[' ) lim=$(cat $C/input_current_limit)"
	echo S2_DONE
}
state() {
	echo "mem_sleep: $(cat $P/mem_sleep)  state: $(cat $P/state)  pm_test: $(cat $P/pm_test)"
	for f in $P/suspend_stats/success $P/suspend_stats/fail $P/suspend_stats/last_failed_dev \
		$P/suspend_stats/last_failed_step $P/pm_wakeup_irq $W/wake_after_ms $W/fired \
		/sys/module/pmgr_pwrstate/parameters/suspend_power_off /sys/module/dwc2/parameters/suspend_keep \
		/sys/module/brcmfmac/parameters/pcie_d3 /sys/module/apple_dart/parameters/resume_reset \
		/sys/module/printk/parameters/console_suspend; do
		[ -e $f ] && echo "$f = $(cat $f 2>/dev/null)"; done
	echo "wakeup sources (name active_count event_count):"
	for d in /sys/class/wakeup/*; do [ -d $d ] && echo "  $(cat $d/name) $(cat $d/active_count) $(cat $d/event_count)"; done
	echo "bat: $(($(cat $B/charge_now)/1000)) mAh $(($(cat $B/voltage_now)/1000)) mV $(($(cat $B/current_now)/1000)) mA"
}
precheck() {
	lsmod | grep -q '^apple_isp' && { mark "ABORT: apple_isp loaded (no suspend with the ISP)"; exit 1; }
	grep -q s2idle $P/mem_sleep || { mark "ABORT: no s2idle"; exit 1; }
	[ -d $W ] || { mark "ABORT: no s2idle_wake_timer (wrong kernel?)"; exit 1; }
	[ "$(cat /sys/module/pmgr_pwrstate/parameters/suspend_power_off)" = N ] ||
		{ mark "ABORT: pmgr_pwrstate.suspend_power_off is on: domains would lose power"; exit 1; }
	echo 1 > $P/pm_print_times; echo 1 > $P/pm_debug_messages
	echo N > /sys/module/printk/parameters/console_suspend	# = no_console_suspend
	echo 8 > /proc/sys/kernel/printk
}
suspend_once() {	# $1 = pm_test level
	echo $1 > $P/pm_test || { mark "pm_test $1 rejected"; return 1; }
	sync; mark "enter freeze (pm_test=$1) at $(cut -d' ' -f1 /proc/uptime)"
	t0=$(cut -d. -f1 /proc/uptime)
	echo freeze > $P/state; rc=$?
	t1=$(cut -d. -f1 /proc/uptime)
	mark "back from freeze rc=$rc after $((t1 - t0)) s, wakeup_irq=$(cat $P/pm_wakeup_irq 2>/dev/null) timer_fired=$(cat $W/fired)"
	echo none > $P/pm_test
	return $rc
}
charge() { sleep 7; cat $B/charge_now; }	# >5 s: bq27xxx cache refresh
trap 'final; exit' EXIT HUP INT TERM
case $MODE in
info) state; trap - EXIT; exit 0 ;;
freezer|devices|platform) precheck; state; suspend_once $MODE; state ;;
real) precheck; state; echo $((SECS * 1000)) > $W/wake_after_ms; suspend_once none; state ;;
meas|awake)
	precheck; echo 0 > $C/input_current_limit; sleep 4
	q0=$(charge); t0=$(cut -d. -f1 /proc/uptime); v0=$(cat $B/voltage_now)
	if [ $MODE = meas ]; then echo $((SECS * 1000)) > $W/wake_after_ms; suspend_once none
	else mark "awake for $SECS s"; sleep $SECS; fi
	q1=$(charge); t1=$(cut -d. -f1 /proc/uptime)
	dq=$(( (q0 - q1) / 1000 )); dt=$((t1 - t0))
	mark "RESULT $MODE ${SECS}s: dQ=${dq} mAh in ${dt} s = $(( dq * 3600 / dt )) mA avg (gauge 1 mAh steps; V0=$((v0/1000)) mV)"
	state ;;
*) sed -n '2,16p' $0; trap - EXIT; exit 1 ;;
esac
