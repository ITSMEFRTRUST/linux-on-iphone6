# iPhone 6 (N61) touch IC: power, clock and reset, register level

Static RE only, from the iOS 12.5.5 kernelcache (`re/kc.macho`). Nothing was run on the phone. This follows on from `TOUCH-PLUMBING.md` and settles the open items there. The SPI/N1 protocol is covered in `MULTITOUCH.md`, not here.
Tags: **[C]** seen in code (VA given), **[D]** device tree / plist, **[I]** inferred, **[G]** guess.

## 1. PMGR `TCLK` = 32.768 kHz touch clock

- `ApplePMGRFunctionEnableTouchClock::initWithTargetDataAndSymbol` (0xfffffff008534bf4) [C]: data must be 5 words, `{phandle, 'TCLK', a0, a1, a2}`. It stores `a0` = **enable delay in us** (8), `a1` = **disable delay in us** (0x64 = 100) and `24000000 / a2` = **divider** (24 MHz / 0x8000 = **732 = 0x2dc**). So `a2` is the target frequency in Hz (32768).
- `callFunction` (0xfffffff008534b7c) [C]: with `*arg3 == 3` it calls `enableTouchClock(1, 8, 732)`; with `*arg1 == 0` it calls `enableTouchClock(0, 100, 0)`. The MT kext calls it exactly that way (arg3 = &3 to enable, arg1 = &0 to disable, 0xfffffff007f657a4).
- **Register** [C]: `ApplePMGR::enableTouchClock` (0xfffffff00852b234) uses `_regGroups[kRegGroupTouch]` (group index 1, struct stride 0x18 at this+0xc90). `AppleT7000PMGR::initRegGroups` (0xfffffff008537a38) fills group 1 with `initRegGroup(1, map 0, 0x1e000)` (0xfffffff008537a5c). RegMap 0 = ADT pmgr `reg[0]` (`initRegMap(0, 0)` -> `mapDeviceMemoryWithIndex(0)`, 0xfffffff008520858) = 0x20e000000 (dtree.txt:1298 `0xe000000`, arm-io +0x200000000).
  **TOUCH_CTL = 0x20e01e000** (inside Hoolock's pmgr syscon range 0x20e000000 + 0x24000).
- Bits [C]: [9:0] divider (1..0x3ff, `PMGR_TOUCH_CTL_CLK_DIVISOR_SMASK`), bit19 = enable, bit18 = busy (polled), bit31 = gate (1 = gated). The other bits are preserved.
- **Enable** [C]: `v = rd`; `wr(v & 0x7fffffff)`; IODelay(8 us); `wr((v & 0x7ffffc00) | 0x2dc | 0x80000)`; then poll with IODelay(1 us) until bit18 = 0.
- **Disable** [C]: `v = rd`; `wr(v & ~0x80000)`; poll until bit18 = 0; `wr(that | 0x80000000)`; IODelay(100 us).
- (`setTouchClockSource`, map 1 offset 0x40324 bit0, exists but N61 has no `TSRC` function. It is not used.)
- **Other gates** [D]/[I]: the multi-touch node has no `clock-gates`. spi2 has `clock-gates [76]` = `ps_spi2` (0x20e020190, parent `ps_sio_p` 0x20e0201f8) and `clock-ids [302]` (an SPI functional clock mux that iBoot leaves set up). Nothing else is touch-specific. TOUCH_CTL is a plain PMGR control register, not a PS register. ps_spi2 only matters for SPI transfers, not for powering the IC.

## 2. D2186 `pmuL(0x20e)` = LDO15 (VDD)

- `AppleD2186PMULDOFunction` init/call (0xfffffff007ff68f4 / 0xfffffff007ff67f8) [C]: data byte8 = LDO index 0x0e (ldo15), byte9 = flags 0x02 (bit1 = fixed rail, enable-only; bit0 = 0, so iOS switches it **off** at driver init), halfword10 = voltage 0. `callFunction` -> `_setLDO(14, voltage 0, options 0, enable=*arg1)`.
- `_setLDO` (0xfffffff007ff534c) [C]: voltage 0 means no voltage write. For options 0 it does a read-modify-write of `table[idx].enreg` with `table[idx].enmask`. The table is at 0xfffffff007051b48, 12 bytes per entry: `{u16 vmin_mV, u8 step, u8 nsteps, u8 vmask, u8 ?, u16 vreg, u16 enreg, u8 enmask, pad}`. Entry 14: `enreg 0x0085, enmask 0x02`, all voltage fields 0 = **no voltage register**. There are **no delays** in `_setLDO`.
  - The neighbours in reg 0x0085 are ldo14 0x01, ldo16 0x04 (`oscar_power1`), ldo17 0x10 and ldo18 0x20. Live 0x0085 = **0x37** = exactly those five masks, so **LDO15 is ON** now (iBoot/OTP left it on). [C + live]
  - ldo1-13 use enable regs 0x82/0x83 and voltage regs 0x308..0x368, for reference.
- LDO15 has no other ADT users (only `multi-touch` uses `pmuL(0x20e)`) [D].

## 3. Chestnut `dpLE(0x302)` = display-PMU LDO2 (AVDD)

- `AppleDisplayPMUFunctionLDOEnable::callFunction` (0xfffffff0084ae324) -> `AppleChestnutDisplayPMU::setLDO(ldo=2, options=3, -, enable)` (0xfffffff0084ae788) [C]. LDO must be 0..3 and the bit is `0x04 << ldo`. The I2C read and write are a 1-byte register 0x05 plus 1 byte of data. **No voltage register** (the voltage arg is never read). **No delay.**
- Enable [C]: `r = rd(0x05)`. If `r & 0x10` it returns. Otherwise `r |= 0x10`. Then, refcounted: options bit0 -> boost, `|= 0x03` if the boost refcount was 0. Options bit1 -> sage, `|= 0x04 << sage-ldo` (ADT `sage-ldo = 0` -> 0x04) if the sage refcount was 0. It writes only if the value changed. `handleStart` seeds both refcounts from the live reg 0x05 (bits 0-1 -> boost, bit2 -> sage).
- Disable [C]: clears 0x10 and decrements the refcounts. It clears 0x03 / 0x04 only when a count drops to 0 (the LCD's `dpLE(0x301)` holds them).
- Live 0x05 = **0x0f** = boost(0x03) + sage(0x04) + LCD LDO1(0x08). **Touch AVDD (0x10) is OFF.** So **on = write 0x1f, off = write 0x0f**. Never clear bits 0-3, or the panel goes dark.

## 4. iOS sequencing (AppleMultitouchZ2SPI methods; AppleMultitouchN1SPI inherits them)

The class check is from the metaclass: the N1 vtable is at 0xfffffff007513318 and the Z2 vtable at 0xfffffff007512738. N1 overrides only slots 0x2a8, 0x580, 0x908 and 0x910 [C].

**Power on** (`setPowerEnabled(true)` 0xfffffff007f5df44 -> `callPowerFunction(1)` 0xfffffff007f5dcbc -> slot 0x740 = 0xfffffff007f652f4) [C]:
1. If less than **200 ms** have passed since the last power-off, IOSleep for the remainder (slot 0x780 = const 200).
2. `power_mamba` (absent) -> **`power_ana`** (Chestnut LDO2 on) -> IODelay(`power-ana-enable-delay-us`, absent = 0) -> **`power_ldo`** (LDO15 on).
3. slot 0x8b8 `enable_download` (absent on N61, no-op).
4. `enable_cs(1)`: **IOSleep(5 ms)**, then GPIO 185 -> logical 1 = **high** (`GPIO(0xb9,0x101)`, active-high, so CS idle/deasserted).
5. `clock_enable(1)`: the TCLK enable from section 1.
6. **IOSleep(15 ms)** (slot 0x788 = const 0xf).
7. N1-only slot 0x908 (0xfffffff007f6ad3c): an SPI write of 2 bytes `19 c1` ("sending REQ_WAKEUP"), then **IOSleep(10 ms)**. Reset is still asserted at this point.
8. `display_sync(1)` (absent).
9. `setReset(deassert)` (0xfffffff007f651b0): first "ensuring S_CLK is high", which is a 16-byte dummy SPI transfer `1a a1` + 7x `18 e1`. Then reset GPIO 2 is deasserted (active-low, so it goes **high**), then **IOSleep(reset-deassert-delay = 15 ms)** (plist 0xf; the code default is 2). There is no `reset-deassert-high-z` in the N61 personality.
10. Bootload (firmware download) is scheduled after 15 ms. See MULTITOUCH.md.

**Power off** (slot 0x728 = 0xfffffff007f65140, runs before the power function) [C]: `setReset(assert)`: GPIO 2 goes **low**, then IOSleep(1 ms) -> `display_sync(0)` -> `enable_cs(0)` (GPIO 185 low, no delay) -> `clock_enable(0)` (TCLK disable) -> `callPowerFunction(0)`: **`power_ldo` off first, then `power_ana`** (then mamba).
IOSleep = 0xfffffff007cf8040 (ms scale 1e6) and IODelay = 0xfffffff007cf8110 (us scale 1e3) [C].

## 5. Raw test sequence from Linux (not run)

Pins (ADT pin = Linux line): reset 2 (active-low), IRQ 3, CS 185. `tools/gpio` drives a line and releases it on exit. `gpiochip_generic_free` does not touch the pad register, so the level stays [I]. Check it with `/tmp/mem r` (pin reg = 0x20e300000 + 4*pin: bit0 data, bits3:1 mode, 1 = out). Fallback without the gpio tool: RMW the pin reg, `(R & ~0xf) | 0x2` = out-low, `| 0x3` = out-high.

```sh
# 0. read-only snapshot
/tmp/i2c 0 27 r 05            # expect 0f  (touch AVDD off)
/tmp/i2c 0 74 R 0085          # expect 37  (LDO15 on)
/tmp/mem r 20e01e000          # TOUCH_CTL = V   (bit31 gate, bit19 en, [9:0] div)
/tmp/mem r 20e300008 2        # pins 2,3
/tmp/mem r 20e3002e4          # pin 185
# 1. hold the IC in reset, CS low
/tmp/gpio 2=0 185=0
# 2. clean power cycle (LDO15 is on without AVDD today), keep >=200 ms off
/tmp/i2c 0 74 W 0085 35 s 200 # 0x37 & ~0x02
# 3. power on in iOS order: AVDD, then VDD
/tmp/i2c 0 27 w 05 1f         # 0x0f | 0x10
/tmp/i2c 0 74 W 0085 37       # 0x35 | 0x02
# 4. CS high after 5 ms
/tmp/gpio s 5 185=1
# 5. 32 kHz clock (example for V = 0x80000000; in general use V&0x7fffffff, then (V&0x7ffffc00)|0x802dc)
/tmp/mem w 20e01e000 00000000 s 1 w 20e01e000 000802dc r 20e01e000   # bit18 must read 0
# 6. 15 ms, (iOS sends REQ_WAKEUP 19 c1 + 10 ms here; needs spi2, skip), release reset, 15 ms
/tmp/gpio s 25 2=1 s 15
/tmp/gpio g 3                 # IRQ line: watch for the bootloader pulling it low [I]
```

Power off (the inverse, iOS order):
```sh
/tmp/gpio 2=0 s 1 185=0                                   # assert reset, 1 ms, CS low
/tmp/mem w 20e01e000 000002dc r 20e01e000 w 20e01e000 800002dc   # clear en, check bit18=0, gate
/tmp/i2c 0 74 W 0085 35                                   # LDO15 off first
/tmp/i2c 0 27 w 05 0f                                     # then AVDD off (keep LCD bits 0-3!)
```

Notes and risks:
- Always RMW from the step-0 values. A blind `w 05 00` blanks the LCD. Writes to 0x0085 also hit ldo14/16/17/18 (camera/oscar rails).
- The I2C writes to 0x74 go behind the bound `simple-mfd-i2c` (I2C_SLAVE_FORCE). That is harmless for this register, because the regmap is uncached [I].
- Without REQ_WAKEUP and the dummy SCLK transfer, the N1 may still come out of reset into its ROM bootloader [G]. The full sequence needs spi2 up (ps_spi2 on, pins muxed), see TOUCH-PLUMBING §6.
- For a Linux driver: `regulator-fixed`-style gates for Chestnut 0x05 bit4 and D2186 0x0085 bit1, plus a gate+divider clock at pmgr+0x1e000 (enable bit19, gate bit31 inverted, div [9:0], busy bit18).
