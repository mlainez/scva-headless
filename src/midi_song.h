/* SPDX-License-Identifier: CC0-1.0
 *
 * Standard MIDI File reading and float32 WAV writing, shared by the wine
 * renderer and the native one so both drive the engine from the same events.
 * Nothing here touches the engine.
 */
#ifndef SCVA_MIDI_SONG_H
#define SCVA_MIDI_SONG_H
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scva_map.h"

struct event {
  uint64_t tick;
  uint32_t order;
  uint32_t tempo;                       /* nonzero for a tempo change */
  uint8_t status, data1, data2;
  uint8_t port;                         /* as the file numbers them: 0 is A */
  uint32_t sysex_len;
  const unsigned char *sysex;
};

struct song {
  struct event *ev;
  size_t count, cap;
  uint16_t division;
  uint32_t sysex_max;                   /* longest SysEx, for the send buffer */
  double smpte_tick;                    /* seconds per tick, 0 when the tempo
                                           map decides instead */
};

static void push(struct song *s, const struct event *e)
{
  if (s->count == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 2048;
    s->ev = realloc(s->ev, s->cap * sizeof *s->ev);
    if (!s->ev) { fprintf(stderr, "out of memory\n"); exit(1); }
  }
  s->ev[s->count] = *e;
  s->ev[s->count].order = (uint32_t)s->count;
  ++s->count;
}

static int vlq(const unsigned char *p, size_t n, size_t *i, uint32_t *out)
{
  uint32_t v = 0;
  int k;
  for (k = 0; k < 4; ++k) {
    unsigned char c;
    if (*i >= n) return 0;
    c = p[(*i)++];
    v = (v << 7) | (c & 0x7f);
    if (!(c & 0x80)) { *out = v; return 1; }
  }
  return 0;
}

static uint32_t be32(const unsigned char *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Roland's 32-part demo files state the port only in the track NAME, as
   "PartA nch." / "PartB nch.", and carry no FF 21. The name can arrive after
   events have been pushed, so the port is applied to the whole track at the
   end rather than as events are read. */
static int track_name_port(const unsigned char *p, uint32_t len)
{
  uint32_t i;
  for (i = 0; i + 4 < len; ++i)
    if ((p[i] == 'P' || p[i] == 'p') && (p[i+1] == 'a' || p[i+1] == 'A') &&
        (p[i+2] == 'r' || p[i+2] == 'R') && (p[i+3] == 't' || p[i+3] == 'T') &&
        (p[i+4] == 'B' || p[i+4] == 'b'))
      return 1;
  return 0;
}

/* A damaged track stops where the damage is; whatever parsed before it still
   plays, and the other tracks are untouched. Files in the wild are truncated
   often enough that refusing the whole song over one bad byte loses music
   that every other player manages to sound. */
static void parse_track(struct song *s, const unsigned char *p, size_t n)
{
  uint64_t tick = 0;
  unsigned char running = 0;
  size_t i = 0;
  size_t first = s->count;
  int port = 0, port_from_meta = 0;
  while (i < n) {
    struct event e;
    uint32_t delta, len;
    unsigned char st;
    if (!vlq(p, n, &i, &delta) || i >= n) break;
    tick += delta;
    st = p[i];
    if (st & 0x80) { ++i; if (st < 0xf0) running = st; }
    else { st = running; if (!st) break; }
    memset(&e, 0, sizeof e);
    e.tick = tick;
    e.status = st;
    if (st == 0xff) {
      unsigned char type;
      if (i >= n) break;
      type = p[i++];
      if (!vlq(p, n, &i, &len) || i + len > n) break;
      if (type == 0x51 && len == 3) {
        e.tempo = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        push(s, &e);
      } else if (type == 0x03 && !port_from_meta &&
                 track_name_port(p + i, len)) {
        port = 1;
      } else if (type == 0x21 && len == 1) {
        port = p[i];             /* FF 21 is explicit, so it beats the name */
        port_from_meta = 1;
      }
      i += len;
      if (type == 0x2f) break;
    } else if (st == 0xf0 || st == 0xf7) {
      if (!vlq(p, n, &i, &len) || i + len > n) break;
      if (len) {
        e.sysex = p + i;
        e.sysex_len = len;
        if (len > s->sysex_max) s->sysex_max = len;
        push(s, &e);
      }
      i += len;
    } else {
      unsigned want = ((st & 0xf0) == 0xc0 || (st & 0xf0) == 0xd0) ? 1u : 2u;
      if (i + want > n) break;
      e.data1 = p[i];
      e.data2 = want == 2 ? p[i + 1] : 0;
      i += want;
      /* A data byte has its top bit clear. Files exist that break that rule,
         and the engine follows such a controller number out of its own table
         and into a null pointer, so the message is dropped rather than
         played. */
      if (!(e.data1 & 0x80) && !(e.data2 & 0x80))
        push(s, &e);
    }
  }
  if (port)
    for (i = first; i < s->count; ++i) s->ev[i].port = (uint8_t)port;
}

/* How many ports the song actually uses: one past the highest it names. The
   engine is a 16-part machine, so each port costs another instance. */
static int song_ports(const struct song *s)
{
  int top = 0;
  size_t i;
  for (i = 0; i < s->count; ++i)
    if (s->ev[i].port > top) top = s->ev[i].port;
  return top + 1;
}

/* Frames per tick. An SMPTE file measures its ticks in real time, so the
   tempo map has no say there. */
static double song_frames_per_tick(const struct song *s, double rate,
                                   uint32_t tempo)
{
  if (s->smpte_tick > 0.0) return rate * s->smpte_tick;
  return rate * (double)tempo / (1e6 * s->division);
}

static int cmp(const void *a, const void *b)
{
  const struct event *x = a, *y = b;
  if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
  return x->order < y->order ? -1 : x->order > y->order;
}

static int parse(struct song *s, const unsigned char *p, size_t n)
{
  uint16_t format;
  size_t i;
  memset(s, 0, sizeof *s);
  if (n < 14 || memcmp(p, "MThd", 4) || be32(p + 4) < 6) return 0;
  format = (uint16_t)(p[8] << 8 | p[9]);
  s->division = (uint16_t)(p[12] << 8 | p[13]);
  if (format > 2 || !s->division) return 0;
  if (s->division & 0x8000) {
    /* SMPTE: a negative frame rate in the high byte, ticks per frame in the
       low one. 29 is the 29.97 of drop frame. */
    double fps = -(double)(signed char)(s->division >> 8);
    double sub = (double)(s->division & 0xff);
    if (fps == 29.0) fps = 30000.0 / 1001.0;
    if (fps <= 0.0 || sub <= 0.0) return 0;
    s->smpte_tick = 1.0 / (fps * sub);
  }
  /* Walk the chunks the file actually holds rather than the track count it
     declares: the two disagree in the wild, and a chunk this does not know
     is something to step over, not a reason to refuse the song. */
  i = 8 + be32(p + 4);
  while (i + 8 <= n) {
    uint32_t len = be32(p + i + 4);
    size_t body = i + 8;
    if ((size_t)len > n - body) len = (uint32_t)(n - body);   /* truncated */
    if (!memcmp(p + i, "MTrk", 4)) parse_track(s, p + body, len);
    i = body + len;
  }
  qsort(s->ev, s->count, sizeof *s->ev, cmp);
  return s->count > 0;
}

/* An SMF stores an F0 event without its leading F0; the engine wants the whole
   message. An F7 event is a raw continuation. Returns 0 if it does not fit. */
static int sysex_message(const struct event *e, unsigned char *buf, size_t cap)
{
  size_t n = e->sysex_len;
  if (e->status == 0xf7) {
    if (n > cap) return 0;
    memcpy(buf, e->sysex, n);
    return (int)n;
  }
  if (n + 1 > cap) return 0;
  buf[0] = 0xf0;
  memcpy(buf + 1, e->sysex, n);
  return (int)n + 1;
}

/* ------------------------------------------------------------------ WAV */

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

/* bits is 32 for float samples, the engine's own format, or 16 for the
   integer PCM that every player accepts. */
static void header(FILE *f, unsigned rate, uint32_t frames, int bits)
{
  unsigned bytes = bits == 16 ? 2u : 4u;
  uint32_t data = frames * 2u * bytes;
  fwrite("RIFF", 1, 4, f); put32(f, 36u + data);
  fwrite("WAVEfmt ", 1, 8, f); put32(f, 16);
  put16(f, bits == 16 ? 1 : 3); put16(f, 2);
  put32(f, rate); put32(f, rate * 2u * bytes);
  put16(f, (uint16_t)(2u * bytes)); put16(f, (uint16_t)bits);
  fwrite("data", 1, 4, f); put32(f, data);
}

/* Scaling by 32768 rather than 32767 is what makes this reproducible: it is a
   power of two, so the multiply is exact whatever width the machine evaluates
   in, and the truncation that follows cannot land either side of an integer
   depending on whether an x87 register held 80 bits or 32. */
static int16_t to_s16(float v)
{
  int n;
  if (v > 1.0f) v = 1.0f; else if (v < -1.0f) v = -1.0f;
  n = (int)(v * 32768.0f);
  if (n > 32767) n = 32767;
  if (n < -32768) n = -32768;
  return (int16_t)n;
}

/* One frame out. Float is what the engine produced; 16-bit clips, because
   summed voices can pass full scale and an integer sample cannot. */
static void put_frame(FILE *f, float l, float r, int bits)
{
  if (bits != 16) {
    fwrite(&l, 4, 1, f);
    fwrite(&r, 4, 1, f);
    return;
  }
  put16(f, (uint16_t)to_s16(l));
  put16(f, (uint16_t)to_s16(r));
}

static unsigned char *slurp(const char *path, size_t *n)
{
  FILE *f = fopen(path, "rb");
  unsigned char *b;
  long len;
  if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
  fseek(f, 0, SEEK_END); len = ftell(f); rewind(f);
  b = malloc((size_t)len);
  if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) {
    free(b); fclose(f); fprintf(stderr, "cannot read %s\n", path); return NULL;
  }
  fclose(f); *n = (size_t)len; return b;
}
#endif
