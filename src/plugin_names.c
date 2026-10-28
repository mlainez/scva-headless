/* SPDX-License-Identifier: CC0-1.0
 *
 * See plugin_names.h.
 */
#include "plugin_names.h"
#include "scva_map.h"
#include "synth_registry.h"
#include "win9x.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Appends one patch, growing the array as it goes. */
static int add(struct scva_names *N, size_t *cap, int map, int bank, int prog,
               int drum, const char *name)
{
  struct scva_patch *p;
  if (N->npatch == *cap) {
    size_t nc = *cap ? *cap * 2 : 1024;
    p = realloc(N->patch, nc * sizeof *p);
    if (!p) return 0;
    N->patch = p;
    *cap = nc;
  }
  p = &N->patch[N->npatch++];
  p->map = (unsigned char)map;
  p->bank = (unsigned char)bank;
  p->prog = (unsigned char)prog;
  p->drum = (unsigned char)drum;
  snprintf(p->name, sizeof p->name, "%s", name);
  return 1;
}

/* Every tone set the engine takes a map name for, opened once each at its
   own default rate and asked for its names. */
static int from_engine(const struct synth_backend *b, struct scva_names *N,
                       const char *dir)
{
  static const char *const maps[SCVA_MAPS] = { "default", "55", "88", "88pro", "8820" };
  struct synth_options opt;
  void *opts = calloc(1, b->opts_size);
  char core[2048], err[256];
  size_t cap = 0;
  int m, bank, prog, ok = 1;

  if (!opts) return 0;
  snprintf(core, sizeof core, "%s/%s", dir, b->caps->core_file);
  for (m = SCVA_MAP_55; m < SCVA_MAPS && ok; ++m) {
    struct synth *s;
    b->opts_init(opts);
    if (!synth_apply_option(b, opts, b->caps->set_option, maps[m], err, sizeof err))
      continue;                     /* no tone set for that map */
    opt.core = core;
    opt.rate = b->caps->default_rate;
    opt.max_block = 0;
    opt.backend_opts = opts;
    s = b->open(&opt, err, sizeof err);
    if (!s) continue;
    for (prog = 0; prog < 128 && ok; ++prog)
      for (bank = 0; bank < 128 && ok; ++bank) {
        const char *n = synth_name(s, 0, bank, prog);
        if (n) ok = add(N, &cap, m, bank, prog, 0, n);
      }
    for (prog = 0; prog < 128 && ok; ++prog) {
      const char *n = synth_name(s, 1, 0, prog);
      if (n) ok = add(N, &cap, m, 0, prog, 1, n);
    }
    synth_close(s);
  }
  free(opts);
  if (!ok || !N->npatch) { scva_names_free(N); return 0; }
  snprintf(N->module, sizeof N->module, "%s", b->caps->name);
  return 1;
}

int plugin_names_load(const struct synth_backend *b, struct scva_names *N,
                      const char *dir)
{
  if (!b->name || !b->caps->set_option) return scva_names_load(N, dir);
  memset(N, 0, sizeof *N);
  return from_engine(b, N, dir);
}
