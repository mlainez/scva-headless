/* SPDX-License-Identifier: CC0-1.0
 *
 * A VST2 entry point that forwards to SOUND Canvas VA's own wrapper.
 *
 * The wrapper exports its entry as `R2RPluginMain` rather than the
 * `VSTPluginMain` a host looks for, so a host cannot open it directly.  This
 * publishes both of the names a VST2 host tries and hands the call straight
 * through; it holds no plug-in logic of its own.
 *
 * It exists because the shim that shipped alongside the wrapper fails its own
 * DllMain under wine (LoadLibrary returns 1114, ERROR_DLL_INIT_FAILED) while
 * the wrapper itself loads perfectly.
 *
 * The wrapper is resolved next to THIS dll rather than through the registry or
 * the working directory, so the host's cwd cannot change which one is found.
 */
#include <windows.h>

typedef void *(*entry_fn)(void *);
static HMODULE wrapper;
static HMODULE self;

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) self = (HMODULE)inst;
  return TRUE;                      /* never fail here: a host reports only 1114 */
}

static entry_fn resolve(void)
{
  char path[MAX_PATH];
  DWORD n;
  if (wrapper) return (entry_fn)(void *)GetProcAddress(wrapper, "R2RPluginMain");
  n = GetModuleFileNameA(self, path, sizeof path);
  if (n == 0 || n >= sizeof path) return NULL;
  while (n > 0 && path[n - 1] != '\\' && path[n - 1] != '/') --n;
  path[n] = 0;
  if (n + sizeof "Wrapper.dll" > sizeof path) return NULL;
  lstrcatA(path, "Wrapper.dll");
  wrapper = LoadLibraryA(path);
  if (!wrapper) wrapper = LoadLibraryA("Wrapper.dll");
  if (!wrapper) return NULL;
  return (entry_fn)(void *)GetProcAddress(wrapper, "R2RPluginMain");
}

__declspec(dllexport) void *VSTPluginMain(void *audio_master)
{
  entry_fn f = resolve();
  return f ? f(audio_master) : NULL;
}

/* the name older hosts look for */
__declspec(dllexport) void *main_plugin(void *audio_master)
{
  return VSTPluginMain(audio_master);
}
