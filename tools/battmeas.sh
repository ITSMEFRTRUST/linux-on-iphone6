#!/bin/sh
# battmeas.sh SECONDS - run the phone from the battery for SECONDS (charger input suspended), print gauge samples,
# always restore the charger input limit. Usage: battmeas.sh 20
B=/sys/class/power_supply/battery; C=/sys/class/power_supply/sn2400-charger
T=${1:-20}; LIM=$(cat $C/input_current_limit); [ "$LIM" -ge 500000 ] || LIM=500000	# 90 mA = the chip dropped it after a resume, not a setting
restore() {	# the chip sometimes reads back 90000 after the write: write again and verify
	i=0; while [ $i -lt 5 ]; do echo $LIM > $C/input_current_limit; sleep 1; [ "$(cat $C/input_current_limit)" -ge "$LIM" ] && return; i=$((i+1)); done
	echo "WARNING: input_current_limit still $(cat $C/input_current_limit), wanted $LIM"
}
trap restore EXIT; trap exit HUP INT TERM
echo 0 > $C/input_current_limit
sleep 4
i=0; sum=0
while [ $i -lt $T ]; do
  c=$(cat $B/current_now); v=$(cat $B/voltage_now); sum=$((sum + c)); i=$((i + 2))
  echo "t=$i I=$((c / 1000))mA V=$((v / 1000))mV $(cat $B/status)"
  sleep 2
done
echo "avg $((sum / (T / 2) / 1000)) mA"
