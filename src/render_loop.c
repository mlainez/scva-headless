/* SPDX-License-Identifier: CC0-1.0
 *
 * See render_loop.h.
 */
#include "render_loop.h"
#include "midi_song.h"
#include <stdlib.h>
#include <string.h>

/* Sends every event due before the end of the block starting at frame. The
   tempo map is followed as it goes, so *at and *last_tick are where the last
   event sent landed. */
static void send_due(const struct song *s, double rate, size_t *ei,
                     uint64_t frame, uint64_t *at, uint64_t *last_tick,
                     double *per_tick, const struct render_port *ports,
                     int nports, unsigned char *sysex_buf, int *sent)
{
  while (*ei < s->count) {
    const struct event *e = s->ev + *ei;
    uint64_t when = *at + (uint64_t)((double)(e->tick - *last_tick) * *per_tick);
    if (when > frame + RENDER_BLOCK) break;
    *at = when;
    *last_tick = e->tick;
    if (e->tempo) {
      *per_tick = song_frames_per_tick(s, rate, e->tempo);
    } else if (e->sysex_len) {
      if (sysex_message(e, sysex_buf, (size_t)s->sysex_max + 1)) {
        /* SysEx carries no channel, so it is addressed to the whole
           machine: both halves of a two-port performance get it. */
        int p;
        for (p = 0; p < nports; ++p) ports[p].long_msg(ports[p].ctx, sysex_buf);
        ++*sent;
      }
    } else {
      unsigned int msg = (unsigned int)e->status |
        ((unsigned int)e->data1 << 8) | ((unsigned int)e->data2 << 16);
      map_policy_short(ports[nports > 1 && e->port].maps, msg);
      ++*sent;
    }
    ++*ei;
  }
}

int render_song(const struct song *s, double rate, double tail,
                const struct render_port *ports, int nports,
                const struct render_io *io, struct render_result *res)
{
  /* Heap rather than stack, for the alignment the core's SIMD stores get
     from malloc. */
  float *buf = calloc(4 * RENDER_BLOCK, sizeof *buf);
  float *left = buf, *right = buf + RENDER_BLOCK;
  float *left_b = buf + 2 * RENDER_BLOCK, *right_b = buf + 3 * RENDER_BLOCK;
  unsigned char *sysex_buf = NULL;
  uint64_t frame = 0, at = 0, last_tick = 0;
  double per_tick = song_frames_per_tick(s, rate, 500000);
  size_t ei = 0;
  int k;

  memset(res, 0, sizeof *res);
  if (!buf) return -1;
  /* One buffer for the longest SysEx in the song: a GS bulk dump runs well
     past the 255 bytes a byte-sized length could hold. */
  if (s->sysex_max) {
    sysex_buf = malloc((size_t)s->sysex_max + 1);
    if (!sysex_buf) { free(buf); return -1; }
  }
  while (ei < s->count || frame < at + (uint64_t)(tail * rate)) {
    send_due(s, rate, &ei, frame, &at, &last_tick, &per_tick, ports, nports,
             sysex_buf, &res->sent);
    memset(left, 0, RENDER_BLOCK * sizeof *left);
    memset(right, 0, RENDER_BLOCK * sizeof *right);
    if (io->pace) io->pace(io->ctx, frame);
    ports[0].process(ports[0].ctx, left, right, RENDER_BLOCK);
    if (nports > 1) {
      memset(left_b, 0, RENDER_BLOCK * sizeof *left_b);
      memset(right_b, 0, RENDER_BLOCK * sizeof *right_b);
      ports[1].process(ports[1].ctx, left_b, right_b, RENDER_BLOCK);
      for (k = 0; k < RENDER_BLOCK; ++k) {
        left[k] += left_b[k];
        right[k] += right_b[k];
      }
    }
    for (k = 0; k < RENDER_BLOCK; ++k) {
      float l = left[k] < 0 ? -left[k] : left[k];
      float r = right[k] < 0 ? -right[k] : right[k];
      if (l > res->peak) res->peak = l;
      if (r > res->peak) res->peak = r;
    }
    if (!io->deliver(io->ctx, left, right, RENDER_BLOCK, frame)) break;
    frame += RENDER_BLOCK;
    res->frames += RENDER_BLOCK;
  }
  free(sysex_buf);
  free(buf);
  return 0;
}
