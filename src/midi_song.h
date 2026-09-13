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
  uint8_t port;                         /* 0 = port A, 1 = port B */
  uint8_t sysex_len;
  const unsigned char *sysex;
};

struct song {
  struct event *ev;
  size_t count, cap;
  uint16_t division;
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
  for (i = 0; i + 4 < len + 1 && i + 4 <= len; ++i)
    if ((p[i] == 'P' || p[i] == 'p') && (p[i+1] == 'a' || p[i+1] == 'A') &&
        (p[i+2] == 'r' || p[i+2] == 'R') && (p[i+3] == 't' || p[i+3] == 'T') &&
        i + 4 < len && (p[i+4] == 'B' || p[i+4] == 'b'))
      return 1;
  return 0;
}

static int parse_track(struct song *s, const unsigned char *p, size_t n)
{
  uint64_t tick = 0;
  unsigned char running = 0;
  size_t i = 0;
  size_t first = s->count;
  int port = 0;
  while (i < n) {
    struct event e;
    uint32_t delta, len;
    unsigned char st;
    if (!vlq(p, n, &i, &delta) || i >= n) return 0;
    tick += delta;
    st = p[i];
    if (st & 0x80) { ++i; if (st < 0xf0) running = st; }
    else { st = running; if (!st) return 0; }
    memset(&e, 0, sizeof e);
    e.tick = tick;
    e.status = st;
    if (st == 0xff) {
      unsigned char type;
      if (i >= n) return 0;
      type = p[i++];
      if (!vlq(p, n, &i, &len) || i + len > n) return 0;
      if (type == 0x51 && len == 3) {
        e.tempo = (uint32_t)p[i] << 16 | (uint32_t)p[i + 1] << 8 | p[i + 2];
        push(s, &e);
      } else if (type == 0x03 && track_name_port(p + i, len)) {
        port = 1;
      } else if (type == 0x21 && len == 1 && p[i]) {
        port = p[i] ? 1 : 0;     /* FF 21, when a file bothers to carry it */
      }
      i += len;
      if (type == 0x2f) break;
    } else if (st == 0xf0 || st == 0xf7) {
      if (!vlq(p, n, &i, &len) || i + len > n) return 0;
      if (len && len <= 255) {
        e.sysex = p + i;
        e.sysex_len = (uint8_t)len;
        push(s, &e);
      }
      i += len;
    } else {
      unsigned want = ((st & 0xf0) == 0xc0 || (st & 0xf0) == 0xd0) ? 1u : 2u;
      if (i + want > n) return 0;
      e.data1 = p[i];
      e.data2 = want == 2 ? p[i + 1] : 0;
      i += want;
      push(s, &e);
    }
  }
  if (port)
    for (i = first; i < s->count; ++i) s->ev[i].port = (uint8_t)port;
  return 1;
}

/* How many ports the song actually uses. */
static int song_ports(const struct song *s)
{
  size_t i;
  for (i = 0; i < s->count; ++i)
    if (s->ev[i].port) return 2;
  return 1;
}

static int cmp(const void *a, const void *b)
{
  const struct event *x = a, *y = b;
  if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
  return x->order < y->order ? -1 : x->order > y->order;
}

static int parse(struct song *s, const unsigned char *p, size_t n)
{
  uint16_t tracks, format;
  size_t i;
  unsigned t;
  memset(s, 0, sizeof *s);
  if (n < 14 || memcmp(p, "MThd", 4) || be32(p + 4) < 6) return 0;
  format = (uint16_t)(p[8] << 8 | p[9]);
  tracks = (uint16_t)(p[10] << 8 | p[11]);
  s->division = (uint16_t)(p[12] << 8 | p[13]);
  if (format > 2 || !s->division || (s->division & 0x8000)) return 0;
  i = 8 + be32(p + 4);
  for (t = 0; t < tracks && i + 8 <= n; ++t) {
    uint32_t len = be32(p + i + 4);
    if (memcmp(p + i, "MTrk", 4) || i + 8 + len > n) return 0;
    if (!parse_track(s, p + i + 8, len)) return 0;
    i += 8 + len;
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

static void header(FILE *f, unsigned rate, uint32_t frames)
{
  uint32_t data = frames * 2u * 4u;
  fwrite("RIFF", 1, 4, f); put32(f, 36u + data);
  fwrite("WAVEfmt ", 1, 8, f); put32(f, 16);
  put16(f, 3); put16(f, 2);
  put32(f, rate); put32(f, rate * 8u);
  put16(f, 8); put16(f, 32);
  fwrite("data", 1, 4, f); put32(f, data);
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
