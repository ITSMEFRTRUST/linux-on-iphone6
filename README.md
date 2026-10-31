# iphone6-linux

Linux on the **iPhone 6** (Apple A8 / T7000, board **n61**), built on top of
[Hoolock Linux](https://github.com/HoolockLinux). This repository holds the
kernel driver patches, an m1n1 patch, host-side boot tooling, on-device test
tools, an initramfs overlay, and **Orchard**, a touch-first phone shell written
in C on LVGL.

> **Read [DISCLAIMER.md](DISCLAIMER.md) before you start.** This is
> experimental low-level software. It can leave the phone stuck until you put it
> back into DFU mode, and the optional writable-storage step can destroy your
> iOS data.

## What this is

- A **tethered** boot: every power-on goes through the checkm8 bootrom exploit
  from a Linux host over USB. checkm8 does not survive a power-off, so an
  untethered boot is not possible on A8. Unplug or reboot and the phone falls
  back to iOS (or stays in DFU).
- **iOS is not modified.** Linux runs from RAM (kernel + initramfs sent over
  USB on each boot). The internal flash is read-only to Linux, except for one
  *optional* step that lets Linux write inside a single file that iOS
  pre-allocated for it (see [Writable /data](#8-optional-writable-data-on-the-internal-flash)).
- No Apple firmware or iOS files are shipped here. You extract what is needed
  from **your own** IPSW / your own device (see [Firmware](#4-extract-your-own-firmware)).

### Hardware required

- An **iPhone 6** (iPhone7,2, board n61). Not the 6 Plus (n56). Other A8
  devices are not supported.
- A **Linux host** (x86_64 or arm64) with a USB port and a Lightning cable. It
  runs the exploit, sends the boot image, and talks to the phone over USB
  networking.

## Status

✅ works on hardware · 🚧 WIP, partial or untested · ❌ not started

| Component | Status | Notes |
|---|---|---|
| Display | ✅ | Boots on simpledrm. Own `adp-t7000` DRM driver adds page flips at 60 Hz with vblank IRQs. |
| Touchscreen | ✅ | N1 variant of `apple_z2`, multi-touch, ~60 reports/s. Needs the patched m1n1 (calibration) and your own touch firmware. |
| Speaker | ✅ | CS35L19 amp + MCA, CPU-fed (PIO) audio path. ALSA card "iPhone 6". |
| Microphones | ✅ | Bottom, rear and front mics through the CS42L67 codec. Voice Memos app records. |
| Haptics | ✅ | TI DRV2604, LRA mode, iOS voltages. |
| Charger | ✅ | TI SN2400 driver. Holds/charges on PC USB (~500 mA input). Wake-on-plug is 🚧. |
| Battery | ✅ | Fuel gauge readable (level, current, cycle count) via `/sys/class/power_supply/battery` (own `apple_swi_battery` driver: the bq27540 gauge over HDQ/SWI on UART5, read-only, on top of the `bq27xxx` core). |
| Bluetooth | ✅ | BCM4345 on UART, BlueZ + BlueALSA; scan, pair, audio to headphones. Firmware from your own iOS. |
| Wi-Fi | ✅ | BCM4345 over PCIe (A8 PCIe/DART fixes + brcmfmac). Scan/connect from Settings. Firmware from your own iOS. |
| Internal flash, read | ✅ | Nick Chan's WIP ANS1/ASP driver (`asp.c`). ~400 MB/s, read-only by default. |
| Internal flash, writable /data | ✅ | ext4 inside a file pre-allocated by iOS; writes are fenced to that file's extents. Optional, risky; see below. |
| Rear camera | 🚧 beta | Mostly works: ISP driver, live preview, 8 MP stills, autofocus. Still has bugs, see [Known issues](#known-issues--notes). Needs your own ISP firmware. |
| Front camera | 🚧 beta | Same ISP path and the same beta caveats; mirrored-photo option. |
| s2idle suspend | 🚧 | Enters and resumes (timer/button wake) in tests. Not used automatically; small power gain so far. |
| Headphone codec (CS42L67) | 🚧 | Driver does mic capture. Headphone-jack playback is untested. |
| GPU (PowerVR) | ❌ | Research only. All rendering is in software. |
| Baseband / cellular | ❌ | Research only (Qualcomm modem on HSIC). No calls, SMS or data. |
| NFC | 🚧 | Chip powers up and answers NCI; its firmware refuses reader/poll mode. Not usable. |
| Orchard apps | ✅ | Shell and apps run on hardware; see [Orchard](#orchard). |

## Upstream open source vs. what this project adds

This project stands on a lot of existing work. To be clear about who did what:

### Used from upstream (not written here)

| Project | Role |
|---|---|
| [Hoolock Linux](https://github.com/HoolockLinux) kernel (`ans1` branch, which contains `hoolock`) | A7–A11 Linux base: display (simpledrm), buttons, UART, GPIO, I2C, cpufreq, USB gadget, RTC on A8. |
| Hoolock **m1n1** | Boot stage that loads Linux and fixes up the device tree. |
| **HoolockRD** | Base initramfs (busybox, telnet over USB networking). |
| **pongoOS** | Pre-boot environment that receives and boots the image. |
| **palera1n** / **checkm8** | Bootrom exploit + DFU handling used to reach pongoOS. |
| Nick Chan's WIP **`asp.c`** flash driver | Internal storage; part of Hoolock's `ans1` branch. |
| Asahi Linux drivers (mainline) | `apple_z2.c` (touch), `sound/soc/apple/mca.c` (audio template), `clk-apple-nco` (audio clocks), `pcie-apple` / `apple-dart` (used for Wi-Fi). |
| Mainline Linux drivers | `drv260x`, `cs35l33`, `brcmfmac`, `hci_bcm`, BlueZ, ALSA, etc. |
| [LVGL](https://lvgl.io) 9.6, libvterm | UI toolkit and terminal emulator used by Orchard. |

### Added by this project

- **Kernel patches** (`patches/kernel/*.patch`, 138 patches, GPL-2.0-only):
  - SN2400 charger driver + DT binding.
  - CS35L19 support in `cs35l33` (plus a mainline fix for H/G headroom parsing).
  - `mca-t7000`: MCA v1 support with a CPU-fed shim (PIO) instead of DMA.
  - `apple,t7000-nco` support in the NCO clock driver.
  - Z2 **N1** touchscreen variant for `apple_z2`.
  - `cs42l67` codec driver (mics; headphone path WIP).
  - `adp-t7000` display driver (page flips, vblank).
  - ISP camera driver for A8 (`drivers/media/platform/apple/isp-t7000/`). Written
    for this project; Asahi Linux's `apple-isp` driver (for the M1 ISP) was used
    as a reference for the firmware command set and message layouts, as the
    comments in the patches note.
  - `apple_swi_battery` battery-gauge driver (HDQ over UART).
  - A8 PCIe / DART fixes for Wi-Fi, Bluetooth on UART, NFC experiments, power-management tweaks, `drv260x` calibration warning.
  - A write-window option for `asp.c` (`allow_write`, `write_start`, `write_len`) so writes outside the allowed extents are rejected.
  - n61 device-tree nodes for all of the above.
- **m1n1 patches** (`patches/m1n1/*.patch`, 2 patches, MIT): pass the touch
  calibration from the Apple device tree to Linux, plus a small build fix
  (`wcslen`) for newer Rust toolchains.
- **Host tools** (`bin/`, `host/`): image builder (`mkblob`), boot script
  (`boot`, `autoboot`, `audioboot`), USB shell (`ph`, `phsh`), file push
  (`push`), userspace bundle builders (`mkbt`, `mkwifi`, `mknet`, `mkcurl`,
  `mkdisk`), udev autoboot, writable-storage setup (`iosdisk`).
- **Device tools** (`tools/`): freestanding aarch64 helpers for registers, I2C,
  GPIO, SPI, audio tone tests, flash data area, display/camera test tools.
- **Initramfs overlay** (`rd/`): boot hooks for flash, codec, camera, Wi-Fi,
  display, Orchard autostart, NTP, optional Tailscale, and a firewall.
- **Orchard** (`ui/`): the phone shell.
- **Reverse-engineering notes** (`re/`) describing the hardware in our own
  words: `CS35L19.md` (speaker amp), `MCA-V1.md` (audio controller),
  `SIO-PROTOCOL.md` (SmartIO coprocessor), `SN2400.md` (charger),
  `SPI-S5L.md` (SPI controller), `MULTITOUCH.md`, `TOUCH-PLUMBING.md`,
  `TOUCH-POWER.md` (touchscreen), `CAMERA-STREAM.md` (ISP) and
  `POWER-AUDIT.md`, plus a few small helper scripts (`re/*.py`, `re/tools/`).

## Install

Everything below runs on the **host**. Paths are relative to a checkout of this
repo unless stated otherwise.

### 1. Host prerequisites

- `clang`, `lld`, LLVM tools (`llvm-strip`, `llvm-objdump`) — the kernel and all
  tools are built with LLVM.
- Usual kernel build deps: `make`, `bc`, `flex`, `bison`, `openssl` headers,
  `cpio`, `gzip`, `kmod` (for `modinfo`).
- **Rust nightly** (with `rust-src`) — needed by m1n1.
- **palera1n** (tested with the Linux x86_64 build of 3.0.0 beta, from
  <https://github.com/palera1n/palera1n/releases>; used only for its checkm8 /
  DFU stage), **pongoterm** (the pongoOS USB console, built from
  `scripts/pongoterm.c` in [pongoOS](https://github.com/checkra1n/pongoOS)
  with `-DUSE_LIBUSB -lusb-1.0`) and a **`Pongo.bin`** (for example the one in
  [HoolockLinux/docs](https://github.com/HoolockLinux/docs) `binaries/`).
  `bin/boot` expects them at:
  - `dl/palera1n/bin/palera1n` (unpack the release tarball into `dl/palera1n/`)
  - `dl/pongoterm`
  - `dl/Pongo.bin` (or set `PONGO=/path/to/Pongo.bin`)
- The [`ipsw`](https://github.com/blacktop/ipsw) tool, to unpack your own IPSW.
- For Orchard: `clang`/`lld` (the phone build cross-compiles for
  `aarch64-linux-musl`), `resvg`, ImageMagick (`magick`), `curl`, `git`; SDL2
  for the optional desktop preview; libjpeg-turbo headers for the desktop
  builds' JPEG encoder (optional).
- `usbmuxd` / `libimobiledevice` tools (`inetcat`) and an SSH client, only for
  the optional writable /data step.
- Python 3.

### 2. Build the kernel

```sh
# in the root of this checkout (bin/mkblob looks for ./hoolock-linux by default)
git clone https://github.com/HoolockLinux/linux hoolock-linux
cd hoolock-linux
git fetch origin ans1          # Hoolock's ans1 branch: the hoolock branch + Nick Chan's WIP flash driver
git checkout -b n61 ed8528f48  # the tested base ("asp: remove unused init")
git am ../patches/kernel/*.patch
cp ../config-n61 .config
make LLVM=1 ARCH=arm64 olddefconfig
make LLVM=1 ARCH=arm64 -j"$(nproc)" Image.gz dtbs modules
```

The patches apply in order onto `ans1` at `ed8528f48`; newer `ans1` or
`hoolock` commits may need rebasing. `config-n61` uses 4K pages and enables
the drivers this project needs (DRV260X, CS35L33/CS35L19, SN2400, the SWI
battery gauge, Apple NCO, Z2 touch built in; ASP flash, `adp-t7000` display,
ISP camera, CS42L67 codec, PCIe/DART/brcmfmac Wi-Fi as modules). One build
covers everything, Wi-Fi included: `bin/mkblob` picks the modules out of the
build tree. To use a build made with `make O=dir`, pass `KB=dir` to
`bin/mkblob`.

### 3. Build m1n1 (patched)

The stock m1n1 build works for a plain boot, but the touchscreen needs the
calibration patch.

```sh
git clone --recursive -b idevice https://github.com/HoolockLinux/m1n1 m1n1-src
cd m1n1-src
git checkout -b n61 d5a10ac52  # tested base on the idevice branch
git am ../patches/m1n1/*.patch
__CARGO_TEST_CHANNEL_OVERRIDE_DO_NOT_USE_THIS=nightly make USE_CLANG=1 BUILDSTD=1
# result: build/m1n1.bin
```

Without `M1N1=…`, `bin/mkblob` uses `dl/m1n1/m1n1.bin` (for example the
unpatched build from Hoolock's m1n1 CI; touch then has no calibration).

### 4. Extract your own firmware

No Apple firmware is in this repository. Several drivers need blobs that ship
inside iOS. Get the IPSW for **your** device (iPhone7,2; the project was
developed against iOS 12.5.5, 16H62) and unpack it with `ipsw`. Most results
go in `var/extra-rd/`, which `bin/mkblob` copies into the initramfs as-is; the
Wi-Fi files go in `re/wififw/`, where `bin/mkblob` reads them and renames them.
Both directories are gitignored.

| Needed by | What | Where it comes from | Put it at |
|---|---|---|---|
| Touchscreen | N1 multi-touch firmware (Z2FW container) | rootfs `usr/share/firmware/multitouch/N61.mtprops` (HFS+ compressed), "C1F59,1" firmware; the container layout is in `re/MULTITOUCH.md` §1 and §8 | `var/extra-rd/lib/firmware/apple/dfrmtfw-n61.bin` |
| Bluetooth | BCM4345 patch RAM (`.hcd`) | embedded in iOS's BlueTool | `var/extra-rd/lib/firmware/brcm/BCM.apple,n61.hcd` |
| Wi-Fi | BCM4345 firmware, CLM and TX-cap blobs, and an NVRAM text file for your module | iOS root filesystem (`tempranillo` firmware set and NVRAM templates) | `re/wififw/tempranillo.trx`, `tempranillo.clmb`, `tempranillo.txcb`, `n61-unit-MUR.txt` (installed as `lib/firmware/brcm/brcmfmac4345b1-pcie.apple,tempranillo.{bin,clm_blob,txcap_blob,txt}`) |
| Camera | ISP firmware + sensor setfiles | ISP firmware from iOS; the setfiles are arrays inside the dyld shared cache (`H6ISP.mediacapture`), see `re/CAMERA-STREAM.md` §4 | `var/extra-rd/lib/firmware/apple/isp-t7000.bin`, `isp_<module>_01XX.dat` |

The exact extraction commands for each blob, and the converter scripts for the
touch firmware container, the Bluetooth `.hcd` and the Wi-Fi NVRAM, are not
yet documented or included here. Note that once the kernel build has the Wi-Fi
modules (it does with `config-n61`), `bin/mkblob` stops with an error if the
`re/wififw/` files are missing.

Per-device calibration must come from **your** phone; do not copy another
unit's values:

- Touch: iBoot puts the MtCl / PxCl calibration into the live device tree, and
  the patched m1n1 passes it on to Linux. Nothing to do.
- Bluetooth: `tools/btcal` sends the BT TX/RX calibration and sets the BT
  address. It reads them at runtime from `$BTCAL_DIR` (default `/data/bt`):
  `n61-bt-tx-cal.bin` (20 bytes) and `n61-bt-rx-cal.bin` (206 bytes) from your
  phone's syscfg (BTTx / BTRx), and `address` (one line, from the ADT `BMac`).
  How to read them out of syscfg is not yet documented.
- Speaker: Apple's device tree also carries a `speaker-config` property and
  speaker calibration values for the amp. Their meaning is only partly
  decoded (see `re/CS35L19.md`), and they belong to your unit and your own ADT.

### 5. Build Orchard

```sh
cd ui
./fetch-deps.sh      # LVGL 9.6.0, libvterm 0.3.3, an aarch64 musl sysroot (Alpine) in ../var/sysroot,
                     # Alpine's static libjpeg-turbo in ../var/jpeg
make phone           # static aarch64 binary: build/phone/orchard(.stripped)
make sdl             # optional: desktop preview window (mouse = finger, F1 = Home, F2 = Power)
```

`fetch-deps.sh` checks what it downloads: the libvterm tarball and Alpine's
`apk-tools-static` against pinned SHA-256 hashes, and Alpine's public signing
keys (also pinned) are installed so `apk` verifies every Alpine package
signature. The Alpine package versions are pinned in the script and need a
bump when Alpine's `latest-stable` moves on. The `bin/mk*` bundle builders
(`mkbt`, `mkwifi`, `mknet`, `mkcurl`, `mkdisk`) still run `apk` with
`--allow-untrusted`, so they do **not** check signatures yet.

The image also needs userspace bundles in `tools/` (all gitignored, built on
the host from Alpine aarch64 packages): `bin/mkwifi` → `tools/wifi-user.tgz`
(iw, wpa_supplicant), `bin/mknet` → `tools/net-user.tgz` (TLS client, CA
bundle, `nft` for the firewall), `bin/mkcurl` → `tools/net.tgz` (static curl
for Web and Weather), `bin/mkdisk` → `tools/disk.tgz` (e2fsprogs, dmsetup) and
`bin/mkbt` → `tools/bt.tgz` (BlueZ, BlueALSA). Several of them, and
`bin/audioboot`, also expect a `tools/alsa.tgz` (ALSA utilities, mpg123);
how to build that one is not yet documented.

### 6. Build the boot image

```sh
M1N1=m1n1-src/build/m1n1.bin bin/mkblob
```

This concatenates m1n1 + kernel command line + the Apple DTBs + `Image.gz` +
the HoolockRD initramfs (with `var/extra-rd/`, `rd/`, Orchard and the kernel
modules appended) into `m1n1-linux.bin`. Useful variables: `KB=<kernel build
dir>`, `NO_ORCHARD=1` (plain shell), `BOOTARGS_EXTRA="…"`, `OUT=<file>`.
Expected inputs: HoolockRD's `initramfs.gz` at `dl/initramfs.gz` (from
HoolockRD's CI, `initramfs.gz.zip` on
<https://nightly.link/HoolockLinux/HoolockRD/workflows/build/master>), the
kernel build in `hoolock-linux/` (or `KB=`), m1n1 at `dl/m1n1/m1n1.bin` (or
`M1N1=`), and `ui/build/phone/orchard.stripped` unless `NO_ORCHARD=1`.

### 7. Boot

1. Plug the phone into the host and put it into **DFU mode** (iPhone 6:
   hold Home + Power ~8 s, release Power, keep holding Home ~5 s; the
   screen stays black).
2. Run:
   ```sh
   bin/boot                      # or BLOB=path/to/image.bin bin/boot
   ```
   It waits for the DFU device (`05ac:1227`), runs checkm8 through palera1n
   into pongoOS, sends the image with pongoterm, and waits for USB
   networking. "LINUX_UP" means the phone answers on `172.16.43.1`.
3. Get a shell:
   ```sh
   bin/ph "uname -a"             # one command over USB telnet
   bin/push i2c                  # copy tools/i2c to /tmp on the phone (build it first)
   ```
   Orchard starts by itself once the display and touchscreen are up.

The phone runs from RAM: every boot starts from a fresh initramfs. Anything
not on the optional `/data` is lost on reboot.

#### Optional: autoboot on DFU

`host/99-iphone6-linux.rules` + `host/iphone6-linux-boot.service` start the
boot automatically whenever a phone in DFU mode shows up. The service runs
`bin/autoboot`, which logs to `var/autoboot.log` and runs `bin/audioboot`
(boot the blob named in `var/autoboot-blob`, else `m1n1-linux.bin`, then push
`tools/alsa.tgz` and a few test tools).

The service file is a template: replace `YOUR_USER`, `YOUR_UID` and
`/path/to/iphone6-linux` in it first. `bin/boot` calls `sudo` (palera1n and
pongoterm need raw USB access), so unattended autoboot needs a password-less
sudo rule for that user.

> **⚠ Security:** a `NOPASSWD` rule for anything inside this checkout is
> effectively root for any program running as your user, because your user
> can replace those files (or the `dl/` directory itself). If you want
> unattended autoboot, copy palera1n, pongoterm and `Pongo.bin` to a
> root-owned directory such as `/usr/local/libexec/iphone6-linux/`
> (`sudo install -d -o root -g root -m 755 …`, files mode 755/644, owned by
> root), point `bin/boot` at that copy, and allow only those exact paths in
> sudoers. Otherwise skip autoboot and run `bin/boot` by hand, typing your
> password.

```sh
sudo cp host/99-iphone6-linux.rules /etc/udev/rules.d/
sudo cp host/iphone6-linux-boot.service /etc/systemd/system/   # after editing the placeholders
sudo systemctl daemon-reload
sudo udevadm control --reload
```

To turn it off, `touch var/no-autoboot`. **Do this before restoring or
jailbreaking iOS**, otherwise the host boots Linux every time the phone enters
DFU.

#### Getting back to DFU (re-DFU)

The buttons do not trigger a hardware reset while Linux runs. To re-enter DFU:

1. Hold **Home + Power**.
2. While holding, run `bin/ph "reboot -f"` on the host.
3. When the screen goes black, wait ~8 s, release **Power**, keep holding
   **Home** ~5 s.

If the kernel hangs (USB networking gone), the normal DFU button combination
recovers it. To go back to iOS, just let the phone power-cycle normally.

### 8. Optional: writable /data on the internal flash

> ## ⚠️ DATA-LOSS WARNING
> This step makes Linux **write to the same flash that holds your iOS data**,
> using a **work-in-progress** flash driver. A bug here, a wrong offset, or a
> stale configuration after an iOS restore can **corrupt iOS and lose
> everything on the phone**. Back up first. Only do this on a phone whose data
> you can afford to lose. Recovery is a full DFU restore of iOS.

How it stays fenced off:

- Linux cannot parse iOS's APFS (metadata is encrypted with a key held in the
  phone's effaceable storage). So **iOS itself** creates the space: a
  zero-filled, contiguous-as-possible, pre-allocated file
  (`/private/var/linux/disk.img`, default 4 GiB), locked with `chflags`.
- A small iOS tool (`tools/ios-l2p`, built with clang + `ld64.lld`; only the
  `libSystem.tbd` linker stub comes from an iOS SDK) reports the file's physical extents (`F_LOG2PHYS_EXT`).
- The extents are verified by hashing the same ranges on iOS and on Linux
  before any write.
- On Linux, `asp.ko` is loaded with `allow_write=1` plus **write windows** equal
  to those extents; any write outside them is rejected by the driver.
  `dm-linear` stitches the extents together, and the ext4 is created with
  `-E nodiscard`. Its UUID is recorded; Linux refuses to mount anything else.

Steps:

1. `touch var/no-autoboot` (so the host does not boot Linux), then boot iOS
   jailbroken (checkra1n or equivalent) with SSH over usbmux.
2. On the host: `bin/iosdisk [MiB]` — builds the iOS tool, creates and locks
   the file, records its extents and hashes, and writes
   `var/extra-rd/etc/flashdata.conf`. It logs in as `root` over usbmux
   (`inetcat 44`) and asks for the iOS root password interactively; set
   `IOS_ASKPASS=<script>` to have an askpass helper answer it. Building
   `tools/ios-l2p` needs a `libSystem.tbd` from your own Apple SDK
   (`LIBSYSTEM_TBD=…`); none is shipped here.
3. `rm var/no-autoboot`, rebuild with `bin/mkblob`, DFU and `bin/boot`.
4. On the phone: `flashdata hash` and compare with the hashes from step 2.
   **Stop if they differ.**
5. `flashdata mkfs`, then add the printed `UUID=` to
   `var/extra-rd/etc/flashdata.conf`, rebuild the image and boot again.
6. From then on the boot hook mounts it on `/data` (Wi-Fi networks, Bluetooth
   pairings, music, photos, recordings, Orchard settings live there).

If you ever restore or erase iOS, **delete `flashdata.conf` from
`var/extra-rd/etc/`** before booting Linux again. The UUID check is a safety
net, not a guarantee.

## How it works

### Boot chain

`checkm8` (bootrom exploit, via palera1n) → **pongoOS** (receives the image over
USB) → **m1n1** (reads Apple's device tree, fixes up the Linux DT, passes
touch calibration and the flash firmware region) → **Linux** + initramfs. The
initramfs is HoolockRD plus this project's overlay; its boot hooks load the
flash, codec, camera, display and Wi-Fi drivers and then start Orchard.

### Audio

On iOS, a small coprocessor (SIO) moves audio data. Linux does not run that
coprocessor's firmware. Instead the AP programs the MCA (I2S/TDM controller),
its NCO clock and the I2S switch directly, and **feeds samples by CPU writes**
into the SIO "shim" FIFO window (PIO, no DMA). It costs a few percent of one
CPU while playing. The speaker amp is a CS35L19, handled as a variant of the
mainline `cs35l33` driver; mics go through the CS42L67 codec on the same MCA
block.

### Touch

The touch controller is an Apple "Z2"-family chip (N1 variant) on SPI. The
kernel driver uploads the firmware (from your iOS) and per-device calibration,
which m1n1 copies from Apple's device tree. After that it reports multi-touch
input.

### Flash

The internal NAND sits behind Apple's ANS1/ASP controller. Nick Chan's WIP
`asp.c` driver exposes the iOS GPT partition (APFS container) and the small
system partitions as block devices, read-only by default. This project adds the
write-window guard used by the optional `/data`.

### Camera

The A8 ISP is a coprocessor that runs Apple's firmware. The driver loads that
firmware (from your iOS), talks to it over its command interface to start the
rear or front sensor, and receives frames, which Orchard converts (NEON) for
preview and saves as JPEG stills. **Status: beta**: usable, but not finished
(see Known issues).

### Orchard

A static aarch64 (musl) binary drawing straight to DRM with page flips. It
includes: lock screen, home screen with dock, Control Center, app switcher,
search, on-screen keyboard, haptics, Low Power Mode, and these apps:

- **Terminal** (libvterm)
- **Music**: plays local MP3 files. It can also talk to a remote music relay,
  but that is off unless you configure one (`music_relay` setting or
  `ORCHARD_MUSICD`).
- **Files**
- **Settings** (Wi-Fi, Bluetooth, display, battery, …)
- **Clock**, **Notes**, **Calculator**
- **Web**: a lightweight reader-style browser (no JavaScript, no CSS); search
  goes to DuckDuckGo's HTML endpoint.
- **Weather** (Open-Meteo)
- **Camera** (beta): rear and front, stills, with Warm and Vivid filters. The filters
  use 3D LUT files (`warm.cube`, `vivid.cube`) that you supply yourself.
- **Voice Memos**

Web and Weather need Wi-Fi and the `tools/net.tgz` curl bundle.
Web also has an optional weak hook (`web_full_open`) for a full-site
renderer; none ships with this project, so the "Full Site" menu items stay
hidden.

`ui/` builds three variants: `make phone`, `make sdl` (desktop preview) and
`make snap` (headless screenshot tool for tests).

## Known issues / notes

- **Camera is beta.** It mostly works, but known bugs remain:
  - shutter taps made while a still is being captured are dropped silently;
  - the Warm/Vivid filters only apply to the saved photo and thumbnails, not
    to the live viewfinder;
  - the first frames after opening the camera can be dark or black for a few
    seconds;
  - autofocus and the viewfinder-lag fixes have had less testing than the rest;
  - expect other rough edges; it is not a finished camera app.

- **Tethered only**: every power-on goes through checkm8 from the host over
  USB. There is no untethered boot on A8.
- Pressing Power with the display's "deep off" mode enabled can wedge the
  panel; that mode stays disabled by default.
- One harmless amp clock warning is logged per audio stream.
- Battery life is a few hours with the screen on; s2idle is not used
  automatically yet.
- No GPU acceleration; heavy UI is CPU-bound.
- Wake on charger plug-in is not working yet.
- The flash driver is upstream WIP; it cannot be unloaded, so the
  read-only vs. writable choice is fixed for each boot.
- NFC, cellular and headphone playback do not work.
- **Not all patches are upstream-ready.** Some kernel patch subjects are marked
  `LOCAL`, `WIP` or are debug-only (for example `0062 … tsl2583: LOCAL:
  ignore_chip_id`, `0112`/`0113 … ISP … test module (WIP …)`, `0053`/`0056 …
  nxp-nci … debug parameter`). They are kept so the series applies and builds
  as tested; treat them as hacks, not as proposals.
- A few patches (some `adp-t7000` display and ISP patches) cite **iOS
  kernelcache / firmware addresses** in comments, as pointers back to the
  reverse engineering.
- The notes in `re/` cite kernelcache addresses and the live Apple device tree
  (ADT). Neither the kernelcache nor an ADT dump is included: you extract them
  yourself (`re/tools/kc.py` reads your own kernelcache; `DUMPDT=1 bin/boot`
  saves your phone's live ADT to `re/live-adt.txt`, which is gitignored). Some
  notes also refer to helper scripts and notes that are not part of this
  release.

## Security note

The phone exposes an **unauthenticated root telnet shell** on its USB network
interface (from HoolockRD; `bin/ph` uses it). Anyone who can reach that
interface owns the phone. Keep it **USB-only**: do not bridge or route the
USB network (`172.16.42.x` / `172.16.43.x`) to other networks, and do not
expose it through port forwarding or tunnels.

The overlay's inbound firewall (`rd/etc/firewall.nft`, loaded by
`rd/etc/hl-rd.d/44-firewall.sh` before Wi-Fi starts) accepts everything on
`usb0` and `lo` only. From any other interface (`wlan0`, `tailscale0`, …) it
drops TCP port 23 and anything addressed to `172.16.0.0/16`, and lets in only
replies to the phone's own connections, DHCP replies, IPv6 neighbour discovery,
IGMP/MLD queries, Tailscale's UDP port 41641 and rate-limited ping; every other
new inbound connection is dropped. It needs `nft` from `tools/net-user.tgz`
(`bin/mknet`); without it the hook logs `NO FIREWALL` to `/tmp/firewall.log`
and nothing is filtered. Check `nft list ruleset` on your phone before you rely
on it.

Optional Tailscale support (`tools/tsup`, `rd/etc/hl-rd.d/46-tailscale.sh`)
uses Tailscale binaries you install yourself in `/data/tailscale`, and your own
tailnet login.

## License

- `ui/`, `tools/`, `bin/`, `rd/`, `host/`, `re/` and other original files
  in this repository: **GPL-3.0-or-later** (see [LICENSE](LICENSE)), except
  files with their own SPDX header (for example `tools/isp/ispcat.c`,
  GPL-2.0-only).
- Kernel patches: **GPL-2.0-only**, like Linux. m1n1 patches: **MIT**, like
  m1n1. See [patches/LICENSE.md](patches/LICENSE.md).
- Bundled and fetched third-party components keep their own licences; see
  [THIRD_PARTY.md](THIRD_PARTY.md).

## Credits

Created by ITSMEFRTRUST, with AI assistance from Claude (Anthropic).

**AI disclosure.** The code, patches and documentation in this repository were
written with AI assistance (Claude, by Anthropic), with the author directing
the work, testing on hardware, and making the decisions. Kernel patches carry
`Assisted-by: Claude` trailers. Hoolock Linux does not accept AI-assisted
contributions, so **do not submit these patches to Hoolock as-is**; the same
applies to any upstream (including m1n1) with a similar policy. Check each
project's contribution rules before sending anything derived from this work.

Thanks to the projects this builds on: Hoolock Linux (kernel, m1n1, HoolockRD)
and Nick Chan for the ASP flash driver; the Asahi Linux developers for m1n1
and the Apple drivers this project adapts; the pongoOS, checkm8 and palera1n
developers; the LVGL and libvterm projects; and the mainline Linux, BlueZ,
BlueALSA and ALSA developers.

See [DISCLAIMER.md](DISCLAIMER.md).
