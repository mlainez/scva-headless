/* SPDX-License-Identifier: CC0-1.0
 *
 * SOUND Canvas VA's SCCore.dll as a synth backend (see synth.h).
 *
 * On 64-bit Windows the core loads through LoadLibrary. Elsewhere it goes
 * through the PE loader in pe_loader.c: on Linux because there is no other
 * way, on 32-bit Windows because the system loader will not take a core that
 * imports MSVCR100 and declares a subsystem newer than Windows 98. Same
 * engine either way.
 */
#include "synth.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__i386__)
#define SCVA_LOADLIBRARY
#endif

#ifdef SCVA_LOADLIBRARY
#include <windows.h>
#else
#include "pe_loader.h"
#include "scva_runtime.h"
#endif
#include "scva_core.h"

struct scva_synth {
  struct synth base;
#ifdef SCVA_LOADLIBRARY
  HMODULE lib;
#else
  struct pe_image *img;
#endif
  struct scva_core_api api;
  int max_block;
};

static void unload(struct scva_synth *e)
{
#ifdef SCVA_LOADLIBRARY
  if (e->lib) FreeLibrary(e->lib);
#else
  if (e->img) pe_unload(e->img);
#endif
  free(e);
}

static int attach_thread(void)
{
#ifdef SCVA_LOADLIBRARY
  return 1;
#else
  return pe_thread_attach();
#endif
}

static struct synth *open_scva(const struct synth_options *opt, char *err,
                               size_t errlen)
{
  struct scva_synth *e = calloc(1, sizeof *e);
  struct scva_core_setup setup;
  const char *missing;

  if (!e) { snprintf(err, errlen, "out of memory"); return NULL; }
#ifdef SCVA_LOADLIBRARY
  e->lib = LoadLibraryA(opt->core);
  if (!e->lib) {
    snprintf(err, errlen, "cannot load %s", opt->core);
    free(e);
    return NULL;
  }
  missing = scva_core_bind(&e->api, scva_core_lookup_module, (void *)e->lib, SCVA_CORE_LIVE);
#else
  e->img = scva_core_map(opt->core, err, errlen);
  if (!e->img) { free(e); return NULL; }
  missing = scva_core_bind(&e->api, scva_core_lookup_pe, e->img, SCVA_CORE_LIVE);
#endif
  if (missing) {
    snprintf(err, errlen, "the core is missing exports");
    unload(e);
    return NULL;
  }
  e->base.backend = &synth_scva;
  e->base.rate = opt->rate;
  e->base.midi_block = 1;
  e->max_block = opt->max_block;
  setup.rate = (float)opt->rate;
  setup.max_block = opt->max_block;
  setup.init_arg = 0;
  setup.config.a = 1;
  setup.config.b = 1;
  scva_core_start(&e->api, &setup, NULL);
  return &e->base;
}

/* Deactivate, never terminate: TG_terminate calls exit(). */
static void close_scva(struct synth *s)
{
  struct scva_synth *e = (struct scva_synth *)s;
  if (attach_thread()) e->api.deactivate();
  unload(e);
}

static void short_msg(struct synth *s, uint32_t msg)
{
  ((struct scva_synth *)s)->api.short_midi(msg, 0);
}

/* The core reads up to F7; the length is implicit. */
static void long_msg(struct synth *s, const uint8_t *msg, size_t len)
{
  (void)len;
  ((struct scva_synth *)s)->api.long_midi(msg, 0);
}

/* TG_Process writes exactly the frames asked for, so it can write straight
   into the caller's buffers at an offset, in pieces no longer than the
   block size the engine was activated with. */
static void render(struct synth *s, float *l, float *r, uint32_t n)
{
  struct scva_synth *e = (struct scva_synth *)s;
  while (n) {
    uint32_t k = n > (uint32_t)e->max_block ? (uint32_t)e->max_block : n;
    e->api.process(l, r, (int)k);
    l += k;
    r += k;
    n -= k;
  }
}

static const struct synth_caps caps = {
  "scva",
  NULL,                       /* any rate: it resamples from 32 kHz itself */
  1,                          /* TG_ShortMidiIn takes a sample timestamp */
  1                           /* CC32 selects the map, per part */
};

const struct synth_backend synth_scva = {
  &caps, open_scva, close_scva, attach_thread, short_msg, long_msg, render,
  NULL                        /* names come from the name files */
};
