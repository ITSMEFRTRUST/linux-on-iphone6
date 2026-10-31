#!/bin/sh
# test/camera/shutter.sh [B] - the Camera's shutter through the snap build B (default build) against the pretend
# /dev/isp0 (test/unit/cam_isp_mock.c, LD_PRELOAD). Cases: a driver before the preset flags (photo = the next frame,
# no still); with the flags (a 1280x960 still, the shutter greyed meanwhile); Home 300 ms after the tap (the still is
# finished and saved anyway); the last-photo thumbnail tapped during a still (greyed, doesn't open the viewer; the
# photo is saved). snap's `wait` is simulated time (~10x faster than real): the long waits give the mock's 30 fps
# real time to finish a still (~2 s for an ISPMOCK_UNSTEADY one).
B=${1:-build}
cd "$(dirname "$0")/../.." || exit 1
case $B in /*) ;; *) B=$PWD/$B ;; esac
T=$(mktemp -d "${TMPDIR:-/tmp}/camshut.XXXXXX") || exit 1
trap 'rm -rf "$T"' EXIT
cc -shared -fPIC -O1 -Isrc test/unit/cam_isp_mock.c -o "$T/ispmock.so" -ldl -lpthread || exit 1
fails=0
SHUT="375 1246" THUMB="110 1246"

# run NAME "ENV" SNAP-COMMANDS...: fresh data dir, output in $T/NAME/out, screenshots in $T/NAME/
run() {
	name=$1 env=$2; shift 2
	D=$T/$name; mkdir -p "$D/data"; printf 'start_locked=0\n' > "$D/data/prefs"
	(cd "$D" && env $env LD_PRELOAD="$T/ispmock.so" ORCHARD_CAMERA=real ORCHARD_DATA="$D/data" TZ=EST5EDT \
		"$B/snap/orchard-snap" "$@" > "$D/out" 2>&1)
}
photos() { ls "$T/$1/data/Photos" 2>/dev/null | grep -c '^IMG_'; }
px() { magick "$T/$1/$2" -crop "1x1+$3+$4" -depth 8 -format '%[fx:int(255*r)] %[fx:int(255*g)] %[fx:int(255*b)]' info:; }
fail() { echo "FAIL ($1): $2"; fails=$((fails + 1)); }
check_violations() { ! grep -q VIOLATION "$T/$1/out" || fail "$1" "mock: $(grep VIOLATION "$T/$1/out" | head -3)"; }

# 1. a driver before the flags: EINVAL, bare START, the photo is the next frame
run old "" open Camera wait 1500 tap $SHUT wait 3000
grep -q 'driver without preset flags' "$T/old/out" || fail old "no fallback to bare camera numbers"
grep -q '^cam: still' "$T/old/out" && fail old "took a still with a driver without presets"
[ "$(photos old)" = 1 ] || fail old "$(photos old) photo(s), want 1"
check_violations old

# 2. the flags: a full-resolution still, the shutter greyed during it and white again after
run still "ISPMOCK_FLAGS=1" open Camera wait 1500 shot a.ppm tap $SHUT shot b.ppm wait 8000 shot c.ppm
grep -q '^cam: still 1280x960 after' "$T/still/out" || fail still "no 1280x960 still"
[ "$(photos still)" = 1 ] || fail still "$(photos still) photo(s), want 1"
[ "$(px still a.ppm $SHUT)" = "255 255 255" ] || fail still "shutter not white before: $(px still a.ppm $SHUT)"
[ "$(px still b.ppm $SHUT)" != "255 255 255" ] || fail still "shutter not greyed during the still"
[ "$(px still c.ppm $SHUT)" = "255 255 255" ] || fail still "shutter not white after: $(px still c.ppm $SHUT)"
check_violations still

# 3. Home 300 ms after the tap, during a still that never settles: finished and saved anyway, then the sensor off
run home "ISPMOCK_FLAGS=1 ISPMOCK_UNSTEADY=1" open Camera wait 1500 tap $SHUT wait 300 key home wait 30000
grep -q 'finished anyway' "$T/home/out" || fail home "the still wasn't finished after Home"
[ "$(photos home)" = 1 ] || fail home "$(photos home) photo(s), want 1"
grep -q '^ispmock: close' "$T/home/out" || fail home "the camera wasn't closed after Home"
check_violations home

# 4. the last-photo thumbnail during a still: greyed, the viewer doesn't open, both photos saved
run thumb "ISPMOCK_FLAGS=1 ISPMOCK_UNSTEADY=1" open Camera wait 1500 tap $SHUT wait 30000 shot a.ppm \
	tap $SHUT wait 250 tap $THUMB shot b.ppm wait 30000 shot c.ppm
[ "$(photos thumb)" = 2 ] || fail thumb "$(photos thumb) photo(s), want 2"
grep -q 'finished anyway' "$T/thumb/out" && fail thumb "the camera was turned off during the still (viewer opened?)"
[ "$(px thumb c.ppm $SHUT)" = "255 255 255" ] || fail thumb "not the camera at the end (viewer open?)"
ta=$(px thumb a.ppm $THUMB) tb=$(px thumb b.ppm $THUMB)
[ "$ta" != "$tb" ] || fail thumb "thumbnail not greyed during the still ($ta)"
check_violations thumb

[ $fails = 0 ] && echo "camera shutter: all ok" || { echo "camera shutter: $fails failure(s) (logs: kept in $T)";
	trap - EXIT; exit 1; }
