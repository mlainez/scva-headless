/* SPDX-License-Identifier: CC0-1.0
 *
 * Roland Virtual Sound Canvas 3.2's engine as a synth backend (synth.h).
 * synth_options.core is vsc.sys; the rate must be one VSC has.
 */
#ifndef SYNTH_VSC_H
#define SYNTH_VSC_H
#include "synth.h"
#include "vsc_engine.h"

/* synth_options.backend_opts for synth_vsc. */
struct synth_vsc_options {
  const char *tones;          /* vsc55.dat, vsc88.dat or vsc88pro.dat; NULL
                                 for vsc88pro.dat beside vsc.sys */
  struct vsc_config config;   /* its rate is synth_options.rate */
};
#endif
