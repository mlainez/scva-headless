/* SPDX-License-Identifier: CC0-1.0 */
/* Render a Standard MIDI File through SOUND Canvas VA's engine to a WAV.
 *
 *   wine scva-render.exe --core SCCore.dll --midi in.mid --out out.wav
 *                        [--rate 48000] [--tail 3] [--reset gs|gm|none]
 *
 * This is an **oracle wrapper**: it is the wall between the reference
 * implementation and this project's own work. What crosses the wall is
 * audio, and nothing else. SOUND Canvas VA is a later Roland product, not an
 * SC-88 - `M-001` showed its data blob holds the same samples re-packed - so
 * its output ranks as a proxy, below hardware recordings and below firmware.
 *
 * The engine is driven through its own exported C API rather than through the
 * VST wrapper, which needs a registry key and a GUI stack neither of which a
 * headless render wants. The signatures below were read from the exported
 * functions' prologues - which registers each one consumes before writing -
 * so they are the calling convention and not an algorithm.
 */
#include <windows.h>
#include "midi_song.h"
#include "core_path.h"
#if defined(__i386__)
/* The 32-bit core imports MSVCR100 and declares a subsystem newer than
   Windows 98, so the system loader refuses it there. src/pe_loader.c maps it
   and supplies that runtime itself, which is the same thing it does on Linux,
   and it applies the SSE shims on the way. */
#define SCVA_OWN_LOADER 1
#include "pe_loader.h"
#include "scva_runtime.h"
#else
#define SCVA_SHIM_HOST_APPLY 1     /* the system loader path applies them */
#include "sse3_shim.h"
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scva_core.h"
#include "scva_map.h"
#include "map_policy.h"
#include "render_loop.h"
#include "audio_waveout.h"

/* Windows refcounts modules by path: a second LoadLibrary of the same file
   hands back the first module, globals and all. A copy under another name
   loads as a separate module - the same trick Roland uses by shipping 32
   byte-different cores in the Mac build. Our own loader has no such rule, so
   where it is used the second engine is simply a second mapping. */
#ifndef SCVA_OWN_LOADER
static HMODULE load_second_core(const char *core, char *made, size_t madelen)
{
  char dir[MAX_PATH];
  FILE *in, *out;
  unsigned char buf[65536];
  size_t n;
  HMODULE h;
  if (!GetTempPathA(sizeof dir, dir)) return NULL;
  snprintf(made, madelen, "%sscva_portB_%lu.dll", dir,
           (unsigned long)GetCurrentProcessId());
  in = fopen(core, "rb");
  if (!in) return NULL;
  out = fopen(made, "wb");
  if (!out) { fclose(in); return NULL; }
  while ((n = fread(buf, 1, sizeof buf, in)) > 0)
    if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return NULL; }
  fclose(in);
  fclose(out);
  h = LoadLibraryA(made);
  if (!h) DeleteFileA(made);
  else scva_sse3_apply(h);
  return h;
}
#endif

/* Where the audio goes and how fast. */
struct sink {
  FILE *wav;
  int bits;
  int realtime;                      /* hold to the wall clock */
  double rate;
  DWORD start_ms;
  int play;
  struct waveout out;                /* --play */
  int wb;                            /* the next buffer to fill */
};

static void pace(void *ctx, uint64_t frame)
{
  struct sink *o = ctx;
  if (o->play) {
    /* waveOut is the clock now: wait for a buffer to come free */
    while (!waveout_is_free(&o->out, o->wb)) waveout_wait(&o->out, 100);
  } else if (o->realtime) {
    /* TG_Process generates on demand, so --flat-out is correct and faster */
    double audio_ms = 1000.0 * (double)frame / o->rate;
    for (;;) {
      double elapsed = (double)(GetTickCount() - o->start_ms);
      if (elapsed >= audio_ms) break;
      Sleep(audio_ms - elapsed > 4.0 ? 2 : 1);
    }
  }
  if (!frame) { printf("first process...\n"); fflush(stdout); }
}

static int deliver(void *ctx, const float *left, const float *right, int n,
                   uint64_t frame)
{
  struct sink *o = ctx;
  int k;
  if (!frame) { printf("first process returned\n"); fflush(stdout); }
  for (k = 0; k < n; ++k) {
    if (o->play) {
      o->out.pcm[o->wb][2 * k]     = to_s16(left[k]);
      o->out.pcm[o->wb][2 * k + 1] = to_s16(right[k]);
    } else {
      put_frame(o->wav, left[k], right[k], o->bits);
    }
  }
  if (o->play) {
    waveout_write(&o->out, o->wb);
    o->wb = (o->wb + 1) % o->out.nbuf;
  }
  return 1;
}

int main(int argc, char **argv)
{
  const char *core = NULL, *midi = NULL, *out = NULL, *reset = "gs";
  const char *mapname = "default";
  int mapval = 0;
  double rate = 48000.0, tail = 3.0;
  int maxblock = 4096;
#ifdef SCVA_OWN_LOADER
  struct pe_image *lib = NULL, *lib_b = NULL;
  char peerr[256];
  scva_core_lookup_fn lookup = scva_core_lookup_pe;
#else
  HMODULE lib, lib_b = NULL;
  scva_core_lookup_fn lookup = scva_core_lookup_module;
#endif
  const char *missing;
  struct scva_core_api a, b;
  struct map_policy maps_a, maps_b;
  struct render_port ports[2];
  struct render_io io;
  struct render_result res;
  struct sink o;
  int have_b = 0;
  char coreb[MAX_PATH] = "";
  struct song s;
  unsigned char *bytes;
  size_t n = 0;
  int bits = 32;                     /* --bits 16 for integer PCM */
  int play = 0;                      /* --play: to the sound card, no file */
  int i, nports;
  int realtime = 1;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--midi") && i + 1 < argc) midi = argv[++i];
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
    else if (!strcmp(argv[i], "--tail") && i + 1 < argc) tail = atof(argv[++i]);
    else if (!strcmp(argv[i], "--bits") && i + 1 < argc) {
      bits = atoi(argv[++i]);
      if (bits != 16 && bits != 32) {
        fprintf(stderr, "--bits takes 16 or 32\n");
        return 2;
      }
    }
    else if (!strcmp(argv[i], "--reset") && i + 1 < argc) reset = argv[++i];
    else if (!strcmp(argv[i], "--map") && i + 1 < argc) mapname = argv[++i];
    else if (!strcmp(argv[i], "--maxblock") && i + 1 < argc) maxblock = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--flat-out")) realtime = 0;
    else if (!strcmp(argv[i], "--play")) play = 1;
    else { fprintf(stderr, "usage: scva-render --core DLL --midi FILE --out FILE\n"
                     "  --bits 16   integer PCM every player accepts\n"
                     "  --bits 32   float, the engine's own format (default)\n"
                     "  --play      straight to the sound card, no file and\n"
                     "              no MIDI driver anywhere in the way\n"); return 2; }
  }
  if (!midi || (!out && !play)) {
    fprintf(stderr, "need --midi, and --out FILE or --play\n");
    return 2;
  }
  mapval = scva_map_value(mapname);
  if (mapval < 0) {
    fprintf(stderr, "--map wants default, 55, 88, 88pro or 8820\n");
    return 2;
  }

  core = scva_core_path(core);
#ifdef SCVA_OWN_LOADER
  /* pe_load maps the image, binds the runtime the core wants and applies the
     SSE shims, so nothing here depends on what the system loader will accept. */
  lib = scva_core_map(core, peerr, sizeof peerr);
  if (!lib) { fprintf(stderr, "pe_load(%s): %s\n", core, peerr); return 1; }
#else
  lib = LoadLibraryA(core);
  if (!lib) { fprintf(stderr, "LoadLibrary(%s) failed: %lu\n", core, GetLastError()); return 1; }
  {
    int n = scva_sse3_apply(lib);
    if (n) printf("SSE3 shim: %d sites patched\n", n);
  }
#endif
  if ((missing = scva_core_bind(&a, lookup, (void *)lib, SCVA_CORE_ALL))) {
    fprintf(stderr, "missing %s\n", missing);
    return 1;
  }

  bytes = slurp(midi, &n);
  if (!bytes || !parse(&s, bytes, n)) { fprintf(stderr, "not a MIDI file this can play\n"); return 1; }

  /* get_config before initialize removed: under test */

  /* The setters dereference state that initialize creates - calling them
     first faults on a null pointer - so the order is fixed: initialize,
     then rate and block size, then activate. */
  nports = song_ports(&s);
  if (nports > 1) {
#ifdef SCVA_OWN_LOADER
    lib_b = scva_core_map(core, peerr, sizeof peerr);       /* a second mapping */
#else
    lib_b = load_second_core(core, coreb, sizeof coreb);
#endif
    if (!lib_b) {
      fprintf(stderr, "cannot open a second core for port B\n");
      return 1;
    }
    if ((missing = scva_core_bind(&b, lookup, (void *)lib_b, SCVA_CORE_ALL))) {
      fprintf(stderr, "missing %s\n", missing);
      return 1;
    }
    have_b = 1;
    if (nports > 2)
      fprintf(stderr, "this song names %d ports; the engine is a two-port "
                      "machine here, so everything past A shares port B\n",
              nports);
    printf("two-port song: second core from %s\n", coreb);
  }

  if (maxblock < SCVA_MIN_BLOCK) {
    fprintf(stderr, "%s: --maxblock %d is below the engine's minimum, "
                    "using %d\n", argv[0], maxblock, SCVA_MIN_BLOCK);
    maxblock = SCVA_MIN_BLOCK;
  }
  {
    struct scva_core_setup setup;
    struct scva_core_report report;
    int k;
    setup.rate = (float)rate;
    setup.max_block = maxblock;
    setup.init_arg = 0;
    setup.config.a = 1;
    setup.config.b = 1;
    scva_core_start(&a, &setup, &report);
    printf("initialize -> %d\n", report.initialized);
    printf("rate %.0f, max block %d (rate set both sides)\n", rate, maxblock);
    printf("set_config(1,1) -> %d\n", report.config_set);
    printf("config now: %d, %d\n", report.config.a, report.config.b);
    printf("activate -> %d, fatal %d\n", report.activated, report.fatal);
    for (k = 0; k < 8; ++k) {
      const char *msg = a.errors(k);
      if (msg && *msg && (unsigned char)*msg >= 32)
        printf("  error string %d: %s\n", k, msg);
    }
    fflush(stdout);
    if (have_b) scva_core_start(&b, &setup, NULL);
  }
  if (!strcmp(reset, "gs")) { scva_core_gs_reset(&a);
    if (have_b) scva_core_gs_reset(&b);
    printf("gs reset sent\n"); fflush(stdout); }
  else if (!strcmp(reset, "gm")) { scva_core_gm_reset(&a);
    if (have_b) scva_core_gm_reset(&b);
    printf("gm reset sent\n"); fflush(stdout); }
  map_policy_init(&maps_a, MAP_RENDER, mapval, scva_core_sink, &a);
  map_policy_init(&maps_b, MAP_RENDER, mapval, scva_core_sink, &b);
  /* a program change latches CC32, so set it before the music starts */
  { int ch; for (ch = 0; ch < 16; ++ch) {
      a.short_midi(scva_map_cc(ch, mapval), 0);
      if (have_b) b.short_midi(scva_map_cc(ch, mapval), 0);
    }
    printf("tone map %s (CC32=%d)\n", mapname, mapval); fflush(stdout); }

  memset(&o, 0, sizeof o);
  o.bits = bits;
  o.realtime = realtime;
  o.rate = rate;
  if (play) {
    /* Windows offers no way for a program to join the MIDI device list, so
       rather than route a file through a driver and a virtual cable, the
       renderer reads it and puts the audio out itself. waveOut in 16-bit
       stereo is what every Windows sound device accepts. */
    int nbuf = (int)((double)200.0 * rate / 1000.0 / RENDER_BLOCK);   /* ~200 ms ahead */
    if (nbuf < 4) nbuf = 4;
    if (nbuf > 64) nbuf = 64;
    switch (waveout_open(&o.out, (unsigned)rate, nbuf, RENDER_BLOCK)) {
    case WAVEOUT_NO_DEVICE:
      fprintf(stderr, "cannot open an audio device at %.0f Hz\n", rate);
      return 1;
    case WAVEOUT_NO_MEMORY:
      fprintf(stderr, "out of memory\n");
      return 1;
    }
    o.play = 1;
    printf("playing, %d x %d frames buffered (%.0f ms)\n",
           nbuf, RENDER_BLOCK, 1000.0 * nbuf * RENDER_BLOCK / rate);
    fflush(stdout);
  } else {
    o.wav = fopen(out, "wb");
    if (!o.wav) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    header(o.wav, (unsigned)rate, 0, bits);
  }

  ports[0].maps = &maps_a;
  ports[0].long_msg = scva_core_long_sink;
  ports[0].process = scva_core_process_sink;
  ports[0].ctx = &a;
  ports[1] = ports[0];
  ports[1].maps = &maps_b;
  ports[1].ctx = &b;
  io.pace = pace;
  io.deliver = deliver;
  io.ctx = &o;
  o.start_ms = GetTickCount();
  if (render_song(&s, rate, tail, ports, have_b ? 2 : 1, &io, &res) < 0) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  if (play) {
    waveout_drain_close(&o.out);             /* let the tail drain */
  } else {
    rewind(o.wav);
    header(o.wav, (unsigned)rate, res.frames, bits);
    fclose(o.wav);
  }
  /* TG_terminate calls exit(), so print first */
  printf("%s: %u frames at %.0f Hz, %.1f s, peak %.5f, %d messages, voices %d\n",
         play ? "played" : out, res.frames, rate, res.frames / rate, res.peak,
         res.sent, a.voices());
  fflush(stdout);
  if (have_b) {
    b.deactivate();
#ifdef SCVA_OWN_LOADER
    (void)coreb;
#else
    FreeLibrary(lib_b);
#endif
    DeleteFileA(coreb);
  }
  a.deactivate();
  a.terminate();
  return 0;
}
