/* SPDX-License-Identifier: CC0-1.0
 *
 * A synthesizer engine, as the code around it sees one: open an instance,
 * send it MIDI, have it render, close it. Each engine is a backend that
 * fills in struct synth_backend; nothing outside a backend knows which
 * engine it is driving, only what that engine's capabilities are.
 */
#ifndef SYNTH_H
#define SYNTH_H
#include <stddef.h>
#include <stdint.h>

struct synth_backend;

/* One running engine instance. Backends embed this as their first member
   and fill in the rest when they open it. */
struct synth {
  const struct synth_backend *backend;
  double rate;                /* the rate it renders at, which an engine with
                                 fixed rates may not have been asked for */
  int midi_block;             /* MIDI lands on multiples of this many frames
                                 of output; 1 when it lands on any frame */
};

/* What an instance is opened with. */
struct synth_options {
  const char *core;           /* the engine's file, as the backend names it */
  double rate;                /* output sample rate */
  int max_block;              /* frames the caller renders at most at once */
  const void *backend_opts;   /* the backend's own options, NULL for its
                                 defaults; see the backend's header */
};

/* What a backend can do, so callers can adapt to it. */
struct synth_caps {
  const char *name;           /* "scva", "vsc" */
  const char *core_file;      /* its engine's file name: "SCCore.dll" */
  const int *rates;           /* the only rates it renders at, ending in 0;
                                 NULL when it takes any */
  int default_rate;           /* the rate to use when none is asked for; 0
                                 when the caller's own default will do */
  int midi_timestamps;        /* MIDI lands on its sample, not its block */
  int tone_map_per_part;      /* CC32 picks a tone map per part; otherwise
                                 one tone set serves every part */
  const char *set_option;     /* the option choosing that one tone set,
                                 which takes a map name; NULL if none */
};

/* One command-line option a backend takes, so that front ends offer it
   without knowing it. */
struct synth_option {
  const char *flag;           /* "--set" */
  const char *arg;            /* what it takes, as the help shows it */
  const char *help;
  /* Applies value to the backend's options. 0, with the reason in err, if
     the value is not one it takes. */
  int (*apply)(void *opts, const char *value, char *err, size_t errlen);
};

struct synth_backend {
  const struct synth_caps *caps;
  /* Its options: a table ending in a NULL flag, and the struct they fill,
     opts_size bytes set to the defaults by opts_init, which open takes as
     synth_options.backend_opts. No options: NULL, 0, NULL. */
  const struct synth_option *options;
  size_t opts_size;
  void (*opts_init)(void *opts);
  /* NULL, with the reason in err, if the engine cannot be loaded. */
  struct synth *(*open)(const struct synth_options *opt, char *err, size_t errlen);
  /* Stops the engine and frees it. */
  void (*close)(struct synth *s);
  /* Prepares the calling thread to enter an engine of this backend; call it
     on any thread before that thread renders or sends. 0 on failure. */
  int (*attach_thread)(void);
  /* status in the low byte, then the data bytes */
  void (*short_msg)(struct synth *s, uint32_t msg);
  /* a whole SysEx, F0 to F7 */
  void (*long_msg)(struct synth *s, const uint8_t *msg, size_t len);
  /* n frames of stereo, any n */
  void (*render)(struct synth *s, float *left, float *right, uint32_t n);
  /* The engine's own name for a tone (bank is CC0, program the program
     change) or, with drums set, a drum set (by program); NULL where it has
     none. NULL for a backend whose names come from elsewhere. */
  const char *(*name)(struct synth *s, int drums, int bank, int program);
};

static inline void synth_close(struct synth *s) { if (s) s->backend->close(s); }
static inline void synth_short(struct synth *s, uint32_t msg) { s->backend->short_msg(s, msg); }
static inline void synth_long(struct synth *s, const uint8_t *msg, size_t len)
{ s->backend->long_msg(s, msg, len); }
static inline void synth_render(struct synth *s, float *l, float *r, uint32_t n)
{ s->backend->render(s, l, r, n); }
static inline const char *synth_name(struct synth *s, int drums, int bank, int program)
{ return s->backend->name ? s->backend->name(s, drums, bank, program) : NULL; }

/* synth_short in the shape of a map_policy sink. */
static inline void synth_sink(void *s, uint32_t msg) { synth_short((struct synth *)s, msg); }

/* The backends this build has. VSC's engine is 32-bit code. */
extern const struct synth_backend synth_scva;
#if defined(__i386__)
extern const struct synth_backend synth_vsc;
#endif
#endif
