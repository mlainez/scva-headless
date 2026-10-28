/* SPDX-License-Identifier: CC0-1.0
 *
 * The map console of the live tools: what each typed line does to the parts'
 * maps, and what it answers.
 */
#include "check.h"
#include "console.h"
#include "scva_map.h"

static char reply[1024];

static void print(void *ctx, const char *text)
{
  (void)ctx;
  strncat(reply, text, sizeof reply - strlen(reply) - 1);
}

static void discard(void *ctx, uint32_t msg) { (void)ctx; (void)msg; }

static struct map_policy maps;

/* What an engine with one tone set for every part was asked for. */
static int set_asked, set_refuses;

static int set_all(void *ctx, int map, char *err, size_t errlen)
{
  (void)ctx;
  set_asked = map;
  if (set_refuses) { snprintf(err, errlen, "no set for map %d", map); return 0; }
  return 1;
}

/* Runs one line on a console that trims trim, every part on map 0 before;
   whole selects an engine with one tone set for every part. */
static int type_on(int whole, const char *trim, const char *text)
{
  struct console c;
  char line[256];
  map_policy_init(&maps, whole ? MAP_NONE : MAP_LIVE, SCVA_MAP_DEFAULT, discard, NULL);
  c.maps = &maps;
  c.set_all = whole ? set_all : NULL;
  c.set_ctx = NULL;
  set_asked = -1;
  c.trim = trim;
  c.print = print;
  c.ctx = NULL;
  reply[0] = '\0';
  snprintf(line, sizeof line, "%s", text);
  return console_line(&c, line);
}

static int type(const char *trim, const char *text) { return type_on(0, trim, text); }

static int parts_on(int map)
{
  int ch, n = 0;
  for (ch = 0; ch < 16; ++ch) n += map_policy_map(&maps, ch) == map;
  return n;
}

TEST(a_map_name_moves_every_part)
{
  CHECK_INT(type(" ", "88pro"), 0);
  CHECK_INT(parts_on(SCVA_MAP_88PRO), 16);
  CHECK_STR(reply, "all parts -> map 3\n");
}

TEST(a_channel_and_a_map_move_that_part_only)
{
  type(" ", "10 sc-55");
  CHECK_INT(map_policy_map(&maps, 9), SCVA_MAP_55);
  CHECK_INT(parts_on(SCVA_MAP_DEFAULT), 15);
  CHECK_STR(reply, "channel 10 -> map 1\n");
}

TEST(an_empty_line_lists_every_parts_map)
{
  type(" ", "");
  CHECK_STR(reply, "map: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
}

TEST(q_and_quit_stop_and_change_nothing)
{
  CHECK_INT(type(" ", "q"), 1);
  CHECK_INT(type(" ", "quit"), 1);
  CHECK_INT(parts_on(SCVA_MAP_DEFAULT), 16);
  CHECK_STR(reply, "");
}

TEST(anything_else_gets_the_help_and_changes_nothing)
{
  const char *bad[] = { "frobnicate", "17 88", "0 88", "3 sc-8850" };
  size_t i;
  for (i = 0; i < sizeof bad / sizeof *bad; ++i) {
    CHECK_INT(type(" ", bad[i]), 0);
    CHECK(strstr(reply, "type a map") != NULL);
    CHECK(strstr(reply, "<channel 1-16> <map>") != NULL);
    CHECK_INT(parts_on(SCVA_MAP_DEFAULT), 16);
  }
}

TEST(trailing_characters_the_front_end_names_are_ignored)
{
  CHECK_INT(type("\r ", "88 \r"), 0);
  CHECK_INT(parts_on(SCVA_MAP_88), 16);
  CHECK_INT(type("\r ", "quit\r\n"), 0);       /* a newline is not trimmed */
  CHECK_INT(type("\r ", "quit \r"), 1);
}

TEST(characters_the_front_end_does_not_name_are_kept)
{
  type(" ", "88\r");
  CHECK_INT(parts_on(SCVA_MAP_88), 0);
  CHECK(strstr(reply, "type a map") != NULL);
}

TEST(with_one_set_for_every_part_a_map_switches_the_engine)
{
  set_refuses = 0;
  CHECK_INT(type_on(1, " ", "88"), 0);
  CHECK_INT(set_asked, SCVA_MAP_88);
  CHECK_INT(parts_on(SCVA_MAP_88), 16);
  CHECK_STR(reply, "all parts -> map 2\n");
}

TEST(with_one_set_for_every_part_a_single_part_is_refused)
{
  set_refuses = 0;
  CHECK_INT(type_on(1, " ", "10 55"), 0);
  CHECK_INT(set_asked, -1);
  CHECK_INT(parts_on(SCVA_MAP_DEFAULT), 16);
  CHECK(strstr(reply, "every part") != NULL);
}

TEST(a_map_the_engine_has_no_set_for_is_refused_with_its_reason)
{
  set_refuses = 1;
  CHECK_INT(type_on(1, " ", "8820"), 0);
  CHECK_INT(set_asked, SCVA_MAP_8820);
  CHECK_INT(parts_on(SCVA_MAP_DEFAULT), 16);
  CHECK_STR(reply, "no set for map 4\n");
  set_refuses = 0;
}

int main(void)
{
  RUN(a_map_name_moves_every_part);
  RUN(a_channel_and_a_map_move_that_part_only);
  RUN(an_empty_line_lists_every_parts_map);
  RUN(q_and_quit_stop_and_change_nothing);
  RUN(anything_else_gets_the_help_and_changes_nothing);
  RUN(trailing_characters_the_front_end_names_are_ignored);
  RUN(characters_the_front_end_does_not_name_are_kept);
  RUN(with_one_set_for_every_part_a_map_switches_the_engine);
  RUN(with_one_set_for_every_part_a_single_part_is_refused);
  RUN(a_map_the_engine_has_no_set_for_is_refused_with_its_reason);
  return test_summary();
}
