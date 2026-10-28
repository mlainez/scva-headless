/* SPDX-License-Identifier: CC0-1.0
 *
 * See scva_lists.h.
 */
#include "scva_lists.h"
#include "scva_map.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *scva_engine_dir(const char *file, const char *fallback, char *buf,
                            size_t cap)
{
  const char *env = getenv("SCVA_DLL_DIR");
  if (env && *env) {
    FILE *f;
    snprintf(buf, cap, "%s/%s", env, file);
    f = fopen(buf, "rb");
    if (f) {
      fclose(f);
      snprintf(buf, cap, "%s", env);
      return buf;
    }
  }
  snprintf(buf, cap, "%s", fallback && *fallback ? fallback : ".");
  return buf;
}

const char *scva_core_dir(const char *fallback, char *buf, size_t cap)
{
  return scva_engine_dir("SCCore.dll", fallback, buf, cap);
}

int scva_lists_build(struct scva_lists *L, const struct scva_names *N)
{
  size_t i, n = N->npatch ? N->npatch : 1;
  int m, top = SCVA_MAP_DEFAULT, has[SCVA_MAPS] = {0};

  memset(L, 0, sizeof *L);
  for (i = 0; i < N->npatch; ++i)
    if (N->patch[i].map < SCVA_MAPS) {
      has[N->patch[i].map] = 1;
      if (N->patch[i].map > top) top = N->patch[i].map;
    }
  L->has_maps = top != SCVA_MAP_DEFAULT;
  for (m = 0; m < SCVA_MAPS; ++m) {
    int src = !L->has_maps ? SCVA_MAP_DEFAULT :
              m == SCVA_MAP_DEFAULT || !has[m] ? top : m;
    L->tone[m] = calloc(n, sizeof *L->tone[m]);
    L->kit[m] = calloc(n, sizeof *L->kit[m]);
    if (!L->tone[m] || !L->kit[m]) { scva_lists_free(L); return 0; }
    for (i = 0; i < N->npatch; ++i) {
      if (N->patch[i].map != src) continue;
      if (N->patch[i].drum) L->kit[m][L->nkit[m]++] = (uint32_t)i;
      else L->tone[m][L->ntone[m]++] = (uint32_t)i;
    }
    if (L->ntone[m] > L->ntone_max) L->ntone_max = L->ntone[m];
    if (L->nkit[m] > L->nkit_max) L->nkit_max = L->nkit[m];
  }
  return 1;
}

void scva_lists_free(struct scva_lists *L)
{
  int m;
  for (m = 0; m < SCVA_MAPS; ++m) {
    free(L->tone[m]);
    free(L->kit[m]);
  }
  memset(L, 0, sizeof *L);
}

int scva_lists_find(const struct scva_names *N, const uint32_t *list,
                    uint32_t n, int bank, int prog)
{
  uint32_t i;
  for (i = 0; i < n; ++i) {
    const struct scva_patch *p = &N->patch[list[i]];
    if (p->bank == bank && p->prog == prog) return (int)i;
  }
  return -1;
}

int scva_lists_carry(const struct scva_names *N, const struct scva_patch *p,
                     const uint32_t *list, uint32_t n)
{
  int k = scva_lists_find(N, list, n, p->bank, p->prog);
  if (k < 0) k = scva_lists_find(N, list, n, 0, p->prog);
  return k < 0 ? 0 : k;
}

void scva_patch_label(const struct scva_patch *p, char *out, size_t cap)
{
  if (p->bank) snprintf(out, cap, "%03d-%03d %s", p->prog + 1, p->bank, p->name);
  else snprintf(out, cap, "%03d %s", p->prog + 1, p->name);
}

void scva_patch_label_map(const struct scva_patch *p, char *out, size_t cap)
{
  const char *tag = scva_map_tag(p->map);
  char in[SCVA_NAME_MAX + 16];
  scva_patch_label(p, in, sizeof in);
  if (*tag) snprintf(out, cap, "SC-%s %s", tag, in);
  else snprintf(out, cap, "%s", in);
}
