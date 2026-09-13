/* SPDX-License-Identifier: CC0-1.0
 *
 * A minimal VST2 host, just large enough to drive SOUND Canvas VA's own
 * wrapper and write a WAV.
 *
 * It exists because the plug-in asks its host for a directory - opcode 41,
 * audioMasterGetDirectory - and dereferences whatever it gets back. A host
 * that answers "unsupported" hands it a null and the plug-in crashes, which is
 * where MrsWatson stops. Answering that one callback is most of what this file
 * is for.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vst2.h"

static double g_rate = 44100.0;
static int32_t g_block = 512;
static char g_dir[MAX_PATH];

static intptr_t VSTCALLBACK_host(AEffect *fx, int32_t op, int32_t idx,
                                 intptr_t val, void *ptr, float opt)
{
  (void)fx; (void)idx; (void)val; (void)opt;
  if (op != amIdle) { printf("     [host callback op=%d]\n", op); fflush(stdout); }
  switch (op) {
    case amVersion:        return 2400;
    case amCurrentId:      return 0;
    case amIdle:           return 0;
    case amGetSampleRate:  return (intptr_t)g_rate;
    case amGetBlockSize:   return g_block;
    case amGetCurrentProcessLevel: return 2;        /* realtime audio thread */
    case amGetDirectory:   return (intptr_t)g_dir;  /* <-- the one that matters */
    case amGetVendorString:  if (ptr) strcpy((char *)ptr, "scva-linux"); return 1;
    case amGetProductString: if (ptr) strcpy((char *)ptr, "scva-vst-render"); return 1;
    case amGetVendorVersion: return 1;
    case amCanDo:          return 0;
    case amUpdateDisplay:  return 1;
    default:               return 0;
  }
}

/* ---- the smallest MIDI file reader that can drive this ------------------ */
struct ev { unsigned long tick; int len; unsigned char b[3]; unsigned char *sx; int sxlen; };

static int cmp_ev(const void *a, const void *b)
{
  unsigned long x = ((const struct ev *)a)->tick, y = ((const struct ev *)b)->tick;
  return x < y ? -1 : x > y ? 1 : 0;
}

static unsigned char *slurp(const char *p, size_t *n)
{
  FILE *f = fopen(p, "rb");
  unsigned char *b;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
  b = malloc(*n);
  if (b && fread(b, 1, *n, f) != *n) { free(b); b = NULL; }
  fclose(f);
  return b;
}

int main(int argc, char **argv)
{
  const char *plug = "scva-vst.dll", *midi = NULL, *out = "out.wav";
  double tail = 3.0;
  int i;
  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--plugin") && i + 1 < argc) plug = argv[++i];
    else if (!strcmp(argv[i], "--midi") && i + 1 < argc) midi = argv[++i];
    else if (!strcmp(argv[i], "--out") && i + 1 < argc) out = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) g_rate = atof(argv[++i]);
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) g_block = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--tail") && i + 1 < argc) tail = atof(argv[++i]);
    else { fprintf(stderr, "usage: scva_vst_render --midi F --out F [--plugin DLL]\n"); return 2; }
  }
  if (!midi) { fprintf(stderr, "no --midi\n"); return 2; }

  /* the directory the plug-in will be told about: the plug-in's own */
  {
    char full[MAX_PATH];
    DWORD n = GetFullPathNameA(plug, sizeof full, full, NULL);
    if (!n) { fprintf(stderr, "cannot resolve %s\n", plug); return 1; }
    strcpy(g_dir, full);
    n = (DWORD)strlen(g_dir);
    while (n > 0 && g_dir[n-1] != '\\' && g_dir[n-1] != '/') --n;
    g_dir[n] = 0;
  }
  printf("plugin dir reported to the plug-in: %s\n", g_dir);

  HMODULE lib = LoadLibraryA(plug);
  if (!lib) { fprintf(stderr, "cannot load %s (%lu)\n", plug, GetLastError()); return 1; }
  AEffect *(*entry)(audioMasterCallback) =
    (void *)GetProcAddress(lib, "VSTPluginMain");
  if (!entry) entry = (void *)GetProcAddress(lib, "main");
  if (!entry) { fprintf(stderr, "no VST entry point\n"); return 1; }

  AEffect *fx = entry(VSTCALLBACK_host);
  if (!fx) { fprintf(stderr, "plug-in returned no AEffect\n"); return 1; }
  printf("AEffect: magic %.4s  inputs %d  outputs %d  params %d  flags 0x%x\n",
         (char *)&fx->magic, fx->numInputs, fx->numOutputs, fx->numParams, fx->flags);
  if (!(fx->flags & effFlagsCanReplacing) || !fx->processReplacing) {
    fprintf(stderr, "plug-in cannot processReplacing\n"); return 1;
  }
#define STEP(name, op, idx, val, ptr, opt) \
  do { printf("  -> %s\n", name); fflush(stdout); \
       fx->dispatcher(fx, (op), (idx), (val), (ptr), (opt)); \
       printf("  <- %s ok\n", name); fflush(stdout); } while (0)
  STEP("effOpen",          effOpen, 0, 0, NULL, 0.0f);
  STEP("effSetSampleRate", effSetSampleRate, 0, 0, NULL, (float)g_rate);
  STEP("effSetBlockSize",  effSetBlockSize, 0, g_block, NULL, 0.0f);
  STEP("effMainsChanged",  effMainsChanged, 0, 1, NULL, 0.0f);
  STEP("effStartProcess",  effStartProcess, 0, 0, NULL, 0.0f);

  /* ---- read the MIDI ---------------------------------------------------- */
  size_t n; unsigned char *d = slurp(midi, &n);
  if (!d || n < 14 || memcmp(d, "MThd", 4)) { fprintf(stderr, "not a MIDI file\n"); return 1; }
  int ntrk = (d[10] << 8) | d[11], div = (d[12] << 8) | d[13];
  size_t pos = 8 + ((size_t)d[4]<<24 | (size_t)d[5]<<16 | (size_t)d[6]<<8 | d[7]);
  struct ev *evs = NULL; size_t ne = 0, cap = 0;
  unsigned long tempo_tick[4096]; unsigned tempo_us[4096]; size_t nt = 0;
  for (i = 0; i < ntrk && pos + 8 <= n; ++i) {
    size_t len = (size_t)d[pos+4]<<24 | (size_t)d[pos+5]<<16 | (size_t)d[pos+6]<<8 | d[pos+7];
    unsigned char *t = d + pos + 8; size_t j = 0; unsigned long tick = 0; int run = -1;
    pos += 8 + len;
    while (j < len) {
      unsigned long v = 0;
      while (j < len) { unsigned char c = t[j++]; v = (v<<7)|(c&0x7f); if (!(c&0x80)) break; }
      tick += v;
      if (j >= len) break;
      unsigned char st = t[j];
      if (st == 0xff) {
        unsigned char kind = t[j+1]; j += 2; unsigned long l = 0;
        while (j < len) { unsigned char c = t[j++]; l = (l<<7)|(c&0x7f); if (!(c&0x80)) break; }
        if (kind == 0x51 && l == 3 && nt < 4096) {
          tempo_tick[nt] = tick;
          tempo_us[nt++] = ((unsigned)t[j]<<16)|((unsigned)t[j+1]<<8)|t[j+2];
        }
        j += l;
      } else if (st == 0xf0 || st == 0xf7) {
        ++j; unsigned long l = 0;
        while (j < len) { unsigned char c = t[j++]; l = (l<<7)|(c&0x7f); if (!(c&0x80)) break; }
        if (ne == cap) { cap = cap ? cap*2 : 1024; evs = realloc(evs, cap*sizeof *evs); }
        evs[ne].tick = tick; evs[ne].len = 0; evs[ne].sxlen = (int)l + 1;
        evs[ne].sx = malloc(l + 1); evs[ne].sx[0] = 0xf0;
        memcpy(evs[ne].sx + 1, t + j, l); ++ne;
        j += l;
      } else {
        int nb;
        if (st & 0x80) { run = st; ++j; }
        nb = ((run & 0xf0) == 0xc0 || (run & 0xf0) == 0xd0) ? 1 : 2;
        if (ne == cap) { cap = cap ? cap*2 : 1024; evs = realloc(evs, cap*sizeof *evs); }
        evs[ne].tick = tick; evs[ne].len = nb + 1; evs[ne].sx = NULL; evs[ne].sxlen = 0;
        evs[ne].b[0] = (unsigned char)run;
        evs[ne].b[1] = t[j]; evs[ne].b[2] = nb > 1 ? t[j+1] : 0;
        ++ne; j += nb;
      }
    }
  }
  qsort(evs, ne, sizeof *evs, cmp_ev);
  printf("%zu MIDI events, division %d\n", ne, div);

  /* ---- render ----------------------------------------------------------- */
  FILE *w = fopen(out, "wb");
  unsigned char hdr[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
                           16,0,0,0, 3,0, 2,0, 0,0,0,0, 0,0,0,0, 8,0, 32,0,
                           'd','a','t','a',0,0,0,0};
  unsigned rate_i = (unsigned)g_rate, br = rate_i * 8u;
  memcpy(hdr+24,&rate_i,4); memcpy(hdr+28,&br,4);
  fwrite(hdr,1,44,w);

  float *bufs[2]; float *interleaved;
  bufs[0] = calloc((size_t)g_block, sizeof(float));
  bufs[1] = calloc((size_t)g_block, sizeof(float));
  interleaved = calloc((size_t)g_block * 2, sizeof(float));
  float *in[2] = { bufs[0], bufs[1] };

  double us = nt ? tempo_us[0] : 500000.0, per_tick = g_rate * us / (1e6 * div);
  size_t ei = 0, ti = 0; unsigned long last_tick = 0; double at = 0.0;
  unsigned long frame = 0, total_frames = 0;
  /* event absolute frame positions */
  double *evframe = malloc(ne * sizeof *evframe);
  for (size_t k = 0; k < ne; ++k) {
    while (ti < nt && tempo_tick[ti] <= evs[k].tick) {
      at += (double)(tempo_tick[ti] - last_tick) * per_tick;
      last_tick = tempo_tick[ti]; per_tick = g_rate * (double)tempo_us[ti] / (1e6 * div);
      ++ti;
    }
    evframe[k] = at + (double)(evs[k].tick - last_tick) * per_tick;
  }
  double end = ne ? evframe[ne-1] + tail * g_rate : tail * g_rate;

  unsigned char evbuf[65536];
  while ((double)frame < end) {
    VstEvents *ve = (VstEvents *)evbuf;
    int cnt = 0;
    unsigned char *slot = evbuf + sizeof(VstEvents) + 64 * sizeof(void *);
    ve->reserved = 0;
    while (ei < ne && evframe[ei] < (double)(frame + g_block) && cnt < 64) {
      int delta = (int)(evframe[ei] - (double)frame);
      if (delta < 0) delta = 0;
      if (evs[ei].sx) {
        VstMidiSysexEvent *se = (VstMidiSysexEvent *)slot;
        memset(se, 0, sizeof *se);
        se->type = 6; se->byteSize = sizeof *se; se->deltaFrames = delta;
        se->dumpBytes = evs[ei].sxlen; se->sysexDump = (char *)evs[ei].sx;
        ve->events[cnt++] = se; slot += sizeof *se;
      } else {
        VstMidiEvent *me = (VstMidiEvent *)slot;
        memset(me, 0, sizeof *me);
        me->type = 1; me->byteSize = sizeof *me; me->deltaFrames = delta;
        me->midiData[0] = (char)evs[ei].b[0];
        me->midiData[1] = (char)evs[ei].b[1];
        me->midiData[2] = (char)evs[ei].b[2];
        ve->events[cnt++] = me; slot += sizeof *me;
      }
      ++ei;
    }
    ve->numEvents = cnt;
    if (cnt) fx->dispatcher(fx, effProcessEvents, 0, 0, ve, 0.0f);
    memset(bufs[0], 0, (size_t)g_block * sizeof(float));
    memset(bufs[1], 0, (size_t)g_block * sizeof(float));
    fx->processReplacing(fx, in, bufs, g_block);
    for (i = 0; i < g_block; ++i) {
      interleaved[2*i]   = bufs[0][i];
      interleaved[2*i+1] = bufs[1][i];
    }
    fwrite(interleaved, sizeof(float), (size_t)g_block * 2, w);
    frame += (unsigned long)g_block; total_frames += (unsigned long)g_block;
  }
  { unsigned data = (unsigned)(total_frames * 8UL), riff = data + 36u;
    fseek(w,4,SEEK_SET); fwrite(&riff,4,1,w);
    fseek(w,40,SEEK_SET); fwrite(&data,4,1,w); }
  fclose(w);
  fx->dispatcher(fx, effStopProcess, 0, 0, NULL, 0.0f);
  fx->dispatcher(fx, effMainsChanged, 0, 0, NULL, 0.0f);
  fx->dispatcher(fx, effClose, 0, 0, NULL, 0.0f);
  printf("wrote %s, %lu frames\n", out, total_frames);
  return 0;
}
