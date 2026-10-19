/* SPDX-License-Identifier: CC0-1.0  -- see pe_loader.h */
#define _GNU_SOURCE
#include "pe_loader.h"
#include "win9x.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>
#ifdef _WIN32
/* Windows already gives a thread block, structured exception handling and a
   loader-shaped address space, so the port needs only the memory calls. The
   point of using this loader there rather than LoadLibrary is that the core
   asks for MSVCR100 and declares a subsystem newer than Windows 98, neither
   of which the system loader will forgive - while the bindings below supply
   that runtime and nothing checks a subsystem field. */
#include <windows.h>
#else
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <asm/prctl.h>
#include <asm/ldt.h>
#include <sys/syscall.h>
#endif

/* stub space reserved past the image: SSE3 stubs then the SSE2 arena */
#define SCVA_SHIM_SPACE 0x10000
#include "sse3_shim.h"
#if defined(__i386__)
#include "sse2_shim.h"
#endif


/* Windows reads its thread block through a segment register, and on both
   widths Linux happens to leave that exact register alone: 64-bit Windows
   uses GS while Linux x86-64 keeps its TLS in FS, and 32-bit Windows uses FS
   while Linux i386 keeps its TLS in GS. So the register is free on both, and
   pointing it at a block we build is what makes running the image without
   wine possible at all. Without it the CRT start-up faults on the first
   segment-relative access it makes. */
#if defined(__x86_64__)
#define PE_TIB_EXCEPT     0x00
#define PE_TIB_STACKBASE  0x08
#define PE_TIB_STACKLIMIT 0x10
#define PE_TIB_SELF       0x30
#define PE_TIB_PID        0x40
#define PE_TIB_TID        0x48
#define PE_TIB_TLS        0x58
#define PE_TIB_PEB        0x60
#else
#define PE_TIB_EXCEPT     0x00
#define PE_TIB_STACKBASE  0x04
#define PE_TIB_STACKLIMIT 0x08
#define PE_TIB_SELF       0x18
#define PE_TIB_PID        0x20
#define PE_TIB_TID        0x24
#define PE_TIB_TLS        0x2c
#define PE_TIB_PEB        0x30
#endif

#ifdef _WIN32
static void *pe_reserve(void *want, size_t n)
{
  void *p = want ? VirtualAlloc(want, n, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE) : NULL;
  if (!p) p = VirtualAlloc(NULL, n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  return p;
}
static void pe_release(void *p, size_t n) { (void)n; VirtualFree(p, 0, MEM_RELEASE); }
static void pe_setprot(void *p, size_t n, int exec, int write)
{
  DWORD old;
  DWORD f = exec ? (write ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                 : (write ? PAGE_READWRITE : PAGE_READONLY);
  VirtualProtect(p, n, f, &old);
}
/* The thread block, the exception chain and the segment register are all the
   real thing here. */
static int install_teb(void) { return 1; }
#else
/* Every thread that enters the image needs its own block: the stack bounds in
   it must be that thread's, because MSVC's stack probes walk down to them.
   The PEB and the TLS pointer array are shared, so all threads see one set of
   thread-local state - the core is set up on one thread and, in a plugin
   host, run on another. A thread's block outlives the thread; nothing tells
   us it ended. */
static __thread unsigned char *t_teb;
static unsigned char *g_peb;
static void **g_tls_slots;
#if defined(__i386__)
static int g_teb_entry = -1;    /* the GDT slot, the same on every thread */
#endif

static void *pe_reserve(void *want, size_t n)
{
  void *p = mmap(want, n, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | (want ? MAP_FIXED_NOREPLACE : 0),
                 -1, 0);
  if (p == MAP_FAILED || (want && p != want)) {
    if (p != MAP_FAILED) munmap(p, n);
    p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  }
  return p == MAP_FAILED ? NULL : p;
}
static void pe_release(void *p, size_t n) { munmap(p, n); }
static void pe_setprot(void *p, size_t n, int exec, int write)
{
  int prot = PROT_READ | (exec ? PROT_EXEC : 0) | (write ? PROT_WRITE : 0);
  mprotect(p, n, prot);
}

static int install_teb(void)
{
  size_t sz = 0x2000;
  unsigned char *teb;
  if (t_teb) {
    /* attached before the first image with TLS was loaded */
    if (!*(void **)(t_teb + PE_TIB_TLS))
      *(void **)(t_teb + PE_TIB_TLS) = g_tls_slots;
    return 1;
  }
  if (!g_peb) {
    g_peb = mmap(NULL, sz, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_peb == MAP_FAILED) { g_peb = NULL; return 0; }
    memset(g_peb, 0, sz);
  }
  teb = mmap(NULL, sz, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (teb == MAP_FAILED) return 0;
  memset(teb, 0, sz);
  *(void **)(teb + PE_TIB_TLS) = g_tls_slots;
  {
    /* NT_TIB: the few fields start-up code actually reads.
       The stack bounds must be the REAL ones - MSVC's stack probes compare
       against them, so a guess sends a deep call straight through the floor. */
    void *stack_addr = NULL; size_t stack_sz = 0;
    pthread_attr_t at;
    if (pthread_getattr_np(pthread_self(), &at) == 0) {
      pthread_attr_getstack(&at, &stack_addr, &stack_sz);
      pthread_attr_destroy(&at);
    }
    if (!stack_addr) { char dummy; stack_addr = (void *)((uintptr_t)&dummy - (8u<<20)); stack_sz = 8u<<20; }
    *(void **)(teb + PE_TIB_EXCEPT) = (void *)~(uintptr_t)0; /* -1 */
    *(void **)(teb + PE_TIB_STACKBASE) =
      (void *)((uintptr_t)stack_addr + stack_sz);
    *(void **)(teb + PE_TIB_STACKLIMIT) = stack_addr;
    *(void **)(teb + PE_TIB_SELF) = teb;
    *(void **)(teb + PE_TIB_PEB) = g_peb;
    *(uint32_t *)(teb + PE_TIB_PID) = 1;
    *(uint32_t *)(teb + PE_TIB_TID) = 1;
    *(uint32_t *)(g_peb + 0x00) = 0;                  /* InheritedAddressSpace */
#if defined(__x86_64__)
    *(uint32_t *)(g_peb + 0x118) = 10;                /* OSMajorVersion */
    *(uint32_t *)(g_peb + 0x11c) = 0;
#else
    *(uint32_t *)(g_peb + 0x0a4) = 10;
    *(uint32_t *)(g_peb + 0x0a8) = 0;
#endif
  }
#if defined(__x86_64__)
  if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)teb) != 0) return 0;
#else
  {
    /* i386 has no arch_prctl for this: a descriptor is added to the thread's
       GDT and FS loaded with its selector. entry_number -1 asks the kernel to
       pick a free slot and write back which one it used; later threads ask
       for that same slot in their own GDT. */
    struct user_desc d;
    memset(&d, 0, sizeof d);
    d.entry_number    = (unsigned)g_teb_entry;
    d.base_addr       = (unsigned long)teb;
    d.limit           = 0xfffff;
    d.seg_32bit       = 1;
    d.contents        = 0;
    d.read_exec_only  = 0;
    d.limit_in_pages  = 1;
    d.seg_not_present = 0;
    d.useable         = 1;
    if (syscall(SYS_set_thread_area, &d) != 0) return 0;
    g_teb_entry = (int)d.entry_number;
    __asm__ volatile("movw %w0, %%fs"
                     :: "q"((unsigned short)((d.entry_number << 3) | 3)));
  }
#endif
  t_teb = teb;
  return 1;
}
#endif

int pe_thread_attach(void) { return install_teb(); }

/* ---------- the little of the PE format that matters -------------------- */
#pragma pack(push, 1)
struct dos { uint16_t magic; uint8_t pad[58]; uint32_t lfanew; };
struct fh { uint16_t machine, nsec; uint32_t stamp, symp, nsym; uint16_t optsz, chars; };
struct dir { uint32_t rva, size; };
struct oh64 {
  uint16_t magic; uint8_t major, minor;
  uint32_t codesz, initsz, uninitsz, entry, codebase;
  uint64_t imagebase;
  uint32_t salign, falign;
  uint16_t osmaj, osmin, imgmaj, imgmin, submaj, submin;
  uint32_t win32ver, imagesz, headersz, checksum;
  uint16_t subsystem, dllchars;
  uint64_t stackres, stackcom, heapres, heapcom;
  uint32_t loaderflags, ndirs;
  struct dir dirs[16];
};
struct oh32 {
  uint16_t magic; uint8_t major, minor;
  uint32_t codesz, initsz, uninitsz, entry, codebase, database;
  uint32_t imagebase;
  uint32_t salign, falign;
  uint16_t osmaj, osmin, imgmaj, imgmin, submaj, submin;
  uint32_t win32ver, imagesz, headersz, checksum;
  uint16_t subsystem, dllchars;
  uint32_t stackres, stackcom, heapres, heapcom;
  uint32_t loaderflags, ndirs;
  struct dir dirs[16];
};

#if defined(__x86_64__)
typedef struct oh64 opt_hdr;
typedef uint64_t thunk_t;
#define PE_OPT_MAGIC  0x20b
#define PE_MACHINE    0x8664
#define PE_RELOC_ABS  10                    /* IMAGE_REL_BASED_DIR64  */
#define PE_ORD_FLAG   ((thunk_t)1 << 63)
#else
typedef struct oh32 opt_hdr;
typedef uint32_t thunk_t;
#define PE_OPT_MAGIC  0x10b
#define PE_MACHINE    0x14c
#define PE_RELOC_ABS  3                     /* IMAGE_REL_BASED_HIGHLOW */
#define PE_ORD_FLAG   ((thunk_t)1 << 31)
#endif

struct sh {
  char name[8]; uint32_t vsize, vaddr, rawsize, rawptr, relptr, lineptr;
  uint16_t nrel, nline; uint32_t chars;
};
struct imp_desc { uint32_t oft, stamp, fwd, name, ft; };
struct exp_dir {
  uint32_t flags, stamp; uint16_t maj, min;
  uint32_t name, base, nfuncs, nnames, funcs, names, ords;
};
#pragma pack(pop)

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
   kernel32 since Windows 95 and pe_bind() takes the real one. What the system
   there cannot supply is the C runtime below. */
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
static void *pe_bind(const char *dll, const char *name);
static WINAPI_CC void *ms_GetProcAddress(void *m, const char *n)
{
  void *r = n ? pe_bind(NULL, n) : NULL;
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

struct binding { const char *name; void *fn; };
static const struct binding BINDINGS[] = {
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

static void *pe_bind(const char *dll, const char *name)
{
  int i;
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

/* ---------- the loader -------------------------------------------------- */
#if defined(_WIN32) && defined(__i386__)
/* Implicit TLS on 32-bit Windows. The image finds its block through the
   array at fs:[0x2c], which only the system loader can extend: on NT it is
   ntdll's per-module array, on 9x the TlsAlloc slot array. So each
   `mov eax, fs:[0x2c]` in the image becomes a call to a stub returning an
   array of our own, whose entry 0 is this thread's copy of the block.

   Every image shares one TlsAlloc slot holding a per-thread list keyed by an
   id that is never reused, so an image loaded after another was unloaded
   cannot pick up the old one's block. Nothing tells a mapped image that a
   thread has ended, so a thread's copies are never freed. */
struct pe_tls {
  uint32_t id;
  unsigned char *tmpl;      /* raw bytes; the rest of the block is zero */
  size_t raw, total;
  unsigned char *stub;
};
struct pe_tls_node {
  struct pe_tls_node *next;
  uint32_t id;
  void *arr[1];             /* the image indexes this with _tls_index = 0 */
};
static DWORD g_tls_slot = TLS_OUT_OF_INDEXES;
static uint32_t g_tls_ids;

static void **__cdecl pe_tls_array(const struct pe_tls *t)
{
  DWORD e = GetLastError();
  struct pe_tls_node *head = TlsGetValue(g_tls_slot), *n;
  for (n = head; n; n = n->next)
    if (n->id == t->id) break;
  if (!n) {
    n = calloc(1, sizeof *n + t->total);
    if (!n) abort();
    n->next = head;
    n->id = t->id;
    n->arr[0] = n + 1;
    memcpy(n->arr[0], t->tmpl, t->raw);
    TlsSetValue(g_tls_slot, n);
  }
  SetLastError(e);
  return n->arr;
}
#endif

/* The TLS pointer array at gs:[0x58] belongs to the thread, not to an image,
   so it is allocated once and every image gets its own index - which is what
   TlsAlloc does on Windows. Without it a second image overwrites the first
   image's block and the two share thread-local state. */
#ifndef _WIN32
static uint32_t g_tls_next;
#define PE_TLS_SLOTS 64
/* Slots handed back by pe_unload, so a host that creates and destroys plugin
   instances does not run the array out. */
static int g_tls_free[PE_TLS_SLOTS];
static int g_tls_nfree;
#endif

void *pe_symbol(struct pe_image *img, const char *name)
{
  struct dos *d = (struct dos *)img->base;
  unsigned char *nt = img->base + d->lfanew;
  struct fh *fh = (struct fh *)(nt + 4);
  opt_hdr *oh = (opt_hdr *)(nt + 4 + sizeof *fh);
  struct exp_dir *ed;
  uint32_t *names, *funcs; uint16_t *ords; uint32_t i;
  (void)fh;
  if (!oh->dirs[0].rva) return NULL;
  ed = (struct exp_dir *)(img->base + oh->dirs[0].rva);
  names = (uint32_t *)(img->base + ed->names);
  funcs = (uint32_t *)(img->base + ed->funcs);
  ords  = (uint16_t *)(img->base + ed->ords);
  for (i = 0; i < ed->nnames; ++i)
    if (!strcmp((char *)(img->base + names[i]), name))
      return img->base + funcs[ords[i]];
  return NULL;
}

struct pe_image *pe_load(const char *path, char *err, size_t errlen)
{
#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return NULL; } while (0)
  long fsz;
  unsigned char *g_file;
  FILE *fp = fopen(path, "rb");
  struct pe_image *img = calloc(1, sizeof *img);
  if (!img) FAIL("out of memory");
  img->tls_index = -1;
  if (!fp) FAIL("cannot open %s", path);
  fseek(fp, 0, SEEK_END); fsz = ftell(fp); rewind(fp);
  g_file = malloc((size_t)fsz);
  if (!g_file || fread(g_file, 1, (size_t)fsz, fp) != (size_t)fsz) {
    free(g_file); fclose(fp);
    FAIL("cannot read %s", path);
  }
  fclose(fp);

  if (!install_teb()) FAIL("cannot install a thread block on GS");
  if (getenv("PE_TRACE")) fprintf(stderr, "pe: GS points at a TEB\n");

  struct dos *d = (struct dos *)g_file;
  if (d->magic != 0x5a4d) FAIL("not a PE");
  unsigned char *nt = g_file + d->lfanew;
  if (memcmp(nt, "PE\0\0", 4)) FAIL("no PE signature");
  struct fh *fh = (struct fh *)(nt + 4);
  opt_hdr *oh = (opt_hdr *)(nt + 4 + sizeof *fh);
  if (fh->machine != PE_MACHINE)
    FAIL("wrong machine: this build hosts %s cores",
         PE_MACHINE == 0x8664 ? "64-bit" : "32-bit");
  if (oh->magic != PE_OPT_MAGIC) FAIL("wrong PE optional header");

  /* Prefer the image's own base: then the relocation pass has nothing to do
     and cannot be the thing that is wrong. Falling back to anywhere is fine,
     but if behaviour differs between the two, the relocations are the suspect. */
  /* Space past the image belongs to the shim stubs, so that the calls which
     replace each patched site are always within reach of rel32. */
  size_t mapsz = (size_t)oh->imagesz + SCVA_SHIM_SPACE;
  unsigned char *base = pe_reserve((void *)(uintptr_t)oh->imagebase, mapsz);
  if (!base) FAIL("cannot reserve %u bytes", oh->imagesz);
  if (getenv("PE_TRACE"))
    fprintf(stderr, base == (unsigned char *)(uintptr_t)oh->imagebase
            ? "pe: mapped at its preferred base, no relocation needed\n"
            : "pe: relocating (preferred base taken)\n");
  memcpy(base, g_file, oh->headersz);
  struct sh *sec = (struct sh *)(nt + 4 + sizeof *fh + fh->optsz);
  for (int i = 0; i < fh->nsec; ++i) {
    if (sec[i].rawsize)
      memcpy(base + sec[i].vaddr, g_file + sec[i].rawptr, sec[i].rawsize);
    if (sec[i].vsize > sec[i].rawsize)
      memset(base + sec[i].vaddr + sec[i].rawsize, 0, sec[i].vsize - sec[i].rawsize);
  }

  if (getenv("PE_TRACE")) fprintf(stderr, "pe: %s\n", "sections copied");
  /* relocations */
  int64_t delta = (int64_t)(uintptr_t)base - (int64_t)oh->imagebase;
  if (delta && oh->dirs[5].size) {
    unsigned char *p = base + oh->dirs[5].rva, *end = p + oh->dirs[5].size;
    while (p < end) {
      uint32_t page = *(uint32_t *)p, blk = *(uint32_t *)(p + 4);
      if (blk < 8) break;
      uint16_t *e = (uint16_t *)(p + 8);
      uint32_t n = (blk - 8) / 2;
      for (uint32_t i = 0; i < n; ++i) {
        int type = e[i] >> 12, off = e[i] & 0xfff;
        if (type == PE_RELOC_ABS) {
#if defined(__x86_64__)
          *(uint64_t *)(base + page + off) += (uint64_t)delta;
#else
          *(uint32_t *)(base + page + off) += (uint32_t)delta;
#endif
        } else if (type != 0) FAIL("unhandled relocation type %d", type);
      }
      p += blk;
    }
  }

  if (getenv("PE_TRACE")) fprintf(stderr, "pe: %s\n", "relocations applied");
  /* imports */
  if (oh->dirs[1].size) {
    struct imp_desc *im = (struct imp_desc *)(base + oh->dirs[1].rva);
    for (; im->name; ++im) {
      thunk_t *oft = (thunk_t *)(base + (im->oft ? im->oft : im->ft));
      thunk_t *ft  = (thunk_t *)(base + im->ft);
      for (; *oft; ++oft, ++ft) {
        if (*oft & PE_ORD_FLAG)
          FAIL("%s imports by ordinal", (char *)(base + im->name));
        const char *nm = (const char *)(base + (*oft & 0xffffffff) + 2);
        void *fn = pe_bind((const char *)(base + im->name), nm);
        if (!fn) FAIL("no binding for %s (%s)", nm, (char *)(base + im->name));
        *ft = (thunk_t)(uintptr_t)fn;
      }
    }
  }

  if (getenv("PE_TRACE")) fprintf(stderr, "pe: %s\n", "imports bound");

  /* The core's interpolation sums four taps with a pair of haddps. Where the
     processor has no SSE3 that pair becomes a call to an SSE1 stub which
     pairs the lanes the same way, so the audio does not change. SCVA_SSE3
     forces it on for testing on a machine that does have SSE3. */
#if defined(__i386__)
  {
    const char *force = getenv("SCVA_SSE2");
    int want = force ? (*force == '0' ? 0 : 1) : !scva_cpu_has_sse2();
    if (want) {
      int left = 0;
      int done = scva_sse2_patch(base, base + oh->imagesz + 0x1000,
                                 SCVA_SHIM_SPACE - 0x1000, &left);
      if (done < 0)
        fprintf(stderr, "pe: SSE2 shim: this is not the core the table "
                        "describes, leaving it alone\n");
      else
        fprintf(stderr, "pe: SSE2 shim, %d sites patched, %d left\n",
                done, left);
    }
  }
#endif

  {
    const char *force = getenv("SCVA_SSE3");
    int want = force ? (*force == '0' ? 0 : 1) : !scva_cpu_has_sse3();
    if (want) {
      int done = 0;
      for (int i = 0; i < fh->nsec; ++i) {
        if (!(sec[i].chars & 0x20000000)) continue;      /* code only */
        done += scva_sse3_patch(base + sec[i].vaddr, sec[i].vsize,
                                base + oh->imagesz);
      }
      if (done || getenv("PE_TRACE"))
        fprintf(stderr, "pe: SSE3 shim, %d site%s patched\n",
                done, done == 1 ? "" : "s");
    }
  }

#if defined(_WIN32) && defined(__i386__)
  if (oh->dirs[9].size) {
    static const unsigned char site[6] = { 0x64, 0xa1, 0x2c, 0, 0, 0 };
    struct {
      uint32_t start, end, index_addr, callbacks, zerofill, chars;
    } *td = (void *)(base + oh->dirs[9].rva);
    struct pe_tls *t = calloc(1, sizeof *t);
    unsigned char *s;
    int sites = 0;
    if (!t) FAIL("out of memory for TLS");
    if (g_tls_slot == TLS_OUT_OF_INDEXES) g_tls_slot = TlsAlloc();
    t->id = ++g_tls_ids;
    t->raw = td->end - td->start;
    t->total = t->raw + td->zerofill;
    t->tmpl = malloc(t->raw ? t->raw : 1);
    t->stub = pe_reserve(NULL, 0x1000);
    if (g_tls_slot == TLS_OUT_OF_INDEXES || !t->tmpl || !t->stub)
      FAIL("cannot set up thread-local storage");
    memcpy(t->tmpl, (void *)(uintptr_t)td->start, t->raw);
    /* pushfd; push ecx; push edx; push t; call pe_tls_array; add esp, 4;
       pop edx; pop ecx; popfd; ret - eax is the only register it changes */
    s = t->stub;
    *s++ = 0x9c; *s++ = 0x51; *s++ = 0x52;
    *s++ = 0x68; memcpy(s, &t, 4); s += 4;
    *s++ = 0xe8;
    { int32_t rel = (int32_t)((uintptr_t)pe_tls_array - (uintptr_t)(s + 4));
      memcpy(s, &rel, 4); s += 4; }
    *s++ = 0x83; *s++ = 0xc4; *s++ = 0x04;
    *s++ = 0x5a; *s++ = 0x59; *s++ = 0x9d; *s++ = 0xc3;
    pe_setprot(t->stub, 0x1000, 1, 0);
    for (int i = 0; i < fh->nsec; ++i) {
      unsigned char *c = base + sec[i].vaddr;
      if (!(sec[i].chars & 0x20000000) || sec[i].vsize < sizeof site) continue;
      for (size_t k = 0; k + sizeof site <= sec[i].vsize; ++k) {
        int32_t rel;
        if (memcmp(c + k, site, sizeof site)) continue;
        rel = (int32_t)((uintptr_t)t->stub - (uintptr_t)(c + k + 5));
        c[k] = 0xe8; memcpy(c + k + 1, &rel, 4); c[k + 5] = 0x90;
        k += sizeof site - 1;
        ++sites;
      }
    }
    if (td->index_addr) *(uint32_t *)(uintptr_t)td->index_addr = 0;
    img->tls = t;
    if (getenv("PE_TRACE"))
      fprintf(stderr, "pe: TLS through a stub, %d sites, block %u bytes\n",
              sites, (unsigned)t->total);
  }
#endif

  /* SCVA_DUMP_TEXT writes the patched code out so a disassembler can confirm
     that nothing outside SSE1 survived either pass. */
  {
    const char *dump = getenv("SCVA_DUMP_TEXT");
    if (dump) {
      FILE *f = fopen(dump, "wb");
      if (f) {
        for (int k = 0; k < fh->nsec; ++k)
          if (sec[k].chars & 0x20000000)
            fwrite(base + sec[k].vaddr, 1, sec[k].vsize, f);
        fclose(f);
        fprintf(stderr, "pe: patched code written to %s\n", dump);
      }
    }
  }

  /* protections: everything executable-and-readable is simplest and safe here */
  for (int i = 0; i < fh->nsec; ++i) {
    size_t len = (sec[i].vsize + 0xfff) & ~(size_t)0xfff;
    pe_setprot(base + (sec[i].vaddr & ~(size_t)0xfff), len,
               (sec[i].chars & 0x20000000) != 0,
               (sec[i].chars & 0x80000000) != 0);
  }
  pe_setprot(base + oh->imagesz, SCVA_SHIM_SPACE, 1, 0);

  if (getenv("PE_TRACE")) fprintf(stderr, "pe: %s\n", "protections set");
  /* Thread-local storage. The image reads gs:[0x58] for its TLS pointer array
     and indexes it: without this the very first thread-local access is
     `mov (%rax,%rcx,8),%rbx` with rax = 0, which is where the first native
     TG_Process died. 32-bit Windows is handled above, before the code
     was made read-only. */
#if defined(_WIN32) && !defined(__i386__)
  if (oh->dirs[9].size)
    FAIL("this core wants thread-local storage, which is not handled here");
#elif defined(_WIN32)
  if (oh->dirs[9].size) {
    uint32_t *cb = (uint32_t *)(uintptr_t)
      ((uint32_t *)(base + oh->dirs[9].rva))[3];
    while (cb && *cb) {
      WINAPI_CC void (*f)(void *, uint32_t, void *) = (void *)(uintptr_t)*cb;
      f(base, 1, NULL);
      ++cb;
    }
  }
#else
  if (oh->dirs[9].size) {
    struct {
      thunk_t start, end, index_addr, callbacks;
      uint32_t zerofill, chars;
    } *tls = (void *)(base + oh->dirs[9].rva);
    size_t raw = (size_t)(tls->end - tls->start);
    size_t total = raw + tls->zerofill;
    unsigned char *blockmem = calloc(1, total ? total : 1);
    uint32_t idx;
    if (!blockmem) FAIL("out of memory for TLS");
    if (!g_tls_slots) {
      g_tls_slots = calloc(PE_TLS_SLOTS, sizeof *g_tls_slots);
      if (!g_tls_slots) FAIL("out of memory for TLS");
      *(void **)(t_teb + PE_TIB_TLS) = g_tls_slots;
    }
    if (g_tls_nfree > 0) idx = (uint32_t)g_tls_free[--g_tls_nfree];
    else if (g_tls_next < PE_TLS_SLOTS) idx = g_tls_next++;
    else FAIL("out of TLS slots");
    img->tls_index = (int)idx;
    if (raw) memcpy(blockmem, (void *)(uintptr_t)tls->start, raw);
    g_tls_slots[idx] = blockmem;
    if (tls->index_addr) *(uint32_t *)(uintptr_t)tls->index_addr = idx;
    if (getenv("PE_TRACE"))
      fprintf(stderr, "pe: TLS slot %u, block %zu bytes (%zu raw + %u zero)\n",
              idx, total, raw, tls->zerofill);
    if (tls->callbacks) {
      thunk_t *cb = (thunk_t *)(uintptr_t)tls->callbacks;
      while (*cb) {
        /* PIMAGE_TLS_CALLBACK is WINAPI: stdcall on i386. */
        WINAPI_CC void (*f)(void *, uint32_t, void *) = (void *)(uintptr_t)*cb;
        f(base, 1, NULL);
        ++cb;
      }
    }
  }
#endif

  img->base = base; img->size = mapsz; img->entry = oh->entry;
  if (oh->entry) {
    /* DllMain is WINAPI: stdcall on i386, so the callee clears the arguments
       and the caller must not. */
    WINAPI_CC int (*dllmain)(void *, uint32_t, void *) =
      (void *)(base + oh->entry);
    if (getenv("PE_TRACE")) fprintf(stderr, "pe: calling DllMain at +0x%llx\n", (unsigned long long)oh->entry);
    if (!dllmain(base, 1 /* DLL_PROCESS_ATTACH */, NULL))
      FAIL("DllMain returned FALSE");
  }
  free(g_file);
  return img;
#undef FAIL
}

void pe_unload(struct pe_image *img)
{
  if (!img) return;
#ifndef _WIN32
  if (img->tls_index >= 0 && g_tls_slots) {
    free(g_tls_slots[img->tls_index]);
    g_tls_slots[img->tls_index] = NULL;
    if (g_tls_nfree < PE_TLS_SLOTS) g_tls_free[g_tls_nfree++] = img->tls_index;
  }
#endif
#if defined(_WIN32) && defined(__i386__)
  if (img->tls) {
    struct pe_tls *t = img->tls;
    pe_release(t->stub, 0x1000);
    free(t->tmpl);
    free(t);
  }
#endif
  if (img->base) pe_release(img->base, img->size);
  free(img);
}
