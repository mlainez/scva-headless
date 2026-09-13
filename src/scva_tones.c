/* SPDX-License-Identifier: CC0-1.0
 *
 * Writes the tone table this project reads, from whatever names are available
 * on this machine: a SOUND Canvas VA install if there is one, the built-in
 * General MIDI set otherwise.
 *
 * The output is yours, derived from your own install. It is a starting point
 * for a table checked against published documentation - not something to
 * redistribute, which is why this project ships the converter and not its
 * result.
 *
 *   scva-tones [--core DIR] > scva-tones.txt
 *
 * Put the file beside SCCore.dll, or point $SCVA_TONES at it, and the plugins
 * will use it in preference to the vendor's files.
 */
#define _GNU_SOURCE
#include "scva_names.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
  struct scva_names N;
  const char *dir = getenv("SCVA_DLL_DIR");
  size_t i;
  int a;

  for (a = 1; a < argc; ++a) {
    if (!strcmp(argv[a], "--core") && a + 1 < argc) dir = argv[++a];
    else if (!strcmp(argv[a], "-h") || !strcmp(argv[a], "--help")) {
      fprintf(stderr, "usage: %s [--core DIR] > scva-tones.txt\n", argv[0]);
      return 0;
    } else {
      fprintf(stderr, "%s: unknown argument %s\n", argv[0], argv[a]);
      return 2;
    }
  }
  if (!dir) dir = ".";
  if (!scva_names_load(&N, dir)) {
    fprintf(stderr, "%s: no names found in %s\n", argv[0], dir);
    return 1;
  }

  printf("# scva-headless tone table\n");
  printf("# module: %s\n", N.module);
  printf("#\n");
  printf("#   t <map> <bank> <program> <name>   melodic tone\n");
  printf("#   d <map> <bank> <program> <name>   drum kit\n");
  printf("#   k <map> <program> <key> <name>    one key inside a kit\n");
  printf("#\n");
  printf("# map is the CC32 value, bank the CC0 value, both as sent.\n");
  for (i = 0; i < N.npatch; ++i) {
    const struct scva_patch *p = &N.patch[i];
    printf("%c %u %u %u %s\n", p->drum ? 'd' : 't', p->map, p->bank, p->prog,
           p->name);
  }
  for (i = 0; i < N.nkey; ++i) {
    const struct scva_key *k = &N.key[i];
    printf("k %u %u %u %s\n", k->map, k->prog, k->key, k->name);
  }
  fprintf(stderr, "%zu patches, %zu key names\n", N.npatch, N.nkey);
  scva_names_free(&N);
  return 0;
}
