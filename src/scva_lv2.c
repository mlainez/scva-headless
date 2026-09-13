/* SPDX-License-Identifier: CC0-1.0
 *
 * SOUND Canvas VA as an LV2 instrument: MIDI in, stereo out.
 *
 * The engine is Roland's, loaded from your own SCCore.dll at instantiate
 * time. Nothing of Roland's is distributed with this plugin - the core is
 * looked for in $SCVA_DLL_DIR, then beside the bundle.
 *
 * Two things about the engine shape this file:
 *
 *   TG_Process allocates nothing once set up, so run() is realtime-safe:
 *   56 allocations happen during instantiate and none across an hour of
 *   audio.
 *
 *   TG_terminate calls exit(). Calling it from cleanup() would take the host
 *   down with it, so this plugin never calls it - it deactivates and unmaps.
 */
#define _GNU_SOURCE
#include "pe_loader.h"
#include "scva_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>

#define SCVA_URI "https://github.com/mlainez/scva-headless#scva"

/* Declared to the engine; run() is chunked to it. */
#define SCVA_MAXBLOCK 4096

enum {
  PORT_MIDI_IN = 0,
  PORT_OUT_L   = 1,
  PORT_OUT_R   = 2,
  PORT_MAP     = 3
};

typedef MSABI int (*tg_initialize_fn)(int);
typedef MSABI void (*tg_set_sample_rate_fn)(float);
typedef MSABI void (*tg_set_max_block_fn)(int);
typedef MSABI int (*tg_activate_fn)(float, int);
typedef MSABI void (*tg_deactivate_fn)(void);
typedef MSABI void (*tg_short_midi_fn)(unsigned int, int);
typedef MSABI void (*tg_long_midi_fn)(const unsigned char *, int);
typedef MSABI void (*tg_process_fn)(float *, float *, int);
struct tg_system_config { int a, b; };
typedef MSABI int (*tg_set_config_fn)(const struct tg_system_config *);

struct scva {
  struct pe_image *img;
  tg_set_sample_rate_fn set_rate;
  tg_activate_fn activate;
  tg_deactivate_fn deactivate;
  tg_short_midi_fn short_midi;
  tg_long_midi_fn long_midi;
  tg_process_fn process;

  const LV2_Atom_Sequence *midi_in;
  float *out_l, *out_r;
  const float *map_port;

  LV2_URID midi_event;
  int map_now;                 /* tone map currently pushed to the engine */
};

/* $SCVA_DLL_DIR/SCCore.dll, then <bundle>/SCCore.dll. */
static struct pe_image *open_core(const char *bundle, char *err, size_t errlen)
{
  char path[2048];
  const char *dir = getenv("SCVA_DLL_DIR");
  struct pe_image *img;
  if (dir && *dir) {
    snprintf(path, sizeof path, "%s/SCCore.dll", dir);
    img = pe_load(path, err, errlen);
    if (img) return img;
  }
  if (bundle && *bundle) {
    snprintf(path, sizeof path, "%sSCCore.dll", bundle);
    img = pe_load(path, err, errlen);
    if (img) return img;
  }
  snprintf(err, errlen,
           "no SCCore.dll: set SCVA_DLL_DIR or put it in the bundle");
  return NULL;
}

static void send_map(struct scva *s, int mapval)
{
  int ch;
  for (ch = 0; ch < 16; ++ch)
    s->short_midi(scva_map_cc(ch, mapval), 0);
  s->map_now = mapval;
}

static LV2_Handle instantiate(const LV2_Descriptor *desc, double rate,
                              const char *bundle,
                              const LV2_Feature *const *features)
{
  struct scva *s;
  LV2_URID_Map *map = NULL;
  char err[256];
  int i;
  (void)desc;

  for (i = 0; features && features[i]; ++i)
    if (!strcmp(features[i]->URI, LV2_URID__map))
      map = (LV2_URID_Map *)features[i]->data;
  if (!map) return NULL;                 /* urid:map is required */

  s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->midi_event = map->map(map->handle, LV2_MIDI__MidiEvent);

  s->img = open_core(bundle, err, sizeof err);
  if (!s->img) { fprintf(stderr, "scva.lv2: %s\n", err); free(s); return NULL; }

#define GET(f, t, n) s->f = (t)pe_symbol(s->img, n); \
  if (!s->f) { fprintf(stderr, "scva.lv2: missing %s\n", n); goto fail; }
  {
    tg_initialize_fn initialize;
    tg_set_max_block_fn set_block;
    tg_set_config_fn set_config;
    GET(set_rate, tg_set_sample_rate_fn, "TG_setSampleRate")
    GET(activate, tg_activate_fn, "TG_activate")
    GET(deactivate, tg_deactivate_fn, "TG_deactivate")
    GET(short_midi, tg_short_midi_fn, "TG_ShortMidiIn")
    GET(long_midi, tg_long_midi_fn, "TG_LongMidiIn")
    GET(process, tg_process_fn, "TG_Process")
    initialize = (tg_initialize_fn)pe_symbol(s->img, "TG_initialize");
    set_block = (tg_set_max_block_fn)pe_symbol(s->img, "TG_setMaxBlockSize");
    set_config = (tg_set_config_fn)pe_symbol(s->img, "TG_XPsetSystemConfig");
    if (!initialize || !set_block || !set_config) goto fail;

    /* The order matters: the rate is set on both sides of the block size,
       and the second call is the last thing before activate. Otherwise the
       core emits Inf and NaN while reporting no error. */
    initialize(0);
    s->set_rate((float)rate);
    set_block(SCVA_MAXBLOCK);
    { struct tg_system_config c; c.a = 1; c.b = 1; set_config(&c); }
    s->set_rate((float)rate);
    s->activate((float)rate, SCVA_MAXBLOCK);
  }
#undef GET
  {
    static const unsigned char gs_reset[] = {
      0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
    s->long_midi(gs_reset, 0);
  }
  s->map_now = -1;
  return s;

fail:
  pe_unload(s->img);
  free(s);
  return NULL;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
  struct scva *s = h;
  switch (port) {
  case PORT_MIDI_IN: s->midi_in  = (const LV2_Atom_Sequence *)data; break;
  case PORT_OUT_L:   s->out_l    = (float *)data; break;
  case PORT_OUT_R:   s->out_r    = (float *)data; break;
  case PORT_MAP:     s->map_port = (const float *)data; break;
  default: break;
  }
}

/* TG_Process writes exactly the frames asked for, so it can write straight
   into the output ports at an offset. It is chunked to the declared maximum. */
static void render(struct scva *s, uint32_t at, uint32_t frames)
{
  while (frames) {
    uint32_t n = frames > SCVA_MAXBLOCK ? SCVA_MAXBLOCK : frames;
    s->process(s->out_l + at, s->out_r + at, (int)n);
    at += n;
    frames -= n;
  }
}

static void deliver(struct scva *s, const uint8_t *d, uint32_t size)
{
  if (!size) return;
  if (d[0] == 0xf0) {
    s->long_midi(d, 0);
    return;
  }
  /* A program change latches the tone map, so CC32 goes first on that part. */
  if (s->map_now > 0 && (d[0] & 0xf0) == 0xc0)
    s->short_midi(scva_map_cc(d[0] & 0x0f, s->map_now), 0);
  {
    unsigned int msg = d[0];
    if (size > 1) msg |= (unsigned int)d[1] << 8;
    if (size > 2) msg |= (unsigned int)d[2] << 16;
    s->short_midi(msg, 0);
  }
}

static void run(LV2_Handle h, uint32_t n_samples)
{
  struct scva *s = h;
  uint32_t at = 0;

  if (s->map_port) {
    int want = (int)(*s->map_port + 0.5f);
    if (want < 0) want = 0;
    if (want > SCVA_MAP_8820) want = SCVA_MAP_8820;
    if (want != s->map_now) send_map(s, want);
  }

  if (s->midi_in) {
    LV2_ATOM_SEQUENCE_FOREACH(s->midi_in, ev) {
      uint32_t frame;
      if (ev->body.type != s->midi_event) continue;
      frame = (uint32_t)ev->time.frames;
      if (frame > n_samples) frame = n_samples;
      if (frame > at) { render(s, at, frame - at); at = frame; }
      deliver(s, (const uint8_t *)LV2_ATOM_BODY_CONST(&ev->body),
              ev->body.size);
    }
  }
  if (at < n_samples) render(s, at, n_samples - at);
}

static void cleanup(LV2_Handle h)
{
  struct scva *s = h;
  if (!s) return;
  /* Deactivate, never terminate: TG_terminate calls exit(). */
  if (s->deactivate) s->deactivate();
  pe_unload(s->img);
  free(s);
}

static const LV2_Descriptor descriptor = {
  SCVA_URI, instantiate, connect_port, NULL, run, NULL, cleanup, NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
  return index == 0 ? &descriptor : NULL;
}
