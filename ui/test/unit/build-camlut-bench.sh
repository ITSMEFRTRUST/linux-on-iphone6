#!/bin/sh
# cam_lut.c's bench / PPM grader (-DCAM_LUT_BENCH): a static aarch64 musl build for the phone (like
# tools/isp/build.sh) + a PC build. Usage: ui/test/unit/build-camlut-bench.sh [out dir, default var/camlut]
#   camlut-bench CUBE [W H]           load + apply timings on a synthetic W×H picture (default 8 MP)
#   camlut-bench CUBE in.ppm out.ppm  grade a P6 PPM exactly as a saved photo would be
set -e
R=$(cd "$(dirname "$0")/../../.." && pwd); S=$R/ui/src; O=${1:-$R/var/camlut}; SR=$R/var/sysroot
mkdir -p "$O"
nice -n 10 clang --target=aarch64-linux-musl --sysroot=$SR -resource-dir=$SR/usr/lib/llvm22/lib/clang/22 \
	-O2 -Wall -Wextra -Wno-unused-parameter -mcpu=apple-a8 -static -fuse-ld=lld -rtlib=compiler-rt \
	-DCAM_LUT_BENCH -o "$O/camlut-bench" "$S/cam_lut.c"
nice -n 10 cc -O2 -Wall -Wextra -Wno-unused-parameter -DCAM_LUT_BENCH -o "$O/camlut-bench-pc" "$S/cam_lut.c" -lm
echo "built $O/camlut-bench (aarch64) + $O/camlut-bench-pc"
