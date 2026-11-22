/* SPDX-License-Identifier: CC0-1.0
 *
 * cpu_probe.c: an instruction the system refuses is caught and reported, and
 * the caller carries on as if the probe had returned.
 */
#include "check.h"
#include "cpu_probe.h"

void probe_ret(void) __asm__("scva_test_probe_ret");
void probe_ud2(void) __asm__("scva_test_probe_ud2");
__asm__(".text\n"
        "scva_test_probe_ret:\n\t"
        "ret\n"
        "scva_test_probe_ud2:\n\t"
        "ud2\n\t"
        "ret");

TEST(a_function_that_returns_runs_cleanly)
{
  CHECK_INT(cpu_runs_cleanly(probe_ret), 1);
}

/* Only Windows catches the fault; elsewhere it would end the process. */
TEST(an_illegal_instruction_is_caught_and_reported)
{
#if defined(_WIN32) && defined(__i386__)
  CHECK_INT(cpu_runs_cleanly(probe_ud2), 0);
#else
  SKIP("faults are caught on 32-bit Windows only");
#endif
}

TEST(the_caller_carries_on_after_a_fault)
{
#if defined(_WIN32) && defined(__i386__)
  volatile int sentinel = 0x5ca1ab1e;
  int i, caught = 0;
  for (i = 0; i < 3; ++i) caught += !cpu_runs_cleanly(probe_ud2);
  CHECK_INT(caught, 3);
  CHECK_INT(sentinel, 0x5ca1ab1e);
  CHECK_INT(cpu_runs_cleanly(probe_ret), 1);
#else
  SKIP("faults are caught on 32-bit Windows only");
#endif
}

TEST(sse_runs_where_the_os_saves_its_state)
{
  CHECK_INT(cpu_os_runs_sse(), 1);
}

int main(void)
{
  RUN(a_function_that_returns_runs_cleanly);
  RUN(an_illegal_instruction_is_caught_and_reported);
  RUN(the_caller_carries_on_after_a_fault);
  RUN(sse_runs_where_the_os_saves_its_state);
  return test_summary();
}
