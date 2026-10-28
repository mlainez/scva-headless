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

static int has_dir(const char *path)
{
#ifdef _WIN32
  if (strchr(path, '\\') || strchr(path, ':')) return 1;
#endif
  return strchr(path, '/') != NULL;
}

/* A tone set named without a directory is looked for beside vsc.sys. */
static void tones_path(const char *core, const char *tones, char *out, size_t cap)
{
  const char *slash = strrchr(core, '/');
#ifdef _WIN32
  const char *back = strrchr(core, '\\');
  if (back && (!slash || back > slash)) slash = back;
#endif
  if (has_dir(tones)) snprintf(out, cap, "%s", tones);
  else snprintf(out, cap, "%.*s%s", slash ? (int)(slash - core + 1) : 0, core, tones);
}

static struct synth *open_vsc(const struct synth_options *opt, char *err,
                              size_t errlen)
{
  struct synth_vsc_options defaults;
  const struct synth_vsc_options *vo = opt->backend_opts;
  struct vsc_config cfg;
  struct vsc_synth *e = calloc(1, sizeof *e);
  char tones[2048];
  int p;

  if (!e) { snprintf(err, errlen, "out of memory"); return NULL; }
  if (!vo) { synth_vsc.opts_init(&defaults); vo = &defaults; }
  cfg = vo->config;
  cfg.rate = (int)opt->rate;
  tones_path(opt->core, vo->tones, tones, sizeof tones);
  e->engine = vsc_open(opt->core, tones, &cfg, err, errlen);
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

/* ---- options ------------------------------------------------------------- */

static void opts_init(void *opts)
{
  static const struct vsc_config defaults = VSC_CONFIG_DEFAULT;
  struct synth_vsc_options *o = opts;
  o->tones = "vsc88pro.dat";
  o->config = defaults;
}

/* An integer from lo to hi, all of value. */
static int int_in(const char *value, int lo, int hi, int *out)
{
  char *end;
  long v = strtol(value, &end, 10);
  if (end == value || *end || v < lo || v > hi) return 0;
  *out = (int)v;
  return 1;
}

/* A decimal from 0 to 8. Checked by hand and read with atof: mingw replaces
   strtod with its own, which the Windows 98 build cannot import. */
static int gain(const char *value, float *out)
{
  const char *dot = strchr(value, '.');
  double v;
  if (!*value || strspn(value, "0123456789.") != strlen(value) ||
      (dot && strchr(dot + 1, '.')) || !strcmp(value, "."))
    return 0;
  v = atof(value);
  if (v > 8.0) return 0;
  *out = (float)v;
  return 1;
}

#define REFUSE(...) do { snprintf(err, errlen, __VA_ARGS__); return 0; } while (0)

/* Also takes the map names a console types: Default is VSC's own set. */
static int apply_set(void *opts, const char *v, char *err, size_t errlen)
{
  struct synth_vsc_options *o = opts;
  if (!strcmp(v, "default")) o->tones = "vsc88pro.dat";
  else if (!strcmp(v, "55")) o->tones = "vsc55.dat";
  else if (!strcmp(v, "88")) o->tones = "vsc88.dat";
  else if (!strcmp(v, "88pro")) o->tones = "vsc88pro.dat";
  else REFUSE("VSC has the 55, 88 and 88pro tone sets");
  return 1;
}

static int apply_poly(void *opts, const char *v, char *err, size_t errlen)
{
  if (!int_in(v, 1, 128, &((struct synth_vsc_options *)opts)->config.polyphony))
    REFUSE("--poly takes 1 to 128");
  return 1;
}

static int apply_volume(void *opts, const char *v, char *err, size_t errlen)
{
  if (!int_in(v, 0, 100, &((struct synth_vsc_options *)opts)->config.volume))
    REFUSE("--volume takes 0 to 100");
  return 1;
}

static int apply_rev(void *opts, const char *v, char *err, size_t errlen)
{
  if (!gain(v, &((struct synth_vsc_options *)opts)->config.rev_gain))
    REFUSE("--rev-gain takes 0 to 8");
  return 1;
}

static int apply_cho(void *opts, const char *v, char *err, size_t errlen)
{
  if (!gain(v, &((struct synth_vsc_options *)opts)->config.cho_gain))
    REFUSE("--cho-gain takes 0 to 8");
  return 1;
}

static int apply_delay(void *opts, const char *v, char *err, size_t errlen)
{
  if (!gain(v, &((struct synth_vsc_options *)opts)->config.delay_gain))
    REFUSE("--delay-gain takes 0 to 8");
  return 1;
}

static int apply_cpu(void *opts, const char *v, char *err, size_t errlen)
{
  struct synth_vsc_options *o = opts;
  if (!strcmp(v, "auto")) o->config.cpu = VSC_CPU_AUTO;
  else if (!strcmp(v, "mmx")) o->config.cpu = VSC_CPU_MMX;
  else if (!strcmp(v, "fpu")) o->config.cpu = VSC_CPU_FPU;
  else REFUSE("--cpu takes auto, mmx or fpu");
  return 1;
}

static const struct synth_option options[] = {
  {"--set", "55|88|88pro", "tone set, beside vsc.sys (default 88pro)", apply_set},
  {"--poly", "N", "maximum voices, 1-128 (default 64)", apply_poly},
  {"--volume", "N", "master volume, 0-100 (default 80)", apply_volume},
  {"--rev-gain", "G", "reverb return gain (default 1.26)", apply_rev},
  {"--cho-gain", "G", "chorus return gain (default 1.26)", apply_cho},
  {"--delay-gain", "G", "delay return gain (default 1.26)", apply_delay},
  {"--cpu", "auto|mmx|fpu", "engine code path (default auto)", apply_cpu},
  {NULL, NULL, NULL, NULL}
};

static const int rates[] = { 11025, 22050, 44100, 0 };

static const struct synth_caps caps = {
  "vsc",
  "vsc.sys",
  rates,
  22050,                      /* VSC 3.2's own */
  0,                          /* MIDI lands on the next block */
  0,                          /* one tone set for every part */
  "--set"
};

const struct synth_backend synth_vsc = {
  &caps, options, sizeof(struct synth_vsc_options), opts_init, open_vsc, close_vsc, attach_thread, short_msg, long_msg, render, name
};
