/* SPDX-License-Identifier: CC0-1.0
 *
 * What the resampler does to a signal: a tone comes out the same tone at the
 * new rate, one the new rate cannot carry is removed rather than folded
 * back, the two streams stay in step, and how the output is asked for does
 * not change it.
 */
#include "check.h"
#include "resample.h"
#include <math.h>

#define PI 3.14159265358979323846

/* A source: a sine of freq Hz at rate, or one impulse at frame at. */
struct source {
  double rate, freq;
  long at;                      /* impulse frame, -1 for the sine */
  long n;                       /* frames handed out */
};

static void pull(void *ctx, float *l, float *r, uint32_t k)
{
  struct source *s = ctx;
  uint32_t i;
  for (i = 0; i < k; ++i, ++s->n) {
    float v = s->at >= 0 ? (s->n == s->at ? 1.0f : 0.0f)
                         : (float)(0.5 * sin(2.0 * PI * s->freq * (double)s->n / s->rate));
    l[i] = v;
    r[i] = -v;
  }
}

/* Worst error, in dB below the tone, of a freq sine resampled from in to
   out, once the filter is past the silence before the start: 25 input
   frames in. */
static double sine_error_db(double in, double out, double freq)
{
  struct source s = { in, freq, -1, 0 };
  struct resampler *r = resampler_new(in, out, pull, &s);
  static float l[48000], rr[48000];
  double worst = 0.0;
  int j;
  resampler_run(r, l, rr, 48000);
  for (j = (int)(25.0 * out / in) + 1; j < 48000; ++j) {
    double want = 0.5 * sin(2.0 * PI * freq * (double)j / out);
    double e = fabs(l[j] - want);
    if (fabs(rr[j] + want) > e) e = fabs(rr[j] + want);
    if (e > worst) worst = e;
  }
  resampler_free(r);
  return 20.0 * log10(worst / 0.5);
}

TEST(a_tone_comes_out_the_same_tone_going_up)
{
  CHECK(sine_error_db(44100, 48000, 1000) < -80.0);
  CHECK(sine_error_db(22050, 48000, 5000) < -80.0);
  CHECK(sine_error_db(11025, 96000, 440) < -80.0);
}

TEST(a_tone_comes_out_the_same_tone_going_down)
{
  CHECK(sine_error_db(44100, 32000, 3000) < -80.0);
}

TEST(a_tone_above_the_new_rates_reach_is_removed_not_folded_back)
{
  struct source s = { 44100, 20000, -1, 0 };
  struct resampler *r = resampler_new(44100, 32000, pull, &s);
  static float l[32000], rr[32000];
  double peak = 0.0;
  int j;
  resampler_run(r, l, rr, 32000);
  for (j = 200; j < 32000; ++j)
    if (fabs(l[j]) > peak) peak = fabs(l[j]);
  resampler_free(r);
  CHECK(20.0 * log10(peak / 0.5) < -60.0);
}

TEST(the_two_streams_stay_in_step)
{
  /* one second of input is one second of output, to the frame */
  struct source s = { 22050, 0, 22050, 0 };
  struct resampler *r = resampler_new(22050, 48000, pull, &s);
  static float l[50000], rr[50000];
  int j, top = 0;
  resampler_run(r, l, rr, 50000);
  for (j = 1; j < 50000; ++j)
    if (l[j] > l[top]) top = j;
  resampler_free(r);
  CHECK_INT(top, 48000);
}

TEST(how_the_output_is_asked_for_does_not_change_it)
{
  static float whole_l[20000], whole_r[20000], part_l[20000], part_r[20000];
  static const uint32_t sizes[] = { 1, 7, 64, 333, 1024 };
  struct source a = { 44100, 1234, -1, 0 }, b = { 44100, 1234, -1, 0 };
  struct resampler *ra = resampler_new(44100, 48000, pull, &a);
  struct resampler *rb = resampler_new(44100, 48000, pull, &b);
  uint32_t done = 0, i = 0;
  resampler_run(ra, whole_l, whole_r, 20000);
  while (done < 20000) {
    uint32_t n = sizes[i++ % 5];
    if (n > 20000 - done) n = 20000 - done;
    resampler_run(rb, part_l + done, part_r + done, n);
    done += n;
  }
  CHECK(!memcmp(whole_l, part_l, sizeof whole_l));
  CHECK(!memcmp(whole_r, part_r, sizeof whole_r));
  resampler_free(ra);
  resampler_free(rb);
}

int main(void)
{
  RUN(a_tone_comes_out_the_same_tone_going_up);
  RUN(a_tone_comes_out_the_same_tone_going_down);
  RUN(a_tone_above_the_new_rates_reach_is_removed_not_folded_back);
  RUN(the_two_streams_stay_in_step);
  RUN(how_the_output_is_asked_for_does_not_change_it);
  return test_summary();
}
