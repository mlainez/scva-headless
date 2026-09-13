# SPDX-License-Identifier: CC0-1.0
MINGW  ?= x86_64-w64-mingw32-gcc
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
BUILD  := build

WINFLAGS := -O2 -Wall
ALSA     := $(shell pkg-config --cflags --libs alsa 2>/dev/null || echo -lasound)

all: $(BUILD)/scva_engine.exe $(BUILD)/scva_render.exe $(BUILD)/scva-daemon $(BUILD)/scva-vst.dll

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/scva_engine.exe: src/scva_engine.c src/scva_core.h | $(BUILD)
	$(MINGW) $(WINFLAGS) -o $@ $<

$(BUILD)/scva_render.exe: src/scva_render.c | $(BUILD)
	$(MINGW) $(WINFLAGS) -o $@ $<

$(BUILD)/scva-vst.dll: src/scva_vst_shim.c | $(BUILD)
	$(MINGW) -O2 -shared -o $@ $< -Wl,--export-all-symbols

$(BUILD)/scva-daemon: src/scva_daemon.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(ALSA)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
