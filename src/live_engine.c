/* SPDX-License-Identifier: CC0-1.0
 *
 * See live_engine.h.
 */
#include "live_engine.h"
#include "midi_reset.h"
#include "synth_registry.h"
#include <stdio.h>
#include <string.h>

static void gs_reset(struct synth *s)
{
  size_t len;
  const unsigned char *msg = midi_gs_reset(&len);
  synth_long(s, msg, len);
}

/* A new tone set is a new engine, reset. The one playing stays if the new
   one will not open. */
static int set_all(void *ctx, int map, char *err, size_t errlen)
{
  static const char *const names[] = { "default", "55", "88", "88pro", "8820" };
  struct live_engine *l = ctx;
  struct synth *s;
  const char *option = l->backend->caps->set_option;
  int was = map_policy_map(&l->maps, 0);
  if (!synth_apply_option(l->backend, l->opts, option, names[map], err, errlen))
    return 0;
  s = l->backend->open(&l->opt, err, errlen);
  if (!s) {
    char ignored[64];
    synth_apply_option(l->backend, l->opts, option, names[was], ignored, sizeof ignored);
    return 0;
  }
  synth_close(l->synth);
  l->synth = s;
  gs_reset(s);
  map_policy_init(&l->maps, MAP_NONE, map, synth_sink, s);
  return 1;
}

int live_engine_open(struct live_engine *l, const struct synth_backend *b,
                     const char *core, double rate, int rate_given, int block,
                     void *opts, int map, const char *mapname,
                     int *status, char *err, size_t errlen)
{
  const struct synth_caps *caps = b->caps;
  memset(l, 0, sizeof *l);
  l->backend = b;
  l->opts = opts;
  *status = 2;
  if (!rate_given && caps->default_rate) rate = caps->default_rate;
  if (!synth_check_rate(b, rate, err, errlen)) return 0;
  /* a map at start-up names the one tone set, as one typed at the console */
  if (map && !caps->tone_map_per_part &&
      !synth_apply_option(b, opts, caps->set_option, mapname, err, errlen))
    return 0;
  l->opt.core = core;
  l->opt.rate = rate;
  l->opt.max_block = block;
  l->opt.backend_opts = opts;
  *status = 1;
  l->synth = b->open(&l->opt, err, errlen);
  if (!l->synth) return 0;
  gs_reset(l->synth);
  map_policy_init(&l->maps, caps->tone_map_per_part ? MAP_LIVE : MAP_NONE, map,
                  synth_sink, l->synth);
  *status = 0;
  return 1;
}

void live_engine_console(struct live_engine *l, struct console *c)
{
  c->maps = &l->maps;
  c->set_all = l->backend->caps->tone_map_per_part ? NULL : set_all;
  c->set_ctx = l;
}

void live_engine_close(struct live_engine *l)
{
  synth_close(l->synth);
}
