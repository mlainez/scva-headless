# SPDX-License-Identifier: CC0-1.0
#
# Two ways to run the same engine, built separately:
#
#   make linux      native Linux binaries - pe_loader maps SCCore.dll into the
#                   process, so no wine and no Windows anywhere
#   make windows    real Windows PE binaries, via mingw-w64. They run under
#                   wine here and unchanged on Windows; nothing in them is
#                   wine-specific
#   make            both
#   make probes     the diagnostics, which need the native loader
#
MINGW  ?= x86_64-w64-mingw32-gcc
CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra
BUILD  := build

WINFLAGS := -O2 -Wall
ALSA     := $(shell pkg-config --cflags --libs alsa 2>/dev/null || echo -lasound)
SNDFILE  := $(shell pkg-config --libs sndfile 2>/dev/null)

LINUX_BINS   := $(BUILD)/scva-native $(BUILD)/scva-daemon
WINDOWS_BINS := $(BUILD)/scva_render.exe $(BUILD)/scva_engine.exe \
                $(BUILD)/scva-vst.dll
PROBES       := $(BUILD)/ring-probe $(BUILD)/overrun-probe

all: linux windows
linux: $(LINUX_BINS)
windows: $(WINDOWS_BINS)
probes: $(PROBES)

$(BUILD):
	@mkdir -p $(BUILD)

# ---- the native path: no wine -------------------------------------------
# pe_loader.c maps the Windows core into this process and binds its fifty
# imports, so these are ordinary Linux executables.

$(BUILD)/scva-native: src/scva_native.c src/pe_loader.c src/pe_loader.h \
                      src/midi_song.h src/core_path.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/scva_native.c src/pe_loader.c -lpthread -lm

$(BUILD)/scva-daemon: src/scva_daemon.c src/core_path.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $< $(ALSA)

# ---- diagnostics ---------------------------------------------------------
# Not part of either renderer. They read the engine's own state, which is
# only reachable because the native loader owns the mapping.

$(BUILD)/ring-probe: src/ring_probe.c src/pe_loader.c src/pe_loader.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/ring_probe.c src/pe_loader.c -lpthread -lm

$(BUILD)/overrun-probe: src/overrun_probe.c src/pe_loader.c src/pe_loader.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/overrun_probe.c src/pe_loader.c -lpthread -lm

# ---- the Windows path ----------------------------------------------------
# Genuine PE binaries. They load SCCore.dll the ordinary way, so they need no
# loader of ours; wine is only how this machine executes them.

$(BUILD)/scva_render.exe: src/scva_render.c src/midi_song.h src/core_path.h | $(BUILD)
	$(MINGW) $(WINFLAGS) -o $@ $<

$(BUILD)/scva_engine.exe: src/scva_engine.c src/scva_core.h | $(BUILD)
	$(MINGW) $(WINFLAGS) -o $@ $<

$(BUILD)/scva-vst.dll: src/scva_vst_shim.c | $(BUILD)
	$(MINGW) -O2 -shared -o $@ $< -Wl,--export-all-symbols

clean:
	rm -rf $(BUILD)

.PHONY: all linux windows probes clean
