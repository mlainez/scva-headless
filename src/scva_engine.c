/* SPDX-License-Identifier: CC0-1.0
 *
 * The engine half of the daemon, run under wine.
 *
 * Reads framed MIDI from stdin, writes interleaved float32 stereo to stdout,
 * and never blocks on either: it drains whatever MIDI has arrived, renders one
 * block, and writes it.  The Linux half paces the whole thing by consuming the
 * audio at the sound card's rate, so backpressure through the pipe IS the
 * clock and there is no timer here to drift.
 *
 * Frame format on stdin:
 *   0x01 b0 b1 b2      one short message, already packed little-endian
 *   0x02 len32 bytes   one sysex, length little-endian
 */
#include "scva_core.h"
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>

static HANDLE in_h;

/* Non-blocking: return how many bytes are sitting in the pipe right now. */
static DWORD pending(void)
{
  DWORD avail = 0;
  if (!PeekNamedPipe(in_h, NULL, 0, NULL, &avail, NULL)) return 0;
  return avail;
}

static int read_exact(unsigned char *p, size_t n)
{
  size_t got = 0;
  while (got < n) {
    DWORD r = 0;
    if (!ReadFile(in_h, p + got, (DWORD)(n - got), &r, NULL) || r == 0) return 0;
    got += r;
  }
  return 1;
}

int main(int argc, char **argv)
{
  const char *dll = "SCCore.dll";
  double rate = 44100.0;
  int block = 256, maxblock = 4096, i;
  struct scva s;
  float *L, *R, *out;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--core") && i + 1 < argc) dll = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    else { fprintf(stderr, "usage: scva_engine [--core DLL] [--rate HZ] [--block N]\n"); return 2; }
  }
  if (block < 16) block = 16;
  if (block > maxblock) block = maxblock;

  in_h = GetStdHandle(STD_INPUT_HANDLE);
  _setmode(_fileno(stdout), _O_BINARY);

  if (!scva_open(&s, dll, rate, maxblock)) return 1;
  scva_gs_reset(&s);
  fprintf(stderr, "scva_engine: ready, %.0f Hz, block %d\n", rate, block);
  fflush(stderr);

  /* The engine writes past the frame count it is given, so its buffers are
     sized for the declared maximum rather than for the block. */
  L = calloc((size_t)maxblock, sizeof *L);
  R = calloc((size_t)maxblock, sizeof *R);
  out = calloc((size_t)maxblock * 2, sizeof *out);
  if (!L || !R || !out) return 1;

  for (;;) {
    while (pending() >= 1) {
      unsigned char tag;
      if (!read_exact(&tag, 1)) goto done;
      if (tag == 0x01) {
        unsigned char b[3];
        unsigned int msg;
        if (!read_exact(b, 3)) goto done;
        msg = (unsigned int)b[0] | ((unsigned int)b[1] << 8) |
              ((unsigned int)b[2] << 16);
        s.short_midi(msg, 0);
      } else if (tag == 0x02) {
        unsigned char len[4];
        unsigned char *buf;
        unsigned int n;
        if (!read_exact(len, 4)) goto done;
        n = (unsigned int)len[0] | ((unsigned int)len[1] << 8) |
            ((unsigned int)len[2] << 16) | ((unsigned int)len[3] << 24);
        if (n == 0 || n > (1u << 20)) goto done;
        buf = malloc(n);
        if (!buf || !read_exact(buf, n)) { free(buf); goto done; }
        s.long_midi(buf, 0);
        free(buf);
      } else {
        goto done;                      /* desynchronised: stop rather than guess */
      }
    }
    memset(L, 0, (size_t)maxblock * sizeof *L);
    memset(R, 0, (size_t)maxblock * sizeof *R);
    s.process(L, R, block);
    for (i = 0; i < block; ++i) { out[2*i] = L[i]; out[2*i+1] = R[i]; }
    if (fwrite(out, sizeof *out, (size_t)block * 2, stdout) != (size_t)block * 2)
      break;
    fflush(stdout);
  }
done:
  scva_close(&s);
  return 0;
}
