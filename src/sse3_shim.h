/* SPDX-License-Identifier: CC0-1.0
 *
 * The core sums the four taps of its sample interpolation with a pair of
 * haddps, which is SSE3: a Prescott Pentium 4 or a revision E Athlon 64 at
 * the oldest. Nothing in the core asks cpuid first, and the pair sits in the
 * voice render path, so an older processor faults on the first note. Both
 * cores do it, at six sites each, and nothing in either needs SSSE3 or SSE4.
 *
 * Written in SSE1 the same sum pairs the lanes the same way - (x0+x1) and
 * (x2+x3), then those two - so it rounds identically and the audio does not
 * change. It does not fit in the bytes the pattern occupies, so each site
 * becomes a call to a stub that performs the whole span.
 *
 * The two cores spell the span differently:
 *
 *   32-bit, 13 bytes        haddps xmm0,xmm0            f2 0f 7c c0
 *                           movss  [ebp-0x30],xmm2      f3 0f 11 55 d0
 *                           haddps xmm0,xmm0            f2 0f 7c c0
 *
 *   64-bit, 8 bytes         haddps xmm0,xmm0            f2 0f 7c c0
 *                           haddps xmm0,xmm0            f2 0f 7c c0
 *
 * The 32-bit store spills xmm2 into the slot the following mulss reads, which
 * leaves xmm2 free as scratch and lets that stub put it back from the same
 * slot. The 64-bit span has no such gift, so its stub borrows xmm1 and saves
 * it on the stack; the save is movups because a call leaves rsp 8 modulo 16.
 */
#ifndef SCVA_SSE3_SHIM_H
#define SCVA_SSE3_SHIM_H
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* room for both stubs, well inside one page */
#define SCVA_SSE3_STUB_SIZE 64

/* ECX bit 0 of cpuid leaf 1. */
static int scva_cpu_has_sse3(void)
{
#if defined(__i386__) || defined(__x86_64__)
  unsigned int a = 1, b = 0, c = 0, d = 0;
#if defined(__i386__) && defined(__PIC__)
  /* ebx is the PIC register, so it is swapped out around the instruction */
  __asm__ volatile("xchgl %%ebx, %1\n\tcpuid\n\txchgl %%ebx, %1"
                   : "+a"(a), "=r"(b), "=c"(c), "=d"(d));
#else
  __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
#endif
  (void)a; (void)b; (void)d;
  return (c & 1u) != 0;
#else
  return 1;
#endif
}

struct scva_sse3_site {
  const unsigned char *pattern;
  size_t pattern_len;
  const unsigned char *stub;
  size_t stub_len;
  size_t stub_at;               /* where this stub sits in the stub page */
};

/* haddps xmm0,xmm0 / movss [ebp-0x30],xmm2 / haddps xmm0,xmm0 */
static const unsigned char scva_sse3_pat32[13] = {
  0xf2,0x0f,0x7c,0xc0,  0xf3,0x0f,0x11,0x55,0xd0,  0xf2,0x0f,0x7c,0xc0
};
/* movss [ebp-0x30],xmm2 / movaps xmm2,xmm0 / shufps xmm2,xmm0,0xb1 /
   addps xmm0,xmm2 / movhlps xmm2,xmm0 / addss xmm0,xmm2 /
   movss xmm2,[ebp-0x30] / ret */
static const unsigned char scva_sse3_stub32[28] = {
  0xf3,0x0f,0x11,0x55,0xd0,  0x0f,0x28,0xd0,  0x0f,0xc6,0xd0,0xb1,
  0x0f,0x58,0xc2,  0x0f,0x12,0xd0,  0xf3,0x0f,0x58,0xc2,
  0xf3,0x0f,0x10,0x55,0xd0,  0xc3
};

/* haddps xmm0,xmm0 / haddps xmm0,xmm0 */
static const unsigned char scva_sse3_pat64[8] = {
  0xf2,0x0f,0x7c,0xc0,  0xf2,0x0f,0x7c,0xc0
};
/* lea rsp,[rsp-0x18] / movups [rsp],xmm1 / movaps xmm1,xmm0 /
   shufps xmm1,xmm0,0xb1 / addps xmm0,xmm1 / movhlps xmm1,xmm0 /
   addss xmm0,xmm1 / movups xmm1,[rsp] / lea rsp,[rsp+0x18] / ret
   lea rather than sub and add, so that EFLAGS crosses the stub untouched */
static const unsigned char scva_sse3_stub64[36] = {
  0x48,0x8d,0x64,0x24,0xe8,  0x0f,0x11,0x0c,0x24,  0x0f,0x28,0xc8,
  0x0f,0xc6,0xc8,0xb1,  0x0f,0x58,0xc1,  0x0f,0x12,0xc8,
  0xf3,0x0f,0x58,0xc1,  0x0f,0x10,0x0c,0x24,  0x48,0x8d,0x64,0x24,0x18,  0xc3
};

/* Rewrites every occurrence in [text, text+n) into a call to its stub. The
   stub page holds SCVA_SSE3_STUB_SIZE executable bytes and must lie within
   2 GB of the code. Returns how many sites were patched. */
static int scva_sse3_patch(unsigned char *text, size_t n, unsigned char *stubs)
{
  static const struct scva_sse3_site sites[2] = {
    { scva_sse3_pat32, sizeof scva_sse3_pat32,
      scva_sse3_stub32, sizeof scva_sse3_stub32, 0 },
    { scva_sse3_pat64, sizeof scva_sse3_pat64,
      scva_sse3_stub64, sizeof scva_sse3_stub64, 30 }
  };
  size_t i;
  int which, hits = 0;

  if (!text || !stubs) return 0;
  for (which = 0; which < 2; ++which)
    memcpy(stubs + sites[which].stub_at, sites[which].stub,
           sites[which].stub_len);

  /* longest pattern first: the 13-byte span contains two haddps that the
     8-byte one must not claim */
  for (which = 0; which < 2; ++which) {
    const struct scva_sse3_site *s = &sites[which];
    unsigned char *stub = stubs + s->stub_at;
    if (n < s->pattern_len) continue;
    for (i = 0; i + s->pattern_len <= n; ++i) {
      long long rel;
      if (memcmp(text + i, s->pattern, s->pattern_len) != 0) continue;
      rel = (long long)(stub - (text + i + 5));
      if (rel < -2147483647LL || rel > 2147483647LL) continue;
      text[i] = 0xe8;                                  /* call rel32 */
      text[i + 1] = (unsigned char)(rel & 0xff);
      text[i + 2] = (unsigned char)((rel >> 8) & 0xff);
      text[i + 3] = (unsigned char)((rel >> 16) & 0xff);
      text[i + 4] = (unsigned char)((rel >> 24) & 0xff);
      memset(text + i + 5, 0x90, s->pattern_len - 5);   /* nop the remainder */
      i += s->pattern_len - 1;
      ++hits;
    }
  }
  return hits;
}

#if defined(_WIN32) && defined(SCVA_SHIM_HOST_APPLY)
/* The Windows hosts hand the core to LoadLibrary rather than mapping it
   themselves, so the code arrives already protected and the patch has to open
   it. windows.h is included by the host before this header. */
static int scva_sse3_patch_module(void *module)
{
  unsigned char *base = (unsigned char *)module;
  unsigned char *nt, *sec, *stubs;
  unsigned short nsec, optsz;
  DWORD old;
  int i, hits = 0;

  if (!base || base[0] != 'M' || base[1] != 'Z') return 0;
  nt = base + *(const int *)(base + 0x3c);
  if (nt[0] != 'P' || nt[1] != 'E') return 0;
  nsec  = *(const unsigned short *)(nt + 6);
  optsz = *(const unsigned short *)(nt + 20);
  sec   = nt + 24 + optsz;

  stubs = (unsigned char *)VirtualAlloc(NULL, SCVA_SSE3_STUB_SIZE,
            MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
  if (!stubs) return 0;

  for (i = 0; i < (int)nsec; ++i) {
    const unsigned char *e = sec + i * 40;
    unsigned int vsize = *(const unsigned int *)(e + 8);
    unsigned int vaddr = *(const unsigned int *)(e + 12);
    unsigned int chars = *(const unsigned int *)(e + 36);
    if (!(chars & 0x20000000) || !vsize) continue;      /* code only */
    if (!VirtualProtect(base + vaddr, vsize, PAGE_EXECUTE_READWRITE, &old))
      continue;
    hits += scva_sse3_patch(base + vaddr, vsize, stubs);
    VirtualProtect(base + vaddr, vsize, old, &old);
  }
  return hits;
}

/* SCVA_SSE3 forces the shim on (1) or off (0); otherwise the processor
   decides. Returns how many sites were patched. */
static int scva_sse3_apply(void *module)
{
  const char *force = getenv("SCVA_SSE3");
  int want = force ? (*force != '0') : !scva_cpu_has_sse3();
  return want ? scva_sse3_patch_module(module) : 0;
}
#endif
#endif
