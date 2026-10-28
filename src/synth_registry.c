/* SPDX-License-Identifier: CC0-1.0
 *
 * See synth_registry.h.
 */
#include "synth_registry.h"
#include "win9x.h"
#include <string.h>

const struct synth_backend *const synth_backends[] = {
  &synth_scva,
#if defined(__i386__)
  &synth_vsc,
#endif
  NULL
};

const struct synth_backend *synth_find(const char *name)
{
  int i;
  for (i = 0; synth_backends[i]; ++i)
    if (!strcmp(synth_backends[i]->caps->name, name)) return synth_backends[i];
  return NULL;
}

const char *synth_engine_names(void)
{
  static char names[64];
  int i;
  if (!names[0])
    for (i = 0; synth_backends[i]; ++i) {
      if (i) strcat(names, "|");
      strcat(names, synth_backends[i]->caps->name);
    }
  return names;
}

int synth_parse_option(const struct synth_backend *b, void *opts, int argc,
                       char **argv, int *i, char *err, size_t errlen)
{
  const struct synth_option *o;
  if (!b->options) return 0;
  for (o = b->options; o->flag; ++o) {
    if (strcmp(argv[*i], o->flag)) continue;
    if (*i + 1 >= argc) {
      snprintf(err, errlen, "%s takes %s", o->flag, o->arg);
      return -1;
    }
    if (!o->apply(opts, argv[*i + 1], err, errlen)) return -1;
    *i += 1;
    return 1;
  }
  return 0;
}

int synth_check_rate(const struct synth_backend *b, double rate, char *err,
                     size_t errlen)
{
  const int *r = b->caps->rates;
  size_t at;
  if (!r) return 1;
  for (; *r; ++r)
    if ((double)*r == rate) return 1;
  at = (size_t)snprintf(err, errlen, "--engine %s renders at", b->caps->name);
  for (r = b->caps->rates; *r && at < errlen; ++r)
    at += (size_t)snprintf(err + at, errlen - at, "%s %d", r == b->caps->rates ? "" :
                           r[1] ? "," : " or", *r);
  if (at < errlen)
    snprintf(err + at, errlen - at, " Hz, not %.0f: nothing resamples for it", rate);
  return 0;
}

void synth_print_options(const struct synth_backend *b, FILE *f)
{
  const struct synth_option *o;
  if (!b->options) return;
  for (o = b->options; o->flag; ++o) {
    char lhs[64];
    snprintf(lhs, sizeof lhs, "%s %s", o->flag, o->arg);
    fprintf(f, "  %-22s %s\n", lhs, o->help);
  }
}
