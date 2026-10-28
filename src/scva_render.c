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
 *
 * --engine picks another engine this build has (synth_registry.h), which
 * brings its own options.
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
#include "render_engines.h"
#include "synth_registry.h"
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

/* SOUND Canvas VA's core, driven through its own API as this renderer
   always has: its setup reported, and terminated last. The first engine is
   loaded before the song is read. */
static struct scva_core_api core_api[2];
#ifdef SCVA_OWN_LOADER
static const scva_core_lookup_fn core_lookup = scva_core_lookup_pe;
#else
static const scva_core_lookup_fn core_lookup = scva_core_lookup_module;
static HMODULE core_b;
#endif
static char core_b_copy[MAX_PATH];     /* port B's copy of the core, if any */

static int core_voices(struct render_engines *e) { (void)e; return core_api[0].voices(); }

static void core_close(struct render_engines *e)
{
  if (e->n > 1) {
    core_api[1].deactivate();
#ifndef SCVA_OWN_LOADER
    FreeLibrary(core_b);
#endif
    DeleteFileA(core_b_copy);
  }
  core_api[0].deactivate();
  core_api[0].terminate();
}

static int core_load(const char *core)
{
  const char *missing;
#ifdef SCVA_OWN_LOADER
  char peerr[256];
  /* pe_load maps the image, binds the runtime the core wants and applies the
     SSE shims, so nothing here depends on what the system loader will accept. */
  struct pe_image *lib = scva_core_map(core, peerr, sizeof peerr);
  if (!lib) { fprintf(stderr, "pe_load(%s): %s\n", core, peerr); return 0; }
#else
  HMODULE lib = LoadLibraryA(core);
  if (!lib) { fprintf(stderr, "LoadLibrary(%s) failed: %lu\n", core, GetLastError()); return 0; }
  {
    int n = scva_sse3_apply(lib);
    if (n) printf("SSE3 shim: %d sites patched\n", n);
  }
#endif
  if ((missing = scva_core_bind(&core_api[0], core_lookup, (void *)lib, SCVA_CORE_ALL))) {
    fprintf(stderr, "missing %s\n", missing);
    return 0;
  }
  return 1;
}

/* The setters dereference state that initialize creates - calling them
   first faults on a null pointer - so the order is fixed: initialize, then
   rate and block size, then activate. */
static int core_open(struct render_engines *e, const char *core, int nports,
                     double rate, int maxblock, const char *reset, int mapval,
                     const char *mapname, const char *argv0)
{
  struct scva_core_api *a = &core_api[0], *b = &core_api[1];
  const char *missing;
  int i;

  memset(e, 0, sizeof *e);
  e->n = 1;
  if (nports > 1) {
#ifdef SCVA_OWN_LOADER
    char peerr[256];
    void *lib_b = scva_core_map(core, peerr, sizeof peerr);   /* a second mapping */
#else
    void *lib_b = core_b = load_second_core(core, core_b_copy, sizeof core_b_copy);
#endif
    if (!lib_b) {
      fprintf(stderr, "cannot open a second core for port B\n");
      return 0;
    }
    if ((missing = scva_core_bind(b, core_lookup, lib_b, SCVA_CORE_ALL))) {
      fprintf(stderr, "missing %s\n", missing);
      return 0;
    }
    e->n = 2;
    if (nports > 2)
      fprintf(stderr, "this song names %d ports; the engine is a two-port "
                      "machine here, so everything past A shares port B\n",
              nports);
    printf("two-port song: second core from %s\n", core_b_copy);
  }

  if (maxblock < SCVA_MIN_BLOCK) {
    fprintf(stderr, "%s: --maxblock %d is below the engine's minimum, "
                    "using %d\n", argv0, maxblock, SCVA_MIN_BLOCK);
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
    scva_core_start(a, &setup, &report);
    printf("initialize -> %d\n", report.initialized);
    printf("rate %.0f, max block %d (rate set both sides)\n", rate, maxblock);
    printf("set_config(1,1) -> %d\n", report.config_set);
    printf("config now: %d, %d\n", report.config.a, report.config.b);
    printf("activate -> %d, fatal %d\n", report.activated, report.fatal);
    for (k = 0; k < 8; ++k) {
      const char *msg = a->errors(k);
      if (msg && *msg && (unsigned char)*msg >= 32)
        printf("  error string %d: %s\n", k, msg);
    }
    fflush(stdout);
    if (e->n > 1) scva_core_start(b, &setup, NULL);
  }
  if (!strcmp(reset, "gs")) { scva_core_gs_reset(a);
    if (e->n > 1) scva_core_gs_reset(b);
    printf("gs reset sent\n"); fflush(stdout); }
  else if (!strcmp(reset, "gm")) { scva_core_gm_reset(a);
    if (e->n > 1) scva_core_gm_reset(b);
    printf("gm reset sent\n"); fflush(stdout); }
  for (i = 0; i < 2; ++i) {
    map_policy_init(&e->maps[i], MAP_RENDER, mapval, scva_core_sink, &core_api[i]);
    e->ports[i].maps = &e->maps[i];
    e->ports[i].long_msg = scva_core_long_sink;
    e->ports[i].process = scva_core_process_sink;
    e->ports[i].ctx = &core_api[i];
  }
  /* a program change latches CC32, so set it before the music starts */
  { int ch; for (ch = 0; ch < 16; ++ch) {
      a->short_midi(scva_map_cc(ch, mapval), 0);
      if (e->n > 1) b->short_midi(scva_map_cc(ch, mapval), 0);
    }
    printf("tone map %s (CC32=%d)\n", mapname, mapval); fflush(stdout); }
  e->rate = rate;
  e->midi_block = 1;
  e->voices = core_voices;
  e->close = core_close;
  return 1;
}

static void usage(void)
{
  int i;
  fprintf(stderr, "usage: scva-render --core DLL --midi FILE --out FILE\n"
                  "  --bits 16   integer PCM every player accepts\n"
                  "  --bits 32   float, the engine's own format (default)\n"
                  "  --play      straight to the sound card, no file and\n"
                  "              no MIDI driver anywhere in the way\n");
  if (synth_backends[1]) {
    fprintf(stderr, "  --engine %s  the engine (default %s)\n",
            synth_engine_names(), synth_backends[0]->caps->name);
    for (i = 1; synth_backends[i]; ++i) {
      fprintf(stderr, "with --engine %s, --core is its engine file, and:\n",
              synth_backends[i]->caps->name);
      synth_print_options(synth_backends[i], stderr);
    }
  }
}

int main(int argc, char **argv)
{
  const char *core = NULL, *midi = NULL, *out = NULL, *reset = "gs";
  const char *mapname = "default";
  int mapval = 0;
  double rate = 48000.0, tail = 3.0;
  int rate_given = 0;
  int maxblock = 4096;
  char err[256];
  const struct synth_backend *engine = synth_backends[0];
  void *engine_opts = NULL;
  struct render_engines e;
  struct render_io io;
  struct render_result res;
  struct sink o;
  struct song s;
  unsigned char *bytes;
  size_t n = 0;
  int bits = 32;                     /* --bits 16 for integer PCM */
  int play = 0;                      /* --play: to the sound card, no file */
  int i, block;
  int realtime = 1;

  /* The engine first: the rest of the command line may be its options. */
  for (i = 1; i + 1 < argc; ++i)
    if (!strcmp(argv[i], "--engine")) {
      engine = synth_find(argv[i + 1]);
      if (!engine) {
        fprintf(stderr, "--engine takes %s\n", synth_engine_names());
        return 2;
      }
    }
  if (engine->opts_size) {
    engine_opts = calloc(1, engine->opts_size);
    if (!engine_opts) { fprintf(stderr, "out of memory\n"); return 1; }
    engine->opts_init(engine_opts);
  }

  for (i = 1; i < argc; ++i) {
    int took;
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--engine") && i + 1 < argc) ++i;
    else if (!strcmp(argv[i], "--midi") && i + 1 < argc) midi = argv[++i];
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) { rate = atof(argv[++i]); rate_given = 1; }
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
    else if ((took = synth_parse_option(engine, engine_opts, argc, argv, &i,
                                        err, sizeof err)) < 0) {
      fprintf(stderr, "%s\n", err);
      return 2;
    }
    else if (!took) { usage(); return 2; }
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
  if (engine != &synth_scva) {
    if (mapval && !engine->caps->tone_map_per_part) {
      fprintf(stderr, "--engine %s has one tone set for every part, chosen with "
                      "its own options, not --map:\n", engine->caps->name);
      synth_print_options(engine, stderr);
      return 2;
    }
    if (!core) {
      fprintf(stderr, "--engine %s needs --core, its engine file\n", engine->caps->name);
      return 2;
    }
    if (!rate_given && engine->caps->default_rate) rate = engine->caps->default_rate;
    if (!synth_check_rate(engine, rate, err, sizeof err)) {
      fprintf(stderr, "%s\n", err);
      return 2;
    }
  } else {
    core = scva_core_path(core);
    if (!core_load(core)) return 1;
  }

  bytes = slurp(midi, &n);
  if (!bytes || !parse(&s, bytes, n)) { fprintf(stderr, "not a MIDI file this can play\n"); return 1; }

  if (engine == &synth_scva) {
    if (!core_open(&e, core, song_ports(&s), rate, maxblock, reset, mapval, mapname,
                   argv[0]))
      return 1;
  } else {
    struct synth_options opt;
    opt.core = core;
    opt.rate = rate;
    opt.max_block = RENDER_BLOCK;
    opt.backend_opts = engine_opts;
    if (!render_engines_synth(&e, engine, &opt, song_ports(&s) > 1 ? 2 : 1, reset,
                              err, sizeof err)) {
      fprintf(stderr, "%s\n", err);
      return 1;
    }
    rate = e.rate;
    printf("%s engine, %.0f Hz, MIDI on %d-frame blocks\n", engine->caps->name,
           rate, e.midi_block);
    fflush(stdout);
  }
  block = e.midi_block > 1 ? e.midi_block : RENDER_BLOCK;

  memset(&o, 0, sizeof o);
  o.bits = bits;
  o.realtime = realtime;
  o.rate = rate;
  if (play) {
    /* Windows offers no way for a program to join the MIDI device list, so
       rather than route a file through a driver and a virtual cable, the
       renderer reads it and puts the audio out itself. waveOut in 16-bit
       stereo is what every Windows sound device accepts. */
    int nbuf = (int)((double)200.0 * rate / 1000.0 / block);   /* ~200 ms ahead */
    if (nbuf < 4) nbuf = 4;
    if (nbuf > 64) nbuf = 64;
    switch (waveout_open(&o.out, (unsigned)rate, nbuf, block)) {
    case WAVEOUT_NO_DEVICE:
      fprintf(stderr, "cannot open an audio device at %.0f Hz\n", rate);
      return 1;
    case WAVEOUT_NO_MEMORY:
      fprintf(stderr, "out of memory\n");
      return 1;
    }
    o.play = 1;
    printf("playing, %d x %d frames buffered (%.0f ms)\n",
           nbuf, block, 1000.0 * nbuf * block / rate);
    fflush(stdout);
  } else {
    o.wav = fopen(out, "wb");
    if (!o.wav) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    header(o.wav, (unsigned)rate, 0, bits);
  }

  io.pace = pace;
  io.deliver = deliver;
  io.ctx = &o;
  o.start_ms = GetTickCount();
  if (render_song(&s, rate, tail, e.midi_block, e.ports, e.n, &io, &res) < 0) {
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
  printf("%s: %u frames at %.0f Hz, %.1f s, peak %.5f, %d messages",
         play ? "played" : out, res.frames, rate, res.frames / rate, res.peak,
         res.sent);
  if ((i = e.voices(&e)) >= 0) printf(", voices %d", i);
  printf("\n");
  fflush(stdout);
  e.close(&e);
  free(engine_opts);
  return 0;
}
