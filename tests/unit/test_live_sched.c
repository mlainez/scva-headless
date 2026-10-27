/* SPDX-License-Identifier: CC0-1.0
 *
 * When scva-winmidi plays each live message: at its arrival time plus the
 * latency, placed in the output stream by where the device says it is.
 * At 48 kHz a millisecond is 48 frames.
 */
#include "check.h"
#include "live_sched.h"

#define RATE 48000.0

/* A scheduler whose MIDI time 0 is at stream frame origin. */
static struct live_sched at_origin(uint64_t origin, unsigned latency_ms,
                                   uint64_t horizon)
{
  struct live_sched s;
  live_sched_init(&s, RATE, latency_ms, horizon);
  live_sched_measure(&s, &origin, 0, 0);
  return s;
}

/* The first frame by which a message that arrived at when_ms is due. */
static uint64_t first_due(const struct live_sched *s, uint32_t when_ms)
{
  uint64_t f = 0;
  while (!live_sched_due(s, when_ms, f) && f < 1000000) ++f;
  return f;
}

TEST(a_message_is_due_at_its_arrival_plus_the_latency)
{
  struct live_sched s = at_origin(1000, 40, 1 << 20);
  CHECK_INT(first_due(&s, 10), 1000 + 50 * 48);
  CHECK_INT(first_due(&s, 0), 1000 + 40 * 48);
}

TEST(messages_keep_their_spacing)
{
  struct live_sched s = at_origin(0, 40, 1 << 20);
  CHECK_INT(first_due(&s, 125) - first_due(&s, 100), 25 * 48);
}

TEST(a_message_from_before_the_stream_started_is_due_at_once)
{
  struct live_sched s;
  uint64_t played = 0;
  live_sched_init(&s, RATE, 0, 4096);
  live_sched_measure(&s, &played, 0, 1000);      /* MIDI started 1 s earlier */
  CHECK(live_sched_due(&s, 0, 0));
  CHECK(live_sched_due(&s, 999, 0));
  CHECK(!live_sched_due(&s, 1001, 0));
}

TEST(no_message_is_held_longer_than_the_buffer_is_deep)
{
  struct live_sched deep = at_origin(0, 40, 4096);
  struct live_sched shallow = at_origin(0, 40, 1024);
  /* due at 1920: inside a 4096-frame buffer, so held; past a 1024-frame one,
     so let through rather than trusting a position that far off */
  CHECK(!live_sched_due(&deep, 0, 0));
  CHECK(live_sched_due(&shallow, 0, 0));
  CHECK(live_sched_due(&shallow, 100000, 0));
}

TEST(the_time_since_midi_started_is_taken_off_the_position)
{
  struct live_sched s;
  uint64_t played = 48000;
  live_sched_init(&s, RATE, 0, 1 << 20);
  live_sched_measure(&s, &played, 0, 1000);      /* 1 s in, 1 s played */
  CHECK_INT(first_due(&s, 1000), 48000);
}

TEST(a_new_position_moves_the_origin_a_sixteenth_of_the_way)
{
  struct live_sched s = at_origin(0, 0, 1 << 20);
  uint64_t played = 1600;
  live_sched_measure(&s, &played, 0, 0);
  CHECK_INT(first_due(&s, 0), 100);
}

TEST(a_steady_position_is_settled_on)
{
  struct live_sched s = at_origin(0, 0, 1 << 20);
  uint64_t played = 4800;
  int i;
  uint64_t f;
  for (i = 0; i < 400; ++i) live_sched_measure(&s, &played, 0, 0);
  f = first_due(&s, 0);
  CHECK(f >= 4799 && f <= 4800);            /* to within a frame */
}

TEST(without_a_position_the_device_is_taken_to_be_a_buffer_behind)
{
  struct live_sched s;
  live_sched_init(&s, RATE, 0, 4096);
  live_sched_measure(&s, NULL, 10000, 0);
  CHECK(!live_sched_due(&s, 0, 10000 - 4096 - 1));
  CHECK(live_sched_due(&s, 0, 10000 - 4096));
  live_sched_init(&s, RATE, 0, 4096);
  live_sched_measure(&s, NULL, 1000, 0);        /* less than a buffer yet */
  CHECK_INT(first_due(&s, 0), 0);
}

TEST(the_devices_position_keeps_counting_past_32_bits)
{
  struct live_sched s;
  live_sched_init(&s, RATE, 0, 4096);
  CHECK_INT(live_sched_position(&s, 0xffffff00u), 0xffffff00u);
  CHECK_INT(live_sched_position(&s, 0x100u), 0x100000100ull);
  CHECK_INT(live_sched_position(&s, 0x200u), 0x100000200ull);
}

TEST(the_first_position_reading_is_taken_as_it_is)
{
  struct live_sched s;
  live_sched_init(&s, RATE, 0, 4096);
  CHECK_INT(live_sched_position(&s, 12345), 12345);
}

int main(void)
{
  RUN(a_message_is_due_at_its_arrival_plus_the_latency);
  RUN(messages_keep_their_spacing);
  RUN(a_message_from_before_the_stream_started_is_due_at_once);
  RUN(no_message_is_held_longer_than_the_buffer_is_deep);
  RUN(the_time_since_midi_started_is_taken_off_the_position);
  RUN(a_new_position_moves_the_origin_a_sixteenth_of_the_way);
  RUN(a_steady_position_is_settled_on);
  RUN(without_a_position_the_device_is_taken_to_be_a_buffer_behind);
  RUN(the_devices_position_keeps_counting_past_32_bits);
  RUN(the_first_position_reading_is_taken_as_it_is);
  return test_summary();
}
