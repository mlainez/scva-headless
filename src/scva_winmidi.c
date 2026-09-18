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
#include "core_path.h"
#if defined(__i386__)
/* The 32-bit core imports MSVCR100 and declares a subsystem newer than
   Windows 98, so the system loader refuses it there. src/pe_loader.c maps it
   and supplies that runtime itself, applying the SSE shims on the way. */
#define SCVA_OWN_LOADER 1
#include "pe_loader.h"
#else
#define SCVA_SHIM_HOST_APPLY 1     /* the system loader path applies them */
#include "sse3_shim.h"
#endif

#define SCVA_CC __cdecl
typedef SCVA_CC int  (*tg_initialize_fn)(int);
typedef SCVA_CC void (*tg_set_sample_rate_fn)(float);
typedef SCVA_CC void (*tg_set_max_block_fn)(int);
typedef SCVA_CC int  (*tg_activate_fn)(float, int);
typedef SCVA_CC void (*tg_deactivate_fn)(void);
typedef SCVA_CC void (*tg_short_midi_fn)(unsigned int, int);
typedef SCVA_CC void (*tg_long_midi_fn)(const unsigned char *, int);
typedef SCVA_CC void (*tg_process_fn)(float *, float *, int);
struct tg_system_config { int a, b; };
typedef SCVA_CC int (*tg_set_config_fn)(const struct tg_system_config *);

static tg_set_sample_rate_fn set_rate;
static tg_activate_fn tg_activate;
static tg_deactivate_fn tg_deactivate;
static tg_short_midi_fn short_midi;
static tg_long_midi_fn long_midi;
static tg_process_fn process_fn;

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
static int queue_get_due(struct qmsg *out, uint64_t until, double rate,
                         unsigned int latency_ms, uint64_t horizon)
{
  int got = 0;
  EnterCriticalSection(&q_lock);
  if (q_tail != q_head) {
    uint64_t due = (uint64_t)(((double)q[q_tail].when + latency_ms)
                              * rate / 1000.0);
    /* The card's clock and the system timer are not the same clock. If they
       drift apart far enough that a message would be held back further than
       the buffer is deep, play it rather than let the queue grow for ever. */
    if (due > until + horizon) due = until;
    if (due <= until) {
      *out = q[q_tail];
      q_tail = (q_tail + 1) % QCAP;
      got = 1;
    }
  }
  LeaveCriticalSection(&q_lock);
  return got;
}

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
    if (hdr && hdr->dwBytesRecorded > 0)
      queue_put((const unsigned char *)hdr->lpData,
                (int)hdr->dwBytesRecorded, (unsigned int)p2);
  }
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

/* One typed line. Mirrors scva-daemon's console_command exactly - same
   commands, same wording - so switching platforms costs nothing. Returns 1
   when it asked to stop. */
static int console_command(char *line, unsigned char *map_of)
{
  int ch, want, off = 0;
  size_t n = strlen(line);

  while (n > 0 && line[n - 1] == ' ') line[--n] = '\0';
  if (!line[0]) {
    printf("map:");
    for (ch = 0; ch < 16; ++ch) printf(" %d", map_of[ch]);
    printf("\n");
    return 0;
  }
  if (!strcmp(line, "q") || !strcmp(line, "quit")) return 1;

  /* "<channel> <map>" sets one part, a bare map name sets all of them */
  if (sscanf(line, "%d %n", &ch, &off) == 1 && off > 0 && line[off] &&
      ch >= 1 && ch <= 16 && (want = scva_map_value(line + off)) >= 0) {
    map_of[ch - 1] = (unsigned char)want;
    short_midi(scva_map_cc(ch - 1, want), 0);
    printf("channel %d -> map %d\n", ch, want);
    return 0;
  }
  if ((want = scva_map_value(line)) >= 0) {
    for (ch = 0; ch < 16; ++ch) {
      map_of[ch] = (unsigned char)want;
      short_midi(scva_map_cc(ch, want), 0);
    }
    printf("all parts -> map %d\n", want);
    return 0;
  }
  printf("type a map (" SCVA_MAP_USAGE "), or\n"
         "  <channel 1-16> <map>   one part only\n"
         "  <enter>                what each part is set to\n"
         "  q                      stop\n");
  return 0;
}

/* _kbhit()/_getch() rather than ReadFile/ReadConsole on the standard input
   handle: those read a whole line under the console's default line-editing
   mode, which would stall the audio loop behind the Enter key exactly the
   way a note would stall behind one. This checks and returns immediately
   when nothing has been typed, so it costs nothing to call every time round
   the loop; the worst case for noticing a keystroke is one iteration, the
   same as the 100 ms this loop can already wait for a wave buffer to free.
   Returns 1 when it was told to stop. */
static int poll_console(unsigned char *map_of)
{
  static char line[256];
  static size_t len = 0;
  while (_kbhit()) {
    int c = _getch();
    if (c == '\r' || c == '\n') {
      putchar('\n');
      line[len] = '\0';
      len = 0;
      if (console_command(line, map_of)) return 1;
    } else if (c == '\b' || c == 127) {
      if (len > 0) { --len; printf("\b \b"); }
    } else if (len + 1 < sizeof line) {
      line[len++] = (char)c;
      putchar(c);
    }
  }
  return 0;
}

int main(int argc, char **argv)
{
  const char *core = NULL, *want_in = NULL, *mapname = "default";
  const char *name = "SCVA";        /* --name: the window title and the
                                        name status lines print, so several
                                        of these running at once can be told
                                        apart */
  unsigned int rate = 48000;
  int block = 256, latency_ms = 40, mapval = 0, i;
  unsigned char map_of[16];
#ifdef SCVA_OWN_LOADER
  struct pe_image *lib;
  char peerr[256];
#define SCVA_SYM(h, n) pe_symbol(h, n)
#else
  HMODULE lib;
#define SCVA_SYM(h, n) ((void *)GetProcAddress(h, n))
#endif
  HMIDIIN hin = NULL;
  HWAVEOUT hout = NULL;
  HANDLE ev;
  WAVEFORMATEX wf;
  MIDIHDR syshdr;
  static unsigned char sysbuf[1024];
  WAVEHDR *hdr;
  short **pcm;
  float *left, *right;
  int nbuf, b, dev;
  uint64_t rendered = 0;            /* frames handed to the device so far */

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--midi-in") && i + 1 < argc) want_in = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--latency") && i + 1 < argc) latency_ms = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--map") && i + 1 < argc) mapname = argv[++i];
    else if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
    else if (!strcmp(argv[i], "--list")) { list_inputs(); return 0; }
    else {
      fprintf(stderr,
        "usage: scva-winmidi [--midi-in NAME|INDEX] [--core DLL]\n"
        "                    [--rate HZ] [--block N] [--latency MS]\n"
        "                    [--map " SCVA_MAP_USAGE "] [--name NAME]\n"
        "\n"
        "  --list             the MIDI inputs this machine has\n"
        "  --midi-in          index, or any part of the name\n"
        "  --name             window title, so several of these running\n"
        "                     at once can be told apart (default SCVA)\n");
      return (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) ? 0 : 2;
    }
  }
  SetConsoleTitleA(name);
  mapval = scva_map_value(mapname);
  if (mapval < 0) {
    fprintf(stderr, "scva-winmidi: --map wants " SCVA_MAP_USAGE "\n");
    return 2;
  }
  for (i = 0; i < 16; ++i) map_of[i] = (unsigned char)mapval;
  if (block < SCVA_MIN_BLOCK) {
    if (block > 0)
      fprintf(stderr, "scva-winmidi: --block %d is below the engine's "
                      "minimum, using %d\n", block, SCVA_MIN_BLOCK);
    block = SCVA_MIN_BLOCK;
  }
  nbuf = (int)((double)latency_ms * rate / 1000.0 / block);
  if (nbuf < 2) nbuf = 2;
  if (nbuf > 32) nbuf = 32;

  /* ---- the engine ---- */
  {
    const char *path = scva_core_path(core);
#ifdef SCVA_OWN_LOADER
    lib = pe_load(path, peerr, sizeof peerr);
    if (!lib) {
      fprintf(stderr, "scva-winmidi: cannot load %s: %s\n"
                      "  pass --core or set SCVA_DLL_DIR\n", path, peerr);
      return 1;
    }
#else
    lib = LoadLibraryA(path);
    if (!lib) {
      fprintf(stderr, "scva-winmidi: cannot load %s\n"
                      "  pass --core or set SCVA_DLL_DIR\n", path);
      return 1;
    }
    {
      int n = scva_sse3_apply(lib);
      if (n) printf("scva-winmidi: SSE3 shim, %d sites patched\n", n);
    }
#endif
  }
#define GET(v, t, n) v = (t)SCVA_SYM(lib, n); \
  if (!v) { fprintf(stderr, "scva-winmidi: the core has no %s\n", n); return 1; }
  {
    tg_initialize_fn initialize;
    tg_set_max_block_fn set_block;
    tg_set_config_fn set_config;
    GET(initialize, tg_initialize_fn, "TG_initialize")
    GET(set_block, tg_set_max_block_fn, "TG_setMaxBlockSize")
    GET(set_config, tg_set_config_fn, "TG_XPsetSystemConfig")
    GET(set_rate, tg_set_sample_rate_fn, "TG_setSampleRate")
    GET(tg_activate, tg_activate_fn, "TG_activate")
    GET(tg_deactivate, tg_deactivate_fn, "TG_deactivate")
    GET(short_midi, tg_short_midi_fn, "TG_ShortMidiIn")
    GET(long_midi, tg_long_midi_fn, "TG_LongMidiIn")
    GET(process_fn, tg_process_fn, "TG_Process")
    /* The rate is set on both sides of the block size and again immediately
       before activate; otherwise the core emits Inf and NaN silently. */
    initialize(0);
    set_rate((float)rate);
    set_block(block);
    { struct tg_system_config c; c.a = 1; c.b = 1; set_config(&c); }
    set_rate((float)rate);
    tg_activate((float)rate, block);
  }
#undef GET
  {
    static const unsigned char gs_reset[] = {
      0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
    long_midi(gs_reset, 0);
  }

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
  memset(&syshdr, 0, sizeof syshdr);
  syshdr.lpData = (LPSTR)sysbuf;
  syshdr.dwBufferLength = sizeof sysbuf;
  midiInPrepareHeader(hin, &syshdr, sizeof syshdr);
  midiInAddBuffer(hin, &syshdr, sizeof syshdr);
  midiInStart(hin);
  {
    MIDIINCAPSA c;
    if (midiInGetDevCapsA((UINT)dev, &c, sizeof c) == MMSYSERR_NOERROR)
      printf("scva-winmidi[%s]: listening on %d  %s\n", name, dev, c.szPname);
  }

  /* ---- audio out ---- */
  ev = CreateEventA(NULL, FALSE, FALSE, NULL);
  memset(&wf, 0, sizeof wf);
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = 2;
  wf.nSamplesPerSec = rate;
  wf.wBitsPerSample = 16;
  wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
  wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
  if (waveOutOpen(&hout, WAVE_MAPPER, &wf, (DWORD_PTR)ev, 0,
                  CALLBACK_EVENT) != MMSYSERR_NOERROR) {
    fprintf(stderr, "scva-winmidi: cannot open an audio device at %u Hz\n",
            rate);
    return 1;
  }
  printf("scva-winmidi[%s]: audio %u Hz, %d x %d frames (%.0f ms), map %s\n",
         name, rate, nbuf, block, 1000.0 * nbuf * block / rate, mapname);

  hdr = calloc((size_t)nbuf, sizeof *hdr);
  pcm = calloc((size_t)nbuf, sizeof *pcm);
  left = malloc((size_t)block * sizeof *left);
  right = malloc((size_t)block * sizeof *right);
  if (!hdr || !pcm || !left || !right) return 1;
  for (b = 0; b < nbuf; ++b) {
    pcm[b] = calloc((size_t)block * 2, sizeof **pcm);
    hdr[b].lpData = (LPSTR)pcm[b];
    hdr[b].dwBufferLength = (DWORD)(block * 2 * sizeof **pcm);
    waveOutPrepareHeader(hout, &hdr[b], sizeof hdr[b]);
    hdr[b].dwFlags |= WHDR_DONE;          /* all free to begin with */
  }

  printf("scva-winmidi[%s]: playing. Type a map (" SCVA_MAP_USAGE "), <enter>\n"
         "to show it, q or Ctrl+C to stop.\n", name);
  for (;;) {
    int did = 0;
    if (poll_console(map_of)) break;
    for (b = 0; b < nbuf; ++b) {
      struct qmsg m;
      int k;
      if (!(hdr[b].dwFlags & WHDR_DONE)) continue;

      /* Only what is due by the end of this block, so the timing error is one
         block rather than the whole output buffer. Anything already late goes
         in here too: it cannot be put back. */
      rendered += block;
      while (queue_get_due(&m, rendered, (double)rate, (unsigned)latency_ms,
                           (uint64_t)nbuf * block)) {
        if (m.b[0] == 0xf0) {
          long_midi(m.b, 0);
        } else {
          unsigned char st = m.b[0] & 0xf0, ch = m.b[0] & 0x0f;
          unsigned int msg;
          if (st == 0xb0 && m.len >= 3 && m.b[1] == 0x20)
            map_of[ch] = m.b[2];
          else if (st == 0xc0)
            short_midi(scva_map_cc(ch, map_of[ch]), 0);
          msg = m.b[0];
          if (m.len > 1) msg |= (unsigned int)m.b[1] << 8;
          if (m.len > 2) msg |= (unsigned int)m.b[2] << 16;
          short_midi(msg, 0);
        }
      }
      process_fn(left, right, block);
      for (k = 0; k < block; ++k) {
        float l = left[k], r = right[k];
        if (l > 1.0f) l = 1.0f; else if (l < -1.0f) l = -1.0f;
        if (r > 1.0f) r = 1.0f; else if (r < -1.0f) r = -1.0f;
        pcm[b][2 * k]     = (short)(l * 32767.0f);
        pcm[b][2 * k + 1] = (short)(r * 32767.0f);
      }
      hdr[b].dwFlags &= ~WHDR_DONE;
      waveOutWrite(hout, &hdr[b], sizeof hdr[b]);
      did = 1;
    }
    if (!did) WaitForSingleObject(ev, 100);
  }
  printf("scva-winmidi[%s]: stopping\n", name);
  tg_deactivate();
  return 0;
}
