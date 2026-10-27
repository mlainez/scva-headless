/* SPDX-License-Identifier: CC0-1.0
 *
 * Tone map names as users type them (--map, the console, the CLAP text
 * field), and the MIDI messages that put a part on a map.
 */
#include "check.h"
#include "scva_map.h"

/* Decodes a packed short message the way the engine reads it. */
static void unpack(unsigned int msg, int *status, int *d1, int *d2)
{
  *status = (int)(msg & 0xff);
  *d1 = (int)((msg >> 8) & 0xff);
  *d2 = (int)((msg >> 16) & 0xff);
}

TEST(the_spellings_on_the_box_select_their_map)
{
  CHECK_INT(scva_map_value("SC-55"), SCVA_MAP_55);
  CHECK_INT(scva_map_value("SC-88"), SCVA_MAP_88);
  CHECK_INT(scva_map_value("SC-88Pro"), SCVA_MAP_88PRO);
  CHECK_INT(scva_map_value("SC-8820"), SCVA_MAP_8820);
}

TEST(case_dashes_spaces_and_the_sc_prefix_do_not_matter)
{
  CHECK_INT(scva_map_value("sc88"), SCVA_MAP_88);
  CHECK_INT(scva_map_value("88"), SCVA_MAP_88);
  CHECK_INT(scva_map_value("SC 88 Pro"), SCVA_MAP_88PRO);
  CHECK_INT(scva_map_value("sc_88_pro"), SCVA_MAP_88PRO);
  CHECK_INT(scva_map_value("88PRO"), SCVA_MAP_88PRO);
  CHECK_INT(scva_map_value("Default"), SCVA_MAP_DEFAULT);
  CHECK_INT(scva_map_value("DEFAULT"), SCVA_MAP_DEFAULT);
}

TEST(a_raw_cc32_value_is_accepted_where_a_map_exists)
{
  CHECK_INT(scva_map_value("0"), SCVA_MAP_DEFAULT);
  CHECK_INT(scva_map_value("1"), SCVA_MAP_55);
  CHECK_INT(scva_map_value("4"), SCVA_MAP_8820);
}

TEST(values_without_a_map_are_rejected)
{
  CHECK_INT(scva_map_value("5"), -1);
  CHECK_INT(scva_map_value("9"), -1);
  CHECK_INT(scva_map_value("12"), -1);
}

TEST(names_that_are_not_a_map_are_rejected)
{
  CHECK_INT(scva_map_value(NULL), -1);
  CHECK_INT(scva_map_value(""), -1);
  CHECK_INT(scva_map_value("sc"), -1);
  CHECK_INT(scva_map_value("SC-8850"), -1);
  CHECK_INT(scva_map_value("88pro2"), -1);
  CHECK_INT(scva_map_value("piano"), -1);
}

TEST(the_map_message_is_bank_select_lsb_on_the_part)
{
  int status, d1, d2;
  unpack(scva_map_cc(9, SCVA_MAP_88PRO), &status, &d1, &d2);
  CHECK_INT(status, 0xb9);
  CHECK_INT(d1, 32);
  CHECK_INT(d2, 3);
  unpack(scva_map_cc(0, SCVA_MAP_DEFAULT), &status, &d1, &d2);
  CHECK_INT(status, 0xb0);
  CHECK_INT(d1, 32);
  CHECK_INT(d2, 0);
}

TEST(the_program_message_is_a_program_change_on_the_part)
{
  int status, d1, d2;
  unpack(scva_map_pc(15, 127), &status, &d1, &d2);
  CHECK_INT(status, 0xcf);
  CHECK_INT(d1, 127);
  unpack(scva_map_pc(0, 0), &status, &d1, &d2);
  CHECK_INT(status, 0xc0);
  CHECK_INT(d1, 0);
}

TEST(the_usage_text_names_every_map)
{
  CHECK(strstr(SCVA_MAP_USAGE, "default") != NULL);
  CHECK(strstr(SCVA_MAP_USAGE, "55") != NULL);
  CHECK(strstr(SCVA_MAP_USAGE, "88pro") != NULL);
  CHECK(strstr(SCVA_MAP_USAGE, "8820") != NULL);
}

int main(void)
{
  RUN(the_spellings_on_the_box_select_their_map);
  RUN(case_dashes_spaces_and_the_sc_prefix_do_not_matter);
  RUN(a_raw_cc32_value_is_accepted_where_a_map_exists);
  RUN(values_without_a_map_are_rejected);
  RUN(names_that_are_not_a_map_are_rejected);
  RUN(the_map_message_is_bank_select_lsb_on_the_part);
  RUN(the_program_message_is_a_program_change_on_the_part);
  RUN(the_usage_text_names_every_map);
  return test_summary();
}
