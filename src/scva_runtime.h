/* SPDX-License-Identifier: CC0-1.0
 *
 * What SOUND Canvas VA's core imports, for the PE loader to bind it to.
 *
 * The core is unusually self-contained: 50 imports across five libraries,
 * no file I/O, no registry, no threads it creates itself, no GUI and no COM.
 * So it needs only a handful of Win32 calls and enough of the Visual C++
 * runtime to run its static initialisers.
 */
#ifndef SCVA_RUNTIME_H
#define SCVA_RUNTIME_H
#include "pe_loader.h"

/* A pe_resolve_fn. On Windows the system's kernel32 is used first, then
   these, then the library the core names, if the system has it. */
void *scva_runtime_resolve(void *ctx, const char *dll, const char *name);

/* Maps a core and runs its start-up. NULL, with the reason in err, on
   failure. */
static inline struct pe_image *scva_core_map(const char *path, char *err,
                                             size_t errlen)
{
  struct pe_options opt;
  opt.resolve = scva_runtime_resolve;
  opt.ctx = NULL;
  opt.no_entry = 0;
  return pe_load(path, &opt, err, errlen);
}
#endif
