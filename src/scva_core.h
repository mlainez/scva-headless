/* SPDX-License-Identifier: CC0-1.0
 *
 * How SCCore.dll must be driven: the order its setup calls go in, and the
 * resets that put it in a known state. Every tool and plugin starts its
 * engines here, so the order lives in one place.
 */
#ifndef SCVA_CORE_H
#define SCVA_CORE_H
#include <stdint.h>
#include <string.h>
#include "midi_reset.h"
#include "scva_core_api.h"

/* How to bring one engine up. */
struct scva_core_setup {
  float rate;
  int max_block;                    /* at least SCVA_MIN_BLOCK */
  int init_arg;                     /* TG_initialize's argument */
  struct tg_system_config config;   /* normally 1, 1 */
};

/* What the engine answered on the way, for tools that show it. */
struct scva_core_report {
  int initialized;                  /* TG_initialize */
  int config_set;                   /* TG_XPsetSystemConfig */
  struct tg_system_config config;   /* TG_XPgetCurSystemConfig after it */
  int activated;                    /* TG_activate */
  int fatal;                        /* TG_isFatalError after it */
};

/* Initialises and activates an engine. The rate is set on both sides of the
   block size and once more as the last call before activate. Measured on a
   probe varying one call at a time:

     rate, block, rate             finite 2048 of 2048
     rate, block, config, rate     finite 2048 of 2048
     rate, block                   +/-Inf from the second sample, then NaN
     rate, block, config           the same
     block, rate                   the same
     no rate at all                the same

   so the block size invalidates the rate-derived state, and nothing reports
   it. With a report, the config is read back and the fatal flag checked. */
static inline void scva_core_start(const struct scva_core_api *api,
                                   const struct scva_core_setup *setup,
                                   struct scva_core_report *report)
{
  int initialized, config_set;
  initialized = api->initialize(setup->init_arg);
  api->set_sample_rate(setup->rate);
  api->set_max_block(setup->max_block);
  config_set = api->set_config(&setup->config);
  if (report) {
    report->initialized = initialized;
    report->config_set = config_set;
    memset(&report->config, 0, sizeof report->config);
    api->get_config(&report->config);
  }
  api->set_sample_rate(setup->rate);
  if (report) {
    report->activated = api->activate(setup->rate, setup->max_block);
    report->fatal = api->fatal();
  } else {
    api->activate(setup->rate, setup->max_block);
  }
}

/* TG_ShortMidiIn, untimed, in the shape of a map_policy sink. */
static inline void scva_core_sink(void *api, uint32_t msg)
{
  ((const struct scva_core_api *)api)->short_midi(msg, 0);
}

/* TG_LongMidiIn and TG_Process, in the shape of a render_loop port. */
/* The core reads up to F7, so the length goes unused. */
static inline void scva_core_long_sink(void *api, const unsigned char *msg,
                                       size_t len)
{
  (void)len;
  ((const struct scva_core_api *)api)->long_midi(msg, 0);
}

static inline void scva_core_process_sink(void *api, float *l, float *r, int n)
{
  ((const struct scva_core_api *)api)->process(l, r, n);
}

static inline void scva_core_gs_reset(const struct scva_core_api *api)
{
  api->long_midi(midi_gs_reset(NULL), 0);
}

static inline void scva_core_gm_reset(const struct scva_core_api *api)
{
  api->long_midi(midi_gm_reset(NULL), 0);
}
#endif
