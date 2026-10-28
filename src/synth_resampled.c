/* SPDX-License-Identifier: CC0-1.0
 *
 * See synth_resampled.h.
 */
#include "synth_resampled.h"
#include "resample.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct resampled {
  struct synth base;
  struct synth_backend backend;     /* the inner one's, with the rate freed */
  struct synth_caps caps;
  struct synth *inner;
  struct resampler *rs;
};

static void pull(void *ctx, float *l, float *r, uint32_t n)
{
  synth_render(ctx, l, r, n);
}

static void close_resampled(struct synth *s)
{
  struct resampled *e = (struct resampled *)s;
  resampler_free(e->rs);
  synth_close(e->inner);
  free(e);
}

static void short_msg(struct synth *s, uint32_t msg)
{
  synth_short(((struct resampled *)s)->inner, msg);
}

static void long_msg(struct synth *s, const uint8_t *msg, size_t len)
{
  synth_long(((struct resampled *)s)->inner, msg, len);
}

static void render(struct synth *s, float *l, float *r, uint32_t n)
{
  resampler_run(((struct resampled *)s)->rs, l, r, n);
}

static const char *name(struct synth *s, int drums, int bank, int program)
{
  return synth_name(((struct resampled *)s)->inner, drums, bank, program);
}

struct synth *synth_resampled(struct synth *inner, double rate, char *err,
                              size_t errlen)
{
  struct resampled *e = calloc(1, sizeof *e);
  if (!e) {
    synth_close(inner);
    snprintf(err, errlen, "out of memory");
    return NULL;
  }
  e->inner = inner;
  e->rs = resampler_new(inner->rate, rate, pull, inner);
  if (!e->rs) {
    synth_close(inner);
    free(e);
    snprintf(err, errlen, "out of memory");
    return NULL;
  }
  e->caps = *inner->backend->caps;
  e->caps.rates = NULL;
  e->caps.default_rate = 0;
  e->backend = *inner->backend;
  e->backend.caps = &e->caps;
  e->backend.close = close_resampled;
  e->backend.short_msg = short_msg;
  e->backend.long_msg = long_msg;
  e->backend.render = render;
  e->backend.name = inner->backend->name ? name : NULL;
  e->base.backend = &e->backend;
  e->base.rate = rate;
  e->base.midi_block = (int)ceil(inner->midi_block * rate / inner->rate);
  return &e->base;
}

struct synth *synth_open_at(const struct synth_backend *b,
                            const struct synth_options *opt, char *err,
                            size_t errlen)
{
  const int *r = b->caps->rates;
  struct synth_options at;
  struct synth *inner;
  int above = 0, below = 0;       /* the nearest rates either side */
  if (!r) return b->open(opt, err, errlen);
  for (; *r; ++r) {
    if ((double)*r == opt->rate) return b->open(opt, err, errlen);
    if (*r > opt->rate) { if (!above || *r < above) above = *r; }
    else if (*r > below) below = *r;
  }
  /* rendered at or above the rate asked for where it can be, so the
     resampler only ever discards what that rate cannot carry anyway */
  at = *opt;
  at.rate = above ? above : below;
  inner = b->open(&at, err, errlen);
  return inner ? synth_resampled(inner, opt->rate, err, errlen) : NULL;
}
