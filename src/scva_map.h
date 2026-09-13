/* SPDX-License-Identifier: CC0-1.0
 *
 * Tone map = Bank Select LSB (CC32), latched by a program change.
 * 0 Default (= SC-8820), 1 SC-55, 2 SC-88, 3 SC-88Pro, 4 SC-8820; >4 clamps.
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

/* -1 if the name is not one of them. */
static inline int scva_map_value(const char *name)
{
  if (!name) return -1;
  if (!strcmp(name, "default")) return SCVA_MAP_DEFAULT;
  if (!strcmp(name, "55") || !strcmp(name, "sc55")) return SCVA_MAP_55;
  if (!strcmp(name, "88") || !strcmp(name, "sc88")) return SCVA_MAP_88;
  if (!strcmp(name, "88pro") || !strcmp(name, "sc88pro")) return SCVA_MAP_88PRO;
  if (!strcmp(name, "8820") || !strcmp(name, "sc8820")) return SCVA_MAP_8820;
  if (name[0] >= '0' && name[0] <= '9' && !name[1]) return name[0] - '0';
  return -1;
}

/* The CC32 message for one channel, packed for TG_ShortMidiIn. */
static inline unsigned int scva_map_cc(int chan, int mapval)
{
  return 0xb0u | (unsigned)(chan & 0x0f) | (0x20u << 8) |
         ((unsigned)mapval << 16);
}
#endif
