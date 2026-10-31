#!/bin/sh
# pa_meas.sh SECS CFG... - power audit A/B runner (battery-only via charger input limit 0). Run detached:
#   setsid sh /tmp/pa_meas.sh 40 base bl:2047 base > /tmp/pa_log 2>&1 < /dev/null &
# Each CFG: apply, settle 6 s, sample every 2 s for SECS, restore. Restores everything on exit/HUP/INT/TERM.
# CFG: base | bl:N (backlight) | psoff (wifi power_save off) | gov:G | orchstop | tsoff | wifioff
B=/sys/class/power_supply/battery; C=/sys/class/power_supply/sn2400-charger
L=$(ls -d /sys/class/backlight/* | head -1); G=/sys/devices/system/cpu/cpufreq/policy0/scaling_governor
T=$1; shift
LIM=$(cat $C/input_current_limit); [ "$LIM" -ge 500000 ] || LIM=500000
SB=$(cat $L/brightness); SG=$(cat $G); SPS=$(iw dev wlan0 get power_save 2>/dev/null | awk '{print $3}')
OP=$(pidof orchard)
restore_cfg() {
	[ -n "$BP" ] && kill $BP 2>/dev/null; BP=
	echo $SB > $L/brightness; echo $SG > $G
	[ -n "$SPS" ] && iw dev wlan0 set power_save $SPS
	[ -n "$OP" ] && kill -CONT $OP 2>/dev/null
}
final() {
	restore_cfg
	i=0; while [ $i -lt 10 ]; do echo $LIM > $C/input_current_limit; sleep 2; [ "$(cat $C/input_current_limit)" -ge "$LIM" ] && break; i=$((i+1)); done
	echo "restored: bl=$(cat $L/brightness) gov=$(cat $G) ps=$(iw dev wlan0 get power_save | awk '{print $3}') lim=$(cat $C/input_current_limit) orchard=$(cat /proc/$OP/stat 2>/dev/null | awk '{print $3}')"
	echo PA_DONE
}
trap 'final; exit' EXIT HUP INT TERM
echo 0 > $C/input_current_limit; sleep 4
for cfg in "$@"; do
	for k in $(echo $cfg | tr + ' '); do case $k in
	base) ;;
	bl:*) echo ${k#bl:} > $L/brightness ;;
	gov:*) echo ${k#gov:} > $G ;;
	psoff) iw dev wlan0 set power_save off ;;
	orchstop) kill -STOP $OP ;;
	tsoff) tsup off > /dev/null ;;
	wifioff) wifiup off > /dev/null ;;
	btoff) bluetoothctl power off > /dev/null ;;
	busy) sh -c 'while :; do :; done' & BP="$BP $!" ;;
	esac; done
	echo "== $cfg"
	n=0; s=0; i=0; all=
	while [ $i -lt $((T + 6)) ]; do
		c=$(($(cat $B/current_now) / 1000)); all="$all $c"
		[ $i -ge 6 ] && { s=$((s + c)); n=$((n + 1)); }
		sleep 2; i=$((i + 2))
	done
	echo "samples:$all"
	echo "RESULT $cfg avg=$((s / n)) mA n=$n V=$(($(cat $B/voltage_now) / 1000))"
	for k in $(echo $cfg | tr + ' '); do case $k in
	btoff) bluetoothctl power on > /dev/null; bluetoothctl show | grep Powered ;;
	tsoff) tsup on > /dev/null; tsup status ;;
	wifioff) wifiup on > /dev/null; i=0; while [ $i -lt 30 ] && ! ip -4 addr show wlan0 | grep -q inet; do sleep 1; i=$((i+1)); done; wifiup status; iw dev wlan0 set power_save $SPS ;;
	esac; done
	restore_cfg; sleep 2
done
