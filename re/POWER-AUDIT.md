# iPhone 6 Linux power audit (2026-10-20, live phone, kernel 7.3.0-rc1-gf689d352d4a8, autoboot m1n1-als)

Power-audit agent. Read-only plus reversible runtime knobs, all restored. Tags: **[M]** measured on the phone, **[I]** inferred.
All currents come from the bq27540 gauge (`battery/current_now` = AvgCurrent, about a 1 s average) while the phone runs from
the battery: the SN2400 input limit is set to 0 and USB data stays up. Runner: `tools/pa_meas.sh` (detached, restores on
EXIT/HUP). Each config gets a 6 s settle plus 40 s of 2 s samples, so n=20.

## Bottom line

| state (what the user sees) | mA [M] | battery life at 1743 mAh |
|---|---|---|
| screen "off" (power button = backlight 0, UI running), Wi-Fi + Tailscale + BT up | **~225** | **~7.7 h** |
| same, Wi-Fi link down | 172 | ~10 h |
| screen on, idle UI, brightness 1800/2047 (Orchard sets **performance** governor) | **503** | **~3.5 h** |
| screen on, idle, brightness 2047, performance | ~690 (436 + 250) [I from M parts] | ~2.5 h |
| screen on, brightness 1800, schedutil | 341 | ~5 h |
| one core busy, performance (scrolling / page load) | ~930 | ~1.9 h |

For comparison, iOS on this phone idles at roughly 5-10 mA with the screen off (deep sleep) [I, typical figure, not measured here].
**Linux never really idles.** It has no cpuidle state beyond plain WFI, no DPMS, and no suspend, so the "screen off" floor is
about 20-40x what iOS draws.

### Top drains, ranked

1. **The always-on SoC/display floor: 172 mA [M]** with backlight 0, Wi-Fi link down, BT radio off and the CPU at 300 MHz.
   The total is measured. The split below is inferred, because no reversible knob exists for these parts:
   - CPU cores in shallow WFI only (cpuidle `current_driver = none`) [M]. One core busy at 300 MHz costs +90 mA over WFI
     [M]. A WFI core at 300 MHz is not power-gated, and its cost relative to a gated core is not measured. The performance
     result (item 2) shows that idle WFI power depends heavily on voltage/frequency, which means WFI is shallow. Estimate:
     20-60 mA for both cores [I].
   - Display pipe + DSI + LCD panel keep scanning out at 60 Hz with backlight 0. `card1-DSI-1/dpms = On`, genpd `disp0`,
     `mipi_dsi`, `disp_busmux` on [M]. `adpt_crtc_atomic_disable` deliberately leaves VFTG running (adp-t7000.c:491). The
     scanout reads about 240 MB/s from DRAM, which keeps the memory controller out of self-refresh. Estimate: 30-60 mA [I].
   - The BCM4345 stays powered on PCIe with the link in L0 and no ASPM (`wifiup off` leaves the modules loaded), plus the
     BT chip powered with its radio off, the touch IC scanning, and the USB PHY/controller in HS device mode. USB is a test
     artefact: unplugged, it would suspend. Estimate: 20-50 mA [I].
   Fixes (kernel agent): (a) a cpuidle driver with Apple deep-WFI / core power-down (CYC_OVRD-style). (b) Real DPMS off:
   stop VFTG, put the panel to sleep with DSI 0x28/0x10, then power-gate disp0/mipi_dsi, so DRAM can self-refresh.
   (c) After those, s2idle so the screen-off state actually sleeps.
2. **Orchard forces `performance` whenever the screen is on: +210 mA [M]** at idle (performance 436 mA vs schedutil/powersave
   ~226 mA, both at backlight 0). At brightness 1800 it is +162 mA (503 vs 341). The cause is `sys_cpu_perf(true)` in
   `shell.c:screen_set()`, which writes `performance` to every policy, so both cores sit at 1392 MHz the whole time the
   screen is on. One busy core at 1392 MHz costs ~930 mA total [M]. **This is the largest drain while someone is using the phone,
   and the cheapest fix:** keep schedutil (optionally raise the minimum or boost only during touch/animation via
   `scaling_min_freq` for ~1 s after input). UI agent.
3. **Backlight: up to +250 mA [M].** The DWI code is perceptual (exponential). Measured extra over backlight 0: 200/600/1024
   → ~0 mA (within noise), 1536 → +45, 1800 → +115, 2047 → **+250**. Everything above ~1500 is expensive. A default of max/2
   (1023) is almost free. Suggest capping auto/max at ~1800 and keeping the "screen off" path at 0, which it already is.
4. **Wi-Fi associated: +45-50 mA [M]** average with power save on (base ~222 vs `wifiup off` 175). Power save is on by
   default and works: turning it off costs **+69 mA** (290 mA, flat) [M]. The samples are bimodal, ~172 vs ~270-300 mA: the
   radio is awake about 35-40% of the time. Causes [M]: AP DTIM period 1 (beacon interval 100 ms), and ~2.4 packets/s on
   wlan0 (≈1.3 rx + 1.1 tx/s, mostly tailscaled's WireGuard/DERP/disco keepalives; tailscale0 itself carries no traffic).
   brcmfmac uses PM_FAST (PM2), which stays awake ~200 ms (fw default pm2_sleep_ret) after every packet [I].
   Fixes [I]: when the screen is off, switch to PM_MAX (PM1) or lower `pm2_sleep_ret`, and set `bcn_li_dtim` (listen
   every 3rd DTIM) in brcmfmac. Fewer tailscale keepalives also help.
5. **Tailscale: ~+15-20 mA [M, low confidence]** (tsoff 200 mA vs base 218/227; that is within 2 sd of the base noise). Most of
   its cost is the Wi-Fi wakeups in item 4. tailscaled CPU is tiny (2:37 CPU-min/day, ~6 ctx switches/s) [M].

Measured as negligible (inside the ±10 mA noise of a 40 s average):
- Orchard paused with SIGSTOP: 221 vs ~225 [M], despite 181 wakeups/s.
- BT radio off (`bluetoothctl power off`): 230 vs 228 [M].
- powersave vs schedutil when idle: 227/205 vs 225 [M]. schedutil already sits at 300 MHz: time_in_state over 24 h is
  300 MHz 87476 s, 1392 MHz 908 s, other OPPs <150 s each.

## Raw numbers (mA, gauge, negative = discharge omitted)

Base (as-is: bl 0, schedutil, PS on, wifi/ts/BT up), 11 runs of 40 s: 233 (first 60 s run), 224, 224, 221, 233, 218,
227, 207, 228, 231, 229. Mean ~225, sd ~7. One outlier of 279 came right after an Orchard SIGCONT (catch-up/net burst).

| config | avg | samples pattern |
|---|---|---|
| bl 200 / 600 / 1024 | 228 / 212 / 230 | same bimodal as base |
| bl 1536 / 1800 / 2047 | 268 / 341 / 474 (and 503 in the first 60 s run) | |
| wifi power_save off | 290 | flat 266-316 |
| gov performance (bl 0) | 436 | flat 437-455 |
| gov powersave (bl 0) | 227, 205 | bimodal 175/270 |
| gov powersave + 1 busy core | 310 | floor 261-265 |
| gov powersave + 2 busy cores | 349 | floor 326 |
| gov performance + 1 busy core | 927 | 826-1005 |
| performance + bl 1800 (≈ screen on, idle) | 503 | 460-567 |
| tailscale off | 200 | |
| wifi off (link down, modules loaded) | 175 | flat 170-190 |
| wifi off + BT off | 174 | flat 170-176 |
| BT off | 230 | |
| Orchard SIGSTOP (30 s) | 221 | |

**Gauge lag:** a step (backlight 0→2047, governor→performance) shows up fully in the **2nd sample**, ≤2-4 s after the change.
So the 6 s settle is enough. The per-sample noise (±65 mA) is the Wi-Fi radio waking. Average ≥40 s, and compare a config
with a base run taken in the same session.

## Wakeups / CPU (idle, screen off, 30-40 s windows) [M]

- arch_timer ~450-530 IRQ/s across both CPUs (HZ=1000, NO_HZ_IDLE). IPI1 (function call) ~31/s when nothing is forked (90/s
  while the measuring script forks). /proc/stat busy ≈ 1.4%.
- Top IRQs: timer, i2c1 6.3/s (SN2400 `online` poll every 2 s + gauge reads via the SN2400 bus handover), brcmf_pcie 2.3/s,
  usbdev 0.2/s. adp-t7000 (vblank) 0/s while screen off. irq/54-apple-z2 touch 0/s.
- Threads: Orchard main **181 wakeups/s**, 1.1% CPU, poll() on the next LVGL timer. Orchard charger thread (tid 328) ~30/s:
  the 2 s `sleep(2)` loop, where every sysfs read of the charger/gauge does i2c/HDQ transactions with sleeping polls
  (`read_poll_timeout(…, 100 µs …)` in apple_swi_battery.c). Then rcu_preempt 13/s, tailscaled ~14/s over several threads,
  wpa_supplicant 2.4/s, brcmf irq thread 2/s, kcompactd 2/s.
- Orchard's always-running lv_timers (it sleeps until the nearest one, so these set its wake rate, `ui/src`):
  LVGL refresh 16 ms (`LV_DEF_REFR_PERIOD 16` = 62/s, even with nothing to draw), net.c 20 ms (only while requests are
  pending), app_music `engine` 100 ms, bt.c `fake_tick` 100 ms + `ui_poll` 150 ms, wifi.c `tick` 200 ms, app_clock `bg_tick`
  250 ms, tailscale.c `tick` 500 ms, sys.c
  charger thread 2 s. None of them pause when the screen is off. This costs <10 mA today (SIGSTOP test). It will matter
  once the kernel has a deep idle state, because every wake then costs an exit from deep idle.
- `screen_set(off)` only sets backlight 0 + the blackout overlay + schedutil. `screen_set(on)` sets **performance** (item 2).
  Auto-lock default is 120 s (`prefs_int("autolock", 120)`).
- No uevent storm (0 uevents/10 s while idle). mdev/kworker bursts only appear right after the charger limit is toggled.

## Hardware blocks powered (debugfs `pm_genpd_summary`, read-only) [M]

On: cpu0, cpu1, cpm, lio, aic, iomux, gpio, mcc, mcu, amp, smx, sf, cp, sep (locked on), ans (flash), pcie + pcie_aux +
pcie_ref (Wi-Fi), usb/usbctrl/usbotg, disp_busmux + disp0 + mipi_dsi (adp-t7000 active), dwi (backlight), sio_p + sio,
uart0 (console), uart1 (BT), uart3 (NFC), i2c0/1/2, mca2 (I2S; runtime PM "unsupported", so always clocked, while
ASoC bias = Off).
Off or runtime-suspended: gfx, media/isp/venc/vdec/jpg/msr, disp1, dp, hsic*, usb2host*, spi0-3 (spi2 touch suspended
between transfers), uart2/4-8 (uart5 = gauge, suspended), mca0/1/3/4, pwm0, aes0, debug.
So no big unused block (GPU, media, ISP) is left on. The floor comes from the CPU idle state, display scanout and PCIe/radios.
Regulators: ldo15 (touch VDD) + chestnut-ldo2 (touch AVDD) on, cs35l19 VA/VP unused (amp off).

## Recommendations (ordered by mA saved per effort)

1. **Orchard: drop `sys_cpu_perf(true)`** (or performance only for ~300 ms after a touch). Saves **~160-210 mA whenever the
   screen is on**. Screen-on idle goes from ~3.5 h to ~5 h. A one-line change.
2. **Kernel: real DPMS off on screen-off** (stop VFTG + panel sleep + gate disp0/mipi_dsi), and have Orchard call it instead
   of only setting backlight 0. Estimated 30-60 mA [I], and it lets DRAM self-refresh, which multiplies the cpuidle gains.
3. **Kernel: cpuidle deep WFI / core power-down** for Typhoon. Estimated 20-60 mA at idle [I], more while the screen is on.
4. **brcmfmac screen-off mode**: PM_MAX or a short pm2_sleep_ret, plus bcn_li_dtim. Recovers part of the 45-50 mA [I].
   Or let Orchard turn Wi-Fi off after N minutes of screen-off when nothing needs it.
5. **Orchard timers**: pause the 100-250 ms background timers and the display refresh timer while the screen is off. Small
   now (<10 mA), important after items 2-3.
6. **Backlight**: steps above ~1500 are expensive (2047 = +250 mA). Consider a ~1800 cap for auto-brightness.
7. Later: s2idle with wake on power button / modem-less RTC. That is the only path to iOS-like standby (single-digit mA).

## Tooling notes / gotchas

- `tools/battmeas.sh` traps EXIT/INT/TERM but **not HUP**. If `bin/ph` times out while it runs, telnet closes, the script
  dies, and **the charger input limit is left at 0**: the phone drains while plugged in. This happened once during this
  audit and was restored by hand. After a 0 → 500000 restore, the chip sometimes reads back **90000**. Write it again and
  verify. `tools/pa_meas.sh` does both (traps HUP, retries until the read-back ≥ the saved limit) and runs detached
  (`setsid … &`, poll the log).
- New helper files (uncommitted, main tree): `tools/pa_meas.sh` (A/B runner: base, bl:N, gov:G, psoff, tsoff, wifioff,
  btoff, orchstop, busy, combinable with `+`), `tools/pa_cpu.sh` (per-thread CPU ticks + context switches over N s).
