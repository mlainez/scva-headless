/* SPDX-License-Identifier: CC0-1.0
 *
 * A standalone Windows synth: opens a MIDI input port and plays it through
 * SOUND Canvas VA. This is what a game talks to.
 *
 * Windows has no way for a program to add itself to the MIDI device list -
 * that list comes from drivers, which is why the one soft-synth that appears
 * in it ships a kernel driver. So this opens an input port that already
 * exists:
 *
 *   a USB or MIDI-interface keyboard   works with nothing else installed
 *   a game or DOSBox                   needs a virtual cable such as loopMIDI,
 *                                      whose port both sides then select
 *
 *   scva-winmidi --list
 *   scva-winmidi --midi-in "loopMIDI Port"
 *
 * Audio goes out through waveOut in 16-bit stereo, which every Windows audio
 * device accepts. Latency is the buffer count times the block, so it is set
 * by --latency rather than fixed.
 */
/* midiIn, waveOut, LoadLibrary and a critical section are all Win95-era, so
   this program itself asks for nothing newer and the 32-bit build sets 0x0400
   from the Makefile. How old a machine it actually runs on is decided by the
   core: the 64-bit one declares subsystem 6.0 and imports the Universal CRT,
   so it wants Vista or later. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#include <windows.h>
#include <mmsystem.h>
#include <conio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scva_map.h"
#include "map_policy.h"
#include "console.h"
#include "live_sched.h"
#include "rt_priority.h"
#include "audio_waveout.h"
#include "core_path.h"
#include "scva_core_api.h"
#include "live_engine.h"
#include "synth_registry.h"

/* The engine: SOUND Canvas VA's core by default (synth_scva.c loads it the
   way each Windows width needs), or another one this build has. */
static struct live_engine eng;

/* The MIDI callback runs on its own thread, so messages are queued rather
   than pushed into the engine from under it. */
#define QCAP     2048
#define QMSGMAX  256
struct qmsg {
  int len;                          /* 1..3 short, or a whole sysex */
  unsigned int when;                /* ms since midiInStart, from the driver */
  unsigned char b[QMSGMAX];
};
static struct qmsg q[QCAP];
static volatile LONG q_head, q_tail;   /* head written by MIDI, tail by audio */
static CRITICAL_SECTION q_lock;
static long q_dropped;

static void queue_put(const unsigned char *b, int len, unsigned int when)
{
  if (len < 1 || len > QMSGMAX) return;
  EnterCriticalSection(&q_lock);
  if ((q_head + 1) % QCAP == q_tail) {
    ++q_dropped;                    /* full: better to lose one than block */
  } else {
    memcpy(q[q_head].b, b, (size_t)len);
    q[q_head].len = len;
    q[q_head].when = when;
    q_head = (q_head + 1) % QCAP;
  }
  LeaveCriticalSection(&q_lock);
}

/* Takes the next message only if it is due by `until`, a frame position in
   the stream being rendered. Draining the whole queue into each block instead
   is what makes a large buffer ruin the rhythm: every event that arrived
   during the buffer window lands on the same instant, so the timing error is
   the buffer size. Honouring the arrival time keeps it at one block. */
static int queue_get_due(struct qmsg *out, uint64_t until,
                         const struct live_sched *sched)
{
  int got = 0;
  EnterCriticalSection(&q_lock);
  if (q_tail != q_head) {
    if (live_sched_due(sched, q[q_tail].when, until)) {
      *out = q[q_tail];
      q_tail = (q_tail + 1) % QCAP;
      got = 1;
    }
  }
  LeaveCriticalSection(&q_lock);
  return got;
}

/* Sysex arrives in buffers the driver fills and hands back one at a time.
   Each must be given back before it can carry another message, but
   midiInAddBuffer is not on the list of calls a MIDI callback may make, so
   the callback only flags the buffer and the audio loop re-adds it. */
#define NSYSBUF  4
#define SYSBUFSZ 1024
static MIDIHDR sys_hdr[NSYSBUF];
static unsigned char sys_buf[NSYSBUF][SYSBUFSZ];
static volatile LONG sys_returned[NSYSBUF];

static void CALLBACK midi_cb(HMIDIIN h, UINT msg, DWORD_PTR inst,
                             DWORD_PTR p1, DWORD_PTR p2)
{
  (void)h; (void)inst;
  /* p2 is the arrival time in milliseconds since midiInStart. Keeping it is
     what lets the rhythm survive a deep output buffer. */
  if (msg == MIM_DATA) {
    unsigned char b[3];
    unsigned int m = (unsigned int)p1;
    b[0] = (unsigned char)(m & 0xff);
    b[1] = (unsigned char)((m >> 8) & 0xff);
    b[2] = (unsigned char)((m >> 16) & 0xff);
    queue_put(b, 3, (unsigned int)p2);
  } else if (msg == MIM_LONGDATA) {
    MIDIHDR *hdr = (MIDIHDR *)p1;
    if (!hdr) return;
    if (hdr->dwBytesRecorded > 0)
      queue_put((const unsigned char *)hdr->lpData,
                (int)hdr->dwBytesRecorded, (unsigned int)p2);
    if (hdr->dwUser < NSYSBUF)
      InterlockedExchange(&sys_returned[hdr->dwUser], 1);
  }
}

/* Frames the device has played. Some drivers only report bytes. Returns 0
   when the device reports neither. */
static int played_frames(HWAVEOUT h, struct live_sched *sched, uint64_t *frames)
{
  MMTIME t;
  DWORD now;

  t.wType = TIME_SAMPLES;
  if (waveOutGetPosition(h, &t, sizeof t) != MMSYSERR_NOERROR) return 0;
  if (t.wType == TIME_SAMPLES) now = t.u.sample;
  else if (t.wType == TIME_BYTES) now = t.u.cb / 4;  /* 16-bit stereo */
  else return 0;
  *frames = live_sched_position(sched, now);
  return 1;
}

static void list_inputs(void)
{
  UINT n = midiInGetNumDevs(), i;
  if (!n) {
    printf("no MIDI input devices.\n"
           "  a keyboard shows up on its own; for a game, install a virtual\n"
           "  cable such as loopMIDI and its port appears here\n");
    return;
  }
  printf("MIDI input devices:\n");
  for (i = 0; i < n; ++i) {
    MIDIINCAPSA c;
    if (midiInGetDevCapsA(i, &c, sizeof c) == MMSYSERR_NOERROR)
      printf("  %u  %s\n", i, c.szPname);
  }
}

/* Accepts an index, or any part of a device's name. */
static int find_input(const char *want)
{
  UINT n = midiInGetNumDevs(), i;
  char *endp;
  long idx;

  if (!want || !*want) return n ? 0 : -1;
  idx = strtol(want, &endp, 10);
  if (*endp == '\0' && idx >= 0 && (UINT)idx < n) return (int)idx;
  for (i = 0; i < n; ++i) {
    MIDIINCAPSA c;
    if (midiInGetDevCapsA(i, &c, sizeof c) == MMSYSERR_NOERROR &&
        strstr(c.szPname, want))
      return (int)i;
  }
  return -1;
}

static void print_stdout(void *ctx, const char *text)
{
  (void)ctx;
  fputs(text, stdout);
}

/* One typed line, on scva-daemon's console (console.h). Returns 1 when it
   asked to stop. */
static int console_command(char *line, struct live_engine *l)
{
  struct console c;
  live_engine_console(l, &c);
  c.trim = " ";
  c.print = print_stdout;
  c.ctx = NULL;
  return console_line(&c, line);
}

/* _kbhit()/_getch() rather than ReadFile/ReadConsole on the standard input
   handle: those read a whole line under the console's default line-editing
   mode, which would stall the audio loop behind the Enter key exactly the
   way a note would stall behind one. This checks and returns immediately
   when nothing has been typed, so it costs nothing to call every time round
   the loop; the worst case for noticing a keystroke is one iteration, the
   same as the 100 ms this loop can already wait for a wave buffer to free.
   Returns 1 when it was told to stop. */
static int poll_console(struct live_engine *l)
{
  static char line[256];
  static size_t len = 0;
  while (_kbhit()) {
    int c = _getch();
    if (c == '\r' || c == '\n') {
      putchar('\n');
      line[len] = '\0';
      len = 0;
      if (console_command(line, l)) return 1;
    } else if (c == '\b' || c == 127) {
      if (len > 0) { --len; printf("\b \b"); }
    } else if (len + 1 < sizeof line) {
      line[len++] = (char)c;
      putchar(c);
    }
  }
  return 0;
}

static void usage(void)
{
  int i;
  fprintf(stderr,
    "usage: scva-winmidi [--midi-in NAME|INDEX] [--core DLL]\n"
    "                    [--rate HZ] [--block N] [--latency MS]\n"
    "                    [--map " SCVA_MAP_USAGE "] [--name NAME]\n"
    "\n"
    "  --list             the MIDI inputs this machine has\n"
    "  --midi-in          index, or any part of the name\n"
    "  --name             window title, so several of these running\n"
    "                     at once can be told apart (default SCVA)\n");
  if (synth_backends[1]) {
    fprintf(stderr, "  --engine %s  the engine (default %s)\n",
            synth_engine_names(), synth_backends[0]->caps->name);
    for (i = 1; synth_backends[i]; ++i) {
      fprintf(stderr, "with --engine %s, --core is its engine file, a map typed at\n"
                      "the console chooses its one tone set, and:\n",
              synth_backends[i]->caps->name);
      synth_print_options(synth_backends[i], stderr);
    }
  }
}

int main(int argc, char **argv)
{
  const char *core = NULL, *want_in = NULL, *mapname = "default";
  const char *name = "SCVA";        /* --name: the window title and the
                                        name status lines print, so several
                                        of these running at once can be told
                                        apart */
  unsigned int rate = 48000;
  int block = 256, latency_ms = 40, mapval = 0, i, rate_given = 0;
  const struct synth_backend *engine = synth_backends[0];
  void *engine_opts = NULL;
  char corebuf[MAX_PATH], err[256];
  HMIDIIN hin = NULL;
  struct waveout out;
  float *left, *right;
  int nbuf, b, dev;
  uint64_t rendered = 0;            /* frames handed to the device so far */
  DWORD midi_t0;                    /* timeGetTime() at midiInStart */
  struct live_sched sched;

  /* The engine first: the rest of the command line may be its options. */
  for (i = 1; i + 1 < argc; ++i)
    if (!strcmp(argv[i], "--engine")) {
      engine = synth_find(argv[i + 1]);
      if (!engine) {
        fprintf(stderr, "scva-winmidi: --engine takes %s\n", synth_engine_names());
        return 2;
      }
    }
  if (engine->opts_size) {
    engine_opts = calloc(1, engine->opts_size);
    if (!engine_opts) { fprintf(stderr, "scva-winmidi: out of memory\n"); return 1; }
    engine->opts_init(engine_opts);
  }

  for (i = 1; i < argc; ++i) {
    int took;
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--engine") && i + 1 < argc) ++i;
    else if (!strcmp(argv[i], "--midi-in") && i + 1 < argc) want_in = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
      rate = (unsigned)atoi(argv[++i]);
      rate_given = 1;
    }
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--latency") && i + 1 < argc) latency_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--map") && i + 1 < argc) mapname = argv[++i];
    else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
    else if (!strcmp(argv[i], "--list")) { list_inputs(); return 0; }
    else if ((took = synth_parse_option(engine, engine_opts, argc, argv, &i,
                                        err, sizeof err)) < 0) {
      fprintf(stderr, "scva-winmidi: %s\n", err);
      return 2;
    }
    else if (!took) {
      usage();
      return (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) ? 0 : 2;
    }
  }
  SetConsoleTitleA(name);
  mapval = scva_map_value(mapname);
  if (mapval < 0) {
    fprintf(stderr, "scva-winmidi: --map wants " SCVA_MAP_USAGE "\n");
    return 2;
  }
  if (block < SCVA_MIN_BLOCK) {
    if (block > 0)
      fprintf(stderr, "scva-winmidi: --block %d is below the engine's "
                      "minimum, using %d\n", block, SCVA_MIN_BLOCK);
    block = SCVA_MIN_BLOCK;
  }
  /* ---- the engine ---- */
  if (engine == &synth_scva) {
    snprintf(corebuf, sizeof corebuf, "%s", scva_core_path(core));
  } else if (core) {
    snprintf(corebuf, sizeof corebuf, "%s", core);
  } else {
    fprintf(stderr, "scva-winmidi: --engine %s needs --core, its engine file\n",
            engine->caps->name);
    return 2;
  }
  {
    int status;
    if (!live_engine_open(&eng, engine, corebuf, rate, rate_given, block, engine_opts,
                          mapval, mapname, &status, err, sizeof err)) {
      if (status == 1 && engine == &synth_scva)
        fprintf(stderr, "scva-winmidi: %s\n"
                        "  looked for %s; pass --core or set SCVA_DLL_DIR\n",
                err, corebuf);
      else
        fprintf(stderr, "scva-winmidi: %s\n", err);
      return status;
    }
  }
  rate = (unsigned)eng.synth->rate;
  nbuf = (int)((double)latency_ms * rate / 1000.0 / block);
  if (nbuf < 2) nbuf = 2;
  if (nbuf > 32) nbuf = 32;
  live_sched_init(&sched, (double)rate, (unsigned)latency_ms,
                  (uint64_t)nbuf * block);

  /* ---- MIDI in ---- */
  InitializeCriticalSection(&q_lock);
  dev = find_input(want_in);
  if (dev < 0) {
    fprintf(stderr, "scva-winmidi: no MIDI input matching '%s'\n",
            want_in ? want_in : "(any)");
    list_inputs();
    return 1;
  }
  if (midiInOpen(&hin, (UINT)dev, (DWORD_PTR)midi_cb, 0,
                 CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
    fprintf(stderr, "scva-winmidi: cannot open MIDI input %d\n", dev);
    return 1;
  }
  for (b = 0; b < NSYSBUF; ++b) {
    sys_hdr[b].lpData = (LPSTR)sys_buf[b];
    sys_hdr[b].dwBufferLength = SYSBUFSZ;
    sys_hdr[b].dwUser = (DWORD_PTR)b;
    midiInPrepareHeader(hin, &sys_hdr[b], sizeof sys_hdr[b]);
    midiInAddBuffer(hin, &sys_hdr[b], sizeof sys_hdr[b]);
  }
  /* 1 ms rather than the default 10-16, for timeGetTime() here and the
     timestamps the driver puts on each message. */
  timeBeginPeriod(1);
  midi_t0 = timeGetTime();
  midiInStart(hin);
  {
    MIDIINCAPSA c;
    if (midiInGetDevCapsA((UINT)dev, &c, sizeof c) == MMSYSERR_NOERROR)
      printf("scva-winmidi[%s]: listening on %d  %s\n", name, dev, c.szPname);
  }

  /* ---- audio out ---- */
  switch (waveout_open(&out, rate, nbuf, block)) {
  case WAVEOUT_NO_DEVICE:
    fprintf(stderr, "scva-winmidi: cannot open an audio device at %u Hz\n",
            rate);
    return 1;
  case WAVEOUT_NO_MEMORY:
    return 1;
  }
  printf("scva-winmidi[%s]: audio %u Hz, %d x %d frames (%.0f ms), map %s\n",
         name, rate, nbuf, block, 1000.0 * nbuf * block / rate, mapname);

  left = malloc((size_t)block * sizeof *left);
  right = malloc((size_t)block * sizeof *right);
  if (!left || !right) return 1;

  rt_priority_raise();

  printf("scva-winmidi[%s]: playing. Type a map (" SCVA_MAP_USAGE "), <enter>\n"
         "to show it, q or Ctrl+C to stop.\n", name);
  for (;;) {
    int did = 0;
    uint64_t played;
    int have_played;
    if (poll_console(&eng)) break;
    for (b = 0; b < NSYSBUF; ++b)
      if (InterlockedExchange(&sys_returned[b], 0))
        midiInAddBuffer(hin, &sys_hdr[b], sizeof sys_hdr[b]);
    /* Where MIDI time 0 falls in the stream: the frame playing now, less
       the MIDI time that has passed. */
    have_played = played_frames(out.h, &sched, &played);
    live_sched_measure(&sched, have_played ? &played : NULL, rendered,
                       (uint32_t)(DWORD)(timeGetTime() - midi_t0));
    for (b = 0; b < nbuf; ++b) {
      struct qmsg m;
      int k;
      if (!waveout_is_free(&out, b)) continue;

      /* Only what is due by the end of this block, so the timing error is one
         block rather than the whole output buffer. Anything already late goes
         in here too: it cannot be put back. */
      rendered += block;
      while (queue_get_due(&m, rendered, &sched)) {
        if (m.b[0] == 0xf0) {
          synth_long(eng.synth, m.b, (size_t)m.len);
        } else {
          unsigned int msg = m.b[0];
          if (m.len > 1) msg |= (unsigned int)m.b[1] << 8;
          if (m.len > 2) msg |= (unsigned int)m.b[2] << 16;
          map_policy_short(&eng.maps, msg);
        }
      }
      synth_render(eng.synth, left, right, (uint32_t)block);
      for (k = 0; k < block; ++k) {
        float l = left[k], r = right[k];
        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
        out.pcm[b][2 * k]     = (short)(l * 32767.0f);
        out.pcm[b][2 * k + 1] = (short)(r * 32767.0f);
      }
      waveout_write(&out, b);
      did = 1;
    }
    if (!did) waveout_wait(&out, 100);
  }
  printf("scva-winmidi[%s]: stopping\n", name);
  timeEndPeriod(1);
  live_engine_close(&eng);
  free(engine_opts);
  return 0;
}
