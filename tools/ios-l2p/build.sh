#!/bin/bash
# build l2p for iOS 12 arm64. Needs a libSystem link stub, which is part of Apple's iOS SDK and is NOT shipped here:
# point LIBSYSTEM_TBD at usr/lib/libSystem.tbd from an iPhoneOS SDK you obtained yourself (default: ./libSystem.tbd).
set -e; cd "$(dirname "$0")"
TBD=${LIBSYSTEM_TBD:-libSystem.tbd}
[ -f "$TBD" ] || { echo "no $TBD: set LIBSYSTEM_TBD=<iPhoneOS SDK>/usr/lib/libSystem.tbd"; exit 1; }
clang -target arm64-apple-ios12.0 -O2 -fno-stack-protector -fno-builtin -c l2p.c -o l2p.o
ld64.lld -arch arm64 -platform_version ios 12.0 12.0 -adhoc_codesign -o l2p l2p.o "$TBD"
file l2p
