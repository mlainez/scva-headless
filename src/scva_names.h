/* SPDX-License-Identifier: CC0-1.0
 *
 * Reads the tone and drum name files that ship with SOUND Canvas VA so a host
 * can offer instrument names instead of bank and program numbers.
 *
 * Nothing of Roland's is distributed with this project. These files are read
 * from the user's own install, beside SCCore.dll; when they are absent the
 * plugin still runs, it simply has no names to offer.
 *
 * The format is tab-separated text:
 *
 *   SSW TONEFILE Ver 2.0
 *   MODULENAME=SC-8820
 *   BANKCONTROLCC#=0       controller carrying the bank axis: 0 here, 32 for GM2
 *   PAGECOUNT=5
 *   PAGE=2 <TAB> 88Map     one page per tone map, numbered as its CC32 value
 *   TONECOUNT=128
 *    <TAB> 0 <TAB> 1 ...   header row: the bank axis
 *   1 <TAB> Piano 1 ...    one row per program, 1-based
 *
 * .tnf holds melodic tones and .drf drum kits. .drk names the keys inside a
 * kit, and transposes the axes: its header is the kit's program number and
 * each row is a MIDI note.
 */
#ifndef SCVA_NAMES_H
#define SCVA_NAMES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCVA_NAME_MAX 32
#define SCVA_MAPS     5

struct scva_patch {
  unsigned char map;            /* tone map, numbered as CC32 */
  unsigned char bank;           /* value on the file's bank controller */
  unsigned char prog;           /* 0..127, as sent in a program change */
  unsigned char drum;           /* a kit rather than a melodic tone */
  char name[SCVA_NAME_MAX];
};

struct scva_key {
  unsigned char map;
  unsigned char prog;           /* the kit this key belongs to */
  unsigned char key;            /* MIDI note */
  char name[SCVA_NAME_MAX];
};

struct scva_names {
  struct scva_patch *patch;
  size_t npatch;
  struct scva_key *key;
  size_t nkey;
  char module[32];              /* "SC-8820" */
  unsigned char bank_cc;        /* 0 or 32 */
};

/* "" for the default map, so a name can be prefixed unconditionally. */
static inline const char *scva_map_tag(int map)
{
  static const char *const tag[SCVA_MAPS] = { "", "55", "88", "88Pro", "8820" };
  return (map >= 0 && map < SCVA_MAPS) ? tag[map] : "";
}

static inline char *scva__slurp(const char *path, size_t *len)
{
  FILE *f = fopen(path, "rb");
  char *buf;
  long n;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  n = ftell(f);
  if (n <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
  buf = malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
    free(buf); fclose(f); return NULL;
  }
  fclose(f);
  buf[n] = '\0';
  *len = (size_t)n;
  return buf;
}

/* Trims both ends and rejects the placeholders the files use for "nothing
   here": an empty cell, or "---" for a key a kit leaves silent. */
static inline int scva__cell(const char *s, size_t n, char *out)
{
  size_t a = 0, b = n;
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) --b;
  if (b == a) return 0;
  if (b - a == 3 && !memcmp(s + a, "---", 3)) return 0;
  if (b - a >= SCVA_NAME_MAX) b = a + SCVA_NAME_MAX - 1;
  memcpy(out, s + a, b - a);
  out[b - a] = '\0';
  return 1;
}

#define SCVA__GROW(arr, n, cap, type)                                   \
  do {                                                                  \
    if ((n) == (cap)) {                                                 \
      size_t nc__ = (cap) ? (cap) * 2 : 256;                            \
      type *p__ = (type *)realloc((arr), nc__ * sizeof *(arr));         \
      if (!p__) { free(buf); return 0; }                                \
      (arr) = p__; (cap) = nc__;                                        \
    }                                                                   \
  } while (0)

/* One pass over one file. kind: 0 tones, 1 kits, 2 keys. */
static inline int scva__parse(struct scva_names *N, const char *path, int kind,
                       size_t *cap_patch, size_t *cap_key)
{
  size_t len = 0;
  char *buf = scva__slurp(path, &len);
  char *line, *end, *next;
  int axis[129];                /* header row: bank numbers, or kit programs */
  int naxis = 0, map = -1;

  if (!buf) return 0;
  for (line = buf; line < buf + len; line = next) {
    end = strchr(line, '\n');
    if (!end) end = buf + len;
    next = end + 1;
    while (end > line && (end[-1] == '\r' || end[-1] == '\n')) --end;
    *end = '\0';
    if (line == end || *line == ';') continue;

    if (!strncmp(line, "MODULENAME=", 11)) {
      scva__cell(line + 11, strlen(line + 11), N->module);
      continue;
    }
    if (!strncmp(line, "BANKCONTROLCC#=", 15)) {
      N->bank_cc = (unsigned char)atoi(line + 15);
      continue;
    }
    if (!strncmp(line, "PAGE=", 5)) {
      map = atoi(line + 5);
      naxis = 0;
      continue;
    }
    /* Matched by prefix, not by looking for '=' anywhere: a tone name is
       free to contain one. */
    if (!strncmp(line, "SSW", 3) || !strncmp(line, "PAGECOUNT=", 10) ||
        !strncmp(line, "KIND=", 5) || !strncmp(line, "TONECOUNT=", 10) ||
        !strncmp(line, "DRUMKEYCOUNT=", 13))
      continue;
    if (map < 0 || map >= SCVA_MAPS || !strchr(line, '\t')) continue;

    {
      char *tab = strchr(line, '\t');
      char label[16];
      char *p;
      int row, i;

      if (!naxis) {                     /* header row: a blank first cell */
        for (p = tab + 1; p <= end && naxis < 129; ) {
          char *q = strchr(p, '\t');
          if (!q || q > end) q = end;
          axis[naxis++] = atoi(p);
          p = q + 1;
        }
        continue;
      }
      if (!scva__cell(line, (size_t)(tab - line), label)) continue;
      row = atoi(label);
      /* .tnf and .drf number programs from 1; .drk numbers notes from 0. */
      if (kind != 2) --row;
      if (row < 0 || row > 127) continue;

      for (i = 0, p = tab + 1; i < naxis && p <= end; ++i) {
        char *q = strchr(p, '\t');
        char cell[SCVA_NAME_MAX];
        if (!q || q > end) q = end;
        if (scva__cell(p, (size_t)(q - p), cell)) {
          if (kind == 2) {
            SCVA__GROW(N->key, N->nkey, *cap_key, struct scva_key);
            N->key[N->nkey].map = (unsigned char)map;
            N->key[N->nkey].prog = (unsigned char)axis[i];
            N->key[N->nkey].key = (unsigned char)row;
            memcpy(N->key[N->nkey].name, cell, sizeof cell);
            ++N->nkey;
          } else {
            SCVA__GROW(N->patch, N->npatch, *cap_patch, struct scva_patch);
            N->patch[N->npatch].map = (unsigned char)map;
            N->patch[N->npatch].bank = (unsigned char)axis[i];
            N->patch[N->npatch].prog = (unsigned char)row;
            N->patch[N->npatch].drum = (unsigned char)(kind == 1);
            memcpy(N->patch[N->npatch].name, cell, sizeof cell);
            ++N->npatch;
          }
        }
        p = q + 1;
      }
    }
  }
  free(buf);
  return 1;
}

#undef SCVA__GROW

/* The General MIDI 1 sound set, as published by the MIDI Manufacturers
   Association. It is an industry specification rather than anyone's product
   data, so it can be built in, and it gives a usable fallback when the tone
   files are not installed: the first 128 programs of every GS module are the
   GM set. */
static const char *const scva_gm_tone[128] = {
  "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano",
  "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord",
  "Clavi", "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba",
  "Xylophone", "Tubular Bells", "Dulcimer", "Drawbar Organ",
  "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ",
  "Accordion", "Harmonica", "Tango Accordion", "Acoustic Guitar (nylon)",
  "Acoustic Guitar (steel)", "Electric Guitar (jazz)",
  "Electric Guitar (clean)", "Electric Guitar (muted)", "Overdriven Guitar",
  "Distortion Guitar", "Guitar harmonics", "Acoustic Bass",
  "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass",
  "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2", "Violin",
  "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings",
  "Orchestral Harp", "Timpani", "String Ensemble 1", "String Ensemble 2",
  "SynthStrings 1", "SynthStrings 2", "Choir Aahs", "Voice Oohs",
  "Synth Voice", "Orchestra Hit", "Trumpet", "Trombone", "Tuba",
  "Muted Trumpet", "French Horn", "Brass Section", "SynthBrass 1",
  "SynthBrass 2", "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax",
  "Oboe", "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute",
  "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle",
  "Ocarina", "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
  "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)",
  "Lead 8 (bass + lead)", "Pad 1 (new age)", "Pad 2 (warm)",
  "Pad 3 (polysynth)", "Pad 4 (choir)", "Pad 5 (bowed)", "Pad 6 (metallic)",
  "Pad 7 (halo)", "Pad 8 (sweep)", "FX 1 (rain)", "FX 2 (soundtrack)",
  "FX 3 (crystal)", "FX 4 (atmosphere)", "FX 5 (brightness)",
  "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo",
  "Shamisen", "Koto", "Kalimba", "Bag pipe", "Fiddle", "Shanai",
  "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum",
  "Melodic Tom", "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise",
  "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter",
  "Applause", "Gunshot"
};

/* The GM percussion map, notes 35 to 81. */
#define SCVA_GM_DRUM_LO 35
#define SCVA_GM_DRUM_HI 81
static const char *const scva_gm_drum[SCVA_GM_DRUM_HI - SCVA_GM_DRUM_LO + 1] = {
  "Acoustic Bass Drum", "Bass Drum 1", "Side Stick", "Acoustic Snare",
  "Hand Clap", "Electric Snare", "Low Floor Tom", "Closed Hi Hat",
  "High Floor Tom", "Pedal Hi-Hat", "Low Tom", "Open Hi-Hat", "Low-Mid Tom",
  "Hi-Mid Tom", "Crash Cymbal 1", "High Tom", "Ride Cymbal 1",
  "Chinese Cymbal", "Ride Bell", "Tambourine", "Splash Cymbal", "Cowbell",
  "Crash Cymbal 2", "Vibraslap", "Ride Cymbal 2", "Hi Bongo", "Low Bongo",
  "Mute Hi Conga", "Open Hi Conga", "Low Conga", "High Timbale",
  "Low Timbale", "High Agogo", "Low Agogo", "Cabasa", "Maracas",
  "Short Whistle", "Long Whistle", "Short Guiro", "Long Guiro", "Claves",
  "Hi Wood Block", "Low Wood Block", "Mute Cuica", "Open Cuica",
  "Mute Triangle", "Open Triangle"
};

static inline void scva_names_free(struct scva_names *N);

/* A table in this project's own format, so a list transcribed from published
   documentation can be used instead of the vendor's files. One record per
   line, '#' starts a comment:
 *
 *   t <map> <bank> <program> <name>     a melodic tone
 *   d <map> <bank> <program> <name>     a drum kit
 *   k <map> <program> <key> <name>      one key inside a kit
 *
 * Numbers are as sent on the wire: map is the CC32 value, bank the CC0 value,
 * program and key 0..127. The name runs to the end of the line.
 */
static inline size_t scva_names_table(struct scva_names *N, const char *path)
{
  size_t len = 0;
  char *buf = scva__slurp(path, &len);
  char *line, *end, *next;
  size_t cap_patch = 0, cap_key = 0;

  if (!buf) return 0;
  for (line = buf; line < buf + len; line = next) {
    int a, b, c, used = 0;
    char kind;
    end = strchr(line, '\n');
    if (!end) end = buf + len;
    next = end + 1;
    while (end > line && (end[-1] == '\r' || end[-1] == '\n')) --end;
    *end = '\0';
    if (line == end || *line == '#') continue;
    if (sscanf(line, " %c %d %d %d %n", &kind, &a, &b, &c, &used) < 4 || !used)
      continue;
    if (a < 0 || a >= SCVA_MAPS || b < 0 || b > 127 || c < 0 || c > 127)
      continue;
    if (kind == 'k') {
      if (N->nkey == cap_key) {
        size_t nc = cap_key ? cap_key * 2 : 256;
        struct scva_key *p = (struct scva_key *)
          realloc(N->key, nc * sizeof *N->key);
        if (!p) break;
        N->key = p;
        cap_key = nc;
      }
      N->key[N->nkey].map = (unsigned char)a;
      N->key[N->nkey].prog = (unsigned char)b;
      N->key[N->nkey].key = (unsigned char)c;
      scva__cell(line + used, strlen(line + used), N->key[N->nkey].name);
      ++N->nkey;
    } else if (kind == 't' || kind == 'd') {
      if (N->npatch == cap_patch) {
        size_t nc = cap_patch ? cap_patch * 2 : 256;
        struct scva_patch *p = (struct scva_patch *)
          realloc(N->patch, nc * sizeof *N->patch);
        if (!p) break;
        N->patch = p;
        cap_patch = nc;
      }
      N->patch[N->npatch].map = (unsigned char)a;
      N->patch[N->npatch].bank = (unsigned char)b;
      N->patch[N->npatch].prog = (unsigned char)c;
      N->patch[N->npatch].drum = (unsigned char)(kind == 'd');
      scva__cell(line + used, strlen(line + used), N->patch[N->npatch].name);
      ++N->npatch;
    }
  }
  free(buf);
  return N->npatch;
}

/* Synthesises the GM set when no tone files are installed. */
static inline size_t scva_names_gm(struct scva_names *N)
{
  int i;
  memset(N, 0, sizeof *N);
  snprintf(N->module, sizeof N->module, "General MIDI");
  N->patch = (struct scva_patch *)calloc(129, sizeof *N->patch);
  N->key = (struct scva_key *)calloc(
    SCVA_GM_DRUM_HI - SCVA_GM_DRUM_LO + 1, sizeof *N->key);
  if (!N->patch || !N->key) { scva_names_free(N); return 0; }
  for (i = 0; i < 128; ++i) {
    N->patch[i].prog = (unsigned char)i;
    snprintf(N->patch[i].name, SCVA_NAME_MAX, "%s", scva_gm_tone[i]);
  }
  N->patch[128].drum = 1;
  snprintf(N->patch[128].name, SCVA_NAME_MAX, "Standard Kit");
  N->npatch = 129;
  for (i = SCVA_GM_DRUM_LO; i <= SCVA_GM_DRUM_HI; ++i) {
    struct scva_key *k = &N->key[N->nkey++];
    k->key = (unsigned char)i;
    snprintf(k->name, SCVA_NAME_MAX, "%s",
             scva_gm_drum[i - SCVA_GM_DRUM_LO]);
  }
  return N->npatch;
}

/* Looks for the SC-8820 set first, then the GM subset. Returns the number of
   patches found, so zero means "carry on without names". */
static inline size_t scva_names_load(struct scva_names *N, const char *dir)
{
  static const char *const sets[2][3] = {
    { "SCVSC.tnf", "SCVSC.drf", "SCVSC.drk" },
    { "GM.tnf",    "GM.drf",    "GM.drk"    }
  };
  size_t cap_patch = 0, cap_key = 0;
  size_t i;

  memset(N, 0, sizeof *N);

  /* A table of this project's own takes precedence over the vendor's files:
     $SCVA_TONES names one, otherwise it sits beside the core. */
  {
    const char *own = getenv("SCVA_TONES");
    char path[2048];
    if (own && *own && scva_names_table(N, own)) {
      snprintf(N->module, sizeof N->module, "custom");
      return N->npatch;
    }
    if (dir && *dir) {
      snprintf(path, sizeof path, "%s/scva-tones.txt", dir);
      if (scva_names_table(N, path)) {
        snprintf(N->module, sizeof N->module, "custom");
        return N->npatch;
      }
    }
    scva_names_free(N);
  }

  if (!dir || !*dir) return scva_names_gm(N);
  for (i = 0; i < 2; ++i) {
    char path[2048];
    int k;
    for (k = 0; k < 3; ++k) {
      snprintf(path, sizeof path, "%s/%s", dir, sets[i][k]);
      scva__parse(N, path, k, &cap_patch, &cap_key);
    }
    if (N->npatch) break;
  }
  /* No install to read: fall back to the GM set, which needs no files. */
  if (!N->npatch) return scva_names_gm(N);
  return N->npatch;
}

static inline void scva_names_free(struct scva_names *N)
{
  free(N->patch);
  free(N->key);
  memset(N, 0, sizeof *N);
}

static inline const char *scva_names_find(const struct scva_names *N, int map,
                                   int bank, int prog, int drum)
{
  size_t i;
  for (i = 0; i < N->npatch; ++i)
    if (N->patch[i].map == map && N->patch[i].bank == bank &&
        N->patch[i].prog == prog && N->patch[i].drum == (drum != 0))
      return N->patch[i].name;
  return NULL;
}

#endif
