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
  uint32_t used;              /* frames of it already handed out; a full
                                 block means a new one is rendered next */
  /* The engine's names, read once at open so that asking for one never
     enters the engine while another thread renders: [program][bank]. */
  char (*tones)[128][VSC_NAME_LEN + 1];
  char drums[128][VSC_NAME_LEN + 1];
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
  char tones_path[2048];
  int p;

  if (!e) { snprintf(err, errlen, "out of memory"); return NULL; }
  if (vo && vo->tones) snprintf(tones_path, sizeof tones_path, "%s", vo->tones);
  else default_tones(opt->core, tones_path, sizeof tones_path);
  cfg.rate = (int)opt->rate;
  e->engine = vsc_open(opt->core, tones_path, &cfg, err, errlen);
  if (!e->engine) { free(e); return NULL; }
  e->base.backend = &synth_vsc;
  e->base.rate = vsc_rate(e->engine);
  e->base.midi_block = vsc_block_frames(e->engine);
  e->used = (uint32_t)e->base.midi_block;
  e->block = malloc((size_t)e->base.midi_block * 2 * sizeof *e->block);
  e->tones = calloc(128, sizeof *e->tones);
  if (!e->block || !e->tones) {
    free(e->block);
    free(e->tones);
    vsc_close(e->engine);
    free(e);
    snprintf(err, errlen, "out of memory");
    return NULL;
  }
  for (p = 0; p < 128; ++p)
    if (!vsc_tone_names(e->engine, p, e->tones[p])) memset(e->tones[p], 0, sizeof e->tones[p]);
  if (!vsc_drum_names(e->engine, e->drums)) memset(e->drums, 0, sizeof e->drums);
  return &e->base;
}

static void close_vsc(struct synth *s)
{
  struct vsc_synth *e = (struct vsc_synth *)s;
  vsc_close(e->engine);
  free(e->block);
  free(e->tones);
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

/* Any n: the engine renders whole blocks, and what one call does not take
   of a block is the start of the next call's output. MIDI sent between
   calls therefore lands where the next block starts. */
static void render(struct synth *s, float *l, float *r, uint32_t n)
{
  struct vsc_synth *e = (struct vsc_synth *)s;
  uint32_t block = (uint32_t)e->base.midi_block;
  while (n) {
    uint32_t take, k;
    if (e->used == block) {
      vsc_render(e->engine, e->block);
      e->used = 0;
    }
    take = block - e->used < n ? block - e->used : n;
    for (k = e->used; k < e->used + take; ++k) {
      *l++ = (float)e->block[2 * k] / 32768.0f;
      *r++ = (float)e->block[2 * k + 1] / 32768.0f;
    }
    e->used += take;
    n -= take;
  }
}

static const char *name(struct synth *s, int drums, int bank, int program)
{
  struct vsc_synth *e = (struct vsc_synth *)s;
  const char *n;
  if (program < 0 || program > 127 || bank < 0 || bank > 127) return NULL;
  n = drums ? e->drums[program] : e->tones[program][bank];
  return n[0] ? n : NULL;
}

static const int rates[] = { 11025, 22050, 44100, 0 };

static const struct synth_caps caps = {
  "vsc",
  rates,
  0,                          /* MIDI lands on the next block */
  0                           /* one tone set for every part */
};

const struct synth_backend synth_vsc = {
  &caps, open_vsc, close_vsc, attach_thread, short_msg, long_msg, render, name
};
