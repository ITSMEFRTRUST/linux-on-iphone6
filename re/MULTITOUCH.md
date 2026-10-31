# iPhone 6 (N61) touchscreen — iOS 12.5.5 multitouch RE → Linux

Static RE only (nothing run on the phone). Sources: `re/kc.macho` (kexts AppleMultitouchSPI / N61 / AppleSamsungSPI / D2186 / Chestnut / PMGR), IPSW rootfs (`dl/ipsw/`, iOS 12.5.5 16H62), MultitouchHID plugin + MultitouchSupport (dyld cache), the C1F59 touch firmware itself, `re/live-adt.txt`.
Tags: **[C]** seen in code/data (VA given, `f6xxxx` = 0xfffffff007f6xxxx), **[G]** inferred. "z2" = Asahi `drivers/input/touchscreen/apple_z2.c`.

## TL;DR
- **It IS the Z2 family.** iOS class chain: `AppleMultitouchSPI` → `AppleMultitouchZ2SPI` → **`AppleMultitouchN1SPI`** (N61's IOClass). N1 only overrides ctor/dtor, start, bootloader factory and two wake commands; frame read / IRQ path / HBPP transport are the Z2 code [C vtable diff, `re/mtfw/vtables.txt`]. Bootloader `MTSPIBootloader_N1` ⊂ `MTSPIBootloader_Z2`.
- Same opcodes as z2: 0xEB read-with-counter, 0xE1-class reply, HBPP 0x3001 DATA blob with identical checksums, `1a a1` ack, 30-byte finger records at +24 with count at +16. **apple_z2.c is adaptable** — no newer protocol. (`re/TOUCH-PLUMBING.md` §0.1 "not a Z2, new driver needed" is superseded by this.)
- What z2 lacks for N61: power rails + 32 kHz clock + CS/wake dance, HBPP probe, ack checking, `18 e1` DATA prefix, two calibration blobs, N1 register writes + calibrate + EXECUTE, 8-bit SPI (A8 SPI controller is 8-bit only), frame checksums, Y flip.
- Firmware: `/usr/share/firmware/multitouch/N61.mtprops` (rootfs, not kernelcache) → `re/mtfw/`. Ready-made Z2FW file: `re/mtfw/apple-n61-mtfw.bin` (tool `re/tools/n61-mtfw.py`).

## 1. Firmware blob
- Location [C]: rootfs `usr/share/firmware/multitouch/N61.mtprops` (HFS+ decmpfs type 4; decoder `re/tools/decmpfs.py`). iOS user space (mtmergeprops) merges it into the driver via `mt-merge-personality = "C1F59,1"`; the kext takes it as property "Constructed Firmware" (setProperties f62458..).
- Plist: `{"C1F59,1": {"Constructed Firmware": <65320 B>, "Constructed Firmware Version": "0x0412.bin", "PreconstructedBootloadPacketType": "Z2", "ResetInterval": 432000}}`.
- The 65320 B are **one complete, pre-serialised HBPP DATA packet** [C, checksums verified]:

  | bytes | content |
  |---|---|
  | 0..1 | `18 e1` (HBPP sync/DATA prefix; iOS prepends it to every DATA packet it builds too — f6900c, f692d4, f698ec) |
  | 2..3 | `30 01` cmd 0x3001 |
  | 4..5 | `3f c6` = len in 32-bit words (0x3fc6 → 65304 B) |
  | 6..9 | addr = 0 (sw32) |
  | 10..11 | `01 05` = BE16 sum of bytes 4..9 |
  | 12..65315 | payload (ARM/Thumb fw image; byte-swap 16-bit pairs → LE image with ARM vectors `e59ff018` at 0) |
  | last 4 | `62 67 00 58` = sw32(sum of payload bytes 0x00586267) |

  sw32(v) = bytes v[15:8] v[7:0] v[31:24] v[23:16], i.e. exactly what z2 gets by sending LE structs with `bits_per_word = 16`.
- Sent by `MTSPIBootloader_Z2` f68d60 "sending preconstructed firmware with %u bytes": **whole OSData in one SPI transfer** (≤ 0x10000 B; DMA if ≥ 0x100 B unless DisableFDLDMA — PIO is fine on Linux), nothing added; then ATN_ACK `1a a1` → rx must be BE16 **0x4bc1**; up to 5 attempts; no IRQ wait, no inter-packet delay [C f68e90..].
- Files in `re/mtfw/`: `N61.mtprops`, `C1F59-constructed.bin` (raw property), `C1F59-fw-image.bin` (LE payload for disassembly), `MtCl-live.bin` / `PxCl-live.bin` (this phone's calibration from live ADT), `apple-n61-mtfw.bin` (Z2FW container, §8), `vtables.txt`.

## 2. Power-on / reset sequence (iOS order) [C unless G]
Delays: f7cf8040 = IOSleep(ms), f7cf8110 = IODelay(µs).
1. If < **200 ms** since last power-off → sleep the rest (f5e02c; Z2 slot 0x780 = 200).
2. **power_ana on** = Chestnut display PMU (i2c0 **0x27**) reg **0x05 |= 0x10** (LDO2), plus shared boost bits 0x03 and sage bit 0x04 if not set (LCD normally has them on → expect 0x0f → write 0x1f [G]). No delay (`power-ana-enable-delay-us` absent).
3. **power_ldo on** = D2186 PMU (i2c0 **0x74**, 16-bit BE reg addr) reg **0x0085 |= 0x02** (ldo15, switch only, voltage unknown). No delay. Note iOS turns ldo15 OFF at driver start, so it is off after iBoot unless the kext already ran [C f7ff6990].
4. enable_cs(1): IOSleep(**5**), then GPIO **185 (0xb9)** = 1 (CS idles high) [f6590c].
5. clock_enable: PMGR TOUCH_CTL **0x20e01e000**: `v=rd; wr(v&0x7fffffff); udelay(8); wr((v&0x7ffffc00)|0x802dc); poll bit18==0` → 32.768 kHz from 24 MHz (div 0x2dc) [C 852b234]. Off: clear bit19, poll bit18, set bit31, udelay(100).
6. IOSleep(**15**).
7. N1 **REQ_WAKEUP**: tx `19 c1` (2 B full duplex), IOSleep(**10**) — before reset release [f6ad50].
8. Reset release: dummy 16 B transfer `1a a1` + 7×`18 e1` ("ensuring S_CLK is high", rx dropped) → **reset GPIO 2 deassert** (ADT flags 0x10001 = active-low [G strong; same as CS35L19 reset]) → IOSleep(reset-deassert-delay = **15**, default 2) [f653e8, f654a4, f652c0].
9. IOSleep(15), then **HBPP probe** (§3) [f64ef4].
10. Bootload (§4), then IOSleep(2), read identity reports, "Driver is Ready".
- resetDevice: assert reset + 1 ms, sleep min-reset-interval (1 ms), then step 8.
- Power-off: reset assert, CS GPIO = 0, clock off, LDO off, then ANA off.
- Periodic reset every ResetInterval = 432000 s [G link].
- z2 does only: reset low, 5–10 ms, release, wait ≤ 20 ms for a boot IRQ. iOS never waits for an IRQ during boot.

## 3. SPI wire protocol
**Link** [C]:
- ADT `reg` = raw 32-byte nub config `{u32 cs=0, u32 period_ns=124, u8 cpol=0, u8 cpha=0, u8 msb_first=1, u8 bits=8, u32 word_delay_ns=5000, u32 cs_setup_hold_ns=10000}`.
- AppleMultitouchZ2SPI start **overrides period → 83 ns and word delay → 0** (f650a0). So the runtime link is mode 0, MSB first, **8-bit**, ~12 MHz (60 MHz nclk / 5 [G clock]), CS setup/hold 10 µs.
- All transfers full duplex with tx len == rx len.
- CS is a GPIO (185, active-low) driven by the SPI driver per transfer.
- z2 uses 11.5 MHz plus 16-bit words for blobs. Here everything is 8-bit and byte-swapped in software.

**HBPP (bootloader mode)**: 16-bit BE words; checksum = byte sum of everything after the 2-byte opcode, BE16.

| op | tx | reply |
|---|---|---|
| probe | `1a a1` + 7×`18 e1` (16 B) | BE16 rx[0..1] and rx[2..3] ∈ {18e1, 1aa1, 1f01, 4879, 4969, 4ad1, 4bc1} [f66384, f6654c] |
| ATN_ACK | `1a a1` | BE16 status: 0x4bc1 DATA OK, 0x4ad1 write OK |
| DATA | `18 e1 30 01` lenw(BE16) sw32(addr) ck16 payload sw32(sum) | then ATN_ACK = 4bc1 |
| MemWrite | `1e 33` sw32(addr) sw32(mask) sw32(val) ck16 (16 B) | then ATN_ACK = 4ad1 [f668a0] — identical to Asahi's "ReadModifyWrite" record |
| MemRead | `1c 73` sw32(addr) ck16 (8 B) | then long ack `1a a1 18 e1 18 e1 18 e1`; value = BE16(rx[2..3]) \| BE16(rx[4..5])<<16 |
| Calibrate | `1f 01` | ignored = Asahi "RequestCalibration" |
| EXECUTE | `1d 53` sw32(addr) sw32(1) ck16 (12 B) | reply ignored, no ack [f69ecc] |

**Normal mode** (after EXECUTE). 16-byte commands; checksum = LE16 sum of tx[0..13] at [14..15]; reply echoes cmd and carries the same checksum:
- **0xEB read length** `EB ctr 0…`: rx[0]&0xf0 == 0xE0 (z2 insists 0xE1), len = le16 rx[1..2], le16 rx[14..15] = Σ rx[0..13].
- **Data phase** (len+5 bytes; tx `EB ctr 01 …` with the checksum moved to the last 2 bytes):
  - rx[0] = 0xEB/0xEA.
  - low byte of Σ rx[0..4] must be 0.
  - L = le16 rx[2..3].
  - payload = rx[5 .. 5+L-2), followed by le16 Σ payload.
  - z2 reads (len+8)&~3 with a zero tx and checks nothing, but takes the same payload at rx+5.
- The counter alternates 1↔2 **only after a good frame** (z2 toggles on every read).
- Max 5 retries 1 ms apart, then the device is reset.
- Others: E2 device info (iface ver, max packet), E3 report info, E6/E7 GET short/long, E4/E5 SET, E1 CMD_STATUS, **EE wake** (N1: sent before any E2–E7 if ≥ 100 ms idle, then 2 ms sleep; never before frame reads). inter-packet-delay 25 µs between GET/SET stages; IOSleep(2) before each GET/SET.
- Identity reports: 0xD1 family, 0xD3 endianness/rows/cols/bcdVer, **0xD9 sensor surface** (u32 W, u32 H in 0.01 mm + s16 xmin, ymin, xmax, ymax), 0xD0/0xA1 region.
- **IRQ flow**: GPIO **3** (ADT interrupts [3 0]; trigger type unresolved [G falling edge / level low]) → workloop thread → readOneFrame → frame copied **raw** to user space (MTIODataQueue). Report 0x50 = status/critical errors, 0x53 = dynamic-cal messages. Finger parsing is in user space; `parser-type` and `touch-size-id` are never read by the kext.

## 4. Bootload order (MTSPIBootloader_Z2::bootloadDevice f68718 + N1 overrides) [C]
1. enable_download(1) (no `function-enable_download` on N61 → no-op).
2. "Calibration Config" (none in N61.mtprops → skip).
3. **PxCl** DATA → 0x10009600 (1 packet, 0x80 B).
4. **MtCl** DATA → 0x10009000, chunked ≤ 0x3f0 B: 0x3f0 @0x10009000 + 0x10 @0x100093f0. Each packet is followed by an ack check.
5. **Firmware** packet (§1) + ack 4bc1.
6. **N1 performCalibSeq** (f6bd10). All writes mask 0xffffffff; any failure aborts:
   1. MemRead 0x10008ffc (version, logged)
   2. 0x10003060 = 0x17d3 (fll-mval)
   3. 0x1000305c = 0x2 (ref-clk-div)
   4. 0x10003058 = 0x6 (hard-coded)
   5. 0x10003000 = 3 (const-cal; 2 if version == 0x434d11a0)
   6. 0x10003518 = 1 (clk32 enable)
   7. MemRead 0x10003800 ("SPI_APU_EN", unused)
   8. `1f 01`
   9. IOSleep(**65**)
   10. ATN_ACK (only transfer success checked, no polling)
7. **EXECUTE** @ fw-execute-addr **0x10003400**.
8. enable_download(0), IOSleep(**40**).

Calibration happens before the firmware, and the calibration sequence runs between firmware and EXECUTE.

## 5. Calibration blobs
- iBoot fills ADT `multi-touch-calibration` (0x400 B, from syscfg MtCl) and `prox-calibration` (0x80 B, PxCl); the IPSW DT string is just a template. The kext copies them to "Calibration Data" / "Prox Calibration Data" (f64d34) [C].
- **They are sent opaque**: no header, length or version parsing and no trimming. The length is rounded up to 4; blobs over 0x1000 B are dropped. "Invalid calibration data or version" only means the property is missing, and that is non-fatal [C f6a4e0, f6940c].
- Payload bytes go on the wire 16-bit-pair-swapped, like all DATA payloads. MtCl header bytes `61 02 00 02 00 03 00 20 …` are meaningful only to the firmware.
- z2 sends one DT blob (`apple,z2-cal-blob`) as a single 0x3001 packet without the `18 e1` prefix. N61 needs **two** blobs (MtCl + PxCl), possibly chunked at 0x3f0.
- m1n1 must forward both ADT props into the Linux DT (they are per-phone data).

## 6. Contact report format (user-space parser: MultitouchSupport `_MTParse_BinaryFrameHeader` 0x19b73c10c, firmware builder fw+0x538e) [C]
The payload (= z2's `msg`) starts with the report ID: **0x44 = contacts** (normal), 0x45 = contacts + 696 B image, 0x43 = image only.

**Header**:

| off | meaning |
|---|---|
| 0 | report ID |
| 1 | frame counter |
| 2 | header length (24) |
| 3 | header version (2) |
| 4 | u32 timestamp in ms |
| 8 | mode |
| 9 | 7 |
| 10 | image format / b7 = drop frame |
| 11 | record option bits |
| 14 | u16 image length |
| **16** | **contact count** |
| 17 | record size (30) |
| 18 | status |
| 20 | flags |
| 22 | s16 optical proximity |

First record at msg + hdr[2] + hdr[14] (= 24 for 0x44).

**Record (30 B, LE)**:

| off | field | z2.c |
|---|---|---|
| 0 | u8 path id (1–9; slot key) | finger ✓ |
| 1 | u8 stage | state ✓ |
| 2 | u8 finger id | unknown2 lo |
| 3 | s8 hand id | unknown2 hi |
| 4 | s16 X, 0.01 mm | abs_x ✓ (signed) |
| 6 | s16 Y, 0.01 mm | abs_y ✓ (signed) |
| 8 | s16 vX, 1/8 mm/s | rel_x ✗ (velocity) |
| 10 | s16 vY, 1/8 mm/s | rel_y ✗ |
| 12 | u16 major radius, 0.01 mm | tool_major |
| 14 | u16 minor radius, 0.01 mm | tool_minor |
| 16 | s16 orientation (rad = v·π/32768) | orientation ✓ |
| 18 | u16 zTotal (8.8) | touch_major ✗ |
| 20 | u16 zDensity (8.8) | touch_minor ✗ |
| 22 | 0 | unused ✓ |
| 24 | s16 2nd angle | unused ✓ |
| 26 | u16 pressure (g) | pressure ✓ |
| 28 | u16 flags | multi ✗ |

**Stage values**:
- 0 NotTracking
- 1 StartInRange, 2 HoverInRange (hover)
- **3 MakeTouch, 4 Touching** (touching, same as z2's 3/4)
- 5 BreakTouch (lift)
- 6 LingerInRange
- 7 OutOfRange

**Coordinates**:
- iOS normalises with the 0xD9 bounds: nx = (X−xmin)/(xmax−xmin), same for Y.
- **Y points up**, so screen y = (1−ny)·1334 and x = nx·750. No X flip or rotation was found.
- The firmware config holds **5850 × 10405** (58.50 × 104.05 mm = 750×1334 at 78 µm) [C value / G meaning]. Assume X 0..5850 and Y 0..10405 with `touchscreen-inverted-y` until 0xD9 is read on hardware.
- The axis orientation needs one live touch test.

## 7. Linux side — what's needed
**SPI controller**:
- A8 `spi-1,samsung` is spi-apple's register layout minus everything ≥ 0x100 (no FIFOSTAT/IE/IF/SHIFTCFG/PINCFG/DELAY). STATUS 0x08 is W1C with FIFO levels in [10:6]/[15:11], FIFO depth 16, 8-bit only, CTRL 0xD in one write [C AppleSamsungSPI].
- Hoolock has an **uncommitted S5L variant in `drivers/spi/spi-apple.c`** (`apple,t7000-spi`, notes in `re/SPI-S5L.md`). Still needed:
  - honour word_delay (WORD_DELAY 0x38); the touch path doesn't need it (iOS sets 0)
  - in IRQ mode, enable only RX-complete once TX is queued
  - a spi2 DT node: reg 0x20a088000/0x4000, AIC 154, `power-domains = <&ps_spi2>`, `cs-gpios = <&pinctrl 185 GPIO_ACTIVE_LOW>`, clock 60 MHz? [G — `re/TOUCH-PLUMBING.md` suggests clkref 24 MHz; verify the divider on hw]
- PIO suffices; DMA (SIO channels 24/25) isn't needed.

**Touch driver** = apple_z2.c + an N1 variant (`apple,n61-touchscreen`, match data with flags):
1. Supplies: `vdd-ana` (Chestnut 0x27 reg 5 bit 4) and `vdd` (D2186 0x85 bit 1). There are no regulator drivers yet; bring-up via `tools/i2c`. Proper fix: small regmap regulators.
2. 32 kHz clock: syscon PMGR offset 0x1e000 sequence. Bring-up via `tools/mem`; proper fix: a clk provider or driver-side syscon.
3. CS-high GPIO step (the SPI core's cs-gpio idles high anyway), `19 c1` + 10 ms, dummy `1a a1 18e1×7`, reset release + 15 ms + 15 ms.
4. HBPP probe instead of waiting for the boot IRQ.
5. Send blobs as 8-bit: swab16 the z2 cmd-1 payloads; prepend `18 e1` to 0x3001 DATA packets; check the ack (4bc1/4ad1) instead of the 20 ms IRQ wait.
6. Two calibration props (`apple,z2-cal-blob` → MtCl at 0x10009000, plus a prox-cal prop → 0x10009600), chunked ≤ 0x3f0.
7. Delay records (65 ms after `1f 01`, 40 ms after EXECUTE).
8. Frame read: tx `EB ctr 01 … ck` for len+5 bytes; verify both checksums; toggle ctr only on success.
9. Parse: check msg[0] == 0x44 (handle 0x45), use hdr[2]/hdr[14]/hdr[16]/hdr[17], signed coords; map rel_x/y and touch_major/minor fields per §6; `touchscreen-size-x = <5850>`, `-y = <10405>`, `touchscreen-inverted-y` [G]; reset GPIO 2 active-low, IRQ GPIO 3.
10. `CONFIG_TOUCHSCREEN_APPLE_Z2` (+ `INPUT_TOUCHSCREEN`) is off in config-n61.

## 8. Ready-made firmware file (`re/mtfw/apple-n61-mtfw.bin`, 65520 B)
Built by `python3 re/tools/n61-mtfw.py re/mtfw/N61.mtprops re/mtfw/apple-n61-mtfw.bin`. Z2FW v1 container using z2 record semantics (cmd-1 payloads as LE u16 words), in iOS order:
1. **cmd 3** prox-cal @0x10009600 *(new)*
2. cmd 2 cal @0x10009000
3. cmd 1 fw blob (without `18 e1`, driver adds it)
4. 5 × 0x1e33 writes (§4.6, const-cal = 3)
5. 0x1f01
6. **cmd 4** delay 65 ms *(new)*
7. 0x1d53 EXECUTE 0x10003400 (checksum 0x45)
8. **cmd 4** delay 40 ms *(new)*

Unmodified z2 would reject cmd 3/4. The version-dependent const-cal (2 vs 3) is static here.

## 9. Open (need hardware)
- Whether iBoot leaves ldo15, Chestnut LDO2 and TOUCH_CTL on. Read-only checks: `tools/i2c 0 74 R 0085`, `tools/i2c 0 27 r 05`, `tools/mem r 20e01e000`.
- IRQ trigger type.
- SPI input clock (24 vs 60 MHz).
- Whether the device needs the `18 e1` prefix and 0x3f0 chunking (iOS does both; keep them).
- Real 0xD9 bounds, Endianness byte (expect LE), HBPP 0xEB vs 0xEA mode, axis orientation.
- Version register 0x10008ffc value (decides const-cal).

## Provenance
- Kext disassembly: `re/dis/MultitouchSPI.s`. Vtables: `re/mtfw/vtables.txt`.
- IPSW: `dl/ipsw/` (3 GB zip + 4.3 GB extracted rootfs; deletable).
- Related notes from another session: `re/TOUCH-PLUMBING.md` (ADT providers; its "not Z2" and "spi-apple register-compatible" claims are superseded), `re/SPI-S5L.md`.
