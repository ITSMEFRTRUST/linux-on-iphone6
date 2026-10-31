#!/bin/sh
# battab.sh - A/B power test on battery: governor x backlight, each 16 s, twice; restores everything.
B=/sys/class/power_supply/battery; C=/sys/class/power_supply/sn2400-charger
G=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor; L=$(ls -d /sys/class/backlight/* | head -1)
LIM=$(cat $C/input_current_limit); [ "$LIM" -ge 500000 ] || LIM=500000	# 90 mA = the chip dropped it after a resume, not a setting
SG=$(cat $G); SB=$(cat $L/brightness)
trap 'echo $LIM > $C/input_current_limit; echo $SG > $G; echo $SB > $L/brightness' EXIT INT TERM
echo 0 > $C/input_current_limit; sleep 3
meas() { i=0; s=0; while [ $i -lt 8 ]; do c=$(cat $B/current_now); s=$((s + c)); i=$((i + 1)); sleep 2; done; echo "$1: $((s / 8000)) mA"; }
for r in 1 2; do
  for cfg in ${CFGS:-performance:$SB schedutil:$SB powersave:$SB performance:0 schedutil:0 powersave:0}; do
    echo ${cfg%%:*} > $G; echo ${cfg##*:} > $L/brightness; sleep 3; meas "$cfg"
  done
done
