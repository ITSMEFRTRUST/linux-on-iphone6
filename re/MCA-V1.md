# A8 (t7000) MCA, `mca-version 1` — register map

Source: iOS 12.5.5 `AppleAE2Audio` kext (`AppleAE2MCA`, AppleA5AE2-89.14), disassembly `re/dis/AE2Audio.s`, plus live reads on the phone (`re/mca2-poweron.txt`).
MCA2 (speaker) = phys `0x20a0a8000` (size 0x4000) + per-instance cfg `0x20a002008`; IRQ 171; power domain `mca2` = PMGR `0x20e020140` (write target state 0xf; Linux leaves it off-0 because nothing consumes it).

Apple accesses registers only through `_readReg(idx)` 0x…86e96b8 / `_writeReg(idx,val)` 0x…86e9738 → offset = idx*4.
Supported versions: 0,1 (this layout; config fn 0x…86e729c, IRQ fn 0x…86e922c) and 4,5 (`_setIISConfig_v4` 0x…86e7850, different layout — ignore).

| off | idx | reset (live) | meaning (CONFIRMED from code unless marked ?) |
|---|---|---|---|
| 0x00 | 0 | 0x00010002 | version/ID (only logged vs DT `mca-version`) |
| 0x04 | 1 | 0x00800080 | FIFO depth: TX [9:0] = 0x80, RX [25:16] = 0x80 (v0/1 expect 0x80) |
| 0x08 | 2 | 0 | MSB pad: [7:0] = 32 − txWidth, [15:8] = 32 − rxWidth |
| 0x0c | 3 | 0 | frame/clock cfg: [31:24] = clk/2 − 1 (from helper), [15:12] = slots − 1 (cfg byte 4), bit3 = !(cfg[1]&2), bit2 = master/slave mismatch, bit6 (v≠0, cfg[0xe]==0), bit0 = MCLK output, low bits from bit-width table @0x…71fc8c0. Start sets bit1 (`|= 2`, fn 0x…86e8298) |
| 0x10 | 4 | 0x00000002 | IRQ enable? config writes 0, later 3 (cfg u16@0xc ≠ 0) or 1; IRQ handler checks bit1 before frame-error handling |
| 0x14 | 5 | 0 | clock divider (from helper sp+0x14; halves packed lo/hi when cfg[4]==2 && cfg@0xe==0) |
| 0x18 | 6 | 0x00000008 | **TX control**: bit0 = enable, bits[16:8] = slot offset/count (cfg byte 6 (+1)), low nibble 9 or 0xd, bit1 per cfg[1] bits 5:4. Config writes it with bit0 cleared; `startIISCommand` writes the stored value (bit0 set) |
| 0x1c | 7 | 0x00000004 | TX ? — config writes 0xd |
| 0x20 | 8 | 0x007f0000 | TX FIFO: [31:16] = depth − 1, low = DMA burst/threshold (4 or depth) |
| 0x24 | 9 | 0xffffffff | TX slot mask (cfg u32@0x14, trimmed to txChannels bits) |
| 0x2c | 0xb | 0x00000008 | **RX control** (same format as 0x18) |
| 0x30 | 0xc | 0x00000004 | RX ? — config writes 0xd |
| 0x34 | 0xd | 0x007f0000 | RX FIFO: [31:16] = depth − 1, low = 4 |
| 0x38 | 0xe | 0xffffffff | RX slot mask (cfg u32@0x18) |
| 0x40 | 0x10 | 0x10000000 | **IRQ status, W1C** (mask 0xb0003000): bit31 frame error, bit29 RX overflow, bit28 RX underflow, bit13 TX overflow, bit12 TX underflow |
| 0x44 | 0x11 | 0 | self-clearing command: bit0 / bit1 = TX / RX ? (fn 0x…86e85b0), bit8 / bit9 = TX / RX FIFO flush (fn 0x…86e84cc), v0 init writes 0x301 |
| 0x48 | 0x12 | 0 | v1 init: write 0x11=3, 0x12=1, poll 0x12 bit0 clear (soft reset) |
| 0x1000.. / 0x1800.. | – | non-zero | never touched by the AP driver → likely the TX / RX FIFO data windows the SIO DMA uses (?) |

## Config sequence (v1, fn 0x…86e729c)
1. `0x44 = 3; 0x48 = 1; poll 0x48 bit0 == 0` (reset)
2. `0x10 = 0; 0x1c = 0xd; 0x30 = 0xd`
3. `0x08 = pads; 0x20 = ((txDepth−1)<<16)|4; 0x34 = ((rxDepth−1)<<16)|4`
4. `0x14 = clkdiv; 0x24 = txSlotMask; 0x38 = rxSlotMask`
5. `0x18 = txCtl & ~1; 0x2c = rxCtl & ~1; 0x10 = 1|3; 0x40 = 0xb0003000 (clear IRQs); 0x0c = frame cfg`
Start: `0x18 = txCtl (bit0)`, if RX `0x2c = rxCtl`, then `0x0c |= 2`. Stop: `0x18 = 0; 0x44 = 0x100; poll bit8` (TX), `0x2c = 0; 0x44 = 0x200; poll bit9` (RX).
Direction bits in Apple's code: bit0 = RX, bit1 = TX.

## Speaker config (DT `mca2/audio-speaker` `reg`, 36 bytes = the `iisConfig` struct)
`10 12 00 00 | 08 00 01 00 | 00 1b b7 00 (12 MHz) | 00 00 10 00 | 30 00 01 00 | 01 00 00 00 | 0f 00 00 00 | 01 04 10 10 | 00 00 00 00`
- u32@0x08 = 12 000 000 (MCLK), u16@0x10 / u16@0x12 = 48 / 1 → 48 kHz, u16@0x0e = 16
- TX slot mask 0x1, RX slot mask 0xf, txChannels 1, rxChannels 4, txWidth 16, rxWidth 16
- byte 4 = 8 (→ 0x0c[15:12] = 7, slots/frame?), byte 1 = 0x12
- Reference tap (`audio-codec-reference`): same but slot masks 0xff/0xff, 8/8 channels? (`…ff 00 00 00 ff 00 00 00 08 08 10 10`)
- Still unknown: helper 0x…7e6b3d8 (turns the config into width/rate/clkdiv/word values), NCO clock (`function-mclk_frequency: NCOf(2)`), i2s-switch route (`function-i2s_route: i2sR('aud2','pin2',0x330303)`), the FIFO data port offset.

## Clocking (from `AppleS5L8960X` platform kext, `AppleS5L8960XNCO`)
- MCA2's `function-mclk_frequency: NCOf(2)` → NCO channel 2 set to `2 × fs × frameBits` = 2 × 48 000 × 128 = **12.288 MHz** (MCA divides by 2 → BCLK 6.144 MHz).
- A8 NCO @ `0x20e011000`, 5 channels, **stride = DT `pmgr-nco-page-size` (default 0x1000)** — M1 is 0x4000. Same registers as mainline `clk-apple-nco.c`: CTRL 0x0 (bit31 enable, **bit30 busy — iOS polls it before every write**), DIV 0x4 (coarse via LFSR poly 0xa01 / init 0x7ff, fine [1:0]), INC1 0x8, INC2 0xc, ACCINIT 0x10. Set-rate fn 0x…833a3xx–a6b0, reg writer 0x…833a728.
- iOS CTRL value = 0x80001000 (| 0x20000000, | 0x01000000 under flags) — mainline only sets bit31; bit12 may matter.
- NCO input clock ("nclk") rate is filled in by iBoot at runtime (DT `clock-frequencies` = 0 in the IPSW dump) → **measure on hardware**.
- → Linux: `clk-apple-nco.c` needs a per-compatible channel stride (+ maybe CTRL bit12); no new driver.

## i2s-switch (`AppleAE2I2SSwitch3`, @ `0x20a003000`, reset 0x00181818 per word)
- Route fn 0x…86e5810: `i2sR(src, dst, flags)` with names 'audN' / 'pinN'. For src 'aud2': reg index = '2' − 0x2b = 7 (0x1c); mode 0 → mask |= 0x1f00 (or 0x1f0000 when arg4 ≠ 0), value |= ('2' − 0x28 = 10) << 8 (or << 16). So MCA2 → pin2 = switch reg 0x1c field [12:8] (or [20:16]) = 10.
- Third arg `0x330303` not decoded yet. Pin mux for the 'pin2' I2S pads (pinctrl function) still unknown.

## Decoded clock config (2026-10-17)
C = CONFIRMED from code, G = guess. MCA2 base 0x20a0a8000, speaker iisConfig as above (`cfg[0]=0x10`, `cfg[1]=0x12`, `cfg[2]=0`).

**Helper 0x…7e6b3d8** `(cfg, *slotBits, *frameBits, *padBits, *bclkDiv)` (C):
slotBits = roundup8(max(cfg[0x1e],cfg[0x1f])) (or cfg[5] if ≠0) = 16 · frameBits = slotBits×cfg[4] (or u16@0xc if set, must be ≥) = 128 · padBits = frameBits − slotBits×cfg[4] = 0 ·
bclkDiv by cfg[2]&3: 0 → 1, 1 → u32@8, 2 → ~MCLK/frameBits rounded to even (exact or fail) = **1** (MCA then forces 1→2).
Width table @0x…71fc8c0 (index (w−16)/4): 16→0x100000, 20→0x200000, 24→0x300000, 28→0 (invalid), 32→0x400000.

**Corrected field meanings (C):** 0x0c [31:24] = FSYNC width − 1 (u16@0xe if ≠0, else frameBits/2) — *not* a clock divider; [22:20] = slot width code (table); [15:12] = slots − 1; [11:8] = bclkDiv/2 − 1 (BCLK = NCO/bclkDiv); bit0 = **clock master** (only when cfg[0] bit0, also programs NCO = bclkDiv×fs×frameBits = 12.288 MHz); bit2 = cfg[1]bit0 ≠ cfg[0]bit0; bit3 = !(cfg[1]&2); bit6 = (v≠0 && u16@0xe==0). 0x14 = padBits (extra bits/frame), packed lo/hi only if cfg[4]==2. 0x18/0x2c [16:8] = cfg[6] (+1 when master). 0x20 low = FIFO DMA threshold = depth (since `busy-dma` absent ⇒ `[0x12c]`=1).
**cfg[0] bit0 = 0 for the speaker ⇒ iOS runs MCA2 as BCLK/FSYNC SLAVE and never programs NCO2 for it.** The `audio-codec-reference` cfg on the same MCA2 (cfg[0]=0x11) is the master variant. A slave MCA with no external clock = exactly the observed "no TX underflow".

**Second mapping 0x20a002008 = `mcaMCLKControl`** (`_getMCLKClkCon` 0x…86e87b4, written by config 0x…86e73a4, `setMCLKEnabled` 0x…86e8990, `setIISControllerActive` 0x…86e6db0) (C):
cfg[2]&3==0 → sel = 0x10 if nclk==MCLK(12 MHz), 0x18 if nclk==2×, 0x20 if 4×, else 0; cfg[2]&3≠0 → sel = 0x08 if bclkDiv==1. `| 0x40` = MCLK pin output enable (cfg[0] bit4 && enabled). Speaker ⇒ 0x50/0x58/0x60. "nclk" = AppleARMIO `clock-frequencies` entry tagged nclk via `clock-frequencies-nclk` (not in our ADT dump) ⇒ **sel is G; 0x58 (nclk = 24 MHz) most likely**.

**i2s-switch (t7000 = `AppleAE2I2SSwitch3`, vtable 0x…7623368; the 0x…86e5810 fn in the old notes is Switch5)** (C unless G):
20 words @0x20a003000, all rewritten 0x00→0x4c each update (0x…86e9dec). Reset: words 0–13 = 0x00181818, 0x38 = 0x18, 0x3c–0x4c = 0.
Word = per node: PINn = n (0x00–0x10), AUDn = n+5 (AUD2 → 0x1c), DSPn = n+10; 0x3c+4n = PINn control. Node codes: AUDn = n, PINn = n+8, DSPn = n+0x10, dspc 0x17, 0x18 = none.
Route fn 0x…86e9f5c(mode, src, dst, lane): mode0 → src word [12:8] (lane1: [20:16]) = dst code, src=PIN also sets PINctl bit4; mode1 → src word [4:0] = dst code, PINctl bit0 = 1 if src=PIN else 0; mode2 = no-op (MCLK).
i2sR args = {src, dst, b0, b1, b2}: 0x330303 ⇒ b0=3 (src node mask), b1=3 (dst node mask), b2=0x33 (link mask). Driver passes value = running dirs (bit0 RX, bit1 TX) for b0/b1 and state = (running ? (master ? 2 : 1) : 0) | (MCLK on ? 0x20 : 0) for b2 (0x…86e7f38 → 0x…7e8803c). Bits 0/1 of b2 pick mode1 direction, 4/5 mode2. (G: mode0 = data, mode1 = BCLK/FSYNC.)
Speaker slave TX (G): 0x1c = 0x00180a18 (AUD2 data → PIN2), 0x08 = 0x00181802 (PIN2 clock → AUD2), 0x44 = 0x01; RX too ⇒ 0x08 = 0x00180202, 0x44 = 0x11. Master variant: 0x1c = 0x00180a0a, 0x44 bit0 = 0.

**IRQ status 0x40:** handler (0x…86e922c) only reads/clears mask 0xb0003000; bits 26/10 never used (G: RX/TX FIFO-level / DMA-request status, harmless).

**Exact MCA2 speaker sequence (v1, from 0x…86e729c / 0x…86e8028):**
`0x20a002008 = 0x40|sel` · `0x44 = 3` · `0x48 = 1`, poll bit0 · `0x10 = 0` · `0x1c = 0xd` · `0x30 = 0xd` · `0x08 = 0x1010` · `0x20 = 0x00170018` · `0x34 = 0x00030004` · `0x14 = 0` · `0x24 = 1` · `0x38 = 0xf` · `0x18 = 0x108` · `0x2c = 0x10a` · `0x10 = 1` · `0x40 = 0xb0003000` · `0x0c = 0x0f107000`.
Start: switch update · `0x18 = 0x109` · (`0x2c = 0x10b` if RX) · `0x0c = 0x0f107002`.
Master test (codec-reference style, makes the clock run with no external master): NCO2 = 12.288 MHz, `0x0c = 0x0f107005` → start `0x0f107007`, `0x18 = 0x208` → start `0x209`, switch clock AUD2→PIN2.

### Follow-up (2026-10-17, after first hardware test)
- **PMGR gate → register (C):** addr = `ps-regs[map]` + index×8 (`_setPSLevel` 0x…8525980), from DT `pmgr/devices` {name,id1,map,index,alias=parent}: MCA2 id 66 = **0x20e020140** (parent 4), SIO id 90 = **0x20e0201f0** (parent 4), SIO_P id 4 = **0x20e0201f8**. MCA0–4 = 0x…130–0x…150, all parent SIO_P.
- **NCO ref (C):** nco `clock-ids[0]` = 267 → `clock-frequencies[11]` = 342 857 142 Hz; set-rate 0x…833a430 special-cases that value as 2.4 GHz /7. NCO has no clock/power gate of its own. NCO2 @ 0x20e013000 for 12.288 MHz: CTRL=0 · DIV(+4)=0x4cf · INC1(+8)=0x041eb000 · INC2(+0xc)=0xfefe3000 · ACCINIT(+0x10)=0x7efe3000 · CTRL=0x80001000 (poll CTRL bit30 clear before each write except the final enable).
- **Master clock (C formula / G wiring):** frame generator runs from NCO2 (iOS sets NCO = bclkDiv×fs×frameBits; 0x0c[11:8]=0 ⇒ BCLK = NCO/2). 0x20a002008 only makes the MCLK pin (nclk/1,2,4 or 0x08 = NCO path), bit6 = pad output enable.
- **Shim pull (G):** MCA side has no DMA bit in 0x10/0x44; the candidates are 0x1c (TX) / 0x30 (RX) = 0xd, written by iOS right after reset (0x…86e7430, reset 0x4) and missing from the SIO-PROTOCOL.md minimal sequence; and 0x20 low half = DMA burst (`MCA_dma_burst_size`, 0x18).
- **Switch (C coupling / G semantics):** a mode1 route with src=PINn always sets word[PINn][4:0] = AUD code **and** PINn-ctl (0x3c+4n) bit0; mode0 with src=PINn sets word[PINn][12:8] **and** PINn-ctl bit4. Amp test (ADSPCLK_ERR only with PIN2 [4:0]=2) ⇒ word[X] = *source feeding X*. Master TX: 0x20a003008 = 0x00180202, 0x20a003044 = 0x11, 0x20a00301c [4:0] = 0x18 (do not take AUD2's clock from PIN2); add AUD2 [12:8]=0x0a only for RX sense.
