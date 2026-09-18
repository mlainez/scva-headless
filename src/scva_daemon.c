/* SPDX-License-Identifier: CC0-1.0
 *
 * An ordinary ALSA sequencer MIDI device backed by SOUND Canvas VA.
 *
 *   aconnect -l              # a client called SCVA appears
 *   aplaymidi -p SCVA x.mid
 *
 * The core is loaded into this process by the PE loader, so there is no wine
 * and no second process. MIDI arrives on the sequencer port and goes straight
 * into the engine; the engine's stereo goes to an ALSA PCM device.
 *
 * One poll waits on the sequencer and the card together, so a note reaches the
 * engine the moment it arrives rather than whenever the next block happens to
 * end. The card still sets the pace - a block is rendered only when there is
 * somewhere to put it - so there is deliberately no timer in this program.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <sched.h>
#include <sys/mman.h>
#include "pe_loader.h"
#include "core_path.h"
#include "scva_map.h"
#include "scva_pcm.h"

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

/* $XDG_RUNTIME_DIR is where a user service is meant to keep its socket. */
static const char *control_path(const char *given)
{
  static char buf[256];
  const char *run;
  if (given && *given) return given;
  run = getenv("XDG_RUNTIME_DIR");
  if (run && *run)
    snprintf(buf, sizeof buf, "%s/scva-daemon.sock", run);
  else
    snprintf(buf, sizeof buf, "/tmp/scva-daemon-%u.sock",
             (unsigned)getuid());
  return buf;
}

static int control_listen(const char *path)
{
  struct sockaddr_un a;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
  unlink(path);                     /* a socket left by a killed daemon */
  if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 4) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* One command, one reply, one connection: enough for "set the map", and it
   keeps every client a single shell redirect. */
static int control_send(const char *path, const char *text)
{
  struct sockaddr_un a;
  char reply[512];
  ssize_t n;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return 1;
  memset(&a, 0, sizeof a);
  a.sun_family = AF_UNIX;
  snprintf(a.sun_path, sizeof a.sun_path, "%s", path);
  if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
    fprintf(stderr, "scva-daemon: no daemon listening on %s\n", path);
    close(fd);
    return 1;
  }
  dprintf(fd, "%s\n", text);
  shutdown(fd, SHUT_WR);
  while ((n = read(fd, reply, sizeof reply - 1)) > 0) {
    reply[n] = '\0';
    fputs(reply, stdout);
  }
  close(fd);
  return 0;
}

/* One typed line. Returns 1 when it asked to stop. */
static int console_command(char *line, unsigned char *map_of,
                           tg_short_midi_fn short_midi, int out)
{
  int ch, want, off = 0;
  size_t n = strlen(line);

  while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = '\0';
  if (!line[0]) {
    dprintf(out, "map:");
    for (ch = 0; ch < 16; ++ch) dprintf(out, " %d", map_of[ch]);
    dprintf(out, "\n");
    return 0;
  }
  if (!strcmp(line, "q") || !strcmp(line, "quit")) return 1;

  /* "<channel> <map>" sets one part, a bare map name sets all of them */
  if (sscanf(line, "%d %n", &ch, &off) == 1 && off > 0 && line[off] &&
      ch >= 1 && ch <= 16 && (want = scva_map_value(line + off)) >= 0) {
    map_of[ch - 1] = (unsigned char)want;
    short_midi(scva_map_cc(ch - 1, want), 0);
    dprintf(out, "channel %d -> map %d\n", ch, want);
    return 0;
  }
  if ((want = scva_map_value(line)) >= 0) {
    for (ch = 0; ch < 16; ++ch) {
      map_of[ch] = (unsigned char)want;
      short_midi(scva_map_cc(ch, want), 0);
    }
    dprintf(out, "all parts -> map %d\n", want);
    return 0;
  }
  dprintf(out, "type a map (" SCVA_MAP_USAGE "), or\n"
                  "  <channel 1-16> <map>   one part only\n"
                  "  <enter>                what each part is set to\n"
                  "  q                      stop\n");
  return 0;
}

int main(int argc, char **argv)
{
  const char *core = NULL;
  const char *pcm_name = NULL;      /* NULL: walk the fallback chain */
  const char *pcm_opened = NULL;
  const char *mapname = "default";
  const char *port_name = "SCVA";
  /* Matching the hardware avoids a resample in the sound server, and 48 kHz
     is what modern cards run. The renderers default to the same. */
  unsigned int rate = 48000;
  /* How far ahead of the speaker to run. This is the delay between a note
     arriving and being heard, so it is kept short; the engine costs under 1%
     of realtime, so the buffer is the whole latency. Raise it on a machine
     that cannot keep up - the symptom is the audio breaking up. */
  unsigned int latency_us = 20000;
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
  int port, nfds, nseq, npcm, rc = 0;
  long underruns = 0;
  int console = 1;          /* typed commands on stdin */
  const char *ctl_path = NULL;
  int ctl_fd = -1, ctl_slot = -1;

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--pcm") && i + 1 < argc) pcm_name = argv[++i];
    else if (!strcmp(argv[i], "--name") && i + 1 < argc) port_name = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--latency") && i + 1 < argc)
      latency_us = (unsigned)atoi(argv[++i]) * 1000u;
    else if (!strcmp(argv[i], "--map") && i + 1 < argc) mapname = argv[++i];
    else if (!strcmp(argv[i], "--control") && i + 1 < argc) ctl_path = argv[++i];
    else if (!strcmp(argv[i], "--send") && i + 1 < argc)
      return control_send(control_path(ctl_path), argv[++i]);
    else if (!strcmp(argv[i], "--list-pcm")) {
      struct pcm_cand cand[64];
      size_t n, k;
      snd_lib_error_set_handler(alsa_quiet);
      n = pcm_candidates(cand, sizeof cand / sizeof cand[0]);
      printf("tried in this order, then --pcm for anything else:\n");
      for (k = 0; k < n; ++k) {
        snd_pcm_t *p = try_pcm(cand[k].name, rate, latency_us);
        printf("  %-28s %s\n", cand[k].name,
               p ? "works at this rate" : "will not open");
        if (p) snd_pcm_close(p);
        free(cand[k].name);
      }
      return 0;
    }
    else {
      fprintf(stderr,
        "usage: scva-daemon [--core DLL] [--pcm DEV] [--name NAME]\n"
        "                   [--rate HZ] [--block N]\n"
        "                   [--map " SCVA_MAP_USAGE "]\n"
        "\n"
        "  --latency MS       delay before a note is heard (default 20)\n"
        "  --control PATH     control socket (default under $XDG_RUNTIME_DIR)\n"
        "  --send TEXT        send one command to a running daemon and exit\n"
        "  --list-pcm         what this machine offers, in try order\n"
        "  --pcm DEV          force one; otherwise it is detected\n");
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
  /* The core corrupts its own heap below 255 frames: 254 aborts every time,
     255 renders identically to 4096. 256 is the floor, rounded. */
  if (block < SCVA_MIN_BLOCK) {
    if (block > 0)
      fprintf(stderr, "scva-daemon: --block %d is below the engine's minimum, "
                      "using %d\n", block, SCVA_MIN_BLOCK);
    block = SCVA_MIN_BLOCK;
  }

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
  pcm = open_pcm(pcm_name, rate, latency_us, &pcm_opened, 0);
  if (!pcm) {
    fprintf(stderr, "scva-daemon: no PCM device would take float32 stereo "
                    "at %u Hz\n", rate);
    if (pcm_name)
      fprintf(stderr, "  '%s' did not open\n", pcm_name);
    else
      open_pcm(NULL, rate, latency_us, &pcm_opened, 1);  /* say what failed */
    fprintf(stderr, "  `aplay -L` lists what this machine has; name one with "
                    "--pcm\n");
    snd_seq_close(seq);
    return 1;
  }
  {
    snd_pcm_uframes_t bufsz = 0, per = 0;
    snd_pcm_get_params(pcm, &bufsz, &per);
    fprintf(stderr, "scva-daemon: audio on '%s', %u Hz, %.0f ms buffer\n",
            pcm_opened, rate, 1000.0 * (double)bufsz / rate);
  }

  ctl_path = control_path(ctl_path);
  ctl_fd = control_listen(ctl_path);
  if (ctl_fd >= 0)
    fprintf(stderr, "scva-daemon: control socket %s\n", ctl_path);
  else
    fprintf(stderr, "scva-daemon: no control socket at %s\n", ctl_path);

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);          /* a client that hangs up mid-reply */

  /* Both of these are best-effort and silent when refused. A page fault or a
     scheduler delay in the middle of a block is heard as a dropout, and an
     ordinary desktop grants neither by default. */
  mlockall(MCL_CURRENT | MCL_FUTURE);
  {
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = sched_get_priority_min(SCHED_FIFO) + 5;
    if (sched_setscheduler(0, SCHED_FIFO, &sp) == 0)
      fprintf(stderr, "scva-daemon: realtime priority %d\n",
              sp.sched_priority);
  }

  snd_midi_event_new(1024, &coder);
  snd_midi_event_no_status(coder, 1);

  nseq = snd_seq_poll_descriptors_count(seq, POLLIN);
  npcm = snd_pcm_poll_descriptors_count(pcm);
  nfds = nseq + npcm;
  pfds = calloc((size_t)nfds + 2, sizeof *pfds);   /* stdin, control */
  left = malloc((size_t)block * sizeof *left);
  right = malloc((size_t)block * sizeof *right);
  inter = malloc((size_t)block * 2 * sizeof *inter);
  if (!pfds || !left || !right || !inter) {
    fprintf(stderr, "scva-daemon: out of memory\n");
    rc = 1;
    goto done;
  }

  /* Wait on the sequencer and the card together. Blocking in the write
     instead would leave MIDI unread for most of every block, so a note
     played during one would not reach the engine until the next. */
  while (!stop_now) {
    unsigned short revents = 0;
    int ready;

    snd_seq_poll_descriptors(seq, pfds, (unsigned)nseq, POLLIN);
    snd_pcm_poll_descriptors(pcm, pfds + nseq, (unsigned)npcm);
    {
      int extra = 0;
      if (console) {
        pfds[nfds].fd = STDIN_FILENO;
        pfds[nfds].events = POLLIN;
        pfds[nfds].revents = 0;
        ++extra;
      }
      if (ctl_fd >= 0) {
        pfds[nfds + extra].fd = ctl_fd;
        pfds[nfds + extra].events = POLLIN;
        pfds[nfds + extra].revents = 0;
        ctl_slot = nfds + extra;
        ++extra;
      } else {
        ctl_slot = -1;
      }
      ready = poll(pfds, (nfds_t)(nfds + extra), 100);
    }
    if (ready < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (snd_pcm_poll_descriptors_revents(pcm, pfds + nseq, (unsigned)npcm,
                                         &revents) < 0)
      revents = POLLOUT;                  /* cannot tell: try the write */

    if (console && (pfds[nfds].revents & (POLLIN | POLLHUP))) {
      char buf[256];
      ssize_t n = read(STDIN_FILENO, buf, sizeof buf - 1);
      if (n <= 0) {
        console = 0;              /* stdin is gone: stop watching it */
      } else {
        char *p = buf, *nl;
        buf[n] = '\0';
        while ((nl = strchr(p, '\n')) != NULL) {
          *nl = '\0';
          if (console_command(p, map_of, short_midi, STDERR_FILENO))
            stop_now = 1;
          p = nl + 1;
        }
      }
    }

    if (ctl_slot >= 0 && (pfds[ctl_slot].revents & POLLIN)) {
      int c = accept(ctl_fd, NULL, NULL);
      if (c >= 0) {
        char buf[256];
        ssize_t n = read(c, buf, sizeof buf - 1);
        if (n > 0) {
          char *p = buf, *nl;
          buf[n] = '\0';
          if (!strchr(buf, '\n')) strcat(buf, "\n");
          while ((nl = strchr(p, '\n')) != NULL) {
            *nl = '\0';
            if (console_command(p, map_of, short_midi, c)) stop_now = 1;
            p = nl + 1;
          }
        }
        close(c);
      }
    }

    /* MIDI first: whatever arrived goes into the engine before the block it
       belongs to is rendered. input_pending(1) fetches from the kernel, so
       this never blocks waiting for an event that is not there. */
    while (snd_seq_event_input_pending(seq, 1) > 0) {
      snd_seq_event_t *ev;
      if (snd_seq_event_input(seq, &ev) >= 0) {
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
      }
    }
    /* then one block, but only when the card has somewhere to put it */
    if (revents & (POLLOUT | POLLERR)) {
      snd_pcm_sframes_t w;
      int k;
      process(left, right, block);
      for (k = 0; k < block; ++k) {
        inter[2 * k]     = left[k];
        inter[2 * k + 1] = right[k];
      }
      w = snd_pcm_writei(pcm, inter, (snd_pcm_uframes_t)block);
      if (w < 0) {
        ++underruns;
        if (snd_pcm_recover(pcm, (int)w, 1) < 0) break;
      }
    }
  }

  fprintf(stderr, "\nscva-daemon: stopping\n");
  if (underruns)
    fprintf(stderr, "scva-daemon: %ld dropout%s - raise --latency\n",
            underruns, underruns == 1 ? "" : "s");
done:
  free(pfds);
  free(left);
  free(right);
  free(inter);
  if (coder) snd_midi_event_free(coder);
  if (ctl_fd >= 0) { close(ctl_fd); unlink(ctl_path); }
  /* Deactivate, never terminate: TG_terminate calls exit(). */
  tg_deactivate();
  pe_unload(img);
  if (pcm) snd_pcm_close(pcm);
  if (seq) snd_seq_close(seq);
  return rc;
}
