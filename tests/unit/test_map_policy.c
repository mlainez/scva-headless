/* SPDX-License-Identifier: CC0-1.0
 *
 * What reaches the engine when MIDI and map changes go through the tone map
 * policy, in each of the three ways the tools use a map: a fixed map for a
 * rendered file, a map per part from a live console, and one map from a
 * plugin host.
 */
#include "check.h"
#include "map_policy.h"
#include "scva_map.h"

/* Everything the policy sent, in order. */
struct wire {
  uint32_t msg[128];
  int n;
};

static void record(void *ctx, uint32_t msg)
{
  struct wire *w = ctx;
  if (w->n < (int)(sizeof w->msg / sizeof w->msg[0])) w->msg[w->n++] = msg;
}

static uint32_t pc(int ch, int prog) { return 0xc0u | (uint32_t)ch | ((uint32_t)prog << 8); }
static uint32_t cc32(int ch, int v) { return 0xb0u | (uint32_t)ch | (0x20u << 8) | ((uint32_t)v << 16); }
static uint32_t note_on(int ch, int key) { return 0x90u | (uint32_t)ch | ((uint32_t)key << 8) | (100u << 16); }

static struct map_policy start(enum map_mode mode, int map, struct wire *w)
{
  struct map_policy p;
  memset(w, 0, sizeof *w);
  map_policy_init(&p, mode, map, record, w);
  return p;
}

/* Whether part ch is put on map v by a CC32 and then has prog re-sent. */
static int resends(const struct wire *w, int ch, int v, int prog)
{
  int i;
  for (i = 0; i + 1 < w->n; ++i)
    if (w->msg[i] == cc32(ch, v) && w->msg[i + 1] == pc(ch, prog)) return 1;
  return 0;
}

TEST(messages_other_than_program_changes_pass_through_untouched)
{
  enum map_mode modes[] = { MAP_RENDER, MAP_LIVE, MAP_PLUGIN };
  int m;
  for (m = 0; m < 3; ++m) {
    struct wire w;
    struct map_policy p = start(modes[m], SCVA_MAP_88, &w);
    if (modes[m] == MAP_PLUGIN) { map_policy_set(&p, -1, SCVA_MAP_88); w.n = 0; }
    map_policy_short(&p, note_on(3, 60));
    map_policy_short(&p, cc32(3, 1));     /* a file's own Bank Select LSB */
    CHECK_INT(w.n, 2);
    CHECK_INT(w.msg[0], note_on(3, 60));
    CHECK_INT(w.msg[1], cc32(3, 1));
  }
}

TEST(a_rendered_file_on_a_chosen_map_gets_it_before_each_program_change)
{
  struct wire w;
  struct map_policy p = start(MAP_RENDER, SCVA_MAP_55, &w);
  map_policy_short(&p, pc(9, 25));
  CHECK_INT(w.n, 2);
  CHECK_INT(w.msg[0], cc32(9, SCVA_MAP_55));
  CHECK_INT(w.msg[1], pc(9, 25));
}

TEST(a_rendered_file_on_the_default_map_is_left_alone)
{
  struct wire w;
  struct map_policy p = start(MAP_RENDER, SCVA_MAP_DEFAULT, &w);
  map_policy_short(&p, pc(0, 5));
  CHECK_INT(w.n, 1);
  CHECK_INT(w.msg[0], pc(0, 5));
}

TEST(live_program_changes_always_carry_the_parts_map_default_included)
{
  struct wire w;
  struct map_policy p = start(MAP_LIVE, SCVA_MAP_DEFAULT, &w);
  map_policy_short(&p, pc(2, 40));
  CHECK_INT(w.n, 2);
  CHECK_INT(w.msg[0], cc32(2, SCVA_MAP_DEFAULT));
  CHECK_INT(w.msg[1], pc(2, 40));
}

TEST(a_wire_cc32_does_not_move_a_live_part_off_its_map)
{
  struct wire w;
  struct map_policy p = start(MAP_LIVE, SCVA_MAP_88PRO, &w);
  map_policy_short(&p, cc32(4, 0));
  map_policy_short(&p, pc(4, 1));
  CHECK_INT(w.n, 3);
  CHECK_INT(w.msg[1], cc32(4, SCVA_MAP_88PRO));
  CHECK_INT(map_policy_map(&p, 4), SCVA_MAP_88PRO);
}

TEST(setting_one_live_part_re_sends_only_that_parts_last_program)
{
  struct wire w;
  struct map_policy p = start(MAP_LIVE, SCVA_MAP_DEFAULT, &w);
  map_policy_short(&p, pc(5, 48));
  map_policy_short(&p, pc(6, 12));
  w.n = 0;
  CHECK_INT(map_policy_set(&p, 5, SCVA_MAP_55), 1);
  CHECK_INT(w.n, 2);
  CHECK(resends(&w, 5, SCVA_MAP_55, 48));
  CHECK_INT(map_policy_map(&p, 5), SCVA_MAP_55);
  CHECK_INT(map_policy_map(&p, 6), SCVA_MAP_DEFAULT);
  /* and the part's later program changes carry the new map */
  w.n = 0;
  map_policy_short(&p, pc(5, 49));
  CHECK_INT(w.msg[0], cc32(5, SCVA_MAP_55));
}

TEST(setting_every_live_part_re_sends_each_parts_own_program)
{
  struct wire w;
  int ch;
  struct map_policy p = start(MAP_LIVE, SCVA_MAP_DEFAULT, &w);
  map_policy_short(&p, pc(0, 19));
  map_policy_short(&p, pc(9, 16));
  w.n = 0;
  map_policy_set(&p, -1, SCVA_MAP_88);
  CHECK_INT(w.n, 32);
  CHECK(resends(&w, 0, SCVA_MAP_88, 19));
  CHECK(resends(&w, 9, SCVA_MAP_88, 16));
  CHECK(resends(&w, 15, SCVA_MAP_88, 0));   /* GM's first until told */
  for (ch = 0; ch < 16; ++ch) CHECK_INT(map_policy_map(&p, ch), SCVA_MAP_88);
}

TEST(re_setting_a_live_map_it_already_has_still_re_sends_it)
{
  struct wire w;
  struct map_policy p = start(MAP_LIVE, SCVA_MAP_88, &w);
  CHECK_INT(map_policy_set(&p, 3, SCVA_MAP_88), 1);
  CHECK_INT(w.n, 2);
}

TEST(a_plugin_injects_nothing_until_a_map_is_chosen)
{
  struct wire w;
  struct map_policy p = start(MAP_PLUGIN, SCVA_MAP_DEFAULT, &w);
  map_policy_short(&p, pc(0, 7));
  CHECK_INT(w.n, 1);
  CHECK_INT(w.msg[0], pc(0, 7));
}

TEST(a_plugins_first_map_is_sent_even_when_it_is_default)
{
  struct wire w;
  struct map_policy p = start(MAP_PLUGIN, SCVA_MAP_DEFAULT, &w);
  CHECK_INT(map_policy_set(&p, -1, SCVA_MAP_DEFAULT), 1);
  CHECK_INT(w.n, 32);
}

TEST(a_plugin_host_repeating_its_map_sends_nothing)
{
  struct wire w;
  struct map_policy p = start(MAP_PLUGIN, SCVA_MAP_DEFAULT, &w);
  map_policy_set(&p, -1, SCVA_MAP_88PRO);
  w.n = 0;
  CHECK_INT(map_policy_set(&p, -1, SCVA_MAP_88PRO), 0);
  CHECK_INT(w.n, 0);
  CHECK_INT(map_policy_set(&p, -1, SCVA_MAP_55), 1);
  CHECK_INT(w.n, 32);
}

TEST(a_plugin_on_a_chosen_map_puts_it_before_program_changes_but_not_on_default)
{
  struct wire w;
  struct map_policy p = start(MAP_PLUGIN, SCVA_MAP_DEFAULT, &w);
  map_policy_set(&p, -1, SCVA_MAP_8820);
  w.n = 0;
  map_policy_short(&p, pc(1, 30));
  CHECK_INT(w.n, 2);
  CHECK_INT(w.msg[0], cc32(1, SCVA_MAP_8820));
  map_policy_set(&p, -1, SCVA_MAP_DEFAULT);
  w.n = 0;
  map_policy_short(&p, pc(1, 31));
  CHECK_INT(w.n, 1);
}

TEST(a_program_sent_around_the_policy_is_the_one_re_sent_on_a_map_change)
{
  struct wire w;
  struct map_policy p = start(MAP_PLUGIN, SCVA_MAP_DEFAULT, &w);
  map_policy_program(&p, 0, 81);
  map_policy_set(&p, -1, SCVA_MAP_55);
  CHECK(resends(&w, 0, SCVA_MAP_55, 81));
}

int main(void)
{
  RUN(messages_other_than_program_changes_pass_through_untouched);
  RUN(a_rendered_file_on_a_chosen_map_gets_it_before_each_program_change);
  RUN(a_rendered_file_on_the_default_map_is_left_alone);
  RUN(live_program_changes_always_carry_the_parts_map_default_included);
  RUN(a_wire_cc32_does_not_move_a_live_part_off_its_map);
  RUN(setting_one_live_part_re_sends_only_that_parts_last_program);
  RUN(setting_every_live_part_re_sends_each_parts_own_program);
  RUN(re_setting_a_live_map_it_already_has_still_re_sends_it);
  RUN(a_plugin_injects_nothing_until_a_map_is_chosen);
  RUN(a_plugins_first_map_is_sent_even_when_it_is_default);
  RUN(a_plugin_host_repeating_its_map_sends_nothing);
  RUN(a_plugin_on_a_chosen_map_puts_it_before_program_changes_but_not_on_default);
  RUN(a_program_sent_around_the_policy_is_the_one_re_sent_on_a_map_change);
  return test_summary();
}
