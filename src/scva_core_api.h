/* SPDX-License-Identifier: CC0-1.0
 *
 * SCCore.dll's exported C API: the one table every tool and plugin calls the
 * engine through, whichever loader mapped the core.
 *
 * The signatures were read from the exported functions' prologues - which
 * registers each one consumes before writing - so they are the calling
 * convention and not an algorithm. See docs/ENGINE-NOTES.md for what each
 * argument means and the order the engine needs them called in.
 */
#ifndef SCVA_CORE_API_H
#define SCVA_CORE_API_H

#ifdef _WIN32
#include <windows.h>
#endif

/* The exports are Microsoft cdecl: ms_abi on x86-64, cdecl on i386. A
   Windows compiler uses that already; on Linux x86-64 it must be asked for. */
#if defined(_WIN32)
#define SCVA_CORE_CC __cdecl
#elif defined(__x86_64__)
#define SCVA_CORE_CC __attribute__((ms_abi))
#else
#define SCVA_CORE_CC
#endif

/* Reads two 32-bit fields through a pointer; the getter writes eight bytes
   back, so the configuration is exactly this pair. */
struct tg_system_config { int a, b; };

typedef SCVA_CORE_CC int  (*scva_tg_initialize_fn)(int);
typedef SCVA_CORE_CC void (*scva_tg_set_sample_rate_fn)(float);
typedef SCVA_CORE_CC void (*scva_tg_set_max_block_fn)(int);
/* rate in XMM0; second argument is the max block size */
typedef SCVA_CORE_CC int  (*scva_tg_activate_fn)(float, int);
typedef SCVA_CORE_CC void (*scva_tg_deactivate_fn)(void);
/* calls exit(): nothing after it runs */
typedef SCVA_CORE_CC void (*scva_tg_terminate_fn)(void);
/* second argument is a timestamp in samples */
typedef SCVA_CORE_CC void (*scva_tg_short_midi_fn)(unsigned int, int);
/* F7-terminated, so the second argument is a timestamp too */
typedef SCVA_CORE_CC void (*scva_tg_long_midi_fn)(const unsigned char *, int);
typedef SCVA_CORE_CC void (*scva_tg_process_fn)(float *, float *, int);
typedef SCVA_CORE_CC int  (*scva_tg_running_voices_fn)(void);
typedef SCVA_CORE_CC int  (*scva_tg_is_fatal_fn)(void);
typedef SCVA_CORE_CC const char *(*scva_tg_error_strings_fn)(int);
typedef SCVA_CORE_CC int  (*scva_tg_set_config_fn)(const struct tg_system_config *);
typedef SCVA_CORE_CC void (*scva_tg_get_config_fn)(struct tg_system_config *);

struct scva_core_api {
  scva_tg_initialize_fn initialize;
  scva_tg_set_sample_rate_fn set_sample_rate;
  scva_tg_set_max_block_fn set_max_block;
  scva_tg_activate_fn activate;
  scva_tg_deactivate_fn deactivate;
  scva_tg_terminate_fn terminate;
  scva_tg_short_midi_fn short_midi;
  scva_tg_long_midi_fn long_midi;
  scva_tg_process_fn process;
  scva_tg_running_voices_fn voices;
  scva_tg_is_fatal_fn fatal;
  scva_tg_set_config_fn set_config;
  scva_tg_get_config_fn get_config;
  scva_tg_error_strings_fn errors;
};

/* Which exports a caller cannot do without. */
#define SCVA_CORE_INITIALIZE   0x0001u
#define SCVA_CORE_SAMPLE_RATE  0x0002u
#define SCVA_CORE_MAX_BLOCK    0x0004u
#define SCVA_CORE_ACTIVATE     0x0008u
#define SCVA_CORE_DEACTIVATE   0x0010u
#define SCVA_CORE_TERMINATE    0x0020u
#define SCVA_CORE_SHORT_MIDI   0x0040u
#define SCVA_CORE_LONG_MIDI    0x0080u
#define SCVA_CORE_PROCESS      0x0100u
#define SCVA_CORE_VOICES       0x0200u
#define SCVA_CORE_FATAL        0x0400u
#define SCVA_CORE_SET_CONFIG   0x0800u
#define SCVA_CORE_GET_CONFIG   0x1000u
#define SCVA_CORE_ERRORS       0x2000u
/* What a live engine needs: set it up, feed it MIDI, render, stop it. */
#define SCVA_CORE_LIVE (SCVA_CORE_INITIALIZE | SCVA_CORE_SAMPLE_RATE | \
  SCVA_CORE_MAX_BLOCK | SCVA_CORE_SET_CONFIG | SCVA_CORE_ACTIVATE | \
  SCVA_CORE_DEACTIVATE | SCVA_CORE_SHORT_MIDI | SCVA_CORE_LONG_MIDI | \
  SCVA_CORE_PROCESS)
/* Everything the table holds. */
#define SCVA_CORE_ALL 0x3fffu

/* Looks an export up in a loaded core; NULL when it has none of that name. */
typedef void *(*scva_core_lookup_fn)(void *core, const char *name);

/* Fills api from the core, then returns the name of the first export in
   required that it lacks, or NULL when it has them all. */
static inline const char *scva_core_bind(struct scva_core_api *api,
                                         scva_core_lookup_fn lookup,
                                         void *core, unsigned required)
{
  const char *missing = NULL;
#define SCVA__BIND(field, type, bit, name)                                  \
  api->field = (type)lookup(core, name);                                    \
  if (!api->field && (required & (bit)) && !missing) missing = name;
  SCVA__BIND(initialize, scva_tg_initialize_fn, SCVA_CORE_INITIALIZE, "TG_initialize")
  SCVA__BIND(set_sample_rate, scva_tg_set_sample_rate_fn, SCVA_CORE_SAMPLE_RATE, "TG_setSampleRate")
  SCVA__BIND(set_max_block, scva_tg_set_max_block_fn, SCVA_CORE_MAX_BLOCK, "TG_setMaxBlockSize")
  SCVA__BIND(activate, scva_tg_activate_fn, SCVA_CORE_ACTIVATE, "TG_activate")
  SCVA__BIND(deactivate, scva_tg_deactivate_fn, SCVA_CORE_DEACTIVATE, "TG_deactivate")
  SCVA__BIND(terminate, scva_tg_terminate_fn, SCVA_CORE_TERMINATE, "TG_terminate")
  SCVA__BIND(short_midi, scva_tg_short_midi_fn, SCVA_CORE_SHORT_MIDI, "TG_ShortMidiIn")
  SCVA__BIND(long_midi, scva_tg_long_midi_fn, SCVA_CORE_LONG_MIDI, "TG_LongMidiIn")
  SCVA__BIND(process, scva_tg_process_fn, SCVA_CORE_PROCESS, "TG_Process")
  SCVA__BIND(voices, scva_tg_running_voices_fn, SCVA_CORE_VOICES, "TG_XPgetCurTotalRunningVoices")
  SCVA__BIND(fatal, scva_tg_is_fatal_fn, SCVA_CORE_FATAL, "TG_isFatalError")
  SCVA__BIND(set_config, scva_tg_set_config_fn, SCVA_CORE_SET_CONFIG, "TG_XPsetSystemConfig")
  SCVA__BIND(get_config, scva_tg_get_config_fn, SCVA_CORE_GET_CONFIG, "TG_XPgetCurSystemConfig")
  SCVA__BIND(errors, scva_tg_error_strings_fn, SCVA_CORE_ERRORS, "TG_getErrorStrings")
#undef SCVA__BIND
  return missing;
}

/* Lookups for the two ways a core gets loaded. */
#ifdef SCVA_PE_LOADER_H
static inline void *scva_core_lookup_pe(void *core, const char *name)
{
  return pe_symbol((struct pe_image *)core, name);
}
#endif
#ifdef _WIN32
static inline void *scva_core_lookup_module(void *core, const char *name)
{
  return (void *)GetProcAddress((HMODULE)core, name);
}
#endif
#endif
