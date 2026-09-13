/* SPDX-License-Identifier: CC0-1.0
 *
 * SCCore.dll is looked for as --core, then $SCVA_DLL_DIR/SCCore.dll, then
 * dll/SCCore.dll. Same order on Linux and Windows.
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

/* Smallest block the engine will accept. Below 255 frames it corrupts its own
   heap during TG_activate: 254 aborts every time, 255 renders identically to
   4096. Rounded up to a power of two. */
#define SCVA_MIN_BLOCK 256

/* explicit_path is the --core argument, or NULL. */
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
