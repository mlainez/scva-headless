/* SPDX-License-Identifier: CC0-1.0
 *
 * Where is SCCore.dll? The same three answers on Linux and on Windows, in the
 * same order, so one command line works on both:
 *
 *   1. whatever --core said
 *   2. $SCVA_DLL_DIR/SCCore.dll
 *   3. dll/SCCore.dll, the layout the README asks for
 *
 * The engine is never redistributed with this project, so the path has to come
 * from the person running it one way or another.
 */
#ifndef SCVA_CORE_PATH_H
#define SCVA_CORE_PATH_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SCVA_SEP "\\"
#else
#define SCVA_SEP "/"
#endif

/* Returns a path to try. `explicit_path` is the --core argument, or NULL.
   The result points at static storage or at the argument itself. */
static const char *scva_core_path(const char *explicit_path)
{
  static char buf[1024];
  const char *dir;
  if (explicit_path && *explicit_path) return explicit_path;
  dir = getenv("SCVA_DLL_DIR");
  if (dir && *dir) {
    snprintf(buf, sizeof buf, "%s" SCVA_SEP "SCCore.dll", dir);
    return buf;
  }
  return "dll" SCVA_SEP "SCCore.dll";
}
#endif
