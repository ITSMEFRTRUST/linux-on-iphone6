# Orchard — design + developer brief

Orchard is an iOS-style phone shell for Linux on an iPhone 6 (A8, 2 cores, 1 GB RAM, no GPU).
C + LVGL 9.6, software-rendered into a 750×1334 framebuffer (iPhone 6 = @2x: **1 pt = 2 px**, `PT(x)`).

**Look:** modern iOS (iOS 26 layout/shapes) **without Liquid Glass** — solid fills, big rounded
corners, capsule buttons, floating tab bar, large bold titles. Panels that sit on the wallpaper use
`ui_material()` (pre-blurred wallpaper, never real-time blur). Colours come from a desktop theme (`theme.c`):
**always use `pal.*`** (semantic, see `orchard.h`), never raw hex. Dark themes are the main case,
light themes must still be readable. Fonts: `font(F_TEXT|F_TEXT_MEDIUM|F_TEXT_SEMI|F_TEXT_BOLD|
F_DISPLAY_LIGHT|F_DISPLAY_BOLD|F_MONO, px)` (Inter + JetBrainsMono Nerd Font; icons = Nerd Font
glyphs, `G_*` macros in `orchard.h`). iOS sizes: body 17pt → 34px, large title 34pt bold → 68px,
caption 13pt → 26px, row height ≈ 100px, list inset 32px, cell radius 36px.

## Files
`src/orchard.h` = the whole internal API (authoritative — read it first). `src/ui.c` = UI kit
(nav stack, lists, controls, tab bar, alerts). `src/kbd.c` = keyboard. `src/shell.c, home.c, lock.c,
cc.c, switcher.c` = system UI. `src/input.c` = touch routing. `src/app_*.c` = apps (one file each).
`src/sys.c` = sysfs helpers. `src/bt.c` = Bluetooth (one long-lived `bluetoothctl` in a worker thread;
A2DP: `/etc/asound.conf` default PCM → `bluealsa:DEV=<addr>,PROFILE=a2dp` while a sink is connected, the
speaker config is kept in `/tmp/orchard-asound-speaker.conf` and restored; `bt_rename` = BlueZ alias
(`set-alias` when it is bluetoothctl's default device, else `dbus-send`); battery % from `info`'s
`Battery Percentage` (re-read on connect + every minute while connected); previews use fake devices —
`ORCHARD_BT=off|none` to see the other states, `ORCHARD_BT_DEBUG=1` = bluetoothctl transcript on stderr;
`ORCHARD_FLASH=demo` = the phone's real flash numbers for the Storage page, `=<file>` = parse that APFS
image instead of `/dev/asp0n1p1`). `src/wifi.c` = Wi-Fi: wpa_supplicant's control socket
`/var/run/wpa_supplicant/wlan0` spoken directly (wpa_ctrl protocol: datagram sockets bound to
`/tmp/orchard-wpa-<pid>-{c,m}<n>` (new n per open: late replies to a lost socket go nowhere),
one command in flight + a queue, the `m` socket ATTACHed for
`CTRL-EVENT-*`), polled from one LVGL timer, never blocking; on/off = `/usr/sbin/wifiup on|off` (forked,
reaped by the timer; off = flag file `/data/wifi/off` or `/tmp/wifi/off`); DHCP is the boot side's wpa_cli
action script. SELECT_NETWORK disables every other network, so joins re-enable them afterwards
(`ENABLE_NETWORK n no-connect`) before `SAVE_CONFIG`; Auto-Join off = `SET_NETWORK n disabled 1`; a new
network that fails with `WRONG_KEY` is removed again. Settings › Wi-Fi: switch + current network, My /
Other Networks (tap = join or the password page, hold = menu, swipe = Forget, (i) = IP / router / DNS /
signal / Auto-Join / Forget), Other… (hidden), Known Networks; status bar fan (`wifi_icon`, 1–3 bars) left
of the battery; Control Center tile (tap = on/off, hold = Settings). Wi-Fi Sleep (`wifi_sleep`, power.c decides):
`DISCONNECT` + wlan0 down (radio off, firmware + PCIe stay; the user's on/off and the off flag untouched); wake = wlan0 up,
`SCAN freq=<old channel>`, `RECONNECT` (joins from those results, else its normal scan), marker `/tmp/orchard-wifisleep`
so a restarted Orchard wakes it; a failed up / a lost control socket is retried (never stuck half awake); meanwhile the UI shows Wi-Fi on, the network joining and the status bar fan dimmed
(`wifi_reconnecting`). `test/wifi/sleep.sh B` runs it against a mock wpa_supplicant (`test/wifi/wpa_mock.py`). Previews use fake networks —
`ORCHARD_WIFI=off|none|idle` for the other states, password `wrongpass` = the wrong-password alert;
`ORCHARD_WIFI=real` (+ `ORCHARD_WPA_CTRL=<socket>`, `ORCHARD_WIFIUP=<script>`) drives a real or mock
wpa_supplicant from the snap/sdl build. `src/apfs.c` = read-only APFS container reader (latest checkpoint →
object map → volume superblocks + space manager) behind Settings › General › iPhone Storage. `src/prefs.c` = settings store. `src/theme.c` = theme + wallpapers.

## Keys + system gestures
Home button (no swipe-up, no home indicator): press = home / close Search, Control Center or the
switcher / unlock (first press wakes a dark screen), double-press = app switcher, hold ≥ 0.5 s =
Search from anywhere. Power = screen off + lock. Volume keys = volume HUD (Vol− = Ctrl in the
Terminal). Touch: pull down from the top zone (status bar + nav bar, y < 120 px) = Control Center
everywhere — it follows the finger and settles on release (a tap up there still reaches the app);
left-edge swipe = back (follows the finger too). There is no swipe-down for Search.
Touch routing (`input.c`): every finger is classified when it lands — keyboard keys, `O_TOP`
(y < `EDGE_TOP` 120: undecided until 16 px; down within ~60° = Control Center pull, else the press is
replayed to LVGL as a tap/drag), `O_LEFT` (x < `EDGE_LEFT` 64 in an app with a pushed page: rightwards
= back swipe, else replayed to LVGL), otherwise LVGL's pointer (one at a time, queued, read on the next
timer pass). Gestures pass the finger's speed on release. The phone maps raw panel units through a
calibrated fit (`hal_phone.c` `map_touch`, override file `$ORCHARD_DATA/touchcal` or
`/etc/orchard/touchcal`, `ORCHARD_TOUCHCAL=0` = plain stretch) and holds a contact at its landing point
until it moves ≥ 10 px (`steady_pos`), so taps don't turn into micro-drags.
Music plays MP3s stored on the phone (`$ORCHARD_DATA/Music`, `/root/Music`, `/tmp/music`; `bin/orc music [dir]` copies a folder over the USB link). It can also show playlists from an optional remote library through a relay that speaks a small plain-HTTP API (none ships with this project): `ORCHARD_MUSICD=host:port` or pref `music_relay` for one over the USB link, `ORCHARD_MUSICD2` or pref `music_relay2` for one over Wi-Fi; empty / `off` = none, the default.

Web's site icons (`web_icon.c`): when a reader page loads, its apple-touch-icon / non-SVG `rel=icon` / `/apple-touch-icon.png` /
`/favicon.ico` is fetched (page's own LAN pass, no cookies, ≤ 256 KB), decoded on the net_bg worker (PNG, JPEG, ICO via
`ico.c` — fuzzed by `test/unit/ico_fuzz.c`) and cached as raw pixels in `$ORCHARD_DATA/web/icons/<host>.bgra` (looked up
again after a week, 6 MB cap); the start page reads tiles from there on the worker, never the network. Preview:
`test/web/icons.sh OUTDIR B` (local fixture sites on 127.0.0.1).
Web looks like Chrome for iOS: an omnibox bar under the status bar (`TOP_H` 100) and a bottom toolbar (`TB_H`: back,
forward, new tab, the tab count, ⋯), the ⋯ bottom sheet, a two-column tab grid, a new tab page (the start page),
"Open in New Tab" in the link long-press menus; toolbar glyphs are the `G_M_*` Material ones. **Tabs:** one tab is
live (the nav stack); the others are their pages' URLs (each with its LAN pass), a Forward list, a title and a
snapshot (333 × 420 RGB565, ~280 KB). At most 20 tabs (≈ 5.6 MB of snapshots, kept in statics across app close),
then a "Too Many Tabs" alert; a restored tab loads only its top page, the older ones load when back reaches them.
Grid cards carry their tab's id, not its index (a queued × or tap still means its own tab). Saved (deferred, after
each load / back) to `data_dir()/web/tabs.txt`, one field per line: `c<TAB>cur`, then per tab `t<TAB>title`,
`u<TAB>pv<TAB>url` per page (oldest first, pv 1 = LAN pass), `f<TAB>url` per Forward entry; written to a temp file
and renamed only if the write worked; no snapshots in it (cards show a site tile until the tab is seen). Reopening
Web restores the last tab and reloads its page from the network. The snapshot is read back from the framebuffer
(needs DIRECT render mode with the cached shadow buffer, hal_phone.c), so it must be taken *before* an overlay (⋯
sheet, grid) covers the page; only the toolbar's + snapshots at new-tab time. The loading bar is time-based: one
100 ms timer only while a page loads, paused under the Lock Screen. The omnibox emulates iOS select-all on focus
(`sel_all`: the whole URL shows selected; `sel_typed`: typing or a paste replaces it).

Camera (`app_camera.c` + `cam_*.c`) reads the A8 ISP's `/dev/isp0` (interface v1 in `src/isp_uapi.h`, the kernel
copy is `include/uapi/linux/apple-isp-t7000.h`): one long-lived camera thread (`cam_dev.c`) opens it (version check only),
STARTs rear / front, then INFO + mmap (the driver fills them only while streaming; STOP = unmap; a START that is EBUSY
because the buffers were sized for the other camera = close + reopen + START), polls + reads frames, converts each NV12 frame straight out of the mmap'd buffer and RELEASEs it (at most 30 fps:
the rear IMX237 preset streams ~120 fps, so 3 of 4 frames go straight back unconverted — without that the camera thread
took 72 % of a core on hw); the UI timer
only swaps the newest of three 750 × 1000 XRGB buffers into one lv_image (the 3:4 viewfinder between the black bars).
Photos are full resolution: the viewfinder STARTs with `ISP_START_PHOTO_VIEW` (4:3 binned preset, rear 1632x1224,
front 1376x968), the shutter STOPs, STARTs `ISP_START_STILL` (rear 3264x2448 = 8 MP, front 1376x968), waits until the new
preset's auto-exposure (it restarts from dark) is steady within 2 % and >= 80 % of the viewfinder's brightness (at most
60 frames: then the last one), saves that frame and STARTs the
viewfinder again (`take_still` in `cam_dev.c`; log line `cam: still WxH after N frame(s), T ms`). From the tap until
the viewfinder's next frame the shutter and the last-photo thumbnail are greyed and disabled (`cam_dev_busy`;
`cam_dev_capture` refuses a tap meanwhile, so there is no blink / tap for a photo that isn't taken). Once
START(STILL) is sent the photo is always finished — the user got the blink for it: the camera turned off meanwhile
(Home, lock, switcher) or a front / rear flip doesn't drop it; after an off the first bright-enough frame (>= 80 % of
the viewfinder, no steadiness) will do and it is capped at 40 frames (AE climbs from dark for ~20-30), then STOP +
close. A tap the camera thread hasn't picked up yet (within one frame) is dropped by an off, not taken later. A driver without the flags answers EINVAL: `cam_src.c` then STARTs
with the bare camera number, and if that passes, sends bare numbers until the source is closed (the next open asks
again; an EINVAL for the bare number too isn't the flags: no downgrade). `cam_src_has_presets()` = false then (and
for the fake source): no STOP / START(STILL), a photo is just the next viewfinder frame, as before the presets.
`cam_conv.c` = the whole picture path: landscape sensors turn 90° clockwise (front = mirrored; `ORCHARD_CAMERA_ROT`,
`ORCHARD_CAMERA_RANGE=full|video` override). Only the source rows the picture uses are copied (64-byte loads); each is
converted along the row 16 samples per NEON step (tbl gathers out of 32 / 64-byte windows planned at init, 16-bit
maths, vst4) into a 16-row buffer that a 4×4 NEON transpose writes into the destination columns; a scalar path with the
same numbers covers no-NEON builds and shrinking > 2× (thumbnails). Base tone on top of BT.601 (the ISP's picture looks
flat / washed out): S-curve on the luma (contrast 30 %) + chroma × 1.2, in the converter's LUT / coefficients, so the
viewfinder, thumbnails and photos (before the film filter) get it; `ORCHARD_CAMERA_TONE=contrast,saturation` (`0,100` =
the plain ISP picture). `test/unit/cam_conv_test.c`: colours, every rotation / mirror, bounds, ±2 of the old converter
with the plain tone, NEON = scalar bit for bit (run its aarch64 build from `test/unit/build-camvf-bench.sh` on an aarch64
box); `camvf-bench` (same script) = camint's converter vs this one on live frames or `synth`. Estimated on the phone
(calibrated against yuv_blit on a Cortex-A76): 960×540 → 750×1000 ≈ 7.5 ms per frame (≈ 23 % of a core at 30 fps),
640×480 ≈ 6 ms. Measured on the phone (2026-10-21): front 1376×776 → 750×1000 = 16.8 ms avg at 30 fps (50 % of a
core), rear 1408×792 = 6.1 ms avg (at 120 fps, before pacing). The photo (the still, or without the presets the next
frame) is copied and saved on its own thread as `$ORCHARD_DATA/Photos/IMG_0001.JPG`
(libjpeg-turbo, upright at the sensor's size; a front photo looks like the mirrored viewfinder unless Settings › Camera ›
Mirror Front Camera (pref `camera_mirror`, default 1, iOS's switch) is off; `.thumbs/IMG_0001.bgra` = its 92 px thumbnail). The camera runs only
while its viewfinder is on screen (screen off, locked, switcher, the photo viewer, app closed = STOP + close, the
sensor powers down) and holds auto-lock meanwhile (`camera` lease). No device / START fails / no frame in 5 s =
"Camera Unavailable" + the reason + Try Again; nothing ever blocks the UI thread. Previews: `ORCHARD_CAMERA=fake`
(default in sdl / snap: an animated NV12 test card through the same path), `=none|nostream|fail|stall` = the error
screens, `ORCHARD_CAMERA_SIZE=WxH`; `cam:` log lines = frames, fps, convert ms, the camera thread's CPU (when it stops,
every 5 s with `ORCHARD_CAMERA_STATS=1`). Unit tests: `test/unit/cam_src_test.c` (the flags' fallback, behind a
pretend /dev/isp0), `test/unit/cam_dev_test.c` (the shutter: next frame / still, busy, Home mid-still, the off cap,
60-frame timeout, flip, a stale tap, the failure paths). `test/unit/cam_isp_mock.c` (LD_PRELOAD into snap / sdl with
`ORCHARD_CAMERA=real`) is a driver before the flags by default; `ISPMOCK_FLAGS=1` takes them (STILL = 1280×960),
`ISPMOCK_UNSTEADY=1` = a still that never settles, `ISPMOCK_FOCUS=1` takes ISP_IOC_FOCUS (default ENOTTY = no AF). `test/camera/shutter.sh [B]` runs the snap build through it (old
driver, still + greyed shutter, Home mid-still, the thumbnail during a still).
Filters (`cam_lut.c`, iOS-style): the top-right rings button opens a strip of three live previews (ORIGINAL / WARM /
VIVID) over the bottom of the viewfinder; pref `camera_filter` (default `warm`). The chosen 3D LUT (any .cube file the user supplies
as `$ORCHARD_DATA/luts/warm.cube` / `vivid.cube`; none ship with Orchard; without one a filter falls back to Original) grades
every saved photo + its thumbnail on the save thread just before the JPEG encode: fixed-point tetrahedral
interpolation (= ffmpeg `lut3d=interp=tetrahedral` ±1 level), split over both cores from 1 MP. Measured on the phone
(2026-10-21): 8 MP = 230 ms (2 threads; 459 ms on one), 1.2 MP = 36 ms, .cube parse 17 ms (once, in the background at
app open). Files: `$ORCHARD_DATA/luts/<name>.cube` first, then `/usr/share/orchard/luts/` (staged in
`var/extra-rd/usr/share/orchard/luts/`); a missing / broken file = Original, logged once (`cam: filter …`). The
viewfinder itself is not graded (only the 150 px previews, from the shown frame every 400 ms). Unit test:
`test/unit/cam_lut_test.c` (+ ffmpeg comparison); bench / PPM grader: `test/unit/build-camlut-bench.sh`.
Focus (`cam_focus.c`): the driver runs continuous AF on the rear camera from stream start; a tap on the viewfinder =
AF + exposure window at that point (per-mille of the frame in sensor orientation: the tap goes back through the cover
crop, rotation and mirror) + one AF scan, the yellow square shrinks in, pulses, fades, and 4 s later AF is continuous
again on that point; a long press = the same scan, then "AF LOCK" until the next tap or a flip. The front lens is
fixed-focus: a tap only moves the exposure window. The UI posts the request, cam_dev's worker sends it between two
frames (`ISP_IOC_FOCUS`, uapi minor 1; a driver without it = ENOTTY = tap to focus off, the square still shows).
Voice Memos (`app_memos.c` + `memo.c`) records the codec's mics from ALSA `hw:0,2` (2 ch S16_LE 48 kHz over the ASP:
TX1 = ADC1A = bottom mic, TX2 = ADC2A = rear by the driver's defaults) with `arecord` (= `/usr/bin/aplay` started as
"arecord", alsa.tgz has no arecord) on a recorder thread that keeps one channel and writes a mono 16-bit 48 kHz WAV to
`$ORCHARD_DATA/Recordings/<name>.wav` (`.rec-<name>.wav` until it is finished; a crash's leftover is recovered by the next
scan). Name = the start time ("Oct 21, 10:45 PM"), rename / delete from the row's hold menu, swipe = delete, the
player card's trash. Pref `memos_mic` 0 bottom / 1 rear / 2 front (the mic button in the nav bar): rear and front are
offered only when the codec module has `ADC1A Capture Volume` (n61-mic2/3: the fixed mux labels), front = `ASP TX2 Mux`
→ ADC2B for the recording and back to ADC2A after. hw:0,2 in use (`/proc/asound/card0/pcm2c/sub0/status`) = an alert
naming the owner, nothing starts. Playback = `aplay` on the default PCM (the speaker plug or BlueALSA, as bt.c sets
it) fed raw samples from the playhead by a player thread (pause = aplay stops, seek = a new one; one at a time).
Tapping a recording only selects it — nothing plays by itself; Music pauses when recording / playback starts. Home or
closing = the recording stops and is saved, playback pauses; locking pauses playback only — the recording goes on with
the screen dark (like iOS; nothing suspends the SoC) and holds auto-lock only while the app is in front. Space: below
100 MB free a recording won't start ("Not Enough Storage"), below 50 MB a running one stops and is saved; arecord
writes into a 1 MiB pipe and each recording logs its overrun count (`memos: recorded …`). Previews: a synthetic
voice (`ORCHARD_MEMOS=fake`, default in sdl / snap; `=busy` / `=nomic` = the alerts); `test/memos/run.sh B` runs
the real process path against stand-in arecord / aplay / amixer; `test/unit/memo_test.c` = WAV / names / meter.

## App contract
```c
const app_t app_foo = { .name = "Foo", .glyph = G_..., .icon = IC_..., .open = app_open, .close = app_close };
```
- `open(root)`: build the UI into `root` (full screen; the shell draws the status bar over the top
  40 px; keep controls `BOTTOM_PAD` above the bottom edge — no home indicator, the Home button does home).
  Most apps start with `lv_obj_t *c = nav_root(root, "Title");` (large title, scrolling column),
  fill it with `ui_section()` / `ui_row*()`, and drill down with `nav_push("Title")` (slide-in page
  with back button + edge-swipe back; up to 16 pages). `nav_on_leave(cb)` = cleanup when that page
  goes away. The bar centres the title and makes room for it: back label = previous title → "Back"
  → chevron only, then the title ends in "…"; `nav_set_title(page, t)` renames a page. A full-screen
  sheet over the pages (Music's Now Playing) calls `nav_lock(true)` while it is up, so the edge swipe
  can't pop the hidden page (an alert blocks it too: `nav_locked()`). Rule: every full-screen overlay that
  isn't a nav page calls `nav_lock` (Web's tab grid and ⋯ sheet do). `nav_land()` lands a push in flight at once
  (a page restored in place, no slide); `nav_cut(to)` drops the pages above depth `to` at once, leave callbacks
  run (Web's tab switch) — it does nothing while a back-drag is in progress.
  Tabbed apps (Clock, Music) create their own containers + `ui_tabbar()`.
- Rows: `ui_row` keeps title (+ `ui_row_subtitle`) on one line each ("…") and centred in the 100 px
  row. `ui_row_tap` (CLICKED), `ui_row_hold(row, cb, ud)` (long press: tick, then cb — usually a
  `ui_row_menu`; the release isn't a tap), `ui_row_swipe(row, glyph, label, color, cb, ud)` (swipe left
  shows an action pill, iOS swipe actions; any other press puts it away, `ui_swipe_shown()`).
  Menus: `ui_menu` / `ui_row_menu(row, items, n, cb)` = iOS 26 popover card on the overlay
  (`MI_RED`, `MI_GROUP`, `MI_CHECK|MI_ON`, `MI_HEAD`); Files' and Settings' long-press menus use it.
- `close()`: the shell deletes every object right after this — set all static `lv_obj_t *` to NULL
  and delete UI timers. State that must survive closing stays in statics.
- Background work (alarms, playback) lives in the app's `*_init()` hook, called once at startup.
- Text input: an `lv_textarea` from `ui_text_field()` / `ui_search()` opens the keyboard on tap
  (`kbd_attach`). The textarea gets `EV_KBD_SHOWN` / `EV_KBD_HIDDEN` (orchard.h) when the keyboard
  shows for it / goes away — shrink scroll areas by `kbd_height(KBD_TEXT)` meanwhile. Don't use
  LVGL's FOCUSED / DEFOCUSED for that: kbd.c owns a textarea's focus (focused = caret = keyboard). `LV_EVENT_READY` = "done" on a
  one-line field. Locking leaves it alone: the keyboard waits under the lock screen (`kbd_cover`: HIDDEN so a wake
  doesn't draw it, `kbd_visible()` still true, no keys held, no caret blink) and is there again on unlock, like iOS
  (the slide shows it as it uncovers it: `kbd_uncover_show`). Multi-line: `lv_textarea_create` + `kbd_attach(ta, KBD_TEXT)` on click.
- Haptics: `ui_haptic_tap()` on discrete actions (sparingly), `ui_haptic_success()`.
- Confirmations: `ui_alert(title, msg, (const char*[]){"Cancel", "!Delete"}, 2, cb, ud)` ('!' = destructive).
  One at a time: another raised meanwhile waits its turn, one raised from an alert's button callback
  fades in as that one goes. Alerts (and menus) sit above the keyboard and take its touches.
- Notifications: `shell_banner(&app_foo, "Title", "Body")`; alarms: `shell_alarm(...)` (buzzes, wakes the screen).
- Persistent data: files under `data_dir()`; small settings via `prefs_int/str` + `prefs_set_*`.
  Known keys: `haptics` `autolock`(s, 0=never, default 30) `clock24` `wallpaper`(index) `start_locked`
  `kbd_predict` `kbd_suggest` `kbd_autocap` `kbd_haptic` `dnd` (Do Not Disturb, default 0) `lowpower` (Low Power Mode, default 0)
  `wifisleep` (Wi-Fi Sleep, default 0 = off) `camera_mirror` (front photos mirrored like the viewfinder) (all 0/1, default 1 unless noted); `wifisleep_min` (1 / 5 / 15, default 5); `music_relay` / `music_relay2` (host:port of Music's optional USB / Wi-Fi relay, empty / `off` = none). Search / command fields: call `kbd_no_autocap()` right after `kbd_attach`. Password fields: a textarea in password mode is
  secret automatically (no suggestions / word model / auto-caps / pop-ups, nothing remembered); call
  `kbd_secure()` after `kbd_attach` when an eye button may show the text.
  The phone's filesystem is RAM (initramfs) — data survives until reboot; `bin/orc pull` saves it.

## LVGL gotchas (learned the hard way)
- An object that handles `LV_EVENT_GESTURE` must `lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE)`,
  otherwise the gesture goes to its parent chain (up to the screen) instead. Gestures fire after
  70 px of travel (input.c tunes this). Drag controls inside a gesture handler (sliders) should
  also drop GESTURE_BUBBLE so dragging them doesn't trigger the parent's swipe.
- Nerd Font icons don't match their label box: most are wider than the advance LVGL sizes the label by
  (fa-wifi 1.15 em vs 0.6 em) and sit a little high, so `lv_obj_center` on an icon label shows it right of
  centre and LVGL clips whatever hangs more than font_h/4 past the box (the right side of the Wi-Fi / USB /
  speaker icons). Call `ui_glyph_fit(label)` on a one-glyph label: its box becomes the ink's box (or, with a
  fixed width, the ink is centred in it), then centre / align / flex as usual. `ui_icon_font(glyph, ink)` =
  the size at which the glyph's larger side is ~ink px (list tiles: 36 px in 60, so every icon looks the same
  size). `ui_icon(parent, glyph, font, color)` = `text()` + fit; `ui_icon_line(...)` keeps the font's line box (a row /
  column lays out as before) with the ink centred in it; a fixed width or height = the ink centred in it. Change a
  fitted label's glyph with `ui_icon_set(label, glyph)` (sets only on change, fits again; plain text = a plain label
  again), never `lv_label_set_text`; after changing its font, `ui_glyph_fit` it again. `ui_icon_button` is fitted.
- `sys_*` hardware calls (backlight, volume, reboot) are no-ops in the sdl/snap builds — the
  preview never touches the PC. Write UI that still looks right with them returning defaults.

## Performance rules (A8 CPU, software rendering)
- Redraw small areas. No per-frame full-screen changes except short (<350 ms) transitions.
- UI refresh timers ≥ 250 ms (stopwatch label may tick at 30–50 ms — only its label invalidates).
- No shadows/opa/transform-scale on big containers; avoid `clip_corner` on large scrolling objects.
- Don't create hundreds of objects: long lists (files, songs) — build ≤ ~150 rows, or paginate.

## Motion (orchard.h `MOTION_*`, ui.c)
- One curve for everything: `motion_anim(var, exec, from, to, ms, done)` / `motion_path` (ease-out
  quart — moves a lot on the first frame, settles softly). Durations: `MOTION_ZOOM` (app open/close),
  `MOTION_PAGE` (nav push/pop, Control Center, lock), `MOTION_SHEET` (keyboard, banners),
  `MOTION_SMALL` (thumbs, alert fade). Gesture releases leave at the finger's speed:
  `motion_fling_ms(dist, v, max)`.
- Never block input on an animation (no `busy` flags): new input finishes or retargets it.
- Anything that only *moves* (pages, panels, sheets): `slide_track(obj, true)` then move it with
  `slide_move()` — its pixels are shifted in the framebuffer and only the uncovered strip, soft edges
  (shadow, rounded corners) and overlays above it are drawn (≈5 ms frames instead of 20–40).
  `slide_track(obj, false)` when it lands (one full redraw of it). Track bottom to top. Backends
  report pixels moved outside LVGL with `hal_damage()` (phone: added to the DIRTYFB clips).
  `ORCHARD_NOSLIDE=1` redraws instead (to compare frames, e.g. snap shots with and without).
- Build things that slide in off screen with invalidation off (`lv_display_enable_invalidation`)
  and apply their position at once (`lv_obj_refr_pos`) — otherwise LVGL invalidates both places on
  the next layout pass and the whole screen gets redrawn anyway.
- App open/close zooms the app's last frame (copied off the screen at close; first launch: built and
  drawn once off screen), scaled by a plain copy loop — not an LVGL transform. Double-press: the app
  shrinks into its switcher card.
- Measure on the phone: start it with `ORCHARD_MOTION=1` (one line per animation: frames, fps,
  frame ms, share redrawn, delay after the input, plus a `stall:` line for any pass that spent
  > 25 ms outside drawing — page builds, slow timers, blocking reads) or `=2` (every frame) in
  `/tmp/orchard.log`: `ORCHARD_MOTION=1 B=build-x bin/orc deploy`. `PERF=1` (make / orc deploy, own
  B) builds LVGL's sysmon in (FPS / CPU once a second) — debug only, it never lets the phone idle.
- Never block the UI thread on hardware: sysfs files behind an ADC / i2c (the charger's `online`,
  `current_now`: ~11 ms each) are read by a sampler thread (sys.c). Idle (home screen) ≈ 0.4% CPU,
  screen off ≈ 0.3%; keep it there — timers that only refresh what's on screen check that it is.
- Screen off = the UI sleeps (`power.c`): 1.2 s after Power / auto-lock every LVGL timer is paused except the
  background ones registered with `power_keep(t, dark_ms, name)` (alarms, music, Bluetooth, Wi-Fi, network jobs,
  the Terminal session), invalidation is off, touch isn't read; the main loop sleeps until the next kept
  timer or a key (Power / Home wake; touch never does). Asleep, the display pipe is off (adp-t7000 DPMS; on
  again ≈ 250 ms before the wake redraw), the touch panel inhibited (`/sys/class/input/*/inhibited`) and the CPU
  capped at 600 MHz. A background timer that must keep going with the screen
  off registers itself; one that only feeds the UI needs nothing. `power_dark()` = asleep (skip pings / polls).
  Wake: if DPMS on fails, or adp-t7000's `display_state` (card device dir; "ok" / "bad …", missing on old
  drivers = trust the ioctl) says bad, the backlight stays off and power.c retries every 500 ms (bad ≥ 2 s since
  the last try = DPMS off + on) for 10 s, then once every 30 s (logged once); Power off + on starts fast again.
  Key events and display changes are logged with a dmesg-clock stamp; `tools/disp/wedge.sh` (`snap` / `keys`)
  and `tools/disp/stress.sh` diagnose a dark-screen wedge on the phone.
  `ORCHARD_POWERLOG=1` = screen changes + a line a minute (wake-ups, frames, CPU, battery) in the log. Snap
  `idle MS` runs the phone's loop on the simulated clock and lists the timers that woke it.
- Auto-lock holds: `power_keep_awake(token, on)` / `power_awake_for(token, ms)` (a lease: refresh it while the
  reason lasts, it can't get stuck). Held: Terminal output in front, the 30 s
  after a typed key while the keyboard is up, a running stopwatch / timer on screen while charging. When the last
  one goes the countdown starts from then. Music playing holds nothing (iOS). A touch on a dimmed screen only
  brightens it (input.c eats it); the dim is a third of the current level.
- Idle with the screen on must draw nothing: set a label / flag / colour only when it changes (setting the same
  text again still redraws it — and a `LONG_MODE_DOTS` label holds the cut text, so compare with what you set);
  a caret blinks only in `LV_PART_CURSOR | LV_STATE_FOCUSED` (an unfocused textarea's blink was 60 animation
  passes a second forever). LVGL's input read timer sleeps while no finger is down (input.c). Background engines
  slow down when their UI is closed (Music 500 ms, Wi-Fi 1 s, Bluetooth 500 ms, Terminal 1 s).
  Under the lit lock screen the open app is as good as closed: `lock_listen(cb, true)` (lock.c) — on lock slow
  down / pause its foreground polls + UI timers, on unlock put them back at once (`lv_timer_ready`); lock.c stops
  LVGL spinners under it itself — make every spinner with `ui_spinner()` (never `lv_spinner_create`): one made
  while locked starts stopped. A listener added while locked is called with `true` at once. Bluetooth discovery
  (Settings › Bluetooth open) pauses under the lock / with the screen off and starts again on unlock (bt.c `seen`). Snap: `ORCHARD_BT=none` drops the fake Bluetooth's 100 ms tick, so `idle` counts
  what the phone wakes for (lit lock over any app: 12 wake-ups / 5 s).
- Wi-Fi Sleep (Settings › Battery, prefs `wifisleep` / `wifisleep_min`; **off by default** until it is measured on the
  phone): once the screen has been dark 1 / 5 / 15 min
  (checked by the 30 s dark shell tick, no timer of its own) Wi-Fi sleeps unless something needs the network: music
  playing, a net.c request, a `power_net_hold(token, on)` / `power_net_for(token, ms)` the charger,
  or a curl / wget / ssh / scp / sftp / rsync / apk / git process. A hold or the charger turning up wakes
  it; the screen coming on wakes it first thing (it reconnects while the display comes back). Tailscale rides along
  (no route: tailscaled stays quiet, rebinds by itself). `ORCHARD_WIFISLEEP_S=<s>` = that delay, `ORCHARD_WIFISLEEP=charger`
  = also on the charger (a phone measured on its cable); `wifi: sleep:` lines in the log time each wake. A wake never
  stays half done: a failed wlan0 up is tried again (0.5 / 1 / 2 s), then once `wifiup on`; RECONNECT repeats until
  wpa_supplicant takes it, also after the control socket was lost on the way. A net.c request made while it sleeps
  (or in the first ≤ 20 s of a wake, `wifi_sleep_wait()`) wakes it (`power_net_for`) and starts once it is back
  (≤ 20 s; its timeout counts from then) instead of failing its DNS lookup.
- Low Power Mode (`power_lpm()`, Settings › Battery, Control Center tile): CPU ≤ 840 MHz, Wi-Fi power save,
  charger read every 10 s, Wi-Fi signal every 30 s, Auto-Lock ≤ 30 s, no second hand on the home clock, yellow
  battery; off by itself at 80 % on the charger; the 20 % alert offers it. Settings › Battery also shows the
  draw, time left and usage since unplugged (`power_usage()`, `power_calc.h`, `test/unit/power_test.c`).
- CPU: schedutil, minimum raised to the top for 1 s after every touch / key and while a video plays
  (`power_busy()`), instead of the performance governor whenever the screen was on (`ORCHARD_CPU=perf` = old).

## Build + test (each builder uses its own build dir B)
```sh
cd ui
mkdir -p build-NAME/snap && cp -al build/snap/third_party build/snap/flags build-NAME/snap/ 2>/dev/null  # reuse compiled LVGL (+ its flags file, or it all rebuilds)
make B=build-NAME snap -j8          # headless screenshot tool → build-NAME/snap/orchard-snap
mkdir -p build-NAME/data && printf 'start_locked=0\n' > build-NAME/data/prefs
ORCHARD_THEME=build/theme ORCHARD_DATA=$PWD/build-NAME/data TZ=EST5EDT \
  build-NAME/snap/orchard-snap open Clock wait 400 shot /path/a.ppm tap 375 700 wait 300 shot /path/b.ppm
magick /path/a.ppm -resize 50% /path/a.png   # then look at the png
```
Snap commands: `open APPNAME` · `tap X Y` · `drag X1 Y1 X2 Y2` · `wait MS` · `key home|power` ·
`type TEXT` (through the keyboard path) · `press bksp|enter` · `notify TITLE BODY` (banner) ·
`touch X Y` / `lift` (a finger step by step: one frame each) · `shot FILE.ppm`. Time is simulated. Home pressed twice < 400 ms apart = app switcher.
Also test the built-in dark palette: run without `ORCHARD_THEME` (uses the iOS-dark fallback).
`make B=build-NAME phone` cross-compiles for the phone (aarch64 musl) — must also build warning-free.
