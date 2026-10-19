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
#include <lv2/worker/worker.h>

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

/* The host side: program_changed(handle, -1) reloads the whole list. Never to
   be called from run(). */
#define LV2_PROGRAMS__Host "http://kxstudio.sf.net/ns/lv2ext/programs#Host"

typedef struct {
  void *handle;
  void (*program_changed)(void *handle, int32_t index);
} LV2_Programs_Host;

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
  PORT_MAP     = 3,
  PORT_KIT     = 4,
  PORT_INST    = 5,    /* the editor's pick: a position in the map's tones */
  PORT_TONE    = 6     /* out: the position part 1 plays, for the editor */
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
  const float *map_port, *kit_port, *inst_port;
  float *tone_port;
  int inst_seen;               /* the Instrument port's last value, -1 before */

  LV2_URID midi_event;

  struct scva_names names;
  struct scva_lists lists;
  int kit_sent;                /* names.patch index on the drum part, or -1 */
  /* One program list of tones per tone map; the host is offered
     prog_map's. Kits are the Drum Kit port. */
  struct scva_program *progs[SCVA_MAPS];
  uint32_t nprogs[SCVA_MAPS];
  volatile int prog_map;
  /* Part 1's position in that list, picked from the host's program list or
     the editor, -1 before any. A host keeps its selection by position across
     a reload, so a new map plays the tone at that position in its own list:
     what the host shows. */
  int tone_pos;
  /* select_program arrives on the host's thread, so it is latched here and
     applied at the top of the next run(). */
  uint32_t sel_bank, sel_prog;
  int sel_pending;
  /* The host is told of a new list from the worker: never from run(). */
  const LV2_Worker_Schedule *worker;
  const LV2_Programs_Host *programs_host;
};

/* Per tone map, one program per tone of the shared lists. */
static void build_programs(struct scva *s)
{
  int m;
  uint32_t i;
  if (!scva_lists_build(&s->lists, &s->names)) return;
  for (m = 0; m < SCVA_MAPS; ++m) {
    uint32_t n = s->lists.ntone[m];
    s->progs[m] = calloc(n ? n : 1, sizeof *s->progs[m]);
    if (!s->progs[m]) continue;
    for (i = 0; i < n; ++i) {
      const struct scva_patch *p = &s->names.patch[s->lists.tone[m][i]];
      struct scva_program *e = &s->progs[m][i];
      scva_patch_label(p, e->name, sizeof e->name);
      e->d.bank = SCVA_BANK(p->bank, p->map, p->drum);
      e->d.program = p->prog;
      e->d.name = e->name;
    }
    s->nprogs[m] = n;
  }
}

/* The Drum Kit port's kit in tone map m, or Standard where m has no kit of
   that number, on the drum part. Sent when either changes. */
static void send_kit(struct scva *s, int m)
{
  const struct scva_patch *p;
  int want = s->kit_port ? (int)(*s->kit_port + 0.5f) : 0, k;
  if (want < 0) want = 0;
  if (want > 127) want = 127;
  k = scva_lists_find(&s->names, s->lists.kit[m], s->lists.nkit[m], 0, want);
  if (k < 0) k = scva_lists_find(&s->names, s->lists.kit[m], s->lists.nkit[m], 0, 0);
  if (k < 0 || (int)s->lists.kit[m][k] == s->kit_sent) return;
  p = &s->names.patch[s->lists.kit[m][k]];
  scva_engine_patch(s->eng, SCVA_DRUM_CH, p->map, p->bank, p->prog);
  s->kit_sent = (int)s->lists.kit[m][k];
}

static const LV2_Program_Descriptor *get_program(LV2_Handle h, uint32_t index)
{
  struct scva *s = h;
  int m = s->prog_map;
  return index < s->nprogs[m] ? &s->progs[m][index].d : NULL;
}

static void select_program(LV2_Handle h, uint32_t bank, uint32_t program)
{
  struct scva *s = h;
  int m = s->prog_map;
  uint32_t i;
  for (i = 0; i < s->nprogs[m]; ++i)
    if (s->progs[m][i].d.bank == bank && s->progs[m][i].d.program == program)
      s->tone_pos = (int)i;
  s->sel_bank = bank;
  s->sel_prog = program;
  s->sel_pending = 1;
}

/* The worker thread: tells the host to fetch the new map's list. */
static LV2_Worker_Status work(LV2_Handle h, LV2_Worker_Respond_Function respond,
                              LV2_Worker_Respond_Handle rh, uint32_t size,
                              const void *data)
{
  struct scva *s = h;
  (void)respond; (void)rh; (void)size; (void)data;
  if (s->programs_host && s->programs_host->program_changed)
    s->programs_host->program_changed(s->programs_host->handle, -1);
  return LV2_WORKER_SUCCESS;
}

static LV2_Worker_Status work_response(LV2_Handle h, uint32_t size,
                                       const void *body)
{
  (void)h; (void)size; (void)body;
  return LV2_WORKER_SUCCESS;
}

static const LV2_Worker_Interface worker_iface = { work, work_response, NULL };

/* A new tone map brings its own program list; the host's selected position
   in it is played, so what the host shows after reloading the list is what
   sounds. Called from run(). */
/* Part 1 to position k of the current map's tones, past the end meaning
   the last. Latched for the top of run(), like a host's selection. */
static void play_tone(struct scva *s, int k)
{
  int m = s->prog_map, n = (int)s->nprogs[m];
  if (k >= n) k = n - 1;
  s->tone_pos = k;
  s->sel_bank = s->progs[m][k].d.bank;
  s->sel_prog = s->progs[m][k].d.program;
  s->sel_pending = 1;
}

static void switch_programs(struct scva *s, int m)
{
  char none = 0;
  if (m == s->prog_map) return;
  s->prog_map = m;
  if (s->tone_pos >= 0 && s->nprogs[m])
    play_tone(s, s->tone_pos);
  if (s->worker) s->worker->schedule_work(s->worker->handle, sizeof none, &none);
}

/* The editor picked a position; a value the port already had is not news. */
static void follow_inst_port(struct scva *s)
{
  int v;
  if (!s->inst_port) return;
  v = (int)(*s->inst_port + 0.5f);
  if (v < 0) v = 0;
  if (v == s->inst_seen) return;
  s->inst_seen = v;
  if (s->nprogs[s->prog_map]) play_tone(s, v);
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

  s = calloc(1, sizeof *s);
  if (!s) return NULL;
  for (i = 0; features && features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID__map))
      map = (LV2_URID_Map *)features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_WORKER__schedule))
      s->worker = features[i]->data;
    else if (!strcmp(features[i]->URI, LV2_PROGRAMS__Host))
      s->programs_host = features[i]->data;
  }
  if (!map) { free(s); return NULL; }    /* urid:map is required */
  s->midi_event = map->map(map->handle, LV2_MIDI__MidiEvent);
  s->tone_pos = -1;
  s->inst_seen = -1;
  s->kit_sent = -1;

  /* the bundle path keeps its trailing slash */
  snprintf(here, sizeof here, "%s", bundle ? bundle : "");
  n = strlen(here);
  while (n > 1 && here[n - 1] == '/') here[--n] = '\0';
  scva_core_dir(here, dir, sizeof dir);

  /* The instrument and kit lists are the install's own: nothing of Roland's
     ships with this plugin, so without them there is nothing to offer. */
  scva_names_load(&s->names, dir);
  if (!s->names.npatch || !strcmp(s->names.module, "General MIDI")) {
    fprintf(stderr, "scva.lv2: no SOUND Canvas VA name files (SCVSC.tnf, "
            "SCVSC.drf) beside SCCore.dll in %s\n", dir);
    scva_names_free(&s->names);
    free(s);
    return NULL;
  }
  s->eng = scva_engine_open(dir, rate, err, sizeof err);
  if (!s->eng) {
    fprintf(stderr, "scva.lv2: %s\n", err);
    scva_names_free(&s->names);
    free(s);
    return NULL;
  }
  build_programs(s);
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
  case PORT_KIT:     s->kit_port = (const float *)data; break;
  case PORT_INST:    s->inst_port = (const float *)data; break;
  case PORT_TONE:    s->tone_port = (float *)data; break;
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
  {
    int map = s->map_port ? (int)(*s->map_port + 0.5f) : 0;
    if (map < 0) map = 0;
    if (map > SCVA_MAP_8820) map = SCVA_MAP_8820;
    scva_engine_set_map(s->eng, map);
    switch_programs(s, map);
    send_kit(s, map);
  }
  follow_inst_port(s);
  if (s->sel_pending) apply_selection(s);
  if (s->tone_port) *s->tone_port = (float)s->tone_pos;

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
  int m;
  if (!s) return;
  scva_engine_close(s->eng);
  scva_names_free(&s->names);
  for (m = 0; m < SCVA_MAPS; ++m) free(s->progs[m]);
  scva_lists_free(&s->lists);
  free(s);
}

static const void *extension_data(const char *uri)
{
  if (!strcmp(uri, LV2_PROGRAMS__Interface)) return &programs_iface;
  if (!strcmp(uri, LV2_WORKER__interface)) return &worker_iface;
  return NULL;
}

static const LV2_Descriptor descriptor = {
  SCVA_URI, instantiate, connect_port, NULL, run, NULL, cleanup, extension_data
};

LV2_SYMBOL_EXPORT const LV2_Descriptor *lv2_descriptor(uint32_t index)
{
  return index == 0 ? &descriptor : NULL;
}
