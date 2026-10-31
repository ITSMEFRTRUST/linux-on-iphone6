#!/bin/sh
# wk_trace.sh SECS [reader] - timer/wakeup trace of an idle window (optionally with a "cat battery/current_now every 2 s"
# reader). Output: /tmp/wk_trace.txt. Leaves tracing off, events disabled, buffer back to its old size.
T=${1:-10}; R=$2; TR=/sys/kernel/tracing
mount | grep -q tracefs || mount -t tracefs nodev $TR
OB=$(cat $TR/buffer_size_kb | cut -d' ' -f1)
off() { echo 0 > $TR/tracing_on; echo 0 > $TR/events/enable; echo $OB > $TR/buffer_size_kb; }
trap 'off; exit' EXIT HUP INT TERM
echo 0 > $TR/tracing_on; echo 0 > $TR/events/enable; echo 4096 > $TR/buffer_size_kb; echo > $TR/trace
for e in timer/hrtimer_expire_entry timer/timer_expire_entry timer/hrtimer_start sched/sched_wakeup sched/sched_waking irq/irq_handler_entry irq/softirq_entry ipi/ipi_raise; do
	[ -d $TR/events/$e ] && echo 1 > $TR/events/$e/enable; done
RP=
[ -n "$R" ] && { ( while :; do cat /sys/class/power_supply/battery/current_now > /dev/null; sleep 2; done ) & RP=$!; }
echo 1 > $TR/tracing_on; sleep $T; echo 0 > $TR/tracing_on
[ -n "$RP" ] && kill $RP
cat $TR/trace > /tmp/wk_trace.txt; wc -l /tmp/wk_trace.txt; cat $TR/per_cpu/cpu*/stats | grep -E 'overrun|entries'
