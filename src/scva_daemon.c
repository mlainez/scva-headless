/* SPDX-License-Identifier: CC0-1.0
 *
 * An ordinary ALSA sequencer MIDI device backed by SOUND Canvas VA, or by
 * another engine this build has (--engine, synth_registry.h).
 *
 *   aconnect -l              # a client called SCVA appears
 *   aplaymidi -p SCVA x.mid
 *
 * The engine is loaded into this process by the PE loader, so there is no
 * wine and no second process. MIDI arrives on the sequencer port and goes straight
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
#include "core_path.h"
#include "scva_map.h"
#include "map_policy.h"
#include "console.h"
#include "rt_priority.h"
#include "scva_pcm.h"
#include "scva_core_api.h"
#include "live_engine.h"
#include "synth_registry.h"


static volatile sig_atomic_t stop_now;
static void on_signal(int sig) { (void)sig; stop_now = 1; }

/* $XDG_RUNTIME_DIR is where a user service is meant to keep its socket. The
   default folds in --name, so two daemons started under different names
   never fight over the same socket without anyone having to remember
   --control too; asking for the same --control path on both is still a
   collision, because that is what naming one by hand instead means. */
static const char *control_path(const char *given, const char *name)
{
  static char buf[256];
  const char *run;
  char safe[128];
  size_t i;
  if (given && *given) return given;
  for (i = 0; name[i] && i + 1 < sizeof safe; ++i)
    safe[i] = (name[i] == '/') ? '_' : name[i];
  safe[i] = '\0';
  run = getenv("XDG_RUNTIME_DIR");
  if (run && *run)
    snprintf(buf, sizeof buf, "%s/scva-daemon-%s.sock", run, safe);
  else
    snprintf(buf, sizeof buf, "/tmp/scva-daemon-%s-%u.sock", safe,
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

static void print_fd(void *fd, const char *text)
{
  dprintf(*(int *)fd, "%s", text);
}

/* One typed line, replied to on out. Returns 1 when it asked to stop. */
static int console_command(char *line, struct live_engine *l, int out)
{
  struct console c;
  live_engine_console(l, &c);
  c.trim = "\r ";               /* a socket client may send CRLF */
  c.print = print_fd;
  c.ctx = &out;
  return console_line(&c, line);
}

static void usage(void)
{
  int i;
  fprintf(stderr,
    "usage: scva-daemon [--core DLL] [--pcm DEV] [--name NAME]\n"
    "                   [--rate HZ] [--block N]\n"
    "                   [--map " SCVA_MAP_USAGE "]\n"
    "\n"
    "  --latency MS       delay before a note is heard (default 20)\n"
    "  --control PATH     control socket (default under $XDG_RUNTIME_DIR,\n"
    "                     named after --name so two daemons with\n"
    "                     different names never share one)\n"
    "  --send TEXT        send one command to a running daemon and exit\n"
    "                     (needs the same --name if it was not default)\n"
    "  --list-pcm         what this machine offers, in try order\n"
    "  --pcm DEV          force one; otherwise it is detected\n");
  if (synth_backends[1]) {
    fprintf(stderr, "  --engine %s  the engine (default %s)\n",
            synth_engine_names(), synth_backends[0]->caps->name);
    for (i = 1; synth_backends[i]; ++i) {
      fprintf(stderr, "with --engine %s, --core is its engine file, a map typed at\n"
                      "the console chooses its one tone set, and:\n",
              synth_backends[i]->caps->name);
      synth_print_options(synth_backends[i], stderr);
    }
  }
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
  int mapval = 0, block = 256, i, rate_given = 0;
  /* The engine, and one map per part, changed only by the console and
     --send. */
  const struct synth_backend *engine = synth_backends[0];
  void *engine_opts = NULL;
  struct live_engine l;
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
  const char *send_text = NULL;
  int ctl_fd = -1, ctl_slot = -1;

  /* The engine first: the rest of the command line may be its options. */
  for (i = 1; i + 1 < argc; ++i)
    if (!strcmp(argv[i], "--engine")) {
      engine = synth_find(argv[i + 1]);
      if (!engine) {
        fprintf(stderr, "scva-daemon: --engine takes %s\n", synth_engine_names());
        return 2;
      }
    }
  if (engine->opts_size) {
    engine_opts = calloc(1, engine->opts_size);
    if (!engine_opts) { fprintf(stderr, "scva-daemon: out of memory\n"); return 1; }
    engine->opts_init(engine_opts);
  }

  for (i = 1; i < argc; ++i) {
    int took;
    if (!strcmp(argv[i], "--core") && i + 1 < argc) core = argv[++i];
    else if (!strcmp(argv[i], "--engine") && i + 1 < argc) ++i;
    else if (!strcmp(argv[i], "--pcm") && i + 1 < argc) pcm_name = argv[++i];
    else if (!strcmp(argv[i], "--name") && i + 1 < argc) port_name = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
      rate = (unsigned)atoi(argv[++i]);
      rate_given = 1;
    }
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--latency") && i + 1 < argc)
      latency_us = (unsigned)atoi(argv[++i]) * 1000u;
    else if (!strcmp(argv[i], "--map") && i + 1 < argc) mapname = argv[++i];
    else if (!strcmp(argv[i], "--control") && i + 1 < argc) ctl_path = argv[++i];
    /* Deferred rather than acted on here: it needs --name resolved first to
       find the right socket, and that must not depend on argument order. */
    else if (!strcmp(argv[i], "--send") && i + 1 < argc) send_text = argv[++i];
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
    else if ((took = synth_parse_option(engine, engine_opts, argc, argv, &i,
                                        err, sizeof err)) < 0) {
      fprintf(stderr, "scva-daemon: %s\n", err);
      return 2;
    }
    else if (!took) {
      usage();
      return argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))
             ? 0 : 2;
    }
  }
  if (send_text) return control_send(control_path(ctl_path, port_name), send_text);
  mapval = scva_map_value(mapname);
  if (mapval < 0) {
    fprintf(stderr, "scva-daemon: --map wants " SCVA_MAP_USAGE "\n");
    return 2;
  }
  /* The core corrupts its own heap below 255 frames: 254 aborts every time,
     255 renders identically to 4096. 256 is the floor, rounded. */
  if (block < SCVA_MIN_BLOCK) {
    if (block > 0)
      fprintf(stderr, "scva-daemon: --block %d is below the engine's minimum, "
                      "using %d\n", block, SCVA_MIN_BLOCK);
    block = SCVA_MIN_BLOCK;
  }

  /* --- the engine, in this process -------------------------------------- */
  if (engine == &synth_scva) {
    snprintf(corebuf, sizeof corebuf, "%s", scva_core_path(core));
  } else if (core) {
    snprintf(corebuf, sizeof corebuf, "%s", core);
  } else {
    fprintf(stderr, "scva-daemon: --engine %s needs --core, its engine file\n",
            engine->caps->name);
    return 2;
  }
  {
    int status;
    if (!live_engine_open(&l, engine, corebuf, rate, rate_given, block, engine_opts,
                          mapval, mapname, &status, err, sizeof err)) {
      if (status == 1 && engine == &synth_scva)
        fprintf(stderr, "scva-daemon: %s\n"
                        "  looked for '%s'; pass --core or set $SCVA_DLL_DIR\n",
                err, corebuf);
      else
        fprintf(stderr, "scva-daemon: %s\n", err);
      return status;
    }
  }
  rate = (unsigned)l.synth->rate;

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

  ctl_path = control_path(ctl_path, port_name);
  ctl_fd = control_listen(ctl_path);
  if (ctl_fd >= 0)
    fprintf(stderr, "scva-daemon: control socket %s\n", ctl_path);
  else
    fprintf(stderr, "scva-daemon: no control socket at %s\n", ctl_path);

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);          /* a client that hangs up mid-reply */

  {
    int prio = rt_priority_raise();
    if (prio) fprintf(stderr, "scva-daemon: realtime priority %d\n", prio);
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
          if (console_command(p, &l, STDERR_FILENO))
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
            if (console_command(p, &l, c)) stop_now = 1;
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
            synth_long(l.synth, mbuf, (size_t)n);
          } else {
            unsigned int msg = mbuf[0];
            if (n > 1) msg |= (unsigned int)mbuf[1] << 8;
            if (n > 2) msg |= (unsigned int)mbuf[2] << 16;
            map_policy_short(&l.maps, msg);
          }
        }
        snd_seq_free_event(ev);
      }
    }
    /* then one block, but only when the card has somewhere to put it */
    if (revents & (POLLOUT | POLLERR)) {
      snd_pcm_sframes_t w;
      int k;
      synth_render(l.synth, left, right, (uint32_t)block);
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
  live_engine_close(&l);
  free(engine_opts);
  if (pcm) snd_pcm_close(pcm);
  if (seq) snd_seq_close(seq);
  return rc;
}
