/* SPDX-License-Identifier: CC0-1.0
 *
 * An ordinary ALSA sequencer MIDI device backed by SOUND Canvas VA.
 *
 *   aconnect -l              # a client called SCVA appears
 *   aplaymidi -p SCVA x.mid
 *
 * The core is loaded into this process by the PE loader, so there is no wine
 * and no second process. MIDI arrives on the sequencer port and goes straight
 * into the engine; the engine's stereo goes to an ALSA PCM device. The PCM
 * device sets the pace: writing to it blocks until the card has room. There is
 * deliberately no timer anywhere in this program.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include "pe_loader.h"
#include "core_path.h"
#include "scva_map.h"

typedef MSABI int  (*tg_initialize_fn)(int);
typedef MSABI void (*tg_set_sample_rate_fn)(float);
typedef MSABI void (*tg_set_max_block_fn)(int);
typedef MSABI int  (*tg_activate_fn)(float, int);
typedef MSABI void (*tg_deactivate_fn)(void);
typedef MSABI void (*tg_short_midi_fn)(unsigned int, int);
typedef MSABI void (*tg_long_midi_fn)(const unsigned char *, int);
typedef MSABI void (*tg_process_fn)(float *, float *, int);
struct tg_system_config { int a, b; };
typedef MSABI int (*tg_set_config_fn)(const struct tg_system_config *);

static volatile sig_atomic_t stop_now;
static void on_signal(int sig) { (void)sig; stop_now = 1; }

int main(int argc, char **argv)
{
  const char *core = NULL;
  const char *pcm_name = "default";
  const char *mapname = "default";
  const char *port_name = "SCVA";
  unsigned int rate = 44100;
  int mapval = 0, block = 256, i;
  /* One map per part. A program change latches CC32, so it is sent again
     before every one; a CC32 arriving on the wire replaces it for that
     channel, which is how the map changes while the daemon runs. */
  unsigned char map_of[16];

  struct pe_image *img;
  tg_set_sample_rate_fn set_rate;
  tg_activate_fn tg_activate;
  tg_deactivate_fn tg_deactivate;
  tg_short_midi_fn short_midi;
  tg_long_midi_fn long_midi;
  tg_process_fn process;
  char corebuf[4096], err[256];

  snd_seq_t *seq = NULL;
  snd_pcm_t *pcm = NULL;
  snd_midi_event_t *coder = NULL;
  struct pollfd *pfds;
  float *left, *right, *inter;
  unsigned char mbuf[1024];
  int port, nfds, rc = 0;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--pcm") && i + 1 < argc) pcm_name = argv[++i];
    else if (!strcmp(argv[i], "--name") && i + 1 < argc) port_name = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--map") && i + 1 < argc) mapname = argv[++i];
    else {
      fprintf(stderr,
        "usage: scva-daemon [--core DLL] [--pcm DEV] [--name NAME]\n"
        "                   [--rate HZ] [--block N]\n"
        "                   [--map " SCVA_MAP_USAGE "]\n"
        "\n"
        "  --pcm accepts anything `aplay -L` lists, eg. plughw:0,0\n");
      return argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))
             ? 0 : 2;
    }
  }
  mapval = scva_map_value(mapname);
  if (mapval < 0) {
    fprintf(stderr, "scva-daemon: --map wants " SCVA_MAP_USAGE "\n");
    return 2;
  }
  for (i = 0; i < 16; ++i) map_of[i] = (unsigned char)mapval;
  if (block < 1) block = 256;

  /* --- the engine, in this process -------------------------------------- */
  snprintf(corebuf, sizeof corebuf, "%s", scva_core_path(core));
  img = pe_load(corebuf, err, sizeof err);
  if (!img) {
    fprintf(stderr, "scva-daemon: %s\n"
                    "  looked for '%s'; pass --core or set $SCVA_DLL_DIR\n",
            err, corebuf);
    return 1;
  }
#define GET(v, t, n) v = (t)pe_symbol(img, n); \
  if (!v) { fprintf(stderr, "scva-daemon: the core has no %s\n", n); return 1; }
  {
    tg_initialize_fn initialize;
    tg_set_max_block_fn set_block;
    tg_set_config_fn set_config;
    GET(initialize, tg_initialize_fn, "TG_initialize")
    GET(set_block, tg_set_max_block_fn, "TG_setMaxBlockSize")
    GET(set_config, tg_set_config_fn, "TG_XPsetSystemConfig")
    GET(set_rate, tg_set_sample_rate_fn, "TG_setSampleRate")
    GET(tg_activate, tg_activate_fn, "TG_activate")
    GET(tg_deactivate, tg_deactivate_fn, "TG_deactivate")
    GET(short_midi, tg_short_midi_fn, "TG_ShortMidiIn")
    GET(long_midi, tg_long_midi_fn, "TG_LongMidiIn")
    GET(process, tg_process_fn, "TG_Process")
    /* The rate is set on both sides of the block size and again immediately
       before activate; otherwise the core emits Inf and NaN silently. */
    initialize(0);
    set_rate((float)rate);
    set_block(block);
    { struct tg_system_config c; c.a = 1; c.b = 1; set_config(&c); }
    set_rate((float)rate);
    tg_activate((float)rate, block);
  }
#undef GET
  {
    static const unsigned char gs_reset[] = {
      0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
    long_midi(gs_reset, 0);
  }

  /* --- the ALSA sequencer port ------------------------------------------ */
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
    fprintf(stderr, "scva-daemon: cannot open the sequencer\n");
    return 1;
  }
  snd_seq_set_client_name(seq, port_name);
  port = snd_seq_create_simple_port(seq, port_name,
      SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTH);
  if (port < 0) {
    fprintf(stderr, "scva-daemon: cannot create the port\n");
    snd_seq_close(seq);
    return 1;
  }
  fprintf(stderr, "scva-daemon: MIDI port '%s' is up (client %d), map %s\n",
          port_name, snd_seq_client_id(seq), mapname);

  /* --- the PCM device, which is also the clock -------------------------- */
  if (snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
    fprintf(stderr, "scva-daemon: cannot open PCM '%s'\n"
                    "  `aplay -L` lists the devices; pass one with --pcm,\n"
                    "  eg. --pcm plughw:0,0 to bypass a busy dmix\n", pcm_name);
    snd_seq_close(seq);
    return 1;
  }
  if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE,
                         SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1,
                         200000) < 0) {
    fprintf(stderr, "scva-daemon: PCM '%s' will not take float32 stereo "
                    "at %u Hz\n", pcm_name, rate);
    snd_pcm_close(pcm);
    snd_seq_close(seq);
    return 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  snd_midi_event_new(1024, &coder);
  snd_midi_event_no_status(coder, 1);

  nfds = snd_seq_poll_descriptors_count(seq, POLLIN);
  pfds = calloc((size_t)nfds, sizeof *pfds);
  left = malloc((size_t)block * sizeof *left);
  right = malloc((size_t)block * sizeof *right);
  inter = malloc((size_t)block * 2 * sizeof *inter);
  if (!pfds || !left || !right || !inter) {
    fprintf(stderr, "scva-daemon: out of memory\n");
    rc = 1;
    goto done;
  }

  while (!stop_now) {
    /* MIDI first, so a note that arrived during the last block is not late */
    snd_seq_poll_descriptors(seq, pfds, (unsigned)nfds, POLLIN);
    if (poll(pfds, (nfds_t)nfds, 0) > 0) {
      snd_seq_event_t *ev;
      while (snd_seq_event_input(seq, &ev) >= 0) {
        long n = snd_midi_event_decode(coder, mbuf, sizeof mbuf, ev);
        if (n > 0) {
          if (mbuf[0] == 0xF0) {
            long_midi(mbuf, 0);
          } else {
            unsigned char st = mbuf[0] & 0xf0, ch = mbuf[0] & 0x0f;
            unsigned int msg;
            if (st == 0xB0 && n >= 3 && mbuf[1] == 0x20) {
              map_of[ch] = mbuf[2];
              fprintf(stderr, "scva-daemon: channel %d -> map %d\n",
                      ch + 1, mbuf[2]);
            } else if (st == 0xC0) {
              short_midi(scva_map_cc(ch, map_of[ch]), 0);
            }
            msg = mbuf[0];
            if (n > 1) msg |= (unsigned int)mbuf[1] << 8;
            if (n > 2) msg |= (unsigned int)mbuf[2] << 16;
            short_midi(msg, 0);
          }
        }
        snd_seq_free_event(ev);
        if (snd_seq_event_input_pending(seq, 0) <= 0) break;
      }
    }
    /* then one block of audio, and writing it is what paces us */
    {
      snd_pcm_sframes_t w;
      int k;
      process(left, right, block);
      for (k = 0; k < block; ++k) {
        inter[2 * k]     = left[k];
        inter[2 * k + 1] = right[k];
      }
      w = snd_pcm_writei(pcm, inter, (snd_pcm_uframes_t)block);
      if (w < 0 && snd_pcm_recover(pcm, (int)w, 1) < 0) break;
    }
  }

  fprintf(stderr, "\nscva-daemon: stopping\n");
done:
  free(pfds);
  free(left);
  free(right);
  free(inter);
  if (coder) snd_midi_event_free(coder);
  /* Deactivate, never terminate: TG_terminate calls exit(). */
  tg_deactivate();
  pe_unload(img);
  if (pcm) snd_pcm_close(pcm);
  if (seq) snd_seq_close(seq);
  return rc;
}
