/* SPDX-License-Identifier: CC0-1.0
 *
 * A minimal CLAP host for tests, for Linux and (built with mingw) Windows.
 * It creates and activates the plugin on the main thread and processes on a
 * second one, and takes its instructions on stdin:
 *
 *   params                    each parameter: "param ID NAME|MIN|MAX|DEFAULT"
 *   value ID                  "value ID V"
 *   text ID V                 "text ID TEXT", or "text ID -" if refused
 *   fromtext ID TEXT          "fromtext ID V", or "fromtext ID -"
 *   set ID V                  queue a parameter change for the next call
 *   midi FRAME HEX...         queue a 3-byte MIDI event
 *   sysex FRAME HEX...        queue a SysEx event
 *   process N                 process N frames on the audio thread; prints
 *                             "processed N STATUS", "L k0 k1 ..." (left
 *                             samples times 256), and "out ID V" for each
 *                             parameter value the plugin reported
 *   peak                      "peak P", the last process's largest absolute
 *                             sample either side
 *   flush                     params flush on the main thread, same "out"
 *   callback                  run on_main_thread if the plugin asked; prints
 *                             "callback", then "rescan FLAGS" and
 *                             "notenames changed" as the host is told
 *   load KEY                  preset-load from the plugin's own location
 *   presets                   run preset discovery: "preset NAME|KEY|FEATURE"
 *   plugins                   "plugin ID|NAME" for each the file offers
 *   notenames                 "note KEY CHANNEL NAME" for each
 *   quit
 *
 *   clap_host PLUGIN.clap [INDEX [RATE]] < script
 *
 * INDEX picks the file's plugin and its preset provider (default 0), RATE
 * the sample rate (48000).
 */
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <clap/clap.h>
#include <clap/factory/preset-discovery.h>

static const clap_plugin_t *plug;
static const clap_plugin_params_t *params;
static int callback_requested;

/* ---- host extensions ------------------------------------------------------- */

static void h_rescan(const clap_host_t *h, clap_param_rescan_flags f)
{ (void)h; printf("rescan %u\n", (unsigned)f); }
static void h_clear(const clap_host_t *h, clap_id id, clap_param_clear_flags f)
{ (void)h; (void)id; (void)f; }
static void h_request_flush(const clap_host_t *h) { (void)h; printf("flush requested\n"); }
static const clap_host_params_t host_params = { h_rescan, h_clear, h_request_flush };

static void h_names_changed(const clap_host_t *h) { (void)h; printf("notenames changed\n"); }
static const clap_host_note_name_t host_note_name = { h_names_changed };

static const void *h_get_extension(const clap_host_t *h, const char *id)
{
  (void)h;
  if (!strcmp(id, CLAP_EXT_PARAMS)) return &host_params;
  if (!strcmp(id, CLAP_EXT_NOTE_NAME)) return &host_note_name;
  return NULL;
}
static void h_request_restart(const clap_host_t *h) { (void)h; }
static void h_request_process(const clap_host_t *h) { (void)h; }
static void h_request_callback(const clap_host_t *h) { (void)h; callback_requested = 1; }

static const clap_host_t host = {
  CLAP_VERSION_INIT, NULL, "scva tests", "scva-headless", "", "1",
  h_get_extension, h_request_restart, h_request_process, h_request_callback
};

/* ---- event lists ------------------------------------------------------------- */

union ev {
  clap_event_header_t h;
  clap_event_param_value_t param;
  clap_event_midi_t midi;
  clap_event_midi_sysex_t sysex;
};
static union ev in_ev[256];
static unsigned char sysex_buf[256][1024];
static uint32_t n_in;

static uint32_t in_size(const clap_input_events_t *l) { (void)l; return n_in; }
static const clap_event_header_t *in_get(const clap_input_events_t *l, uint32_t i)
{ (void)l; return i < n_in ? &in_ev[i].h : NULL; }
static const clap_input_events_t in_list = { NULL, in_size, in_get };

static bool out_push(const clap_output_events_t *l, const clap_event_header_t *e)
{
  (void)l;
  if (e->space_id == CLAP_CORE_EVENT_SPACE_ID && e->type == CLAP_EVENT_PARAM_VALUE) {
    const clap_event_param_value_t *p = (const clap_event_param_value_t *)e;
    printf("out %u %g\n", (unsigned)p->param_id, p->value);
  }
  return true;
}
static const clap_output_events_t out_list = { NULL, out_push };

static void queue_header(union ev *e, uint32_t size, uint16_t type, uint32_t time)
{
  memset(e, 0, sizeof *e);
  e->h.size = size;
  e->h.time = time;
  e->h.space_id = CLAP_CORE_EVENT_SPACE_ID;
  e->h.type = type;
}

/* ---- the audio thread --------------------------------------------------------- */

static float out_l[8192], out_r[8192];
static int frames_to_process, process_status;

static void process_now(void)
{
  float *chans[2] = { out_l, out_r };
  clap_audio_buffer_t out;
  clap_process_t pr;
  memset(&out, 0, sizeof out);
  out.data32 = chans;
  out.channel_count = 2;
  memset(&pr, 0, sizeof pr);
  pr.steady_time = -1;
  pr.frames_count = (uint32_t)frames_to_process;
  pr.audio_outputs = &out;
  pr.audio_outputs_count = 1;
  pr.in_events = &in_list;
  pr.out_events = &out_list;
  process_status = plug->process(plug, &pr);
}

#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID a) { (void)a; process_now(); return 0; }
static void on_audio_thread(void)
{
  HANDLE t = CreateThread(NULL, 0, thread_main, NULL, 0, NULL);
  WaitForSingleObject(t, INFINITE);
  CloseHandle(t);
}
#else
static void *thread_main(void *a) { (void)a; process_now(); return NULL; }
static void on_audio_thread(void)
{
  pthread_t t;
  pthread_create(&t, NULL, thread_main, NULL);
  pthread_join(t, NULL);
}
#endif

/* ---- preset discovery ----------------------------------------------------------- */

static const char *preset_name, *preset_key;
static bool rx_begin(const clap_preset_discovery_metadata_receiver_t *r, const char *name, const char *key)
{ (void)r; preset_name = name; preset_key = key; return true; }
static void rx_feature(const clap_preset_discovery_metadata_receiver_t *r, const char *f)
{ (void)r; printf("preset %s|%s|%s\n", preset_name, preset_key, f); }
static void rx_plugin_id(const clap_preset_discovery_metadata_receiver_t *r, const clap_universal_plugin_id_t *id)
{ (void)r; (void)id; }
static void rx_error(const clap_preset_discovery_metadata_receiver_t *r, int32_t c, const char *m)
{ (void)r; (void)c; printf("preset error %s\n", m); }

static bool ix_filetype(const clap_preset_discovery_indexer_t *i, const clap_preset_discovery_filetype_t *f)
{ (void)i; (void)f; return true; }
static bool ix_location(const clap_preset_discovery_indexer_t *i, const clap_preset_discovery_location_t *l)
{ (void)i; printf("location %s kind %u\n", l->name, (unsigned)l->kind); return true; }
static bool ix_soundpack(const clap_preset_discovery_indexer_t *i, const clap_preset_discovery_soundpack_t *s)
{ (void)i; (void)s; return true; }
static const void *ix_ext(const clap_preset_discovery_indexer_t *i, const char *id)
{ (void)i; (void)id; return NULL; }

static uint32_t which;               /* the plugin, by index */

static void discover(const clap_plugin_entry_t *entry)
{
  static const clap_preset_discovery_indexer_t ix = {
    CLAP_VERSION_INIT, "scva tests", "scva-headless", "", "1", NULL,
    ix_filetype, ix_location, ix_soundpack, ix_ext
  };
  clap_preset_discovery_metadata_receiver_t rx;
  const clap_preset_discovery_factory_t *f =
    entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID);
  const clap_preset_discovery_provider_t *p;
  if (!f) { printf("no preset factory\n"); return; }
  p = f->get_descriptor(f, which) ? f->create(f, &ix, f->get_descriptor(f, which)->id) : NULL;
  if (!p || !p->init(p)) { printf("provider failed\n"); return; }
  memset(&rx, 0, sizeof rx);
  rx.on_error = rx_error;
  rx.begin_preset = rx_begin;
  rx.add_plugin_id = rx_plugin_id;
  rx.add_feature = rx_feature;
  p->get_metadata(p, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, &rx);
  p->destroy(p);
}

/* ---- main ------------------------------------------------------------------------- */

static int parse_hex(const char *p, unsigned char *out, int cap)
{
  unsigned v;
  int k = 0, used;
  while (k < cap && sscanf(p, "%x%n", &v, &used) == 1) { out[k++] = (unsigned char)v; p += used; }
  return k;
}

int main(int argc, char **argv)
{
  const clap_plugin_entry_t *entry;
  const clap_plugin_factory_t *fac;
  char line[4096];
#ifdef _WIN32
  HMODULE so;
#else
  void *so;
#endif
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc < 2 || argc > 4) {
    fprintf(stderr, "usage: clap_host PLUGIN.clap [INDEX [RATE]]\n");
    return 2;
  }
  if (argc > 2) which = (uint32_t)atoi(argv[2]);
#ifdef _WIN32
  so = LoadLibraryA(argv[1]);
  entry = so ? (const clap_plugin_entry_t *)GetProcAddress(so, "clap_entry") : NULL;
#else
  so = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  entry = so ? (const clap_plugin_entry_t *)dlsym(so, "clap_entry") : NULL;
#endif
  if (!entry || !entry->init(argv[1])) { printf("cannot load %s\n", argv[1]); return 1; }
  fac = entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
  plug = fac && fac->get_plugin_descriptor(fac, which)
         ? fac->create_plugin(fac, &host, fac->get_plugin_descriptor(fac, which)->id) : NULL;
  if (!plug || !plug->init(plug)) { printf("cannot create the plugin\n"); return 1; }
  params = plug->get_extension(plug, CLAP_EXT_PARAMS);
  printf("%s\n", plug->activate(plug, argc > 3 ? atof(argv[3]) : 48000.0, 1, 4096)
                   ? "activated" : "activate failed");
  plug->start_processing(plug);

  while (fgets(line, sizeof line, stdin)) {
    unsigned id;
    double v;
    int frame, n;
    char rest[1024];
    line[strcspn(line, "\r\n")] = '\0';
    if (!strcmp(line, "params")) {
      uint32_t i;
      for (i = 0; i < params->count(plug); ++i) {
        clap_param_info_t info;
        if (params->get_info(plug, i, &info))
          printf("param %u %s|%g|%g|%g\n", (unsigned)info.id, info.name,
                 info.min_value, info.max_value, info.default_value);
      }
    } else if (sscanf(line, "value %u", &id) == 1) {
      if (params->get_value(plug, id, &v)) printf("value %u %g\n", id, v);
    } else if (sscanf(line, "text %u %lf", &id, &v) == 2) {
      char t[256];
      printf("text %u %s\n", id, params->value_to_text(plug, id, v, t, sizeof t) ? t : "-");
    } else if (sscanf(line, "fromtext %u %1023[^\n]", &id, rest) == 2) {
      if (params->text_to_value(plug, id, rest, &v)) printf("fromtext %u %g\n", id, v);
      else printf("fromtext %u -\n", id);
    } else if (sscanf(line, "set %u %lf", &id, &v) == 2) {
      union ev *e = &in_ev[n_in++];
      queue_header(e, sizeof e->param, CLAP_EVENT_PARAM_VALUE, 0);
      e->param.param_id = id;
      e->param.value = v;
      e->param.note_id = -1; e->param.port_index = -1;
      e->param.channel = -1; e->param.key = -1;
    } else if (sscanf(line, "midi %d %n", &frame, &n) == 1) {
      union ev *e = &in_ev[n_in++];
      queue_header(e, sizeof e->midi, CLAP_EVENT_MIDI, (uint32_t)frame);
      parse_hex(line + n, e->midi.data, 3);
    } else if (sscanf(line, "sysex %d %n", &frame, &n) == 1) {
      union ev *e = &in_ev[n_in];
      queue_header(e, sizeof e->sysex, CLAP_EVENT_MIDI_SYSEX, (uint32_t)frame);
      e->sysex.buffer = sysex_buf[n_in];
      e->sysex.size = (uint32_t)parse_hex(line + n, sysex_buf[n_in], 1024);
      ++n_in;
    } else if (sscanf(line, "process %d", &frames_to_process) == 1) {
      int i;
      on_audio_thread();
      n_in = 0;
      printf("processed %d %d\nL", frames_to_process, process_status);
      for (i = 0; i < frames_to_process; ++i) printf(" %ld", lround(out_l[i] * 256.0f));
      printf("\n");
    } else if (!strcmp(line, "flush")) {
      params->flush(plug, &in_list, &out_list);
      n_in = 0;
    } else if (!strcmp(line, "callback")) {
      if (callback_requested) {
        callback_requested = 0;
        printf("callback\n");
        plug->on_main_thread(plug);
      }
    } else if (sscanf(line, "load %1023s", rest) == 1) {
      const clap_plugin_preset_load_t *pl = plug->get_extension(plug, CLAP_EXT_PRESET_LOAD);
      if (!pl) pl = plug->get_extension(plug, CLAP_EXT_PRESET_LOAD_COMPAT);
      printf("load %s %s\n", rest,
             pl && pl->from_location(plug, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, NULL, rest)
               ? "ok" : "refused");
    } else if (!strcmp(line, "peak")) {
      int i;
      float peak = 0.0f;
      for (i = 0; i < frames_to_process; ++i) {
        if (fabsf(out_l[i]) > peak) peak = fabsf(out_l[i]);
        if (fabsf(out_r[i]) > peak) peak = fabsf(out_r[i]);
      }
      printf("peak %.6f\n", peak);
    } else if (!strcmp(line, "plugins")) {
      uint32_t i;
      for (i = 0; i < fac->get_plugin_count(fac); ++i)
        printf("plugin %s|%s\n", fac->get_plugin_descriptor(fac, i)->id,
               fac->get_plugin_descriptor(fac, i)->name);
    } else if (!strcmp(line, "presets")) {
      discover(entry);
    } else if (!strcmp(line, "notenames")) {
      const clap_plugin_note_name_t *nn = plug->get_extension(plug, CLAP_EXT_NOTE_NAME);
      uint32_t i, count = nn ? nn->count(plug) : 0;
      for (i = 0; i < count; ++i) {
        clap_note_name_t note;
        if (nn->get(plug, i, &note))
          printf("note %d %d %s\n", note.key, note.channel, note.name);
      }
    } else if (!strcmp(line, "quit")) {
      break;
    }
  }
  plug->stop_processing(plug);
  plug->deactivate(plug);
  plug->destroy(plug);
  entry->deinit();
  printf("destroyed\n");
  return 0;
}
