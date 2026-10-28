/* SPDX-License-Identifier: CC0-1.0
 *
 * The patch names a plugin offers for its engine: SOUND Canvas VA's from the
 * name files beside its core (scva_names.h), an engine that names its own
 * sounds from the engine itself, once for each tone set it has.
 */
#ifndef PLUGIN_NAMES_H
#define PLUGIN_NAMES_H
#include "scva_names.h"
#include "synth.h"

/* Fills N for backend b, whose files are in dir. 0, with N empty, if there
   are no names to offer. */
int plugin_names_load(const struct synth_backend *b, struct scva_names *N,
                      const char *dir);
#endif
