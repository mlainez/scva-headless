# SPDX-License-Identifier: CC0-1.0
#
#   make linux      native Linux binaries, no wine
#   make windows    64-bit Windows PE binaries via mingw-w64
#   make windows32  32-bit Windows PE binaries, for the 32-bit SCCore.dll
#   make            linux and windows
#
# windows32 is not part of "make": it needs the i686 toolchain and a 32-bit
# core, and a 64-bit process cannot host a 32-bit DLL, so it is a separate
# build rather than a variant of the others.
#
MINGW  ?= x86_64-w64-mingw32-gcc
MINGW32 ?= i686-w64-mingw32-gcc
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
BUILD  := build

WINFLAGS := -O2 -Wall
ALSA     := $(shell pkg-config --cflags --libs alsa 2>/dev/null || echo -lasound)

LINUX_BINS   := $(BUILD)/scva-native $(BUILD)/scva-daemon
WINDOWS_BINS := $(BUILD)/scva_render.exe $(BUILD)/scva_engine.exe \
                $(BUILD)/scva-vst.dll
WIN32_BINS   := $(BUILD)/scva_render32.exe $(BUILD)/scva_engine32.exe

all: linux windows
linux: $(LINUX_BINS)
windows: $(WINDOWS_BINS)
windows32: $(WIN32_BINS)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/scva-native: src/scva_native.c src/pe_loader.c src/pe_loader.h \
                      src/midi_song.h src/core_path.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/scva_native.c src/pe_loader.c -lpthread -lm

$(BUILD)/scva-daemon: src/scva_daemon.c src/core_path.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(ALSA)

$(BUILD)/scva_render.exe: src/scva_render.c src/midi_song.h src/core_path.h | $(BUILD)
	$(MINGW) $(WINFLAGS) -o $@ $<

$(BUILD)/scva_engine.exe: src/scva_engine.c src/scva_core.h | $(BUILD)
	$(MINGW) $(WINFLAGS) -o $@ $<

$(BUILD)/scva-vst.dll: src/scva_vst_shim.c | $(BUILD)
	$(MINGW) -O2 -shared -o $@ $< -Wl,--export-all-symbols

# ---- the 32-bit Windows path --------------------------------------------
# Same sources: the exported functions are cdecl on x86-32 and the signatures
# carry over unchanged, so only the toolchain differs.

$(BUILD)/scva_render32.exe: src/scva_render.c src/midi_song.h src/core_path.h \
                            src/scva_map.h | $(BUILD)
	$(MINGW32) $(WINFLAGS) -o $@ $<

$(BUILD)/scva_engine32.exe: src/scva_engine.c src/scva_core.h src/scva_map.h | $(BUILD)
	$(MINGW32) $(WINFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD)

.PHONY: all linux windows windows32 clean
