/* SPDX-License-Identifier: CC0-1.0
 *
 * A PE image for the loader's own tests, built with mingw-w64 and no C
 * runtime. Built twice:
 *
 *   PROBE_DEP   probe_dep.dll, which exports probe_hook. Only there to be
 *               imported: the tests bind that import to a function of their
 *               own and never load this one.
 *   (default)   peprobe.dll, which imports probe_hook from probe_dep.dll,
 *               marks when its entry point runs, and keeps an external and a
 *               file-static variable in its COFF symbol table.
 */
#include <windows.h>

#define EXPORT __declspec(dllexport)

#ifdef PROBE_DEP
EXPORT int probe_hook(int x) { return x; }
#else
__declspec(dllimport) int probe_hook(int x);

static int entry_ran;
int probe_external = 0x1234;
/* volatile, or it is folded into probe_read_static and has no symbol */
static volatile int probe_static = 0x5678;

/* The import, called with x: whatever the loader bound it to answers. */
EXPORT int probe_call(int x) { return probe_hook(x); }
EXPORT int probe_entry_ran(void) { return entry_ran; }
/* Keeps probe_static from being folded away. */
EXPORT int probe_read_static(void) { return probe_static; }
#endif

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
  (void)h; (void)reserved;
#ifndef PROBE_DEP
  if (reason == DLL_PROCESS_ATTACH) entry_ran = 1;
#else
  (void)reason;
#endif
  return TRUE;
}
