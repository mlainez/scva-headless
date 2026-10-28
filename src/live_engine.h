/* SPDX-License-Identifier: CC0-1.0
 *
 * The engine of a live front end (scva-daemon, scva-winmidi): any synth.h
 * backend, its tone maps, and the console's say over them. An engine with
 * one tone set for every part changes sets by being reopened on the new one
 * and reset, as VSC itself does.
 */
#ifndef LIVE_ENGINE_H
#define LIVE_ENGINE_H
#include "console.h"
#include "map_policy.h"
#include "synth.h"

struct live_engine {
  const struct synth_backend *backend;
  struct synth_options opt;
  void *opts;                   /* the backend's own options */
  struct synth *synth;          /* the engine playing now */
  struct map_policy maps;
};

/* Opens backend b's engine on core at rate (or b's own default rate, if
   rate_given is 0 and b has one), rendering block frames at a time, with
   b's options opts, reset to GS and on map (mapname). 0, with the reason in
   err, on failure: 2 for something the command line got wrong, 1 for the
   engine. */
int live_engine_open(struct live_engine *l, const struct synth_backend *b,
                     const char *core, double rate, int rate_given, int block,
                     void *opts, int map, const char *mapname,
                     int *status, char *err, size_t errlen);

/* Points console c at l's maps, and at l's tone set where it has one. */
void live_engine_console(struct live_engine *l, struct console *c);

void live_engine_close(struct live_engine *l);
#endif
