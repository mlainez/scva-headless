/* SPDX-License-Identifier: CC0-1.0
 *
 * SOUND Canvas VA as an LV2 instrument: MIDI in, stereo out.
 *
 * The engine is Roland's, loaded from your own SCCore.dll at instantiate
 * time. Nothing of Roland's is distributed with this plugin - the core is
 * looked for in $SCVA_DLL_DIR, then beside the bundle. Driving it is
 * scva_engine.c's job, shared with the CLAP plugin.
 *
 * TG_Process allocates nothing once set up, so run() is realtime-safe: 56
 * allocations happen during instantiate and none across an hour of audio.
 */
#define _GNU_SOURCE
#include "scva_engine.h"
#include "scva_map.h"
#include "scva_names.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>

#define SCVA_URI "https://github.com/mlainez/scva-headless#scva"

/* LV2 has no official program-list extension. This one is KXStudio's, which
   Carla, Ardour and Qtractor read; it is an ABI rather than a library, so it
   is declared here rather than depended on. */
#define LV2_PROGRAMS__Interface \
  "http://kxstudio.sf.net/ns/lv2ext/programs#Interface"

typedef struct {
  uint32_t bank;
  uint32_t program;
  const char *name;
} LV2_Program_Descriptor;

typedef struct {
  const LV2_Program_Descriptor *(*get_program)(LV2_Handle, uint32_t index);
  void (*select_program)(LV2_Handle, uint32_t bank, uint32_t program);
} LV2_Programs_Interface;

/* A patch needs four numbers, and the extension offers two. Bank carries the
   GS 14-bit bank exactly as MIDI defines it - CC0 is the MSB, CC32 the tone
   map - and bit 14, which lies outside that range, marks a drum kit. */
#define SCVA_BANK_DRUM   (1u << 14)
#define SCVA_BANK(cc0, map, drum) \
  (((drum) ? SCVA_BANK_DRUM : 0u) | ((uint32_t)(cc0) << 7) | (uint32_t)(map))
#define SCVA_BANK_CC0(b)  (((b) >> 7) & 0x7f)
#define SCVA_BANK_MAP(b)  ((b) & 0x7f)
#define SCVA_BANK_ISDRUM(b) (((b) & SCVA_BANK_DRUM) != 0)

enum {
  PORT_MIDI_IN = 0,
  PORT_OUT_L   = 1,
  PORT_OUT_R   = 2,
  PORT_MAP     = 3
};

/* The descriptor hands out a const char *, so each name is owned alongside
   the descriptor that points at it. */
struct scva_program {
  LV2_Program_Descriptor d;
  char name[SCVA_NAME_MAX + 16];
};

struct scva {
  struct scva_engine *eng;

  const LV2_Atom_Sequence *midi_in;
  float *out_l, *out_r;
  const float *map_port;

  LV2_URID midi_event;

  struct scva_names names;
  struct scva_program *progs;
  uint32_t nprogs;
  /* select_program arrives on the host's thread, so it is latched here and
     applied at the top of the next run(). */
  uint32_t sel_bank, sel_prog;
  int sel_pending;
};

/* One entry per patch the shared lists offer, tones then kits. */
static void build_programs(struct scva *s)
{
  struct scva_lists L;
  uint32_t i, n;
  if (!scva_lists_build(&L, &s->names)) return;
  n = L.ntone + L.nkit;
  s->progs = calloc(n ? n : 1, sizeof *s->progs);
  if (s->progs) {
    for (i = 0; i < n; ++i) {
      const struct scva_patch *p =
        &s->names.patch[i < L.ntone ? L.tone[i] : L.kit[i - L.ntone]];
      struct scva_program *e = &s->progs[i];
      scva_patch_label(p, e->name, sizeof e->name);
      e->d.bank = SCVA_BANK(p->bank, p->map, p->drum);
      e->d.program = p->prog;
      e->d.name = e->name;
    }
    s->nprogs = n;
  }
  scva_lists_free(&L);
}

static const LV2_Program_Descriptor *get_program(LV2_Handle h, uint32_t index)
{
  struct scva *s = h;
  return index < s->nprogs ? &s->progs[index].d : NULL;
}

static void select_program(LV2_Handle h, uint32_t bank, uint32_t program)
{
  struct scva *s = h;
  s->sel_bank = bank;
  s->sel_prog = program;
  s->sel_pending = 1;
}

static const LV2_Programs_Interface programs_iface = {
  get_program, select_program
};

static LV2_Handle instantiate(const LV2_Descriptor *desc, double rate,
                              const char *bundle,
                              const LV2_Feature *const *features)
{
  struct scva *s;
  LV2_URID_Map *map = NULL;
  char err[256], here[1024], dir[1024];
  size_t n;
  int i;
  (void)desc;

  for (i = 0; features && features[i]; ++i)
    if (!strcmp(features[i]->URI, LV2_URID__map))
      map = (LV2_URID_Map *)features[i]->data;
  if (!map) return NULL;                 /* urid:map is required */

  s = calloc(1, sizeof *s);
  if (!s) return NULL;
  s->midi_event = map->map(map->handle, LV2_MIDI__MidiEvent);

  /* the bundle path keeps its trailing slash */
  snprintf(here, sizeof here, "%s", bundle ? bundle : "");
  n = strlen(here);
  while (n > 1 && here[n - 1] == '/') here[--n] = '\0';
  scva_core_dir(here, dir, sizeof dir);

  s->eng = scva_engine_open(dir, rate, err, sizeof err);
  if (!s->eng) {
    fprintf(stderr, "scva.lv2: %s\n", err);
    free(s);
    return NULL;
  }
  /* Names are a convenience: without them the plugin still plays. */
  if (scva_names_load(&s->names, dir)) build_programs(s);
  return s;
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

static void apply_selection(struct scva *s)
{
  int drum = SCVA_BANK_ISDRUM(s->sel_bank);
  scva_engine_patch(s->eng, drum ? SCVA_DRUM_CH : SCVA_TONE_CH,
                    (int)SCVA_BANK_MAP(s->sel_bank),
                    (int)SCVA_BANK_CC0(s->sel_bank), (int)(s->sel_prog & 0x7f));
  s->sel_pending = 0;
}

static void run(LV2_Handle h, uint32_t n_samples)
{
  struct scva *s = h;
  uint32_t at = 0;

  /* The host's audio thread, not the one that opened the engine. */
  if (!scva_engine_thread()) {
    memset(s->out_l, 0, n_samples * sizeof *s->out_l);
    memset(s->out_r, 0, n_samples * sizeof *s->out_r);
    return;
  }
  if (s->sel_pending) apply_selection(s);

  if (s->map_port) {
    int want = (int)(*s->map_port + 0.5f);
    if (want < 0) want = 0;
    if (want > SCVA_MAP_8820) want = SCVA_MAP_8820;
    scva_engine_set_map(s->eng, want);
  }

  if (s->midi_in) {
    LV2_ATOM_SEQUENCE_FOREACH(s->midi_in, ev) {
      uint32_t frame;
      if (ev->body.type != s->midi_event) continue;
      frame = (uint32_t)ev->time.frames;
      if (frame > n_samples) frame = n_samples;
      if (frame > at) {
        scva_engine_render(s->eng, s->out_l + at, s->out_r + at, frame - at);
        at = frame;
      }
      scva_engine_midi(s->eng, (const uint8_t *)LV2_ATOM_BODY_CONST(&ev->body),
                       ev->body.size);
    }
  }
  if (at < n_samples)
    scva_engine_render(s->eng, s->out_l + at, s->out_r + at, n_samples - at);
}

static void cleanup(LV2_Handle h)
{
  struct scva *s = h;
  if (!s) return;
  scva_engine_close(s->eng);
  scva_names_free(&s->names);
  free(s->progs);
  free(s);
}

static const void *extension_data(const char *uri)
{
  if (!strcmp(uri, LV2_PROGRAMS__Interface)) return &programs_iface;
  return NULL;
}

static const LV2_Descriptor descriptor = {
  SCVA_URI, instantiate, connect_port, NULL, run, NULL, cleanup, extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
  return index == 0 ? &descriptor : NULL;
}
