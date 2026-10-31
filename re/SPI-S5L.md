# A8 (T7000) SPI controller — "spi-1,samsung" / AppleSamsungSPI RE

Source: `com.apple.driver.AppleSamsungSPI` (AppleSamsungIP-128.200.2), iOS 12.5.5 kc.macho.
Kext Mach-O header @ 0xfffffff007e8b160, `__TEXT_EXEC.__text` 0xfffffff007e8b230..0xfffffff007e8e948.
Class `AppleSamsungSPIController`, vtable @ 0xfffffff0074dd590. Static analysis only (no phone access).

Confidence tags: **[seen]** = read directly in code; **[inf]** = inferred (bit name/meaning guessed by
matching M1 bit positions, or by usage); **[hw]** = live reset values you measured.

## Verdict

**`spi-apple.c` cannot drive it as-is, and it is more than a one-line quirk.** The A8 block is the
*older* Samsung-derived Apple SPI: the low register file (0x00–0x4c) matches M1 almost exactly,
but none of the M1 "new" registers (FIFOSTAT 0x10c, IE/IF 0x130–0x13c, SHIFTCFG 0x150, PINCFG
0x154, DELAY 0x160/0x168) are ever touched by iOS, and FIFO levels and IRQ flags live in STATUS
(0x08) instead. The M1 driver would see TX level 0 / RX level 0 forever (FIFOSTAT reads 0 [hw])
and would never see a completion flag, so every transfer would overflow the TX FIFO and then time out.

Recommendation: add an `"apple,s5l-spi"`-style variant (match data) to `spi-apple.c`, or write a
~250-line `spi-apple-s5l.c` that keeps the same structure. Shared code: set_cs, CLKDIV,
RXCNT/TXCNT, CTRL resets, TXDATA/RXDATA. New code: FIFO-level reads, IRQ/poll flag handling,
CFG layout (word size, IRQ enables, the always-set bits), dummy-TX full duplex. **Decision: write the
variant inside spi-apple.c** (the probe, clock and pm code is the same; the hot path is about
5 functions).

## Register map (spi-version 1, which is what all four A8 SPIs report)

| Off | Name (M1 name) | Use on A8 | Conf |
|---|---|---|---|
| 0x00 | CTRL | iOS writes **0xD = RUN\|TX_RESET\|RX_RESET in one write**, every transfer (from ivar 0x174, set at power-on). 0 at power-on and power-off. Never cleared after a transfer. Bit names match M1 (RUN b0, TXRST b2, RXRST b3). | seen (value); inf (names) |
| 0x04 | CFG | see CFG table. Reset 0x8 [hw] | seen |
| 0x08 | STATUS | **W1C IRQ status plus FIFO levels**: b0 RX-complete, b1 TX/RX-threshold, TX level = b[10:6], RX level = b[15:11] (5-bit, 0..16). Cleared by writing 0x40000F (v1; v0 0xF), and the IRQ handler writes back what it read. | seen |
| 0x0C | PIN | b1 = CS level (iOS internal-CS path: assert → clear b1, deassert → set b1). Written to 0 at power-on. Reset 0x3 [hw]. Not used on the A8 because there is no `internal-cs` in the ADT. | seen |
| 0x10 | TXDATA | PIO TX. **iOS always writes 1 byte per FIFO entry (ldrb)**, using 0xFF as filler | seen |
| 0x20 | RXDATA | PIO RX, 1 byte per entry (strb), or dropped | seen |
| 0x30 | CLKDIV | `ceil(nclk * period_ns / 1e9)`; validated ≤ **2048** (M1 caps 0x7FF); v0 forces min 2, v1 allows 1. Reset 1 [hw] | seen |
| 0x34 | RXCNT | transfer length (see flow). Also read back for the debug history | seen |
| 0x38 | WORD_DELAY | `ceil(nclk * worddelay_ns / 1e9)` written on every transfer | seen |
| 0x4C | TXCNT | **v1 only** (v0 has no TXCNT, so on v0 a DMA transfer needs RXCNT≠0) | seen |
| 0x10C, 0x130–0x13C, 0x150, 0x154, 0x160, 0x168 | M1 FIFOSTAT / IE / IF / SHIFTCFG / PINCFG / DELAY | **never accessed** by iOS. 0x100–0x11C read 0 [hw]; on M1 an idle FIFOSTAT has RXEMPTY (b20) set, so this register probably does not exist here | seen (no access); inf (absent) |

Complete list of offsets that iOS accesses (all calls through `readReg` vt+0x648 / `writeReg` vt+0x650 at
ivar 0xF8 = mapped base): R: 0x08, 0x0C, 0x20, 0x34; W: 0x00, 0x04, 0x08, 0x0C, 0x10, 0x30, 0x34, 0x38, 0x4C. **[seen]**

### CFG (0x04), v1
| Bits | Meaning | Conf |
|---|---|---|
| b1 | CPHA (config flag byte+9 bit0) | seen position; inf name (= M1) |
| b2 | CPOL (config byte+8) | seen; inf name (= M1) |
| **b3, b4** | **always set (0x18)**. b3 is set at reset (0x8 [hw]). Unknown in M1 Linux; spi-apple.c clears them | seen; meaning unknown (master/enable?) |
| b6:5 | MODE: 0 idle (between transfers), 1 = 0x20 PIO/IRQ, 2 = 0x40 DMA | seen; matches M1 |
| b7 | IE RX-complete (0x80) | seen; matches M1 |
| b8 | IE TX/RX-threshold (0x100) | seen; matches M1 |
| b13 | LSB-first (set when the config "msb-first" byte+0xA == 0); v0: b11 | seen; matches M1 |
| **b14** | **always set** (v1 0x4000; v0 b12 0x1000). Unknown | seen; meaning unknown |
| b16:15 | word size 0=8, 1=16, 2=32 (shift from table; v0 shift 13). Only 8/16/32 are accepted | seen; matches M1 |
| b18:17 | FIFO threshold: never set (left at 0) | seen |
| b20 | optional, v1 only (config byte+9 bit1). When set, WORD_DELAY is forced to ≥2. Probably CS-pulse between words | seen; inf meaning |

### Version table (`spi-version`, ADT; missing → 0). Loaded into ivars 0x140..0x150 **[seen]**
| field | v0 | v1 (A8) |
|---|---|---|
| word-size shift | 13 | 15 |
| always-set CFG bit | 0x1000 | 0x4000 |
| LSB-first bit | 0x800 | 0x2000 |
| FIFO depth | 8 | **16** |
| STATUS clear mask | 0xF | 0x40000F |
| TX level in STATUS | b[7:4] | b[10:6] |
| RX level in STATUS | b[11:8] | b[15:11] |
| TXCNT reg / min CLKDIV / CFG b20 | no / 2 / no | yes / 1 / yes |

## Clock
`nclk` comes from the provider (`provider->vt+0x550("nclk")`). spi0..3 `clock-ids` 300..303 map to
arm-io `clock-frequencies[id-256]`, which gives **60 MHz** for all four (cross-check: uart 278 → 24 MHz).
**[inf]** for the mapping; the values were parsed from re/live-adt.txt. Gate: `clock-gates [76]` = `ps_spi2` in t7000-pmgr.dtsi.
In Linux this is a 60 MHz fixed-clock, so `DIV_ROUND_UP(60e6, hz)`, the same formula as M1.

## Chip select
- ADT has **no `internal-cs`** on any spi node, so iOS uses the GPIO `function-spi_cs%d`
  (spi2: GPIO 0xB9 = 185, flags 0x10001; multi-touch `enable_cs` uses the same pin, flags 0x101). **[seen]**
- CS setup/hold delay: `ceil(cs_delay_ns/1000)` µs IODelay before deassert and after assert. **[seen]**
- CS stays asserted after a transfer if the command flag (+0x80 bit0) is set (keep-CS). **[seen]**
- Linux: `cs-gpios = <&pinctrl_ap 185 GPIO_ACTIVE_LOW>` (polarity **[inf]**; check flag bit16),
  so spi-core drives the GPIO and never calls `set_cs`. The M1 native set_cs is bit-compatible anyway.

## Transfer flow (PIO, `_executeSPICommandPIO` @ 0xfffffff007e8c584) **[seen]**
1. `len = max(tx,rx)`; `RXCNT = len`; v1: `TXCNT = len`. If rx>tx, the extra TX is filler 0xFF; if tx>rx, the extra RX is dropped.
   **The bus always runs full duplex**: RX-only still pushes 0xFF, and TX-only still drains RX.
2. programHW: `CLKDIV`, `WORD_DELAY`, `STATUS = clear-mask`, `CFG = base` (mode 0), `CTRL = 0xD`.
3. Assert CS (GPIO plus delay).
4. `CFG = base | 0x20` (MODE=1), then prefill TXDATA with up to 16 bytes (data, then 0xFF filler).
5. Poll or IRQ decision: if `len*period_ns` ≤ ~1374 ns·byte (≈11 µs at 8 bits), poll (initial delay = transfer time,
   then 10 polls × 10 µs on STATUS & mask). Otherwise IRQ: `CFG |= 0x100` (threshold) while TX remains, else `|= 0x80` (RX-complete).
   Timeout 5 s.
6. Service routine (IRQ handler / poll): `s = STATUS; STATUS = s` (W1C); `rxlvl = s[15:11]`; if 0, return.
   Read `rxlvl` bytes from RXDATA, then write `16 - s[10:6]` bytes to TXDATA. Loop until all counts reach 0.
   Once TX is fully queued, switch to MODE|IE_RXCOMPLETE (IRQ) or mask = b0 (poll).
7. Done: `CFG = base` (mode 0, IEs off), `STATUS = clear`, deassert CS unless keep-CS.
DMA path (CFG mode 0x40, dma-parent 35, 0xFF-filled dummy page for TX) exists, but Linux does not need it.

## Line-by-line diff against spi-apple.c
| spi-apple.c | A8 | Fix |
|---|---|---|
| L151 init `PIN = CS` | OK (bit1 same) | keep |
| L152-153 SHIFTCFG/PINCFG rmw (0x150/0x154) | regs not used by iOS (probably absent) | drop for variant |
| L156 `CTRL = RXRST\|TXRST` | OK | keep |
| L159 `CFG = MODE_IRQ\|8B` (0x20) | **clears b3/b4/b14, which iOS always sets** | base = `0x4018`, OR in mode/IE per phase |
| L165-170 IE_FIFO/IE_XFER/DELAY writes | not present | drop |
| L178-184 CPHA b1 / CPOL b2 / LSB b13 | same bits on v1 | keep |
| L193 set_cs PIN b1 | same (unused: GPIO CS) | keep |
| L201-202 CLKDIV `DIV_ROUND_UP`, max 0x7FF | same reg; HW max 2048 | keep (cap harmless) |
| L205 SHIFTCFG_BITS = bpw | **no SHIFTCFG**; word size is CFG[16:15], 8/16/32 only | set CFG word size; `bits_per_word_mask = 8\|16\|32` |
| L214 poll heuristic, FIFO_DEPTH 16 | depth 16 on v1 | keep |
| L221-222 IRQ: IF/IE_FIFO, IF/IE_XFER | **absent**; flags = STATUS b0/b1 (W1C); enables = CFG b7/b8 | rewrite irq: `s=STATUS; STATUS=s; if (s&3) …`; clear CFG IEs |
| L235-262 wait on IF regs; IE via 0x130/0x138 | same | poll STATUS & {b0,b1}; enable via CFG b7/b8 |
| L275 TX level = FIFOSTAT[15:8] | **STATUS[10:6]** | change |
| L320 RX level = FIFOSTAT[31:24] | **STATUS[15:11]** | change |
| L383 FIFO reset before each xfer | iOS combines it with RUN (0xD) | either; keep M1 order |
| L386-387 IF clears (~0) | use `STATUS = 0x40000F` | change |
| L390-393 wait flags TXCOMPLETE/RXCOMPLETE | only RX-complete (b0) is used. A TXCOMPLETE flag (M1 b2) is never used by iOS | wait for b0 with RXCNT = len always |
| L396-397 `TXCNT = tx?words:0`, `RXCNT = rx?words:0` | iOS: both = len, dummy 0xFF TX, drain RX | **always full duplex**: feed 0xFF when !tx_buf; read and discard when !rx_buf |
| L403 `CTRL = RUN` after prime | iOS: RUN before prime, transfer gated by CFG MODE=1 | M1 order is likely fine **[inf]**; fall back to iOS order if it stalls |
| L448-449 TXOVERFLOW/RXUNDERRUN from IF_FIFO | not available | drop |
| L452 `CTRL = 0` | iOS leaves RUN set, but sets CFG mode = 0 | do both |
| — | WORD_DELAY 0x38 never written by M1 | write 0 (reset is 0 [hw]) |
| L513 compatible | ADT `spi-1,samsung`, spi-version 1 | new compatible e.g. `apple,t7000-spi`, `apple,s5l-spi` |

Open questions to settle on hardware (one test each): what CFG b3/b4/b14 do (can they be cleared?);
whether RXCNT=0 TX-only works on v1 (the DMA asserts suggest it is allowed); whether a 16-bit word is one FIFO entry
(iOS PIO only ever uses bytes); the meaning of STATUS b22 in the clear mask.
