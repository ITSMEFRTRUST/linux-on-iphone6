#!/bin/sh
# wk_ab.sh SECS - wakeup A/B: window with charger limit normal, then limit 0 (battery), then normal again. Restores on exit/HUP.
#   setsid sh /tmp/wk_ab.sh 10 > /tmp/wk_log 2>&1 < /dev/null &
C=/sys/class/power_supply/sn2400-charger; T=${1:-10}
LIM=$(cat $C/input_current_limit); [ "$LIM" -ge 500000 ] || LIM=500000
final() { i=0; while [ $i -lt 10 ]; do echo $LIM > $C/input_current_limit; sleep 2; [ "$(cat $C/input_current_limit)" -ge "$LIM" ] && break; i=$((i+1)); done; echo "restored lim=$(cat $C/input_current_limit)"; echo WK_DONE; }
trap 'final; exit' EXIT HUP INT TERM
sh /tmp/wk_win.sh $T normal
echo 0 > $C/input_current_limit; sleep 6
sh /tmp/wk_win.sh $T battery
sh /tmp/wk_win.sh $T battery2
echo $LIM > $C/input_current_limit; sleep 2; [ "$(cat $C/input_current_limit)" -ge "$LIM" ] || echo $LIM > $C/input_current_limit; sleep 6
sh /tmp/wk_win.sh $T normal-after
