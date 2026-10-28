/* SPDX-License-Identifier: CC0-1.0
 *
 * What the VSC backend tells the code around it: the rates it renders at,
 * where its MIDI lands, and the engine's own names for the tone set loaded,
 * spot-checked against Roland's published tone lists. Needs the user's own
 * files: SCVA_TEST_VSC_DIR holds vsc.sys and the vscNN.dat tone sets.
 */
#include "check.h"
#include "synth_vsc.h"

static char core[1024], tones[1024];

static struct synth *open_at(int rate, const char *set, char *err, size_t errlen)
{
  static const struct vsc_config defaults = VSC_CONFIG_DEFAULT;
  const char *dir = getenv("SCVA_TEST_VSC_DIR");
  struct synth_vsc_options vo;
  struct synth_options opt;
  snprintf(core, sizeof core, "%s/vsc.sys", dir);
  snprintf(tones, sizeof tones, "%s/%s", dir, set);
  vo.tones = tones;
  vo.config = defaults;
  opt.core = core;
  opt.rate = rate;
  opt.max_block = 0;
  opt.backend_opts = &vo;
  return synth_vsc.open(&opt, err, errlen);
}

static const char *name_or_none(struct synth *s, int drums, int bank, int program)
{
  const char *n = synth_name(s, drums, bank, program);
  return n ? n : "(none)";
}

static int drum_sets(struct synth *s)
{
  int p, n = 0;
  for (p = 0; p < 128; ++p) n += synth_name(s, 1, 0, p) != NULL;
  return n;
}

TEST(it_renders_at_vsc_rates_only)
{
  const int *r = synth_vsc.caps->rates;
  CHECK(r != NULL);
  if (!r) return;
  CHECK_INT(r[0], 11025);
  CHECK_INT(r[1], 22050);
  CHECK_INT(r[2], 44100);
  CHECK_INT(r[3], 0);
}

TEST(another_rate_is_refused_with_the_ones_it_has)
{
  char err[256] = "";
  struct synth *s = open_at(48000, "vsc88pro.dat", err, sizeof err);
  CHECK(s == NULL);
  CHECK(strstr(err, "11025") && strstr(err, "22050") && strstr(err, "44100"));
  synth_close(s);
}

TEST(midi_lands_on_the_engine_block_of_each_rate)
{
  static const int rate[] = { 11025, 22050, 44100 }, block[] = { 125, 250, 500 };
  int i;
  CHECK(!synth_vsc.caps->midi_timestamps);
  for (i = 0; i < 3; ++i) {
    char err[256] = "";
    struct synth *s = open_at(rate[i], "vsc88pro.dat", err, sizeof err);
    CHECK(s != NULL);
    if (!s) { fprintf(stderr, "  %s\n", err); continue; }
    CHECK_INT((int)s->rate, rate[i]);
    CHECK_INT(s->midi_block, block[i]);
    synth_close(s);
  }
}

TEST(one_tone_set_serves_every_part)
{
  CHECK(!synth_vsc.caps->tone_map_per_part);
}

TEST(tones_are_named_by_bank_and_program_as_roland_lists_them)
{
  char err[256] = "";
  struct synth *s = open_at(22050, "vsc88pro.dat", err, sizeof err);
  CHECK(s != NULL);
  if (!s) { fprintf(stderr, "  %s\n", err); return; }
  CHECK_STR(name_or_none(s, 0, 0, 0), "Piano 1");
  CHECK_STR(name_or_none(s, 0, 8, 0), "Piano 1w");
  CHECK_STR(name_or_none(s, 0, 16, 0), "Piano 1d");
  CHECK_STR(name_or_none(s, 0, 0, 40), "Violin");
  CHECK_STR(name_or_none(s, 0, 8, 40), "Slow Violin");
  CHECK_STR(name_or_none(s, 0, 1, 0), "(none)");          /* no such variation */
  CHECK_STR(name_or_none(s, 0, 0, 128), "(none)");        /* no such program */
  synth_close(s);
}

TEST(each_tone_set_has_its_own_drum_sets)
{
  static const struct { const char *set, *first, *last; int count; } want[] = {
    { "vsc55.dat",    "STANDARD",  "CM-64/32L", 10 },
    { "vsc88.dat",    "STANDARD1", "CM-64/32L", 15 },
    { "vsc88pro.dat", "STANDARD1", "CM-64/32L", 26 },
  };
  size_t i;
  for (i = 0; i < sizeof want / sizeof *want; ++i) {
    char err[256] = "";
    struct synth *s = open_at(22050, want[i].set, err, sizeof err);
    CHECK(s != NULL);
    if (!s) { fprintf(stderr, "  %s\n", err); continue; }
    CHECK_STR(name_or_none(s, 1, 0, 0), want[i].first);
    CHECK_STR(name_or_none(s, 1, 0, 127), want[i].last);
    CHECK_INT(drum_sets(s), want[i].count);
    synth_close(s);
  }
}

TEST(the_names_do_not_depend_on_the_rate)
{
  char err[256] = "";
  struct synth *a = open_at(11025, "vsc88.dat", err, sizeof err);
  struct synth *b = open_at(44100, "vsc88.dat", err, sizeof err);
  int bank, prog, same = 1;
  CHECK(a && b);
  if (a && b)
    for (prog = 0; prog < 128; ++prog)
      for (bank = 0; bank < 128; ++bank)
        if (strcmp(name_or_none(a, 0, bank, prog), name_or_none(b, 0, bank, prog))) same = 0;
  CHECK(same);
  synth_close(a);
  synth_close(b);
}

TEST(sound_canvas_va_differs_on_every_count)
{
  /* the other backend in this build, for contrast */
  CHECK(synth_scva.caps->rates == NULL);
  CHECK(synth_scva.caps->midi_timestamps);
  CHECK(synth_scva.caps->tone_map_per_part);
  CHECK(synth_scva.name == NULL);
}

int main(void)
{
  if (!getenv("SCVA_TEST_VSC_DIR")) {
    printf("skip test_synth_vsc (set SCVA_TEST_VSC_DIR)\n");
    return 0;
  }
  RUN(it_renders_at_vsc_rates_only);
  RUN(another_rate_is_refused_with_the_ones_it_has);
  RUN(midi_lands_on_the_engine_block_of_each_rate);
  RUN(one_tone_set_serves_every_part);
  RUN(tones_are_named_by_bank_and_program_as_roland_lists_them);
  RUN(each_tone_set_has_its_own_drum_sets);
  RUN(the_names_do_not_depend_on_the_rate);
  RUN(sound_canvas_va_differs_on_every_count);
  return test_summary();
}
