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
/* rate in XMM0; second argument is the max block size */
typedef int (*tg_activate_fn)(float, int);
typedef void (*tg_deactivate_fn)(void);
typedef void (*tg_terminate_fn)(void);
/* second argument is a timestamp in samples */
typedef void (*tg_short_midi_fn)(unsigned int, int);
/* F7-terminated, so the second argument is a timestamp too */
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

/* Windows refcounts modules by path: a second LoadLibrary of the same file
   hands back the first module, globals and all. A copy under another name
   loads as a separate module - the same trick Roland uses by shipping 32
   byte-different cores in the Mac build. */
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
  return h;
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
#ifdef SCVA_OWN_LOADER
  struct pe_image *lib = NULL, *lib_b = NULL;
  char peerr[256];
#define SCVA_SYM(h, n) pe_symbol(h, n)
#else
  HMODULE lib, lib_b = NULL;
#define SCVA_SYM(h, n) ((void *)GetProcAddress(h, n))
#endif
  struct api a, b;
  int have_b = 0;
  char coreb[MAX_PATH] = "";
  struct song s;
  unsigned char *bytes;
  size_t n = 0, ei = 0;
  FILE *wav = NULL;
  float *left, *right, *left_b = NULL, *right_b = NULL;
  unsigned char *sysex_buf = NULL;
  uint64_t frame = 0, at = 0;
  uint32_t total = 0, tempo = 500000;
  uint64_t last_tick = 0;
  double per_tick;
  float peak = 0.0f;
  int bits = 32;                     /* --bits 16 for integer PCM */
  int play = 0;                      /* --play: to the sound card, no file */
  HWAVEOUT hout = NULL;
  WAVEHDR *whdr = NULL;
  short **wbuf = NULL;
  HANDLE wev = NULL;
  int nbuf = 0, wb = 0;
  int i, sent = 0, nports;
  int realtime = 1;
  DWORD start_ms;

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
    else { fprintf(stderr, "usage: scva_render --core DLL --midi FILE --out FILE\n"
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
  if (song_ports(&s) > 1) {
    lib_b = load_second_core(core, coreb, sizeof coreb);
    if (!lib_b) {
      fprintf(stderr, "cannot open a second core for port B\n");
      return 1;
    }
#define GETB(field, name) \
    b.field = (void *)GetProcAddress(lib_b, name); \
    if (!b.field) { fprintf(stderr, "missing %s\n", name); return 1; }
    GETB(initialize, "TG_initialize")
    GETB(set_sample_rate, "TG_setSampleRate")
    GETB(set_max_block, "TG_setMaxBlockSize")
    GETB(activate, "TG_activate")
    GETB(deactivate, "TG_deactivate")
    GETB(terminate, "TG_terminate")
    GETB(short_midi, "TG_ShortMidiIn")
    GETB(long_midi, "TG_LongMidiIn")
    GETB(process, "TG_Process")
    GETB(voices, "TG_XPgetCurTotalRunningVoices")
    GETB(fatal, "TG_isFatalError")
    GETB(set_config, "TG_XPsetSystemConfig")
    GETB(get_config, "TG_XPgetCurSystemConfig")
    GETB(errors, "TG_getErrorStrings")
#undef GETB
    have_b = 1;
    if (nports > 2)
      fprintf(stderr, "this song names %d ports; the engine is a two-port "
                      "machine here, so everything past A shares port B\n",
              nports);
    printf("two-port song: second core from %s\n", coreb);
  }

  printf("initialize -> %d\n", a.initialize(0));
  a.set_sample_rate((float)rate);
  if (maxblock < SCVA_MIN_BLOCK) {
    fprintf(stderr, "%s: --maxblock %d is below the engine's minimum, "
                    "using %d\n", argv[0], maxblock, SCVA_MIN_BLOCK);
    maxblock = SCVA_MIN_BLOCK;
  }
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
  if (have_b) {
    struct tg_system_config cfg;
    b.initialize(0);
    b.set_sample_rate((float)rate);
    b.set_max_block(maxblock);
    cfg.a = 1; cfg.b = 1;
    b.set_config(&cfg);
    b.set_sample_rate((float)rate);
    b.activate((float)rate, maxblock);
  }
  if (!strcmp(reset, "gs")) { a.long_midi(gs_reset, 0);
    if (have_b) b.long_midi(gs_reset, 0);
    printf("gs reset sent\n"); fflush(stdout); }
  else if (!strcmp(reset, "gm")) { a.long_midi(gm_reset, 0);
    if (have_b) b.long_midi(gm_reset, 0);
    printf("gm reset sent\n"); fflush(stdout); }
  /* a program change latches CC32, so set it before the music starts */
  { int ch; for (ch = 0; ch < 16; ++ch) {
      unsigned int cc = 0xb0u | (unsigned)ch | (0x20u << 8) |
                        ((unsigned)mapval << 16);
      a.short_midi(cc, 0);
      if (have_b) b.short_midi(cc, 0);
    }
    printf("tone map %s (CC32=%d)\n", mapname, mapval); fflush(stdout); }

  left = calloc((size_t)maxblock, sizeof *left);
  right = calloc((size_t)maxblock, sizeof *right);
  if (!left || !right) { fprintf(stderr, "out of memory\n"); return 1; }
  if (have_b) {
    left_b = calloc((size_t)maxblock, sizeof *left_b);
    right_b = calloc((size_t)maxblock, sizeof *right_b);
    if (!left_b || !right_b) { fprintf(stderr, "out of memory\n"); return 1; }
  }

  if (play) {
    /* Windows offers no way for a program to join the MIDI device list, so
       rather than route a file through a driver and a virtual cable, the
       renderer reads it and puts the audio out itself. waveOut in 16-bit
       stereo is what every Windows sound device accepts. */
    WAVEFORMATEX wf;
    int j;
    nbuf = (int)((double)200.0 * rate / 1000.0 / BLOCK);   /* ~200 ms ahead */
    if (nbuf < 4) nbuf = 4;
    if (nbuf > 64) nbuf = 64;
    wev = CreateEventA(NULL, FALSE, FALSE, NULL);
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.nSamplesPerSec = (DWORD)rate;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    if (waveOutOpen(&hout, WAVE_MAPPER, &wf, (DWORD_PTR)wev, 0,
                    CALLBACK_EVENT) != MMSYSERR_NOERROR) {
      fprintf(stderr, "cannot open an audio device at %.0f Hz\n", rate);
      return 1;
    }
    whdr = calloc((size_t)nbuf, sizeof *whdr);
    wbuf = calloc((size_t)nbuf, sizeof *wbuf);
    if (!whdr || !wbuf) { fprintf(stderr, "out of memory\n"); return 1; }
    for (j = 0; j < nbuf; ++j) {
      wbuf[j] = calloc((size_t)BLOCK * 2, sizeof **wbuf);
      if (!wbuf[j]) { fprintf(stderr, "out of memory\n"); return 1; }
      whdr[j].lpData = (LPSTR)wbuf[j];
      whdr[j].dwBufferLength = (DWORD)(BLOCK * 2 * sizeof **wbuf);
      waveOutPrepareHeader(hout, &whdr[j], sizeof whdr[j]);
      whdr[j].dwFlags |= WHDR_DONE;                  /* all free to begin */
    }
    printf("playing, %d x %d frames buffered (%.0f ms)\n",
           nbuf, BLOCK, 1000.0 * nbuf * BLOCK / rate);
    fflush(stdout);
  } else {
    wav = fopen(out, "wb");
    if (!wav) { fprintf(stderr, "cannot write %s\n", out); return 1; }
    header(wav, (unsigned)rate, 0, bits);
  }
  per_tick = song_frames_per_tick(&s, rate, tempo);
  /* TG_Process generates on demand, so --flat-out is correct and faster */
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
        per_tick = song_frames_per_tick(&s, rate, tempo);
      } else if (e->sysex_len) {
        int sn = sysex_message(e, sysex_buf, (size_t)s.sysex_max + 1);
        if (sn) {
          /* SysEx carries no channel: it addresses the whole machine, so both
             halves of a two-port performance get it. */
          a.long_midi(sysex_buf, 0);
          if (have_b) b.long_midi(sysex_buf, 0);
          ++sent;
        }
      } else {
        struct api *E = (have_b && e->port) ? &b : &a;
        unsigned int msg = (unsigned int)e->status |
          ((unsigned int)e->data1 << 8) | ((unsigned int)e->data2 << 16);
        /* a program change latches the map, so set CC32 on that part first */
        if (mapval && (e->status & 0xf0) == 0xc0)
          E->short_midi(0xb0u | (unsigned)(e->status & 0x0f) |
                        (0x20u << 8) | ((unsigned)mapval << 16), 0);
        E->short_midi(msg, 0);
        ++sent;
      }
      ++ei;
    }
    memset(left, 0, (size_t)maxblock * sizeof *left);
    memset(right, 0, (size_t)maxblock * sizeof *right);
    if (have_b) {
      memset(left_b, 0, (size_t)maxblock * sizeof *left_b);
      memset(right_b, 0, (size_t)maxblock * sizeof *right_b);
    }
    if (play) {
      /* waveOut is the clock now: wait for a buffer to come free */
      while (!(whdr[wb].dwFlags & WHDR_DONE)) WaitForSingleObject(wev, 100);
    } else if (realtime) {
      double audio_ms = 1000.0 * (double)frame / rate;
      for (;;) {
        double elapsed = (double)(GetTickCount() - start_ms);
        if (elapsed >= audio_ms) break;
        Sleep(audio_ms - elapsed > 4.0 ? 2 : 1);
      }
    }
    if (!total) { printf("first process...\n"); fflush(stdout); }
    a.process(left, right, BLOCK);
    if (have_b) {
      int j;
      b.process(left_b, right_b, BLOCK);
      for (j = 0; j < BLOCK; ++j) {
        left[j] += left_b[j];
        right[j] += right_b[j];
      }
    }
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
  header(wav, (unsigned)rate, total, bits);
  fclose(wav);
  /* TG_terminate calls exit(), so print first */
  printf("%s: %u frames at %.0f Hz, %.1f s, peak %.5f, %d messages, voices %d\n",
         out, total, rate, total / rate, peak, sent, a.voices());
  fflush(stdout);
  if (have_b) {
    b.deactivate();
    FreeLibrary(lib_b);
    DeleteFileA(coreb);
  }
  a.deactivate();
  a.terminate();
  return 0;
}
