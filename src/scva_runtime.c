/* SPDX-License-Identifier: CC0-1.0
 *
 * See scva_runtime.h.
 */
#define _GNU_SOURCE
#include "scva_runtime.h"
#include "win9x.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

/* ---------- the fifty functions the image asks for ---------------------- */
/* Every one is MSABI because the image calls them with the Microsoft
   convention. They are deliberately minimal: this host is single-threaded and
   drives the engine synchronously, so locks and events have nothing to do. */

#ifndef _WIN32
/* Real locks and condition variables. The image asks for the condition
   variable trio by name through GetProcAddress and calls what it is given, so
   they must resolve. A Windows CRITICAL_SECTION has room for a pthread mutex;
   a CONDITION_VARIABLE is a single pointer, so it holds one we allocate.

   None of this is built on Windows: every one of these entries has been in
   kernel32 since Windows 95 and scva_runtime_resolve() takes the real one.
   What the system there cannot supply is the C runtime below. */
typedef struct { long long a, b, c, d, e; } CRIT;

static WINAPI_CC void ms_InitializeCriticalSectionAndSpinCount(CRIT *c, uint32_t n)
{
  pthread_mutexattr_t at;
  (void)n;
  if (!c) return;
  memset(c, 0, sizeof *c);
  pthread_mutexattr_init(&at);
  pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init((pthread_mutex_t *)c, &at);
  pthread_mutexattr_destroy(&at);
}
/* The 1.1.2 core reaches the same place through the CRT's own wrapper rather
   than through kernel32. The flags argument selects things none of which
   matter to a recursive mutex. */
static CDECL_CC int ms_vcrt_InitializeCriticalSectionEx(CRIT *c, uint32_t spin,
                                                        uint32_t flags)
{
  (void)flags;
  ms_InitializeCriticalSectionAndSpinCount(c, spin);
  return 1;
}

static WINAPI_CC void ms_DeleteCriticalSection(CRIT *c)
{ if (c) pthread_mutex_destroy((pthread_mutex_t *)c); }
static WINAPI_CC void ms_EnterCriticalSection(CRIT *c)
{ if (c) pthread_mutex_lock((pthread_mutex_t *)c); }
static WINAPI_CC void ms_LeaveCriticalSection(CRIT *c)
{ if (c) pthread_mutex_unlock((pthread_mutex_t *)c); }

static WINAPI_CC void ms_InitializeConditionVariable(void **cv)
{
  pthread_cond_t *c;
  if (!cv) return;
  c = calloc(1, sizeof *c);
  if (c) pthread_cond_init(c, NULL);
  *cv = c;
}
static WINAPI_CC int ms_SleepConditionVariableCS(void **cv, CRIT *cs, uint32_t ms)
{
  if (!cv || !*cv || !cs) return 1;
  if (ms == 0xffffffffu) {
    pthread_cond_wait((pthread_cond_t *)*cv, (pthread_mutex_t *)cs);
  } else {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec  += (time_t)(ms / 1000u);
    t.tv_nsec += (long)(ms % 1000u) * 1000000L;
    if (t.tv_nsec >= 1000000000L) { t.tv_nsec -= 1000000000L; ++t.tv_sec; }
    pthread_cond_timedwait((pthread_cond_t *)*cv, (pthread_mutex_t *)cs, &t);
  }
  return 1;
}
static WINAPI_CC void ms_WakeAllConditionVariable(void **cv)
{ if (cv && *cv) pthread_cond_broadcast((pthread_cond_t *)*cv); }
static WINAPI_CC void ms_WakeConditionVariable(void **cv)
{ if (cv && *cv) pthread_cond_signal((pthread_cond_t *)*cv); }
static WINAPI_CC void *ms_CreateEventW(void *a, int b, int c, void *d)
{ (void)a;(void)b;(void)c;(void)d; return (void *)0x1000; }
static WINAPI_CC int   ms_SetEvent(void *h) { (void)h; return 1; }
static WINAPI_CC int   ms_ResetEvent(void *h) { (void)h; return 1; }
static WINAPI_CC int   ms_CloseHandle(void *h) { (void)h; return 1; }
static WINAPI_CC uint32_t ms_WaitForSingleObjectEx(void *h, uint32_t ms, int alert)
{ (void)h;(void)ms;(void)alert; return 0; }
static WINAPI_CC void *ms_GetCurrentProcess(void) { return (void *)-1; }
static WINAPI_CC uint32_t ms_GetCurrentProcessId(void) { return 1; }
static WINAPI_CC uint32_t ms_GetCurrentThreadId(void) { return 1; }
static WINAPI_CC void *ms_GetModuleHandleW(const uint16_t *n)
{
  if (getenv("PE_TRACE") && n) {
    char b[128]; int i = 0;
    while (n[i] && i < 127) { b[i] = (char)n[i]; ++i; }
    b[i] = 0;
    fprintf(stderr, "pe: GetModuleHandleW(\"%s\")\n", b);
  } else if (getenv("PE_TRACE")) fprintf(stderr, "pe: GetModuleHandleW(NULL)\n");
  return (void *)0x2000;
}
static WINAPI_CC void *ms_GetProcAddress(void *m, const char *n)
{
  void *r = n ? scva_runtime_resolve(NULL, NULL, n) : NULL;
  if (getenv("PE_TRACE"))
    fprintf(stderr, "pe: GetProcAddress(%p, \"%s\") -> %s\n",
            m, n ? n : "(ordinal)", r ? "ok" : "NULL");
  return r;
}
static WINAPI_CC int   ms_IsDebuggerPresent(void) { return 0; }
static WINAPI_CC int   ms_IsProcessorFeaturePresent(uint32_t f) { (void)f; return 1; }
/* SLIST_HEADER is two pointers wide: 8 bytes on x86, 16 on x86-64. */
static WINAPI_CC void  ms_InitializeSListHead(void *p) { if (p) memset(p, 0, 2 * sizeof(void *)); }
static WINAPI_CC void *ms_SetUnhandledExceptionFilter(void *f) { (void)f; return NULL; }
static WINAPI_CC long  ms_UnhandledExceptionFilter(void *p) { (void)p; return 1; }
static WINAPI_CC void  ms_TerminateProcess(void *h, uint32_t c)
{ (void)h; fprintf(stderr, "pe: image called TerminateProcess(%u)\n", c); _exit((int)c); }
static WINAPI_CC int ms_QueryPerformanceCounter(int64_t *v)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  if (v) *v = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
  return 1;
}
static WINAPI_CC void ms_GetSystemTimeAsFileTime(uint64_t *ft)
{ struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
  if (ft) *ft = ((uint64_t)t.tv_sec + 11644473600ULL) * 10000000ULL + t.tv_nsec / 100; }
static WINAPI_CC void ms_GetLocalTime(uint16_t *st)
{ if (st) memset(st, 0, 16); }
/* unwinding: only reached if the image throws, which it must not */
static WINAPI_CC void  ms_RtlCaptureContext(void *c) { if (c) memset(c, 0, 1232); }
static WINAPI_CC void *ms_RtlLookupFunctionEntry(uint64_t pc, uint64_t *base, void *hist)
{ (void)pc;(void)hist; if (base) *base = 0; return NULL; }
static WINAPI_CC void  ms_RtlVirtualUnwind(uint32_t a, uint64_t b, uint64_t c, void *d,
                                       void *e, void *f, void *g, void *h)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; }

#endif  /* !_WIN32 */

#ifdef _WIN32
/* The spin count is a tuning hint and the flags only affect debug info, so
   the call every kernel32 since Windows 95 has is enough. */
static CDECL_CC int ms_vcrt_InitializeCriticalSectionEx(CRITICAL_SECTION *c,
                                                        DWORD spin, DWORD flags)
{
  (void)spin; (void)flags;
  InitializeCriticalSection(c);
  return 1;
}
#endif

/* Zeroed: Windows hands back a fresh page where glibc recycles a dirty one. */
static CDECL_CC void *ms_malloc(size_t n) { return calloc(1, n ? n : 1); }
static CDECL_CC void  ms_free(void *p) { free(p); }
static CDECL_CC int   ms_callnewh(size_t n) { (void)n; return 0; }
/* ms_abi makes XMM6-XMM15, RDI and RSI callee-saved where System V does not;
   GCC spills them around the call into glibc, so no thunk is needed. */
static CDECL_CC void *ms_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static CDECL_CC void *ms_memset(void *d, int c, size_t n) { return memset(d, c, n); }

static CDECL_CC void ms_CxxThrowException(void *a, void *b)
{ (void)a;(void)b; fprintf(stderr, "pe: the image threw a C++ exception\n"); abort(); }
static CDECL_CC long ms_C_specific_handler(void *a, void *b, void *c, void *d)
{ (void)a;(void)b;(void)c;(void)d; return 1; }
static CDECL_CC long ms_CxxFrameHandler3(void *a, void *b, void *c, void *d)
{ (void)a;(void)b;(void)c;(void)d; return 1; }
static CDECL_CC void ms_std_exception_copy(void *a, void *b) { (void)a;(void)b; }
static CDECL_CC void ms_std_exception_destroy(void *a) { (void)a; }
static CDECL_CC void ms_std_terminate(void) { fprintf(stderr, "pe: std::terminate\n"); abort(); }
static CDECL_CC void ms_std_type_info_destroy_list(void *a) { (void)a; }

/* The tables hold function pointers INTO the image, so they are MS ABI, and
   both ends are inclusive-exclusive. Getting these wrong leaves the image's
   globals holding whatever the heap had, which shows up later as output that
   differs run to run. */
typedef CDECL_CC void (*pvfv)(void);
typedef CDECL_CC int  (*pifv)(void);

static CDECL_CC void ms_initterm(pvfv *first, pvfv *last)
{
  int n = 0;
  if (!first || !last) return;
  for (; first < last; ++first)
    if (*first) { (*first)(); ++n; }
  if (getenv("PE_TRACE")) fprintf(stderr, "pe: _initterm ran %d initialisers\n", n);
}
static CDECL_CC int ms_initterm_e(pifv *first, pifv *last)
{
  int n = 0;
  if (!first || !last) return 0;
  for (; first < last; ++first)
    if (*first) {
      int r = (*first)();
      ++n;
      if (r) { fprintf(stderr, "pe: an initialiser returned %d\n", r); return r; }
    }
  if (getenv("PE_TRACE")) fprintf(stderr, "pe: _initterm_e ran %d initialisers\n", n);
  return 0;
}
static CDECL_CC int  ms_crt_atexit(void *f) { (void)f; return 0; }
static CDECL_CC int  ms_register_onexit_function(void *t, void *f) { (void)t;(void)f; return 0; }
static CDECL_CC int  ms_initialize_onexit_table(void *t) { (void)t; return 0; }
static CDECL_CC int  ms_execute_onexit_table(void *t) { (void)t; return 0; }
static CDECL_CC int  ms_configure_narrow_argv(int m) { (void)m; return 0; }
static CDECL_CC int  ms_initialize_narrow_environment(void) { return 0; }
static CDECL_CC void ms_cexit(void) {}
static CDECL_CC void ms_exit(int c) { _exit(c); }
static CDECL_CC long ms_seh_filter_dll(uint32_t c, void *p) { (void)c;(void)p; return 0; }
static CDECL_CC int ms_stdio_common_vsprintf(uint64_t opt, char *buf, size_t n,
                                          const char *fmt, void *loc, va_list ap)
{ (void)opt; (void)loc; return vsnprintf(buf, n ? n : 0, fmt, ap); }


#if defined(__i386__)
/* The 32-bit core asks for a different set: no condition variables and no
   critical sections, but a handful of CRT internals the 64-bit build does
   without, and pow by way of the SSE2 libm entry point. */
#ifndef _WIN32
static WINAPI_CC uint32_t ms_GetTickCount(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint32_t)(t.tv_sec * 1000u + (uint32_t)(t.tv_nsec / 1000000)); }
static WINAPI_CC void  ms_Sleep(uint32_t ms)
{ struct timespec t; t.tv_sec = ms / 1000u;
  t.tv_nsec = (long)(ms % 1000u) * 1000000L; nanosleep(&t, NULL); }
static WINAPI_CC long  ms_InterlockedExchange(volatile long *p, long v)
{ return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
static WINAPI_CC long  ms_InterlockedCompareExchange(volatile long *p, long x, long c)
{ __atomic_compare_exchange_n(p, &c, x, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
  return c; }
#endif
/* Windows obfuscates stored pointers; anything self-consistent will do. */
static WINAPI_CC void *ms_EncodePointer(void *p) { return p; }
static WINAPI_CC void *ms_DecodePointer(void *p) { return p; }

static CDECL_CC void  ms_amsg_exit(int code)
{ fprintf(stderr, "pe: CRT fatal %d\n", code); _exit(code); }
static CDECL_CC void  ms_crt_debugger_hook(int r) { (void)r; }
static CDECL_CC void  ms_clean_type_info_names_internal(void *p) { (void)p; }
static CDECL_CC long  ms_CppXcptFilter(unsigned long c, void *p)
{ (void)c; (void)p; return 0; }
static CDECL_CC int   ms_except_handler4_common(void *a, void *b, void *c,
                                                void *d, void *e, void *f)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f; return 1; }
static CDECL_CC void *ms_dllonexit(void *f, void **b, void **e)
{ (void)b; (void)e; return f; }
static CDECL_CC void *ms_onexit(void *f) { return f; }
static CDECL_CC void  ms_lock(int n) { (void)n; }
static CDECL_CC void  ms_unlock(int n) { (void)n; }
static CDECL_CC void *ms_malloc_crt(size_t n) { return calloc(1, n ? n : 1); }
static CDECL_CC double ms_libm_sse2_pow(double x, double y) { return pow(x, y); }
static CDECL_CC int   ms_vsprintf(char *b, const char *f, va_list ap)
{ return vsprintf(b, f, ap); }
static CDECL_CC void *ms_operator_new(size_t n) { return calloc(1, n ? n : 1); }
static CDECL_CC void  ms_operator_delete(void *p) { free(p); }
/* __thiscall with no arguments: `this` arrives in ECX and the callee cleans
   nothing, which is what a no-argument stdcall does. */
static WINAPI_CC void ms_type_info_dtor(void) {}
static CDECL_CC void *ms_encoded_null(void) { return NULL; }
/* Reached from the core's float-to-integer helpers when a conversion is
   invalid. They load a default x87 control word before the call and leave
   restoring the caller's, passed in cw, to this; the result is discarded. */
static CDECL_CC double ms_except1(uint32_t fpe, int op, double arg, double res,
                                  uint64_t cw, void *unused)
{
  uint16_t c = (uint16_t)cw;
  (void)fpe; (void)op; (void)arg; (void)unused;
  __asm__ volatile ("fldcw %0" : : "m"(c));
  return res;
}
#endif

static const struct pe_binding BINDINGS[] = {
#ifndef _WIN32
  {"CloseHandle", ms_CloseHandle}, {"CreateEventW", ms_CreateEventW},
  {"DeleteCriticalSection", ms_DeleteCriticalSection},
  {"EnterCriticalSection", ms_EnterCriticalSection},
  {"LeaveCriticalSection", ms_LeaveCriticalSection},
  {"InitializeCriticalSectionAndSpinCount", ms_InitializeCriticalSectionAndSpinCount},
  {"GetCurrentProcess", ms_GetCurrentProcess},
  {"GetCurrentProcessId", ms_GetCurrentProcessId},
  {"GetCurrentThreadId", ms_GetCurrentThreadId},
  {"GetLocalTime", ms_GetLocalTime},
  {"GetModuleHandleW", ms_GetModuleHandleW},
  {"GetProcAddress", ms_GetProcAddress},
  {"GetSystemTimeAsFileTime", ms_GetSystemTimeAsFileTime},
  {"InitializeSListHead", ms_InitializeSListHead},
  {"IsDebuggerPresent", ms_IsDebuggerPresent},
  {"IsProcessorFeaturePresent", ms_IsProcessorFeaturePresent},
  {"QueryPerformanceCounter", ms_QueryPerformanceCounter},
  {"ResetEvent", ms_ResetEvent}, {"SetEvent", ms_SetEvent},
  {"RtlCaptureContext", ms_RtlCaptureContext},
  {"RtlLookupFunctionEntry", ms_RtlLookupFunctionEntry},
  {"RtlVirtualUnwind", ms_RtlVirtualUnwind},
  {"SetUnhandledExceptionFilter", ms_SetUnhandledExceptionFilter},
  {"UnhandledExceptionFilter", ms_UnhandledExceptionFilter},
  {"TerminateProcess", ms_TerminateProcess},
  {"WaitForSingleObjectEx", ms_WaitForSingleObjectEx},
#endif
  {"__vcrt_InitializeCriticalSectionEx", ms_vcrt_InitializeCriticalSectionEx},
  {"_CxxThrowException", ms_CxxThrowException},
  {"__C_specific_handler", ms_C_specific_handler},
  {"__CxxFrameHandler3", ms_CxxFrameHandler3},
  {"__std_exception_copy", ms_std_exception_copy},
  {"__std_exception_destroy", ms_std_exception_destroy},
  {"__std_terminate", ms_std_terminate},
  {"__std_type_info_destroy_list", ms_std_type_info_destroy_list},
  {"memcpy", ms_memcpy}, {"memset", ms_memset},
  {"malloc", ms_malloc}, {"free", ms_free}, {"_callnewh", ms_callnewh},
  {"__stdio_common_vsprintf", ms_stdio_common_vsprintf},
  {"_cexit", ms_cexit}, {"exit", ms_exit},
  {"_crt_atexit", ms_crt_atexit},
  {"_configure_narrow_argv", ms_configure_narrow_argv},
  {"_initialize_narrow_environment", ms_initialize_narrow_environment},
  {"_initialize_onexit_table", ms_initialize_onexit_table},
  {"_execute_onexit_table", ms_execute_onexit_table},
  {"_register_onexit_function", ms_register_onexit_function},
  {"_initterm", ms_initterm}, {"_initterm_e", ms_initterm_e},
  {"_seh_filter_dll", ms_seh_filter_dll},
#ifndef _WIN32
  {"InitializeConditionVariable", ms_InitializeConditionVariable},
  {"SleepConditionVariableCS", ms_SleepConditionVariableCS},
  {"WakeAllConditionVariable", ms_WakeAllConditionVariable},
  {"WakeConditionVariable", ms_WakeConditionVariable},
#endif
#if defined(__i386__)
#ifndef _WIN32
  {"GetTickCount", ms_GetTickCount}, {"Sleep", ms_Sleep},
  {"InterlockedExchange", ms_InterlockedExchange},
  {"InterlockedCompareExchange", ms_InterlockedCompareExchange},
#endif
  {"EncodePointer", ms_EncodePointer}, {"DecodePointer", ms_DecodePointer},
  {"_amsg_exit", ms_amsg_exit},
  {"_crt_debugger_hook", ms_crt_debugger_hook},
  {"__clean_type_info_names_internal", ms_clean_type_info_names_internal},
  {"__CppXcptFilter", ms_CppXcptFilter},
  {"_except_handler4_common", ms_except_handler4_common},
  {"__dllonexit", ms_dllonexit}, {"_onexit", ms_onexit},
  {"_lock", ms_lock}, {"_unlock", ms_unlock},
  {"_malloc_crt", ms_malloc_crt},
  {"__libm_sse2_pow", ms_libm_sse2_pow},
  {"vsprintf", ms_vsprintf},
  {"??2@YAPAXI@Z", ms_operator_new},
  {"??3@YAXPAX@Z", ms_operator_delete},
  {"?terminate@@YAXXZ", ms_std_terminate},
  {"?_type_info_dtor_internal_method@type_info@@QAEXXZ", ms_type_info_dtor},
  {"_encoded_null", ms_encoded_null},
  {"_except1", ms_except1},
#endif
  {NULL, NULL}
};

void *scva_runtime_resolve(void *ctx, const char *dll, const char *name)
{
  int i;
  (void)ctx;
#ifdef _WIN32
  /* Every Win32 entry the core asks for has been in kernel32 since Windows
     95, so the real one is used where it exists and the table below covers
     the rest: the C runtime, and the two pointer obfuscators that arrived
     with XP SP2 and are therefore missing on 98. */
  {
    HMODULE k = GetModuleHandleA("kernel32.dll");
    void *p = k ? (void *)GetProcAddress(k, name) : NULL;
    if (p) return p;
  }
#endif
  for (i = 0; BINDINGS[i].name; ++i)
    if (!strcmp(BINDINGS[i].name, name)) return BINDINGS[i].fn;
#ifdef _WIN32
  /* Last, the DLL the core names, where the system has it: XP with the
     VC++ redistributable has VCRUNTIME140 and the Universal CRT, so a core
     built against them needs no binding for each of its imports. Windows 98
     has neither and relies on the table alone. */
  if (dll) {
    HMODULE m = LoadLibraryA(dll);
    if (m) return (void *)GetProcAddress(m, name);
  }
#else
  (void)dll;
#endif
  return NULL;
}
