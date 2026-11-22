/* SPDX-License-Identifier: CC0-1.0
 *
 * See cpu_probe.h. The fault is caught with a structured exception handler
 * registered by hand on fs:[0], which every Windows from 95 on dispatches to,
 * in an executable or a DLL alike, whatever the host has installed.
 */
#include "cpu_probe.h"

#if defined(_WIN32) && defined(__i386__)
#include <windows.h>

/* The registration record Windows walks, followed by where to resume. */
struct probe_frame {
  struct probe_frame *next;
  void *handler;
  DWORD esp, eip;
  volatile int faulted;
};

static EXCEPTION_DISPOSITION __cdecl probe_handler(EXCEPTION_RECORD *rec, void *frame,
                                                   CONTEXT *ctx, void *dispatch)
{
  struct probe_frame *f = frame;
  (void)dispatch;
  if (rec->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
    return ExceptionContinueSearch;
  if (rec->ExceptionCode != EXCEPTION_ILLEGAL_INSTRUCTION &&
      rec->ExceptionCode != EXCEPTION_PRIV_INSTRUCTION)
    return ExceptionContinueSearch;
  f->faulted = 1;
  ctx->Esp = f->esp;
  ctx->Eip = f->eip;
  return ExceptionContinueExecution;
}

int cpu_runs_cleanly(void (*fn)(void))
{
  struct probe_frame f;
  f.handler = (void *)probe_handler;
  f.faulted = 0;
  __asm__ volatile("movl $1f, 12(%0)\n\t"        /* f.eip: resume after the call */
                   "movl %%esp, 8(%0)\n\t"       /* f.esp: the stack there */
                   "movl %%fs:0, %%eax\n\t"
                   "movl %%eax, (%0)\n\t"
                   "movl %0, %%fs:0\n\t"
                   "call *%1\n"
                   "1:\n\t"
                   "movl (%0), %%eax\n\t"
                   "movl %%eax, %%fs:0"
                   : : "r"(&f), "r"(fn)
                   : "eax", "ecx", "edx", "memory", "cc");
  return !f.faulted;
}

void cpu_probe_xorps(void) __asm__("cpu_probe_xorps");
__asm__(".text\n"
        "cpu_probe_xorps:\n\t"
        ".byte 0x0f, 0x57, 0xc0\n\t"             /* xorps xmm0, xmm0 */
        "ret");

int cpu_os_runs_sse(void) { return cpu_runs_cleanly(cpu_probe_xorps); }

#else

int cpu_runs_cleanly(void (*fn)(void)) { fn(); return 1; }
int cpu_os_runs_sse(void) { return 1; }

#endif
