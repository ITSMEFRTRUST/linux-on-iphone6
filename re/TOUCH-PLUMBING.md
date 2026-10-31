# iPhone 6 (N61) touchscreen: ADT plumbing -> Linux

Read-only research. Nothing was run on the phone. Tags: **[C]** = read from the iOS 12.5.5 kernelcache disassembly (`re/kc.macho`, VAs given), **[D]** = read from `re/dtree.txt` / `re/live-adt.txt`, **[K]** = read from the Hoolock tree, **[G]** = guess.

## 0. Biggest findings

1. The touch IC is **not a Z2**. The N61 personality (`AppleMultitouchSPI - C1,N61`, in the kernelcache's embedded Info.plist) has `IOClass = AppleMultitouchN1SPI`, `IONameMatch = multi-touch,n61`, `mt-merge-personality = C1F59,1`, `reset-deassert-delay = 0xf`, `fll-mval 0x17d3`, `fw-execute-addr 0x10003400`, `clk32-clock-enable-addr 0x10003518` (val 1), `cal-dl-addr 0x10009000`, `prox-cal-addr 0x10009600`. So the chip has a memory-mapped RAM that the host fills (firmware, calibration), then jumps into. The kext has `MTSPIBootloader_N1`. `drivers/input/touchscreen/apple_z2.c` (compatibles `apple,j293-touchbar`/`j493-touchbar`, HBPP 0x3001 blobs) cannot be reused as is; a new N1 driver is needed. Firmware should be in the `AppleMultitouchSPIC1F57` kext (kc 0xfffffff008712a90); another session has `re/mtfw/` started.
2. The A8 SPI block is the same Apple/Samsung IP family as M1 and looks **register-compatible with `drivers/spi/spi-apple.c`** (the iOS `AppleSamsungSPI` kext, kc 0xfffffff007e8b160, touches `+0x13c`, `+0x134`, `+0x130`, `+0x150` = IF_FIFO/IF_XFER/IE_XFER/SHIFTCFG of spi-apple.c). `CONFIG_SPI_APPLE=y` is already in `config-n61`. t7000.dtsi has **no spi nodes yet**; `ps_spi2` already exists in `t7000-pmgr.dtsi`.
3. iBoot **already copies the calibration into the ADT**: live-adt.txt:2169 `multi-touch-calibration` is a real 0x400-byte blob (device-specific), live-adt.txt:2148 `prox-calibration` is 0x80 bytes. The `syscfg/MtCl/0x400,zeroes/0x400` text in dtree.txt:2040 is just the IPSW template iBoot resolves against SysCfg (NOR). m1n1 only has to forward it.

## 1. ADT providers and what the function arguments mean

| phandle | node | evidence | provider |
|---|---|---|---|
| 19 | `/arm-io/pmgr` | dtree.txt:284 (node), 1571 (`AAPL,phandle: 19`) | ApplePMGR / AppleT7000PMGR: SoC power manager, MMIO 0x20e000000 |
| 24 | `/arm-io/gpio` | dtree.txt:1622, 1627 (`compatible gpio,t7000`, `#gpio-pins: 208`, reg 0xe300000) | AppleS5L8960XGPIOIC |
| 73 | `/arm-io/i2c0/pmu` | dtree.txt:2212-2217 (`pmu,d2186`, reg addr 0x74) | AppleD2186PMU, the main PMU on i2c0 @0x74 |
| 75 | `/arm-io/i2c0/display-pmu` | dtree.txt:2246-2253 (`display-pmu,chestnut`, addr 0x27) | AppleChestnutDisplayPMU, the display bias/LDO chip on i2c0 @0x27 (backlight `lm3534` is separate, @0x63) |

Function-name decode (function = `phandle:FourCC(args)`, FourCC stored byte-reversed in the raw ADT, e.g. `KLCT` = TCLK):

- **`19:TCLK(0x8, 0x64, 0x8000)` = `ApplePMGRFunctionEnableTouchClock`** [C] (initWithTargetDataAndSymbol kc 0xfffffff008534bf4, callFunction 0xfffffff008534b7c; calls `ApplePMGR::enableTouchClock(bool en, UInt32 delay_us, UInt32 divider)` @0xfffffff00852b234). Args: `0x8` = delay in us during enable, `0x64` = 100 us delay on disable, `0x8000` = target Hz; divider = 24 000 000 / 0x8000 = **732** (0x2dc). So it is a **32.768 kHz clock output from the 24 MHz ref**, for the touch IC. Register: PMGR "TOUCH" reg group, bit31 = gate, bit19 = enable, bit18 = busy/status, [9:0] = divider (the string `PMGR_TOUCH_CTL_CLK_DIVISOR_SMASK`, divider max 0x3ff). Enable = clear bit31, delay(8us), write divider|bit19, poll bit18 clear. Disable = clear bit19, poll, delay(100 us), set bit31. **The TOUCH register's offset is not resolved** (the group table at kc 0xfffffff0075f9f88 is filled at runtime; not decoded). Not the "100 Hz" guess.
  The N61 personality also writes `clk32-clock-enable-val=1` into device address 0x10003518, so the chip has a separate internal 32k enable.
- **`73:pmuL(0x20e)` = `AppleD2186PMULDOFunction`** [C] (class vtable kc 0xfffffff0075280e0; init 0xfffffff007ff68f4; call 0xfffffff007ff67f8; `_setLDO` 0xfffffff007ff534c; per-LDO table at 0xfffffff007051b48, 12 bytes/entry). Arg bytes `0e 02 00 00`: byte0 = LDO index 0x0e = **"ldo15"** (printed as index+1), byte1 = 0x02 flags (bit1 = fixed voltage, enable-only; bit0 = enabled at init = 0, so iOS's PMU driver explicitly switches it OFF at start-up), bytes2-3 = voltage/options 0. Table entry 14: **enable = PMU register 0x0085 (16-bit register address), mask 0x02**. So touch power = `pmic@74 reg 0x0085 |= 0x02`. No voltage control. (Neighbours: ldo16 = reg 0x85 bit 2 = `oscar_power1` pmuL(0x20f); ldo14 = reg 0x85 bit 0.)
- **`75:dpLE(0x302)` = `AppleDisplayPMUFunctionLDOEnable`** [C] (kc 0xfffffff0084ae324 -> `AppleChestnutDisplayPMU::setLDO` ~0xfffffff0084ae788). Arg bytes `02 03 00 00`: LDO select 2, options 3 (bit0 + bit1). `setLDO` does an I2C read of **chestnut reg 0x05**, sets bit `0x4 << ldo` = **0x10** for LDO2 (LDO1 = 0x08 = the LCD, `lcd_ldo dpLE(0x301)` dtree.txt:2408), plus refcounted shared bits: option bit0 -> `|= 0x03`, option bit1 -> `|= 0x4 << sage-ldo` (=0x04). Then writes it back. The touch "analog" supply is Chestnut LDO2: **i2c0 @0x27, reg 0x05 |= 0x10 (|0x07 if not already set by the LCD's LDO1)**. 8-bit register, so `tools/i2c` works as is.
- Power-on order in the kext (`callPowerFunction`, kc 0xfffffff007f5dcbc) [C]: `power_ana` (Chestnut) -> optional `power-ana-enable-delay-us` (absent on N61 -> 0) -> `power_ldo` (D2186). Off = `power_ldo` first, then `power_ana`. Other kext functions: `enable_cs`, `clock_enable` (called with arg 3 on enable), `reset` (reset "ensuring S_CLK is high" before release), `reset-deassert-delay` = 0xf (ms, from plist).

## 2. What Hoolock has today [K]

- **PMU**: `t7000-6.dtsi:62-91` has `pmic@74` as `apple,adi-pmic` + `apple,i2c-pmic` (simple-mfd-i2c, `regmap_config_16r_8v` = 16-bit reg, 8-bit value, drivers/mfd/simple-mfd-i2c.c:37,75). Subdevices today: rtc, nvmem only. **No regulator driver for this PMU** (`drivers/regulator` has none; `apple,pmic.yaml` has no regulator child). n61.dts says so ("PMU rails, not described yet") and uses `regulator-fixed` placeholders.
- **Display PMU (0x27)**: no node, no driver (display works because iBoot enabled it).
- **GPIO**: `pinctrl: pinctrl@20e300000` `apple,t7000-pinctrl`, `#gpio-cells=<2>`, `gpio-ranges = <&pinctrl 0 0 208>`, interrupt-controller with `#interrupt-cells=<2>` (t7000.dtsi:325-370). **ADT pin number = Linux pin number 1:1**: `function-iic_scl GPIO(0x61)` = pin 97 = `i2c0_pins APPLE_PINMUX(97,1)`; buttons 0x20/0x21 = gpio-keys 32/33. So reset = `<&pinctrl 2 ...>`, IRQ = `<&pinctrl 3 ...>`, CS = pin 185 (0xb9).
- **spi2**: missing. Pattern to copy: t8103.dtsi:581 (`apple,t8103-spi`, `apple,spi`), `ps_spi2` at `t7000-pmgr.dtsi:202`. The apple,spi binding has no t7000 compatible yet.
- **SPI clock**: t7000 nodes use `clocks = <&clkref>` (24 MHz) for uart/i2c ("bootloader-enabled clocks"). ADT spi2: `clock-gates [76]`, `clock-ids [302]` = PMGR device "SPI2" id 76 (dtree.txt:1033) = `ps_spi2`. Use `&clkref` first; spi-apple divides it (24 MHz ref -> max 12 MHz).
- **Z2 driver**: `apple_z2.c` exists but wrong chip (see 0.1); `CONFIG_INPUT_TOUCHSCREEN` is **not set** in config-n61 (line 2023).
- **DMA**: ADT spi2 has SIO `dma-parent 35`; use PIO (spi-apple is PIO only).

## 3. GPIO flag decoding [C]

`AppleS5L8960XGPIOFunction` (kc vtable 0xfffffff007618438; init 0xfffffff00863cba4, call 0xfffffff00863ca5c). Data = `{phandle, 'GPIO', pin, flags}`; flags bytes:
- **byte0** = pin mode at init: 1 = output, else raw HW mode (0 input, 2 = input+IRQ high ...).
- **byte1** = **active-high (1) / active-low (0)**. Writes are `val ^ byte1 ^ 1`.
- **byte2** = initial *logical* value (1 = asserted).
- **byte3** = alternate/sleep mode.

So: `reset GPIO(0x2, 0x10001)` -> byte0=1 out, byte1=0 **ACTIVE_LOW**, byte2=1 asserted at init (pin held low = in reset). Linux: `reset-gpios = <&pinctrl 2 GPIO_ACTIVE_LOW>`.
`enable_cs GPIO(0xb9, 0x101)` -> output, active-high, initially 0. The kext calls it with 1 ("enabling cs", after a 5 us delay) before power/reset, i.e. it drives pin 185 high (CS idle) as a plain GPIO. `spi_cs0 GPIO(0xb9, 0x10001)` -> output, active-low (the SPI driver chip-selects with this **GPIO function**, no `internal-cs` property, so software CS). Linux: `cs-gpios = <&pinctrl 185 GPIO_ACTIVE_LOW>`.
Consistent with the CS35L19 patch: `function-reset GPIO(0x31, 0x10001)` is also active-low; the n61.dts uses `GPIO_ACTIVE_HIGH` there only because the cs35l33 driver inverts (it drives 1 = run). The same trap does not apply to a new driver using standard `gpiod` semantics.

**Interrupt specifier `[pin flags]`** [C] (`AppleS5L8960XGPIOIC`, config fn kc 0xfffffff00863c704): flags bit0 = level(1)/edge(0), bit1 = high/rising(1) vs low/falling(0), bit2 = both edges. Check: HW mode = {lvl,hi}->2, {lvl,lo}->3, {edge,rise}->4, {edge,fall}->5, {both}->6 = exactly `REG_GPIOx_IN_IRQ_*` of pinctrl-apple-gpio.c:49-54.
- touch `interrupts [3 0]` -> **edge, falling** -> `IRQ_TYPE_EDGE_FALLING`.
- Bonus: PMU `[106 1]`, CS35L19 `[47 1]`, codec `[127 1]` are all **level, low** = `IRQ_TYPE_LEVEL_LOW` (resolves the open polarity note in CS35L19.md / patch 0005).

## 4. Calibration

- Source: iBoot reads SysCfg (NOR flash) keys `MtCl` (0x400 bytes) and `PxCl` (0x80) and writes them into `multi-touch-calibration` / `prox-calibration` of `/arm-io/spi2/multi-touch` before handing over the ADT [D, live-adt.txt:2148,2169]. Present and non-zero on this unit.
- Hoolock's m1n1 (`dl/m1n1/m1n1.bin` strings; upstream kboot.c `dt_set_multitouch`, Asahi m1n1 src/kboot.c:884) only handles `apple,j293` (`/arm-io/spi0/multi-touch`) and `j493` (`/arm-io/spi3/touch-bar`), looks up the FDT node by alias `touchbar0`, and does `fdt_setprop(dt, node, "apple,z2-cal-blob", blob, len)`. **It does not cover N61.** Needed: a small m1n1 patch, e.g. in `dt_set_multitouch()`: `else if (fdt_node_check_compatible(dt, 0, "apple,t7000") == 0) adt_touchbar = "/arm-io/spi2/multi-touch";`, copy both `multi-touch-calibration` -> `apple,mt-cal-blob` and `prox-calibration` -> `apple,mt-prox-cal-blob`, and add alias `touchbar0 = &touchscreen` in the dts (or a separate alias). Source: https://github.com/HoolockLinux/m1n1 (needs a rebuild; not done here).
- The iOS driver downloads cal to device RAM at 0x10009000 and prox cal at 0x10009600.

## 5. Is the touch supply on at boot? (not run, propose)

Likely **off or undefined**: iOS's PMU driver switches ldo15 off at init (flags bit0 = 0), iBoot has no use for touch in a normal boot. Check, read-only:
- Chestnut (8-bit regs, works with existing tool, display is lit so it answers): `tools/i2c 0 27 r 05` -> bit 0x10 set = touch analog on. (Expect 0x0f for LCD LDO1 only.)
- D2186 reg 0x0085 needs a **16-bit register address** (regmap is 16r_8v, big-endian address bytes), which `tools/i2c` cannot do (it writes one address byte). Options: (a) `i2ctransfer -y 0 w2@0x74 0x00 0x85 r1@0x74` if busybox/i2c-tools is in the initramfs (read-only; `-f` not needed, the kernel's pmic@74 is bound but i2c-dev does not need to unbind); (b) add an `R HHLL` 2-byte-address read command to `tools/i2c.c` (write 2 bytes, then read 1; same style as `rd()`). Bit 1 of the result is the touch LDO.
- GPIO/pin state, read-only with `tools/mem`: reset/IRQ pins `tools/mem r 20e300008 2` (pin 2 then 3; reg = 0x20e300000 + 4*pin; bit0 data, bits3:1 mode, bits6:5 periph func), CS `tools/mem r 20e3002e4 1`. To find the real SPI2 CLK/MOSI/MISO pins, dump `tools/mem r 20e3002c0 16` (pins 176-191): iBoot normally pre-muxes board pins, so pins with PERIPH bits (6:5) != 0 next to 185 are the SPI2 pins. My guess: CLK 182, MOSI 183, MISO 184 [G] (M1 and spi1 pattern: CS is the 4th consecutive pin).

## 6. Draft Linux DT [G where marked]

```dts
/* t7000.dtsi, under soc, next to i2c1 */
spi2: spi@20a088000 {
	compatible = "apple,t7000-spi", "apple,spi";	/* NEW compat in apple,spi.yaml */
	reg = <0x2 0x0a088000 0x0 0x4000>;
	interrupt-parent = <&aic>;
	interrupts = <AIC_IRQ 154 IRQ_TYPE_LEVEL_HIGH>;
	clocks = <&clkref>;			/* [G] ADT: gate 76 / id 302; try 24 MHz ref */
	pinctrl-0 = <&spi2_pins>;
	pinctrl-names = "default";
	power-domains = <&ps_spi2>;
	#address-cells = <1>;
	#size-cells = <0>;
	status = "disabled";
};

/* in &pinctrl */
spi2_pins: spi2-pins {			/* [G] pins; func 1 as for i2c */
	pinmux = <APPLE_PINMUX(182, 1)>,	/* CLK  */
		 <APPLE_PINMUX(183, 1)>,	/* MOSI */
		 <APPLE_PINMUX(184, 1)>;	/* MISO */
};

/* t7000-n61.dts */
&spi2 {
	status = "okay";
	cs-gpios = <&pinctrl 185 GPIO_ACTIVE_LOW>;	/* ADT spi_cs0 GPIO(0xb9, 0x10001) */

	touchscreen@0 {
		compatible = "apple,n61-multitouch";	/* NEW driver, N1 protocol, not apple_z2 */
		reg = <0>;
		spi-max-frequency = <4000000>;		/* [G] start low; ADT reg words 0x1388/0x2710 undecoded */
		interrupts-extended = <&pinctrl 3 IRQ_TYPE_EDGE_FALLING>;	/* ADT [3 0] */
		reset-gpios = <&pinctrl 2 GPIO_ACTIVE_LOW>;			/* ADT GPIO(0x2,0x10001) */
		vdd-supply = <&ldo15>;			/* pmuL(0x20e): PMU reg 0x85 bit1 (NEW regulator driver) */
		avdd-supply = <&dpmu_ldo2>;		/* dpLE(0x302): Chestnut@0x27 reg 0x05 bit4 (NEW) */
		/* 32.768 kHz from PMGR TCLK: NEW clock/gate (24 MHz / 732), or poke from the driver */
		firmware-name = "apple/mtfw-n61.bin";	/* from AppleMultitouchSPIC1F57 kext */
		reset-deassert-delay-ms = <15>;		/* plist reset-deassert-delay 0xf */
		/* apple,mt-cal-blob / apple,mt-prox-cal-blob: filled in by m1n1 (see section 4) */
	};
};

/* t7000-6.dtsi, under pmic@74 (needs a tiny regmap regulator driver: enable_reg 0x0085, mask 0x02) */
regulators {
	compatible = "apple,adi-pmic-regulators";	/* NEW */
	ldo15: ldo15 { regulator-name = "LDO15"; };
};
```
Touch size units (touchscreen-size-x/y) unknown; panel is 750x1334 px.

## 7. Work list (smallest to biggest)

1. Read-only checks in section 5 (confirms supply state and finds SPI pins).
2. spi2 node + spi2_pins + t7000 compat in apple,spi.yaml; enable `CONFIG_INPUT_TOUCHSCREEN`; confirm the SPI core works (loopback-less: read the STATUS regs, or probe a `spidev` temporarily).
3. Regulators: tiny regmap driver for D2186 ldo15 (reg 0x85/bit1) and Chestnut LDO2 (reg 0x05/bit4); or bring-up with `tools/i2c 0 27 w 05 ..` and an i2ctransfer write.
4. 32 kHz touch clock (PMGR TOUCH reg; offset still to find, see section 1 TCLK note).
5. m1n1 patch for the calibration blobs.
6. N1 driver: reverse `MTSPIBootloader_N1` + `AppleMultitouchN1SPI` (device RAM map: fw-execute 0x10003400, cal 0x10009000, prox 0x10009600, fll-mval 0x17d3 at 0x10003060, clk32 enable at 0x10003518), plus firmware extraction (C1F57 kext).
