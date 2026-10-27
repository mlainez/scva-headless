/* SPDX-License-Identifier: CC0-1.0
 *
 * See map_policy.h.
 */
#include "map_policy.h"
#include "scva_map.h"
#include <string.h>

void map_policy_init(struct map_policy *p, enum map_mode mode, int map,
                     map_sink_fn sink, void *ctx)
{
  p->mode = mode;
  p->sink = sink;
  p->ctx = ctx;
  p->set = 0;
  memset(p->map_of, map, sizeof p->map_of);
  memset(p->last_pc, 0, sizeof p->last_pc);
}

/* Whether a program change on ch gets the part's CC32 in front of it. */
static int injects(const struct map_policy *p, int ch)
{
  switch (p->mode) {
  case MAP_LIVE:   return 1;
  case MAP_RENDER: return p->map_of[ch] != SCVA_MAP_DEFAULT;
  case MAP_PLUGIN: return p->set && p->map_of[ch] != SCVA_MAP_DEFAULT;
  }
  return 0;
}

void map_policy_short(struct map_policy *p, uint32_t msg)
{
  if ((msg & 0xf0) == 0xc0) {
    int ch = (int)(msg & 0x0f);
    p->last_pc[ch] = (unsigned char)((msg >> 8) & 0x7f);
    if (injects(p, ch)) p->sink(p->ctx, scva_map_cc(ch, p->map_of[ch]));
  }
  p->sink(p->ctx, msg);
}

static void put(struct map_policy *p, int ch, int map)
{
  p->map_of[ch] = (unsigned char)map;
  p->sink(p->ctx, scva_map_cc(ch, map));
  p->sink(p->ctx, scva_map_pc(ch, p->last_pc[ch]));
}

int map_policy_set(struct map_policy *p, int ch, int map)
{
  int c;
  if (p->mode == MAP_PLUGIN && p->set) {
    int same = 1;
    for (c = 0; c < 16; ++c)
      if ((ch < 0 || c == ch) && p->map_of[c] != map) same = 0;
    if (same) return 0;
  }
  p->set = 1;
  if (ch >= 0) { put(p, ch & 0x0f, map); return 1; }
  for (c = 0; c < 16; ++c) put(p, c, map);
  return 1;
}

void map_policy_program(struct map_policy *p, int ch, int prog)
{
  p->last_pc[ch & 0x0f] = (unsigned char)prog;
}
