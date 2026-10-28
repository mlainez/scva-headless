/* SPDX-License-Identifier: CC0-1.0
 *
 * The few ALSA calls scva_pcm.h makes, found in libasound.so.2 at run time
 * rather than linked: a renderer then builds without ALSA and runs where
 * there is none, and --play works wherever the library is installed. That
 * matters for the 32-bit build, whose hosts rarely have a 32-bit alsa-lib.
 *
 * The types and constants are ALSA's own, from its stable ABI.
 */
#ifndef ALSA_DL_H
#define ALSA_DL_H
#include <dlfcn.h>
#include <stdio.h>

typedef struct _snd_pcm snd_pcm_t;
typedef long snd_pcm_sframes_t;
typedef unsigned long snd_pcm_uframes_t;
typedef void (*snd_lib_error_handler_t)(const char *file, int line,
                                        const char *function, int err,
                                        const char *fmt, ...);

#define SND_PCM_STREAM_PLAYBACK       0
#define SND_PCM_FORMAT_FLOAT_LE       14
#define SND_PCM_ACCESS_RW_INTERLEAVED 3

static struct {
  void *lib;
  int (*pcm_open)(snd_pcm_t **, const char *, int, int);
  int (*pcm_set_params)(snd_pcm_t *, int, int, unsigned int, unsigned int,
                        int, unsigned int);
  int (*pcm_close)(snd_pcm_t *);
  snd_pcm_sframes_t (*pcm_writei)(snd_pcm_t *, const void *, snd_pcm_uframes_t);
  int (*pcm_recover)(snd_pcm_t *, int, int);
  int (*device_name_hint)(int, const char *, void ***);
  char *(*device_name_get_hint)(const void *, const char *);
  int (*device_name_free_hint)(void **);
  int (*lib_error_set_handler)(snd_lib_error_handler_t);
} alsa_dl;

#define snd_pcm_open              alsa_dl.pcm_open
#define snd_pcm_set_params        alsa_dl.pcm_set_params
#define snd_pcm_close             alsa_dl.pcm_close
#define snd_pcm_writei            alsa_dl.pcm_writei
#define snd_pcm_recover           alsa_dl.pcm_recover
#define snd_device_name_hint      alsa_dl.device_name_hint
#define snd_device_name_get_hint  alsa_dl.device_name_get_hint
#define snd_device_name_free_hint alsa_dl.device_name_free_hint
#define snd_lib_error_set_handler alsa_dl.lib_error_set_handler

/* Loads libasound.so.2 and finds the calls. 0, with the reason in err, if
   it is not installed for this program's width. */
static int alsa_dl_load(char *err, size_t errlen)
{
  struct { void **slot; const char *name; } fn[] = {
    { (void **)&alsa_dl.pcm_open, "snd_pcm_open" },
    { (void **)&alsa_dl.pcm_set_params, "snd_pcm_set_params" },
    { (void **)&alsa_dl.pcm_close, "snd_pcm_close" },
    { (void **)&alsa_dl.pcm_writei, "snd_pcm_writei" },
    { (void **)&alsa_dl.pcm_recover, "snd_pcm_recover" },
    { (void **)&alsa_dl.device_name_hint, "snd_device_name_hint" },
    { (void **)&alsa_dl.device_name_get_hint, "snd_device_name_get_hint" },
    { (void **)&alsa_dl.device_name_free_hint, "snd_device_name_free_hint" },
    { (void **)&alsa_dl.lib_error_set_handler, "snd_lib_error_set_handler" },
  };
  size_t i;
  if (alsa_dl.lib) return 1;
  alsa_dl.lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
  if (!alsa_dl.lib) {
    snprintf(err, errlen, "%s", dlerror());
    return 0;
  }
  for (i = 0; i < sizeof fn / sizeof *fn; ++i) {
    *fn[i].slot = dlsym(alsa_dl.lib, fn[i].name);
    if (!*fn[i].slot) {
      snprintf(err, errlen, "libasound.so.2 has no %s", fn[i].name);
      dlclose(alsa_dl.lib);
      alsa_dl.lib = NULL;
      return 0;
    }
  }
  return 1;
}
#endif
