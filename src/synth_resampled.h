/* SPDX-License-Identifier: CC0-1.0
 *
 * An engine at a rate it does not render at: one that renders only at rates
 * of its own (synth_caps.rates), wrapped so that it renders at another,
 * through resample.h. MIDI and names go straight through; the audio is
 * resampled. Front ends see an ordinary synth at the rate they asked for.
 */
#ifndef SYNTH_RESAMPLED_H
#define SYNTH_RESAMPLED_H
#include "synth.h"

/* Wraps inner, which it then owns and closes, to render at rate. NULL, with
   the reason in err, if out of memory; inner is closed then too. */
struct synth *synth_resampled(struct synth *inner, double rate, char *err,
                              size_t errlen);

/* Opens b's engine at opt->rate: as it is where b renders at that rate,
   otherwise at the nearest rate b has, resampled. */
struct synth *synth_open_at(const struct synth_backend *b,
                            const struct synth_options *opt, char *err,
                            size_t errlen);
#endif
