/* SPDX-License-Identifier: CC0-1.0
 *
 * Roland Virtual Sound Canvas 3.2's engine as a synth backend (synth.h).
 * synth_options.core is vsc.sys; the rate must be one VSC has.
 */
#ifndef SYNTH_VSC_H
#define SYNTH_VSC_H
#include "synth.h"
#include "vsc_engine.h"

/* synth_options.backend_opts for synth_vsc; synth_vsc.opts_init sets VSC's
   own defaults. */
struct synth_vsc_options {
  const char *tones;          /* the tone set: a path, or a file name beside
                                 vsc.sys (vsc55.dat, vsc88.dat, vsc88pro.dat) */
  struct vsc_config config;   /* its rate is synth_options.rate */
};
#endif
