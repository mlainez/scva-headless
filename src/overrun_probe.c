/* SPDX-License-Identifier: CC0-1.0 */
/* A diagnostic, not part of the renderer.
 *
 * Does TG_Process write past the frame count it is given?
 *
 * Both buffers are watched, and each is followed by a page with no
 * permissions at all, so a write past the end cannot be mistaken for anything:
 * it is a SIGSEGV at a known address, not a value someone has to notice.
 *
 * Two things are measured:
 *   - the highest index written in each buffer, found with a sentinel fill
 *   - whether a guarded buffer survives at all
 *
 * The test can fail in the useful direction: if the engine is well behaved the
 * highest index is frames-1 in both and nothing faults.
 *
 *   build/overrun-probe [--block 256] [--guard] [--gap N]
 */
#define _GNU_SOURCE
#include "pe_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

typedef MSABI int (*tg_initialize_fn)(int);
typedef MSABI void (*tg_set_sample_rate_fn)(float);
typedef MSABI void (*tg_set_max_block_fn)(int);
typedef MSABI int (*tg_activate_fn)(int, int);
typedef MSABI void (*tg_short_midi_fn)(unsigned int);
typedef MSABI void (*tg_long_midi_fn)(const unsigned char *, int);
typedef MSABI void (*tg_process_fn)(float *, float *, int);
struct tg_system_config { int a, b; };
typedef MSABI int (*tg_set_config_fn)(const struct tg_system_config *);

#define SENTINEL_BITS 0x7FBADD11u

static float sentinel(void)
{ float f; uint32_t b = SENTINEL_BITS; memcpy(&f, &b, sizeof f); return f; }
static int is_sentinel(float f)
{ uint32_t b; memcpy(&b, &f, sizeof b); return b == SENTINEL_BITS; }

static sigjmp_buf jump;
static volatile void *fault_at;
static void on_segv(int sig, siginfo_t *si, void *uc)
{ (void)sig; (void)uc; fault_at = si->si_addr; siglongjmp(jump, 1); }

/* A buffer of `floats` floats, followed by a page that cannot be written. */
static float *guarded(size_t floats, size_t *span_out)
{
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  size_t bytes = ((floats * sizeof(float)) + page - 1) & ~(page - 1);
  size_t span = bytes + page;
  unsigned char *p = mmap(NULL, span, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return NULL;
  if (mprotect(p + bytes, page, PROT_NONE) != 0) return NULL;
  if (span_out) *span_out = span;
  /* end the usable area exactly at the guard so an overrun of any size hits it */
  return (float *)(p + bytes - floats * sizeof(float));
}

int main(int argc, char **argv)
{
  const char *core = "dll/SCCore.dll";
  double rate = 48000.0;
  int block = 256, maxblock = 4096, i, use_guard = 0, gap = 0;
  static const unsigned char gs_reset[] = {
    0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
  struct pe_image *img;
  char err[256];
  tg_initialize_fn initialize; tg_set_sample_rate_fn set_rate;
  tg_set_max_block_fn set_block; tg_activate_fn activate;
  tg_short_midi_fn shortmidi; tg_long_midi_fn longmidi;
  tg_process_fn process; tg_set_config_fn set_config;
  float *left, *right;
  size_t room;
  struct sigaction sa;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--maxblock") && i + 1 < argc) maxblock = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--gap") && i + 1 < argc) gap = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--guard")) use_guard = 1;
  }
  room = (size_t)maxblock * 8;

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

  initialize(0);
  set_rate((float)rate);
  set_block(maxblock);
  { struct tg_system_config c; c.a = 1; c.b = 1; set_config(&c); }
  set_rate((float)rate);
  activate(0, maxblock);
  longmidi(gs_reset, (int)sizeof gs_reset);

  if (use_guard) {
    /* exactly `block` usable floats, then an unwritable page */
    left = guarded((size_t)block, NULL);
    right = guarded((size_t)block, NULL);
    room = (size_t)block;
    printf("guarded: %d usable floats per buffer, then PROT_NONE\n", block);
  } else {
    left = calloc(room, sizeof *left);
    right = calloc(room, sizeof *right);
    printf("plain: %zu floats per buffer (%d requested per call)\n", room, block);
  }
  if (!left || !right) { fprintf(stderr, "cannot allocate\n"); return 1; }
  if (gap) printf("(left and right allocated separately)\n");

  shortmidi(0x90u | (60u << 8) | (100u << 16));

  sa.sa_sigaction = on_segv;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sa, NULL);

  for (i = 0; i < 24; ++i) {
    size_t k;
    long hi_l = -1, hi_r = -1;
    for (k = 0; k < room; ++k) { left[k] = sentinel(); right[k] = sentinel(); }
    if (sigsetjmp(jump, 1) == 0) {
      process(left, right, block);
    } else {
      printf("blk %2d: FAULTED writing %p "
             "(left ends %p, right ends %p) -> overrun proven\n",
             i, (void *)fault_at,
             (void *)(left + block), (void *)(right + block));
      return 3;
    }
    {
      /* Holes matter as much as the end: a sample inside the block that the
         engine never wrote keeps whatever the buffer held, which is a
         discontinuity the ear hears as a click. Counted per channel: a
         defect can live in one channel only. */
      long holes_l = 0, holes_r = 0, first_hole_l = -1, first_hole_r = -1;
      for (k = 0; k < room; ++k) {
        if (!is_sentinel(left[k])) hi_l = (long)k;
        if (!is_sentinel(right[k])) hi_r = (long)k;
      }
      for (k = 0; k < (size_t)block; ++k) {
        if (is_sentinel(left[k])) {
          ++holes_l; if (first_hole_l < 0) first_hole_l = (long)k;
        }
        if (is_sentinel(right[k])) {
          ++holes_r; if (first_hole_r < 0) first_hole_r = (long)k;
        }
      }
      if (i < 6 || i == 23 || holes_l || holes_r)
        printf("blk %2d: asked %d | highest written L %ld R %ld | "
               "holes inside block: L %ld (first %ld)  R %ld (first %ld)%s\n",
               i, block, hi_l, hi_r,
               holes_l, first_hole_l, holes_r, first_hole_r,
               (hi_l >= block || hi_r >= block) ? "  <-- PAST THE END" : "");
    }
  }
  printf("done\n");
  return 0;
}
