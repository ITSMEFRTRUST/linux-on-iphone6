# A8 (T7000) camera: phase 3 = sensor power + streaming (static RE, 2026-10-20)

Camera streaming agent. No phone access. Sources: kext decomp `re/isp/kext-decomp-annot.c` + disasm `re/dis/H6Cam.s`
(AppleH6CamIn 14.23.0), the kernelcache `re/kc.macho` (AppleT7000PMGR, AppleS5L8960XGPIOIC), the ISP fw
`re/isp/adc-athena.bin` (13.42.00) + its Ghidra decomp `re/isp/fw-decomp.c`, the ADT `re/dtree.txt`, Asahi
`re/isp/asahi-src`. Tags: **[V]** read in the binary/decomp (address given) · **[A]** inferred/assumed.
Kext addresses are `0xfffffff008xxxxxx` (shortened to `0x8xxxxxx`), fw addresses are fw offsets (fw linked at 0).

Context: hw run 2026-10-20 (`re/HW-TEST-2026-10-20.md` §F): stages 1-9 + irq=1 pass, START ok, but the fw prints
**"No sensor found on channel 0 !" / "... channel 1 !"**. So §1 (sensor power) is the first thing to fix.

## 0. Bottom line
- **The host powers the sensors, the fw talks to them.** iOS `power_on_hardware` (0x845d2d4) switches the camera
  rail, the sensor reference clocks and the sensor shutdown (XCLR) pins **before** it boots the fw [V]. Linux did
  none of it, which explains "No sensor found". The fw then drives sensor I2C itself (ISP-internal SMBus `+0x82000`)
  and MIPI [V]; there is no AP I2C node for the sensors [V].
- Sensor power plan (§1): GPIO 198 high (cam AVDD LDO) → 1 ms → PMGR ref clocks `0x20e010218/1c` = srcSel 0,
  div 0x28 → extclk pins 191/197 to their alternate function → shutdown GPIOs 190 (rear) / 196 (front) high →
  wait → START. Every value is [V] except the extclk pin-function number (iOS "config 2", periph 1 assumed [A])
  and the ref-clock byte decode (0x28, [A] but strongly suggested by the code shape).
- Streaming command sequence (§3): CH_INFO_GET → CAMERA_CONFIG_GET per preset → (SET_FILE_LOAD) → CONFIG_SELECT →
  RECYCLE_MODE_SET/START → CROP_SET → OUTPUT_CONFIG_SET(NV12) → PREVIEW_STREAM_SET → SIF_PIXEL_FORMAT_SET →
  BUFFER_POOL_CONFIG_GET/SET(meta) → buffers on BUF_H2T → CH_START; frames come back on BUF_T2H.
  Opcodes all [V] (fw dispatcher). Packet layouts: [V] for every field the fw reads (offsets below), except the
  semantics of POOL_CONFIG_SET +0x10/+0x14 and the meta size. The A8 buffer-list format is **not** Asahi's (u32,
  3 planes, 0x28-byte entries) and is [V] from both the fw and the kext.
- Expected hw iterations: 1 for sensor power (detect 0x237/0x190 or not), then 1-3 for streaming (most likely
  trouble: the meta pool / buffer-pool sizes and whether the fw wants the meta buffers at all).

## 1. Sensor power-up (what iOS does before the fw boots)

### 1.1 ADT `isp` node functions (dtree.txt l.2588-2619) [V] and the kext objects they land in [V, disasm 0x845a1dc..a3b4]
| ADT function | value | kext field | n61 meaning |
|---|---|---|---|
| function-cam_avdd_ldo | `24:GPIO(0xc6, 0x101)` | +0x4d8 | GPIO **198**: enable of the external camera AVDD LDO (shared) |
| function-sensor_shutdown | `24:GPIO(0xbe, 0x2000101)` | +0x458 | GPIO **190**: rear sensor XCLR / shutdown |
| function-sensor_shutdown2 | `24:GPIO(0xc4, 0x2000101)` | +0x468 | GPIO **196**: front sensor XCLR |
| function-sensor_extclk | `24:GPIO(0xbf, 0x2000101)` | +0x488 | GPIO **191**: rear MCLK pin (pin mux) |
| function-sensor_extclk2 | `24:GPIO(0xc5, 0x2000101)` | +0x490 | GPIO **197**: front MCLK pin |
| function-conf_isp_ref0/1_clk_freq | `19:ISPC(0/1, 0xd5985c28, 0x265dd221)` | +0x4a0/+0x4a8 | PMGR sensor ref clock 0/1 config |
| function-focus_ldo | `73:pmuL(0x413)` | +0x500 | D2186 LDO for the rear AF actuator (not the sensor) |
| function-torch_enable | `24:GPIO(0x8, 0x101)` | +0x4f0 | torch (not touched) |
| (absent on n61) | sensor_reset*, cam0/1_ldo, cam_avdd/dvdd_gpio, focus_subdev | +0x460.., +0x4b8.., +0x508 | the code skips NULL objects |
Phandle 24 = `gpio` = Linux `pinctrl@20e300000` (208 pins, gpio-ranges identity) [V t7000.dtsi]. Phandle 19 = pmgr.

### 1.2 Order in `power_on_hardware` (0x845d2d4) [V]
1. `0x845cda0(on)`: cam_avdd_ldo (+0x4d8) := `1 ^ this+0x94` (=1 normally), the absent ldo/gpio objects, then
   `IOSleep(1)` (func 0x7cf8040(1)).
2. focus_ldo (+0x500) := 1; `IOSleep(1)`.
3. `EnableISPClocksAndPower` (PMGR ISP/ISPSENS0/ISPSENS1) = our stage 1 (sens=1).
4. ISP `+0x20080 = 1` = our stage 2; DART `setActive` = our stage 3.
5. `0x845cc68`: ref clock config: ISPC ref0 with value `row+0x2c`, ref1 with `row+0x6c`, ref2 `row+0xac`.
6. `0x845cd10(1)`: sensor_reset objects (absent) only if `row+0x149`.
7. `EnableSensor0/1/2` (0x84570cc, 0x845720c, 0x8457324): focus_subdev (absent) := 1; `EnableSensorRefClock(ch)`
   (0x845c800) = extclk object `callFunction(NULL, NULL, &3)` (ref-counted, index `row+ch*0x40+0x3c`);
   then shutdown object := 1; sets "BackCameraActive"/"FrontCameraActive" by `row+ch*0x40+0x10` = 'rear'/'frnt';
   `IOSleep(5)` at the end.
8. `0x845cd10(0)`, then `ISP_Init` (fw boot, our stages 4-9). Sensor detection runs at START inside the fw
   (fw 0x7d6e4 "IC_SensorDiscover", prints "No sensor found on channel %zu !" when it fails) [V].

### 1.3 Platform row 0x2a (kext table 0xfffffff0075d6780, row 6) [V, read from kc.macho]
`+0x00 0x2a` id · ch0 block at +0x00: `+0x10 'rear'`, `+0x28 0x28`, `+0x2c 0` (ref0 value), `+0x38 0x96`,
`+0x3c 0` (ch0 uses ref clock 0) · ch1 block at +0x40: `+0x50 'frnt'`, `+0x6c 0` (ref1 value), `+0x7c 1` (ch1
uses ref clock 1) · `+0x148 01 01 01 01`. So **fw channel 0 = rear (IMX237), channel 1 = front (IMX190)** [V].

### 1.4 The ISPC ref clock (AppleT7000PMGR) [V for the code, A for the byte decode]
- `ISPC` callFunction (0x8534e68): `byte = this->bytes[*value]` (bounds `this+0x34`), `srcSel = byte >> 6`,
  `cfg = byte & 0x3f`, then `ApplePMGR::configISPRefClock(this+0x30 = index, srcSel, cfg)`.
- `configISPRefClock` (0x852b49c): asserts `_regGroups[kRegGroupISPRef0ClkCfg].valid`, index < 3;
  `reg = group.offset + 4*index`; `new = (old & 0xfcffffc0) | srcSel << 24 | cfg`; write; then wait for bit 30
  to clear (0x852c6b0, mask 0x40000000, value 0, timeout 0x2ee00).
- Reg group 0 is set by 0x8537a38 (the only PMGR class in this kernelcache is AppleT7000PMGR [V]) to map 0
  offset **0x10218** → ref0 `0x20e010218`, ref1 `0x20e01021c` [V]. Cross-check: the same function sets group 1
  to **0x1e000**, which is the PMGR TOUCH_CTL register the working Linux `clk-apple-touch` driver uses
  (t7000.dtsi `clock-controller@1e000`), so the table is the right SoC's.
- Byte list = the ISPC args after the index: `0xd5985c28, 0x265dd221` as little-endian bytes
  `28 5c 98 d5 21 d2 5d 26` [A: the ADT parser was not traced; the `lsr #6 / and #0x3f` per byte fits].
  Value 0 (row +0x2c / +0x6c) → byte 0x28 → **srcSel 0, cfg 0x28** for both ref clocks.
- Frequency (getISPRefClockFrequency 0x8537e44): `base / table[srcSel] / ...` with table `{5,7,8,9}` — Hz not
  needed (the fw knows the MCLK from the platform id).

### 1.5 The GPIO function semantics (AppleS5L8960XGPIO function, kc 0x863ca5c) [V]
Flags word `0x2000101` = bytes b0=0x01, b1=0x01, b2=0x00, b3=0x02 (stored at +0x3c..+0x3f).
- `callFunction(&v, …)`: output := `v ^ b1 ^ 1` → b1=1 = active high: **shutdown := 1 drives the pin high**.
- `callFunction(NULL, NULL, &cfg)`: cfg 2 → `setConfig(pin, b0)`, cfg 3 → `setConfig(pin, b3 = 2)`, cfg 0/1/12-15 →
  setConfig(pin, cfg). The extclk call uses cfg 3 → **iOS pin config 2** on GPIO 191/197.
- At creation (all NULL): b0 == 1 → output, value `b2 ^ b1 ^ 1` = **low** (sensor held in reset).
- iOS config numbers are abstract (setOutput uses config 0xe/0xf, getInput config 0); the mapping to the
  register (mode bits 3:1, periph bits 6:5) was not traced. Config 2 = "alternate function" → Linux
  `PERIPH = 1` is the **one assumption** of the plan [A] (all t7000 pinmuxes in Hoolock use function 1).

### 1.6 Linux implementation (driver stage 8 prologue, `sens_pwr=1` default; `isp-sens.c`)
Before PRINT_ENABLE/START (the fw probes the sensors at START [V]):
1. GPIO 198 := 1 via gpiolib (`gpio_request_one`, OUT_INIT_HIGH), log the raw pin register; `msleep(2)`.
2. PMGR `0x20e010218`, `0x20e01021c`: `(old & 0xfcffffc0) | 0x28`, poll bit 30 clear ≤ 10 ms, log old/new
   (`refclk=0` skips, `refclk_cfg=` overrides the byte).
3. GPIO 191/197 pin register: PERIPH (bits 6:5) := `extclk_periph` (default 1, -1 = leave), old value saved.
4. GPIO 190 and 196 := 1 (gpiolib); `msleep(10)` (iOS 5 ms + margin).
Unwind (rmmod, failure, idle power-off) in reverse: 190/196 low + free, extclk pin registers restored, ref clock
registers restored, 198 low + free. focus_ldo (rear AF) is not touched (AF is phase 5) [A: not needed for
detection]. Pin registers 190-198 are logged read-only at every step so the main session sees what iBoot left.

## 2. Channels and messages (fw 13.42.00) [V]
Channel table built by fw 0xa586c (and confirmed on hw, stage 6):
| # | name | type | src (doorbell bit) | slots |
|---|---|---|---|---|
| 0 | TERMINAL | 2 (report) | 0 | 512 |
| 1 | IO | 0 (host→fw) | 1 | 8 |
| 2 | DEBUG | 0 | 1 | 8 |
| 3 | **BUF_H2T** | **0** (host→fw, like IO) | **2** | 64 |
| 4 | **BUF_T2H** | **1** (fw request, host replies) | **3** | 64 |
| 5 | SHAREDMALLOC | 1 | 3 | 8 |
| 6 | IO_T2H | 1 | 3 | 8 |

### 2.1 Buffer list (both directions) [V fw 0xa9590 (H2T rx), 0xa8ff0/0xa944c (T2H build); kext 0x845bfb0 (T2H rx)]
```
struct t7000_buflist {          /* fw-relative address in message word0 */
    u32 type;                   /* T2H: 1 or 2 (fw uses 2), 4 = FFW_INTERPROC_MSG; H2T: not checked (Asahi: 1) */
    u32 count;                  /* kext: > 8 = "FW is sending bad message" -> clamps to 8 */
    struct {                    /* 0x28 bytes, entries start at +8 */
        u32 addr[3];            /* plane fw-relative addresses (Y, UV, -) */
        u32 flags[3];           /* fw T2H: (buf flags & 0x0fffffff) | 0x40000000 when the buffer holds data */
        u32 num_planes;         /* +0x18 */
        u32 pool;               /* +0x1c pool type (low 28 bits): 0 META, 1 RENDERED, 7 CAPTURE_FULL_RES, 9 SCL1 */
        u64 tag;                /* +0x20 host cookie, echoed back unchanged (iOS: kernel object pointer) */
    } e[];
    /* T2H only: +0x148 u32 (fw 0xa944c param_5), +0x14c timestamp/extra (FUN_b5ae8), list size 0x180 */
};
```
- **H2T** (host submits empty buffers): word0 = fw-rel list, word1 = length, word2 bit 28 (0x10000000) = list mode
  [V fw 0xa9590: without bit 28 word0 is a single buffer address and word2 & 0x0fffffff its pool]. The driver sends
  word2 = 0x30000000 (Asahi's COMMAND value, bit 28 set) and word1 = max(0x180, size) [A]. Completion = the fw
  sets bit 0 of word0 (same type-0 rule as IO) [V kext ISR counts BUF_H2T completions, 0x845aea4].
  The fw logs (does not reject) plane addresses inside its own region `ctx+0xa8..+0xac` [V].
- **T2H** (fw returns filled buffers): word0 = fw-rel list, word1 = 0x180, word2 = id | 0x10000000 [V fw 0xa944c ->
  0xa7828(chan, list, 0x180, w | 0x10000000)]. Host reply in the same slot: **word0 = (word2 & 0x10000000) ?
  list | 1 : 1, word1 = 0, word2 = 0x80000000** [V kext 0x845c744-0x845c760].
- Asahi differences: Asahi's buflist is u64 type/count + 0x40-byte entries with u64 iovas[4] (M1 fw 12.x); A8 is
  u32 with 3 planes. Asahi's "done" test (`arg2 == 0x10000000`) does not apply; on A8 frame/no-frame comes from the
  context (returned while streaming vs. after CH_BUFFER_RETURN) [A].

## 3. Command packets (IO channel, opcode u16 at +4, status u16 at +6, channel u32 at +8)
Sizes from the kext call sites where they exist, else from the highest offset the fw handler touches.
| opcode | name | size | fields (beyond the 0xc-byte header) | evidence |
|---|---|---|---|---|
| 0x010d | CH_INFO_GET | **0xc4** | +0x20 sensor id (0x237/0x190), +0x60 num presets (0 = no sensor), +0x98 ts freq, +0x9e module SN (18 B) | [V] kext ISP_InitialSensorDetection (decomp l.6990) uses 0xc4, +0x20, +0x60; fw 0x185dc case 0x10d writes up to +0xc0 |
| 0x0106 | CH_CAMERA_CONFIG_GET | **0x84** | in: +0xc preset; out: +0x10/+0x12 in w/h, +0x14/+0x16 out w/h (u16), +0x1c/+0x1e (fps-related), ... | [V] kext (decomp l.7058) uses 0x84; fw case 0x106 |
| 0x0107 | CH_CAMERA_CONFIG_SELECT | 0x10 | +0xc preset | [V] kext ISP_ConfigureSAC (decomp l.6762), fw vtable+0xb8 |
| 0x0111 | CH_SET_FILE_LOAD | 0x14 | +0xc fw-rel addr, +0x10 size | [V] kext 0x8457844 (addr = surface - fw base), fw case 0x111 (needs a detected sensor, else -1) |
| 0x010e | CH_BUFFER_RECYCLE_MODE_SET | 0x10 | +0xc mode (Asahi 1 = EMPTY_ONLY) | [V] fw reads +0xc; [A] value |
| 0x010f | CH_BUFFER_RECYCLE_START | 0xc | - | [V] |
| 0x0801 | CH_CROP_SET | 0x1c | +0xc x, +0x10 y, +0x14 w, +0x18 h | [V] fw 0x8570 case 0x801 stores 4 u32 (CROP_GET returns them in the same order); [A] x/y/w/h meaning (Asahi) |
| 0x0b01 | CH_OUTPUT_CONFIG_SET | 0x38 | +0xc w, +0x10 h, +0x14 colorspace, +0x18 format (0 = 2-plane YUV), +0x1c/+0x20 strides Y/UV, +0x28 padding rows | [V] fw 0x162b8: format 0 → plane sizes stride0*h and stride1*h/2 (= NV12) and configures the RENDERED pool (#1) itself |
| 0x0b0d | CH_PREVIEW_STREAM_SET | 0x10 | +0xc 1 = on | [V] |
| 0x0115 | CH_SIF_PIXEL_FORMAT_SET | 0x14 | +0xc u8 format, +0xd u8 type (Asahi 3/1) | [V] fields, [A] values |
| 0x0116 | CH_BUFFER_POOL_CONFIG_GET | 0x100+ | out: 0x10-byte records from +0x10: u16 type, u16 id, u16 (0x40), u16 count, u32 size, … | [V] fw 0x16b90 (layout), [A] meaning |
| 0x0117 | CH_BUFFER_POOL_CONFIG_SET | 0x9c | +0xc u16 pool type, +0x10 u32, +0x14 u32 (Asahi meta_size0/1) | [V] fw 0x16b64 reads only these; [A] semantics |
| 0x0100 | CH_START | 0xc | - | [V] fw 0x142cc case 0x100 |
| 0x0101 | CH_STOP | 0xc | - | [V] kext ISP_deInit sends 0xc per channel |
| 0x0104 | CH_BUFFER_RETURN | 0xc | - | [V] fw case 0x104 (FUN_15a60 = return all buffers) |
Absent on A8 (do not send): SBS_ENABLE, MBNR_ENABLE, FLICKER_SENSOR_SET, LPDP, semantic_* [V opcode table].
Not sent in v1 (Asahi sends them, optional): CNR_START 0xa2f, AE metering/stability/fps, temporal filter.

## 4. Setfiles [V for the format/location, A for the module choice]
- iOS 12.5.5 ships **no setfile files** in the rootfs: they are arrays inside the dyld cache (H6ISP.mediacapture),
  carved to `re/isp/setfiles/` (PHASE0 §4). Rear IMX237 modules 8421/8422/8423 (0x33d8 bytes, magic
  0x84210110 ...), front IMX190 1421/1422 (0xa710). The selector picks the module from the sensor NVM.
- The kext hands it to the fw with CH_SET_FILE_LOAD (fw-rel address of a DART surface, size) before streaming;
  the fw replays the embedded tuning commands. Asahi treats a missing setfile as a warning (no calibration).
- Driver: `request_firmware("apple/isp_<module>_01XX.dat")`, module = `setfile_rear`/`setfile_front` params
  (default 8421 / 1421), checked against the BE magic; missing file = warning, streaming continues.

## 5. Streaming sequence implemented (driver stage 10, `stream=1`)
START(camera N → fw channel N): sensor on (stage 8 already did §1.6; per-camera shutdown pins stay as set) →
CH_INFO_GET (id, presets; 0 presets → -ENODEV "no sensor") → CAMERA_CONFIG_GET for each preset (≤ 16, logged) →
pick the preset with the smallest output ≥ 640×480 (`preset=` overrides) → SET_FILE_LOAD → CONFIG_SELECT →
RECYCLE_MODE_SET(1) → RECYCLE_START → CROP_SET(0, 0, in_w, in_h) → OUTPUT_CONFIG_SET(out_w, out_h, REC709, NV12,
strides ALIGN(w, 64)) → PREVIEW_STREAM_SET(1) → SIF_PIXEL_FORMAT_SET(3, 1) → POOL_CONFIG_GET (logged; META size
from the type-0 record) → POOL_CONFIG_SET(META, size, size) when a size was found → allocate nbufs NV12 surfaces
(+ nbufs meta surfaces) → submit all on BUF_H2T → CH_START.
Frames: BUF_T2H list → RENDERED entries by tag → ready queue → read(); META entries are resubmitted at once.
STOP: CH_STOP → CH_BUFFER_RETURN → wait ≤ 500 ms for the buffers → done (buffers stay mapped until power-off).
Output size: fw-chosen preset output (W×H from CONFIG_GET); NV12, stride = ALIGN(W, 64), UV at stride*H.

## 6. Verified vs guessed (decides the number of hw iterations)
Verified in binaries: every opcode; CH_INFO/CONFIG_GET/SELECT/SET_FILE_LOAD/CH_STOP sizes (kext call sites);
the fields the fw reads for OUTPUT_CONFIG_SET/CROP_SET/PREVIEW/SIF/POOL_SET; channel types/doorbells; both buffer-list
formats and the T2H reply; the sensor GPIOs, polarity, order; the ref clock register and its write rule.
Guessed: (1) extclk pin function = periph 1; (2) ref clock byte 0x28 (decode of the ISPC args);
(3) whether META buffers + POOL_CONFIG_SET are needed and their size; (4) H2T word1/word2 values (0x30000000 copied
from Asahi; only bit 28 is checked by the handler we read); (5) RECYCLE_MODE 1, SIF 3/1, colorspace 1 (Asahi);
(6) preset choice / crop = full input; (7) which IMX237 module setfile (8421 default).
Plan: hw run 1 = stage 9 + sens_pwr (expect "found" instead of "No sensor found", fw text with fwlog=1); run 2 =
stream test (`ispcat`); runs 3-4 = fix whichever of (3)-(5) the fw complains about (its TERMINAL log names the
failing command: "[ISP] CMD = 0x...", and the status u16 of every command is logged).

## 7. Orientation, mirror, colour range (for a v2 INFO; Orchard has env overrides meanwhile)
- **Colour range / matrix: unknown, set by us.** OUTPUT_CONFIG_SET +0x14 is the field the fw itself logs as
  "range" ("Preivew Output config -> format = %d, range %d, size = %dx%d", fw string 0x146437, used at fw 0x1b99c
  with +0x18 = format, +0x14 = range) [V]. Asahi sends 1 there and calls it colorspace REC709. The driver sends
  `out_range` (default 1). Which value means video (16-235) vs full (0-255) range, and whether the matrix is
  BT.601 or BT.709, is [A] until a frame is checked on hw: point the camera at black + white, read the Y
  histogram of `/tmp/frame.nv12` (min ≈ 16 / max ≈ 235 = video range), then try `out_range=0` / `2` and compare.
- **Mounting orientation: not in the ADT** (no camera rotation property in `re/dtree.txt`; the kext reads
  "back-camera-tilt-and-rotation" from another provider, probably syscfg, which we do not have) [V for the
  absence]. [A] like every iPhone: both sensors deliver landscape frames (long side = sensor width) and a portrait
  UI has to rotate 90°; the direction (90 vs 270) and any vertical flip must be checked once on hw with an
  arrow/text target.
- **Mirror:** [A] the sensors are not mirrored by the fw (no mirror command is sent; iOS mirrors the front
  preview in software). Orchard should mirror the front camera for a selfie-style preview.
- The fw keeps an `m_imageOrientation` in its AE face logic (fw string), i.e. orientation is metadata to the fw,
  not something it applies to the output [A].
- ioctl letter 'I' collides with ISDN in `ioctl-number.rst`: kept for v1, to change before upstreaming.
