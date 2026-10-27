/* SPDX-License-Identifier: CC0-1.0
 *
 * Load a Windows x86 image into this process, without wine: map it,
 * relocate it, bind its imports to functions the caller supplies, set up its
 * thread-local storage, and run its entry point if asked to.
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
  int tls_index;            /* slot this image was given, -1 if none */
  void *tls;                /* 32-bit Windows: its thread-local storage */
  unsigned char *coff;      /* the file's COFF symbols then their strings */
  size_t coff_size;
  uint32_t coff_count;      /* symbol records, auxiliary ones included */
};

/* What an import binds to: dll is the library the image names, name the
   function. NULL when there is nothing to bind it to. */
typedef void *(*pe_resolve_fn)(void *ctx, const char *dll, const char *name);

struct pe_image;

/* The image's code as a patch hook sees it, still writable. */
struct pe_code {
  unsigned char *start;
  size_t size;
};
struct pe_patch_area {
  const struct pe_code *code;   /* the code sections */
  int ncode;
  unsigned char *spare;         /* pe_options.spare bytes past the image,
                                   executable once loaded and within rel32
                                   reach of all of it; NULL if none */
  size_t spare_size;
};

/* Rewrites the image's code before it is made read-only: instructions this
   host cannot run, or calls that must go elsewhere. The image is relocated
   and its imports bound; pe_symbol_coff works on it. 0, with the reason in
   err, fails the load. */
typedef int (*pe_patch_fn)(void *ctx, struct pe_image *img,
                           const struct pe_patch_area *area,
                           char *err, size_t errlen);

struct pe_options {
  pe_resolve_fn resolve;
  void *ctx;                /* passed to resolve and to patch */
  int no_entry;             /* leave the entry point alone */
  pe_patch_fn patch;        /* NULL: the code is left as it is */
  size_t spare;             /* bytes to reserve past the image for patch */
};

/* A resolver over a table: ctx is an array of these ending in {NULL, NULL},
   searched by name whatever the library. */
struct pe_binding { const char *name; void *fn; };
void *pe_resolve_table(void *ctx, const char *dll, const char *name);

/* Map, relocate, bind the imports through opt, patch the code through it,
   and run the entry point unless opt says not to. NULL, with the reason in
   err, on failure. */
struct pe_image *pe_load(const char *path, const struct pe_options *opt,
                         char *err, size_t errlen);
/* Gives the calling thread what the image expects of a Windows thread. Call
   it on any thread before that thread enters the image: a plugin host's
   audio thread is rarely the one that loaded it. Cheap once a thread has it;
   0 if it could not be set up. */
int pe_thread_attach(void);
/* Look an export up by name. NULL if absent. */
void *pe_symbol(struct pe_image *img, const char *name);

/* COFF storage classes worth looking up. */
#define PE_SYM_EXTERNAL 2
#define PE_SYM_STATIC   3

/* What a COFF symbol's value counts from. The format says its section,
   which is what mingw writes; some older Microsoft linkers wrote the
   image-relative address instead. */
enum pe_coff_base { PE_COFF_SECTION, PE_COFF_IMAGE };

/* Look a symbol up in the image's COFF symbol table, for the images that
   keep one: a driver's entry points and tables are found there rather than
   among its exports. NULL if absent, of another class, or outside the
   image. */
void *pe_symbol_coff(const struct pe_image *img, const char *name, int cls,
                     enum pe_coff_base base);
/* Unmap an image and give its TLS slot back. Safe to call on an engine that
   has been deactivated; do not call TG_terminate first, it calls exit(). */
void pe_unload(struct pe_image *img);
#endif
