#!/bin/bash
# Build the camera test tools (static aarch64 musl): tools/isp/ispcat (+ ispfocus). The camera driver (apple-isp.ko)
# is built in-tree with the kernel (patches/, CONFIG_VIDEO_APPLE_ISP_T7000=m); bin/mkblob picks it up from the
# kernel build dir. KSRC = the patched kernel tree (for the uapi header), default ../../hoolock-linux.
#
# Firmware: the ISP firmware and its sensor tuning "setfiles" are Apple's and are NOT part of this project. Extract
# them from YOUR OWN device's IPSW (see README, "Firmware") and put them in var/extra-rd/lib/firmware/apple/
# (isp-t7000.bin + the setfiles), which bin/mkblob bakes into the initramfs.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
D=$R/tools/isp
KSRC=${KSRC:-$R/hoolock-linux}
SR=$R/var/sysroot
CC="clang --target=aarch64-linux-musl --sysroot=$SR -resource-dir=$(ls -d $SR/usr/lib/llvm*/lib/clang/* | head -1)"
for t in ispcat ispfocus; do
	[ -f $D/$t.c ] || continue
	$CC -O2 -Wall -Wextra -Wno-unused-parameter -mcpu=apple-a8 -static -fuse-ld=lld -rtlib=compiler-rt \
		-iquote $KSRC/include/uapi/linux -o $D/$t $D/$t.c
	echo "built $D/$t"
done
