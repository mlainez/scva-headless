/* SPDX-License-Identifier: CC0-1.0
 *
 * The conversions the core uses that SSE1 has no instruction for. Each is
 * three or four bytes, too short for a jump, so it is merged with a
 * neighbouring SSE1 instruction into a span of five or more; the stub
 * replays that neighbour verbatim around the x87 sequence that does the
 * conversion. Generated, like sse2_sites.h, from one known core.
 */
#ifndef SCVA_SSE2_CVT_H
#define SCVA_SSE2_CVT_H

#define SCVA_SSE2_CVTS 41

/* form: 1 cvtps2pd, 2 cvtsd2ss, 3 cvtdq2ps, 4 cvtpd2ps */
static const struct { unsigned int rva; unsigned char span, at, len,
                     form, dreg, sreg; }
scva_sse2_cvt[SCVA_SSE2_CVTS] = {
  {0x4dd7b, 7,0,3,3,0,0},
  {0x4dfbe, 7,0,3,3,0,0},
  {0x4e1fb, 7,0,3,3,0,0},
  {0x4e58d, 7,0,3,3,0,0},
  {0x4e7d9, 7,0,3,3,0,0},
  {0x4ea18, 7,0,3,3,0,0},
  {0x716a2, 7,4,3,1,1,1},
  {0x716b7, 7,0,4,2,2,0},
  {0x71852, 7,4,3,1,1,1},
  {0x7186c, 8,0,4,2,0,0},
  {0x72032, 7,4,3,1,1,1},
  {0x72047, 7,0,4,2,2,0},
  {0x721d2, 7,4,3,1,1,1},
  {0x721e4, 8,0,4,2,0,0},
  {0x73ba6, 7,4,3,1,1,1},
  {0x73bbb, 7,0,4,2,2,0},
  {0x73cfb, 7,4,3,1,1,1},
  {0x73d10, 7,0,4,2,2,0},
  {0x73e4b, 7,4,3,1,1,1},
  {0x73e60, 7,0,4,2,2,0},
  {0x73f8e, 7,4,3,1,1,1},
  {0x73fa3, 7,0,4,2,2,0},
  {0x740db, 7,4,3,1,1,1},
  {0x740f0, 7,0,4,2,2,0},
  {0x7421e, 7,4,3,1,1,1},
  {0x74233, 7,0,4,2,2,0},
  {0x743c6, 7,4,3,1,1,1},
  {0x743e0, 8,0,4,2,0,0},
  {0x7450d, 7,4,3,1,1,1},
  {0x7451f, 8,0,4,2,0,0},
  {0x7464c, 7,4,3,1,1,1},
  {0x7465e, 8,0,4,2,0,0},
  {0x7478b, 7,4,3,1,1,1},
  {0x7479d, 8,0,4,2,0,0},
  {0x748ce, 7,4,3,1,1,1},
  {0x748e0, 8,0,4,2,0,0},
  {0x74a0b, 7,4,3,1,1,1},
  {0x74a1d, 8,0,4,2,0,0},
  {0x9887e,12,0,4,4,0,0},
  {0x98ad8,10,0,4,2,0,0},
  {0x98b15, 8,0,4,2,0,0},
};
#endif
