/* SPDX-License-Identifier: CC0-1.0
 *
 * Load a Windows x86-64 DLL into a Linux process, without wine.
 *
 * This is possible for SOUND Canvas VA's core because that DLL is unusually
 * self-contained: 50 imports across five libraries, no file I/O, no registry,
 * no threads it creates itself, no GUI and no COM. Its data is inside the
 * image. So the loader only has to map it, relocate it, hand it fifty small
 * functions, and run its static initialisers.
 *
 * The calling convention is handled by the compiler: everything that crosses
 * into or out of the image is declared __attribute__((ms_abi)), so GCC emits
 * the argument shuffling and the 32 bytes of shadow space rather than us
 * writing thunks by hand.
 */
#ifndef SCVA_PE_LOADER_H
#define SCVA_PE_LOADER_H
#include <stdint.h>
#include <stddef.h>

#define MSABI __attribute__((ms_abi))

struct pe_image {
  unsigned char *base;      /* where the image is mapped */
  size_t size;
  uint64_t entry;           /* DllMain, image-relative */
};

/* Map, relocate, bind imports, run the entry point. NULL on failure. */
struct pe_image *pe_load(const char *path, char *err, size_t errlen);
/* Look an export up by name. NULL if absent. */
void *pe_symbol(struct pe_image *img, const char *name);
void pe_unload(struct pe_image *img);
#endif
