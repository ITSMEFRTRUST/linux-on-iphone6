#!/bin/sh
# camvf-bench (test/unit/cam_vf_bench.c): a static aarch64 musl build for the phone, like build-camlut-bench.sh.
# Usage: ui/test/unit/build-camvf-bench.sh [out dir, default var/camfix]; on the phone (Camera app closed):
#   camvf-bench [rear|front] [frames]      or anywhere aarch64:  camvf-bench synth [rear|front] [frames]
# Also builds cam_conv_test for aarch64 (the NEON path = the scalar one, bit for bit): run it on any aarch64 box.
set -e
R=$(cd "$(dirname "$0")/../../.." && pwd); S=$R/ui/src; T=$R/ui/test/unit; O=${1:-$R/var/camfix}; SR=$R/var/sysroot
# arm_neon.h: the compiler's own (the sysroot's clang 22 resource dir has none; it must match the compiler)
NEON=$(clang -print-resource-dir)/include
CC="clang --target=aarch64-linux-musl --sysroot=$SR -resource-dir=$SR/usr/lib/llvm22/lib/clang/22 -idirafter $NEON"
mkdir -p "$O"
nice -n 10 $CC -O2 -Wall -Wno-unused-parameter -mcpu=apple-a8 -static -fuse-ld=lld -rtlib=compiler-rt -D_GNU_SOURCE \
	-I"$S" -o "$O/camvf-bench" "$T/cam_vf_bench.c" "$S/cam_conv.c" "$T/cam_conv_vf.c" -lm
nice -n 10 $CC -O2 -Wall -Wno-unused-parameter -Wno-unused-function -mcpu=apple-a8 -static -fuse-ld=lld \
	-rtlib=compiler-rt -D_GNU_SOURCE -I"$S" -o "$O/cam_conv_test" "$T/cam_conv_test.c" "$S/cam_conv.c" \
	"$T/cam_conv_ref.c" -lm
echo "built $O/camvf-bench $O/cam_conv_test"
