/* SPDX-License-Identifier: CC0-1.0 */
/* Render a Standard MIDI File through SOUND Canvas VA's engine to a WAV,
 * with no wine: the core is mapped into this process by src/pe_loader.c.
 *
 *   ./build/scva-render --core dll/SCCore.dll --midi in.mid --out out.wav
 *
 * Deliberately call-for-call the same as src/scva_render.c, which runs the
 * same engine under wine. That is what makes the two comparable: any
 * difference in the audio is the loader's, not the driving.
 */
#define _GNU_SOURCE
#include "pe_loader.h"
#include "midi_song.h"
#include "core_path.h"
#ifdef SCVA_HAVE_PCM
#include "scva_pcm.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "scva_core.h"
#include "scva_map.h"
#include "map_policy.h"
#include "render_loop.h"

static uint64_t now_ms(void)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000u + (uint64_t)(t.tv_nsec / 1000000);
}

static void nap(unsigned ms)
{
  struct timespec t;
  t.tv_sec = ms / 1000u;
  t.tv_nsec = (long)(ms % 1000u) * 1000000L;
  nanosleep(&t, NULL);
}

/* Where the audio goes and how fast. */
struct sink {
  FILE *wav;
  int bits;
  int dump;                          /* print this many first samples */
  int realtime;                      /* hold to the wall clock */
  double rate;
  uint64_t start_ms;
#ifdef SCVA_HAVE_PCM
  snd_pcm_t *pcm;
  float *inter;
  long underruns;
#endif
};

/* TG_Process generates on demand, so --flat-out is correct and faster */
static void pace(void *ctx, uint64_t frame)
{
  struct sink *o = ctx;
  double audio_ms = 1000.0 * (double)frame / o->rate;
  if (!o->realtime) return;
  for (;;) {
    double elapsed = (double)(now_ms() - o->start_ms);
    if (elapsed >= audio_ms) break;
    nap(audio_ms - elapsed > 4.0 ? 2 : 1);
  }
}

static int deliver(void *ctx, const float *left, const float *right, int n,
                   uint64_t frame)
{
  struct sink *o = ctx;
  int k;
  for (k = 0; k < n; ++k) {
    if (o->dump && frame + (uint64_t)k < (uint64_t)o->dump)
      printf("  [%5u] L %.9g  R %.9g\n", (uint32_t)frame + k, left[k], right[k]);
    if (o->wav) put_frame(o->wav, left[k], right[k], o->bits);
#ifdef SCVA_HAVE_PCM
    if (o->inter) { o->inter[2 * k] = left[k]; o->inter[2 * k + 1] = right[k]; }
#endif
  }
#ifdef SCVA_HAVE_PCM
  if (o->pcm) {
    snd_pcm_sframes_t w = snd_pcm_writei(o->pcm, o->inter, (snd_pcm_uframes_t)n);
    if (w < 0) {
      ++o->underruns;
      if (snd_pcm_recover(o->pcm, (int)w, 1) < 0) return 0;
    }
  }
#endif
  return 1;
}

int main(int argc, char **argv)
{
  const char *core = NULL, *midi = NULL, *out = NULL, *reset = "gs";
  const char *mapname = "default";
  int mapval = 0;
  double rate = 48000.0, tail = 3.0;
  int maxblock = 4096;
  struct pe_image *img;
  char err[256];
  const char *missing;
  struct scva_core_api a, b;
  struct map_policy maps_a, maps_b;
  struct render_port ports[2];
  struct render_io io;
  struct render_result res;
  struct sink o;
  int have_b = 0;
  struct song s;
  unsigned char *bytes;
  size_t n = 0;
  int bits = 32;                     /* --bits 16 for integer PCM */
  int i, nports;
  int realtime = 1;
  int dump = 0;
  int initarg = 0;
  int cfga = 1, cfgb = 1;
  int play = 0;                      /* --play: straight to the sound card */
#ifdef SCVA_HAVE_PCM
  const char *pcm_name = NULL;       /* --pcm DEV, otherwise auto-detected */
  const char *pcm_opened = NULL;
  unsigned int latency_us = 20000;
#endif

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
    else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--init") && i + 1 < argc) initarg = (int)strtol(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--cfg") && i + 2 < argc) { cfga = atoi(argv[++i]); cfgb = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--flat-out")) realtime = 0;
    else if (!strcmp(argv[i], "--play")) play = 1;
#ifdef SCVA_HAVE_PCM
    else if (!strcmp(argv[i], "--pcm") && i + 1 < argc) pcm_name = argv[++i];
    else if (!strcmp(argv[i], "--latency") && i + 1 < argc)
      latency_us = (unsigned)atoi(argv[++i]) * 1000u;
#endif
    else { fprintf(stderr, "usage: scva-render --core DLL --midi FILE --out FILE\n"
                     "  --bits 16   integer PCM every player accepts\n"
                     "  --bits 32   float, the engine's own format (default)\n"
                     "  --play      straight to the sound card, no file and\n"
                     "              no MIDI driver anywhere in the way\n"
                     "  --pcm DEV   force one PCM device; otherwise detected\n"
                     "  --latency MS  delay before a note is heard "
                     "(default 20)\n"); return 2; }
  }
  if (!midi || (!out && !play)) {
    fprintf(stderr, "need --midi, and --out FILE or --play\n");
    return 2;
  }
#ifndef SCVA_HAVE_PCM
  if (play) {
    fprintf(stderr, "this build has no --play: it was built without ALSA "
                    "(scva-render32 usually has none). Use --out and play "
                    "the file, or build scva-render instead.\n");
    return 2;
  }
#endif
  mapval = scva_map_value(mapname);
  if (mapval < 0) {
    fprintf(stderr, "--map wants default, 55, 88, 88pro or 8820\n");
    return 2;
  }

  core = scva_core_path(core);
  img = pe_load(core, err, sizeof err);
  if (!img) { fprintf(stderr, "pe_load(%s): %s\n", core, err); return 1; }
  if ((missing = scva_core_bind(&a, scva_core_lookup_pe, img, SCVA_CORE_ALL))) {
    fprintf(stderr, "missing %s\n", missing);
    return 1;
  }

  bytes = slurp(midi, &n);
  if (!bytes || !parse(&s, bytes, n)) { fprintf(stderr, "not a MIDI file this can play\n"); return 1; }

  nports = song_ports(&s);
  if (nports > 1) {
    /* Two-port performance. The core is a fixed 16-part machine, so port B
       needs an engine of its own; the loader maps a second copy of the image
       with its own globals. */
    struct pe_image *img2 = pe_load(core, err, sizeof err);
    if (!img2) { fprintf(stderr, "pe_load(port B): %s\n", err); return 1; }
    if ((missing = scva_core_bind(&b, scva_core_lookup_pe, img2, SCVA_CORE_ALL))) {
      fprintf(stderr, "missing %s\n", missing);
      return 1;
    }
    have_b = 1;
    if (nports > 2)
      fprintf(stderr, "this song names %d ports; the engine is a two-port "
                      "machine here, so everything past A shares port B\n",
              nports);
    printf("two-port song: second engine at %p\n", (void *)img2->base);
  }

  if (maxblock < SCVA_MIN_BLOCK) {
    fprintf(stderr, "%s: --maxblock %d is below the engine's minimum, "
                    "using %d\n", argv[0], maxblock, SCVA_MIN_BLOCK);
    maxblock = SCVA_MIN_BLOCK;
  }
  {
    struct scva_core_setup setup;
    struct scva_core_report report;
    setup.rate = (float)rate;
    setup.max_block = maxblock;
    setup.init_arg = initarg;
    setup.config.a = cfga;
    setup.config.b = cfgb;
    scva_core_start(&a, &setup, &report);
    printf("initialize(%d) -> %d\n", initarg, report.initialized);
    printf("rate %.0f, max block %d (rate set both sides)\n", rate, maxblock);
    printf("set_config(1,1) -> %d\n", report.config_set);
    printf("config now: %d, %d\n", report.config.a, report.config.b);
    printf("activate -> %d, fatal %d\n", report.activated, report.fatal);
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
  o.dump = dump;
  o.rate = rate;
#ifdef SCVA_HAVE_PCM
  if (play) {
    o.inter = calloc((size_t)RENDER_BLOCK * 2, sizeof *o.inter);
    if (!o.inter) { fprintf(stderr, "out of memory\n"); return 1; }
    o.pcm = open_pcm(pcm_name, (unsigned)rate, latency_us, &pcm_opened, 0);
    if (!o.pcm) {
      fprintf(stderr, "no PCM device would take float32 stereo at %.0f Hz",
              rate);
      if (pcm_name) fprintf(stderr, " ('%s')", pcm_name);
      fprintf(stderr, "\n");
      return 1;
    }
    printf("playing on %s\n", pcm_opened);
    /* The PCM device's own blocking write sets the pace, the same way
       scva-daemon lets the card be the clock, so the wall-clock wait is for
       --out alone and would only fight it here. */
    realtime = 0;
  }
#endif
  if (out) {
    o.wav = fopen(out, "wb");
    if (!o.wav) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    header(o.wav, (unsigned)rate, 0, bits);
  }
  o.realtime = realtime;

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
  o.start_ms = now_ms();
  if (render_song(&s, rate, tail, ports, have_b ? 2 : 1, &io, &res) < 0) {
    fprintf(stderr, "out of memory\n");
    return 1;
  }
  if (o.wav) {
    rewind(o.wav);
    header(o.wav, (unsigned)rate, res.frames, bits);
    fclose(o.wav);
  }
  /* TG_terminate calls exit(), so print first */
  printf("%s: %u frames at %.0f Hz, %.1f s, peak %.5f, %d messages, voices %d\n",
         out ? out : "(play)", res.frames, rate, res.frames / rate, res.peak,
         res.sent, a.voices());
#ifdef SCVA_HAVE_PCM
  if (o.underruns)
    printf("%ld dropout%s on the PCM device - raise --latency\n",
           o.underruns, o.underruns == 1 ? "" : "s");
#endif
  fflush(stdout);
#ifdef SCVA_HAVE_PCM
  if (o.pcm) snd_pcm_close(o.pcm);
  free(o.inter);
#endif
  if (have_b) b.deactivate();
  a.deactivate();
  a.terminate();
  return 0;
}
