/* SPDX-License-Identifier: CC0-1.0
 *
 * SOUND Canvas VA as a CLAP instrument: MIDI in, stereo out.
 *
 * CLAP rather than VST3 because its SDK is MIT and this project is CC0;
 * VST3's is GPLv3 or a proprietary Steinberg licence, either of which would
 * force a relicence. A CLAP-to-VST3 wrapper exists if VST3 is needed.
 *
 * The engine is Roland's, loaded at activate() time from your own SCCore.dll.
 * Nothing of Roland's is distributed here: the core is looked for in
 * $SCVA_DLL_DIR, then beside the plugin. Driving it is scva_engine.c's job,
 * shared with the LV2 plugin.
 */
#define _GNU_SOURCE
#include "scva_engine.h"
#include "scva_map.h"
#include "scva_names.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>

#ifdef _WIN32
#include <windows.h>
#else
#include "scva_xui.h"
#endif

#define SCVA_ID "com.github.mlainez.scva-headless"

struct scva {
   clap_plugin_t plugin;
   const clap_host_t *host;

   struct scva_engine *eng;     /* NULL while deactivated */
   char dir[1024];              /* the core's and the name files' */
   int map_want;

   struct scva_names names;
   /* A preset arrives on the main thread; it is latched and applied at the
      top of the next process(). */
   int sel_map, sel_bank, sel_prog, sel_drum, sel_pending;
   /* The kit on the drum part and the map it came from, for note names. A
      preset names its own map, which need not be the one the Tone Map
      parameter holds for everything else. */
   int kit_now, kit_map;
   /* The Instrument and Drum Kit parameters are positions in list_map's
      lists, which follow the Tone Map. sent is the names.patch index last
      sent to the engine, -1 before any. */
   struct scva_lists lists;
   int list_map;
   int inst_want, drum_want, inst_sent, drum_sent;
   volatile int kit_names_changed, lists_changed;
   /* Parameters changed in the editor, one bit per id, still to be reported
      to the host. */
   volatile unsigned gui_dirty;
#ifdef _WIN32
   HWND gui, gui_ctl[3];
   int gui_shown[3];
   uint32_t gui_w, gui_h;       /* 0 until the host sizes the editor */
   int gui_lw, gui_lh;          /* the size last laid out */
   int gui_dpi;                 /* 0 until first asked */
   int gui_list_map;            /* whose patches the editor lists */
#else
   struct scva_xui *xui;
   clap_id xui_timer;
#endif
};

static void apply_selection(struct scva *s, const clap_output_events_t *out);

/* ---- finding and opening the core -------------------------------------- */

/* The directory holding the .clap, from the path the host passes to init. */
static char g_plugin_dir[1024];

/* A preset is four numbers; the host only ever passes the string back. */
static void load_key_make(char *buf, size_t cap, const struct scva_patch *p)
{
   snprintf(buf, cap, "%u.%u.%u.%c", p->map, p->bank, p->prog,
            p->drum ? 'd' : 't');
}

static int load_key_parse(const char *k, int *map, int *bank, int *prog,
                          int *drum)
{
   char kind = 't';
   if (!k || sscanf(k, "%d.%d.%d.%c", map, bank, prog, &kind) < 3) return 0;
   *drum = (kind == 'd');
   return *map >= 0 && *map < SCVA_MAPS && *bank >= 0 && *bank < 128 &&
          *prog >= 0 && *prog < 128;
}

/* ---- parameters -------------------------------------------------------- */

/* 0 Tone Map, for every part.
   1 Instrument, a melodic tone of that map, on the first part.
   2 Drum Kit, a kit of that map, on the drum part.
   A parameter's range cannot change while the plugin is active, so 1 and 2
   span the longest map's list and a position past a shorter one means its
   last entry. */
#define SCVA_PARAM_MAP  0
#define SCVA_PARAM_INST 1
#define SCVA_PARAM_DRUM 2

static int clamp_step(double v, int hi)
{
   int k = (int)(v + 0.5);
   return k < 0 ? 0 : k > hi ? hi : k;
}

/* The patch at position k of the current map's tones or kits, NULL if that
   list is empty. */
static const struct scva_patch *tone_at(const struct scva *s, int k)
{
   uint32_t n = s->lists.ntone[s->list_map];
   if (!n) return NULL;
   return &s->names.patch[s->lists.tone[s->list_map][clamp_step(k, (int)n - 1)]];
}

static const struct scva_patch *kit_at(const struct scva *s, int k)
{
   uint32_t n = s->lists.nkit[s->list_map];
   if (!n) return NULL;
   return &s->names.patch[s->lists.kit[s->list_map][clamp_step(k, (int)n - 1)]];
}

/* Changes are latched here and sent to the engine by process(). */
static void param_event(struct scva *s, const clap_event_header_t *h)
{
   const clap_event_param_value_t *e = (const clap_event_param_value_t *)h;
   if (h->space_id != CLAP_CORE_EVENT_SPACE_ID ||
       h->type != CLAP_EVENT_PARAM_VALUE) return;
   if (e->param_id == SCVA_PARAM_MAP)
      s->map_want = clamp_step(e->value, SCVA_MAP_8820);
   else if (e->param_id == SCVA_PARAM_INST && s->lists.ntone_max)
      s->inst_want = clamp_step(e->value, (int)s->lists.ntone_max - 1);
   else if (e->param_id == SCVA_PARAM_DRUM && s->lists.nkit_max)
      s->drum_want = clamp_step(e->value, (int)s->lists.nkit_max - 1);
}

static void push_value(const clap_output_events_t *out, clap_id id, double v)
{
   clap_event_param_value_t e;
   if (!out) return;
   memset(&e, 0, sizeof e);
   e.header.size = sizeof e;
   e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
   e.header.type = CLAP_EVENT_PARAM_VALUE;
   e.param_id = id;
   e.note_id = -1;
   e.port_index = -1;
   e.channel = -1;
   e.key = -1;
   e.value = v;
   out->try_push(out, &e.header);
}

/* A value set in the editor is the plugin's own change, which the host has
   to be told about like any other. */
static void report_gui(struct scva *s, const clap_output_events_t *out)
{
   unsigned d = s->gui_dirty;
   if (!d) return;
   s->gui_dirty &= ~d;
   if (d & (1u << SCVA_PARAM_MAP))  push_value(out, SCVA_PARAM_MAP, s->map_want);
   if (d & (1u << SCVA_PARAM_INST)) push_value(out, SCVA_PARAM_INST, s->inst_want);
   if (d & (1u << SCVA_PARAM_DRUM)) push_value(out, SCVA_PARAM_DRUM, s->drum_want);
}

/* A new Tone Map brings that map's tones and kits. The Instrument and Drum
   Kit move to the same patch in it, and the host is told: of the new values,
   and on the main thread that every value now reads differently. */
static void switch_lists(struct scva *s, const clap_output_events_t *out)
{
   int m = s->map_want;
   const struct scva_patch *t, *k;
   if (m == s->list_map) return;
   t = tone_at(s, s->inst_want);
   k = kit_at(s, s->drum_want);
   if (t) s->inst_want = scva_lists_carry(&s->names, t, s->lists.tone[m],
                                          s->lists.ntone[m]);
   if (k) s->drum_want = scva_lists_carry(&s->names, k, s->lists.kit[m],
                                          s->lists.nkit[m]);
   s->list_map = m;
   push_value(out, SCVA_PARAM_INST, s->inst_want);
   push_value(out, SCVA_PARAM_DRUM, s->drum_want);
   s->lists_changed = 1;
   if (s->host && s->host->request_callback)
      s->host->request_callback(s->host);
}

static void send_choices(struct scva *s)
{
   const struct scva_patch *p = tone_at(s, s->inst_want);
   if (p && p - s->names.patch != s->inst_sent) {
      scva_engine_patch(s->eng, SCVA_TONE_CH, p->map, p->bank, p->prog);
      s->inst_sent = (int)(p - s->names.patch);
   }
   p = kit_at(s, s->drum_want);
   if (p && p - s->names.patch != s->drum_sent) {
      scva_engine_patch(s->eng, SCVA_DRUM_CH, p->map, p->bank, p->prog);
      s->drum_sent = (int)(p - s->names.patch);
      s->kit_now = p->prog;
      s->kit_map = p->map;
      s->kit_names_changed = 1;
      if (s->host && s->host->request_callback)
         s->host->request_callback(s->host);
   }
}

/* ---- clap_plugin ------------------------------------------------------- */

/* Position of the reset engine's patch, program 0 bank 0, in a list. */
static int reset_patch(const struct scva *s, const uint32_t *list, uint32_t n)
{
   int k = scva_lists_find(&s->names, list, n, 0, 0);
   return k < 0 ? 0 : k;
}

/* Names come from text files beside the core, not from the core, so they load
   without it. Absent, the plugin still plays; it just offers no presets. */
static bool plug_init(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   scva_core_dir(g_plugin_dir, s->dir, sizeof s->dir);
   scva_names_load(&s->names, s->dir);
   scva_lists_build(&s->lists, &s->names);
   s->list_map = SCVA_MAP_DEFAULT;
   s->inst_want = reset_patch(s, s->lists.tone[s->list_map],
                              s->lists.ntone[s->list_map]);
   s->drum_want = reset_patch(s, s->lists.kit[s->list_map],
                              s->lists.nkit[s->list_map]);
   return true;
}

static void plug_destroy(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   scva_engine_close(s->eng);
   scva_names_free(&s->names);
   scva_lists_free(&s->lists);
   free(s);
}

static bool plug_activate(const clap_plugin_t *p, double rate,
                          uint32_t min, uint32_t max)
{
   struct scva *s = p->plugin_data;
   char err[256];
   (void)min; (void)max;
   s->eng = scva_engine_open(s->dir, rate, err, sizeof err);
   if (!s->eng) { fprintf(stderr, "scva: %s\n", err); return false; }
   s->inst_sent = s->drum_sent = -1;
   return true;
}

static void plug_deactivate(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   scva_engine_close(s->eng);
   s->eng = NULL;
}

static bool plug_start_processing(const clap_plugin_t *p) { (void)p; return true; }
static void plug_stop_processing(const clap_plugin_t *p) { (void)p; }
static void plug_reset(const clap_plugin_t *p) { (void)p; }

static clap_process_status plug_process(const clap_plugin_t *p,
                                        const clap_process_t *pr)
{
   struct scva *s = p->plugin_data;
   uint32_t at = 0, ei = 0, n = pr->frames_count;
   uint32_t nev = pr->in_events ? pr->in_events->size(pr->in_events) : 0;
   float *l, *r;

   /* The host's audio thread, not the one that opened the engine. */
   if (!s->eng || !scva_engine_thread()) return CLAP_PROCESS_ERROR;
   if (s->sel_pending) apply_selection(s, pr->out_events);

   /* While processing, the host sends parameter changes here, not to
      flush(). The map is applied for the whole block, ahead of its notes. */
   for (; ei < nev; ++ei)
      param_event(s, pr->in_events->get(pr->in_events, ei));
   ei = 0;
   report_gui(s, pr->out_events);
   switch_lists(s, pr->out_events);

   if (!pr->audio_outputs_count || pr->audio_outputs[0].channel_count < 2)
      return CLAP_PROCESS_ERROR;
   l = pr->audio_outputs[0].data32[0];
   r = pr->audio_outputs[0].data32[1];

   scva_engine_set_map(s->eng, s->map_want);
   send_choices(s);

   for (; ei < nev; ++ei) {
      const clap_event_header_t *h = pr->in_events->get(pr->in_events, ei);
      uint32_t t = h->time > n ? n : h->time;
      if (t > at) { scva_engine_render(s->eng, l + at, r + at, t - at); at = t; }
      if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
      if (h->type == CLAP_EVENT_MIDI) {
         const clap_event_midi_t *m = (const clap_event_midi_t *)h;
         scva_engine_midi(s->eng, m->data, 3);
      } else if (h->type == CLAP_EVENT_MIDI_SYSEX) {
         const clap_event_midi_sysex_t *m = (const clap_event_midi_sysex_t *)h;
         scva_engine_midi(s->eng, m->buffer, m->size);
      }
   }
   if (at < n) scva_engine_render(s->eng, l + at, r + at, n - at);
   return CLAP_PROCESS_CONTINUE;
}

/* ---- ports ------------------------------------------------------------- */

static uint32_t ap_count(const clap_plugin_t *p, bool input)
{ (void)p; return input ? 0 : 1; }

static bool ap_get(const clap_plugin_t *p, uint32_t i, bool input,
                   clap_audio_port_info_t *info)
{
   (void)p;
   if (input || i) return false;
   info->id = 0;
   snprintf(info->name, sizeof info->name, "Out");
   info->channel_count = 2;
   info->flags = CLAP_AUDIO_PORT_IS_MAIN;
   info->port_type = CLAP_PORT_STEREO;
   info->in_place_pair = CLAP_INVALID_ID;
   return true;
}
static const clap_plugin_audio_ports_t s_audio_ports = { ap_count, ap_get };

static uint32_t np_count(const clap_plugin_t *p, bool input)
{ (void)p; return input ? 1 : 0; }

static bool np_get(const clap_plugin_t *p, uint32_t i, bool input,
                   clap_note_port_info_t *info)
{
   (void)p;
   if (!input || i) return false;
   info->id = 0;
   info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
   info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
   snprintf(info->name, sizeof info->name, "MIDI In");
   return true;
}
static const clap_plugin_note_ports_t s_note_ports = { np_count, np_get };

/* ---- the parameters' host side ----------------------------------------- */

static uint32_t pr_count(const clap_plugin_t *p) { (void)p; return 3; }

static bool pr_info(const clap_plugin_t *p, uint32_t i,
                    clap_param_info_t *info)
{
   struct scva *s = p->plugin_data;
   static const char *const name[3] = { "Tone Map", "Instrument", "Drum Kit" };
   if (i > SCVA_PARAM_DRUM) return false;
   memset(info, 0, sizeof *info);
   info->id = i;
   info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
   info->min_value = 0;
   if (i == SCVA_PARAM_MAP) {
      info->max_value = SCVA_MAP_8820;
      info->default_value = 0;
   } else if (i == SCVA_PARAM_INST) {
      info->max_value = s->lists.ntone_max ? s->lists.ntone_max - 1 : 0;
      info->default_value = reset_patch(s, s->lists.tone[SCVA_MAP_DEFAULT],
                                        s->lists.ntone[SCVA_MAP_DEFAULT]);
   } else {
      info->max_value = s->lists.nkit_max ? s->lists.nkit_max - 1 : 0;
      info->default_value = reset_patch(s, s->lists.kit[SCVA_MAP_DEFAULT],
                                        s->lists.nkit[SCVA_MAP_DEFAULT]);
   }
   snprintf(info->name, sizeof info->name, "%s", name[i]);
   return true;
}

static bool pr_value(const clap_plugin_t *p, clap_id id, double *out)
{
   struct scva *s = p->plugin_data;
   if (id == SCVA_PARAM_MAP) *out = s->map_want;
   else if (id == SCVA_PARAM_INST) *out = s->inst_want;
   else if (id == SCVA_PARAM_DRUM) *out = s->drum_want;
   else return false;
   return true;
}

static const char *const s_map_names[SCVA_MAPS] = {
   "Default", "SC-55", "SC-88", "SC-88Pro", "SC-8820"
};

static bool pr_text(const clap_plugin_t *p, clap_id id, double v,
                    char *out, uint32_t cap)
{
   struct scva *s = p->plugin_data;
   const struct scva_patch *pt;
   if (id == SCVA_PARAM_MAP) {
      snprintf(out, cap, "%s", s_map_names[clamp_step(v, SCVA_MAP_8820)]);
      return true;
   }
   pt = id == SCVA_PARAM_INST ? tone_at(s, clamp_step(v, INT32_MAX)) :
        id == SCVA_PARAM_DRUM ? kit_at(s, clamp_step(v, INT32_MAX)) : NULL;
   if (!pt) return false;
   scva_patch_label(pt, out, cap);
   return true;
}

static bool pr_from_text(const clap_plugin_t *p, clap_id id, const char *t,
                         double *out)
{
   struct scva *s = p->plugin_data;
   int m = s->list_map;
   const uint32_t *list = id == SCVA_PARAM_INST ? s->lists.tone[m] : s->lists.kit[m];
   uint32_t i, n = id == SCVA_PARAM_INST ? s->lists.ntone[m] : s->lists.nkit[m];
   char label[SCVA_NAME_MAX + 16];

   if (!t) return false;
   if (id == SCVA_PARAM_MAP) {
      int v = scva_map_value(t);
      if (v < 0) return false;
      *out = v;
      return true;
   }
   if (id != SCVA_PARAM_INST && id != SCVA_PARAM_DRUM) return false;
   for (i = 0; i < n; ++i) {
      scva_patch_label(&s->names.patch[list[i]], label, sizeof label);
      if (!strcmp(label, t)) { *out = i; return true; }
   }
   return false;
}

static void pr_flush(const clap_plugin_t *p, const clap_input_events_t *in,
                     const clap_output_events_t *out)
{
   struct scva *s = p->plugin_data;
   uint32_t i, n = in ? in->size(in) : 0;
   for (i = 0; i < n; ++i) param_event(s, in->get(in, i));
   report_gui(s, out);
   switch_lists(s, out);
}
static const clap_plugin_params_t s_params = {
   pr_count, pr_info, pr_value, pr_text, pr_from_text, pr_flush
};

/* ---- presets ----------------------------------------------------------- */

/* A preset of the current map also becomes the Instrument or Drum Kit
   value, so the two ways of choosing a patch agree. */
static void apply_selection(struct scva *s, const clap_output_events_t *out)
{
   int ch = s->sel_drum ? SCVA_DRUM_CH : SCVA_TONE_CH;
   int m = s->list_map, k;
   const uint32_t *list = s->sel_drum ? s->lists.kit[m] : s->lists.tone[m];
   uint32_t n = s->sel_drum ? s->lists.nkit[m] : s->lists.ntone[m];

   s->sel_pending = 0;
   if (!s->eng) return;                    /* not activated yet */
   scva_engine_patch(s->eng, ch, s->sel_map, s->sel_bank, s->sel_prog);
   if (s->sel_drum) {
      s->kit_now = s->sel_prog;
      s->kit_map = s->sel_map;
   }
   k = scva_lists_find(&s->names, list, n, s->sel_bank, s->sel_prog);
   if (k < 0 || s->names.patch[list[k]].map != s->sel_map) return;
   if (s->sel_drum) {
      s->drum_want = k;
      s->drum_sent = (int)list[k];
      push_value(out, SCVA_PARAM_DRUM, k);
   } else {
      s->inst_want = k;
      s->inst_sent = (int)list[k];
      push_value(out, SCVA_PARAM_INST, k);
   }
}

/* Main thread only. */
static void kit_names_changed(struct scva *s)
{
   const clap_host_note_name_t *nn;
   if (!s->host || !s->host->get_extension) return;
   nn = s->host->get_extension(s->host, CLAP_EXT_NOTE_NAME);
   if (nn && nn->changed) nn->changed(s->host);
}

static bool preset_from_location(const clap_plugin_t *p, uint32_t kind,
                                 const char *location, const char *load_key)
{
   struct scva *s = p->plugin_data;
   int map, bank, prog, drum;

   if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location) return false;
   if (!load_key_parse(load_key, &map, &bank, &prog, &drum)) return false;
   s->sel_map = map;
   s->sel_bank = bank;
   s->sel_prog = prog;
   s->sel_drum = drum;
   s->sel_pending = 1;
   if (drum) kit_names_changed(s);
   return true;
}

static const clap_plugin_preset_load_t s_preset_load = { preset_from_location };

/* The keys of whichever kit the drum part is holding. */
static uint32_t nn_count(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   uint32_t n = 0;
   size_t i;
   for (i = 0; i < s->names.nkey; ++i)
      if (s->names.key[i].map == s->kit_map &&
          s->names.key[i].prog == s->kit_now)
         ++n;
   return n;
}

static bool nn_get(const clap_plugin_t *p, uint32_t index,
                   clap_note_name_t *out)
{
   struct scva *s = p->plugin_data;
   uint32_t n = 0;
   size_t i;
   for (i = 0; i < s->names.nkey; ++i) {
      const struct scva_key *k = &s->names.key[i];
      if (k->map != s->kit_map || k->prog != s->kit_now) continue;
      if (n++ != index) continue;
      memset(out, 0, sizeof *out);
      snprintf(out->name, sizeof out->name, "%s", k->name);
      out->port = -1;
      out->key = k->key;
      out->channel = SCVA_DRUM_CH;
      return true;
   }
   return false;
}

static const clap_plugin_note_name_t s_note_name = { nn_count, nn_get };

/* ---- the editor -------------------------------------------------------- */

/* A pick in the editor is the plugin's own parameter change: latched like
   the host's, then reported to the host from flush() or process(). Main
   thread. */
static void editor_set(struct scva *s, int id, int v)
{
   const clap_host_params_t *hp = NULL;
   if (id == SCVA_PARAM_MAP) s->map_want = v;
   else if (id == SCVA_PARAM_INST) s->inst_want = v;
   else s->drum_want = v;
   s->gui_dirty |= 1u << id;
   if (s->host->get_extension)
      hp = s->host->get_extension(s->host, CLAP_EXT_PARAMS);
   if (hp && hp->request_flush) hp->request_flush(s->host);
}

#ifdef _WIN32
/* Plain Win32 controls, so it runs wherever the plugin does, Windows 98
   included: a host's generic slider cannot step through thousands of
   patches, a list can. The window polls the parameters, so a change from the
   host or from automation shows here too. It resizes, the Instrument list
   taking whatever room there is; this is also its smallest size.

   Every coordinate is at 96 DPI and scaled to the screen's: the font is the
   system's and grows with the DPI, so a layout in plain pixels clips its own
   labels. */
#define SCVA_GUI_W 390
#define SCVA_GUI_H 286
#define SCVA_GUI_CLASS "scva-headless editor"
#define SCVA_GUI_TIMER 1

/* The class belongs to this DLL, not the host, so that it can be unregistered
   before the DLL goes away. GetModuleHandleEx is not on Windows 98. */
static HINSTANCE gui_module(void)
{
   MEMORY_BASIC_INFORMATION m;
   if (!VirtualQuery((LPCVOID)gui_module, &m, sizeof m)) return NULL;
   return (HINSTANCE)m.AllocationBase;
}

/* v, a length at 96 DPI, at the screen's DPI. */
static int px(struct scva *s, int v)
{
   if (!s->gui_dpi) {
      HDC dc = GetDC(NULL);
      s->gui_dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
      if (dc) ReleaseDC(NULL, dc);
      if (s->gui_dpi <= 0) s->gui_dpi = 96;
   }
   return MulDiv(v, s->gui_dpi, 96);
}

static void gui_select(struct scva *s, int id, int v)
{
   UINT msg = id == SCVA_PARAM_INST ? LB_SETCURSEL : CB_SETCURSEL;
   if (!s->gui_ctl[id] || s->gui_shown[id] == v) return;
   SendMessageA(s->gui_ctl[id], msg, (WPARAM)v, 0);
   s->gui_shown[id] = v;
}

/* The Drum Kit and Instrument lists hold the current map's patches. */
static void gui_fill(struct scva *s)
{
   int m = s->list_map, i;
   char label[SCVA_NAME_MAX + 16];
   HWND kit = s->gui_ctl[SCVA_PARAM_DRUM], tone = s->gui_ctl[SCVA_PARAM_INST];

   SendMessageA(tone, WM_SETREDRAW, FALSE, 0);
   SendMessageA(kit, CB_RESETCONTENT, 0, 0);
   SendMessageA(tone, LB_RESETCONTENT, 0, 0);
   for (i = 0; i < (int)s->lists.nkit[m]; ++i) {
      scva_patch_label(&s->names.patch[s->lists.kit[m][i]], label, sizeof label);
      SendMessageA(kit, CB_ADDSTRING, 0, (LPARAM)label);
   }
   for (i = 0; i < (int)s->lists.ntone[m]; ++i) {
      scva_patch_label(&s->names.patch[s->lists.tone[m][i]], label, sizeof label);
      SendMessageA(tone, LB_ADDSTRING, 0, (LPARAM)label);
   }
   SendMessageA(tone, WM_SETREDRAW, TRUE, 0);
   InvalidateRect(tone, NULL, TRUE);
   s->gui_list_map = m;
   for (i = 0; i < 3; ++i) s->gui_shown[i] = -1;
}

static void gui_sync(struct scva *s)
{
   int nt = (int)s->lists.ntone[s->list_map], nk = (int)s->lists.nkit[s->list_map];
   if (s->gui_list_map != s->list_map) gui_fill(s);
   gui_select(s, SCVA_PARAM_MAP, s->map_want);
   /* a position past a shorter list shows its last entry, which is what plays */
   if (nt) gui_select(s, SCVA_PARAM_INST, clamp_step(s->inst_want, nt - 1));
   if (nk) gui_select(s, SCVA_PARAM_DRUM, clamp_step(s->drum_want, nk - 1));
}

static void gui_changed(struct scva *s, int id)
{
   UINT msg = id == SCVA_PARAM_INST ? LB_GETCURSEL : CB_GETCURSEL;
   LRESULT v = SendMessageA(s->gui_ctl[id], msg, 0, 0);
   if (v < 0) return;
   s->gui_shown[id] = (int)v;
   editor_set(s, id, (int)v);
}

static void gui_layout(struct scva *s);

static LRESULT CALLBACK gui_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
   struct scva *s = (struct scva *)GetWindowLongPtrA(w, GWLP_USERDATA);
   if (msg == WM_CREATE) {
      s = ((CREATESTRUCTA *)lp)->lpCreateParams;
      SetWindowLongPtrA(w, GWLP_USERDATA, (LONG_PTR)s);
      return 0;
   }
   if (!s) return DefWindowProcA(w, msg, wp, lp);
   if (msg == WM_COMMAND) {
      int id = LOWORD(wp), code = HIWORD(wp);
      if (id <= SCVA_PARAM_DRUM &&
          (code == CBN_SELCHANGE || code == LBN_SELCHANGE))
         gui_changed(s, id);
      return 0;
   }
   if (msg == WM_TIMER) { gui_layout(s); gui_sync(s); return 0; }
   if (msg == WM_CTLCOLORSTATIC)
      return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
   return DefWindowProcA(w, msg, wp, lp);
}

static HWND gui_child(struct scva *s, const char *cls, DWORD style, int id,
                      int x, int y, int w, int h, const char *text)
{
   HWND c = CreateWindowExA(cls[0] == 'S' ? 0 : WS_EX_CLIENTEDGE, cls, text,
                            WS_CHILD | WS_VISIBLE | style, px(s, x), px(s, y),
                            px(s, w), px(s, h), s->gui,
                            (HMENU)(INT_PTR)id, gui_module(), NULL);
   if (c) SendMessageA(c, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT),
                       TRUE);
   return c;
}

static bool gui_api(const clap_plugin_t *p, const char *api, bool floating)
{
   (void)p;
   return !floating && !strcmp(api, CLAP_WINDOW_API_WIN32);
}

static bool gui_preferred(const clap_plugin_t *p, const char **api,
                          bool *floating)
{
   (void)p;
   *api = CLAP_WINDOW_API_WIN32;
   *floating = false;
   return true;
}

static bool gui_create(const clap_plugin_t *p, const char *api, bool floating)
{
   return gui_api(p, api, floating);
}

static void gui_destroy(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   if (s->gui) {
      KillTimer(s->gui, SCVA_GUI_TIMER);
      DestroyWindow(s->gui);
   }
   s->gui = NULL;
   s->gui_lw = s->gui_lh = 0;
   memset(s->gui_ctl, 0, sizeof s->gui_ctl);
}

static bool gui_scale(const clap_plugin_t *p, double scale)
{ (void)p; (void)scale; return false; }

/* The drop-downs stretch with the width, the list with both. */
/* The editor fills the window the host puts it in, whatever size the host
   made that - SAVIHost restores its own last window size, which need not be
   the editor's - down to a floor where every control is still usable.
   gui_w x gui_h stays the size asked for. Checked on every timer tick. */
#define SCVA_GUI_FLOOR_W 240
#define SCVA_GUI_FLOOR_H 150

static void gui_layout(struct scva *s)
{
   RECT r;
   int w = (int)s->gui_w, h = (int)s->gui_h;
   if (!s->gui) return;
   if (GetClientRect(GetParent(s->gui), &r) && r.right > 0 && r.bottom > 0) {
      w = r.right;
      h = r.bottom;
   }
   if (w < px(s, SCVA_GUI_FLOOR_W)) w = px(s, SCVA_GUI_FLOOR_W);
   if (h < px(s, SCVA_GUI_FLOOR_H)) h = px(s, SCVA_GUI_FLOOR_H);
   if (w == s->gui_lw && h == s->gui_lh) return;
   s->gui_lw = w;
   s->gui_lh = h;
   SetWindowPos(s->gui, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
   SetWindowPos(s->gui_ctl[SCVA_PARAM_MAP], NULL, px(s, 90), px(s, 10),
                w - px(s, 100), px(s, 200), SWP_NOZORDER);
   SetWindowPos(s->gui_ctl[SCVA_PARAM_DRUM], NULL, px(s, 90), px(s, 40),
                w - px(s, 100), px(s, 300), SWP_NOZORDER);
   SetWindowPos(s->gui_ctl[SCVA_PARAM_INST], NULL, px(s, 10), px(s, 92),
                w - px(s, 20), h - px(s, 100), SWP_NOZORDER);
}

static bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
   struct scva *s = p->plugin_data;
   *w = s->gui_w ? s->gui_w : (uint32_t)px(s, SCVA_GUI_W);
   *h = s->gui_h ? s->gui_h : (uint32_t)px(s, SCVA_GUI_H);
   return true;
}

static bool gui_can_resize(const clap_plugin_t *p) { (void)p; return true; }

static bool gui_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *h)
{
   (void)p;
   memset(h, 0, sizeof *h);
   h->can_resize_horizontally = true;
   h->can_resize_vertically = true;
   return true;
}

static bool gui_adjust(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
   struct scva *s = p->plugin_data;
   uint32_t mw = (uint32_t)px(s, SCVA_GUI_W), mh = (uint32_t)px(s, SCVA_GUI_H);
   if (*w < mw) *w = mw;
   if (*h < mh) *h = mh;
   return true;
}

static bool gui_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h)
{
   struct scva *s = p->plugin_data;
   gui_adjust(p, &w, &h);
   s->gui_w = w;
   s->gui_h = h;
   gui_layout(s);
   return true;
}

static bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *win)
{
   struct scva *s = p->plugin_data;
   HINSTANCE inst = gui_module();
   WNDCLASSA wc;
   uint32_t i;

   if (s->gui) return false;
   if (!GetClassInfoA(inst, SCVA_GUI_CLASS, &wc)) {
      memset(&wc, 0, sizeof wc);
      wc.lpfnWndProc = gui_proc;
      wc.hInstance = inst;
      wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
      wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
      wc.lpszClassName = SCVA_GUI_CLASS;
      if (!RegisterClassA(&wc)) return false;
   }
   gui_get_size(p, &s->gui_w, &s->gui_h);
   s->gui = CreateWindowExA(0, SCVA_GUI_CLASS, "", WS_CHILD | WS_VISIBLE |
                            WS_CLIPCHILDREN, 0, 0, (int)s->gui_w,
                            (int)s->gui_h, (HWND)win->win32, NULL, inst, s);
   if (!s->gui) return false;

   gui_child(s, "STATIC", 0, -1, 10, 13, 80, 16, "Tone Map");
   s->gui_ctl[SCVA_PARAM_MAP] = gui_child(s, "COMBOBOX", CBS_DROPDOWNLIST |
      WS_VSCROLL | WS_TABSTOP, SCVA_PARAM_MAP, 90, 10, 290, 200, "");
   gui_child(s, "STATIC", 0, -1, 10, 43, 80, 16, "Drum Kit");
   s->gui_ctl[SCVA_PARAM_DRUM] = gui_child(s, "COMBOBOX", CBS_DROPDOWNLIST |
      WS_VSCROLL | WS_TABSTOP, SCVA_PARAM_DRUM, 90, 40, 290, 300, "");
   gui_child(s, "STATIC", 0, -1, 10, 73, 370, 16, "Instrument (first part)");
   s->gui_ctl[SCVA_PARAM_INST] = gui_child(s, "LISTBOX", LBS_NOTIFY |
      LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_TABSTOP, SCVA_PARAM_INST,
      10, 92, 370, 186, "");

   for (i = 0; i < SCVA_MAPS; ++i)
      SendMessageA(s->gui_ctl[SCVA_PARAM_MAP], CB_ADDSTRING, 0,
                   (LPARAM)s_map_names[i]);
   gui_fill(s);
   gui_layout(s);
   gui_sync(s);
   /* A host may restore a window size saved before the editor's size
      changed - at another DPI, say - and leave the editor clipped. */
   {
      const clap_host_gui_t *hg = s->host->get_extension ?
         s->host->get_extension(s->host, CLAP_EXT_GUI) : NULL;
      if (hg && hg->request_resize)
         hg->request_resize(s->host, s->gui_w, s->gui_h);
   }
   SetTimer(s->gui, SCVA_GUI_TIMER, 100, NULL);
   return true;
}

static bool gui_transient(const clap_plugin_t *p, const clap_window_t *w)
{ (void)p; (void)w; return false; }

static void gui_title(const clap_plugin_t *p, const char *t)
{ (void)p; (void)t; }

static bool gui_show(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   if (s->gui) ShowWindow(s->gui, SW_SHOW);
   return s->gui != NULL;
}

static bool gui_hide(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   if (s->gui) ShowWindow(s->gui, SW_HIDE);
   return s->gui != NULL;
}

static const clap_plugin_gui_t s_gui = {
   gui_api, gui_preferred, gui_create, gui_destroy, gui_scale, gui_get_size,
   gui_can_resize, gui_hints, gui_adjust, gui_set_size, gui_set_parent,
   gui_transient, gui_title, gui_show, gui_hide
};
#else
/* scva_xui.c's window, shared with the LV2 UI. The host's timer drives it:
   each tick hands it the parameters and lets it handle its events. */
#define SCVA_XUI_PERIOD_MS 30

static void xui_map(void *ctx, int v)  { editor_set(ctx, SCVA_PARAM_MAP, v); }
static void xui_tone(void *ctx, int v) { editor_set(ctx, SCVA_PARAM_INST, v); }
static void xui_kit(void *ctx, int v)  { editor_set(ctx, SCVA_PARAM_DRUM, v); }

static const clap_host_timer_support_t *host_timer(const struct scva *s)
{
   return s->host->get_extension ?
      s->host->get_extension(s->host, CLAP_EXT_TIMER_SUPPORT) : NULL;
}

static void xui_tick(struct scva *s)
{
   if (!s->xui) return;
   scva_xui_show(s->xui, s->list_map, s->inst_want, s->drum_want);
   scva_xui_idle(s->xui);
}

static void on_timer(const clap_plugin_t *p, clap_id id)
{
   struct scva *s = p->plugin_data;
   if (id == s->xui_timer) xui_tick(s);
}
static const clap_plugin_timer_support_t s_timer = { on_timer };

static bool gui_api(const clap_plugin_t *p, const char *api, bool floating)
{
   (void)p;
   return !floating && !strcmp(api, CLAP_WINDOW_API_X11);
}

static bool gui_preferred(const clap_plugin_t *p, const char **api,
                          bool *floating)
{
   (void)p;
   *api = CLAP_WINDOW_API_X11;
   *floating = false;
   return true;
}

static bool gui_create(const clap_plugin_t *p, const char *api, bool floating)
{
   return gui_api(p, api, floating);
}

static void gui_destroy(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   const clap_host_timer_support_t *t = host_timer(s);
   if (s->xui_timer != CLAP_INVALID_ID && t && t->unregister_timer)
      t->unregister_timer(s->host, s->xui_timer);
   s->xui_timer = CLAP_INVALID_ID;
   scva_xui_close(s->xui);
   s->xui = NULL;
}

static bool gui_scale(const clap_plugin_t *p, double scale)
{ (void)p; (void)scale; return false; }

/* Before the window exists its size is not known; this is close to it. */
static bool gui_get_size(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
   struct scva *s = p->plugin_data;
   int iw = 690, ih = 510;
   if (s->xui) scva_xui_size(s->xui, &iw, &ih);
   *w = (uint32_t)iw;
   *h = (uint32_t)ih;
   return true;
}

static bool gui_can_resize(const clap_plugin_t *p) { (void)p; return true; }

static bool gui_hints(const clap_plugin_t *p, clap_gui_resize_hints_t *h)
{
   (void)p;
   memset(h, 0, sizeof *h);
   h->can_resize_horizontally = true;
   h->can_resize_vertically = true;
   return true;
}

static bool gui_adjust(const clap_plugin_t *p, uint32_t *w, uint32_t *h)
{
   (void)p;
   if (*w < 320) *w = 320;
   if (*h < 240) *h = 240;
   return true;
}

static bool gui_set_size(const clap_plugin_t *p, uint32_t w, uint32_t h)
{
   struct scva *s = p->plugin_data;
   gui_adjust(p, &w, &h);
   if (s->xui) scva_xui_resize(s->xui, (int)w, (int)h);
   return s->xui != NULL;
}

static bool gui_set_parent(const clap_plugin_t *p, const clap_window_t *win)
{
   struct scva *s = p->plugin_data;
   const clap_host_timer_support_t *t = host_timer(s);
   struct scva_xui_host h;
   if (s->xui || !t || !t->register_timer) return false;
   h.ctx = s;
   h.set_map = xui_map;
   h.set_tone = xui_tone;
   h.set_kit = xui_kit;
   s->xui = scva_xui_open((unsigned long)win->x11, &s->names, &s->lists, &h);
   if (!s->xui) return false;
   if (!t->register_timer(s->host, SCVA_XUI_PERIOD_MS, &s->xui_timer)) {
      scva_xui_close(s->xui);
      s->xui = NULL;
      return false;
   }
   xui_tick(s);
   return true;
}

static bool gui_transient(const clap_plugin_t *p, const clap_window_t *w)
{ (void)p; (void)w; return false; }

static void gui_title(const clap_plugin_t *p, const char *t)
{ (void)p; (void)t; }

static bool gui_show(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   if (s->xui) scva_xui_visible(s->xui, 1);
   return s->xui != NULL;
}

static bool gui_hide(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   if (s->xui) scva_xui_visible(s->xui, 0);
   return s->xui != NULL;
}

static const clap_plugin_gui_t s_gui = {
   gui_api, gui_preferred, gui_create, gui_destroy, gui_scale, gui_get_size,
   gui_can_resize, gui_hints, gui_adjust, gui_set_size, gui_set_parent,
   gui_transient, gui_title, gui_show, gui_hide
};
#endif

static const void *plug_get_extension(const clap_plugin_t *p, const char *id)
{
   (void)p;
   if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_audio_ports;
   if (!strcmp(id, CLAP_EXT_NOTE_PORTS))  return &s_note_ports;
   if (!strcmp(id, CLAP_EXT_PARAMS))      return &s_params;
   if (!strcmp(id, CLAP_EXT_NOTE_NAME))   return &s_note_name;
   if (!strcmp(id, CLAP_EXT_GUI))         return &s_gui;
#ifndef _WIN32
   if (!strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &s_timer;
#endif
   if (!strcmp(id, CLAP_EXT_PRESET_LOAD) ||
       !strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT))
      return &s_preset_load;
   return NULL;
}

/* The audio thread switches lists and kits; the host hears of the new
   parameter texts and key names from here. */
static void plug_on_main_thread(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   if (s->lists_changed) {
      const clap_host_params_t *hp = s->host->get_extension ?
         s->host->get_extension(s->host, CLAP_EXT_PARAMS) : NULL;
      s->lists_changed = 0;
      if (hp && hp->rescan)
         hp->rescan(s->host, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
   }
   if (s->kit_names_changed) {
      s->kit_names_changed = 0;
      kit_names_changed(s);
   }
}

static const char *const s_features[] = {
   CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER,
   CLAP_PLUGIN_FEATURE_STEREO, NULL
};

static const clap_plugin_descriptor_t s_descriptor = {
   .clap_version = CLAP_VERSION_INIT,
   .id = SCVA_ID,
   .name = "SCVA",
   .vendor = "scva-headless",
   .url = "https://github.com/mlainez/scva-headless",
   .manual_url = "",
   .support_url = "",
   .version = "1.0",
   .description = "Roland SOUND Canvas VA's engine, loaded from your own "
                  "SCCore.dll. Set SCVA_DLL_DIR or put it beside the plugin.",
   .features = s_features
};

static const clap_plugin_t *make_plugin(const clap_host_t *host)
{
   struct scva *s = calloc(1, sizeof *s);
   if (!s) return NULL;
   s->host = host;
   s->map_want = 0;
#ifndef _WIN32
   s->xui_timer = CLAP_INVALID_ID;
#endif
   s->plugin.desc = &s_descriptor;
   s->plugin.plugin_data = s;
   s->plugin.init = plug_init;
   s->plugin.destroy = plug_destroy;
   s->plugin.activate = plug_activate;
   s->plugin.deactivate = plug_deactivate;
   s->plugin.start_processing = plug_start_processing;
   s->plugin.stop_processing = plug_stop_processing;
   s->plugin.reset = plug_reset;
   s->plugin.process = plug_process;
   s->plugin.get_extension = plug_get_extension;
   s->plugin.on_main_thread = plug_on_main_thread;
   return &s->plugin;
}

static uint32_t fac_count(const clap_plugin_factory_t *f) { (void)f; return 1; }
static const clap_plugin_descriptor_t *
fac_desc(const clap_plugin_factory_t *f, uint32_t i)
{ (void)f; return i ? NULL : &s_descriptor; }
static const clap_plugin_t *
fac_create(const clap_plugin_factory_t *f, const clap_host_t *host,
           const char *id)
{
   (void)f;
   if (!id || strcmp(id, SCVA_ID)) return NULL;
   return make_plugin(host);
}
static const clap_plugin_factory_t s_factory = {
   fac_count, fac_desc, fac_create
};

/* ---- preset discovery -------------------------------------------------- */

/* The presets are the tone and drum tables of the user's own SOUND Canvas VA
   install. They are not files, so the location is LOCATION_PLUGIN and each
   preset is identified by a load_key the plugin alone has to understand. */
static const clap_preset_discovery_provider_descriptor_t s_provider_desc = {
   .clap_version = CLAP_VERSION_INIT,
   .id = SCVA_ID ".presets",
   .name = "SOUND Canvas VA tones",
   .vendor = "scva-headless"
};

static const clap_preset_discovery_indexer_t *s_indexer;

static bool prov_init(const clap_preset_discovery_provider_t *provider)
{
   clap_preset_discovery_location_t loc;
   const clap_preset_discovery_indexer_t *ix = provider->provider_data;
   memset(&loc, 0, sizeof loc);
   loc.flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT;
   loc.name = "SOUND Canvas VA tones";
   loc.kind = CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN;
   loc.location = NULL;
   return ix && ix->declare_location(ix, &loc);
}

static void prov_destroy(const clap_preset_discovery_provider_t *provider)
{
   free((void *)provider);
}

static bool prov_get_metadata(const clap_preset_discovery_provider_t *provider,
                              uint32_t kind, const char *location,
                              const clap_preset_discovery_metadata_receiver_t *rx)
{
   clap_universal_plugin_id_t pid;
   struct scva_names names;
   struct scva_lists lists;
   char dir[1024];
   uint32_t i;
   int drum, m;
   (void)provider;

   if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location) return false;
   if (!scva_names_load(&names, scva_core_dir(g_plugin_dir, dir, sizeof dir))) {
      if (rx->on_error)
         rx->on_error(rx, 0, "no SOUND Canvas VA tone files beside SCCore.dll");
      return false;
   }
   pid.abi = "clap";
   pid.id = SCVA_ID;

   /* A preset browser shows one list: every map's tones, then every map's
      kits, the map named in each. Default only repeats SC-8820. */
   scva_lists_build(&lists, &names);
   for (drum = 0; drum < 2; ++drum)
      for (m = lists.has_maps ? SCVA_MAP_55 : SCVA_MAP_DEFAULT;
           m <= (lists.has_maps ? SCVA_MAP_8820 : SCVA_MAP_DEFAULT); ++m) {
         const uint32_t *list = drum ? lists.kit[m] : lists.tone[m];
         uint32_t n = drum ? lists.nkit[m] : lists.ntone[m];
         for (i = 0; i < n; ++i) {
            const struct scva_patch *p = &names.patch[list[i]];
            char name[SCVA_NAME_MAX + 32], key[32];
            scva_patch_label_map(p, name, sizeof name);
            load_key_make(key, sizeof key, p);
            if (!rx->begin_preset(rx, name, key)) goto done;
            rx->add_plugin_id(rx, &pid);
            if (rx->add_feature)
               rx->add_feature(rx, drum ? "drum" : CLAP_PLUGIN_FEATURE_INSTRUMENT);
         }
      }
done:
   scva_lists_free(&lists);
   scva_names_free(&names);
   return true;
}

static const void *prov_get_extension(const clap_preset_discovery_provider_t *p,
                                      const char *id)
{
   (void)p; (void)id;
   return NULL;
}

static uint32_t pd_count(const clap_preset_discovery_factory_t *f)
{ (void)f; return 1; }

static const clap_preset_discovery_provider_descriptor_t *
pd_get_descriptor(const clap_preset_discovery_factory_t *f, uint32_t i)
{ (void)f; return i ? NULL : &s_provider_desc; }

static const clap_preset_discovery_provider_t *
pd_create(const clap_preset_discovery_factory_t *f,
          const clap_preset_discovery_indexer_t *indexer, const char *id)
{
   clap_preset_discovery_provider_t *p;
   (void)f;
   if (!indexer || !id || strcmp(id, s_provider_desc.id)) return NULL;
   p = calloc(1, sizeof *p);
   if (!p) return NULL;
   s_indexer = indexer;
   p->desc = &s_provider_desc;
   p->provider_data = (void *)indexer;
   p->init = prov_init;
   p->destroy = prov_destroy;
   p->get_metadata = prov_get_metadata;
   p->get_extension = prov_get_extension;
   return p;
}

static const clap_preset_discovery_factory_t s_preset_factory = {
   pd_count, pd_get_descriptor, pd_create
};

static bool entry_init(const char *path)
{
   const char *slash = NULL, *c;
   for (c = path ? path : ""; *c; ++c)
      if (*c == '/' || *c == '\\') slash = c;
   if (slash && (size_t)(slash - path) < sizeof g_plugin_dir)
      snprintf(g_plugin_dir, sizeof g_plugin_dir, "%.*s", (int)(slash - path), path);
   return true;
}
static void entry_deinit(void)
{
#ifdef _WIN32
   UnregisterClassA(SCVA_GUI_CLASS, gui_module());
#endif
}
static const void *entry_get_factory(const char *id)
{
   if (!strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &s_factory;
   if (!strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) ||
       !strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT))
      return &s_preset_factory;
   return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
   .clap_version = CLAP_VERSION_INIT,
   .init = entry_init,
   .deinit = entry_deinit,
   .get_factory = entry_get_factory
};
