/* SPDX-License-Identifier: CC0-1.0
 *
 * The exported C entry points of SOUND Canvas VA's engine, and the ONE
 * initialisation order that works.  Nothing here is Roland's code: these are
 * the names its export table publishes and the signatures its calling
 * convention implies.
 */
#ifndef SCVA_CORE_H
#define SCVA_CORE_H
#include <windows.h>
#include <stdio.h>

struct scva_config { int a, b; };

struct scva {
  HMODULE lib;
  int  (*initialize)(int);
  void (*set_sample_rate)(float);
  void (*set_max_block)(int);
  int  (*activate)(int, int);
  void (*deactivate)(void);
  void (*terminate)(void);
  void (*short_midi)(unsigned int);
  void (*long_midi)(const unsigned char *, int);
  void (*process)(float *, float *, int);
  int  (*fatal)(void);
  int  (*set_config)(const struct scva_config *);
  void (*get_config)(struct scva_config *);
};

static int scva_open(struct scva *s, const char *dll, double rate, int maxblock)
{
  memset(s, 0, sizeof *s);
  s->lib = LoadLibraryA(dll);
  if (!s->lib) { fprintf(stderr, "scva: cannot load %s\n", dll); return 0; }
#define SCVA_GET(f, n) \
  do { *(void **)&s->f = (void *)GetProcAddress(s->lib, n); \
       if (!s->f) { fprintf(stderr, "scva: missing export %s\n", n); return 0; } \
  } while (0)
  SCVA_GET(initialize,      "TG_initialize");
  SCVA_GET(set_sample_rate, "TG_setSampleRate");
  SCVA_GET(set_max_block,   "TG_setMaxBlockSize");
  SCVA_GET(activate,        "TG_activate");
  SCVA_GET(deactivate,      "TG_deactivate");
  SCVA_GET(terminate,       "TG_terminate");
  SCVA_GET(short_midi,      "TG_ShortMidiIn");
  SCVA_GET(long_midi,       "TG_LongMidiIn");
  SCVA_GET(process,         "TG_Process");
  SCVA_GET(fatal,           "TG_isFatalError");
  SCVA_GET(set_config,      "TG_XPsetSystemConfig");
  SCVA_GET(get_config,      "TG_XPgetCurSystemConfig");
#undef SCVA_GET

  /* THE ORDER. See README: the rate must be set on BOTH sides, and the second
     call must be the last thing before activate. Anything else and the core
     writes +/-Inf from the second sample while reporting no error at all. */
  if (s->initialize(0) != 0) { fprintf(stderr, "scva: initialize failed\n"); return 0; }
  s->set_sample_rate((float)rate);
  s->set_max_block(maxblock);
  { struct scva_config c; c.a = 1; c.b = 1; s->set_config(&c); }
  s->set_sample_rate((float)rate);          /* <-- last call before activate */
  s->activate(0, 1);
  if (s->fatal()) { fprintf(stderr, "scva: fatal after activate\n"); return 0; }
  return 1;
}

static void scva_gs_reset(struct scva *s)
{
  static const unsigned char gs[] =
    { 0xF0,0x41,0x10,0x42,0x12,0x40,0x00,0x7F,0x00,0x41,0xF7 };
  s->long_midi(gs, (int)sizeof gs);
}

static void scva_close(struct scva *s)
{
  if (s->deactivate) s->deactivate();
  if (s->terminate)  s->terminate();
  if (s->lib) FreeLibrary(s->lib);
}
#endif
