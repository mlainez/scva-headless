/* SPDX-License-Identifier: CC0-1.0
 *
 * scva_runtime.c: what the core is bound to keeps the conventions the core
 * calls it with. The 32-bit core calls __libm_sse2_pow as MSVC's own libm
 * defines it, in the SSE registers.
 */
#include "check.h"
#include "scva_runtime.h"

#if defined(__i386__)
/* f(x, y) the way the core calls it: x in xmm0, y in xmm1, the result read
   from xmm0. */
double call_in_xmm(void *f, double x, double y) __asm__("scva_test_call_in_xmm");
__asm__(".text\n"
        "scva_test_call_in_xmm:\n\t"
        "movlps 8(%esp), %xmm0\n\t"
        "movlps 16(%esp), %xmm1\n\t"
        "sub $12, %esp\n\t"
        "call *16(%esp)\n\t"
        "movlps %xmm0, (%esp)\n\t"
        "fldl (%esp)\n\t"
        "add $12, %esp\n\t"
        "ret");

/* 0xffff when every x87 register is empty. */
static unsigned x87_tags(void)
{
  struct { unsigned cw, sw, tw, ip, cs, dp, ds; } env;
  __asm__ volatile("fnstenv %0\n\tfldenv %0" : "=m"(env));
  return env.tw & 0xffff;
}

static void *bound_pow(void)
{
  return scva_runtime_resolve(NULL, "MSVCR100.dll", "__libm_sse2_pow");
}

TEST(pow_takes_its_arguments_and_returns_in_the_sse_registers)
{
  void *f = bound_pow();
  CHECK(f != NULL);
  if (!f) return;
  CHECK(call_in_xmm(f, 2.0, 10.0) == 1024.0);
  CHECK(call_in_xmm(f, 2.0, -3.0) == 0.125);
  CHECK(call_in_xmm(f, 10.0, 0.0) == 1.0);
}

TEST(pow_leaves_the_x87_stack_as_it_found_it)
{
  void *f = bound_pow();
  int i;
  CHECK(f != NULL);
  if (!f) return;
  CHECK_INT(x87_tags(), 0xffff);
  for (i = 0; i < 16; ++i) call_in_xmm(f, 2.0, (double)i);
  CHECK_INT(x87_tags(), 0xffff);
}
#else
TEST(pow_takes_its_arguments_and_returns_in_the_sse_registers)
{
  SKIP("the 32-bit core's binding");
}

TEST(pow_leaves_the_x87_stack_as_it_found_it)
{
  SKIP("the 32-bit core's binding");
}
#endif

int main(void)
{
  RUN(pow_takes_its_arguments_and_returns_in_the_sse_registers);
  RUN(pow_leaves_the_x87_stack_as_it_found_it);
  return test_summary();
}
