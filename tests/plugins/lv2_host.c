/* SPDX-License-Identifier: CC0-1.0
 *
 * A minimal LV2 host for tests. It loads a bundle's plugin, instantiates it
 * on the main thread and runs it on a second thread, as real hosts do, and
 * takes its instructions on stdin:
 *
 *   port map|kit|inst VALUE      set a control port
 *   midi FRAME HEX...            queue a MIDI event for the next run
 *   run N                        run N frames on the audio thread; prints
 *                                "ran N" then "L k0 k1 ..." with each left
 *                                sample times 256, rounded
 *   peak                         "peak P", the last run's largest absolute
 *                                sample either side
 *   quit                         clean up and exit
 *
 *   lv2_host BUNDLE_DIR [INDEX [RATE]] < script
 *
 * INDEX picks the bundle's plugin (default 0), RATE the sample rate (48000).
 *
 * Prints "instantiated" or "instantiate failed" first.
 */
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

static const char *uris[64];
static int nuris;

static LV2_URID map_uri(LV2_URID_Map_Handle h, const char *uri)
{
  int i;
  (void)h;
  for (i = 0; i < nuris; ++i) if (!strcmp(uris[i], uri)) return (LV2_URID)(i + 1);
  uris[nuris] = strdup(uri);
  return (LV2_URID)++nuris;
}

static const LV2_Descriptor *desc;
static LV2_Handle inst;
static float ports[3], out_l[8192], out_r[8192];
static union { LV2_Atom_Sequence seq; unsigned char raw[16384]; } midi;
static LV2_URID midi_type, seq_type, frames_unit;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int pending = -1, done;

static void *audio_thread(void *arg)
{
  (void)arg;
  for (;;) {
    int n;
    pthread_mutex_lock(&mu);
    while (pending < 0) pthread_cond_wait(&cv, &mu);
    n = pending;
    pthread_mutex_unlock(&mu);
    if (n == 0) break;
    desc->run(inst, (uint32_t)n);
    pthread_mutex_lock(&mu);
    pending = -1;
    done = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
  }
  return NULL;
}

static void clear_midi(void)
{
  midi.seq.atom.type = seq_type;
  midi.seq.atom.size = sizeof(LV2_Atom_Sequence_Body);
  midi.seq.body.unit = frames_unit;
  midi.seq.body.pad = 0;
}

static void add_midi(int frame, const unsigned char *bytes, uint32_t n)
{
  LV2_Atom_Event *ev = (LV2_Atom_Event *)((unsigned char *)&midi.seq.body +
                                          lv2_atom_pad_size(midi.seq.atom.size));
  ev->time.frames = frame;
  ev->body.type = midi_type;
  ev->body.size = n;
  memcpy(ev + 1, bytes, n);
  midi.seq.atom.size = lv2_atom_pad_size(midi.seq.atom.size) +
                       (uint32_t)sizeof *ev + n;
}

int main(int argc, char **argv)
{
  char path[1024], line[4096];
  LV2_URID_Map urid = { NULL, map_uri };
  LV2_Feature urid_feature = { LV2_URID__map, &urid };
  const LV2_Feature *features[] = { &urid_feature, NULL };
  LV2_Descriptor_Function entry;
  pthread_t th;
  void *so;
  int last_run = 0;

  if (argc < 2 || argc > 4) {
    fprintf(stderr, "usage: lv2_host BUNDLE_DIR [INDEX [RATE]]\n");
    return 2;
  }
  snprintf(path, sizeof path, "%s/scva.so", argv[1]);
  so = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!so) { printf("dlopen failed: %s\n", dlerror()); return 1; }
  entry = (LV2_Descriptor_Function)dlsym(so, "lv2_descriptor");
  desc = entry ? entry(argc > 2 ? (uint32_t)atoi(argv[2]) : 0) : NULL;
  if (!desc) { printf("no descriptor\n"); return 1; }
  printf("uri %s\n", desc->URI);

  midi_type = map_uri(NULL, LV2_MIDI__MidiEvent);
  seq_type = map_uri(NULL, LV2_ATOM__Sequence);
  frames_unit = map_uri(NULL, LV2_ATOM__frameTime);
  snprintf(path, sizeof path, "%s/", argv[1]);        /* hosts pass the slash */
  inst = desc->instantiate(desc, argc > 3 ? atof(argv[3]) : 48000.0, path, features);
  printf("%s\n", inst ? "instantiated" : "instantiate failed");
  fflush(stdout);
  if (!inst) return 0;

  clear_midi();
  desc->connect_port(inst, 0, &midi.seq);
  desc->connect_port(inst, 1, out_l);
  desc->connect_port(inst, 2, out_r);
  desc->connect_port(inst, 3, &ports[0]);
  desc->connect_port(inst, 4, &ports[1]);
  desc->connect_port(inst, 5, &ports[2]);
  if (desc->activate) desc->activate(inst);
  pthread_create(&th, NULL, audio_thread, NULL);

  while (fgets(line, sizeof line, stdin)) {
    char what[16];
    float value;
    int frame, n, i;
    if (sscanf(line, "port %15s %f", what, &value) == 2) {
      ports[!strcmp(what, "map") ? 0 : !strcmp(what, "kit") ? 1 : 2] = value;
    } else if (sscanf(line, "midi %d %n", &frame, &n) == 1) {
      unsigned char bytes[1024];
      unsigned v;
      int k = 0, used;
      const char *p = line + n;
      while (k < (int)sizeof bytes && sscanf(p, "%x%n", &v, &used) == 1) {
        bytes[k++] = (unsigned char)v;
        p += used;
      }
      add_midi(frame, bytes, (uint32_t)k);
    } else if (sscanf(line, "run %d", &n) == 1 && n > 0 && n <= 8192) {
      pthread_mutex_lock(&mu);
      done = 0;
      pending = n;
      pthread_cond_broadcast(&cv);
      while (!done) pthread_cond_wait(&cv, &mu);
      pthread_mutex_unlock(&mu);
      clear_midi();
      printf("ran %d\nL", n);
      for (i = 0; i < n; ++i) printf(" %ld", lround(out_l[i] * 256.0f));
      printf("\n");
      last_run = n;
      fflush(stdout);
    } else if (!strncmp(line, "peak", 4)) {
      float peak = 0.0f;
      for (i = 0; i < last_run; ++i) {
        if (fabsf(out_l[i]) > peak) peak = fabsf(out_l[i]);
        if (fabsf(out_r[i]) > peak) peak = fabsf(out_r[i]);
      }
      printf("peak %.6f\n", peak);
    } else if (!strncmp(line, "quit", 4)) {
      break;
    }
  }
  pthread_mutex_lock(&mu);
  pending = 0;
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);
  pthread_join(th, NULL);
  if (desc->deactivate) desc->deactivate(inst);
  desc->cleanup(inst);
  printf("cleaned up\n");
  return 0;
}
