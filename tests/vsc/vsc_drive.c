/* SPDX-License-Identifier: CC0-1.0
 *
 * Plays a Standard MIDI File through the VSC backend (synth.h) the way
 * virtual_sc's vsc-play does, and writes the same 16-bit WAV, so the two can
 * be compared byte for byte: events due before the middle of a block go in
 * ahead of it, a two-port song plays on two engines whose 16-bit blocks are
 * summed with saturation, and the song runs on for the tail.
 *
 *   vsc_drive --core vsc.sys --tones vscNN.dat --rate HZ --midi FILE
 *             --out FILE.wav [--tail S]
 */
#include "midi_song.h"
#include "synth_vsc.h"

#define MAX_PORTS 2

static int16_t to16(float f) { return (int16_t)(f * 32768.0f); }

static void send(struct synth **eng, int nports, const struct event *e,
                 unsigned char *sx, size_t sxcap)
{
  if (e->port >= nports) return;
  if (e->status == 0xf0 || e->status == 0xf7) {
    int n = sysex_message(e, sx, sxcap);
    if (n > 0) synth_long(eng[e->port], sx, (size_t)n);
  } else if (e->status < 0xf0) {
    synth_short(eng[e->port],
                e->status | (uint32_t)e->data1 << 8 | (uint32_t)e->data2 << 16);
  }
}

int main(int argc, char **argv)
{
  const char *core = NULL, *tones = NULL, *midi = NULL, *out = NULL;
  double tail = 2.0;
  int rate = 22050, i, p, nports, block;
  struct synth_vsc_options vo;
  struct synth_options opt;
  struct synth *eng[MAX_PORTS] = {0};
  struct song s;
  unsigned char *bytes, *sx;
  size_t n, next = 0, sxcap;
  float *l, *r;
  int16_t *mix;
  FILE *wav;
  double fpt, at = 0.0;
  uint64_t last_tick = 0;
  long long pos = 0;
  char err[256];
  static const struct vsc_config defaults = VSC_CONFIG_DEFAULT;

  for (i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--core")) core = argv[i + 1];
    else if (!strcmp(argv[i], "--tones")) tones = argv[i + 1];
    else if (!strcmp(argv[i], "--rate")) rate = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--midi")) midi = argv[i + 1];
    else if (!strcmp(argv[i], "--out")) out = argv[i + 1];
    else if (!strcmp(argv[i], "--tail")) tail = atof(argv[i + 1]);
    else break;
  }
  if (i != argc || !core || !tones || !midi || !out) {
    fprintf(stderr, "usage: vsc_drive --core vsc.sys --tones vscNN.dat --rate HZ "
                    "--midi FILE --out FILE.wav [--tail S]\n");
    return 2;
  }
  bytes = slurp(midi, &n);
  if (!bytes || !parse(&s, bytes, n)) { fprintf(stderr, "%s: not a MIDI file\n", midi); return 1; }
  nports = song_ports(&s);
  if (nports > MAX_PORTS) nports = MAX_PORTS;

  vo.tones = tones;
  vo.config = defaults;
  opt.core = core;
  opt.rate = rate;
  opt.max_block = 0;
  opt.backend_opts = &vo;
  for (p = 0; p < nports; ++p) {
    eng[p] = synth_vsc.open(&opt, err, sizeof err);
    if (!eng[p]) { fprintf(stderr, "%s\n", err); return 1; }
  }
  rate = (int)eng[0]->rate;
  block = eng[0]->midi_block;

  l = malloc((size_t)block * sizeof *l);
  r = malloc((size_t)block * sizeof *r);
  mix = malloc((size_t)block * 2 * sizeof *mix);
  sxcap = s.sysex_max + 2;
  sx = malloc(sxcap);
  wav = fopen(out, "wb");
  if (!l || !r || !mix || !sx || !wav) { fprintf(stderr, "cannot set up %s\n", out); return 1; }
  header(wav, (unsigned)rate, 0, 16);

  fpt = song_frames_per_tick(&s, rate, 500000);
  for (;;) {
    int k;
    while (next < s.count) {
      const struct event *e = &s.ev[next];
      double t = at + (double)(e->tick - last_tick) * fpt;
      if (t >= (double)pos + block / 2.0) break;
      at = t;
      last_tick = e->tick;
      if (e->tempo) fpt = song_frames_per_tick(&s, rate, e->tempo);
      else send(eng, nports, e, sx, sxcap);
      ++next;
    }
    if (next >= s.count && pos >= at + tail * rate) break;

    for (p = 0; p < nports; ++p) {
      synth_render(eng[p], l, r, (uint32_t)block);
      for (k = 0; k < block; ++k) {
        int sl = to16(l[k]), sr = to16(r[k]);
        if (p) {
          sl += mix[2 * k];
          sr += mix[2 * k + 1];
          sl = sl > 32767 ? 32767 : sl < -32768 ? -32768 : sl;
          sr = sr > 32767 ? 32767 : sr < -32768 ? -32768 : sr;
        }
        mix[2 * k] = (int16_t)sl;
        mix[2 * k + 1] = (int16_t)sr;
      }
    }
    fwrite(mix, 4, (size_t)block, wav);
    pos += block;
  }
  rewind(wav);
  header(wav, (unsigned)rate, (uint32_t)pos, 16);
  fclose(wav);
  for (p = 0; p < nports; ++p) synth_close(eng[p]);
  return 0;
}
