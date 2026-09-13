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
 * $SCVA_DLL_DIR, then beside the plugin.
 *
 * On Windows the core loads through LoadLibrary; on Linux through the PE
 * loader in pe_loader.c. Same engine either way.
 */
#define _GNU_SOURCE
#include "scva_map.h"
#include "scva_names.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <clap/clap.h>

#ifdef _WIN32
#include <windows.h>
#define SCVA_CC __cdecl
#else
#include "pe_loader.h"
#define SCVA_CC MSABI
#endif

#define SCVA_MAXBLOCK 4096
#define SCVA_ID "com.github.mlainez.scva-headless"

typedef SCVA_CC int (*tg_initialize_fn)(int);
typedef SCVA_CC void (*tg_set_sample_rate_fn)(float);
typedef SCVA_CC void (*tg_set_max_block_fn)(int);
typedef SCVA_CC int (*tg_activate_fn)(float, int);
typedef SCVA_CC void (*tg_deactivate_fn)(void);
typedef SCVA_CC void (*tg_short_midi_fn)(unsigned int, int);
typedef SCVA_CC void (*tg_long_midi_fn)(const unsigned char *, int);
typedef SCVA_CC void (*tg_process_fn)(float *, float *, int);
struct tg_system_config { int a, b; };
typedef SCVA_CC int (*tg_set_config_fn)(const struct tg_system_config *);

struct scva {
   clap_plugin_t plugin;
   const clap_host_t *host;

#ifdef _WIN32
   HMODULE lib;
#else
   struct pe_image *img;
#endif
   tg_set_sample_rate_fn set_rate;
   tg_activate_fn tg_activate;
   tg_deactivate_fn tg_deactivate;
   tg_short_midi_fn short_midi;
   tg_long_midi_fn long_midi;
   tg_process_fn process;

   double rate;
   int map_want, map_now;

   struct scva_names names;
   /* A preset arrives on the main thread; it is latched and applied at the
      top of the next process(). */
   int sel_map, sel_bank, sel_prog, sel_drum, sel_pending;
   /* The kit on the drum part and the map it came from, for note names. A
      preset names its own map, which need not be the one the Tone Map
      parameter holds for everything else. */
   int kit_now, kit_map;
};

/* Kits land on the drum part, tones on the first part. */
#define SCVA_TONE_CH 0
#define SCVA_DRUM_CH 9

static void apply_selection(struct scva *s);

/* ---- finding and opening the core -------------------------------------- */

static const char *core_dir(void)
{
   const char *dir = getenv("SCVA_DLL_DIR");
   return (dir && *dir) ? dir : ".";
}

static int core_path(char *buf, size_t cap, const char *leaf)
{
   const char *dir = getenv("SCVA_DLL_DIR");
   if (dir && *dir) {
      snprintf(buf, cap, "%s/%s", dir, leaf);
      return 1;
   }
   snprintf(buf, cap, "%s", leaf);
   return 1;
}

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

static void *sym(struct scva *s, const char *n)
{
#ifdef _WIN32
   return (void *)GetProcAddress(s->lib, n);
#else
   return pe_symbol(s->img, n);
#endif
}

static int open_core(struct scva *s)
{
   char path[2048];
   core_path(path, sizeof path, "SCCore.dll");
#ifdef _WIN32
   s->lib = LoadLibraryA(path);
   if (!s->lib) { fprintf(stderr, "scva: cannot load %s\n", path); return 0; }
#else
   {
      char err[256];
      s->img = pe_load(path, err, sizeof err);
      if (!s->img) { fprintf(stderr, "scva: %s\n", err); return 0; }
   }
#endif
   return 1;
}

static void close_core(struct scva *s)
{
#ifdef _WIN32
   if (s->lib) { FreeLibrary(s->lib); s->lib = NULL; }
#else
   if (s->img) { pe_unload(s->img); s->img = NULL; }
#endif
}

/* ---- the engine -------------------------------------------------------- */

static int engine_start(struct scva *s, double rate)
{
   tg_initialize_fn initialize;
   tg_set_max_block_fn set_block;
   tg_set_config_fn set_config;
   static const unsigned char gs_reset[] = {
      0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };

   if (!open_core(s)) return 0;
   initialize   = (tg_initialize_fn)sym(s, "TG_initialize");
   set_block    = (tg_set_max_block_fn)sym(s, "TG_setMaxBlockSize");
   set_config   = (tg_set_config_fn)sym(s, "TG_XPsetSystemConfig");
   s->set_rate  = (tg_set_sample_rate_fn)sym(s, "TG_setSampleRate");
   s->tg_activate = (tg_activate_fn)sym(s, "TG_activate");
   s->tg_deactivate = (tg_deactivate_fn)sym(s, "TG_deactivate");
   s->short_midi = (tg_short_midi_fn)sym(s, "TG_ShortMidiIn");
   s->long_midi  = (tg_long_midi_fn)sym(s, "TG_LongMidiIn");
   s->process    = (tg_process_fn)sym(s, "TG_Process");
   if (!initialize || !set_block || !set_config || !s->set_rate ||
       !s->tg_activate || !s->tg_deactivate || !s->short_midi ||
       !s->long_midi || !s->process) {
      fprintf(stderr, "scva: the core is missing exports\n");
      close_core(s);
      return 0;
   }
   /* The rate is set on both sides of the block size and the second call is
      the last thing before activate; otherwise the core emits Inf and NaN
      while reporting no error. */
   initialize(0);
   s->set_rate((float)rate);
   set_block(SCVA_MAXBLOCK);
   { struct tg_system_config c; c.a = 1; c.b = 1; set_config(&c); }
   s->set_rate((float)rate);
   s->tg_activate((float)rate, SCVA_MAXBLOCK);
   s->long_midi(gs_reset, 0);
   s->map_now = -1;
   return 1;
}

static void send_map(struct scva *s, int v)
{
   int ch;
   for (ch = 0; ch < 16; ++ch) s->short_midi(scva_map_cc(ch, v), 0);
   s->map_now = v;
}

static void deliver(struct scva *s, const uint8_t *d, uint32_t size)
{
   if (!size) return;
   if (d[0] == 0xf0) { s->long_midi(d, 0); return; }
   if (s->map_now > 0 && (d[0] & 0xf0) == 0xc0)
      s->short_midi(scva_map_cc(d[0] & 0x0f, s->map_now), 0);
   {
      unsigned int msg = d[0];
      if (size > 1) msg |= (unsigned int)d[1] << 8;
      if (size > 2) msg |= (unsigned int)d[2] << 16;
      s->short_midi(msg, 0);
   }
}

/* TG_Process writes exactly the frames asked for, so it can write straight
   into the host's buffers at an offset, chunked to the declared maximum. */
static void render(struct scva *s, float *l, float *r, uint32_t at, uint32_t n)
{
   while (n) {
      uint32_t k = n > SCVA_MAXBLOCK ? SCVA_MAXBLOCK : n;
      s->process(l + at, r + at, (int)k);
      at += k;
      n -= k;
   }
}

/* ---- clap_plugin ------------------------------------------------------- */

/* Names come from text files beside the core, not from the core, so they load
   without it. Absent, the plugin still plays; it just offers no presets. */
static bool plug_init(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   scva_names_load(&s->names, core_dir());
   return true;
}

static void plug_destroy(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   close_core(s);
   scva_names_free(&s->names);
   free(s);
}

static bool plug_activate(const clap_plugin_t *p, double rate,
                          uint32_t min, uint32_t max)
{
   struct scva *s = p->plugin_data;
   (void)min; (void)max;
   s->rate = rate;
   return engine_start(s, rate) ? true : false;
}

static void plug_deactivate(const clap_plugin_t *p)
{
   struct scva *s = p->plugin_data;
   /* Deactivate only. TG_terminate calls exit() and would take the host with
      it. */
   if (s->tg_deactivate) s->tg_deactivate();
   close_core(s);
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

   if (s->sel_pending) apply_selection(s);

   if (!pr->audio_outputs_count || pr->audio_outputs[0].channel_count < 2)
      return CLAP_PROCESS_ERROR;
   l = pr->audio_outputs[0].data32[0];
   r = pr->audio_outputs[0].data32[1];

   if (s->map_want != s->map_now) send_map(s, s->map_want);

   for (; ei < nev; ++ei) {
      const clap_event_header_t *h = pr->in_events->get(pr->in_events, ei);
      uint32_t t = h->time > n ? n : h->time;
      if (t > at) { render(s, l, r, at, t - at); at = t; }
      if (h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
      if (h->type == CLAP_EVENT_MIDI) {
         const clap_event_midi_t *m = (const clap_event_midi_t *)h;
         deliver(s, m->data, 3);
      } else if (h->type == CLAP_EVENT_MIDI_SYSEX) {
         const clap_event_midi_sysex_t *m = (const clap_event_midi_sysex_t *)h;
         deliver(s, m->buffer, m->size);
      }
   }
   if (at < n) render(s, l, r, at, n - at);
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

/* ---- the tone map, as a parameter -------------------------------------- */

static uint32_t pr_count(const clap_plugin_t *p) { (void)p; return 1; }

static bool pr_info(const clap_plugin_t *p, uint32_t i,
                    clap_param_info_t *info)
{
   (void)p;
   if (i) return false;
   memset(info, 0, sizeof *info);
   info->id = 0;
   info->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
   info->min_value = 0;
   info->max_value = SCVA_MAP_8820;
   info->default_value = 0;
   snprintf(info->name, sizeof info->name, "Tone Map");
   return true;
}

static bool pr_value(const clap_plugin_t *p, clap_id id, double *out)
{
   struct scva *s = p->plugin_data;
   if (id) return false;
   *out = s->map_want;
   return true;
}

static bool pr_text(const clap_plugin_t *p, clap_id id, double v,
                    char *out, uint32_t cap)
{
   static const char *names[] = { "Default", "SC-55", "SC-88",
                                  "SC-88Pro", "SC-8820" };
   int k = (int)(v + 0.5);
   (void)p;
   if (id) return false;
   if (k < 0) k = 0;
   if (k > SCVA_MAP_8820) k = SCVA_MAP_8820;
   snprintf(out, cap, "%s", names[k]);
   return true;
}

static bool pr_from_text(const clap_plugin_t *p, clap_id id, const char *t,
                         double *out)
{
   int v;
   (void)p;
   if (id) return false;
   v = scva_map_value(t);
   if (v < 0) return false;
   *out = v;
   return true;
}

static void pr_flush(const clap_plugin_t *p, const clap_input_events_t *in,
                     const clap_output_events_t *out)
{
   struct scva *s = p->plugin_data;
   uint32_t i, n = in ? in->size(in) : 0;
   (void)out;
   for (i = 0; i < n; ++i) {
      const clap_event_header_t *h = in->get(in, i);
      if (h->type == CLAP_EVENT_PARAM_VALUE) {
         const clap_event_param_value_t *e =
            (const clap_event_param_value_t *)h;
         if (e->param_id == 0) s->map_want = (int)(e->value + 0.5);
      }
   }
}
static const clap_plugin_params_t s_params = {
   pr_count, pr_info, pr_value, pr_text, pr_from_text, pr_flush
};

/* ---- presets ----------------------------------------------------------- */

/* CC32 then CC0 then the program change: the bank pair only takes effect on
   the program change that follows it. */
static void apply_selection(struct scva *s)
{
   int ch = s->sel_drum ? SCVA_DRUM_CH : SCVA_TONE_CH;

   s->sel_pending = 0;
   if (!s->short_midi) return;             /* not activated yet */
   s->short_midi(scva_map_cc(ch, s->sel_map), 0);
   s->short_midi((unsigned int)(0xb0 | ch) |
                 ((unsigned int)s->sel_bank << 16), 0);
   s->short_midi((unsigned int)(0xc0 | ch) |
                 ((unsigned int)s->sel_prog << 8), 0);
   if (s->sel_drum) {
      s->kit_now = s->sel_prog;
      s->kit_map = s->sel_map;
   }
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
   if (drum && s->host && s->host->get_extension) {
      const clap_host_note_name_t *nn =
         s->host->get_extension(s->host, CLAP_EXT_NOTE_NAME);
      if (nn && nn->changed) nn->changed(s->host);
   }
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

static const void *plug_get_extension(const clap_plugin_t *p, const char *id)
{
   (void)p;
   if (!strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &s_audio_ports;
   if (!strcmp(id, CLAP_EXT_NOTE_PORTS))  return &s_note_ports;
   if (!strcmp(id, CLAP_EXT_PARAMS))      return &s_params;
   if (!strcmp(id, CLAP_EXT_NOTE_NAME))   return &s_note_name;
   if (!strcmp(id, CLAP_EXT_PRESET_LOAD) ||
       !strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT))
      return &s_preset_load;
   return NULL;
}

static void plug_on_main_thread(const clap_plugin_t *p) { (void)p; }

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
   s->map_now = -1;
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
   size_t i;
   (void)provider;

   if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || location) return false;
   if (!scva_names_load(&names, core_dir())) {
      if (rx->on_error)
         rx->on_error(rx, 0, "no SOUND Canvas VA tone files beside SCCore.dll");
      return false;
   }
   pid.abi = "clap";
   pid.id = SCVA_ID;

   for (i = 0; i < names.npatch; ++i) {
      const struct scva_patch *p = &names.patch[i];
      const char *tag = scva_map_tag(p->map);
      char name[SCVA_NAME_MAX + 16], key[32];
      snprintf(name, sizeof name, "%s%s%s", tag, *tag ? " " : "", p->name);
      load_key_make(key, sizeof key, p);
      if (!rx->begin_preset(rx, name, key)) break;
      rx->add_plugin_id(rx, &pid);
      if (rx->add_feature)
         rx->add_feature(rx, p->drum ? "drum" : CLAP_PLUGIN_FEATURE_INSTRUMENT);
   }
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

static bool entry_init(const char *path) { (void)path; return true; }
static void entry_deinit(void) {}
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
