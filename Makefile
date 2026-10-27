# SPDX-License-Identifier: CC0-1.0
#
#   make linux      native Linux binaries, no wine
#   make linux32    32-bit native Linux, for a 32-bit SCCore.dll and for
#                   running under box86 on 32-bit ARM
#   make lv2        the LV2 instrument plugin (needs lv2-dev)
#   make lv2-32     the same, 32-bit, for a 32-bit SCCore.dll
#   make clap       CLAP plugin for Windows (64-bit) and for Linux
#   make clap-win32 CLAP plugin for 32-bit Windows
#   make clap-32    CLAP plugin for 32-bit Linux
#   make windows    64-bit Windows PE binaries via mingw-w64
#   make windows32  32-bit Windows PE binaries, for the 32-bit SCCore.dll
#   make daemon32   32-bit MIDI daemon; needs a 32-bit alsa-lib
#   make tones32    32-bit scva-tones
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

LINUX_BINS   := $(BUILD)/scva-render $(BUILD)/scva-daemon
LINUX32_BINS := $(BUILD)/scva-render32
LV2_BUNDLE   := $(BUILD)/scva.lv2
LV2_BUNDLE32 := $(BUILD)/scva32.lv2
WINDOWS_BINS := $(BUILD)/scva-render.exe $(BUILD)/scva-winmidi.exe
WIN32_BINS   := $(BUILD)/scva-render32.exe $(BUILD)/scva-winmidi32.exe

all: linux windows
linux: $(LINUX_BINS)
linux32: $(LINUX32_BINS)
lv2: $(LV2_BUNDLE)/scva.so $(LV2_BUNDLE)/scva_ui.so
lv2-32: $(LV2_BUNDLE32)/scva.so $(LV2_BUNDLE32)/scva_ui.so
clap: $(BUILD)/scva.clap $(BUILD)/scva-win64.clap
clap-win32: $(BUILD)/scva-win32.clap
tones: $(BUILD)/scva-tones
tones32: $(BUILD)/scva-tones32
# separate from linux32: it needs a 32-bit alsa-lib, which a multilib
# workstation usually lacks and a buildroot target has
daemon32: $(BUILD)/scva-daemon32
windows: $(WINDOWS_BINS)
windows32: $(WIN32_BINS)

$(BUILD):
	@mkdir -p $(BUILD)

# scva-render --play needs ALSA. scva-render32 does not link it - it exists
# for a 32-bit SCCore.dll and for box86 on 32-bit ARM, both places a 32-bit
# alsa-lib is unlikely to be installed, the same reason daemon32 is kept out
# of linux32 - so --play is a --out-and-play-the-file error there instead.
$(BUILD)/scva-render: src/scva_native.c src/map_policy.c src/map_policy.h src/render_loop.c src/render_loop.h src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                      src/midi_song.h src/core_path.h src/scva_core_api.h src/scva_core.h src/midi_reset.h src/scva_pcm.h | $(BUILD)
	$(CC) $(CFLAGS) -DSCVA_HAVE_PCM -o $@ src/scva_native.c src/map_policy.c src/render_loop.c src/pe_loader.c \
	      $(ALSA) -lpthread -lm

$(BUILD)/scva-render32: src/scva_native.c src/map_policy.c src/map_policy.h src/render_loop.c src/render_loop.h src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                        src/midi_song.h src/core_path.h src/scva_core_api.h src/scva_core.h src/midi_reset.h src/scva_map.h | $(BUILD)
	$(CC) -m32 $(CFLAGS) -o $@ src/scva_native.c src/map_policy.c src/render_loop.c src/pe_loader.c -lpthread -lm

# ---- the engine library --------------------------------------------------
# What the CLAP and LV2 plugins share - opening the core, driving it, the
# patch lists - archived once per toolchain and linked statically into each
# plugin. The 64-bit Windows one has no PE loader: LoadLibrary takes that core.

ENGINE_DEPS := src/scva_engine.c src/scva_engine.h src/synth.h src/synth_scva.c src/midi_reset.h \
               src/map_policy.c src/map_policy.h \
               src/scva_core_api.h src/scva_core.h src/scva_lists.c \
               src/scva_lists.h src/scva_map.h \
               src/scva_names.h src/pe_loader.c src/pe_loader.h src/win9x.h \
               src/sse3_shim.h src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h
ENGINE       := $(BUILD)/engine/linux64/libscvaengine.a
ENGINE32     := $(BUILD)/engine/linux32/libscvaengine.a
ENGINE_WIN64 := $(BUILD)/engine/win64/libscvaengine.a
ENGINE_WIN32 := $(BUILD)/engine/win32/libscvaengine.a
MINGW_AR   ?= $(MINGW:gcc=ar)
MINGW32_AR ?= $(MINGW32:gcc=ar)

$(ENGINE): $(ENGINE_DEPS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/scva_engine.o src/scva_engine.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/synth_scva.o src/synth_scva.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/map_policy.o src/map_policy.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/pe_loader.o src/pe_loader.c
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/scva_lists.o src/scva_lists.c
	$(AR) rcs $@ $(@D)/scva_engine.o $(@D)/synth_scva.o $(@D)/map_policy.o $(@D)/pe_loader.o $(@D)/scva_lists.o

$(ENGINE32): $(ENGINE_DEPS)
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/scva_engine.o src/scva_engine.c
	$(CC) -m32 $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/synth_scva.o src/synth_scva.c
	$(CC) -m32 $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/map_policy.o src/map_policy.c
	$(CC) -m32 $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/pe_loader.o src/pe_loader.c
	$(CC) -m32 $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/scva_lists.o src/scva_lists.c
	$(AR) rcs $@ $(@D)/scva_engine.o $(@D)/synth_scva.o $(@D)/map_policy.o $(@D)/pe_loader.o $(@D)/scva_lists.o

$(ENGINE_WIN64): $(ENGINE_DEPS)
	@mkdir -p $(@D)
	$(MINGW) $(WINFLAGS) -c -o $(@D)/scva_engine.o src/scva_engine.c
	$(MINGW) $(WINFLAGS) -c -o $(@D)/synth_scva.o src/synth_scva.c
	$(MINGW) $(WINFLAGS) -c -o $(@D)/map_policy.o src/map_policy.c
	$(MINGW) $(WINFLAGS) -c -o $(@D)/scva_lists.o src/scva_lists.c
	$(MINGW_AR) rcs $@ $(@D)/scva_engine.o $(@D)/synth_scva.o $(@D)/map_policy.o $(@D)/scva_lists.o

$(ENGINE_WIN32): $(ENGINE_DEPS)
	@mkdir -p $(@D)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -c -o $(@D)/scva_engine.o src/scva_engine.c
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -c -o $(@D)/synth_scva.o src/synth_scva.c
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -c -o $(@D)/map_policy.o src/map_policy.c
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -c -o $(@D)/pe_loader.o src/pe_loader.c
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -c -o $(@D)/scva_lists.o src/scva_lists.c
	$(MINGW32_AR) rcs $@ $(@D)/scva_engine.o $(@D)/synth_scva.o $(@D)/map_policy.o $(@D)/pe_loader.o $(@D)/scva_lists.o

# The Linux editor, shared by the LV2 UI and the CLAP: libX11 and nothing
# else. 32-bit hosts rarely have the libX11 development link, so the 32-bit
# build names the library itself.
XUI    := $(BUILD)/engine/linux64/libscvaxui.a
XUI32  := $(BUILD)/engine/linux32/libscvaxui.a
X11    ?= -lX11
X11_32 ?= -l:libX11.so.6

$(XUI): src/scva_xui.c src/scva_xui.h src/scva_lists.h src/scva_names.h \
        src/scva_map.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/scva_xui.o src/scva_xui.c
	$(AR) rcs $@ $(@D)/scva_xui.o

$(XUI32): src/scva_xui.c src/scva_xui.h src/scva_lists.h src/scva_names.h \
          src/scva_map.h
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -fPIC -fvisibility=hidden -c -o $(@D)/scva_xui.o src/scva_xui.c
	$(AR) rcs $@ $(@D)/scva_xui.o

# ---- the CLAP plugin -----------------------------------------------------
# CLAP because its SDK is MIT and this project is CC0; VST3's is GPLv3 or
# proprietary. Point CLAP_DIR at a checkout's include/ if it is not installed.

$(BUILD)/scva.clap: src/scva_clap.c $(ENGINE) $(XUI) | $(BUILD)
	$(CC) $(CFLAGS) -I$(CLAP_DIR) -fPIC -fvisibility=hidden -shared -o $@ \
	      src/scva_clap.c $(XUI) $(ENGINE) $(X11) -lpthread -lm

.PHONY: clap-32
clap-32: $(BUILD)/scva32.clap

$(BUILD)/scva32.clap: src/scva_clap.c $(ENGINE32) $(XUI32) | $(BUILD)
	$(CC) -m32 $(CFLAGS) -I$(CLAP_DIR) -fPIC -fvisibility=hidden -shared -o $@ \
	      src/scva_clap.c $(XUI32) $(ENGINE32) $(X11_32) -lpthread -lm

$(BUILD)/scva-tones: src/scva_tones.c src/scva_names.h | $(BUILD)
	$(CC) $(CFLAGS) -Isrc -o $@ src/scva_tones.c

$(BUILD)/scva-win64.clap: src/scva_clap.c $(ENGINE_WIN64) | $(BUILD)
	$(MINGW) $(WINFLAGS) -I$(CLAP_DIR) -shared -o $@ src/scva_clap.c \
	   $(ENGINE_WIN64) -lgdi32

# Built like scva-render32.exe, and for the same reason: the plugin maps the
# core with src/pe_loader.c, so it runs on Windows 98 and needs no MSVCR100.
$(BUILD)/scva-win32.clap: src/scva_clap.c $(ENGINE_WIN32) | $(BUILD)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -I$(CLAP_DIR) -shared -o $@ \
	   src/scva_clap.c $(ENGINE_WIN32) -lgdi32

# ---- the LV2 plugin ------------------------------------------------------
# A bundle is a directory: the shared object plus its Turtle. The plugin's
# architecture has to match the host's, and the core's has to match the
# plugin's, so there is a 32-bit bundle too.

$(LV2_BUNDLE)/scva.so: src/scva_lv2.c $(ENGINE) lv2/manifest.ttl lv2/scva.ttl | $(BUILD)
	@mkdir -p $(LV2_BUNDLE)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -shared -o $@ src/scva_lv2.c $(ENGINE) -lpthread -lm
	@cp lv2/manifest.ttl lv2/scva.ttl $(LV2_BUNDLE)/

$(LV2_BUNDLE)/scva_ui.so: src/scva_lv2_ui.c $(XUI) $(ENGINE) | $(BUILD)
	@mkdir -p $(LV2_BUNDLE)
	$(CC) $(CFLAGS) -fPIC -fvisibility=hidden -shared -o $@ src/scva_lv2_ui.c $(XUI) $(ENGINE) $(X11)

$(LV2_BUNDLE32)/scva_ui.so: src/scva_lv2_ui.c $(XUI32) $(ENGINE32) | $(BUILD)
	@mkdir -p $(LV2_BUNDLE32)
	$(CC) -m32 $(CFLAGS) -fPIC -fvisibility=hidden -shared -o $@ src/scva_lv2_ui.c $(XUI32) \
	      $(ENGINE32) $(X11_32)

$(LV2_BUNDLE32)/scva.so: src/scva_lv2.c $(ENGINE32) lv2/manifest.ttl lv2/scva.ttl | $(BUILD)
	@mkdir -p $(LV2_BUNDLE32)
	$(CC) -m32 $(CFLAGS) -fPIC -fvisibility=hidden -shared -o $@ src/scva_lv2.c $(ENGINE32) \
	      -lpthread -lm
	@cp lv2/manifest.ttl lv2/scva.ttl $(LV2_BUNDLE32)/

$(BUILD)/scva-daemon: src/scva_daemon.c src/map_policy.c src/map_policy.h src/console.c src/console.h src/rt_priority.c src/rt_priority.h src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                      src/core_path.h src/scva_core_api.h src/scva_core.h src/midi_reset.h src/scva_map.h src/scva_pcm.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ src/scva_daemon.c src/map_policy.c src/console.c src/rt_priority.c src/pe_loader.c \
	      $(ALSA) -lpthread -lm

$(BUILD)/scva-daemon32: src/scva_daemon.c src/map_policy.c src/map_policy.h src/console.c src/console.h src/rt_priority.c src/rt_priority.h src/pe_loader.c src/pe_loader.h src/sse3_shim.h \
                    src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                        src/core_path.h src/scva_core_api.h src/scva_core.h src/midi_reset.h src/scva_map.h src/scva_pcm.h | $(BUILD)
	$(CC) -m32 $(CFLAGS) -o $@ src/scva_daemon.c src/map_policy.c src/console.c src/rt_priority.c src/pe_loader.c \
	      $(ALSA32) -lpthread -lm

$(BUILD)/scva-tones32: src/scva_tones.c src/scva_names.h | $(BUILD)
	$(CC) -m32 $(CFLAGS) -Isrc -o $@ src/scva_tones.c

$(BUILD)/scva-render.exe: src/scva_render.c src/map_policy.c src/map_policy.h src/audio_waveout.c src/audio_waveout.h src/render_loop.c src/render_loop.h src/midi_song.h src/core_path.h src/scva_core_api.h src/scva_core.h src/midi_reset.h | $(BUILD)
	$(MINGW) $(WINFLAGS) $(WINXPFLAGS) -o $@ src/scva_render.c src/map_policy.c src/audio_waveout.c src/render_loop.c -lwinmm

$(BUILD)/scva-winmidi.exe: src/scva_winmidi.c src/map_policy.c src/map_policy.h src/audio_waveout.c src/audio_waveout.h src/console.c src/console.h src/rt_priority.c src/rt_priority.h src/live_sched.c src/live_sched.h src/core_path.h src/scva_core_api.h src/scva_core.h src/midi_reset.h src/scva_map.h \
                           | $(BUILD)
	$(MINGW) $(WINFLAGS) $(WINXPFLAGS) -o $@ src/scva_winmidi.c src/map_policy.c src/audio_waveout.c src/console.c src/rt_priority.c src/live_sched.c -lwinmm

$(BUILD)/scva-winmidi32.exe: src/scva_winmidi.c src/map_policy.c src/map_policy.h src/audio_waveout.c src/audio_waveout.h src/console.c src/console.h src/rt_priority.c src/rt_priority.h src/live_sched.c src/live_sched.h src/pe_loader.c src/pe_loader.h \
                             src/core_path.h src/scva_core_api.h src/scva_core.h src/midi_reset.h src/scva_map.h src/win9x.h src/sse3_shim.h \
                             src/sse2_shim.h src/sse2_sites.h src/sse2_cvt.h \
                             | $(BUILD)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -o $@ src/scva_winmidi.c src/map_policy.c src/audio_waveout.c src/console.c src/rt_priority.c src/live_sched.c \
	   src/pe_loader.c -lwinmm

# ---- the 32-bit Windows path --------------------------------------------
# Same sources: the exported functions are cdecl on x86-32 and the signatures
# carry over unchanged, so only the toolchain differs.

# The 32-bit Windows binaries carry src/pe_loader.c: the system loader will
# not take a core that imports MSVCR100 and declares a subsystem newer than
# Windows 98, so the image is mapped here instead, exactly as on Linux.
$(BUILD)/scva-render32.exe: src/scva_render.c src/map_policy.c src/map_policy.h src/audio_waveout.c src/audio_waveout.h src/render_loop.c src/render_loop.h src/pe_loader.c src/pe_loader.h \
                            src/midi_song.h src/core_path.h src/scva_core_api.h src/scva_core.h src/midi_reset.h src/scva_map.h \
                            src/sse3_shim.h src/sse2_shim.h src/sse2_sites.h \
                            src/sse2_cvt.h | $(BUILD)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -o $@ src/scva_render.c src/map_policy.c src/audio_waveout.c src/render_loop.c \
	   src/pe_loader.c -lwinmm

# ---- tests ---------------------------------------------------------------
# make test-unit   module tests, built and run in both widths; no core needed
# make test-cli    every built binary's command line, up to the point where a
#                  core would be needed. Binaries not built are reported as
#                  skipped, or fail the run with SCVA_TEST_STRICT=1 (CI).
# make test        both
# tests/checkpoint.sh runs every tier this machine can run and prints a table.

UNIT_TESTS := $(patsubst tests/unit/%.c,%,$(wildcard tests/unit/test_*.c))
UNIT64     := $(addprefix $(BUILD)/test/unit64/,$(UNIT_TESTS))
UNIT32     := $(addprefix $(BUILD)/test/unit32/,$(UNIT_TESTS))
UNIT_DEPS  := tests/unit/check.h $(wildcard src/*.h) src/scva_lists.c src/map_policy.c src/render_loop.c src/console.c src/live_sched.c

$(BUILD)/test/unit64/%: tests/unit/%.c $(UNIT_DEPS)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Isrc -o $@ $< src/scva_lists.c src/map_policy.c src/render_loop.c src/console.c src/live_sched.c -lm

$(BUILD)/test/unit32/%: tests/unit/%.c $(UNIT_DEPS)
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -Isrc -o $@ $< src/scva_lists.c src/map_policy.c src/render_loop.c src/console.c src/live_sched.c -lm

test-unit: $(UNIT64) $(UNIT32)
	@tests/run-unit.sh $^

# The modules the Windows binaries compile in, built with the same toolchains
# and flags, run under wine: msvcrt is not glibc (its qsort is not stable, for
# one), and the Windows renderers depend on these behaviours too.
UNIT_WIN      := test_console test_live_sched test_map test_map_policy test_midi_song test_render_loop
UNIT_WIN64    := $(addprefix $(BUILD)/test/win64/,$(addsuffix .exe,$(UNIT_WIN)))
UNIT_WIN32    := $(addprefix $(BUILD)/test/win32/,$(addsuffix .exe,$(UNIT_WIN)))

$(BUILD)/test/win64/%.exe: tests/unit/%.c $(UNIT_DEPS)
	@mkdir -p $(@D)
	$(MINGW) $(WINFLAGS) -Isrc -o $@ $< src/map_policy.c src/render_loop.c src/console.c src/live_sched.c

$(BUILD)/test/win32/%.exe: tests/unit/%.c $(UNIT_DEPS)
	@mkdir -p $(@D)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -Isrc -o $@ $< src/map_policy.c src/render_loop.c src/console.c src/live_sched.c

test-unit-win: $(UNIT_WIN64) $(UNIT_WIN32)
	@SCVA_TEST_RUNNER=wine tests/run-unit.sh $^

test-cli:
	@tests/cli/run.sh

# The fake core (tests/fakecore/fakecore.c), in the four forms the binaries
# load: no imports for this project's PE loader on Linux, kernel32 for
# LoadLibrary and the loader on Windows. Each also comes as "-noexport",
# lacking one export, for the tools' missing-export path.
FAKE_DIR      := $(BUILD)/test/fakecore
FAKE_VARIANTS := linux64 linux32 win64 win32
FAKECORES     := $(foreach v,$(FAKE_VARIANTS),$(FAKE_DIR)/$(v)/SCCore.dll \
                                              $(FAKE_DIR)/$(v)-noexport/SCCore.dll)
FAKE_FLAGS    := -O2 -Wall -shared -nostdlib -ffreestanding -fno-builtin
fake_cc        = $(if $(findstring 32,$1),$(MINGW32),$(MINGW))
fake_entry     = $(if $(findstring 32,$1),_DllMain@12,DllMain)
fake_defs      = $(if $(findstring linux,$1),-DFAKE_LINUX) $(if $(findstring noexport,$1),-DFAKE_NO_VOICES)
fake_libs      = $(if $(findstring win,$1),-lkernel32) -lgcc

$(FAKE_DIR)/%/SCCore.dll: tests/fakecore/fakecore.c
	@mkdir -p $(@D)
	$(call fake_cc,$*) $(FAKE_FLAGS) $(call fake_defs,$*) -o $@ $< \
	   -Wl,--entry,$(call fake_entry,$*) $(call fake_libs,$*)

fakecores: $(FAKECORES)

# Minimal plugin hosts for the tests, one per width.
$(BUILD)/test/lv2_host64: tests/plugins/lv2_host.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $< -ldl -lpthread -lm

$(BUILD)/test/lv2_host32: tests/plugins/lv2_host.c
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -o $@ $< -ldl -lpthread -lm

$(BUILD)/test/clap_host64: tests/plugins/clap_host.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -I$(CLAP_DIR) -o $@ $< -ldl -lpthread -lm

$(BUILD)/test/clap_host32: tests/plugins/clap_host.c
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -I$(CLAP_DIR) -o $@ $< -ldl -lpthread -lm

$(BUILD)/test/clap_host_win64.exe: tests/plugins/clap_host.c
	@mkdir -p $(@D)
	$(MINGW) $(WINFLAGS) -I$(CLAP_DIR) -o $@ $<

$(BUILD)/test/clap_host_win32.exe: tests/plugins/clap_host.c
	@mkdir -p $(@D)
	$(MINGW32) $(WINFLAGS) -I$(CLAP_DIR) -o $@ $<

PLUGIN_HOSTS := $(BUILD)/test/lv2_host64 $(BUILD)/test/lv2_host32 \
                $(BUILD)/test/clap_host64 $(BUILD)/test/clap_host32 \
                $(BUILD)/test/clap_host_win64.exe $(BUILD)/test/clap_host_win32.exe
plugin-hosts: $(PLUGIN_HOSTS)

test-fake: $(FAKECORES) $(PLUGIN_HOSTS)
	@tests/fake/run.sh

# Needs the user's own core: SCVA_TEST_DLL_DIR=<dir with SCCore.dll>.
test-core:
	@tests/core/run.sh

$(BUILD)/test/icount: tests/perf/icount.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $<

# Instructions retired, against a baseline per core and CPU; same variables.
test-perf: $(BUILD)/test/icount $(BUILD)/test/lv2_host64
	@midi=$$(mktemp -d) && python3 tests/midi/gen.py $$midi && \
	 SCVA_TEST_MIDI=$$midi python3 tests/perf/run.py $(PERF_ARGS)

test: test-unit test-cli

clean:
	rm -rf $(BUILD)

.PHONY: all linux linux32 daemon32 lv2 lv2-32 clap clap-win32 tones tones32 windows windows32 clean \
        test test-unit test-unit-win test-cli test-fake test-core test-perf fakecores plugin-hosts
