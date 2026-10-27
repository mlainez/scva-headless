/* SPDX-License-Identifier: CC0-1.0
 *
 * How a Standard MIDI File becomes the events the renderers play: order,
 * timing, ports, SysEx, and what happens to the damaged files found in the
 * wild. Also the WAV files the renderers write.
 */
#include "check.h"
#include <stdarg.h>
#include "midi_song.h"

/* ---- a small SMF writer ------------------------------------------------ */

struct smf { unsigned char b[16384]; size_t n; };

static void put(struct smf *s, const void *p, size_t n)
{
  memcpy(s->b + s->n, p, n);
  s->n += n;
}

static void put_be(struct smf *s, unsigned v, int bytes)
{
  while (bytes--) s->b[s->n++] = (unsigned char)(v >> (8 * bytes));
}

static void put_vlq(struct smf *s, unsigned v)
{
  unsigned char tmp[4];
  int k = 0;
  tmp[k++] = v & 0x7f;
  while (v >>= 7) tmp[k++] = (unsigned char)(0x80 | (v & 0x7f));
  while (k--) s->b[s->n++] = tmp[k];
}

static void smf_header(struct smf *s, int format, int ntracks, unsigned division)
{
  s->n = 0;
  put(s, "MThd", 4);
  put_be(s, 6, 4);
  put_be(s, (unsigned)format, 2);
  put_be(s, (unsigned)ntracks, 2);
  put_be(s, division, 2);
}

static size_t track_begin(struct smf *s)
{
  size_t at = s->n;
  put(s, "MTrk", 4);
  put_be(s, 0, 4);
  return at;
}

static void track_end(struct smf *s, size_t at)
{
  unsigned len = (unsigned)(s->n - at - 8);
  s->b[at + 4] = (unsigned char)(len >> 24);
  s->b[at + 5] = (unsigned char)(len >> 16);
  s->b[at + 6] = (unsigned char)(len >> 8);
  s->b[at + 7] = (unsigned char)len;
}

/* One event: a delta time, then n raw bytes. */
static void ev(struct smf *s, unsigned delta, int n, ...)
{
  va_list ap;
  put_vlq(s, delta);
  va_start(ap, n);
  while (n--) s->b[s->n++] = (unsigned char)va_arg(ap, int);
  va_end(ap);
}

static void meta(struct smf *s, unsigned delta, int type, const void *data, unsigned len)
{
  put_vlq(s, delta);
  s->b[s->n++] = 0xff;
  s->b[s->n++] = (unsigned char)type;
  put_vlq(s, len);
  put(s, data, len);
}

static void end_of_track(struct smf *s) { meta(s, 0, 0x2f, "", 0); }

static int parse_ok(struct song *song, const struct smf *s)
{
  return parse(song, s->b, s->n);
}

/* ---- order and timing -------------------------------------------------- */

TEST(events_from_every_track_play_in_time_order)
{
  struct smf s; struct song song; size_t t;
  smf_header(&s, 1, 2, 480);
  t = track_begin(&s);
  ev(&s, 0, 3, 0x90, 60, 100);
  ev(&s, 480, 3, 0x80, 60, 0);
  end_of_track(&s); track_end(&s, t);
  t = track_begin(&s);
  ev(&s, 240, 3, 0x91, 64, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.count, 3);
  CHECK_INT(song.ev[0].tick, 0);   CHECK_INT(song.ev[0].status, 0x90);
  CHECK_INT(song.ev[1].tick, 240); CHECK_INT(song.ev[1].status, 0x91);
  CHECK_INT(song.ev[2].tick, 480); CHECK_INT(song.ev[2].status, 0x80);
  free(song.ev);
}

TEST(simultaneous_events_keep_the_order_the_file_gives_them)
{
  /* A controller written before a program change at the same tick must reach
     the engine first, or a bank select would land after the patch it picks. */
  struct smf s; struct song song; size_t t;
  smf_header(&s, 1, 2, 480);
  t = track_begin(&s);
  ev(&s, 10, 3, 0xb0, 0, 8);
  ev(&s, 0, 2, 0xc0, 4);
  end_of_track(&s); track_end(&s, t);
  t = track_begin(&s);
  ev(&s, 10, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.count, 3);
  CHECK_INT(song.ev[0].status, 0xb0);
  CHECK_INT(song.ev[1].status, 0xc0);
  CHECK_INT(song.ev[2].status, 0x90);
  free(song.ev);
}

TEST(a_chord_of_many_simultaneous_events_keeps_file_order)
{
  /* Enough events at one tick, spread over tracks and interleaved with
     events at other ticks, that a sort which does not honour file order
     reorders them on any C library. */
  struct smf s; struct song song; size_t t, i;
  int k, expect = 0, tr;
  smf_header(&s, 1, 4, 480);
  for (tr = 0; tr < 4; ++tr) {
    t = track_begin(&s);
    ev(&s, 0, 3, 0x90 | tr, 100, 1);           /* earlier, as a decoy */
    for (k = 0; k < 16; ++k)
      ev(&s, k == 0 ? 96 : 0, 3, 0xb0 | tr, 16 + k, tr * 16 + k);
    ev(&s, 96, 3, 0x80 | tr, 100, 0);           /* later, as a decoy */
    end_of_track(&s); track_end(&s, t);
  }
  CHECK(parse_ok(&song, &s));
  for (i = 0; i < song.count; ++i) {
    if (song.ev[i].tick != 96) continue;
    /* track 0's sixteen controllers, then track 1's, and so on */
    CHECK_INT(song.ev[i].data2, expect);
    ++expect;
  }
  CHECK_INT(expect, 64);
  free(song.ev);
}

TEST(running_status_repeats_the_previous_status)
{
  struct smf s; struct song song; size_t t;
  smf_header(&s, 0, 1, 96);
  t = track_begin(&s);
  ev(&s, 0, 3, 0x92, 60, 100);
  ev(&s, 0, 2, 62, 90);              /* no status byte */
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.count, 2);
  CHECK_INT(song.ev[1].status, 0x92);
  CHECK_INT(song.ev[1].data1, 62);
  CHECK_INT(song.ev[1].data2, 90);
  free(song.ev);
}

TEST(a_tempo_change_sets_how_long_a_tick_lasts)
{
  struct smf s; struct song song; size_t t;
  unsigned char slow[3] = { 0x07, 0xa1, 0x20 };      /* 500000 us: 120 bpm */
  smf_header(&s, 0, 1, 480);
  t = track_begin(&s);
  meta(&s, 0, 0x51, slow, 3);
  ev(&s, 480, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.ev[0].tempo, 500000);
  /* At 120 bpm a quarter note (480 ticks) lasts half a second. */
  CHECK_NEAR(480 * song_frames_per_tick(&song, 48000, 500000), 24000, 1e-6);
  CHECK_NEAR(480 * song_frames_per_tick(&song, 44100, 250000), 11025, 1e-6);
  free(song.ev);
}

TEST(smpte_timing_counts_real_time_and_ignores_tempo)
{
  struct smf s; struct song song; size_t t;
  /* -25 frames per second, 40 ticks per frame: a tick is one millisecond. */
  smf_header(&s, 0, 1, 0xe728);
  t = track_begin(&s);
  ev(&s, 1000, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_NEAR(1000 * song_frames_per_tick(&song, 48000, 500000), 48000, 1e-6);
  CHECK_NEAR(1000 * song_frames_per_tick(&song, 48000, 250000), 48000, 1e-6);
  free(song.ev);
}

TEST(smpte_29_means_drop_frame_29_97)
{
  struct smf s; struct song song; size_t t;
  smf_header(&s, 0, 1, 0xe350);                 /* -29 fps, 80 per frame */
  t = track_begin(&s);
  ev(&s, 0, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_NEAR(song_frames_per_tick(&song, 48000, 500000),
             48000.0 * 1001.0 / (30000.0 * 80.0), 1e-9);
  free(song.ev);
}

/* ---- ports ------------------------------------------------------------- */

TEST(a_song_on_one_port_needs_one_engine)
{
  struct smf s; struct song song; size_t t;
  smf_header(&s, 0, 1, 480);
  t = track_begin(&s);
  ev(&s, 0, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song_ports(&song), 1);
  CHECK_INT(song.ev[0].port, 0);
  free(song.ev);
}

TEST(a_port_meta_event_routes_its_whole_track)
{
  struct smf s; struct song song; size_t t, i;
  unsigned char port_b = 1;
  smf_header(&s, 1, 2, 480);
  t = track_begin(&s);
  ev(&s, 0, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);
  t = track_begin(&s);
  ev(&s, 0, 3, 0x90, 64, 100);             /* before the port is named */
  meta(&s, 0, 0x21, &port_b, 1);
  ev(&s, 10, 3, 0x80, 64, 0);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song_ports(&song), 2);
  for (i = 0; i < song.count; ++i)
    CHECK_INT(song.ev[i].port, song.ev[i].data1 == 64 ? 1 : 0);
  free(song.ev);
}

TEST(rolands_partb_track_names_route_to_port_b)
{
  /* Roland's 32-part demo files say the port only in the track name. */
  static const char *const names[] = { "PartB 3ch.", "partb 10ch.", "Part B?" };
  int k;
  for (k = 0; k < 2; ++k) {
    struct smf s; struct song song; size_t t;
    smf_header(&s, 0, 1, 480);
    t = track_begin(&s);
    ev(&s, 0, 3, 0x92, 60, 100);
    meta(&s, 0, 0x03, names[k], (unsigned)strlen(names[k]));
    end_of_track(&s); track_end(&s, t);
    CHECK(parse_ok(&song, &s));
    CHECK_INT(song.ev[0].port, 1);
    free(song.ev);
  }
  {
    /* A name that is not "PartB" leaves the track on port A. */
    struct smf s; struct song song; size_t t;
    smf_header(&s, 0, 1, 480);
    t = track_begin(&s);
    ev(&s, 0, 3, 0x92, 60, 100);
    meta(&s, 0, 0x03, names[2], (unsigned)strlen(names[2]));
    end_of_track(&s); track_end(&s, t);
    CHECK(parse_ok(&song, &s));
    CHECK_INT(song.ev[0].port, 0);
    free(song.ev);
  }
}

TEST(an_explicit_port_overrides_the_track_name)
{
  int order;
  for (order = 0; order < 2; ++order) {
    struct smf s; struct song song; size_t t;
    unsigned char port_a = 0;
    smf_header(&s, 0, 1, 480);
    t = track_begin(&s);
    if (order == 0) meta(&s, 0, 0x03, "PartB 1ch.", 10);
    meta(&s, 0, 0x21, &port_a, 1);
    if (order == 1) meta(&s, 0, 0x03, "PartB 1ch.", 10);
    ev(&s, 0, 3, 0x90, 60, 100);
    end_of_track(&s); track_end(&s, t);
    CHECK(parse_ok(&song, &s));
    CHECK_INT(song.ev[0].port, 0);
    free(song.ev);
  }
}

/* ---- SysEx ------------------------------------------------------------- */

static const unsigned char gs_reset_body[10] =
  { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };

TEST(a_sysex_reaches_the_engine_with_its_leading_f0)
{
  /* An SMF stores F0 events without the F0; the engine ignores a SysEx that
     lacks it. */
  struct smf s; struct song song; size_t t;
  unsigned char out[64];
  smf_header(&s, 0, 1, 480);
  t = track_begin(&s);
  put_vlq(&s, 0); s.b[s.n++] = 0xf0; put_vlq(&s, 10); put(&s, gs_reset_body, 10);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.count, 1);
  CHECK_INT(sysex_message(&song.ev[0], out, sizeof out), 11);
  CHECK_INT(out[0], 0xf0);
  CHECK(!memcmp(out + 1, gs_reset_body, 10));
  free(song.ev);
}

TEST(a_continuation_packet_is_sent_as_written)
{
  struct smf s; struct song song; size_t t;
  unsigned char out[16];
  static const unsigned char rest[3] = { 0x01, 0x02, 0xf7 };
  smf_header(&s, 0, 1, 480);
  t = track_begin(&s);
  put_vlq(&s, 0); s.b[s.n++] = 0xf7; put_vlq(&s, 3); put(&s, rest, 3);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(sysex_message(&song.ev[0], out, sizeof out), 3);
  CHECK(!memcmp(out, rest, 3));
  free(song.ev);
}

TEST(a_long_sysex_survives_intact)
{
  struct smf s; struct song song; size_t t; int i;
  unsigned char body[300], out[400];
  for (i = 0; i < 299; ++i) body[i] = (unsigned char)(i & 0x7f);
  body[299] = 0xf7;
  smf_header(&s, 0, 1, 480);
  t = track_begin(&s);
  put_vlq(&s, 0); s.b[s.n++] = 0xf0; put_vlq(&s, 300); put(&s, body, 300);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.sysex_max, 300);
  CHECK_INT(sysex_message(&song.ev[0], out, sizeof out), 301);
  CHECK(!memcmp(out + 1, body, 300));
  /* A buffer that cannot hold it is refused rather than overrun. */
  CHECK_INT(sysex_message(&song.ev[0], out, 300), 0);
  free(song.ev);
}

TEST(an_empty_sysex_is_dropped)
{
  struct smf s; struct song song; size_t t;
  smf_header(&s, 0, 1, 480);
  t = track_begin(&s);
  ev(&s, 0, 2, 0xf0, 0x00);
  ev(&s, 0, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.count, 1);
  CHECK_INT(song.ev[0].status, 0x90);
  free(song.ev);
}

/* ---- files from the wild ------------------------------------------------ */

TEST(every_track_plays_even_when_the_header_miscounts_them)
{
  struct smf s; struct song song; size_t t;
  smf_header(&s, 1, 1, 480);                 /* says one track, has two */
  t = track_begin(&s);
  ev(&s, 0, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);
  t = track_begin(&s);
  ev(&s, 0, 3, 0x91, 64, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.count, 2);
  free(song.ev);
}

TEST(chunks_that_are_not_tracks_are_stepped_over)
{
  struct smf s; struct song song; size_t t;
  smf_header(&s, 1, 2, 480);
  put(&s, "XFIH", 4); put_be(&s, 5, 4); put(&s, "\x01\x02\x03\x04\x05", 5);
  t = track_begin(&s);
  ev(&s, 0, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.count, 1);
  free(song.ev);
}

TEST(a_truncated_file_still_plays_what_it_has)
{
  struct smf s; struct song song; size_t t;
  smf_header(&s, 0, 1, 480);
  t = track_begin(&s);
  ev(&s, 0, 3, 0x90, 60, 100);
  ev(&s, 10, 3, 0x90, 62, 100);
  ev(&s, 10, 1, 0x90);                       /* cut off mid-event */
  track_end(&s, t);
  s.b[t + 7] = (unsigned char)(s.b[t + 7] + 50);   /* claims more than exists */

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.count, 2);
  free(song.ev);
}

TEST(a_message_with_a_corrupt_data_byte_is_dropped_and_the_rest_plays)
{
  /* The engine follows a controller number above 127 into a null pointer. */
  struct smf s; struct song song; size_t t;
  smf_header(&s, 0, 1, 480);
  t = track_begin(&s);
  ev(&s, 0, 3, 0xb0, 0x85, 0x10);
  ev(&s, 0, 3, 0x90, 60, 100);
  end_of_track(&s); track_end(&s, t);

  CHECK(parse_ok(&song, &s));
  CHECK_INT(song.count, 1);
  CHECK_INT(song.ev[0].status, 0x90);
  free(song.ev);
}

TEST(files_that_are_not_playable_midi_are_refused)
{
  struct smf s; struct song song; size_t t;
  static const unsigned char riff[] = "RIFF\x24\x00\x00\x00WAVEfmt ";
  CHECK(!parse(&song, riff, sizeof riff - 1));

  smf_header(&s, 0, 0, 480);                 /* a header and nothing else */
  CHECK(!parse_ok(&song, &s));
  free(song.ev);

  smf_header(&s, 3, 1, 480);                 /* no such format */
  t = track_begin(&s); ev(&s, 0, 3, 0x90, 60, 100); end_of_track(&s); track_end(&s, t);
  CHECK(!parse_ok(&song, &s));
  free(song.ev);

  smf_header(&s, 0, 1, 0);                   /* no time base */
  t = track_begin(&s); ev(&s, 0, 3, 0x90, 60, 100); end_of_track(&s); track_end(&s, t);
  CHECK(!parse_ok(&song, &s));
  free(song.ev);
}

/* ---- WAV output ------------------------------------------------------- */

static unsigned le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }

static void check_wav(int bits, unsigned rate, uint32_t frames)
{
  unsigned char h[44];
  FILE *f = tmpfile();
  unsigned bytes = bits == 16 ? 2 : 4;
  header(f, rate, frames, bits);
  CHECK_INT(ftell(f), 44);
  rewind(f);
  CHECK_INT(fread(h, 1, 44, f), 44);
  fclose(f);
  CHECK(!memcmp(h, "RIFF", 4));
  CHECK_INT(le32(h + 4), 36 + frames * 2 * bytes);
  CHECK(!memcmp(h + 8, "WAVEfmt ", 8));
  CHECK_INT(le32(h + 16), 16);
  CHECK_INT(le16(h + 20), bits == 16 ? 1 : 3);      /* PCM, or IEEE float */
  CHECK_INT(le16(h + 22), 2);
  CHECK_INT(le32(h + 24), rate);
  CHECK_INT(le32(h + 28), rate * 2 * bytes);
  CHECK_INT(le16(h + 32), 2 * bytes);
  CHECK_INT(le16(h + 34), bits);
  CHECK(!memcmp(h + 36, "data", 4));
  CHECK_INT(le32(h + 40), frames * 2 * bytes);
}

TEST(a_16_bit_wav_is_stereo_integer_pcm)   { check_wav(16, 44100, 1000); }
TEST(a_32_bit_wav_is_stereo_float)         { check_wav(32, 48000, 12345); }

TEST(sixteen_bit_samples_scale_by_32768_and_clip)
{
  CHECK_INT(to_s16(0.0f), 0);
  CHECK_INT(to_s16(0.5f), 16384);
  CHECK_INT(to_s16(-0.5f), -16384);
  CHECK_INT(to_s16(-1.0f), -32768);
  CHECK_INT(to_s16(1.0f), 32767);
  CHECK_INT(to_s16(3.0f), 32767);
  CHECK_INT(to_s16(-3.0f), -32768);
}

int main(void)
{
  RUN(events_from_every_track_play_in_time_order);
  RUN(simultaneous_events_keep_the_order_the_file_gives_them);
  RUN(a_chord_of_many_simultaneous_events_keeps_file_order);
  RUN(running_status_repeats_the_previous_status);
  RUN(a_tempo_change_sets_how_long_a_tick_lasts);
  RUN(smpte_timing_counts_real_time_and_ignores_tempo);
  RUN(smpte_29_means_drop_frame_29_97);
  RUN(a_song_on_one_port_needs_one_engine);
  RUN(a_port_meta_event_routes_its_whole_track);
  RUN(rolands_partb_track_names_route_to_port_b);
  RUN(an_explicit_port_overrides_the_track_name);
  RUN(a_sysex_reaches_the_engine_with_its_leading_f0);
  RUN(a_continuation_packet_is_sent_as_written);
  RUN(a_long_sysex_survives_intact);
  RUN(an_empty_sysex_is_dropped);
  RUN(every_track_plays_even_when_the_header_miscounts_them);
  RUN(chunks_that_are_not_tracks_are_stepped_over);
  RUN(a_truncated_file_still_plays_what_it_has);
  RUN(a_message_with_a_corrupt_data_byte_is_dropped_and_the_rest_plays);
  RUN(files_that_are_not_playable_midi_are_refused);
  RUN(a_16_bit_wav_is_stereo_integer_pcm);
  RUN(a_32_bit_wav_is_stereo_float);
  RUN(sixteen_bit_samples_scale_by_32768_and_clip);
  return test_summary();
}
