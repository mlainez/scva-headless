# SPDX-License-Identifier: CC0-1.0
#
#   make linux      native Linux binaries, no wine
#   make windows    Windows PE binaries via mingw-w64
#   make            both
#
MINGW  ?= x86_64-w64-mingw32-gcc
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
BUILD  := build

WINFLAGS := -O2 -Wall
ALSA     := $(shell pkg-config --cflags --libs alsa 2>/dev/null || echo -lasound)

LINUX_BINS   := $(BUILD)/scva-native $(BUILD)/scva-daemon
WINDOWS_BINS := $(BUILD)/scva_render.exe $(BUILD)/scva_engine.exe \
                $(BUILD)/scva-vst.dll

all: linux windows
linux: $(LINUX_BINS)
windows: $(WINDOWS_BINS)

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

clean:
	rm -rf $(BUILD)

.PHONY: all linux windows clean
