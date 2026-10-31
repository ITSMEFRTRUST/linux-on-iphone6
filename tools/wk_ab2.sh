#!/bin/sh
# wk_ab2.sh SECS - does the measuring harness cause the wakeups? idle vs "read battery/current_now every 2 s"
# (what pa_meas/battmeas do), with the charger limit normal and 0. Restores the limit on exit/HUP.
#   setsid sh /tmp/wk_ab2.sh 10 > /tmp/wk_log2 2>&1 < /dev/null &
C=/sys/class/power_supply/sn2400-charger; B=/sys/class/power_supply/battery; T=${1:-10}
LIM=$(cat $C/input_current_limit); [ "$LIM" -ge 500000 ] || LIM=500000
RP=
reader() { ( while :; do cat $B/current_now > /dev/null; sleep 2; done ) & RP=$!; }
stopr() { [ -n "$RP" ] && kill $RP 2>/dev/null; RP=; }
final() { stopr; i=0; while [ $i -lt 10 ]; do echo $LIM > $C/input_current_limit; sleep 2; [ "$(cat $C/input_current_limit)" -ge "$LIM" ] && break; i=$((i+1)); done; echo "restored lim=$(cat $C/input_current_limit)"; echo WK_DONE; }
trap 'final; exit' EXIT HUP INT TERM
sh /tmp/wk_win.sh $T normal-idle
reader; sh /tmp/wk_win.sh $T normal-reader; stopr; sleep 6
echo 0 > $C/input_current_limit; sleep 10
sh /tmp/wk_win.sh $T batt-idle
reader; sh /tmp/wk_win.sh $T batt-reader; stopr; sleep 2
sh /tmp/wk_win.sh $T batt-idle2
