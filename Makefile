# SPDX-License-Identifier: CC0-1.0
#
#   make linux      native Linux binaries, no wine
#   make linux32    32-bit native Linux, for a 32-bit SCCore.dll and for
#                   running under box86 on 32-bit ARM
#   make lv2        the LV2 instrument plugin (needs lv2-dev)
#   make lv2-32     the same, 32-bit, for a 32-bit SCCore.dll
#   make clap       CLAP plugin for Windows (64-bit) and for Linux
#   make clap-win32 CLAP plugin for 32-bit Windows
#   make windows    64-bit Windows PE binaries via mingw-w64
#   make windows32  32-bit Windows PE binaries, for the 32-bit SCCore.dll
#   make daemon32   32-bit MIDI daemon; needs a 32-bit alsa-lib
#   make            linux and windows
#
# A 64-bit process cannot host a 32-bit DLL, so the 32-bit targets are separate
# builds rather than variants: pick the one that matches your core. Neither is
# part of "make" - linux32 needs libc6-dev-i386 and windows32 the i686 mingw
# toolchain.
#
MINGW  ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc
# header-only, MIT: https://github.com/free-audio/clap
CLAP_DIR ?= /usr/include
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
BUILD  := build

WINFLAGS := -O2 -Wall
# The 64-bit Windows binaries target Windows XP x64, which is subsystem 5.02.
# The 64-bit core itself declares 6.0 and imports the Universal CRT, so what
# actually runs there is decided by the core, not by this flag.
WINXPFLAGS := -Wl,--major-subsystem-version=5 -Wl,--minor-subsystem-version=2
# The 32-bit Windows binaries target Windows 98. Subsystem 4.0 is what a Win9x
# loader accepts, and msvcrt's own stdio is used instead of mingw's, whose
# printf drags in GetModuleHandleW and the MultiByteToWideChar pair - all three
# are stubs on Win9x. Nothing here prints anything C89 cannot express.
# -mno-sse keeps our own floating point on the x87, so these binaries need
# nothing newer than a Pentium Pro; what the machine must have is decided by
# the core, and src/sse3_shim.h lowers that.
WIN98FLAGS := -D_WIN32_WINNT=0x0400 -DWINVER=0x0400 -D__USE_MINGW_ANSI_STDIO=0 \
              -march=i686 -mno-sse -mfpmath=387 \
              -Wl,--major-subsystem-version=4 -Wl,--minor-subsystem-version=0
ALSA     := $(shell pkg-config --cflags --libs alsa 2>/dev/null || echo -lasound)
# needs a 32-bit alsa-lib; buildroot and multilib distributions both provide one
ALSA32   := $(shell PKG_CONFIG_PATH=/usr/lib32/pkgconfig:/usr/lib/i386-linux-gnu/pkgconfig \
              pkg-config --cflags --libs alsa 2>/dev/null || echo -lasound)

LINUX_BINS   := $(BUILD)/scva-native $(BUILD)/scva-daemon
LINUX32_BINS := $(BUILD)/scva-native32
LV2_BUNDLE   := $(BUILD)/scva.lv2
LV2_BUNDLE32 := $(BUILD)/scva32.lv2
WINDOWS_BINS := $(BUILD)/scva_render.exe $(BUILD)/scva-vst.dll \
                $(BUILD)/scva-winmidi.exe
WIN32_BINS   := $(BUILD)/scva_render32.exe $(BUILD)/scva-winmidi32.exe

all: linux windows
linux: $(LINUX_BINS)
linux32: $(LINUX32_BINS)
lv2: $(LV2_BUNDLE)/scva.so
lv2-32: $(LV2_BUNDLE32)/scva.so
clap: $(BUILD)/scva.clap $(BUILD)/scva-win64.clap
clap-win32: $(BUILD)/scva-win32.clap
tones: $(BUILD)/scva-tones
# separate from linux32: it needs a 32-bit alsa-lib, which a multilib
# workstation usually lacks and a buildroot target has
daemon32: $(BUILD)/scva-daemon32
windows: $(WINDOWS_BINS)
windows32: $(WIN32_BINS)

$(BUILD):
	@mkdir -p $(BUILD)

# scva-native --play needs ALSA. scva-native32 does not link it - it exists
# for a 32-bit SCCore.dll and for box86 on 32-bit ARM, both places a 32-bit
# alsa-lib is unlikely to be installed, the same reason daemon32 is kept out
# of linux32 - so --play is a --out-and-play-the-file error there instead.
$(BUILD)/scva-native: src/scva_native.c src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                      src/midi_song.h src/core_path.h src/scva_pcm.h | $(BUILD)
	$(CC) $(CFLAGS) -DSCVA_HAVE_PCM -o $@ src/scva_native.c src/pe_loader.c \
	      $(ALSA) -lpthread -lm

$(BUILD)/scva-native32: src/scva_native.c src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                        src/midi_song.h src/core_path.h src/scva_map.h | $(BUILD)
	$(CC) -m32 $(CFLAGS) -o $@ src/scva_native.c src/pe_loader.c -lpthread -lm

# ---- the CLAP plugin -----------------------------------------------------
# CLAP because its SDK is MIT and this project is CC0; VST3's is GPLv3 or
# proprietary. Point CLAP_DIR at a checkout's include/ if it is not installed.

$(BUILD)/scva.clap: src/scva_clap.c src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                    src/scva_map.h src/scva_names.h | $(BUILD)
	$(CC) $(CFLAGS) -I$(CLAP_DIR) -fPIC -shared -o $@ \
	      src/scva_clap.c src/pe_loader.c -lpthread -lm

$(BUILD)/scva-tones: src/scva_tones.c src/scva_names.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ src/scva_tones.c

$(BUILD)/scva-win64.clap: src/scva_clap.c src/scva_map.h src/scva_names.h | $(BUILD)
	$(MINGW) $(WINFLAGS) -I$(CLAP_DIR) -shared -o $@ src/scva_clap.c

$(BUILD)/scva-win32.clap: src/scva_clap.c src/scva_map.h src/scva_names.h | $(BUILD)
	$(MINGW32) $(WINFLAGS) -I$(CLAP_DIR) -shared -o $@ src/scva_clap.c

# ---- the LV2 plugin ------------------------------------------------------
# A bundle is a directory: the shared object plus its Turtle. The plugin's
# architecture has to match the host's, and the core's has to match the
# plugin's, so there is a 32-bit bundle too.

$(LV2_BUNDLE)/scva.so: src/scva_lv2.c src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                       src/scva_map.h src/scva_names.h lv2/manifest.ttl lv2/scva.ttl | $(BUILD)
	@mkdir -p $(LV2_BUNDLE)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ src/scva_lv2.c src/pe_loader.c \
	      -lpthread -lm
	@cp lv2/manifest.ttl lv2/scva.ttl $(LV2_BUNDLE)/

$(LV2_BUNDLE32)/scva.so: src/scva_lv2.c src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                         src/scva_map.h src/scva_names.h lv2/manifest.ttl lv2/scva.ttl | $(BUILD)
	@mkdir -p $(LV2_BUNDLE32)
	$(CC) -m32 $(CFLAGS) -fPIC -shared -o $@ src/scva_lv2.c src/pe_loader.c \
	      -lpthread -lm
	@cp lv2/manifest.ttl lv2/scva.ttl $(LV2_BUNDLE32)/

$(BUILD)/scva-daemon: src/scva_daemon.c src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                      src/core_path.h src/scva_map.h src/scva_pcm.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/scva_daemon.c src/pe_loader.c \
	      $(ALSA) -lpthread -lm

$(BUILD)/scva-daemon32: src/scva_daemon.c src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                        src/core_path.h src/scva_map.h src/scva_pcm.h | $(BUILD)
	$(CC) -m32 $(CFLAGS) -o $@ src/scva_daemon.c src/pe_loader.c \
	      $(ALSA32) -lpthread -lm

$(BUILD)/scva_render.exe: src/scva_render.c src/midi_song.h src/core_path.h | $(BUILD)
	$(MINGW) $(WINFLAGS) $(WINXPFLAGS) -o $@ $< -lwinmm

$(BUILD)/scva-winmidi.exe: src/scva_winmidi.c src/core_path.h src/scva_map.h \
                           | $(BUILD)
	$(MINGW) $(WINFLAGS) $(WINXPFLAGS) -o $@ src/scva_winmidi.c -lwinmm

$(BUILD)/scva-winmidi32.exe: src/scva_winmidi.c src/pe_loader.c src/pe_loader.h \
                             src/core_path.h src/scva_map.h src/win9x.h src/sse3_shim.h \
                             src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                             | $(BUILD)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -o $@ src/scva_winmidi.c \
	   src/pe_loader.c -lwinmm

$(BUILD)/scva-vst.dll: src/scva_vst_shim.c | $(BUILD)
	$(MINGW) -O2 -shared -o $@ $< -Wl,--export-all-symbols

# ---- the 32-bit Windows path --------------------------------------------
# Same sources: the exported functions are cdecl on x86-32 and the signatures
# carry over unchanged, so only the toolchain differs.

# The 32-bit Windows binaries carry src/pe_loader.c: the system loader will
# not take a core that imports MSVCR100 and declares a subsystem newer than
# Windows 98, so the image is mapped here instead, exactly as on Linux.
$(BUILD)/scva_render32.exe: src/scva_render.c src/pe_loader.c src/pe_loader.h \
                            src/midi_song.h src/core_path.h src/scva_map.h \
                            src/sse3_shim.h src/sse2_shim.h src/sse2_sites.h \
                            src/sse2_cvt.h | $(BUILD)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -o $@ src/scva_render.c \
	   src/pe_loader.c -lwinmm

clean:
	rm -rf $(BUILD)

.PHONY: all linux linux32 daemon32 lv2 lv2-32 clap clap-win32 tones windows windows32 clean
