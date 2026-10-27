/* SPDX-License-Identifier: CC0-1.0
 *
 * See scva_engine.h. The engine itself is a synth backend (synth.h); what
 * this adds is what the plugins need on top: tone maps and patches per part.
 */
#include "scva_engine.h"
#include "scva_map.h"
#include "midi_reset.h"
#include "synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct scva_engine {
  struct synth *synth;
  int map_now;                  /* -1 until the first map is sent */
  unsigned char last_pc[16];    /* each part's, re-sent after a map change */
};

struct scva_engine *scva_engine_open(const char *dir, double rate,
                                     char *err, size_t errlen)
{
  char path[2048];
  struct synth_options opt;
  size_t len;
  const unsigned char *reset;
  struct scva_engine *e = calloc(1, sizeof *e);

  if (!e) { snprintf(err, errlen, "out of memory"); return NULL; }
  snprintf(path, sizeof path, "%s/SCCore.dll", dir);
  opt.core = path;
  opt.rate = rate;
  opt.max_block = SCVA_MAXBLOCK;
  e->synth = synth_scva.open(&opt, err, errlen);
  if (!e->synth) { free(e); return NULL; }
  reset = midi_gs_reset(&len);
  synth_long(e->synth, reset, len);
  e->map_now = -1;
  return e;
}

void scva_engine_close(struct scva_engine *e)
{
  if (!e) return;
  synth_close(e->synth);
  free(e);
}

int scva_engine_thread(void)
{
  return synth_scva.attach_thread();
}

/* CC32 alone only latches the map for the next program change, so each
   part's last program is re-sent to make the map audible now. */
int scva_engine_set_map(struct scva_engine *e, int map)
{
  int ch;
  if (map == e->map_now) return 0;
  for (ch = 0; ch < 16; ++ch) {
    synth_short(e->synth, scva_map_cc(ch, map));
    synth_short(e->synth, scva_map_pc(ch, e->last_pc[ch]));
  }
  e->map_now = map;
  return 1;
}

/* CC32 then CC0 then the program change: the bank pair only takes effect on
   the program change that follows it. */
void scva_engine_patch(struct scva_engine *e, int ch, int map, int bank,
                       int prog)
{
  synth_short(e->synth, scva_map_cc(ch, map));
  synth_short(e->synth, (unsigned int)(0xb0 | ch) | ((unsigned int)bank << 16));
  synth_short(e->synth, (unsigned int)(0xc0 | ch) | ((unsigned int)prog << 8));
  e->last_pc[ch] = (unsigned char)prog;
}

void scva_engine_midi(struct scva_engine *e, const uint8_t *d, uint32_t size)
{
  unsigned int msg;
  if (!size) return;
  if (d[0] == 0xf0) { synth_long(e->synth, d, size); return; }
  if ((d[0] & 0xf0) == 0xc0 && size > 1) {
    e->last_pc[d[0] & 0x0f] = d[1] & 0x7f;
    /* A program change latches the tone map, so CC32 goes first. */
    if (e->map_now > 0)
      synth_short(e->synth, scva_map_cc(d[0] & 0x0f, e->map_now));
  }
  msg = d[0];
  if (size > 1) msg |= (unsigned int)d[1] << 8;
  if (size > 2) msg |= (unsigned int)d[2] << 16;
  synth_short(e->synth, msg);
}

void scva_engine_render(struct scva_engine *e, float *l, float *r, uint32_t n)
{
  synth_render(e->synth, l, r, n);
}
