#!/bin/bash
# Build the display-pipe test tools (static aarch64 musl): tools/disp/disp + tools/disp/flipt.
# The display driver itself (adp-t7000.ko) is built in-tree with the kernel (patches/, CONFIG_DRM_ADP_T7000=m);
# bin/mkblob picks it up from the kernel build dir.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
D=$R/tools/disp
S=$R/var/sysroot
CC="clang --target=aarch64-linux-musl --sysroot=$S -resource-dir=$(ls -d $S/usr/lib/llvm*/lib/clang/* | head -1)"
$CC -O2 -Wall -Wextra -Wno-unused-parameter -mcpu=apple-a8 -static -fuse-ld=lld -rtlib=compiler-rt \
	-o $D/disp $D/disp.c
$CC -O2 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -mcpu=apple-a8 -static -fuse-ld=lld -rtlib=compiler-rt \
	-o $D/flipt $D/flipt.c
echo "built $D/disp $D/flipt"
