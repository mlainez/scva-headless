/* SPDX-License-Identifier: CC0-1.0
 *
 * See render_loop.h.
 */
#include "render_loop.h"
#include "midi_song.h"
#include <stdlib.h>
#include <string.h>

/* Where the song is: the next event to send, and where the last one sent
   landed. Block-start timing counts in whole frames; nearest-block timing in
   fractions of one, as VSC's own player does. */
struct cursor {
  int nearest;
  int block;
  size_t ei;
  uint64_t at, last_tick;
  double at_f;
  double per_tick;
};

static void dispatch(const struct song *s, const struct event *e,
                     const struct render_port *ports, int nports,
                     unsigned char *sysex_buf, int *sent)
{
  if (e->sysex_len) {
    int n = sysex_message(e, sysex_buf, (size_t)s->sysex_max + 1);
    if (n) {
      /* SysEx carries no channel, so it is addressed to the whole machine:
         both halves of a two-port performance get it. */
      int p;
      for (p = 0; p < nports; ++p) ports[p].long_msg(ports[p].ctx, sysex_buf, (size_t)n);
      ++*sent;
    }
  } else {
    unsigned int msg = (unsigned int)e->status |
      ((unsigned int)e->data1 << 8) | ((unsigned int)e->data2 << 16);
    map_policy_short(ports[nports > 1 && e->port].maps, msg);
    ++*sent;
  }
}

/* Sends every event that goes in ahead of the block starting at frame: with
   block-start timing, those due before it ends; with nearest-block timing,
   those due before its middle, so each lands on the boundary nearest it. The
   tempo map is followed as it goes. */
static void send_due(const struct song *s, double rate, struct cursor *c,
                     uint64_t frame, const struct render_port *ports, int nports,
                     unsigned char *sysex_buf, int *sent)
{
  while (c->ei < s->count) {
    const struct event *e = s->ev + c->ei;
    if (c->nearest) {
      double t = c->at_f + (double)(e->tick - c->last_tick) * c->per_tick;
      if (t >= (double)frame + c->block / 2.0) break;
      c->at_f = t;
    } else {
      uint64_t when = c->at + (uint64_t)((double)(e->tick - c->last_tick) * c->per_tick);
      if (when > frame + (uint64_t)c->block) break;
      c->at = when;
    }
    c->last_tick = e->tick;
    if (e->tempo) c->per_tick = song_frames_per_tick(s, rate, e->tempo);
    else dispatch(s, e, ports, nports, sysex_buf, sent);
    ++c->ei;
  }
}

/* Whether the song still has events to send or tail to play at frame. */
static int playing(const struct song *s, const struct cursor *c, uint64_t frame,
                   double rate, double tail)
{
  if (c->ei < s->count) return 1;
  if (c->nearest) return (double)frame < c->at_f + tail * rate;
  return frame < c->at + (uint64_t)(tail * rate);
}

int render_song(const struct song *s, double rate, double tail, int midi_block,
                const struct render_port *ports, int nports,
                const struct render_io *io, struct render_result *res)
{
  struct cursor c;
  int block = midi_block > 1 ? midi_block : RENDER_BLOCK;
  /* Heap rather than stack, for the alignment the core's SIMD stores get
     from malloc. */
  float *buf = calloc(4 * (size_t)block, sizeof *buf);
  float *left = buf, *right = buf + block;
  float *left_b = buf + 2 * block, *right_b = buf + 3 * block;
  unsigned char *sysex_buf = NULL;
  uint64_t frame = 0;
  int k;

  memset(res, 0, sizeof *res);
  memset(&c, 0, sizeof c);
  c.nearest = midi_block > 1;
  c.block = block;
  c.per_tick = song_frames_per_tick(s, rate, 500000);
  if (!buf) return -1;
  /* One buffer for the longest SysEx in the song: a GS bulk dump runs well
     past the 255 bytes a byte-sized length could hold. */
  if (s->sysex_max) {
    sysex_buf = malloc((size_t)s->sysex_max + 1);
    if (!sysex_buf) { free(buf); return -1; }
  }
  /* Block-start timing asks whether the song is over before sending a
     block's events, nearest-block timing after: each is the order of the
     renderer it has to match. */
  for (;;) {
    if (!c.nearest && !playing(s, &c, frame, rate, tail)) break;
    send_due(s, rate, &c, frame, ports, nports, sysex_buf, &res->sent);
    if (c.nearest && !playing(s, &c, frame, rate, tail)) break;
    memset(left, 0, (size_t)block * sizeof *left);
    memset(right, 0, (size_t)block * sizeof *right);
    if (io->pace) io->pace(io->ctx, frame);
    ports[0].process(ports[0].ctx, left, right, block);
    if (nports > 1) {
      memset(left_b, 0, (size_t)block * sizeof *left_b);
      memset(right_b, 0, (size_t)block * sizeof *right_b);
      ports[1].process(ports[1].ctx, left_b, right_b, block);
      for (k = 0; k < block; ++k) {
        left[k] += left_b[k];
        right[k] += right_b[k];
      }
    }
    for (k = 0; k < block; ++k) {
      float l = left[k] < 0 ? -left[k] : left[k];
      float r = right[k] < 0 ? -right[k] : right[k];
      if (l > res->peak) res->peak = l;
      if (r > res->peak) res->peak = r;
    }
    if (!io->deliver(io->ctx, left, right, block, frame)) break;
    frame += (uint64_t)block;
    res->frames += (uint32_t)block;
  }
  free(sysex_buf);
  free(buf);
  return 0;
}
