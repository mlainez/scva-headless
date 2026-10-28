/* SPDX-License-Identifier: CC0-1.0
 *
 * See render_engines.h.
 */
#include "render_engines.h"
#include "midi_reset.h"
#include <string.h>

static void long_sink(void *s, const unsigned char *msg, size_t len)
{
  synth_long(s, msg, len);
}

static void process_sink(void *s, float *l, float *r, int n)
{
  synth_render(s, l, r, (uint32_t)n);
}

static int no_voice_count(struct render_engines *e) { (void)e; return -1; }

static void close_synths(struct render_engines *e)
{
  int i;
  for (i = 0; i < e->n; ++i) synth_close(e->synth[i]);
}

int render_engines_synth(struct render_engines *e, const struct synth_backend *b,
                         const struct synth_options *opt, int n, const char *reset,
                         char *err, size_t errlen)
{
  int i;
  memset(e, 0, sizeof *e);
  for (i = 0; i < n; ++i) {
    struct synth *s = b->open(opt, err, errlen);
    if (!s) { close_synths(e); return 0; }
    e->synth[e->n++] = s;
    if (!strcmp(reset, "gs") || !strcmp(reset, "gm")) {
      size_t len;
      const unsigned char *msg = !strcmp(reset, "gs") ? midi_gs_reset(&len)
                                                      : midi_gm_reset(&len);
      synth_long(s, msg, len);
    }
    map_policy_init(&e->maps[i], MAP_RENDER, 0, synth_sink, s);
    e->ports[i].maps = &e->maps[i];
    e->ports[i].long_msg = long_sink;
    e->ports[i].process = process_sink;
    e->ports[i].ctx = s;
  }
  e->rate = e->synth[0]->rate;
  e->midi_block = e->synth[0]->midi_block;
  e->voices = no_voice_count;
  e->close = close_synths;
  return 1;
}
