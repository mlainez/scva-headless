/* SPDX-License-Identifier: CC0-1.0
 *
 * Finding and opening an ALSA PCM playback device that will take float32
 * stereo, shared by scva-daemon and scva-native's --play so both pick a
 * card the same way.
 */
#ifndef SCVA_PCM_H
#define SCVA_PCM_H
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Opens a device and configures it in one go, since a device that opens but
   will not take float32 stereo is no more use than one that does not open. */
static snd_pcm_t *try_pcm(const char *name, unsigned int rate,
                          unsigned int latency_us)
{
  snd_pcm_t *pcm = NULL;
  if (snd_pcm_open(&pcm, name, SND_PCM_STREAM_PLAYBACK, 0) < 0) return NULL;
  if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE,
                         SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1,
                         latency_us) < 0) {
    snd_pcm_close(pcm);
    return NULL;
  }
  return pcm;
}

/* Where the system sends audio, in the order to try. "default" is ALSA's own
   answer and is right wherever it works; it fails only when a sound server
   holds the card, so the servers follow. Nothing else is chosen automatically:
   a named card would play wherever that card happens to go, which on this
   machine would be an HDMI monitor. Pass --pcm to reach those. */
static int pcm_rank(const char *n)
{
  if (!strcmp(n, "default"))         return 0;
  if (!strcmp(n, "pipewire"))        return 1;
  if (!strcmp(n, "pulse"))           return 2;
  if (!strcmp(n, "jack"))            return 3;
  if (!strncmp(n, "sysdefault", 10)) return 4;
  return -1;
}

struct pcm_cand { char *name; int rank; };

static int by_rank(const void *a, const void *b)
{
  const struct pcm_cand *x = a, *y = b;
  return x->rank - y->rank;
}

/* ALSA prints its own complaints straight to stderr, which during probing is
   noise about devices we are about to reject anyway. */
static void alsa_quiet(const char *file, int line, const char *fn, int err,
                       const char *fmt, ...)
{
  (void)file; (void)line; (void)fn; (void)err; (void)fmt;
}

/* Asks ALSA what this machine actually has, rather than guessing names: the
   same hint list `aplay -L` prints. "default" is prepended because it is not
   always in that list yet is the right answer wherever it works - it is a
   dmix on the raw card, so it fails only when a sound server holds it. */
static size_t pcm_candidates(struct pcm_cand *out, size_t cap)
{
  void **hints = NULL;
  size_t n = 0;
  void **h;

  if (cap) {
    out[0].name = strdup("default");
    out[0].rank = pcm_rank("default");
    if (out[0].name) n = 1;
  }
  if (snd_device_name_hint(-1, "pcm", &hints) < 0) return n;
  for (h = hints; *h && n < cap; ++h) {
    char *name = snd_device_name_get_hint(*h, "NAME");
    char *ioid = snd_device_name_get_hint(*h, "IOID");
    int rank;
    /* IOID is NULL for duplex devices, "Output" for playback-only */
    if (name && strcmp(name, "default") &&      /* already first */
        (!ioid || !strcmp(ioid, "Output")) &&
        (rank = pcm_rank(name)) >= 0) {
      out[n].name = name;
      out[n].rank = rank;
      ++n;
      name = NULL;                        /* kept, freed by the caller */
    }
    free(name);
    free(ioid);
  }
  snd_device_name_free_hint(hints);
  qsort(out, n, sizeof *out, by_rank);
  return n;
}

/* An explicit --pcm is honoured as given and never second-guessed. */
static snd_pcm_t *open_pcm(const char *want, unsigned int rate,
                           unsigned int latency_us, const char **opened,
                           int verbose)
{
  struct pcm_cand cand[64];
  static char chosen[128];
  snd_pcm_t *pcm = NULL;
  size_t n, i;

  if (want) {
    *opened = want;                 /* asked for by name: let ALSA complain */
    return try_pcm(want, rate, latency_us);
  }
  snd_lib_error_set_handler(alsa_quiet);
  n = pcm_candidates(cand, sizeof cand / sizeof cand[0]);
  for (i = 0; i < n; ++i) {
    if (!pcm && (pcm = try_pcm(cand[i].name, rate, latency_us)) != NULL) {
      snprintf(chosen, sizeof chosen, "%s", cand[i].name);
      *opened = chosen;
    } else if (verbose && !pcm) {
      fprintf(stderr, "  '%s' did not open\n", cand[i].name);
    }
    free(cand[i].name);
  }
  snd_lib_error_set_handler(NULL);
  if (!pcm) *opened = "default";
  return pcm;
}
#endif
