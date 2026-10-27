/* SPDX-License-Identifier: CC0-1.0
 *
 * See live_sched.h.
 */
#include "live_sched.h"
#include <string.h>

void live_sched_init(struct live_sched *s, double rate, unsigned int latency_ms,
                     uint64_t horizon)
{
  memset(s, 0, sizeof *s);
  s->rate = rate;
  s->latency_ms = latency_ms;
  s->horizon = horizon;
}

uint64_t live_sched_position(struct live_sched *s, uint32_t frames)
{
  if (s->have_pos) s->pos_total += (uint32_t)(frames - s->pos_last);
  else s->pos_total = frames;
  s->pos_last = frames;
  s->have_pos = 1;
  return s->pos_total;
}

void live_sched_measure(struct live_sched *s, const uint64_t *played,
                        uint64_t rendered, uint32_t midi_ms)
{
  /* Without a position from the device, assume it is a whole buffer behind
     what was handed to it. */
  uint64_t at = played ? *played
                       : rendered > s->horizon ? rendered - s->horizon : 0;
  double measured = (double)at - (double)midi_ms * s->rate / 1000.0;
  /* Drivers report the position in steps of several milliseconds, so one
     reading is smoothed over the last 16 rather than used as it is. A step
     from an underrun settles within about 16 blocks. */
  if (s->have_origin) s->origin += (measured - s->origin) / 16.0;
  else { s->origin = measured; s->have_origin = 1; }
}

int live_sched_due(const struct live_sched *s, uint32_t when_ms, uint64_t until)
{
  double at = s->origin + ((double)when_ms + s->latency_ms) * s->rate / 1000.0;
  uint64_t due = at > 0.0 ? (uint64_t)at : 0;
  if (due > until + s->horizon) due = until;
  return due <= until;
}
