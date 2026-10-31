# Orchard — iOS-style shell for Linux on the iPhone 6.
#   make sdl    desktop preview window   (build/sdl/orchard)
#   make snap   headless screenshot tool (build/snap/orchard-snap)
#   make phone  static aarch64 musl binary for the phone (build/phone/orchard)
#   B=dir       build directory (default build/; parallel builders each use their own)
#   PERF=1      phone build with LVGL's sysmon: FPS / render / flush times in /tmp/orchard.log once a
#               second (debug only: it keeps the refresh timer running, so the phone never idles)
B     ?= build
LVGL  := third_party/lvgl
VTERM := third_party/libvterm-0.3.3
SYSROOT := $(abspath ../var/sysroot)

LV_SRC  := $(shell find $(LVGL)/src -name '*.c')
VT_SRC  := $(wildcard $(VTERM)/src/*.c)
COMMON  := src/main.c src/shell.c src/home.c src/lock.c src/cc.c src/switcher.c src/input.c src/kbd.c \
           src/ui.c src/prefs.c src/fonts.c src/sys.c src/theme.c src/bt.c src/wifi.c src/tailscale.c src/apfs.c \
           src/app_terminal.c src/app_settings.c src/app_calc.c src/app_music.c src/app_clock.c \
           src/app_notes.c src/app_files.c \
           src/net.c src/json.c src/html.c src/imgdec.c src/app_web.c src/app_weather.c \
           src/web_icon.c src/power.c \
           src/app_camera.c src/cam_conv.c src/cam_src.c src/cam_dev.c src/cam_save.c src/cam_lut.c src/cam_focus.c \
           src/app_memos.c src/memo.c
ICONS   := clock notes calculator web weather camera terminal music files settings voicememos
ICON_RAW := $(foreach i,$(ICONS),assets/icons/raw/$(i)-120.bgra assets/icons/raw/$(i)-64.bgra)
ASSETS  := $(wildcard assets/*.ttf assets/*.txt) $(ICON_RAW)

# app icons: assets/icons/*.svg (written by assets/icons/gen.py) → raw BGRA (LVGL ARGB8888) at 120 + 64 px
assets/icons/raw/%-120.bgra: assets/icons/%.svg
	@mkdir -p $(@D)
	@resvg -w 120 -h 120 $< $@.png && magick $@.png -depth 8 BGRA:$@ && rm $@.png
assets/icons/raw/%-64.bgra: assets/icons/%.svg
	@mkdir -p $(@D)
	@resvg -w 64 -h 64 $< $@.png && magick $@.png -depth 8 BGRA:$@ && rm $@.png

CPPFLAGS := -I. -Isrc -I$(LVGL) -I$(VTERM)/include -DLV_CONF_INCLUDE_SIMPLE -D_GNU_SOURCE
CFLAGS   := -O2 -g -Wall -Wno-deprecated-declarations -Wno-unused-parameter

sdl_CC    := clang
sdl_FLAGS := -DORCHARD_SDL
sdl_LIBS  := -lSDL2 -lm -lutil
sdl_HAL   := src/hal_sdl.c

snap_CC    := clang
snap_FLAGS := -DORCHARD_SNAP
snap_LIBS  := -lm -lutil
snap_HAL   := src/hal_snap.c

phone_CC    := clang --target=aarch64-linux-musl --sysroot=$(SYSROOT) \
               -resource-dir=$(SYSROOT)/usr/lib/llvm22/lib/clang/22
phone_FLAGS := -DORCHARD_PHONE $(if $(PERF),-DORCHARD_PERF) -mcpu=apple-a8
phone_LIBS  := -static -fuse-ld=lld -rtlib=compiler-rt -lm
phone_HAL   := src/hal_phone.c

# The Camera's JPEG encoder (cam_save.c) uses libjpeg-turbo when there is one: the system's for sdl / snap,
# Alpine's aarch64 static library in ../var/jpeg for the phone (fetch-deps.sh). Only cam_save.o sees
# the flag, so nothing else rebuilds.
# The Camera's converter (cam_conv.c) uses NEON intrinsics: arm_neon.h is generated per compiler, and the sysroot's
# clang 22 resource dir has none, so cam_conv.o also looks in the compiler's own (after every other include dir).
$(B)/phone/src/cam_conv.o: phone_FLAGS += -idirafter $(shell clang -print-resource-dir)/include
JPEG_PHONE := $(abspath ../var/jpeg/root/usr)
ifneq ($(wildcard $(JPEG_PHONE)/lib/libjpeg.a),)
$(B)/phone/src/cam_save.o: phone_FLAGS += -DORCHARD_TURBOJPEG -isystem $(JPEG_PHONE)/include
phone_LIBS += $(JPEG_PHONE)/lib/libjpeg.a
endif
ifneq ($(wildcard /usr/include/jpeglib.h),)
$(B)/sdl/src/cam_save.o: sdl_FLAGS += -DORCHARD_TURBOJPEG
$(B)/snap/src/cam_save.o: snap_FLAGS += -DORCHARD_TURBOJPEG
sdl_LIBS += -ljpeg
snap_LIBS += -ljpeg
endif

# $(B)/<variant>/flags changes when the compiler flags do (PERF=1 on / off): everything rebuilds,
# objects built with different lv_conf switches never get linked together. Written only when different, and
# replaced (tmp + mv) rather than rewritten, so a flags file hard-linked from another build dir along with its
# objects (DESIGN.md's cp -al recipe) keeps its old mtime and is never changed through the link.
define variant
$(1)_OBJ := $$(patsubst %.c,$(B)/$(1)/%.o,$$(LV_SRC) $$(VT_SRC) $$(COMMON) $$($(1)_HAL)) $(B)/$(1)/src/assets.o
$$(shell mkdir -p $(B)/$(1); echo '$$($(1)_FLAGS)' | cmp -s - $(B)/$(1)/flags || { echo '$$($(1)_FLAGS)' > $(B)/$(1)/flags.tmp && mv -f $(B)/$(1)/flags.tmp $(B)/$(1)/flags; })
$(B)/$(1)/third_party/%.o: third_party/%.c lv_conf.h $(B)/$(1)/flags
	@mkdir -p $$(@D)
	@$$($(1)_CC) $$(CPPFLAGS) $$(CFLAGS) $$($(1)_FLAGS) -c $$< -o $$@
$(B)/$(1)/src/%.o: src/%.c lv_conf.h src/orchard.h $(B)/$(1)/flags
	@mkdir -p $$(@D)
	@$$($(1)_CC) $$(CPPFLAGS) $$(CFLAGS) $$($(1)_FLAGS) -c $$< -o $$@
$(B)/$(1)/src/assets.o: src/assets.S $$(ASSETS)
	@mkdir -p $$(@D)
	$$($(1)_CC) -c $$< -o $$@
endef
$(foreach v,sdl snap phone,$(eval $(call variant,$(v))))

$(B)/sdl/orchard: $(sdl_OBJ)
	$(sdl_CC) $^ -o $@ $(sdl_LIBS)
$(B)/snap/orchard-snap: $(snap_OBJ)
	$(snap_CC) $^ -o $@ $(snap_LIBS)
$(B)/phone/orchard: $(phone_OBJ)
	$(phone_CC) $^ -o $@ $(phone_LIBS)
	llvm-strip -o $@.stripped $@

sdl: $(B)/sdl/orchard
snap: $(B)/snap/orchard-snap
phone: $(B)/phone/orchard
clean:
	rm -rf $(B)
.PHONY: sdl snap phone clean
