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

/* One running engine instance. Backends embed this as their first member. */
struct synth {
  const struct synth_backend *backend;
};

/* What an instance is opened with. */
struct synth_options {
  const char *core;           /* the engine's file, as the backend names it */
  double rate;                /* output sample rate */
  int max_block;              /* frames the caller renders at most at once */
};

/* What a backend can do, so callers can adapt to it. */
struct synth_caps {
  const char *name;           /* "scva" */
  int any_rate;               /* renders at whatever rate it is opened at */
  int midi_timestamps;        /* MIDI lands on its sample, not its block */
  int tone_map_per_part;      /* CC32 picks a tone map per part */
};

struct synth_backend {
  const struct synth_caps *caps;
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
};

static inline void synth_close(struct synth *s) { if (s) s->backend->close(s); }
static inline void synth_short(struct synth *s, uint32_t msg) { s->backend->short_msg(s, msg); }
static inline void synth_long(struct synth *s, const uint8_t *msg, size_t len)
{ s->backend->long_msg(s, msg, len); }
static inline void synth_render(struct synth *s, float *l, float *r, uint32_t n)
{ s->backend->render(s, l, r, n); }

/* synth_short in the shape of a map_policy sink. */
static inline void synth_sink(void *s, uint32_t msg) { synth_short((struct synth *)s, msg); }

/* The backends this build has. */
extern const struct synth_backend synth_scva;
#endif
