#!/bin/sh
# wk_meas.sh SECS CFG... - battery-only A/B (charger input limit 0, like pa_meas.sh) that also logs the timer IRQ rate.
# CFG: base | rd:S (extra loop reading battery/voltage_now, sleep S between reads; S=0 back to back)
# Samples battery/current_now every 2 s (the sampler itself costs ~230 timer IRQ/s, same in every CFG).
#   setsid sh /tmp/wk_meas.sh 40 base rd:0 base rd:0 base > /tmp/wk_mlog 2>&1 < /dev/null &
B=/sys/class/power_supply/battery; C=/sys/class/power_supply/sn2400-charger; T=$1; shift
LIM=$(cat $C/input_current_limit); [ "$LIM" -ge 500000 ] || LIM=500000
RP=
stopr() { [ -n "$RP" ] && kill $RP 2>/dev/null; RP=; }
final() { stopr; i=0; while [ $i -lt 10 ]; do echo $LIM > $C/input_current_limit; sleep 2; [ "$(cat $C/input_current_limit)" -ge "$LIM" ] && break; i=$((i+1)); done; echo "restored lim=$(cat $C/input_current_limit)"; echo WK_DONE; }
trap 'final; exit' EXIT HUP INT TERM
tm() { awk '/arch_timer/{print $2+$3}' /proc/interrupts; }
echo 0 > $C/input_current_limit; sleep 4
for cfg in "$@"; do
	case $cfg in rd:*) S=${cfg#rd:}; ( while :; do cat $B/voltage_now > /dev/null; [ "$S" = 0 ] || sleep $S; done ) & RP=$! ;; esac
	echo "== $cfg"; n=0; s=0; i=0; all=
	while [ $i -lt $((T + 6)) ]; do
		[ $i -eq 6 ] && { a=$(tm); u0=$(cut -d' ' -f1 /proc/uptime); }
		c=$(($(cat $B/current_now) / 1000)); all="$all $c"
		[ $i -ge 6 ] && { s=$((s + c)); n=$((n + 1)); }
		sleep 2; i=$((i + 2))
	done
	b=$(tm); u1=$(cut -d' ' -f1 /proc/uptime)
	echo "samples:$all"
	echo "RESULT $cfg avg=$((s / n)) mA n=$n timer/s=$(awk "BEGIN{printf \"%d\", ($b-$a)/($u1-$u0)}")"
	stopr; sleep 2
done
