/* SPDX-License-Identifier: CC0-1.0
 *
 * The engine as the plugins drive it, and the patch lists their hosts choose
 * from: what scva_clap.c and scva_lv2.c share. Archived once per toolchain as
 * libscvaengine.a and linked statically into each plugin.
 */
#ifndef SCVA_ENGINE_H
#define SCVA_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#include "scva_names.h"

/* Declared to the engine; scva_engine_render() is chunked to it. */
#define SCVA_MAXBLOCK 4096

/* Kits land on the drum part, tones on the first part. */
#define SCVA_TONE_CH 0
#define SCVA_DRUM_CH 9

struct scva_engine;

/* Where SCCore.dll and the patch name files are: $SCVA_DLL_DIR when it holds a
   core, otherwise fallback, the plugin's own directory. The result is in buf. */
const char *scva_core_dir(const char *fallback, char *buf, size_t cap);

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

/* The patches a host offers, as indices into scva_names.patch: melodic tones
   and drum kits, each running SC-55 to SC-8820. The Default page renders as
   SC-8820 and only duplicates it, so it is listed only when it is all there
   is, as with the GM fallback. */
struct scva_lists {
  uint32_t *tone, ntone;
  uint32_t *kit, nkit;
};
/* 0 if out of memory, with both lists empty. */
int scva_lists_build(struct scva_lists *L, const struct scva_names *N);
void scva_lists_free(struct scva_lists *L);
/* Position in list of the patch with this map, bank and program, or -1. */
int scva_lists_find(const struct scva_names *N, const uint32_t *list,
                    uint32_t n, int map, int bank, int prog);
/* "SC-88Pro Piano 1", or the bare name where there is no map. */
void scva_patch_label(const struct scva_patch *p, char *out, size_t cap);
#endif
