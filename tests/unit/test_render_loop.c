/* SPDX-License-Identifier: CC0-1.0
 *
 * How a song is played into the engines by the renderers: when each event
 * reaches its engine relative to the audio, which engine of a two-port song
 * it reaches, how the two are mixed, and when the song ends.
 */
#include "check.h"
#include "midi_song.h"
#include "render_loop.h"

#define RATE 48000.0
#define DIVISION 480            /* 50 frames a tick at the default tempo */

/* An engine that records what it was sent and how many blocks it had
   rendered by then, and renders a constant level. */
struct engine {
  struct map_policy maps;
  int blocks;
  float level, right_level;
  uint32_t msg[64];
  int msg_block[64];
  int nmsg;
  int nlong;
  int long_block[8];
};

static void short_in(void *ctx, uint32_t msg)
{
  struct engine *e = ctx;
  if (e->nmsg < 64) { e->msg[e->nmsg] = msg; e->msg_block[e->nmsg++] = e->blocks; }
}

static void long_in(void *ctx, const unsigned char *msg, size_t len)
{
  struct engine *e = ctx;
  (void)msg; (void)len;
  if (e->nlong < 8) e->long_block[e->nlong] = e->blocks;
  ++e->nlong;
}

static void process(void *ctx, float *l, float *r, int n)
{
  struct engine *e = ctx;
  int k;
  for (k = 0; k < n; ++k) { l[k] += e->level; r[k] += e->right_level; }
  ++e->blocks;
}

static struct render_port port_of(struct engine *e)
{
  struct render_port p;
  memset(e, 0, sizeof *e);
  map_policy_init(&e->maps, MAP_RENDER, 0, short_in, e);
  p.maps = &e->maps;
  p.long_msg = long_in;
  p.process = process;
  p.ctx = e;
  return p;
}

/* What the caller saw. */
struct out {
  int blocks;
  int stop_after;               /* deliver says stop after this many, 0 never */
  uint64_t paced[64];
  int npaced;
  float first_left, first_right;
};

static void pace(void *ctx, uint64_t frame)
{
  struct out *o = ctx;
  if (o->npaced < 64) o->paced[o->npaced++] = frame;
}

static int deliver(void *ctx, const float *l, const float *r, int n, uint64_t frame)
{
  struct out *o = ctx;
  (void)n; (void)frame;
  if (!o->blocks) { o->first_left = l[0]; o->first_right = r[0]; }
  ++o->blocks;
  return !(o->stop_after && o->blocks >= o->stop_after);
}

static struct render_io io_of(struct out *o)
{
  struct render_io io;
  memset(o, 0, sizeof *o);
  io.pace = pace;
  io.deliver = deliver;
  io.ctx = o;
  return io;
}

/* A song of hand-made events, in order. */
struct test_song {
  struct song s;
  struct event ev[16];
};

static void song_init(struct test_song *t)
{
  memset(t, 0, sizeof *t);
  t->s.ev = t->ev;
  t->s.division = DIVISION;
}

static void note(struct test_song *t, uint64_t tick, int port, int ch, int key)
{
  struct event *e = &t->ev[t->s.count++];
  e->tick = tick;
  e->status = (uint8_t)(0x90 | ch);
  e->data1 = (uint8_t)key;
  e->data2 = 100;
  e->port = (uint8_t)port;
}

static void tempo(struct test_song *t, uint64_t tick, uint32_t us_per_beat)
{
  struct event *e = &t->ev[t->s.count++];
  e->tick = tick;
  e->tempo = us_per_beat;
}

static void sysex(struct test_song *t, uint64_t tick, const unsigned char *body, uint32_t len)
{
  struct event *e = &t->ev[t->s.count++];
  e->tick = tick;
  e->status = 0xf0;
  e->sysex = body;
  e->sysex_len = len;
  if (len + 1 > t->s.sysex_max) t->s.sysex_max = len + 1;
}

/* Which block (0 first) a message reached the engine before. */
static int block_of(const struct engine *e, int key)
{
  int i;
  for (i = 0; i < e->nmsg; ++i)
    if (((e->msg[i] >> 8) & 0x7f) == (uint32_t)key) return e->msg_block[i];
  return -1;
}

TEST(each_event_reaches_the_engine_before_the_block_it_sounds_in)
{
  struct test_song t; struct engine a; struct out o; struct render_result res;
  struct render_port p = port_of(&a); struct render_io io = io_of(&o);
  song_init(&t);
  note(&t, 0, 0, 0, 60);                 /* frame 0 */
  note(&t, 10, 0, 0, 62);                /* frame 500: the block at 256 */
  note(&t, 40, 0, 0, 64);                /* frame 2000: the block at 1792 */
  CHECK_INT(render_song(&t.s, RATE, 0.0, 1, &p, 1, &io, &res), 0);
  CHECK_INT(block_of(&a, 60), 0);
  CHECK_INT(block_of(&a, 62), 1);
  CHECK_INT(block_of(&a, 64), 7);
}

TEST(a_tempo_change_moves_every_later_event)
{
  struct test_song t; struct engine a; struct out o; struct render_result res;
  struct render_port p = port_of(&a); struct render_io io = io_of(&o);
  song_init(&t);
  tempo(&t, 0, 250000);                  /* twice as fast: 25 frames a tick */
  note(&t, 40, 0, 0, 64);                /* frame 1000: the block at 768 */
  render_song(&t.s, RATE, 0.0, 1, &p, 1, &io, &res);
  CHECK_INT(block_of(&a, 64), 3);
}

TEST(the_song_plays_on_for_the_tail_after_its_last_event)
{
  struct test_song t; struct engine a; struct out o; struct render_result res;
  struct render_port p = port_of(&a); struct render_io io = io_of(&o);
  song_init(&t);
  note(&t, 20, 0, 0, 60);                /* frame 1000 */
  render_song(&t.s, RATE, 0.5, 1, &p, 1, &io, &res);
  CHECK(res.frames >= 1000 + 24000);
  CHECK(res.frames < 1000 + 24000 + RENDER_BLOCK);
  CHECK_INT(res.frames % RENDER_BLOCK, 0);
  CHECK_INT(a.blocks * RENDER_BLOCK, res.frames);
  CHECK_INT(o.blocks * RENDER_BLOCK, res.frames);
}

TEST(each_block_is_paced_at_its_first_frame_before_it_is_rendered)
{
  struct test_song t; struct engine a; struct out o; struct render_result res;
  struct render_port p = port_of(&a); struct render_io io = io_of(&o);
  song_init(&t);
  note(&t, 0, 0, 0, 60);
  render_song(&t.s, RATE, 0.01, 1, &p, 1, &io, &res);
  CHECK_INT(o.npaced, o.blocks);
  CHECK_INT(o.paced[0], 0);
  CHECK_INT(o.paced[1], RENDER_BLOCK);
}

TEST(a_two_port_song_sends_port_b_to_the_second_engine_and_sysex_to_both)
{
  static const unsigned char gs[] = { 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
  struct test_song t; struct engine a, b; struct out o; struct render_result res;
  struct render_port p[2]; struct render_io io = io_of(&o);
  p[0] = port_of(&a);
  p[1] = port_of(&b);
  song_init(&t);
  sysex(&t, 0, gs, sizeof gs);
  note(&t, 0, 0, 0, 60);
  note(&t, 0, 1, 0, 72);
  render_song(&t.s, RATE, 0.0, 1, p, 2, &io, &res);
  CHECK_INT(block_of(&a, 60), 0);
  CHECK_INT(block_of(&a, 72), -1);
  CHECK_INT(block_of(&b, 72), 0);
  CHECK_INT(block_of(&b, 60), -1);
  CHECK_INT(a.nlong, 1);
  CHECK_INT(b.nlong, 1);
  CHECK_INT(res.sent, 3);
}

TEST(a_one_engine_render_plays_every_port_on_it)
{
  struct test_song t; struct engine a; struct out o; struct render_result res;
  struct render_port p = port_of(&a); struct render_io io = io_of(&o);
  song_init(&t);
  note(&t, 0, 1, 0, 72);
  render_song(&t.s, RATE, 0.0, 1, &p, 1, &io, &res);
  CHECK_INT(block_of(&a, 72), 0);
}

TEST(the_two_engines_are_mixed)
{
  struct test_song t; struct engine a, b; struct out o; struct render_result res;
  struct render_port p[2]; struct render_io io = io_of(&o);
  p[0] = port_of(&a);
  p[1] = port_of(&b);
  a.level = 0.125f; a.right_level = 0.25f;
  b.level = -0.5f;  b.right_level = 0.5f;
  song_init(&t);
  note(&t, 0, 0, 0, 60);
  render_song(&t.s, RATE, 0.0, 1, p, 2, &io, &res);
  CHECK_NEAR(o.first_left, -0.375, 1e-6);
  CHECK_NEAR(o.first_right, 0.75, 1e-6);
}

/* The peak of an engine whose loudest side is negative, quiet on the other. */
static float peak_of(float left, float right)
{
  struct test_song t; struct engine a; struct out o; struct render_result res;
  struct render_port p = port_of(&a); struct render_io io = io_of(&o);
  a.level = left;
  a.right_level = right;
  song_init(&t);
  note(&t, 0, 0, 0, 60);
  render_song(&t.s, RATE, 0.0, 1, &p, 1, &io, &res);
  return res.peak;
}

TEST(the_peak_is_the_loudest_sample_on_either_side_either_sign)
{
  CHECK_NEAR(peak_of(-0.75f, 0.125f), 0.75, 1e-6);
  CHECK_NEAR(peak_of(0.125f, -0.75f), 0.75, 1e-6);
  CHECK_NEAR(peak_of(0.5f, 0.25f), 0.5, 1e-6);
}

TEST(the_caller_can_stop_the_song_early)
{
  struct test_song t; struct engine a; struct out o; struct render_result res;
  struct render_port p = port_of(&a); struct render_io io = io_of(&o);
  song_init(&t);
  note(&t, 0, 0, 0, 60);
  o.stop_after = 2;
  render_song(&t.s, RATE, 1.0, 1, &p, 1, &io, &res);
  CHECK_INT(o.blocks, 2);
  CHECK_INT(res.frames, RENDER_BLOCK);    /* the refused block is not counted */
}

TEST(tempo_changes_are_not_counted_as_messages)
{
  struct test_song t; struct engine a; struct out o; struct render_result res;
  struct render_port p = port_of(&a); struct render_io io = io_of(&o);
  song_init(&t);
  tempo(&t, 0, 400000);
  note(&t, 0, 0, 0, 60);
  render_song(&t.s, RATE, 0.0, 1, &p, 1, &io, &res);
  CHECK_INT(res.sent, 1);
  CHECK_INT(a.nmsg, 1);
}

int main(void)
{
  RUN(each_event_reaches_the_engine_before_the_block_it_sounds_in);
  RUN(a_tempo_change_moves_every_later_event);
  RUN(the_song_plays_on_for_the_tail_after_its_last_event);
  RUN(each_block_is_paced_at_its_first_frame_before_it_is_rendered);
  RUN(a_two_port_song_sends_port_b_to_the_second_engine_and_sysex_to_both);
  RUN(a_one_engine_render_plays_every_port_on_it);
  RUN(the_two_engines_are_mixed);
  RUN(the_peak_is_the_loudest_sample_on_either_side_either_sign);
  RUN(the_caller_can_stop_the_song_early);
  RUN(tempo_changes_are_not_counted_as_messages);
  return test_summary();
}
