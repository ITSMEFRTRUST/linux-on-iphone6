#!/bin/sh
# test/wifi/sleep.sh [B] - Wi-Fi Sleep against a mock wpa_supplicant (wpa_mock.py), snap build B (default build):
# screen off -> after ORCHARD_WIFISLEEP_S Wi-Fi sleeps (DISCONNECT) -> screen on -> SCAN on the old channel ->
# RECONNECT -> connected. Cases: plain; the first RECONNECT failing (wlan0 not back yet); the RECONNECT / the SCAN
# getting no reply (the control socket is given up and reopened: RECONNECT must go again); wlan0 up failing twice
# (retried) / always (then `wifiup on`); RECONNECT failing for good (escalates to `wifiup on`, the marker stays).
# Never touches the PC's network: the snap build only pretends to flip wlan0 (ORCHARD_WIFISLEEP_IFFAIL fakes failures).
# Keep $TMPDIR short: the mock's socket path must fit sun_path (108 bytes).
B=${1:-build}
cd "$(dirname "$0")/../.." || exit 1
T=$(mktemp -d "${TMPDIR:-/tmp}/wsleep.XXXXXX") || exit 1
trap 'kill $MP 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/data"; printf 'start_locked=0\n' > "$T/data/prefs"
fails=0
W="DISCONNECT,SCAN freq=5180,RECONNECT,"

# run NAME "MOCK_ENV" "ORCHARD_ENV" WAKE_WAIT_MS WANT_SEQ CONNECTED(1/0) [GREP...]
run() {
	name=$1 menv=$2 oenv=$3 ww=$4 want=$5 conn=$6; shift 6
	rm -f "$T/mark" "$T/cmds"
	env $menv python3 test/wifi/wpa_mock.py "$T/ctrl" "$T/cmds" & MP=$!
	i=0; while [ ! -S "$T/ctrl" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
	env ORCHARD_WIFI=real ORCHARD_WPA_CTRL="$T/ctrl" ORCHARD_WIFIUP=/bin/true ORCHARD_WIFISLEEP_MARK="$T/mark" \
		ORCHARD_WIFISLEEP_S=60 ORCHARD_POWERLOG=1 ORCHARD_DATA="$T/data" TZ=EST5EDT $oenv \
		"$B/snap/orchard-snap" wait 3000 key power wait 2000 idle 100000 key power wait "$ww" > "$T/out" 2>&1
	kill $MP; wait $MP 2>/dev/null; rm -f "$T/ctrl"
	seq=$(grep -E '^[0-9]+ (DISCONNECT|SCAN freq=5180|RECONNECT)$' "$T/cmds" | cut -d' ' -f2- | tr '\n' ',')
	ok=1
	[ "$want" = "*" ] || [ "$seq" = "$want" ] || { echo "FAIL ($name): commands '$seq', want '$want'"; ok=0; }
	grep -q 'wifi sleep: Wi-Fi asleep after' "$T/out" || { echo "FAIL ($name): never went to sleep"; ok=0; }
	grep -q 'wifi: sleep: asleep' "$T/out" || { echo "FAIL ($name): no 'asleep' (wlan0 down step)"; ok=0; }
	if [ "$conn" = 1 ]; then
		grep -q 'wifi: sleep: awake, connected' "$T/out" || { echo "FAIL ($name): didn't reconnect"; ok=0; }
		[ -e "$T/mark" ] && { echo "FAIL ($name): marker left behind"; ok=0; }
	else
		grep -q 'wifi: sleep: awake, connected' "$T/out" && { echo "FAIL ($name): connected (the mock never lets it)"; ok=0; }
		[ -e "$T/mark" ] || { echo "FAIL ($name): marker gone while wpa_supplicant is still disconnected"; ok=0; }
	fi
	for g in "$@"; do	# cmds:<re> = in the mock's log
		f=$T/out; case $g in cmds:*) f=$T/cmds; g=${g#cmds:} ;; esac
		grep -q -- "$g" "$f" || { echo "FAIL ($name): no '$g' in $(basename "$f")"; ok=0; }
	done
	if [ $ok = 1 ]; then
		echo "ok ($name): $(grep -o 'wifi: sleep: awake.*' "$T/out" || echo 'not connected, marker kept')"
	else
		fails=$((fails + 1)); grep -E 'wifi|power:' "$T/out" | tail -20
	fi
}

run plain "" "" 3000 "$W" 1
run reconnect-fail-1 "MOCK_FAIL_RECONNECT=1" "" 3000 "${W}RECONNECT," 1
run reconnect-noreply "MOCK_DROP_RECONNECT=1" "" 15000 "${W}RECONNECT," 1 \
	'lost wpa_supplicant (no reply)' 'RECONNECT lost with the control socket'
run scan-noreply "MOCK_DROP_SCAN=1" "" 15000 "$W" 1 \
	'lost wpa_supplicant (no reply)' 'RECONNECT lost with the control socket'
run reconnect-late "MOCK_LATE_RECONNECT=1" "" 15000 "${W}RECONNECT," 1 \
	'lost wpa_supplicant (no reply)' 'cmds:late reply to RECONNECT lost'
run ifup-fail-2 "" "ORCHARD_WIFISLEEP_IFFAIL=2" 8000 "$W" 1 'up failed (.*again in 500 ms' 'again in 1000 ms'
run ifup-fail-all "" "ORCHARD_WIFISLEEP_IFFAIL=99" 12000 "$W" 1 'again in 2000 ms' 'falling back to wifiup on' \
	'/bin/true on (pid'
run reconnect-fail-all "MOCK_FAIL_RECONNECT=999" "" 30000 "*" 0 'RECONNECT keeps failing' 'up again (3/3)' \
	'still down (wpa_supplicant: interface disabled), falling back to wifiup on'
exit $fails
