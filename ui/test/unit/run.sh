#!/bin/sh
# PC unit tests for Orchard's pure logic (ASan + UBSan). Usage: ui/test/unit/run.sh [build dir, default /tmp/orchard-tests]
set -e
T=$(cd "$(dirname "$0")" && pwd); S=$T/../../src; O=${1:-/tmp/orchard-tests}
mkdir -p "$O"
CC=${CC:-clang}
F="-D_GNU_SOURCE -Wall -Wno-deprecated-declarations -Wno-unused-function -g -O1 -I$S -I$T/../.. -I$T/../../third_party/lvgl -fsanitize=address,undefined -fno-sanitize-recover=undefined"
fail=0
run() {	# name, then the *.c sources (and -D / -l flags); any other argument goes to the test
	name=$1; shift; src=; args=
	for x in "$@"; do case $x in *.c|-D*|-l*) src="$src $x" ;; *) args="$args $x" ;; esac; done
	$CC $F $src -o "$O/$name" && "$O/$name" $args || fail=1
}
run net_resolve_test "$T/net_resolve_test.c" "$S/net.c" "$T/lv_stub.c"
run net_ifclass_test "$T/net_ifclass_test.c" "$T/lv_stub.c"
run ico_test "$T/ico_test.c" "$S/ico.c"
run power_test "$T/power_test.c"
run cam_conv_test "$T/cam_conv_test.c" "$S/cam_conv.c" "$T/cam_conv_ref.c"
run cam_src_test "$T/cam_src_test.c" "$S/cam_conv.c"	# (includes cam_src.c itself, behind a pretend /dev/isp0)
run cam_dev_test "$T/cam_dev_test.c" "$S/cam_dev.c" "$S/cam_conv.c"
run memo_test "$T/memo_test.c" "$S/memo.c"
# + ffmpeg lut3d vs the real filters when they are here (local only, never in git)
run cam_lut_test "$T/cam_lut_test.c" "$S/cam_lut.c" -lm "$O" ${LUT_WARM:-} ${LUT_VIVID:-}
run ico_fuzz_replay -DICO_FUZZ_MAIN "$T/ico_fuzz.c" "$S/ico.c" "$T"/../web/icons/*/favicon.ico "$T"/ico_corpus/*
run html_icon_test "$T/html_icon_test.c" "$S/html.c" "$S/net.c" "$T/lv_stub.c"
# cookie jars: needs curl + python3; a local server on a free port (127.0.0.1 only)
if command -v curl > /dev/null && command -v python3 > /dev/null; then
	PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
	python3 "$T/cookie_server.py" "$PORT" & SRV=$!
	sleep 0.5
	export ORCHARD_WEB_ALLOW_LOOPBACK=1
	run net_cookie_test "$T/net_cookie_test.c" "$T/lv_stub.c" "$PORT"
	kill $SRV
else
	echo "net_cookie_test skipped (no curl / python3)"
fi
[ $fail = 0 ] && echo "all tests ok" || { echo "TESTS FAILED"; exit 1; }
