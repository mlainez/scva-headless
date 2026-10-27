/* SPDX-License-Identifier: CC0-1.0
 *
 * The map console the live tools share: one typed line in, the map changed
 * through the policy, one reply out. Every front end gets the same commands
 * and the same wording; where the reply goes is the front end's.
 *
 *   <map>                    every part on that map
 *   <channel 1-16> <map>     one part only
 *   (empty)                  what each part is on
 *   q, quit                  stop
 *   anything else            the help
 */
#ifndef CONSOLE_H
#define CONSOLE_H
#include "map_policy.h"

/* Writes one piece of the reply. */
typedef void (*console_print_fn)(void *ctx, const char *text);

struct console {
  struct map_policy *maps;
  const char *trim;             /* trailing characters to ignore */
  console_print_fn print;
  void *ctx;
};

/* Runs one line, which it may trim in place. 1 when it asked to stop. */
int console_line(const struct console *c, char *line);
#endif
