/* SPDX-License-Identifier: CC0-1.0
 *
 * Plays a Standard MIDI File through the VSC backend (synth.h) with the
 * timing virtual_sc's vsc-play gives it, and writes the same 16-bit WAV, so
 * the two can be compared byte for byte: each event goes in at the start of
 * the engine block nearest it, a two-port song plays on two engines whose
 * 16-bit blocks are summed with saturation, and the song runs on for the
 * tail.
 *
 *   vsc_drive --core vsc.sys --tones vscNN.dat --rate HZ --midi FILE
 *             --out FILE.wav [--tail S] [--chunk N]
 *
 * --chunk renders N frames per call instead of one engine block. The engine
 * has no timestamps, so a message lands where the backend next starts a
 * block: events are sent ahead of the block they belong to, and a call is cut
 * short rather than start a block whose events are still to be sent - the
 * way any front end on this backend has to drive it.
 */
#include "midi_song.h"
#include "synth_vsc.h"

#define MAX_PORTS 2

/* An event with the frame of the block it goes in ahead of. */
struct timed { const struct event *e; long long at; };

/* vsc-play's scheduling without the rendering: events due before the middle
   of a block go in ahead of it. Returns the frames the song lasts. */
static long long schedule(const struct song *s, int rate, int block, double tail,
                          struct timed *out, size_t *nout)
{
  double fpt = song_frames_per_tick(s, rate, 500000), at = 0.0;
  uint64_t last_tick = 0;
  size_t next = 0;
  long long pos = 0;
  *nout = 0;
  for (;;) {
    while (next < s->count) {
      const struct event *e = &s->ev[next];
      double t = at + (double)(e->tick - last_tick) * fpt;
      if (t >= (double)pos + block / 2.0) break;
      at = t;
      last_tick = e->tick;
      if (e->tempo) fpt = song_frames_per_tick(s, rate, e->tempo);
      else { out[*nout].e = e; out[(*nout)++].at = pos; }
      ++next;
    }
    if (next >= s->count && pos >= at + tail * rate) return pos;
    pos += block;
  }
}

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

static int16_t to16(float f) { return (int16_t)(f * 32768.0f); }

int main(int argc, char **argv)
{
  const char *core = NULL, *tones = NULL, *midi = NULL, *out = NULL;
  double tail = 2.0;
  int rate = 22050, i, p, nports, block, chunk = 0;
  struct synth_vsc_options vo;
  struct synth_options opt;
  struct synth *eng[MAX_PORTS] = {0};
  struct song s;
  struct timed *ev;
  unsigned char *bytes, *sx;
  size_t n, nev, j = 0, sxcap;
  float *l, *r;
  int16_t *mix;
  FILE *wav;
  long long total, pos = 0;
  char err[256];
  static const struct vsc_config defaults = VSC_CONFIG_DEFAULT;

  for (i = 1; i + 1 < argc; i += 2) {
    if (!strcmp(argv[i], "--core")) core = argv[i + 1];
    else if (!strcmp(argv[i], "--tones")) tones = argv[i + 1];
    else if (!strcmp(argv[i], "--rate")) rate = atoi(argv[i + 1]);
    else if (!strcmp(argv[i], "--midi")) midi = argv[i + 1];
    else if (!strcmp(argv[i], "--out")) out = argv[i + 1];
    else if (!strcmp(argv[i], "--tail")) tail = atof(argv[i + 1]);
    else if (!strcmp(argv[i], "--chunk")) chunk = atoi(argv[i + 1]);
    else break;
  }
  if (i != argc || !core || !tones || !midi || !out || chunk < 0) {
    fprintf(stderr, "usage: vsc_drive --core vsc.sys --tones vscNN.dat --rate HZ "
                    "--midi FILE --out FILE.wav [--tail S] [--chunk N]\n");
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
  if (!chunk) chunk = block;

  ev = malloc((s.count ? s.count : 1) * sizeof *ev);
  l = malloc((size_t)chunk * sizeof *l);
  r = malloc((size_t)chunk * sizeof *r);
  mix = malloc((size_t)chunk * 2 * sizeof *mix);
  sxcap = s.sysex_max + 2;
  sx = malloc(sxcap);
  wav = fopen(out, "wb");
  if (!ev || !l || !r || !mix || !sx || !wav) { fprintf(stderr, "cannot set up %s\n", out); return 1; }
  total = schedule(&s, rate, block, tail, ev, &nev);
  header(wav, (unsigned)rate, 0, 16);

  while (pos < total) {
    /* the block anything sent now starts in */
    long long starts = (pos + block - 1) / block * block;
    long long until = pos + chunk < total ? pos + chunk : total;
    int k, frames;
    while (j < nev && ev[j].at <= starts) send(eng, nports, ev[j++].e, sx, sxcap);
    if (j < nev && ev[j].at < until) until = ev[j].at;
    frames = (int)(until - pos);
    for (p = 0; p < nports; ++p) {
      synth_render(eng[p], l, r, (uint32_t)frames);
      for (k = 0; k < frames; ++k) {
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
    fwrite(mix, 4, (size_t)frames, wav);
    pos = until;
  }
  rewind(wav);
  header(wav, (unsigned)rate, (uint32_t)total, 16);
  fclose(wav);
  for (p = 0; p < nports; ++p) synth_close(eng[p]);
  return 0;
}
