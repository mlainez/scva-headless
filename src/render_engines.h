/* SPDX-License-Identifier: CC0-1.0
 *
 * The engines a rendered song plays on - one, or two for a two-port song -
 * as the renderers see them: render_loop ports, the rate and MIDI block they
 * run at, and how to finish with them. Each renderer opens SOUND Canvas VA's
 * core its own way; any synth.h backend is opened here.
 */
#ifndef RENDER_ENGINES_H
#define RENDER_ENGINES_H
#include "render_loop.h"
#include "synth.h"

struct render_engines {
  int n;                            /* 1, or 2 for a two-port song */
  double rate;                      /* what they render at */
  int midi_block;                   /* as render_song takes it */
  struct map_policy maps[2];
  struct render_port ports[2];
  /* Voices sounding on the first engine, -1 if it cannot say. */
  int (*voices)(struct render_engines *e);
  void (*close)(struct render_engines *e);
  struct synth *synth[2];           /* for a synth.h backend */
};

/* Opens n engines through backend b with opt, sends each the reset
   ("gs", "gm" or "none"), and wires them as render ports on tone map 0.
   0, with the reason in err, on failure. */
int render_engines_synth(struct render_engines *e, const struct synth_backend *b,
                         const struct synth_options *opt, int n, const char *reset,
                         char *err, size_t errlen);
#endif
