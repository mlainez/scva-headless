/* SPDX-License-Identifier: CC0-1.0
 *
 * See resample.h.
 */
#include "resample.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HALF   24                   /* input frames each side of a point */
#define TAPS   (2 * HALF)
#define PHASES 512                  /* positions between two input frames */
#define CHUNK  64                   /* input frames pulled at a time */
#define CAP    (TAPS + 4 * CHUNK)   /* input kept, frames */

struct resampler {
  double step;                      /* input frames per output frame */
  double pos;                       /* the next output's input time,
                                       relative to in[0] */
  float in_l[CAP], in_r[CAP];
  int have;                         /* input frames held */
  float table[PHASES + 1][TAPS];    /* the filter at each phase */
  resample_pull_fn pull;
  void *ctx;
};

static double bessel_i0(double x)
{
  double sum = 1.0, term = 1.0;
  int k;
  for (k = 1; k < 40; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
  }
  return sum;
}

/* Each phase's taps, a Kaiser-windowed sinc normalised to unity gain. The
   cutoff is a little below the lower Nyquist frequency, in input cycles per
   frame. */
static void build_table(struct resampler *r, double in_rate, double out_rate)
{
  const double beta = 9.0, pi = 3.14159265358979323846;
  double cutoff = 0.5 * 0.91 * (out_rate < in_rate ? out_rate / in_rate : 1.0);
  double norm = bessel_i0(beta);
  int p, k;
  for (p = 0; p <= PHASES; ++p) {
    double frac = (double)p / PHASES, sum = 0.0;
    for (k = 0; k < TAPS; ++k) {
      double x = (double)(k - HALF + 1) - frac;    /* from the point */
      double w = x / HALF;
      double sinc = x == 0.0 ? 1.0 : sin(2.0 * pi * cutoff * x) / (2.0 * pi * cutoff * x);
      double win = fabs(w) >= 1.0 ? 0.0 : bessel_i0(beta * sqrt(1.0 - w * w)) / norm;
      r->table[p][k] = (float)(sinc * win);
      sum += sinc * win;
    }
    for (k = 0; k < TAPS; ++k) r->table[p][k] = (float)(r->table[p][k] / sum);
  }
}

struct resampler *resampler_new(double in_rate, double out_rate,
                                resample_pull_fn pull, void *ctx)
{
  struct resampler *r = calloc(1, sizeof *r);
  if (!r) return NULL;
  r->step = in_rate / out_rate;
  r->pull = pull;
  r->ctx = ctx;
  build_table(r, in_rate, out_rate);
  /* silence before the input starts, so the first output is its frame 0 */
  r->have = HALF - 1;
  r->pos = HALF - 1;
  return r;
}

/* Makes the frames up to index last available, dropping those no point
   will need again. */
static void fill(struct resampler *r, int last)
{
  int drop = (int)r->pos - (HALF - 1);
  if (last < r->have) return;
  if (drop > 0 && r->have + CHUNK > CAP) {
    memmove(r->in_l, r->in_l + drop, (size_t)(r->have - drop) * sizeof *r->in_l);
    memmove(r->in_r, r->in_r + drop, (size_t)(r->have - drop) * sizeof *r->in_r);
    r->have -= drop;
    r->pos -= drop;
    last -= drop;
  }
  while (last >= r->have) {
    r->pull(r->ctx, r->in_l + r->have, r->in_r + r->have, CHUNK);
    r->have += CHUNK;
  }
}

void resampler_run(struct resampler *r, float *l, float *r_out, uint32_t n)
{
  uint32_t j;
  for (j = 0; j < n; ++j) {
    int i, k, p;
    double phase;
    float a, b, sl0 = 0, sr0 = 0, sl1 = 0, sr1 = 0;
    const float *t0, *t1, *xl, *xr;
    fill(r, (int)r->pos + HALF);
    i = (int)r->pos;
    phase = (r->pos - i) * PHASES;
    p = (int)phase;
    b = (float)(phase - p);
    a = 1.0f - b;
    t0 = r->table[p];
    t1 = r->table[p + 1];
    xl = r->in_l + i - HALF + 1;
    xr = r->in_r + i - HALF + 1;
    for (k = 0; k < TAPS; ++k) {
      sl0 += t0[k] * xl[k];
      sr0 += t0[k] * xr[k];
      sl1 += t1[k] * xl[k];
      sr1 += t1[k] * xr[k];
    }
    l[j] = a * sl0 + b * sl1;
    r_out[j] = a * sr0 + b * sr1;
    r->pos += r->step;
  }
}

void resampler_free(struct resampler *r)
{
  free(r);
}
