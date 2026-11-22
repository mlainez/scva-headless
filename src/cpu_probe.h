/* SPDX-License-Identifier: CC0-1.0
 *
 * What the running system lets an x86-32 program execute, found by trying it.
 */
#ifndef SCVA_CPU_PROBE_H
#define SCVA_CPU_PROBE_H

/* 1 if fn returns normally, 0 if it raises an illegal-instruction or
   privileged-instruction fault, which is caught. fn must not touch ebx, esi,
   edi or ebp. Windows only; elsewhere it calls fn and returns 1. */
int cpu_runs_cleanly(void (*fn)(void));

/* 1 if SSE instructions execute: the CPU has them and the OS saves their
   state. Windows 95 never does, so there an SSE instruction faults even on a
   CPU that has SSE. Linux always does. */
int cpu_os_runs_sse(void);

#endif
