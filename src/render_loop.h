/* SPDX-License-Identifier: CC0-1.0
 *
 * Playing a Standard MIDI File into one or two engines, block by block:
 * each event is sent ahead of the block it falls in, both ports are rendered
 * and mixed, and the mix goes to whatever the caller does with audio. Pacing
 * and output are the caller's, so a WAV writer, an ALSA device and waveOut
 * all drive the engines from exactly the same events.
 *
 * An engine that takes MIDI anywhere is fed in blocks of RENDER_BLOCK, each
 * event ahead of the block it falls in. One whose MIDI lands only on its own
 * blocks (synth.h's midi_block) is fed in those blocks, each event ahead of
 * the boundary nearest it.
 */
#ifndef RENDER_LOOP_H
#define RENDER_LOOP_H
#include <stddef.h>
#include <stdint.h>
#include "map_policy.h"

#define RENDER_BLOCK 256

struct song;

/* One engine: port A, or port B of a two-port song. */
struct render_port {
  struct map_policy *maps;                          /* short messages */
  void (*long_msg)(void *ctx, const unsigned char *msg, size_t len);
  void (*process)(void *ctx, float *left, float *right, int frames);
  void *ctx;
};

struct render_io {
  /* Before each block is rendered; frame is where it starts. May be NULL. */
  void (*pace)(void *ctx, uint64_t frame);
  /* Each mixed block. 0 stops the song there. */
  int (*deliver)(void *ctx, const float *left, const float *right, int frames,
                 uint64_t frame);
  void *ctx;
};

struct render_result {
  uint32_t frames;
  float peak;                   /* largest absolute sample, either side */
  int sent;                     /* MIDI messages sent, SysEx included */
};

/* Plays song s to its end and then tail seconds more, at rate, to engines
   whose MIDI lands on multiples of midi_block frames (1: anywhere). Events
   on a port above 0 go to ports[1] when nports is 2; SysEx goes to every
   port. 0 on success, -1 if out of memory. */
int render_song(const struct song *s, double rate, double tail, int midi_block,
                const struct render_port *ports, int nports,
                const struct render_io *io, struct render_result *res);
#endif
