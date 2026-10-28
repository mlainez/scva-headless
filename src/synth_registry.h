/* SPDX-License-Identifier: CC0-1.0
 *
 * The engines a build can drive, chosen by name (--engine), and the
 * command-line options each one brings with it.
 */
#ifndef SYNTH_REGISTRY_H
#define SYNTH_REGISTRY_H
#include <stdio.h>
#include "synth.h"

/* The backends this build has, ending in NULL. The first is the default. */
extern const struct synth_backend *const synth_backends[];

/* By caps->name; NULL if this build has no such engine. */
const struct synth_backend *synth_find(const char *name);

/* Every engine's name, for a usage line: "scva|vsc". */
const char *synth_engine_names(void);

/* If argv[*i] is one of b's options, applies it with the value after it,
   leaves *i on that value and returns 1. 0 if it is not one of b's. -1, with
   the reason in err, if it is but its value is wrong or missing. */
int synth_parse_option(const struct synth_backend *b, void *opts, int argc,
                       char **argv, int *i, char *err, size_t errlen);

/* Applies b's option flag with value. 0, with the reason in err, if b has
   no such option or the value is wrong. */
int synth_apply_option(const struct synth_backend *b, void *opts, const char *flag,
                       const char *value, char *err, size_t errlen);

/* 1 if b renders at rate. 0 otherwise, with the rates it has in err:
   nothing resamples for it. */
int synth_check_rate(const struct synth_backend *b, double rate, char *err,
                     size_t errlen);

/* b's options, one per line, as a usage text shows them. */
void synth_print_options(const struct synth_backend *b, FILE *f);
#endif
