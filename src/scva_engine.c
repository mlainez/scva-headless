/* SPDX-License-Identifier: CC0-1.0
 *
 * See scva_engine.h. The engine itself is a synth backend (synth.h); what
 * this adds is what the plugins need on top: tone maps and patches per part,
 * or, for an engine with one tone set for every part, the set.
 */
#include "scva_engine.h"
#include "scva_map.h"
#include "map_policy.h"
#include "midi_reset.h"
#include "synth_registry.h"
#include "synth_resampled.h"
#include "win9x.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scva_engine {
  const struct synth_backend *backend;
  struct synth_options opt;
  void *opts;                   /* the backend's own options */
  char path[2048];
  struct synth *synth;
  struct map_policy maps;
};

static int per_part(const struct scva_engine *e)
{
  return e->backend->caps->tone_map_per_part;
}

static int start(struct scva_engine *e, int map, char *err, size_t errlen)
{
  size_t len;
  const unsigned char *reset;
  struct synth *s = synth_open_at(e->backend, &e->opt, err, errlen);
  if (!s) return 0;
  synth_close(e->synth);
  e->synth = s;
  reset = midi_gs_reset(&len);
  synth_long(s, reset, len);
  map_policy_init(&e->maps, per_part(e) ? MAP_PLUGIN : MAP_NONE, map, synth_sink, s);
  return 1;
}

struct scva_engine *scva_engine_open(const struct synth_backend *b, const char *dir,
                                     double rate, char *err, size_t errlen)
{
  struct scva_engine *e = calloc(1, sizeof *e);

  if (!e) { snprintf(err, errlen, "out of memory"); return NULL; }
  e->backend = b;
  if (b->opts_size) {
    e->opts = calloc(1, b->opts_size);
    if (!e->opts) { free(e); snprintf(err, errlen, "out of memory"); return NULL; }
    b->opts_init(e->opts);
  }
  snprintf(e->path, sizeof e->path, "%s/%s", dir, b->caps->core_file);
  e->opt.core = e->path;
  e->opt.rate = rate;
  e->opt.max_block = SCVA_MAXBLOCK;
  e->opt.backend_opts = e->opts;
  if (!start(e, SCVA_MAP_DEFAULT, err, errlen)) {
    free(e->opts);
    free(e);
    return NULL;
  }
  return e;
}

void scva_engine_close(struct scva_engine *e)
{
  if (!e) return;
  synth_close(e->synth);
  free(e->opts);
  free(e);
}

int scva_engine_thread(const struct scva_engine *e)
{
  return e->backend->attach_thread();
}

/* With one tone set for every part, another map is another set: the engine
   is reopened on it and reset, as VSC itself changes sets. A map it has no
   set for gets its default one; if even that will not open, the set playing
   stays. */
int scva_engine_set_map(struct scva_engine *e, int map)
{
  static const char *const names[SCVA_MAPS] = { "default", "55", "88", "88pro", "8820" };
  const char *option = e->backend->caps->set_option;
  char err[256];
  if (per_part(e)) return map_policy_set(&e->maps, -1, map);
  if (map < 0 || map >= SCVA_MAPS || map == map_policy_map(&e->maps, 0)) return 0;
  if (!synth_apply_option(e->backend, e->opts, option, names[map], err, sizeof err))
    synth_apply_option(e->backend, e->opts, option, names[0], err, sizeof err);
  if (!start(e, map, err, sizeof err)) return 0;
  return 1;
}

/* CC32 then CC0 then the program change: the bank pair only takes effect on
   the program change that follows it. An engine with one tone set has no
   map to select, and CC32 would be taken for something else. */
void scva_engine_patch(struct scva_engine *e, int ch, int map, int bank,
                       int prog)
{
  if (per_part(e)) synth_short(e->synth, scva_map_cc(ch, map));
  synth_short(e->synth, (unsigned int)(0xb0 | ch) | ((unsigned int)bank << 16));
  synth_short(e->synth, (unsigned int)(0xc0 | ch) | ((unsigned int)prog << 8));
  map_policy_program(&e->maps, ch, prog);
}

void scva_engine_midi(struct scva_engine *e, const uint8_t *d, uint32_t size)
{
  unsigned int msg;
  if (!size) return;
  if (d[0] == 0xf0) { synth_long(e->synth, d, size); return; }
  msg = d[0];
  if (size > 1) msg |= (unsigned int)d[1] << 8;
  if (size > 2) msg |= (unsigned int)d[2] << 16;
  map_policy_short(&e->maps, msg);
}

void scva_engine_render(struct scva_engine *e, float *l, float *r, uint32_t n)
{
  synth_render(e->synth, l, r, n);
}
