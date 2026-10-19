/* SPDX-License-Identifier: CC0-1.0
 *
 * See scva_engine.h. On 64-bit Windows the core loads through LoadLibrary.
 * Elsewhere it goes through the PE loader in pe_loader.c: on Linux because
 * there is no other way, on 32-bit Windows because the system loader will not
 * take a core that imports MSVCR100 and declares a subsystem newer than
 * Windows 98. Same engine either way.
 */
#include "scva_engine.h"
#include "scva_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) && !defined(__i386__)
#define SCVA_LOADLIBRARY
#endif

#ifdef SCVA_LOADLIBRARY
#include <windows.h>
#define SCVA_CC __cdecl
#else
#include "pe_loader.h"
#define SCVA_CC MSABI
#endif

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

struct scva_engine {
#ifdef SCVA_LOADLIBRARY
  HMODULE lib;
#else
  struct pe_image *img;
#endif
  tg_deactivate_fn deactivate;
  tg_short_midi_fn short_midi;
  tg_long_midi_fn long_midi;
  tg_process_fn process;
  int map_now;                  /* -1 until the first map is sent */
  unsigned char last_pc[16];    /* each part's, re-sent after a map change */
};

static void *sym(struct scva_engine *e, const char *n)
{
#ifdef SCVA_LOADLIBRARY
  return (void *)GetProcAddress(e->lib, n);
#else
  return pe_symbol(e->img, n);
#endif
}

static void unload(struct scva_engine *e)
{
#ifdef SCVA_LOADLIBRARY
  if (e->lib) FreeLibrary(e->lib);
#else
  if (e->img) pe_unload(e->img);
#endif
  free(e);
}

struct scva_engine *scva_engine_open(const char *dir, double rate,
                                     char *err, size_t errlen)
{
  static const unsigned char gs_reset[] = {
    0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
  char path[2048];
  struct scva_engine *e = calloc(1, sizeof *e);
  tg_initialize_fn initialize;
  tg_set_max_block_fn set_block;
  tg_set_config_fn set_config;
  tg_set_sample_rate_fn set_rate;
  tg_activate_fn activate;

  if (!e) { snprintf(err, errlen, "out of memory"); return NULL; }
  snprintf(path, sizeof path, "%s/SCCore.dll", dir);
#ifdef SCVA_LOADLIBRARY
  e->lib = LoadLibraryA(path);
  if (!e->lib) {
    snprintf(err, errlen, "cannot load %s", path);
    free(e);
    return NULL;
  }
#else
  e->img = pe_load(path, err, errlen);
  if (!e->img) { free(e); return NULL; }
#endif
  initialize    = (tg_initialize_fn)sym(e, "TG_initialize");
  set_block     = (tg_set_max_block_fn)sym(e, "TG_setMaxBlockSize");
  set_config    = (tg_set_config_fn)sym(e, "TG_XPsetSystemConfig");
  set_rate      = (tg_set_sample_rate_fn)sym(e, "TG_setSampleRate");
  activate      = (tg_activate_fn)sym(e, "TG_activate");
  e->deactivate = (tg_deactivate_fn)sym(e, "TG_deactivate");
  e->short_midi = (tg_short_midi_fn)sym(e, "TG_ShortMidiIn");
  e->long_midi  = (tg_long_midi_fn)sym(e, "TG_LongMidiIn");
  e->process    = (tg_process_fn)sym(e, "TG_Process");
  if (!initialize || !set_block || !set_config || !set_rate || !activate ||
      !e->deactivate || !e->short_midi || !e->long_midi || !e->process) {
    snprintf(err, errlen, "the core is missing exports");
    unload(e);
    return NULL;
  }
  /* The rate is set on both sides of the block size and the second call is
     the last thing before activate; otherwise the core emits Inf and NaN
     while reporting no error. */
  initialize(0);
  set_rate((float)rate);
  set_block(SCVA_MAXBLOCK);
  { struct tg_system_config c; c.a = 1; c.b = 1; set_config(&c); }
  set_rate((float)rate);
  activate((float)rate, SCVA_MAXBLOCK);
  e->long_midi(gs_reset, 0);
  e->map_now = -1;
  return e;
}

void scva_engine_close(struct scva_engine *e)
{
  if (!e) return;
  if (scva_engine_thread()) e->deactivate();
  unload(e);
}

int scva_engine_thread(void)
{
#ifdef SCVA_LOADLIBRARY
  return 1;
#else
  return pe_thread_attach();
#endif
}

/* CC32 alone only latches the map for the next program change, so each
   part's last program is re-sent to make the map audible now. That includes
   the first send: at start-up the parts play SC-55 tones, not those of the
   Default map. */
int scva_engine_set_map(struct scva_engine *e, int map)
{
  int ch;
  if (map == e->map_now) return 0;
  for (ch = 0; ch < 16; ++ch) {
    e->short_midi(scva_map_cc(ch, map), 0);
    e->short_midi(scva_map_pc(ch, e->last_pc[ch]), 0);
  }
  e->map_now = map;
  return 1;
}

/* CC32 then CC0 then the program change: the bank pair only takes effect on
   the program change that follows it. */
void scva_engine_patch(struct scva_engine *e, int ch, int map, int bank,
                       int prog)
{
  e->short_midi(scva_map_cc(ch, map), 0);
  e->short_midi((unsigned int)(0xb0 | ch) | ((unsigned int)bank << 16), 0);
  e->short_midi((unsigned int)(0xc0 | ch) | ((unsigned int)prog << 8), 0);
  e->last_pc[ch] = (unsigned char)prog;
}

void scva_engine_midi(struct scva_engine *e, const uint8_t *d, uint32_t size)
{
  unsigned int msg;
  if (!size) return;
  if (d[0] == 0xf0) { e->long_midi(d, 0); return; }
  if ((d[0] & 0xf0) == 0xc0 && size > 1) {
    e->last_pc[d[0] & 0x0f] = d[1] & 0x7f;
    /* A program change latches the tone map, so CC32 goes first. */
    if (e->map_now > 0)
      e->short_midi(scva_map_cc(d[0] & 0x0f, e->map_now), 0);
  }
  msg = d[0];
  if (size > 1) msg |= (unsigned int)d[1] << 8;
  if (size > 2) msg |= (unsigned int)d[2] << 16;
  e->short_midi(msg, 0);
}

/* TG_Process writes exactly the frames asked for, so it can write straight
   into the host's buffers at an offset, chunked to the declared maximum. */
void scva_engine_render(struct scva_engine *e, float *l, float *r, uint32_t n)
{
  while (n) {
    uint32_t k = n > SCVA_MAXBLOCK ? SCVA_MAXBLOCK : n;
    e->process(l, r, (int)k);
    l += k;
    r += k;
    n -= k;
  }
}
