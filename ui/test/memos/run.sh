#!/bin/sh
# Voice Memos' process plumbing with stand-in arecord / aplay / amixer (nothing touches ALSA):
#   ui/test/memos/run.sh [build dir B, default build]   (needs make B=... snap first)
# Checks: a bottom-mic recording keeps channel 1, a front-mic one channel 2 and switches ASP TX2 to ADC2B and back,
# a busy hw:0,2 refuses with nothing written, arecord's overruns are counted, a failing arecord leaves no file, playback feeds aplay the whole file.
set -e
T=$(cd "$(dirname "$0")" && pwd); UI=$T/../..; B=${1:-build}
SNAP=$UI/$B/snap/orchard-snap
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
export ORCHARD_MEMOS=real ORCHARD_APLAY=$T/fake_aplay.py ORCHARD_AMIXER=$T/fake_amixer.sh MEMOS_LOG=$W/log
export ORCHARD_CAP_STATUS=$W/status ORCHARD_DATA=$W/data TZ=EST5EDT
mkdir -p "$W/data"
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
py() { python3 -I -c "$@"; }
samples() {	# file → "frames first-sample"
	py 'import sys,struct,wave
w=wave.open(sys.argv[1]); n=w.getnframes(); d=w.readframes(n)
print(n, struct.unpack("<h",d[:2])[0], w.getnchannels(), w.getframerate())' "$1"
}
rec() {	# prefs, then the snap script's extra steps
	printf 'start_locked=0\nmemos_mic=%s\n' "$1" > "$W/data/prefs"
	"$SNAP" open "Voice Memos" wait 600 tap 375 1230 wait 300 tap 375 1230 wait 20000 > "$W/snap.log" 2>&1
}
newest() { ls -t "$W/data/Recordings/"*.wav 2>/dev/null | head -1; }

echo closed > "$W/status"
rec 0
f=$(newest); s=$( [ -n "$f" ] && samples "$f" )
case "$s" in "0 "*|"") bad "bottom mic: no recording ($s)" ;; *" 1000 1 48000") ok "bottom mic: ${s%% *} frames, channel 1, mono 48 kHz" ;; *) bad "bottom mic: $s" ;; esac
grep -q "arecord -q -D hw:0,2 -c 2 -f S16_LE -r 48000 -t raw" "$W/log" && ok "arecord arguments" || bad "arecord arguments"
grep -q "cset" "$W/log" && bad "bottom mic touched the mixer" || ok "bottom mic: mixer untouched"

: > "$W/log"
rm -rf "$W/data/Recordings"
rec 2
f=$(newest); s=$( [ -n "$f" ] && samples "$f" )
case "$s" in *" -2000 1 48000") ok "front mic: channel 2" ;; *) bad "front mic: $s" ;; esac
if grep -q "cset name=ASP TX2 Mux ADC2B" "$W/log" && tail -1 "$W/log" | grep -q "cset name=ASP TX2 Mux ADC2A"; then
	ok "front mic: TX2 → ADC2B, then back to ADC2A"
else bad "front mic mixer: $(tr '\n' '|' < "$W/log")"; fi

: > "$W/log"
MEMOS_OLD_DRIVER=1 rec 2
grep -q "cset" "$W/log" && bad "old driver: mixer switched anyway" || ok "old driver: front pref falls back to the bottom mic"

rm -rf "$W/data/Recordings"
printf 'state: RUNNING\nowner_pid   : %s\n' $$ > "$W/status"
: > "$W/log"
rec 0
[ -z "$(newest)" ] && ! grep -q arecord "$W/log" && ok "busy hw:0,2: refused, nothing written" || bad "busy: $(newest)"

echo closed > "$W/status"
MEMOS_REC_XRUN=1 rec 0
grep -q "memos: recorded .* s, pipe 1024 KiB, full 0 time(s), arecord overruns 2" "$W/snap.log" && ok "1 MiB pipe, overruns counted + logged" || bad "overrun log: $(grep 'memos:' "$W/snap.log")"
rm -rf "$W/data/Recordings"

MEMOS_REC_FAIL=1 rec 0
[ -z "$(ls -A "$W/data/Recordings" 2>/dev/null)" ] && ok "arecord failing: no file, no leftover" || bad "arecord failing: $(ls -A "$W/data/Recordings")"

rec 0
f=$(newest)
bytes=$(( $(stat -c %s "$f") - 44 ))
: > "$W/log"
"$SNAP" open "Voice Memos" wait 600 tap 375 280 wait 300 tap 375 1050 wait 3000 > "$W/snap.log" 2>&1
sleep 1
grep -q "aplay -q -t raw -f S16_LE -r 48000 -c 1 -B 200000 bytes=$bytes" "$W/log" && ok "playback: aplay got all $bytes bytes" ||
	bad "playback: $(tr '\n' '|' < "$W/log") (want $bytes)"
[ $fail = 0 ] && echo "memos: all ok" || { echo "memos: FAILED"; exit 1; }
