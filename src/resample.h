/* SPDX-License-Identifier: CC0-1.0
 *
 * Stereo sample-rate conversion for an engine that renders only at rates of
 * its own, where the host's is another: VSC in a plugin, whose host chooses
 * the rate. A windowed sinc, 48 taps wide, with its cutoff below the lower
 * of the two Nyquist frequencies, so nothing folds back when going down.
 *
 * Output frame j is the input's signal at time j * in_rate / out_rate: the
 * two streams start together and never drift apart. The input is pulled as
 * it is needed, a little ahead of the output.
 */
#ifndef RESAMPLE_H
#define RESAMPLE_H
#include <stdint.h>

/* Renders n input frames into l and r. */
typedef void (*resample_pull_fn)(void *ctx, float *l, float *r, uint32_t n);

struct resampler;

/* NULL if out of memory. */
struct resampler *resampler_new(double in_rate, double out_rate,
                                resample_pull_fn pull, void *ctx);
/* n frames at the output rate, any n. */
void resampler_run(struct resampler *r, float *l, float *r_out, uint32_t n);
void resampler_free(struct resampler *r);
#endif
