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

/* Calling conventions across the boundary.
   x86-64 Windows has exactly one, so all three collapse to ms_abi. x86-32 has
   three that matter and mixing them up corrupts the stack: the Win32 API is
   stdcall, the CRT is cdecl, and the core's own exports are cdecl. */
#if defined(__x86_64__)
#define MSABI      __attribute__((ms_abi))
#define WINAPI_CC  __attribute__((ms_abi))
#define CDECL_CC   __attribute__((ms_abi))
#elif defined(__i386__)
#define MSABI
#define WINAPI_CC  __attribute__((stdcall))
#define CDECL_CC   __attribute__((cdecl))
#else
#error "this loader is x86 only"
#endif

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
