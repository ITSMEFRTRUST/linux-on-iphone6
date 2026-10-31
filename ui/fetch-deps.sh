#!/bin/bash
# Fetch Orchard's third-party sources and the aarch64 musl sysroot (../var/sysroot).
# Nothing fetched here is bundled in the repository. Integrity:
#   - libvterm: the tarball's SHA-256 is pinned below.
#   - Alpine: apk-tools-static (the bootstrap apk.static) is checked against a pinned SHA-256; Alpine's public
#     signing keys (also pinned by SHA-256, the same files as https://alpinelinux.org/keys/ and the alpine-keys
#     package) go into each root's etc/apk/keys, so apk verifies the signed APKINDEX and every package itself.
#   - LVGL: a shallow git clone of the v9.6.0 tag over HTTPS (no extra pinning).
# The Alpine package versions are pinned in the URLs; when latest-stable moves on, update the versions and hashes.
set -e
cd "$(dirname "$0")"
sha() { echo "$1  $2" | sha256sum -c --quiet - || { echo "fetch-deps: SHA-256 mismatch for $2" >&2; exit 1; }; }
mkdir -p third_party
[ -d third_party/lvgl ] || git clone -q --depth 1 -b v9.6.0 https://github.com/lvgl/lvgl third_party/lvgl
if [ ! -d third_party/libvterm-0.3.3 ]; then
	curl -sfL -o third_party/libvterm-0.3.3.tar.gz https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz
	sha 09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0 third_party/libvterm-0.3.3.tar.gz
	tar xzf third_party/libvterm-0.3.3.tar.gz -C third_party && rm third_party/libvterm-0.3.3.tar.gz
fi

ALPINE=https://dl-cdn.alpinelinux.org/alpine/latest-stable/main
A=../dl/apk
# Alpine's aarch64 package signing keys (alpine-keys: usr/share/apk/keys/aarch64/)
KEYS="616ae350:d11f6b21c61b4274e182eb888883a8ba8acdbf820dcc7a6d82a7d9fc2fd2836d
58199dcc:73867d92083f2f8ab899a26ccda7ef63dfaa0032a938620eda605558958a8041"
apk_bootstrap() {
	[ -x $A/x/sbin/apk.static ] && return
	mkdir -p $A/x $A/keys
	curl -sfL -o $A/apk-tools-static.apk $ALPINE/x86_64/apk-tools-static-3.0.8-r0.apk
	sha c8e2c88c13ba12a12269b79a3543e1190ff8c0ab0beb32b58cadfd5881c619e3 $A/apk-tools-static.apk
	tar xzf $A/apk-tools-static.apk -C $A/x 2>/dev/null
	for kv in $KEYS; do
		k=alpine-devel@lists.alpinelinux.org-${kv%%:*}.rsa.pub
		curl -sfL -o $A/keys/$k https://alpinelinux.org/keys/$k
		sha ${kv#*:} $A/keys/$k
	done
}
# apk_root <root> <packages...>: signature-checked install of Alpine aarch64 packages into <root>
apk_root() {
	local root=$1; shift
	apk_bootstrap
	mkdir -p $root/etc/apk/keys && cp $A/keys/*.rsa.pub $root/etc/apk/keys/
	$A/x/sbin/apk.static --arch aarch64 --root $root --initdb --usermode --no-scripts -X $ALPINE add "$@"
}
R=../var/sysroot
[ -f $R/usr/lib/libc.a ] || apk_root $R musl-dev compiler-rt linux-headers
# libjpeg-turbo (NEON) for the Camera's JPEG encoder (cam_save.c): Alpine's static aarch64 library, in a root of its own
J=../var/jpeg
[ -f $J/root/usr/lib/libjpeg.a ] || apk_root $J/root libjpeg-turbo-dev libjpeg-turbo-static
