/* SPDX-License-Identifier: CC0-1.0
 *
 * Tone map = Bank Select LSB (CC32), latched by a program change.
 * 0 Default, 1 SC-55, 2 SC-88, 3 SC-88Pro, 4 SC-8820.
 *
 * 0 and 4 render identically, so Default is the SC-8820 map. Values above 4
 * do not clamp to 4 and are not the default either: 5, 6, 7 and 9 all render
 * the same as each other and as no real map, so they are rejected here rather
 * than passed to the engine.
 */
#ifndef SCVA_MAP_H
#define SCVA_MAP_H
#include <string.h>

#define SCVA_MAP_DEFAULT 0
#define SCVA_MAP_55      1
#define SCVA_MAP_88      2
#define SCVA_MAP_88PRO   3
#define SCVA_MAP_8820    4

#define SCVA_MAP_USAGE "default | 55 | 88 | 88pro | 8820"

/* -1 if the name is not one of them. Case, an "SC" in front and any dashes,
   spaces or underscores are all ignored, so the spelling on the box works:
   SC-88, sc88 and 88 are the same thing. */
static inline int scva_map_value(const char *name)
{
  char n[16];
  size_t i = 0;

  if (!name) return -1;
  for (; *name && i < sizeof n - 1; ++name) {
    char c = *name;
    if (c == '-' || c == ' ' || c == '_') continue;
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    n[i++] = c;
  }
  n[i] = '\0';
  if (n[0] == 's' && n[1] == 'c' && n[2])
    memmove(n, n + 2, strlen(n + 2) + 1);

  if (!strcmp(n, "default")) return SCVA_MAP_DEFAULT;
  if (!strcmp(n, "55"))      return SCVA_MAP_55;
  if (!strcmp(n, "88"))      return SCVA_MAP_88;
  if (!strcmp(n, "88pro"))   return SCVA_MAP_88PRO;
  if (!strcmp(n, "8820"))    return SCVA_MAP_8820;
  /* The raw CC32 value, but only where a map exists. */
  if (n[0] >= '0' && n[0] <= '4' && !n[1]) return n[0] - '0';
  return -1;
}

/* The CC32 message for one channel, packed for TG_ShortMidiIn. */
static inline unsigned int scva_map_cc(int chan, int mapval)
{
  return 0xb0u | (unsigned)(chan & 0x0f) | (0x20u << 8) |
         ((unsigned)mapval << 16);
}

/* A Program Change for one channel, packed for TG_ShortMidiIn. The engine
   only re-resolves a part's sound when a program change arrives - CC32
   alone just latches which map the *next* one will read from - so changing
   the map from the console has nothing to sound different until whatever
   is playing happens to send a fresh program change on its own, which may
   be never. Re-sending the part's own last program right after CC32 is
   what makes the new map audible immediately instead. */
static inline unsigned int scva_map_pc(int chan, int program)
{
  return 0xc0u | (unsigned)(chan & 0x0f) | ((unsigned)program << 8);
}
#endif
