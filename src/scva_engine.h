/* SPDX-License-Identifier: CC0-1.0
 *
 * The engine as the plugins drive it: what scva_clap.c and scva_lv2.c share,
 * with the patch lists of scva_lists.h. Archived once per toolchain as
 * libscvaengine.a and linked statically into each plugin.
 */
#ifndef SCVA_ENGINE_H
#define SCVA_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#include "scva_lists.h"

/* Declared to the engine; scva_engine_render() is chunked to it. */
#define SCVA_MAXBLOCK 4096

/* Kits land on the drum part, tones on the first part. */
#define SCVA_TONE_CH 0
#define SCVA_DRUM_CH 9

struct scva_engine;


/* Loads dir/SCCore.dll and starts it at rate. NULL, with err filled, if the
   core cannot be loaded or lacks an export. */
struct scva_engine *scva_engine_open(const char *dir, double rate,
                                     char *err, size_t errlen);
/* Deactivates and unloads. Never TG_terminate: it calls exit(), which would
   take the host with it. */
void scva_engine_close(struct scva_engine *e);

/* Call on a thread before it enters the engine: a plugin host's audio thread
   is rarely the one that opened it. 0 if the thread cannot be set up. */
int scva_engine_thread(void);

/* Puts every part on a tone map, audibly. 1 if the map changed. */
int scva_engine_set_map(struct scva_engine *e, int map);
/* Puts one part on one patch. */
void scva_engine_patch(struct scva_engine *e, int ch, int map, int bank,
                       int prog);
/* One MIDI message from the host, short or SysEx. */
void scva_engine_midi(struct scva_engine *e, const uint8_t *d, uint32_t size);
/* Writes n frames into l and r. */
void scva_engine_render(struct scva_engine *e, float *l, float *r, uint32_t n);

#endif
