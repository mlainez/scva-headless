/* SPDX-License-Identifier: CC0-1.0
 *
 * Reading instrument names: SOUND Canvas VA's name files, this project's own
 * table, which source wins, and the General MIDI fallback.
 *
 * SCVA_TEST_DLL_DIR, when set, names a directory holding the user's own
 * SCVSC.tnf/.drf/.drk; the counts they must give are checked against it.
 */
#include "check.h"
#include "scva_names.h"

static const char tnf[] =
  "SSW TONEFILE Ver 2.0\r\n"
  "MODULENAME=SC-8820\r\n"
  "BANKCONTROLCC#=0\r\n"
  "; a comment line\r\n"
  "PAGECOUNT=2\r\n"
  "PAGE=1\t55Map\r\n"
  "TONECOUNT=4\r\n"
  "\t0\t8\r\n"
  "1\tPiano 1\tPiano 1w\r\n"
  "2\t---\tPiano 2w\r\n"
  "3\tE=MC2 Lead\t\r\n"
  "4\tA name far longer than thirty-one characters\r\n"
  "PAGE=2\t88Map\r\n"
  "\t0\r\n"
  "1\tPiano 1 (88)\r\n";

static const char drf[] =
  "PAGE=1\t55Map\r\n"
  "\t0\r\n"
  "1\tStandard\r\n"
  "9\tRoom\r\n";

static const char drk[] =
  "PAGE=1\t55Map\r\n"
  "\t0\t8\r\n"
  "36\tKick\tRoom Kick\r\n"
  "38\tSnare\t---\r\n";

static const struct scva_patch *patch(const struct scva_names *N, int map,
                                      int bank, int prog, int drum)
{
  size_t i;
  for (i = 0; i < N->npatch; ++i)
    if (N->patch[i].map == map && N->patch[i].bank == bank &&
        N->patch[i].prog == prog && N->patch[i].drum == drum)
      return &N->patch[i];
  return NULL;
}

static const char *key(const struct scva_names *N, int map, int prog, int note)
{
  size_t i;
  for (i = 0; i < N->nkey; ++i)
    if (N->key[i].map == map && N->key[i].prog == prog && N->key[i].key == note)
      return N->key[i].name;
  return NULL;
}

static const char *scvsc_dir(void)
{
  const char *d = test_tmpdir();
  test_write(d, "SCVSC.tnf", tnf);
  test_write(d, "SCVSC.drf", drf);
  test_write(d, "SCVSC.drk", drk);
  return d;
}

TEST(tones_are_read_per_map_bank_and_program)
{
  struct scva_names N;
  unsetenv("SCVA_TONES");
  CHECK_INT(scva_names_load(&N, scvsc_dir()), 8);     /* 6 tones, 2 kits */
  CHECK_STR(N.module, "SC-8820");
  CHECK_STR(scva_names_find(&N, 1, 0, 0, 0), "Piano 1");
  CHECK_STR(scva_names_find(&N, 1, 8, 0, 0), "Piano 1w");
  CHECK_STR(scva_names_find(&N, 1, 8, 1, 0), "Piano 2w");
  CHECK_STR(scva_names_find(&N, 2, 0, 0, 0), "Piano 1 (88)");
  scva_names_free(&N);
}

TEST(placeholders_and_empty_cells_are_not_patches)
{
  struct scva_names N;
  unsetenv("SCVA_TONES");
  scva_names_load(&N, scvsc_dir());
  CHECK(scva_names_find(&N, 1, 0, 1, 0) == NULL);      /* "---" */
  CHECK(scva_names_find(&N, 1, 8, 2, 0) == NULL);      /* empty cell */
  CHECK(key(&N, 1, 8, 38) == NULL);                    /* "---" key */
  scva_names_free(&N);
}

TEST(a_name_may_contain_an_equals_sign)
{
  struct scva_names N;
  unsetenv("SCVA_TONES");
  scva_names_load(&N, scvsc_dir());
  CHECK_STR(scva_names_find(&N, 1, 0, 2, 0), "E=MC2 Lead");
  scva_names_free(&N);
}

TEST(an_overlong_name_is_shortened_not_dropped)
{
  struct scva_names N;
  const struct scva_patch *p;
  unsetenv("SCVA_TONES");
  scva_names_load(&N, scvsc_dir());
  p = patch(&N, 1, 0, 3, 0);
  CHECK(p != NULL);
  if (p) {
    CHECK_INT(strlen(p->name), SCVA_NAME_MAX - 1);
    CHECK(!strncmp(p->name, "A name far longer than", 22));
  }
  scva_names_free(&N);
}

TEST(drum_kits_and_their_keys_are_read)
{
  struct scva_names N;
  unsetenv("SCVA_TONES");
  scva_names_load(&N, scvsc_dir());
  CHECK(patch(&N, 1, 0, 0, 1) != NULL);
  CHECK_STR(scva_names_find(&N, 1, 0, 0, 1), "Standard");
  CHECK_STR(scva_names_find(&N, 1, 0, 8, 1), "Room");
  CHECK_INT(N.nkey, 3);
  CHECK_STR(key(&N, 1, 0, 36), "Kick");
  CHECK_STR(key(&N, 1, 8, 36), "Room Kick");
  CHECK_STR(key(&N, 1, 0, 38), "Snare");
  scva_names_free(&N);
}

TEST(a_table_beside_the_core_takes_precedence_over_the_vendor_files)
{
  struct scva_names N;
  const char *d = scvsc_dir();
  unsetenv("SCVA_TONES");
  test_write(d, "scva-tones.txt", "t 2 0 5 My Own Tone\n");
  CHECK_INT(scva_names_load(&N, d), 1);
  CHECK_STR(N.module, "custom");
  CHECK_STR(scva_names_find(&N, 2, 0, 5, 0), "My Own Tone");
  scva_names_free(&N);
}

TEST(scva_tones_names_a_table_that_wins_over_everything)
{
  struct scva_names N;
  const char *d = scvsc_dir();
  const char *t = test_tmpdir();
  char path[512];
  test_write(d, "scva-tones.txt", "t 2 0 5 Beside The Core\n");
  test_write(t, "mine.txt", "t 3 0 7 From The Environment\n");
  snprintf(path, sizeof path, "%s/mine.txt", t);
  setenv("SCVA_TONES", path, 1);
  CHECK_INT(scva_names_load(&N, d), 1);
  CHECK_STR(scva_names_find(&N, 3, 0, 7, 0), "From The Environment");
  scva_names_free(&N);

  /* A table that does not exist does not hide the other sources. */
  setenv("SCVA_TONES", "/no/such/table.txt", 1);
  CHECK_INT(scva_names_load(&N, d), 1);
  CHECK_STR(scva_names_find(&N, 2, 0, 5, 0), "Beside The Core");
  scva_names_free(&N);
  unsetenv("SCVA_TONES");
}

TEST(the_table_format_reads_tones_kits_and_keys)
{
  struct scva_names N;
  const char *d = test_tmpdir();
  unsetenv("SCVA_TONES");
  test_write(d, "scva-tones.txt",
             "# a comment\n"
             "t 2 8 0 Piano 1w\n"
             "d 2 0 0 Standard 1\n"
             "k 2 0 36 Kick Drum 1\r\n"
             "t 9 0 0 a map that does not exist\n"
             "t 1 0 200 a program that does not exist\n"
             "x 1 0 0 not a record kind\n");
  CHECK_INT(scva_names_load(&N, d), 2);
  CHECK_STR(scva_names_find(&N, 2, 8, 0, 0), "Piano 1w");
  CHECK_STR(scva_names_find(&N, 2, 0, 0, 1), "Standard 1");
  CHECK_STR(key(&N, 2, 0, 36), "Kick Drum 1");
  scva_names_free(&N);
}

TEST(the_gm_name_files_are_used_when_the_scvsc_set_is_absent)
{
  struct scva_names N;
  const char *d = test_tmpdir();
  unsetenv("SCVA_TONES");
  test_write(d, "GM.tnf", "MODULENAME=GM\nPAGE=0\n\t0\n1\tGrand Piano\n");
  CHECK_INT(scva_names_load(&N, d), 1);
  CHECK_STR(N.module, "GM");
  CHECK_STR(scva_names_find(&N, 0, 0, 0, 0), "Grand Piano");
  scva_names_free(&N);
}

TEST(with_no_files_at_all_the_general_midi_set_is_offered)
{
  struct scva_names N;
  size_t i, tones = 0, kits = 0;
  unsetenv("SCVA_TONES");
  CHECK_INT(scva_names_load(&N, test_tmpdir()), 129);
  CHECK_STR(N.module, "General MIDI");
  for (i = 0; i < N.npatch; ++i) {
    if (N.patch[i].drum) ++kits; else ++tones;
    CHECK_INT(N.patch[i].map, 0);
    CHECK_INT(N.patch[i].bank, 0);
  }
  CHECK_INT(tones, 128);
  CHECK_INT(kits, 1);
  CHECK_STR(scva_names_find(&N, 0, 0, 0, 0), "Acoustic Grand Piano");
  CHECK_STR(scva_names_find(&N, 0, 0, 127, 0), "Gunshot");
  CHECK_STR(scva_names_find(&N, 0, 0, 0, 1), "Standard Kit");
  /* The GM percussion map: notes 35 to 81. */
  CHECK_INT(N.nkey, 47);
  CHECK_STR(key(&N, 0, 0, 35), "Acoustic Bass Drum");
  CHECK_STR(key(&N, 0, 0, 81), "Open Triangle");
  scva_names_free(&N);

  CHECK_INT(scva_names_load(&N, NULL), 129);
  scva_names_free(&N);
}

TEST(the_users_sound_canvas_va_files_give_every_map_its_full_set)
{
  /* The counts published for SOUND Canvas VA's own name files. */
  static const size_t tones[SCVA_MAPS] = { 1262, 418, 418, 774, 1262 };
  static const size_t kits[SCVA_MAPS]  = { 37, 10, 14, 25, 37 };
  static const size_t keys[SCVA_MAPS]  = { 3527, 602, 902, 2580, 3527 };
  size_t nt[SCVA_MAPS] = {0}, nk[SCVA_MAPS] = {0}, nkey[SCVA_MAPS] = {0}, i;
  const char *dir = getenv("SCVA_TEST_DLL_DIR");
  char probe[512];
  struct scva_names N;
  int m;
  FILE *f;

  if (!dir || !*dir) SKIP("SCVA_TEST_DLL_DIR is not set");
  snprintf(probe, sizeof probe, "%s/SCVSC.tnf", dir);
  if (!(f = fopen(probe, "rb"))) SKIP("no SCVSC.tnf in SCVA_TEST_DLL_DIR");
  fclose(f);

  unsetenv("SCVA_TONES");
  scva_names_load(&N, dir);
  /* Names from the vendor files, not a fallback: the LV2 plugin refuses to
     start on the GM set, so this distinction is user-visible. */
  CHECK(strcmp(N.module, "General MIDI") != 0);
  CHECK(strcmp(N.module, "custom") != 0);
  for (i = 0; i < N.npatch; ++i) {
    if (N.patch[i].drum) ++nk[N.patch[i].map]; else ++nt[N.patch[i].map];
  }
  for (i = 0; i < N.nkey; ++i) ++nkey[N.key[i].map];
  for (m = 0; m < SCVA_MAPS; ++m) {
    CHECK_INT(nt[m], tones[m]);
    CHECK_INT(nk[m], kits[m]);
    CHECK_INT(nkey[m], keys[m]);
  }
  CHECK_STR(scva_names_find(&N, 0, 0, 0, 0), "Piano 1");
  scva_names_free(&N);
}

int main(void)
{
  RUN(tones_are_read_per_map_bank_and_program);
  RUN(placeholders_and_empty_cells_are_not_patches);
  RUN(a_name_may_contain_an_equals_sign);
  RUN(an_overlong_name_is_shortened_not_dropped);
  RUN(drum_kits_and_their_keys_are_read);
  RUN(a_table_beside_the_core_takes_precedence_over_the_vendor_files);
  RUN(scva_tones_names_a_table_that_wins_over_everything);
  RUN(the_table_format_reads_tones_kits_and_keys);
  RUN(the_gm_name_files_are_used_when_the_scvsc_set_is_absent);
  RUN(with_no_files_at_all_the_general_midi_set_is_offered);
  RUN(the_users_sound_canvas_va_files_give_every_map_its_full_set);
  return test_summary();
}
