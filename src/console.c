/* SPDX-License-Identifier: CC0-1.0
 *
 * See console.h.
 */
#include "console.h"
#include "scva_map.h"
#include <stdio.h>
#include <string.h>

/* sprintf rather than snprintf: the Windows 98 build uses msvcrt's stdio,
   which has no C99 snprintf, and mingw's drags in imports Windows 98 lacks.
   Every reply is a few small integers, well inside buf. */
int console_line(const struct console *c, char *line)
{
  int ch, want, off = 0;
  size_t n = strlen(line);
  char buf[64];

  while (n > 0 && strchr(c->trim, line[n - 1])) line[--n] = '\0';
  if (!line[0]) {
    c->print(c->ctx, "map:");
    for (ch = 0; ch < 16; ++ch) {
      sprintf(buf, " %d", map_policy_map(c->maps, ch));
      c->print(c->ctx, buf);
    }
    c->print(c->ctx, "\n");
    return 0;
  }
  if (!strcmp(line, "q") || !strcmp(line, "quit")) return 1;

  if (sscanf(line, "%d %n", &ch, &off) == 1 && off > 0 && line[off] &&
      ch >= 1 && ch <= 16 && (want = scva_map_value(line + off)) >= 0) {
    if (c->set_all) {
      c->print(c->ctx, "this engine has one tone set for every part: type a "
                       "map alone\n");
      return 0;
    }
    map_policy_set(c->maps, ch - 1, want);
    sprintf(buf, "channel %d -> map %d\n", ch, want);
    c->print(c->ctx, buf);
    return 0;
  }
  if ((want = scva_map_value(line)) >= 0) {
    if (c->set_all) {
      char err[128];
      if (!c->set_all(c->set_ctx, want, err, sizeof err)) {
        c->print(c->ctx, err);
        c->print(c->ctx, "\n");
        return 0;
      }
    }
    map_policy_set(c->maps, -1, want);
    sprintf(buf, "all parts -> map %d\n", want);
    c->print(c->ctx, buf);
    return 0;
  }
  c->print(c->ctx, "type a map (" SCVA_MAP_USAGE "), or\n"
                   "  <channel 1-16> <map>   one part only\n"
                   "  <enter>                what each part is set to\n"
                   "  q                      stop\n");
  return 0;
}
