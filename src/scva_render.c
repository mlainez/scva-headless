/* SPDX-License-Identifier: CC0-1.0 */
/* Render a Standard MIDI File through SOUND Canvas VA's engine to a WAV.
 *
 *   wine scva_render.exe --core SCCore.dll --midi in.mid --out out.wav
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*tg_initialize_fn)(int);
typedef void (*tg_set_sample_rate_fn)(float);
typedef void (*tg_set_max_block_fn)(int);
/* rate is a float in XMM0; the second argument is the max block size */
typedef int (*tg_activate_fn)(float, int);
typedef void (*tg_deactivate_fn)(void);
typedef void (*tg_terminate_fn)(void);
/* second argument is a timestamp in samples, not a length or a flag */
typedef void (*tg_short_midi_fn)(unsigned int, int);
/* the message is F7-terminated, so this second argument is a timestamp */
typedef void (*tg_long_midi_fn)(const unsigned char *, int);
typedef void (*tg_process_fn)(float *, float *, int);
typedef int (*tg_running_voices_fn)(void);
typedef int (*tg_is_fatal_fn)(void);
typedef const char *(*tg_error_strings_fn)(int);
/* Reads two 32-bit fields through a pointer; the getter writes eight bytes
   back, so the configuration is exactly this pair. */
struct tg_system_config { int a, b; };
typedef int (*tg_set_config_fn)(const struct tg_system_config *);
typedef void (*tg_get_config_fn)(struct tg_system_config *);

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

/* MIDI reading and WAV writing live in midi_song.h, shared with the native
   renderer so both drive the engine from exactly the same events. */

#define BLOCK 256

int main(int argc, char **argv)
{
  const char *core = NULL, *midi = NULL, *out = NULL, *reset = "gs";
  double rate = 48000.0, tail = 3.0;
  int maxblock = 4096;
  static const unsigned char gs_reset[] = {
    0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
  static const unsigned char gm_reset[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
  HMODULE lib;
  struct api a;
  struct song s;
  unsigned char *bytes;
  size_t n = 0, ei = 0;
  FILE *wav;
  /* Sized to the declared maximum, which is what the engine's own rings are
     sized to. TG_Process writes exactly the frames it is asked for. */
  float *left, *right;
  uint64_t frame = 0, at = 0;
  uint32_t total = 0, tempo = 500000;
  uint64_t last_tick = 0;
  double per_tick;
  float peak = 0.0f;
  int i, sent = 0;
  int realtime = 1;
  DWORD start_ms;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--midi") && i + 1 < argc) midi = argv[++i];
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
    else if (!strcmp(argv[i], "--tail") && i + 1 < argc) tail = atof(argv[++i]);
    else if (!strcmp(argv[i], "--reset") && i + 1 < argc) reset = argv[++i];
    else if (!strcmp(argv[i], "--maxblock") && i + 1 < argc) maxblock = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--flat-out")) realtime = 0;
    else { fprintf(stderr, "usage: scva_render --core DLL --midi FILE --out FILE\n"); return 2; }
  }
  if (!midi || !out) { fprintf(stderr, "need --midi and --out\n"); return 2; }

  core = scva_core_path(core);
  lib = LoadLibraryA(core);
  if (!lib) { fprintf(stderr, "LoadLibrary(%s) failed: %lu\n", core, GetLastError()); return 1; }
#define GET(field, name) \
  a.field = (void *)GetProcAddress(lib, name); \
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

  /* get_config before initialize removed: under test */

  /* The setters dereference state that initialize creates - calling them
     first faults on a null pointer - so the order is fixed: initialize,
     then rate and block size, then activate. */
  printf("initialize -> %d\n", a.initialize(0));
  a.set_sample_rate((float)rate);
  a.set_max_block(maxblock);
  /* And AGAIN, after the block size. This is the whole of TASK-171: with the
     rate set only once - on either side of set_max_block - the core writes
     +/-Inf from the second sample onward and NaN after that. Measured on a
     probe that varies one call at a time:

       rate, block, rate     finite 2048 of 2048
       rate, block, config, rate   finite 2048 of 2048
       rate, block           CRASH
       rate, block, config   CRASH
       block, rate           CRASH
       no rate at all        CRASH

     so set_max_block invalidates the rate-derived state and the config call
     has nothing to do with it. The control that says the rate now actually
     arrives: with one rate call frame 0 is bit-identical at 22050, 44100 and
     96000; with two it differs at each. */
  printf("rate %.0f, max block %d (rate set both sides)\n", rate, maxblock);
  {
    struct tg_system_config cfg;
    cfg.a = 1; cfg.b = 1;
    printf("set_config(1,1) -> %d\n", a.set_config(&cfg));
    memset(&cfg, 0, sizeof cfg);
    a.get_config(&cfg);
    printf("config now: %d, %d\n", cfg.a, cfg.b);
  }
  /* LAST call before activate, and this is the whole of TASK-171. */
  a.set_sample_rate((float)rate);
  {
    int act = a.activate((float)rate, maxblock);
    int fat = a.fatal();
    int k;
    printf("activate -> %d, fatal %d\n", act, fat);
    for (k = 0; k < 8; ++k) {
      const char *msg = a.errors(k);
      if (msg && *msg && (unsigned char)*msg >= 32)
        printf("  error string %d: %s\n", k, msg);
    }
    fflush(stdout);
  }
  fflush(stdout);
  if (!strcmp(reset, "gs")) { a.long_midi(gs_reset, 0);
    printf("gs reset sent\n"); fflush(stdout); }
  else if (!strcmp(reset, "gm")) { a.long_midi(gm_reset, 0);
    printf("gm reset sent\n"); fflush(stdout); }

  left = calloc((size_t)maxblock, sizeof *left);
  right = calloc((size_t)maxblock, sizeof *right);
  if (!left || !right) { fprintf(stderr, "out of memory\n"); return 1; }

  wav = fopen(out, "wb");
  if (!wav) { fprintf(stderr, "cannot write %s\n", out); return 1; }
  header(wav, (unsigned)rate, 0);
  per_tick = rate * (double)tempo / (1e6 * s.division);
  /* TG_Process generates on demand: if its ring cannot satisfy the frames
     asked for it synthesises more and loops until it can. There is nothing to
     outrun, so --flat-out is correct and simply faster. Pacing to wall-clock
     is the default only because a daemon wants it. */
  start_ms = GetTickCount();

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
        a.short_midi(msg, 0);
        ++sent;
      }
      ++ei;
    }
    memset(left, 0, (size_t)maxblock * sizeof *left);
    memset(right, 0, (size_t)maxblock * sizeof *right);
    if (realtime) {
      double audio_ms = 1000.0 * (double)frame / rate;
      for (;;) {
        double elapsed = (double)(GetTickCount() - start_ms);
        if (elapsed >= audio_ms) break;
        Sleep(audio_ms - elapsed > 4.0 ? 2 : 1);
      }
    }
    if (!total) { printf("first process...\n"); fflush(stdout); }
    a.process(left, right, BLOCK);
    if (!total) { printf("first process returned\n"); fflush(stdout); }
    for (k = 0; k < BLOCK; ++k) {
      float l = left[k] < 0 ? -left[k] : left[k];
      float r = right[k] < 0 ? -right[k] : right[k];
      if (l > peak) peak = l;
      if (r > peak) peak = r;
      fwrite(left + k, 4, 1, wav);
      fwrite(right + k, 4, 1, wav);
    }
    frame += BLOCK;
    total += BLOCK;
  }
  rewind(wav);
  header(wav, (unsigned)rate, total);
  fclose(wav);
  /* before terminating: TG_terminate calls exit(), so nothing after it runs
     and anything still buffered is lost */
  printf("%s: %u frames at %.0f Hz, %.1f s, peak %.5f, %d messages, voices %d\n",
         out, total, rate, total / rate, peak, sent, a.voices());
  fflush(stdout);
  a.deactivate();
  a.terminate();
  return 0;
}
