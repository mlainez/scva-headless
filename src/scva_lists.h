/* SPDX-License-Identifier: CC0-1.0
 *
 * The instruments and kits a plugin offers, per tone map, and their labels.
 * Part of libscvaengine, but needs no core, so a plugin UI links it alone.
 */
#ifndef SCVA_LISTS_H
#define SCVA_LISTS_H
#include <stddef.h>
#include <stdint.h>
#include "scva_names.h"

/* Where SCCore.dll and the patch name files are: $SCVA_DLL_DIR when it holds a
   core, otherwise fallback, the plugin's own directory. The result is in buf. */
const char *scva_core_dir(const char *fallback, char *buf, size_t cap);

/* The patches a host offers, per tone map, as indices into scva_names.patch:
   melodic tones and drum kits in the order the files give them. The Default
   map renders as SC-8820, so its lists are SC-8820's. Without per-map pages,
   as with the GM fallback, every map gets the one set there is. */
struct scva_lists {
  uint32_t *tone[SCVA_MAPS], ntone[SCVA_MAPS];
  uint32_t *kit[SCVA_MAPS], nkit[SCVA_MAPS];
  uint32_t ntone_max, nkit_max;   /* the longest of each, over the maps */
  int has_maps;                   /* the maps' lists differ */
};
/* 0 if out of memory, with every list empty. */
int scva_lists_build(struct scva_lists *L, const struct scva_names *N);
void scva_lists_free(struct scva_lists *L);
/* Position in list of the patch with this bank and program, or -1. */
int scva_lists_find(const struct scva_names *N, const uint32_t *list,
                    uint32_t n, int bank, int prog);
/* Where p goes in another map's list, when the tone map changes: its bank
   and program, else its program's capital tone, else the first entry. */
int scva_lists_carry(const struct scva_names *N, const struct scva_patch *p,
                     const uint32_t *list, uint32_t n);
/* Within a map: "001 Piano 1", "001-008 Piano 1w" - program, 1-based, then
   the variation where it is not the capital tone. */
void scva_patch_label(const struct scva_patch *p, char *out, size_t cap);
/* Across maps: the same, with the map in front - "SC-88 001 Piano 1". */
void scva_patch_label_map(const struct scva_patch *p, char *out, size_t cap);
#endif
