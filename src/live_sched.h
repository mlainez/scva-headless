/* SPDX-License-Identifier: CC0-1.0
 *
 * When live MIDI is played, for a tool that renders ahead into an output
 * buffer and knows when each message arrived. Each message is due at the
 * frame where its arrival time falls in the output stream, plus a fixed
 * latency, so a deep buffer delays the music without smearing its rhythm.
 *
 * The stream's frame at MIDI time 0 - the origin - is measured from where the
 * device says it is playing, every time round the loop, and smoothed, so an
 * underrun or two drifting clocks move it rather than delay every message
 * after them. Nothing here touches a device or a clock; the caller passes
 * the readings in.
 */
#ifndef LIVE_SCHED_H
#define LIVE_SCHED_H
#include <stdint.h>

struct live_sched {
  double rate;
  unsigned int latency_ms;
  uint64_t horizon;             /* frames between rendering and hearing */
  double origin;                /* stream frame at MIDI time 0 */
  int have_origin;
  uint32_t pos_last;            /* the device's last position reading */
  uint64_t pos_total;
  int have_pos;
};

/* horizon is the output buffer's depth in frames. */
void live_sched_init(struct live_sched *s, double rate, unsigned int latency_ms,
                     uint64_t horizon);

/* A position reading from the device, a 32-bit frame count that wraps,
   as a 64-bit count since the first reading. */
uint64_t live_sched_position(struct live_sched *s, uint32_t frames);

/* Re-measures the origin. played is the frame the device is playing, or
   NULL when it cannot say; rendered is how many frames were handed to it;
   midi_ms is the MIDI time now. */
void live_sched_measure(struct live_sched *s, const uint64_t *played,
                        uint64_t rendered, uint32_t midi_ms);

/* Whether a message that arrived at MIDI time when_ms is due by the stream
   frame until. A message is never held longer than the horizon, so a bad
   position reading cannot stall the queue. */
int live_sched_due(const struct live_sched *s, uint32_t when_ms, uint64_t until);
#endif
