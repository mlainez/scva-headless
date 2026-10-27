/* SPDX-License-Identifier: CC0-1.0
 *
 * See rt_priority.h.
 */
#include "rt_priority.h"

#ifdef _WIN32
#include <windows.h>

/* Under a busy-waiting program such as a DOS game in NTVDM, a
   normal-priority loop misses its turn and the device runs dry. */
int rt_priority_raise(void)
{
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  return 0;
}
#else
#include <sched.h>
#include <string.h>
#include <sys/mman.h>

/* Memory locked and SCHED_FIFO just above the minimum. An ordinary desktop
   grants neither by default. */
int rt_priority_raise(void)
{
  struct sched_param sp;
  mlockall(MCL_CURRENT | MCL_FUTURE);
  memset(&sp, 0, sizeof sp);
  sp.sched_priority = sched_get_priority_min(SCHED_FIFO) + 5;
  return sched_setscheduler(0, SCHED_FIFO, &sp) == 0 ? sp.sched_priority : 0;
}
#endif
