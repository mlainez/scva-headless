/* SPDX-License-Identifier: CC0-1.0 */
/* A diagnostic, not part of the renderer.
 *
 * Reports where the engine's two ring buffers are and how far apart. They must
 * be at least a block apart: TG_Process drains them into the two output
 * pointers with the same index and the same count, so rings that overlap put
 * the left channel into the right one, offset by the separation.
 *
 * --order varies the initialisation sequence one call at a time
 *   (r = setSampleRate, b = setMaxBlockSize, c = XPsetSystemConfig)
 * and reports whether the result is finite, per channel.
 *
 * --poison fills the resampler's two input buffers with a value the engine
 * cannot produce and counts how many slots still hold it afterwards. A slot
 * still poisoned was never written. Zero is what a healthy buffer looks like.
 *
 * The addresses below were read from the instruction stream of TG_Process and
 * of the resampler it calls - which globals they load before they copy - so
 * they are offsets, not algorithms. Loading the core natively is what makes
 * reading them possible.
 *
 *   build/ring-probe [--rate HZ] [--blocks N] [--block N] [--maxblock N]
 *                    [--order rbcr] [--poison]
 */
#define _GNU_SOURCE
#include "pe_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* image-relative; the image maps at its preferred base 0x180000000 */
#define RVA_AVAIL   0x1a62cbc   /* int32: samples resampled into the rings  */
#define RVA_READIDX 0x1a62cd0   /* int32: how far the last drain got        */
#define RVA_SRC_L   0x1a18f70   /* 32 floats: resampler input -> left  out  */
#define RVA_SRC_R   0x1a18ff0   /* 32 floats: resampler input -> right out  */
#define SRC_N       32

typedef MSABI int (*tg_initialize_fn)(int);
typedef MSABI void (*tg_set_sample_rate_fn)(float);
typedef MSABI void (*tg_set_max_block_fn)(int);
typedef MSABI int (*tg_activate_fn)(int, int);
typedef MSABI void (*tg_short_midi_fn)(unsigned int);
typedef MSABI void (*tg_long_midi_fn)(const unsigned char *, int);
typedef MSABI void (*tg_process_fn)(float *, float *, int);
struct tg_system_config { int a, b; };
typedef MSABI int (*tg_set_config_fn)(const struct tg_system_config *);

static float poison_value(void)
{
  /* a bit pattern no audio path produces */
  uint32_t bits = 0xDEADBEEFu;
  float f;
  memcpy(&f, &bits, sizeof f);
  return f;
}

static int poisoned(float f)
{
  uint32_t bits;
  memcpy(&bits, &f, sizeof bits);
  return bits == 0xDEADBEEFu;
}

int main(int argc, char **argv)
{
  const char *core = "dll/SCCore.dll";
  double rate = 48000.0;
  int nblocks = 40, block = 256, maxblock = 4096, i;
  const char *order = "rbcr";
  int do_poison = 0;
  static const unsigned char gs_reset[] = {
    0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
  struct pe_image *img;
  char err[256];
  tg_initialize_fn initialize; tg_set_sample_rate_fn set_rate;
  tg_set_max_block_fn set_block; tg_activate_fn activate;
  tg_short_midi_fn shortmidi; tg_long_midi_fn longmidi;
  tg_process_fn process; tg_set_config_fn set_config;
  volatile int32_t *avail, *readidx;
  float *src_l, *src_r, *left, *right;
  float poison = poison_value();
  int total_l = 0, total_r = 0;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = atof(argv[++i]);
    else if (!strcmp(argv[i], "--blocks") && i + 1 < argc) nblocks = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--maxblock") && i + 1 < argc) maxblock = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--order") && i + 1 < argc) order = argv[++i];
    else if (!strcmp(argv[i], "--poison")) do_poison = 1;
  }

  img = pe_load(core, err, sizeof err);
  if (!img) { fprintf(stderr, "pe_load: %s\n", err); return 1; }
#define GET(v, t, n) v = (t)pe_symbol(img, n); \
  if (!v) { fprintf(stderr, "missing %s\n", n); return 1; }
  GET(initialize, tg_initialize_fn, "TG_initialize")
  GET(set_rate, tg_set_sample_rate_fn, "TG_setSampleRate")
  GET(set_block, tg_set_max_block_fn, "TG_setMaxBlockSize")
  GET(activate, tg_activate_fn, "TG_activate")
  GET(shortmidi, tg_short_midi_fn, "TG_ShortMidiIn")
  GET(longmidi, tg_long_midi_fn, "TG_LongMidiIn")
  GET(process, tg_process_fn, "TG_Process")
  GET(set_config, tg_set_config_fn, "TG_XPsetSystemConfig")
#undef GET

  avail   = (volatile int32_t *)(img->base + RVA_AVAIL);
  readidx = (volatile int32_t *)(img->base + RVA_READIDX);
  src_l   = (float *)(img->base + RVA_SRC_L);
  src_r   = (float *)(img->base + RVA_SRC_R);

  /* The rings are allocated somewhere in this sequence, and the whole
     question is which call sizes them. Each letter is one call, so the order
     can be varied one step at a time:
       r = setSampleRate   b = setMaxBlockSize   c = XPsetSystemConfig  */
  initialize(0);
  for (i = 0; order[i]; ++i) {
    switch (order[i]) {
    case 'r': set_rate((float)rate); break;
    case 'b': set_block(maxblock); break;
    case 'c': { struct tg_system_config c; c.a = 1; c.b = 1; set_config(&c); } break;
    default: fprintf(stderr, "unknown step '%c'\n", order[i]); return 2;
    }
  }
  activate(0, maxblock);
  longmidi(gs_reset, (int)sizeof gs_reset);
  fprintf(stderr, "order \"%s\", maxblock %d: ", order, maxblock);

  left = calloc((size_t)maxblock * 8, sizeof *left);
  right = calloc((size_t)maxblock * 8, sizeof *right);
  if (!left || !right) { fprintf(stderr, "out of memory\n"); return 1; }

  shortmidi(0x90u | (60u << 8) | (100u << 16));   /* one note, held */

  {
    /* Rings closer together than a block overlap, and the left channel lands
       in the right one. */
    float *ring_left  = *(float **)(img->base + 0x1a62cc0);
    float *ring_right = *(float **)(img->base + 0x1a62cc8);
    ptrdiff_t bytes = (unsigned char *)ring_right - (unsigned char *)ring_left;
    fprintf(stderr, "ring feeding LEFT  output: %p\n", (void *)ring_left);
    fprintf(stderr, "ring feeding RIGHT output: %p\n", (void *)ring_right);
    fprintf(stderr, "separation: %td bytes = %td floats  (a block is %d samples)\n",
           bytes, bytes / (ptrdiff_t)sizeof(float), *avail);
  }
  printf("poisoning %d floats of each resampler source before every block\n",
         SRC_N);
  {
    float peak_l = 0.0f, peak_r = 0.0f;
    long bad_l = 0, bad_r = 0;
    for (i = 0; i < nblocks; ++i) {
      int k, left_poison = 0, right_poison = 0;
      if (do_poison)
        for (k = 0; k < SRC_N; ++k) { src_l[k] = poison; src_r[k] = poison; }
      process(left, right, block);
      if (do_poison) {
        for (k = 0; k < SRC_N; ++k) {
          if (poisoned(src_l[k])) ++left_poison;
          if (poisoned(src_r[k])) ++right_poison;
        }
        total_l += left_poison; total_r += right_poison;
      }
      for (k = 0; k < block; ++k) {
        float l = left[k], r = right[k];
        if (!(l == l) || l > 1e30f || l < -1e30f) ++bad_l;
        else if (l > peak_l) peak_l = l > 0 ? l : -l;
        if (!(r == r) || r > 1e30f || r < -1e30f) ++bad_r;
        else if (r > peak_r) peak_r = r > 0 ? r : -r;
      }
    }
    /* Per channel, always: a defect can live in one channel only, and a
       single number will not show it. */
    fprintf(stderr, "peak L %.6f R %.6f | not-finite L %ld R %ld\n",
            peak_l, peak_r, bad_l, bad_r);
    if (do_poison)
      fprintf(stderr, "never-written resampler slots: src_L %d, src_R %d\n",
              total_l, total_r);
  }
  return 0;
}
