/* SPDX-License-Identifier: CC0-1.0 */
/* Render a Standard MIDI File through SOUND Canvas VA's engine to a WAV,
 * with no wine: the core is mapped into this process by src/pe_loader.c.
 *
 *   ./build/scva-native --core dll/SCCore.dll --midi in.mid --out out.wav
 *
 * Deliberately call-for-call the same as src/scva_render.c, which runs the
 * same engine under wine. That is what makes the two comparable: any
 * difference in the audio is the loader's, not the driving.
 */
#define _GNU_SOURCE
#include "pe_loader.h"
#include "midi_song.h"
#include "core_path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The image is Microsoft x86-64, so every pointer into it is MSABI and the
   compiler emits the shuffling and the shadow space. */
typedef MSABI int (*tg_initialize_fn)(int);
typedef MSABI void (*tg_set_sample_rate_fn)(float);
typedef MSABI void (*tg_set_max_block_fn)(int);
/* rate in XMM0; second argument is the max block size */
typedef MSABI int (*tg_activate_fn)(float, int);
typedef MSABI void (*tg_deactivate_fn)(void);
typedef MSABI void (*tg_terminate_fn)(void);
/* second argument is a timestamp in samples */
typedef MSABI void (*tg_short_midi_fn)(unsigned int, int);
/* F7-terminated, so the second argument is a timestamp too */
typedef MSABI void (*tg_long_midi_fn)(const unsigned char *, int);
typedef MSABI void (*tg_process_fn)(float *, float *, int);
typedef MSABI int (*tg_running_voices_fn)(void);
typedef MSABI int (*tg_is_fatal_fn)(void);
typedef MSABI const char *(*tg_error_strings_fn)(int);
struct tg_system_config { int a, b; };
typedef MSABI int (*tg_set_config_fn)(const struct tg_system_config *);
typedef MSABI void (*tg_get_config_fn)(struct tg_system_config *);

struct api {
  tg_initialize_fn initialize;
  tg_set_sample_rate_fn set_sample_rate;
  tg_set_max_block_fn set_max_block;
  tg_activate_fn activate;
  tg_deactivate_fn deactivate;
  tg_terminate_fn terminate;
  tg_short_midi_fn short_midi;
  tg_long_midi_fn long_midi;
  tg_process_fn process;
  tg_running_voices_fn voices;
  tg_is_fatal_fn fatal;
  tg_set_config_fn set_config;
  tg_get_config_fn get_config;
  tg_error_strings_fn errors;
};

#define BLOCK 256

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

int main(int argc, char **argv)
{
  const char *core = NULL, *midi = NULL, *out = NULL, *reset = "gs";
  const char *mapname = "default";
  int mapval = 0;
  double rate = 48000.0, tail = 3.0;
  int maxblock = 4096;
  static const unsigned char gs_reset[] = {
    0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
  static const unsigned char gm_reset[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
  struct pe_image *img;
  char err[256];
  struct api a;
  struct song s;
  unsigned char *bytes;
  size_t n = 0, ei = 0;
  FILE *wav;
  float *left, *right;
  uint64_t frame = 0, at = 0;
  uint32_t total = 0, tempo = 500000;
  uint64_t last_tick = 0;
  double per_tick;
  float peak = 0.0f;
  int i, sent = 0;
  int realtime = 1;
  int dump = 0;
  int initarg = 0;
  int cfga = 1, cfgb = 1;
  uint64_t start_ms;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--midi") && i + 1 < argc) midi = argv[++i];
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
    else if (!strcmp(argv[i], "--tail") && i + 1 < argc) tail = atof(argv[++i]);
    else if (!strcmp(argv[i], "--reset") && i + 1 < argc) reset = argv[++i];
    else if (!strcmp(argv[i], "--map") && i + 1 < argc) mapname = argv[++i];
    else if (!strcmp(argv[i], "--maxblock") && i + 1 < argc) maxblock = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--dump") && i + 1 < argc) dump = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--init") && i + 1 < argc) initarg = (int)strtol(argv[++i], NULL, 0);
    else if (!strcmp(argv[i], "--cfg") && i + 2 < argc) { cfga = atoi(argv[++i]); cfgb = atoi(argv[++i]); }
    else if (!strcmp(argv[i], "--flat-out")) realtime = 0;
    else { fprintf(stderr, "usage: scva-native --core DLL --midi FILE --out FILE\n"); return 2; }
  }
  if (!midi || !out) { fprintf(stderr, "need --midi and --out\n"); return 2; }
  mapval = scva_map_value(mapname);
  if (mapval < 0) {
    fprintf(stderr, "--map wants default, 55, 88, 88pro or 8820\n");
    return 2;
  }

  core = scva_core_path(core);
  img = pe_load(core, err, sizeof err);
  if (!img) { fprintf(stderr, "pe_load(%s): %s\n", core, err); return 1; }
#define GET(field, name) \
  a.field = (void *)pe_symbol(img, name); \
  if (!a.field) { fprintf(stderr, "missing %s\n", name); return 1; }
  GET(initialize, "TG_initialize")
  GET(set_sample_rate, "TG_setSampleRate")
  GET(set_max_block, "TG_setMaxBlockSize")
  GET(activate, "TG_activate")
  GET(deactivate, "TG_deactivate")
  GET(terminate, "TG_terminate")
  GET(short_midi, "TG_ShortMidiIn")
  GET(long_midi, "TG_LongMidiIn")
  GET(process, "TG_Process")
  GET(voices, "TG_XPgetCurTotalRunningVoices")
  GET(fatal, "TG_isFatalError")
  GET(set_config, "TG_XPsetSystemConfig")
  GET(get_config, "TG_XPgetCurSystemConfig")
  GET(errors, "TG_getErrorStrings")
#undef GET

  bytes = slurp(midi, &n);
  if (!bytes || !parse(&s, bytes, n)) { fprintf(stderr, "not a MIDI file this can play\n"); return 1; }

  printf("initialize(%d) -> %d\n", initarg, a.initialize(initarg));
  /* THE ORDER. See README: the rate is set on both sides of the block size and
     the second call is the last thing before activate. */
  a.set_sample_rate((float)rate);
  a.set_max_block(maxblock);
  printf("rate %.0f, max block %d (rate set both sides)\n", rate, maxblock);
  {
    struct tg_system_config cfg;
    cfg.a = cfga; cfg.b = cfgb;
    printf("set_config(1,1) -> %d\n", a.set_config(&cfg));
    memset(&cfg, 0, sizeof cfg);
    a.get_config(&cfg);
    printf("config now: %d, %d\n", cfg.a, cfg.b);
  }
  a.set_sample_rate((float)rate);
  {
    int act = a.activate((float)rate, maxblock);
    int fat = a.fatal();
    printf("activate -> %d, fatal %d\n", act, fat);
    fflush(stdout);
  }
  if (!strcmp(reset, "gs")) { a.long_midi(gs_reset, 0);
    printf("gs reset sent\n"); fflush(stdout); }
  else if (!strcmp(reset, "gm")) { a.long_midi(gm_reset, 0);
    printf("gm reset sent\n"); fflush(stdout); }
  /* a program change latches CC32, so set it before the music starts */
  { int ch; for (ch = 0; ch < 16; ++ch)
      a.short_midi(0xb0u | (unsigned)ch | (0x20u << 8) |
                    ((unsigned)mapval << 16), 0);
    printf("tone map %s (CC32=%d)\n", mapname, mapval); fflush(stdout); }

  left = calloc((size_t)maxblock, sizeof *left);
  right = calloc((size_t)maxblock, sizeof *right);
  if (!left || !right) { fprintf(stderr, "out of memory\n"); return 1; }

  wav = fopen(out, "wb");
  if (!wav) { fprintf(stderr, "cannot write %s\n", out); return 1; }
  header(wav, (unsigned)rate, 0);
  per_tick = rate * (double)tempo / (1e6 * s.division);
  start_ms = now_ms();

  while (ei < s.count || frame < at + (uint64_t)(tail * rate)) {
    int k;
    while (ei < s.count) {
      const struct event *e = s.ev + ei;
      uint64_t when = at + (uint64_t)((double)(e->tick - last_tick) * per_tick);
      if (when > frame + BLOCK) break;
      at = when;
      last_tick = e->tick;
      if (e->tempo) {
        tempo = e->tempo;
        per_tick = rate * (double)tempo / (1e6 * s.division);
      } else if (e->sysex_len) {
        unsigned char sx[260];
        int sn = sysex_message(e, sx, sizeof sx);
        if (sn) { a.long_midi(sx, 0); ++sent; }
      } else {
        unsigned int msg = (unsigned int)e->status |
          ((unsigned int)e->data1 << 8) | ((unsigned int)e->data2 << 16);
        /* a program change latches the map, so set CC32 on that part first */
        if (mapval && (e->status & 0xf0) == 0xc0)
          a.short_midi(0xb0u | (unsigned)(e->status & 0x0f) |
                       (0x20u << 8) | ((unsigned)mapval << 16), 0);
        a.short_midi(msg, 0);
        ++sent;
      }
      ++ei;
    }
    memset(left, 0, (size_t)maxblock * sizeof *left);
    memset(right, 0, (size_t)maxblock * sizeof *right);
    /* TG_Process generates on demand, so --flat-out is correct and faster */
    if (realtime) {
      double audio_ms = 1000.0 * (double)frame / rate;
      for (;;) {
        double elapsed = (double)(now_ms() - start_ms);
        if (elapsed >= audio_ms) break;
        nap(audio_ms - elapsed > 4.0 ? 2 : 1);
      }
    }
    a.process(left, right, BLOCK);
    for (k = 0; k < BLOCK; ++k) {
      float l = left[k] < 0 ? -left[k] : left[k];
      float r = right[k] < 0 ? -right[k] : right[k];
      if (l > peak) peak = l;
      if (r > peak) peak = r;
      if (dump && (int)(total + k) < dump)
        printf("  [%5u] L %.9g  R %.9g\n", total + k, left[k], right[k]);
      fwrite(left + k, 4, 1, wav);
      fwrite(right + k, 4, 1, wav);
    }
    frame += BLOCK;
    total += BLOCK;
  }
  rewind(wav);
  header(wav, (unsigned)rate, total);
  fclose(wav);
  /* TG_terminate calls exit(), so print first */
  printf("%s: %u frames at %.0f Hz, %.1f s, peak %.5f, %d messages, voices %d\n",
         out, total, rate, total / rate, peak, sent, a.voices());
  fflush(stdout);
  a.deactivate();
  a.terminate();
  return 0;
}
