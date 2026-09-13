/* SPDX-License-Identifier: CC0-1.0  -- see pe_loader.h */
#define _GNU_SOURCE
#include "pe_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <asm/prctl.h>
#include <sys/syscall.h>

/* Windows x86-64 reads its thread block through GS. Linux keeps its own TLS in
   FS and leaves GS alone, so GS can be pointed at a block we build - which is
   what makes running this image without wine possible at all. Without it the
   image's own CRT start-up faults on the first gs:[..] it touches. */
static unsigned char *g_teb;
static unsigned char *g_peb;

static int install_teb(void)
{
  size_t sz = 0x2000;
  if (g_teb) return 1;
  g_teb = mmap(NULL, sz, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  g_peb = mmap(NULL, sz, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (g_teb == MAP_FAILED || g_peb == MAP_FAILED) return 0;
  memset(g_teb, 0, sz); memset(g_peb, 0, sz);
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
    *(void **)(g_teb + 0x00) = (void *)~(uintptr_t)0; /* ExceptionList = -1 */
    *(void **)(g_teb + 0x08) = (void *)((uintptr_t)stack_addr + stack_sz); /* StackBase */
    *(void **)(g_teb + 0x10) = stack_addr;                                 /* StackLimit */
    *(void **)(g_teb + 0x30) = g_teb;                 /* Self          */
    *(void **)(g_teb + 0x60) = g_peb;                 /* ProcessEnvironmentBlock */
    *(uint32_t *)(g_teb + 0x40) = 1;                  /* ClientId.Process */
    *(uint32_t *)(g_teb + 0x48) = 1;                  /* ClientId.Thread  */
    *(uint32_t *)(g_peb + 0x00) = 0;                  /* InheritedAddressSpace */
    *(uint32_t *)(g_peb + 0x118) = 10;                /* OSMajorVersion */
    *(uint32_t *)(g_peb + 0x11c) = 0;                 /* OSMinorVersion */
  }
  if (syscall(SYS_arch_prctl, ARCH_SET_GS, (unsigned long)g_teb) != 0) return 0;
  return 1;
}

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

/* Real locks and condition variables. The image asks for the condition
   variable trio by name through GetProcAddress and calls what it is given, so
   they must resolve. A Windows CRITICAL_SECTION has room for a pthread mutex;
   a CONDITION_VARIABLE is a single pointer, so it holds one we allocate. */
typedef struct { long long a, b, c, d, e; } CRIT;

static MSABI void ms_InitializeCriticalSectionAndSpinCount(CRIT *c, uint32_t n)
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
static MSABI void ms_DeleteCriticalSection(CRIT *c)
{ if (c) pthread_mutex_destroy((pthread_mutex_t *)c); }
static MSABI void ms_EnterCriticalSection(CRIT *c)
{ if (c) pthread_mutex_lock((pthread_mutex_t *)c); }
static MSABI void ms_LeaveCriticalSection(CRIT *c)
{ if (c) pthread_mutex_unlock((pthread_mutex_t *)c); }

static MSABI void ms_InitializeConditionVariable(void **cv)
{
  pthread_cond_t *c;
  if (!cv) return;
  c = calloc(1, sizeof *c);
  if (c) pthread_cond_init(c, NULL);
  *cv = c;
}
static MSABI int ms_SleepConditionVariableCS(void **cv, CRIT *cs, uint32_t ms)
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
static MSABI void ms_WakeAllConditionVariable(void **cv)
{ if (cv && *cv) pthread_cond_broadcast((pthread_cond_t *)*cv); }
static MSABI void ms_WakeConditionVariable(void **cv)
{ if (cv && *cv) pthread_cond_signal((pthread_cond_t *)*cv); }
static MSABI void *ms_CreateEventW(void *a, int b, int c, void *d)
{ (void)a;(void)b;(void)c;(void)d; return (void *)0x1000; }
static MSABI int   ms_SetEvent(void *h) { (void)h; return 1; }
static MSABI int   ms_ResetEvent(void *h) { (void)h; return 1; }
static MSABI int   ms_CloseHandle(void *h) { (void)h; return 1; }
static MSABI uint32_t ms_WaitForSingleObjectEx(void *h, uint32_t ms, int alert)
{ (void)h;(void)ms;(void)alert; return 0; }
static MSABI void *ms_GetCurrentProcess(void) { return (void *)-1; }
static MSABI uint32_t ms_GetCurrentProcessId(void) { return 1; }
static MSABI uint32_t ms_GetCurrentThreadId(void) { return 1; }
static MSABI void *ms_GetModuleHandleW(const uint16_t *n)
{
  if (getenv("PE_TRACE") && n) {
    char b[128]; int i = 0;
    while (n[i] && i < 127) { b[i] = (char)n[i]; ++i; }
    b[i] = 0;
    fprintf(stderr, "pe: GetModuleHandleW(\"%s\")\n", b);
  } else if (getenv("PE_TRACE")) fprintf(stderr, "pe: GetModuleHandleW(NULL)\n");
  return (void *)0x2000;
}
static void *bind(const char *name);
static MSABI void *ms_GetProcAddress(void *m, const char *n)
{
  void *r = n ? bind(n) : NULL;
  if (getenv("PE_TRACE"))
    fprintf(stderr, "pe: GetProcAddress(%p, \"%s\") -> %s\n",
            m, n ? n : "(ordinal)", r ? "ok" : "NULL");
  return r;
}
static MSABI int   ms_IsDebuggerPresent(void) { return 0; }
static MSABI int   ms_IsProcessorFeaturePresent(uint32_t f) { (void)f; return 1; }
static MSABI void  ms_InitializeSListHead(void *p) { if (p) memset(p, 0, 16); }
static MSABI void *ms_SetUnhandledExceptionFilter(void *f) { (void)f; return NULL; }
static MSABI long  ms_UnhandledExceptionFilter(void *p) { (void)p; return 1; }
static MSABI void  ms_TerminateProcess(void *h, uint32_t c)
{ (void)h; fprintf(stderr, "pe: image called TerminateProcess(%u)\n", c); _exit((int)c); }
static MSABI int ms_QueryPerformanceCounter(int64_t *v)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  if (v) *v = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
  return 1;
}
static MSABI void ms_GetSystemTimeAsFileTime(uint64_t *ft)
{ struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
  if (ft) *ft = ((uint64_t)t.tv_sec + 11644473600ULL) * 10000000ULL + t.tv_nsec / 100; }
static MSABI void ms_GetLocalTime(uint16_t *st)
{ if (st) memset(st, 0, 16); }
/* unwinding: only reached if the image throws, which it must not */
static MSABI void  ms_RtlCaptureContext(void *c) { if (c) memset(c, 0, 1232); }
static MSABI void *ms_RtlLookupFunctionEntry(uint64_t pc, uint64_t *base, void *hist)
{ (void)pc;(void)hist; if (base) *base = 0; return NULL; }
static MSABI void  ms_RtlVirtualUnwind(uint32_t a, uint64_t b, uint64_t c, void *d,
                                       void *e, void *f, void *g, void *h)
{ (void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h; }

/* Zeroed: Windows hands back a fresh page where glibc recycles a dirty one. */
static MSABI void *ms_malloc(size_t n) { return calloc(1, n ? n : 1); }
static MSABI void  ms_free(void *p) { free(p); }
static MSABI int   ms_callnewh(size_t n) { (void)n; return 0; }
/* ms_abi makes XMM6-XMM15, RDI and RSI callee-saved where System V does not;
   GCC spills them around the call into glibc, so no thunk is needed. */
static MSABI void *ms_memcpy(void *d, const void *s, size_t n) { return memcpy(d, s, n); }
static MSABI void *ms_memset(void *d, int c, size_t n) { return memset(d, c, n); }

static MSABI void ms_CxxThrowException(void *a, void *b)
{ (void)a;(void)b; fprintf(stderr, "pe: the image threw a C++ exception\n"); abort(); }
static MSABI long ms_C_specific_handler(void *a, void *b, void *c, void *d)
{ (void)a;(void)b;(void)c;(void)d; return 1; }
static MSABI long ms_CxxFrameHandler3(void *a, void *b, void *c, void *d)
{ (void)a;(void)b;(void)c;(void)d; return 1; }
static MSABI void ms_std_exception_copy(void *a, void *b) { (void)a;(void)b; }
static MSABI void ms_std_exception_destroy(void *a) { (void)a; }
static MSABI void ms_std_terminate(void) { fprintf(stderr, "pe: std::terminate\n"); abort(); }
static MSABI void ms_std_type_info_destroy_list(void *a) { (void)a; }

/* The tables hold function pointers INTO the image, so they are MS ABI, and
   both ends are inclusive-exclusive. Getting these wrong leaves the image's
   globals holding whatever the heap had, which shows up later as output that
   differs run to run. */
typedef MSABI void (*pvfv)(void);
typedef MSABI int  (*pifv)(void);

static MSABI void ms_initterm(pvfv *first, pvfv *last)
{
  int n = 0;
  if (!first || !last) return;
  for (; first < last; ++first)
    if (*first) { (*first)(); ++n; }
  if (getenv("PE_TRACE")) fprintf(stderr, "pe: _initterm ran %d initialisers\n", n);
}
static MSABI int ms_initterm_e(pifv *first, pifv *last)
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
static MSABI int  ms_crt_atexit(void *f) { (void)f; return 0; }
static MSABI int  ms_register_onexit_function(void *t, void *f) { (void)t;(void)f; return 0; }
static MSABI int  ms_initialize_onexit_table(void *t) { (void)t; return 0; }
static MSABI int  ms_execute_onexit_table(void *t) { (void)t; return 0; }
static MSABI int  ms_configure_narrow_argv(int m) { (void)m; return 0; }
static MSABI int  ms_initialize_narrow_environment(void) { return 0; }
static MSABI void ms_cexit(void) {}
static MSABI void ms_exit(int c) { _exit(c); }
static MSABI long ms_seh_filter_dll(uint32_t c, void *p) { (void)c;(void)p; return 0; }
static MSABI int ms_stdio_common_vsprintf(uint64_t opt, char *buf, size_t n,
                                          const char *fmt, void *loc, va_list ap)
{ (void)opt; (void)loc; return vsnprintf(buf, n ? n : 0, fmt, ap); }

struct binding { const char *name; void *fn; };
static const struct binding BINDINGS[] = {
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
  {"InitializeConditionVariable", ms_InitializeConditionVariable},
  {"SleepConditionVariableCS", ms_SleepConditionVariableCS},
  {"WakeAllConditionVariable", ms_WakeAllConditionVariable},
  {"WakeConditionVariable", ms_WakeConditionVariable},
  {NULL, NULL}
};

static void *bind(const char *name)
{
  int i;
  for (i = 0; BINDINGS[i].name; ++i)
    if (!strcmp(BINDINGS[i].name, name)) return BINDINGS[i].fn;
  return NULL;
}

/* ---------- the loader -------------------------------------------------- */
/* The TLS pointer array at gs:[0x58] belongs to the thread, not to an image,
   so it is allocated once and every image gets its own index - which is what
   TlsAlloc does on Windows. Without it a second image overwrites the first
   image's block and the two share thread-local state. */
static void **g_tls_slots;
static uint32_t g_tls_next;
#define PE_TLS_SLOTS 64

void *pe_symbol(struct pe_image *img, const char *name)
{
  struct dos *d = (struct dos *)img->base;
  unsigned char *nt = img->base + d->lfanew;
  struct fh *fh = (struct fh *)(nt + 4);
  struct oh64 *oh = (struct oh64 *)(nt + 4 + sizeof *fh);
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
  int fd = open(path, O_RDONLY);
  off_t fsz;
  unsigned char *g_file;
  struct pe_image *img = calloc(1, sizeof *img);
  if (!img) FAIL("out of memory");
  if (fd < 0) FAIL("cannot open %s", path);
  fsz = lseek(fd, 0, SEEK_END); lseek(fd, 0, SEEK_SET);
  g_file = mmap(NULL, (size_t)fsz, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (g_file == MAP_FAILED) FAIL("cannot map %s", path);

  if (!install_teb()) FAIL("cannot install a thread block on GS");
  if (getenv("PE_TRACE")) fprintf(stderr, "pe: GS points at a TEB\n");

  struct dos *d = (struct dos *)g_file;
  if (d->magic != 0x5a4d) FAIL("not a PE");
  unsigned char *nt = g_file + d->lfanew;
  if (memcmp(nt, "PE\0\0", 4)) FAIL("no PE signature");
  struct fh *fh = (struct fh *)(nt + 4);
  struct oh64 *oh = (struct oh64 *)(nt + 4 + sizeof *fh);
  if (fh->machine != 0x8664) FAIL("not x86-64");
  if (oh->magic != 0x20b) FAIL("not PE32+");

  /* Prefer the image's own base: then the relocation pass has nothing to do
     and cannot be the thing that is wrong. Falling back to anywhere is fine,
     but if behaviour differs between the two, the relocations are the suspect. */
  unsigned char *base = mmap((void *)(uintptr_t)oh->imagebase, oh->imagesz,
                             PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (base == MAP_FAILED || base != (unsigned char *)(uintptr_t)oh->imagebase) {
    if (base != MAP_FAILED) munmap(base, oh->imagesz);
    base = mmap(NULL, oh->imagesz, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (getenv("PE_TRACE")) fprintf(stderr, "pe: relocating (preferred base taken)\n");
  } else if (getenv("PE_TRACE")) {
    fprintf(stderr, "pe: mapped at its preferred base, no relocation needed\n");
  }
  if (base == MAP_FAILED) FAIL("cannot reserve %u bytes", oh->imagesz);
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
        if (type == 10) *(uint64_t *)(base + page + off) += (uint64_t)delta;
        else if (type != 0) FAIL("unhandled relocation type %d", type);
      }
      p += blk;
    }
  }

  if (getenv("PE_TRACE")) fprintf(stderr, "pe: %s\n", "relocations applied");
  /* imports */
  if (oh->dirs[1].size) {
    struct imp_desc *im = (struct imp_desc *)(base + oh->dirs[1].rva);
    for (; im->name; ++im) {
      uint64_t *oft = (uint64_t *)(base + (im->oft ? im->oft : im->ft));
      uint64_t *ft  = (uint64_t *)(base + im->ft);
      for (; *oft; ++oft, ++ft) {
        if (*oft >> 63) FAIL("%s imports by ordinal", (char *)(base + im->name));
        const char *nm = (const char *)(base + (*oft & 0xffffffff) + 2);
        void *fn = bind(nm);
        if (!fn) FAIL("no binding for %s (%s)", nm, (char *)(base + im->name));
        *ft = (uint64_t)(uintptr_t)fn;
      }
    }
  }

  if (getenv("PE_TRACE")) fprintf(stderr, "pe: %s\n", "imports bound");
  /* protections: everything executable-and-readable is simplest and safe here */
  for (int i = 0; i < fh->nsec; ++i) {
    int prot = PROT_READ;
    if (sec[i].chars & 0x20000000) prot |= PROT_EXEC;
    if (sec[i].chars & 0x80000000) prot |= PROT_WRITE;
    size_t len = (sec[i].vsize + 0xfff) & ~(size_t)0xfff;
    mprotect(base + (sec[i].vaddr & ~(size_t)0xfff), len, prot);
  }

  if (getenv("PE_TRACE")) fprintf(stderr, "pe: %s\n", "protections set");
  /* Thread-local storage. The image reads gs:[0x58] for its TLS pointer array
     and indexes it: without this the very first thread-local access is
     `mov (%rax,%rcx,8),%rbx` with rax = 0, which is where the first native
     TG_Process died. */
  if (oh->dirs[9].size) {
    struct {
      uint64_t start, end, index_addr, callbacks;
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
      *(void **)(g_teb + 0x58) = g_tls_slots;
    }
    if (g_tls_next >= PE_TLS_SLOTS) FAIL("out of TLS slots");
    idx = g_tls_next++;
    if (raw) memcpy(blockmem, (void *)(uintptr_t)tls->start, raw);
    g_tls_slots[idx] = blockmem;
    if (tls->index_addr) *(uint32_t *)(uintptr_t)tls->index_addr = idx;
    if (getenv("PE_TRACE"))
      fprintf(stderr, "pe: TLS slot %u, block %zu bytes (%zu raw + %u zero)\n",
              idx, total, raw, tls->zerofill);
    if (tls->callbacks) {
      uint64_t *cb = (uint64_t *)(uintptr_t)tls->callbacks;
      while (*cb) {
        MSABI void (*f)(void *, uint32_t, void *) = (void *)(uintptr_t)*cb;
        f(base, 1, NULL);
        ++cb;
      }
    }
  }

  img->base = base; img->size = oh->imagesz; img->entry = oh->entry;
  if (oh->entry) {
    MSABI int (*dllmain)(void *, uint32_t, void *) =
      (void *)(base + oh->entry);
    if (getenv("PE_TRACE")) fprintf(stderr, "pe: calling DllMain at +0x%llx\n", (unsigned long long)oh->entry);
    if (!dllmain(base, 1 /* DLL_PROCESS_ATTACH */, NULL))
      FAIL("DllMain returned FALSE");
  }
  munmap(g_file, (size_t)fsz);
  return img;
#undef FAIL
}

void pe_unload(struct pe_image *img) { (void)img; }
