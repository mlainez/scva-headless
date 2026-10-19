/* SPDX-License-Identifier: CC0-1.0
 *
 * SOUND Canvas VA as an LV2 instrument: MIDI in, stereo out.
 *
 * The engine is Roland's, loaded from your own SCCore.dll at instantiate
 * time. Nothing of Roland's is distributed with this plugin - the core is
 * looked for in $SCVA_DLL_DIR, then beside the bundle. Driving it is
 * scva_engine.c's job, shared with the CLAP plugin.
 *
 * The tone map, the first part's tone and the drum part's kit are three
 * control ports, set from the editor (scva_lv2_ui.c); the plugin plays what
 * they hold. There is no program list: a host's would show one pick while
 * the editor made another.
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

enum {
  PORT_MIDI_IN = 0,
  PORT_OUT_L   = 1,
  PORT_OUT_R   = 2,
  PORT_MAP     = 3,
  PORT_KIT     = 4,    /* a GS program number */
  PORT_INST    = 5     /* a position in the map's tones */
};

struct scva {
  struct scva_engine *eng;

  const LV2_Atom_Sequence *midi_in;
  float *out_l, *out_r;
  const float *map_port, *kit_port, *inst_port;

  LV2_URID midi_event;

  struct scva_names names;
  struct scva_lists lists;
  /* names.patch index last sent to each part, -1 before any */
  int tone_sent, kit_sent;
};

static int port_int(const float *port, int lo, int hi)
{
  int v = port ? (int)(*port + 0.5f) : lo;
  return v < lo ? lo : v > hi ? hi : v;
}

/* The Instrument port's position in tone map m, past the end meaning the
   last, on the first part. Sent when it resolves to another patch. */
static void send_tone(struct scva *s, int m)
{
  const struct scva_patch *p;
  int n = (int)s->lists.ntone[m], k;
  if (!n) return;
  k = port_int(s->inst_port, 0, n - 1);
  if ((int)s->lists.tone[m][k] == s->tone_sent) return;
  p = &s->names.patch[s->lists.tone[m][k]];
  scva_engine_patch(s->eng, SCVA_TONE_CH, p->map, p->bank, p->prog);
  s->tone_sent = (int)s->lists.tone[m][k];
}

/* The Drum Kit port's kit in tone map m, or Standard where m has no kit of
   that number, on the drum part. Sent when it resolves to another patch. */
static void send_kit(struct scva *s, int m)
{
  const struct scva_patch *p;
  int want = port_int(s->kit_port, 0, 127), k;
  k = scva_lists_find(&s->names, s->lists.kit[m], s->lists.nkit[m], 0, want);
  if (k < 0) k = scva_lists_find(&s->names, s->lists.kit[m], s->lists.nkit[m], 0, 0);
  if (k < 0 || (int)s->lists.kit[m][k] == s->kit_sent) return;
  p = &s->names.patch[s->lists.kit[m][k]];
  scva_engine_patch(s->eng, SCVA_DRUM_CH, p->map, p->bank, p->prog);
  s->kit_sent = (int)s->lists.kit[m][k];
}

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
  s->tone_sent = s->kit_sent = -1;

  /* the bundle path keeps its trailing slash */
  snprintf(here, sizeof here, "%s", bundle ? bundle : "");
  n = strlen(here);
  while (n > 1 && here[n - 1] == '/') here[--n] = '\0';
  scva_core_dir(here, dir, sizeof dir);

  /* The instrument and kit lists are the install's own: nothing of Roland's
     ships with this plugin, so without them there is nothing to offer. */
  scva_names_load(&s->names, dir);
  if (!s->names.npatch || !strcmp(s->names.module, "General MIDI") ||
      !scva_lists_build(&s->lists, &s->names)) {
    fprintf(stderr, "scva.lv2: no SOUND Canvas VA name files (SCVSC.tnf, "
            "SCVSC.drf) beside SCCore.dll in %s\n", dir);
    scva_names_free(&s->names);
    free(s);
    return NULL;
  }
  s->eng = scva_engine_open(dir, rate, err, sizeof err);
  if (!s->eng) {
    fprintf(stderr, "scva.lv2: %s\n", err);
    scva_lists_free(&s->lists);
    scva_names_free(&s->names);
    free(s);
    return NULL;
  }
  return s;
}

static void connect_port(LV2_Handle h, uint32_t port, void *data)
{
  struct scva *s = h;
  switch (port) {
  case PORT_MIDI_IN: s->midi_in   = (const LV2_Atom_Sequence *)data; break;
  case PORT_OUT_L:   s->out_l     = (float *)data; break;
  case PORT_OUT_R:   s->out_r     = (float *)data; break;
  case PORT_MAP:     s->map_port  = (const float *)data; break;
  case PORT_KIT:     s->kit_port  = (const float *)data; break;
  case PORT_INST:    s->inst_port = (const float *)data; break;
  default: break;
  }
}

static void run(LV2_Handle h, uint32_t n_samples)
{
  struct scva *s = h;
  uint32_t at = 0;
  int map;

  /* The host's audio thread, not the one that opened the engine. */
  if (!scva_engine_thread()) {
    memset(s->out_l, 0, n_samples * sizeof *s->out_l);
    memset(s->out_r, 0, n_samples * sizeof *s->out_r);
    return;
  }
  map = port_int(s->map_port, SCVA_MAP_DEFAULT, SCVA_MAP_8820);
  scva_engine_set_map(s->eng, map);
  send_tone(s, map);
  send_kit(s, map);

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
  scva_lists_free(&s->lists);
  scva_names_free(&s->names);
  free(s);
}

static const void *extension_data(const char *uri)
{
  (void)uri;
  return NULL;
}

static const LV2_Descriptor descriptor = {
  SCVA_URI, instantiate, connect_port, NULL, run, NULL, cleanup, extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
  return index == 0 ? &descriptor : NULL;
}
