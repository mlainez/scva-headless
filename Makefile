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
ALSA_CFLAGS := $(shell pkg-config --cflags alsa 2>/dev/null)
ALSA        := $(shell pkg-config --libs alsa 2>/dev/null || echo -lasound)
# needs a 32-bit alsa-lib; buildroot and multilib distributions both provide one
PKG32       := PKG_CONFIG_PATH=/usr/lib32/pkgconfig:/usr/lib/i386-linux-gnu/pkgconfig
ALSA32_CFLAGS := $(shell $(PKG32) pkg-config --cflags alsa 2>/dev/null)
ALSA32      := $(shell $(PKG32) pkg-config --libs alsa 2>/dev/null || echo -lasound)

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

# ---- objects --------------------------------------------------------------
# Every source is compiled once per toolchain into build/obj/<toolchain>/, and
# the compiler records which headers each object read, so a header change
# rebuilds exactly what uses it. The -pic toolchains are for the plugins.

OBJ      := $(BUILD)/obj
DEPS     := -MMD -MP
objs      = $(addprefix $(OBJ)/$1/,$(addsuffix .o,$2))
PICFLAGS := -fPIC -fvisibility=hidden

$(OBJ)/linux64/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(ALSA_CFLAGS) $(XDEFS) $(DEPS) -c -o $@ $<

$(OBJ)/linux32/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) $(ALSA32_CFLAGS) $(XDEFS) $(DEPS) -c -o $@ $<

$(OBJ)/linux64-pic/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(PICFLAGS) $(XDEFS) $(DEPS) -c -o $@ $<

$(OBJ)/linux32-pic/%.o: src/%.c
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) $(PICFLAGS) $(XDEFS) $(DEPS) -c -o $@ $<

$(OBJ)/win64/%.o: src/%.c
	@mkdir -p $(@D)
	$(MINGW) $(WINFLAGS) $(XDEFS) $(DEPS) -c -o $@ $<

$(OBJ)/win32/%.o: src/%.c
	@mkdir -p $(@D)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) $(XDEFS) $(DEPS) -c -o $@ $<

# scva-render --play needs ALSA. scva-render32 does not link it - it exists
# for a 32-bit SCCore.dll and for box86 on 32-bit ARM, both places a 32-bit
# alsa-lib is unlikely to be installed, the same reason daemon32 is kept out
# of linux32 - so --play is a --out-and-play-the-file error there instead.
$(OBJ)/linux64/scva_native.o: XDEFS := -DSCVA_HAVE_PCM
$(OBJ)/linux64-pic/scva_clap.o $(OBJ)/linux32-pic/scva_clap.o \
$(OBJ)/win64/scva_clap.o $(OBJ)/win32/scva_clap.o: XDEFS := -I$(CLAP_DIR)

-include $(wildcard $(OBJ)/*/*.d)

# Objects reached only through a pattern rule are kept, not deleted as
# intermediates, so a rebuild recompiles only what changed.
.SECONDARY:

# ---- the tools -----------------------------------------------------------

RENDER_MODS     := scva_native map_policy render_loop pe_loader scva_runtime
DAEMON_MODS     := scva_daemon map_policy console rt_priority pe_loader scva_runtime
WIN_RENDER_MODS := scva_render map_policy audio_waveout render_loop
WINMIDI_MODS    := scva_winmidi map_policy audio_waveout console rt_priority live_sched

$(BUILD)/scva-render: $(call objs,linux64,$(RENDER_MODS))
	$(CC) $(CFLAGS) -o $@ $^ $(ALSA) -lpthread -lm

$(BUILD)/scva-render32: $(call objs,linux32,$(RENDER_MODS))
	$(CC) -m32 $(CFLAGS) -o $@ $^ -lpthread -lm

$(BUILD)/scva-daemon: $(call objs,linux64,$(DAEMON_MODS))
	$(CC) $(CFLAGS) -o $@ $^ $(ALSA) -lpthread -lm

$(BUILD)/scva-daemon32: $(call objs,linux32,$(DAEMON_MODS))
	$(CC) -m32 $(CFLAGS) -o $@ $^ $(ALSA32) -lpthread -lm

$(BUILD)/scva-tones: $(call objs,linux64,scva_tones)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/scva-tones32: $(call objs,linux32,scva_tones)
	$(CC) -m32 $(CFLAGS) -o $@ $^

$(BUILD)/scva-render.exe: $(call objs,win64,$(WIN_RENDER_MODS))
	$(MINGW) $(WINFLAGS) $(WINXPFLAGS) -o $@ $^ -lwinmm

$(BUILD)/scva-winmidi.exe: $(call objs,win64,$(WINMIDI_MODS))
	$(MINGW) $(WINFLAGS) $(WINXPFLAGS) -o $@ $^ -lwinmm

# ---- the 32-bit Windows path --------------------------------------------
# Same sources: the exported functions are cdecl on x86-32 and the signatures
# carry over unchanged, so only the toolchain differs.

# The 32-bit Windows binaries carry src/pe_loader.c: the system loader will
# not take a core that imports MSVCR100 and declares a subsystem newer than
# Windows 98, so the image is mapped here instead, exactly as on Linux.
$(BUILD)/scva-render32.exe: $(call objs,win32,$(WIN_RENDER_MODS) pe_loader scva_runtime)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -o $@ $^ -lwinmm

$(BUILD)/scva-winmidi32.exe: $(call objs,win32,$(WINMIDI_MODS) pe_loader scva_runtime)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -o $@ $^ -lwinmm

# ---- the engine library --------------------------------------------------
# What the CLAP and LV2 plugins share - opening the core, driving it, the
# patch lists - archived once per toolchain and linked statically into each
# plugin. The 64-bit Windows one has no PE loader: LoadLibrary takes that core.

ENGINE_MODS  := scva_engine synth_scva map_policy scva_lists
ENGINE       := $(BUILD)/engine/linux64/libscvaengine.a
ENGINE32     := $(BUILD)/engine/linux32/libscvaengine.a
ENGINE_WIN64 := $(BUILD)/engine/win64/libscvaengine.a
ENGINE_WIN32 := $(BUILD)/engine/win32/libscvaengine.a
MINGW_AR   ?= $(MINGW:gcc=ar)
MINGW32_AR ?= $(MINGW32:gcc=ar)

$(ENGINE): $(call objs,linux64-pic,$(ENGINE_MODS) pe_loader scva_runtime)
	@mkdir -p $(@D)
	@rm -f $@
	$(AR) rcs $@ $^

$(ENGINE32): $(call objs,linux32-pic,$(ENGINE_MODS) pe_loader scva_runtime)
	@mkdir -p $(@D)
	@rm -f $@
	$(AR) rcs $@ $^

$(ENGINE_WIN64): $(call objs,win64,$(ENGINE_MODS))
	@mkdir -p $(@D)
	@rm -f $@
	$(MINGW_AR) rcs $@ $^

$(ENGINE_WIN32): $(call objs,win32,$(ENGINE_MODS) pe_loader scva_runtime)
	@mkdir -p $(@D)
	@rm -f $@
	$(MINGW32_AR) rcs $@ $^

# The Linux editor, shared by the LV2 UI and the CLAP: libX11 and nothing
# else. 32-bit hosts rarely have the libX11 development link, so the 32-bit
# build names the library itself.
XUI    := $(BUILD)/engine/linux64/libscvaxui.a
XUI32  := $(BUILD)/engine/linux32/libscvaxui.a
X11    ?= -lX11
X11_32 ?= -l:libX11.so.6

$(XUI): $(call objs,linux64-pic,scva_xui)
	@mkdir -p $(@D)
	@rm -f $@
	$(AR) rcs $@ $^

$(XUI32): $(call objs,linux32-pic,scva_xui)
	@mkdir -p $(@D)
	@rm -f $@
	$(AR) rcs $@ $^

# ---- the CLAP plugin -----------------------------------------------------
# CLAP because its SDK is MIT and this project is CC0; VST3's is GPLv3 or
# proprietary. Point CLAP_DIR at a checkout's include/ if it is not installed.

$(BUILD)/scva.clap: $(call objs,linux64-pic,scva_clap) $(XUI) $(ENGINE)
	$(CC) $(CFLAGS) $(PICFLAGS) -shared -o $@ $^ $(X11) -lpthread -lm

.PHONY: clap-32
clap-32: $(BUILD)/scva32.clap

$(BUILD)/scva32.clap: $(call objs,linux32-pic,scva_clap) $(XUI32) $(ENGINE32)
	$(CC) -m32 $(CFLAGS) $(PICFLAGS) -shared -o $@ $^ $(X11_32) -lpthread -lm

$(BUILD)/scva-win64.clap: $(call objs,win64,scva_clap) $(ENGINE_WIN64)
	$(MINGW) $(WINFLAGS) -shared -o $@ $^ -lgdi32

# Built like scva-render32.exe, and for the same reason: the plugin maps the
# core with src/pe_loader.c, so it runs on Windows 98 and needs no MSVCR100.
$(BUILD)/scva-win32.clap: $(call objs,win32,scva_clap) $(ENGINE_WIN32)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -shared -o $@ $^ -lgdi32

# ---- the LV2 plugin ------------------------------------------------------
# A bundle is a directory: the shared object plus its Turtle. The plugin's
# architecture has to match the host's, and the core's has to match the
# plugin's, so there is a 32-bit bundle too.

$(LV2_BUNDLE)/scva.so: $(call objs,linux64-pic,scva_lv2) $(ENGINE) lv2/manifest.ttl lv2/scva.ttl
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(PICFLAGS) -shared -o $@ $(filter %.o %.a,$^) -lpthread -lm
	@cp lv2/manifest.ttl lv2/scva.ttl $(@D)/

$(LV2_BUNDLE)/scva_ui.so: $(call objs,linux64-pic,scva_lv2_ui) $(XUI) $(ENGINE)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(PICFLAGS) -shared -o $@ $^ $(X11)

$(LV2_BUNDLE32)/scva.so: $(call objs,linux32-pic,scva_lv2) $(ENGINE32) lv2/manifest.ttl lv2/scva.ttl
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) $(PICFLAGS) -shared -o $@ $(filter %.o %.a,$^) -lpthread -lm
	@cp lv2/manifest.ttl lv2/scva.ttl $(@D)/

$(LV2_BUNDLE32)/scva_ui.so: $(call objs,linux32-pic,scva_lv2_ui) $(XUI32) $(ENGINE32)
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) $(PICFLAGS) -shared -o $@ $^ $(X11_32)

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
UNIT_MODS  := scva_lists map_policy render_loop console live_sched

$(OBJ)/test64/%.o: tests/unit/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -Isrc $(DEPS) -c -o $@ $<

$(OBJ)/test32/%.o: tests/unit/%.c
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -Isrc $(DEPS) -c -o $@ $<

$(BUILD)/test/unit64/%: $(OBJ)/test64/%.o $(call objs,linux64,$(UNIT_MODS))
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ -lm

$(BUILD)/test/unit32/%: $(OBJ)/test32/%.o $(call objs,linux32,$(UNIT_MODS))
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -o $@ $^ -lm

# The loader's test reads the probe image (tests/peprobe, built below), so it
# links the loader too.
LOADER_MODS := pe_loader
PROBE_DIR := $(BUILD)/test/peprobe
PROBES    := $(foreach w,linux64 linux32,$(PROBE_DIR)/$(w)/peprobe.dll \
                                       $(PROBE_DIR)/$(w)/peprobe_packed.dll)

$(BUILD)/test/unit64/test_pe_loader: $(OBJ)/test64/test_pe_loader.o \
                                     $(call objs,linux64,$(UNIT_MODS) $(LOADER_MODS))
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread -lm

$(BUILD)/test/unit32/test_pe_loader: $(OBJ)/test32/test_pe_loader.o \
                                     $(call objs,linux32,$(UNIT_MODS) $(LOADER_MODS))
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -o $@ $^ -lpthread -lm

test-unit: $(UNIT64) $(UNIT32) $(PROBES)
	@tests/run-unit.sh $(UNIT64) $(UNIT32)

# The modules the Windows binaries compile in, built with the same toolchains
# and flags, run under wine: msvcrt is not glibc (its qsort is not stable, for
# one), and the Windows renderers depend on these behaviours too.
UNIT_WIN      := test_console test_live_sched test_map test_map_policy test_midi_song test_render_loop
UNIT_WIN_MODS := map_policy render_loop console live_sched
UNIT_WIN64    := $(addprefix $(BUILD)/test/win64/,$(addsuffix .exe,$(UNIT_WIN)))
UNIT_WIN32    := $(addprefix $(BUILD)/test/win32/,$(addsuffix .exe,$(UNIT_WIN)))

$(OBJ)/testwin64/%.o: tests/unit/%.c
	@mkdir -p $(@D)
	$(MINGW) $(WINFLAGS) -Isrc $(DEPS) -c -o $@ $<

$(OBJ)/testwin32/%.o: tests/unit/%.c
	@mkdir -p $(@D)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -Isrc $(DEPS) -c -o $@ $<

$(BUILD)/test/win64/%.exe: $(OBJ)/testwin64/%.o $(call objs,win64,$(UNIT_WIN_MODS))
	@mkdir -p $(@D)
	$(MINGW) $(WINFLAGS) -o $@ $^

$(BUILD)/test/win32/%.exe: $(OBJ)/testwin32/%.o $(call objs,win32,$(UNIT_WIN_MODS))
	@mkdir -p $(@D)
	$(MINGW32) $(WINFLAGS) $(WIN98FLAGS) -o $@ $^

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

# The loader's test image (tests/peprobe/peprobe.c), one per width, and the
# DLL it imports from, which it is linked against directly.

$(PROBE_DIR)/%/probe_dep.dll: tests/peprobe/peprobe.c
	@mkdir -p $(@D)
	$(call fake_cc,$*) $(FAKE_FLAGS) -DPROBE_DEP -o $@ $< \
	   -Wl,--entry,$(call fake_entry,$*) -lgcc

$(PROBE_DIR)/%/peprobe.dll: tests/peprobe/peprobe.c $(PROBE_DIR)/%/probe_dep.dll
	$(call fake_cc,$*) $(FAKE_FLAGS) -o $@ $< $(@D)/probe_dep.dll \
	   -Wl,--entry,$(call fake_entry,$*) -lgcc

# The same image with its sections 512 bytes apart, as a driver's are, so
# code, data and bss share pages.
$(PROBE_DIR)/%/peprobe_packed.dll: tests/peprobe/peprobe.c $(PROBE_DIR)/%/probe_dep.dll
	$(call fake_cc,$*) $(FAKE_FLAGS) -o $@ $< $(@D)/probe_dep.dll \
	   -Wl,--entry,$(call fake_entry,$*) -Wl,--section-alignment,0x200 \
	   -Wl,--file-alignment,0x200 -lgcc

peprobes: $(PROBES)

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

# The VSC backend against virtual_sc's vsc-play: SCVA_TEST_VSC_DIR (vsc.sys
# and the tone sets), SCVA_TEST_VSC_PLAY and SCVA_TEST_CORPUS. 32-bit only,
# as vsc.sys is.
VSC_MODS := synth_vsc vsc_engine pe_loader

$(OBJ)/vsctest32/%.o: tests/vsc/%.c
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -Isrc $(DEPS) -c -o $@ $<

$(BUILD)/test/vsc_drive: $(OBJ)/vsctest32/vsc_drive.o $(call objs,linux32,$(VSC_MODS))
	@mkdir -p $(@D)
	$(CC) -m32 $(CFLAGS) -o $@ $^ -lpthread -lm

test-vsc: $(BUILD)/test/vsc_drive
	@python3 tests/vsc/run.py

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
        test test-unit test-unit-win test-cli test-fake test-core test-perf fakecores plugin-hosts \
        peprobes test-vsc
