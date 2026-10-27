/* SPDX-License-Identifier: CC0-1.0
 *
 * What the plugins offer per tone map, where a selection lands when the map
 * changes, and how patches are labelled.
 */
#include "check.h"
#include "scva_lists.h"
#include "scva_map.h"

static struct scva_patch store[32];
static struct scva_names N;

static void reset_names(void)
{
  memset(store, 0, sizeof store);
  memset(&N, 0, sizeof N);
  N.patch = store;
}

static void add(int map, int bank, int prog, int drum, const char *name)
{
  struct scva_patch *p = &store[N.npatch++];
  p->map = (unsigned char)map;
  p->bank = (unsigned char)bank;
  p->prog = (unsigned char)prog;
  p->drum = (unsigned char)drum;
  snprintf(p->name, sizeof p->name, "%s", name);
}

/* A miniature of the SCVA name files: every map has its own page. */
static void per_map_names(void)
{
  reset_names();
  add(0, 0, 0, 0, "Default Page Piano");
  add(1, 0, 0, 0, "55 Piano");
  add(1, 0, 1, 0, "55 Bright");
  add(2, 0, 0, 0, "88 Piano");
  add(2, 8, 0, 0, "88 Piano w");
  add(2, 0, 0, 1, "88 Standard");
  add(4, 0, 0, 0, "8820 Piano");
  add(4, 16, 1, 0, "8820 Bright v");
  add(4, 0, 0, 1, "8820 Standard");
  add(4, 0, 8, 1, "8820 Room");
}

static const char *name_at(const uint32_t *list, uint32_t n, uint32_t i)
{
  return i < n ? N.patch[list[i]].name : "(past the end)";
}

TEST(each_map_offers_only_its_own_tones_and_kits)
{
  struct scva_lists L;
  per_map_names();
  CHECK(scva_lists_build(&L, &N));
  CHECK_INT(L.ntone[SCVA_MAP_88], 2);
  CHECK_STR(name_at(L.tone[2], L.ntone[2], 0), "88 Piano");
  CHECK_STR(name_at(L.tone[2], L.ntone[2], 1), "88 Piano w");
  CHECK_INT(L.nkit[SCVA_MAP_88], 1);
  CHECK_STR(name_at(L.kit[2], L.nkit[2], 0), "88 Standard");
  CHECK_INT(L.ntone[SCVA_MAP_55], 2);
  CHECK_INT(L.nkit[SCVA_MAP_55], 0);
  scva_lists_free(&L);
}

TEST(the_default_map_offers_what_it_plays_the_sc8820_set)
{
  struct scva_lists L;
  uint32_t i;
  per_map_names();
  CHECK(scva_lists_build(&L, &N));
  CHECK_INT(L.ntone[SCVA_MAP_DEFAULT], L.ntone[SCVA_MAP_8820]);
  CHECK_INT(L.nkit[SCVA_MAP_DEFAULT], L.nkit[SCVA_MAP_8820]);
  for (i = 0; i < L.ntone[0] && i < L.ntone[4]; ++i)
    CHECK_STR(name_at(L.tone[0], L.ntone[0], i), name_at(L.tone[4], L.ntone[4], i));
  scva_lists_free(&L);
}

TEST(without_per_map_names_every_map_offers_the_same_set)
{
  struct scva_lists L;
  int m;
  reset_names();
  add(0, 0, 0, 0, "Acoustic Grand Piano");
  add(0, 0, 1, 0, "Bright Acoustic Piano");
  add(0, 0, 0, 1, "Standard Kit");
  CHECK(scva_lists_build(&L, &N));
  for (m = 0; m < SCVA_MAPS; ++m) {
    CHECK_INT(L.ntone[m], 2);
    CHECK_INT(L.nkit[m], 1);
  }
  scva_lists_free(&L);
}

TEST(the_longest_list_sets_the_parameter_range)
{
  struct scva_lists L;
  per_map_names();
  CHECK(scva_lists_build(&L, &N));
  CHECK_INT(L.ntone_max, 2);
  CHECK_INT(L.nkit_max, 2);
  scva_lists_free(&L);
}

TEST(a_patch_is_found_by_bank_and_program)
{
  struct scva_lists L;
  per_map_names();
  scva_lists_build(&L, &N);
  CHECK_INT(scva_lists_find(&N, L.tone[2], L.ntone[2], 8, 0), 1);
  CHECK_INT(scva_lists_find(&N, L.tone[2], L.ntone[2], 0, 0), 0);
  CHECK_INT(scva_lists_find(&N, L.tone[2], L.ntone[2], 8, 5), -1);
  scva_lists_free(&L);
}

TEST(a_map_change_keeps_the_same_bank_and_program_when_it_exists)
{
  struct scva_lists L;
  struct scva_patch p = { 4, 0, 0, 0, "8820 Piano" };
  per_map_names();
  scva_lists_build(&L, &N);
  CHECK_STR(name_at(L.tone[2], L.ntone[2],
                    (uint32_t)scva_lists_carry(&N, &p, L.tone[2], L.ntone[2])),
            "88 Piano");
  scva_lists_free(&L);
}

TEST(a_map_change_falls_back_to_the_programs_capital_tone)
{
  struct scva_lists L;
  struct scva_patch variation = { 4, 16, 1, 0, "8820 Bright v" };
  per_map_names();
  scva_lists_build(&L, &N);
  /* SC-55 has no bank 16; program 2's capital tone is the nearest match. */
  CHECK_STR(name_at(L.tone[1], L.ntone[1],
                    (uint32_t)scva_lists_carry(&N, &variation, L.tone[1], L.ntone[1])),
            "55 Bright");
  scva_lists_free(&L);
}

TEST(a_map_change_with_no_matching_program_lands_on_the_first_entry)
{
  struct scva_lists L;
  struct scva_patch p = { 1, 0, 1, 0, "55 Bright" };
  per_map_names();
  scva_lists_build(&L, &N);
  CHECK_INT(scva_lists_carry(&N, &p, L.tone[2], L.ntone[2]), 0);
  scva_lists_free(&L);
}

TEST(labels_show_the_program_from_one_and_the_variation_bank)
{
  char out[64];
  struct scva_patch capital = { 2, 0, 0, 0, "Piano 1" };
  struct scva_patch variation = { 2, 8, 0, 0, "Piano 1w" };
  struct scva_patch last = { 2, 0, 127, 0, "Gun Shot" };
  scva_patch_label(&capital, out, sizeof out);   CHECK_STR(out, "001 Piano 1");
  scva_patch_label(&variation, out, sizeof out); CHECK_STR(out, "001-008 Piano 1w");
  scva_patch_label(&last, out, sizeof out);      CHECK_STR(out, "128 Gun Shot");
}

TEST(labels_across_maps_name_the_map)
{
  char out[64];
  struct scva_patch p88 = { 2, 0, 0, 0, "Piano 1" };
  struct scva_patch pro = { 3, 8, 0, 0, "Piano 1w" };
  struct scva_patch def = { 0, 0, 0, 0, "Piano 1" };
  scva_patch_label_map(&p88, out, sizeof out); CHECK_STR(out, "SC-88 001 Piano 1");
  scva_patch_label_map(&pro, out, sizeof out); CHECK_STR(out, "SC-88Pro 001-008 Piano 1w");
  scva_patch_label_map(&def, out, sizeof out); CHECK_STR(out, "001 Piano 1");
}

int main(void)
{
  RUN(each_map_offers_only_its_own_tones_and_kits);
  RUN(the_default_map_offers_what_it_plays_the_sc8820_set);
  RUN(without_per_map_names_every_map_offers_the_same_set);
  RUN(the_longest_list_sets_the_parameter_range);
  RUN(a_patch_is_found_by_bank_and_program);
  RUN(a_map_change_keeps_the_same_bank_and_program_when_it_exists);
  RUN(a_map_change_falls_back_to_the_programs_capital_tone);
  RUN(a_map_change_with_no_matching_program_lands_on_the_first_entry);
  RUN(labels_show_the_program_from_one_and_the_variation_bank);
  RUN(labels_across_maps_name_the_map);
  return test_summary();
}
