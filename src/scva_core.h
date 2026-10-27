/* SPDX-License-Identifier: CC0-1.0
 *
 * How SCCore.dll must be driven: the order its setup calls go in, and the
 * resets that put it in a known state. Every tool and plugin starts its
 * engines here, so the order lives in one place.
 */
#ifndef SCVA_CORE_H
#define SCVA_CORE_H
#include <string.h>
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

/* GS Reset: every part back to its power-on GS state. */
static inline void scva_core_gs_reset(const struct scva_core_api *api)
{
  static const unsigned char gs_reset[] = {
    0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
  api->long_midi(gs_reset, 0);
}

/* GM System On, the General MIDI equivalent. */
static inline void scva_core_gm_reset(const struct scva_core_api *api)
{
  static const unsigned char gm_reset[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
  api->long_midi(gm_reset, 0);
}
#endif
