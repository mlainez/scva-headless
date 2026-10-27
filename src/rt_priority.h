/* SPDX-License-Identifier: CC0-1.0
 *
 * Asking the system to let the audio loop run on time. A page fault or a
 * scheduler delay in the middle of a block is heard as a dropout.
 */
#ifndef RT_PRIORITY_H
#define RT_PRIORITY_H

/* Raises the calling thread as far as the system allows, best-effort and
   silently. Returns the realtime priority obtained, 0 if there is none to
   report. */
int rt_priority_raise(void);
#endif
