/* SPDX-License-Identifier: CC0-1.0
 *
 * Which tone map each part plays from, and the CC32 and Program Change
 * traffic that keeps the engine on it. The engine only reads CC32 when a
 * program change arrives, so the policy sits between the MIDI source and the
 * engine: every short message goes through map_policy_short(), and the map is
 * changed with map_policy_set().
 *
 * The modes are the ways the tools use a map:
 *
 *   MAP_RENDER  a fixed map for the whole file. CC32 goes before each
 *               program change unless the map is Default, which is the
 *               engine's own.
 *   MAP_LIVE    a map per part, chosen at the console. CC32 goes before
 *               every program change, Default included, so the CC32 a file
 *               sends as ordinary Bank Select LSB cannot override it.
 *               Setting a map always re-sends it.
 *   MAP_PLUGIN  one map for every part, from a host parameter. Nothing is
 *               injected until a map other than Default is set, and setting
 *               the map it already has sends nothing, since hosts re-send
 *               their parameters constantly.
 *   MAP_NONE    an engine without tone maps, whose sounds are chosen some
 *               other way. Nothing is injected or re-sent; setting a map
 *               only records it, for a console to show.
 *
 * A wire CC32 is never treated as a map change in any mode: it passes
 * through like any other controller.
 */
#ifndef MAP_POLICY_H
#define MAP_POLICY_H
#include <stdint.h>

enum map_mode { MAP_RENDER, MAP_LIVE, MAP_PLUGIN, MAP_NONE };

/* Where the policy's messages go: a short message, packed status-first. */
typedef void (*map_sink_fn)(void *ctx, uint32_t msg);

struct map_policy {
  enum map_mode mode;
  map_sink_fn sink;
  void *ctx;
  int set;                      /* a map has been set (MAP_PLUGIN) */
  unsigned char map_of[16];
  unsigned char last_pc[16];    /* re-sent when a part's map changes */
};

/* Every part starts on map, every program at 0 (GM's first). */
void map_policy_init(struct map_policy *p, enum map_mode mode, int map,
                     map_sink_fn sink, void *ctx);

/* Sends one short message, with the CC32 the mode puts before it. */
void map_policy_short(struct map_policy *p, uint32_t msg);

/* Puts part ch (0-15), or every part if ch is -1, on map, and re-sends each
   such part's last program so the map is heard now. 1 if anything was sent. */
int map_policy_set(struct map_policy *p, int ch, int map);

/* Records a program sent around the policy, so a later map change re-sends
   the right one. */
void map_policy_program(struct map_policy *p, int ch, int prog);

static inline int map_policy_map(const struct map_policy *p, int ch)
{
  return p->map_of[ch & 0x0f];
}
#endif
