/* SPDX-License-Identifier: CC0-1.0
 *
 * See synth_vsc.h. The engine renders fixed blocks of 16-bit stereo; this
 * turns them into the float frames synth.h deals in.
 */
#include "synth_vsc.h"
#include "win9x.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vsc_synth {
  struct synth base;
  struct vsc *engine;
  int16_t *block;             /* one engine block, interleaved */
};

/* The tone set beside vsc.sys, for a caller that names none. */
static void default_tones(const char *core, char *out, size_t cap)
{
  const char *slash = strrchr(core, '/');
#ifdef _WIN32
  const char *back = strrchr(core, '\\');
  if (back && (!slash || back > slash)) slash = back;
#endif
  snprintf(out, cap, "%.*svsc88pro.dat", slash ? (int)(slash - core + 1) : 0, core);
}

static struct synth *open_vsc(const struct synth_options *opt, char *err,
                              size_t errlen)
{
  static const struct vsc_config defaults = VSC_CONFIG_DEFAULT;
  const struct synth_vsc_options *vo = opt->backend_opts;
  struct vsc_config cfg = vo ? vo->config : defaults;
  struct vsc_synth *e = calloc(1, sizeof *e);
  char tones[2048];

  if (!e) { snprintf(err, errlen, "out of memory"); return NULL; }
  if (vo && vo->tones) snprintf(tones, sizeof tones, "%s", vo->tones);
  else default_tones(opt->core, tones, sizeof tones);
  cfg.rate = (int)opt->rate;
  e->engine = vsc_open(opt->core, tones, &cfg, err, errlen);
  if (!e->engine) { free(e); return NULL; }
  e->base.backend = &synth_vsc;
  e->base.rate = vsc_rate(e->engine);
  e->base.midi_block = vsc_block_frames(e->engine);
  e->block = malloc((size_t)e->base.midi_block * 2 * sizeof *e->block);
  if (!e->block) {
    vsc_close(e->engine);
    free(e);
    snprintf(err, errlen, "out of memory");
    return NULL;
  }
  return &e->base;
}

static void close_vsc(struct synth *s)
{
  struct vsc_synth *e = (struct vsc_synth *)s;
  vsc_close(e->engine);
  free(e->block);
  free(e);
}

/* The engine keeps no per-thread state: it switches the FPU control word
   itself on every call and reads no thread block. */
static int attach_thread(void) { return 1; }

static void short_msg(struct synth *s, uint32_t msg)
{
  vsc_short(((struct vsc_synth *)s)->engine, msg);
}

/* Longer than the 256 bytes the driver carries is dropped. */
static void long_msg(struct synth *s, const uint8_t *msg, size_t len)
{
  vsc_sysex(((struct vsc_synth *)s)->engine, msg, len);
}

/* n must be a whole number of engine blocks. */
static void render(struct synth *s, float *l, float *r, uint32_t n)
{
  struct vsc_synth *e = (struct vsc_synth *)s;
  uint32_t block = (uint32_t)e->base.midi_block, k;
  for (; n >= block; n -= block) {
    vsc_render(e->engine, e->block);
    for (k = 0; k < block; ++k) {
      *l++ = (float)e->block[2 * k] / 32768.0f;
      *r++ = (float)e->block[2 * k + 1] / 32768.0f;
    }
  }
}

static const struct synth_caps caps = {
  "vsc",
  0,                          /* 11025, 22050 or 44100 Hz only */
  0,                          /* MIDI lands on the next block */
  0                           /* one tone set for every part */
};

const struct synth_backend synth_vsc = {
  &caps, open_vsc, close_vsc, attach_thread, short_msg, long_msg, render
};
