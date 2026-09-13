/* SPDX-License-Identifier: CC0-1.0
 *
 * The Linux half: an ordinary ALSA sequencer MIDI device backed by SOUND
 * Canvas VA running under wine.
 *
 *   aconnect -l              # a client called SCVA appears
 *   aplaymidi -p SCVA x.mid
 *
 * MIDI arrives on the sequencer port, is framed down a pipe to the engine, and
 * the engine's float32 stereo comes back up another pipe and goes to an ALSA
 * PCM device.  The PCM device is what sets the pace: writing to it blocks until
 * the card has room, which throttles reads from the engine, which throttles the
 * engine itself.  There is deliberately no timer anywhere in this program.
 */
#define _GNU_SOURCE
#include <alsa/asoundlib.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <poll.h>
#include "scva_map.h"

static volatile sig_atomic_t stop_now;
static void on_signal(int sig) { (void)sig; stop_now = 1; }

static int frame_short(int fd, const unsigned char *b, int n)
{
  unsigned char f[4] = { 0x01, 0, 0, 0 };
  int i;
  for (i = 0; i < n && i < 3; ++i) f[1 + i] = b[i];
  return write(fd, f, 4) == 4;
}

static int frame_sysex(int fd, const unsigned char *b, unsigned int n)
{
  unsigned char h[5];
  h[0] = 0x02;
  h[1] = (unsigned char)(n      ); h[2] = (unsigned char)(n >>  8);
  h[3] = (unsigned char)(n >> 16); h[4] = (unsigned char)(n >> 24);
  if (write(fd, h, 5) != 5) return 0;
  return write(fd, b, n) == (ssize_t)n;
}

int main(int argc, char **argv)
{
  const char *dll_dir = getenv("SCVA_DLL_DIR");
  const char *engine  = getenv("SCVA_ENGINE");
  const char *pcm_name = "default";
  const char *mapname = "default";
  int mapval = 0;
  /* One map per part. A program change latches CC32, so it is sent again
     before every one; a CC32 arriving on the wire replaces it for that
     channel, which is how the map changes while the daemon runs. */
  unsigned char map_of[16];
  const char *port_name = "SCVA";
  unsigned int rate = 44100;
  int block = 256, i;
  char dll_path[4096];

  for (i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--dll-dir") && i + 1 < argc) dll_dir = argv[++i];
    else if (!strcmp(argv[i], "--engine") && i + 1 < argc) engine = argv[++i];
    else if (!strcmp(argv[i], "--pcm") && i + 1 < argc) pcm_name = argv[++i];
    else if (!strcmp(argv[i], "--name") && i + 1 < argc) port_name = argv[++i];
    else if (!strcmp(argv[i], "--rate") && i + 1 < argc) rate = (unsigned)atoi(argv[++i]);
    else if (!strcmp(argv[i], "--block") && i + 1 < argc) block = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--map") && i + 1 < argc) mapname = argv[++i];
    else {
      fprintf(stderr,
        "usage: scva-daemon [--dll-dir DIR] [--engine EXE] [--pcm DEV]\n"
        "                   [--name NAME] [--rate HZ] [--block N]\n"
        "                   [--map " SCVA_MAP_USAGE "]\n");
      return 2;
    }
  }
  mapval = scva_map_value(mapname);
  if (mapval < 0) {
    fprintf(stderr, "scva-daemon: --map wants " SCVA_MAP_USAGE "\n");
    return 2;
  }
  memset(map_of, (unsigned char)mapval, sizeof map_of);
  if (!dll_dir) dll_dir = "dll";
  if (!engine)  engine  = "build/scva_engine.exe";
  snprintf(dll_path, sizeof dll_path, "%s/SCCore.dll", dll_dir);
  /* Both paths must be absolute before the child chdirs into the DLL
     directory, or wine looks for the engine relative to the wrong place and
     reports c0000135, which reads like a missing DLL and is not one. */
  {
    static char engine_abs[4096], dll_abs[4096];
    if (!realpath(engine, engine_abs)) {
      fprintf(stderr, "scva-daemon: no engine at %s\n", engine);
      return 1;
    }
    if (!realpath(dll_dir, dll_abs)) {
      fprintf(stderr, "scva-daemon: no such directory %s\n", dll_dir);
      return 1;
    }
    engine = engine_abs;
    dll_dir = dll_abs;
  }
  if (access(dll_path, R_OK) != 0) {
    fprintf(stderr, "scva-daemon: no SCCore.dll in %s\n"
                    "  supply your own copy - see README\n", dll_dir);
    return 1;
  }

  /* --- the engine, under wine, with pipes both ways --------------------- */
  int to_engine[2], from_engine[2];
  if (pipe(to_engine) || pipe(from_engine)) { perror("pipe"); return 1; }
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }
  if (pid == 0) {
    char rbuf[32], bbuf[32];
    snprintf(rbuf, sizeof rbuf, "%u", rate);
    snprintf(bbuf, sizeof bbuf, "%d", block);
    dup2(to_engine[0], STDIN_FILENO);
    dup2(from_engine[1], STDOUT_FILENO);
    close(to_engine[1]); close(from_engine[0]);
    /* wine resolves the DLL out of the working directory */
    if (chdir(dll_dir) != 0) { perror("chdir"); _exit(127); }
    execlp("wine", "wine", engine, "--core", "SCCore.dll",
           "--rate", rbuf, "--block", bbuf, (char *)NULL);
    perror("exec wine");
    _exit(127);
  }
  close(to_engine[0]); close(from_engine[1]);

  /* --- the ALSA sequencer port ------------------------------------------ */
  snd_seq_t *seq = NULL;
  if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0) < 0) {
    fprintf(stderr, "scva-daemon: cannot open the sequencer\n"); return 1;
  }
  snd_seq_set_client_name(seq, port_name);
  int port = snd_seq_create_simple_port(seq, port_name,
      SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTH);
  if (port < 0) { fprintf(stderr, "scva-daemon: cannot create the port\n"); return 1; }
  fprintf(stderr, "scva-daemon: MIDI port '%s' is up (client %d)\n",
          port_name, snd_seq_client_id(seq));

  /* --- the PCM device, which is also the clock -------------------------- */
  snd_pcm_t *pcm = NULL;
  if (snd_pcm_open(&pcm, pcm_name, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
    fprintf(stderr, "scva-daemon: cannot open PCM '%s'\n", pcm_name); return 1;
  }
  if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_FLOAT_LE,
                         SND_PCM_ACCESS_RW_INTERLEAVED, 2, rate, 1,
                         200000) < 0) {
    fprintf(stderr, "scva-daemon: PCM will not take float32 stereo at %u Hz\n", rate);
    return 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  snd_midi_event_t *coder = NULL;
  snd_midi_event_new(1024, &coder);
  snd_midi_event_no_status(coder, 1);

  int nfds = snd_seq_poll_descriptors_count(seq, POLLIN);
  struct pollfd *pfds = calloc((size_t)nfds, sizeof *pfds);
  float *audio = malloc((size_t)block * 2 * sizeof *audio);
  unsigned char mbuf[1024];

  while (!stop_now) {
    /* MIDI first, so a note that arrived during the last block is not late */
    snd_seq_poll_descriptors(seq, pfds, (unsigned)nfds, POLLIN);
    if (poll(pfds, (nfds_t)nfds, 0) > 0) {
      snd_seq_event_t *ev;
      while (snd_seq_event_input(seq, &ev) >= 0) {
        long n = snd_midi_event_decode(coder, mbuf, sizeof mbuf, ev);
        if (n > 0) {
          if (mbuf[0] == 0xF0) {
            frame_sysex(to_engine[1], mbuf, (unsigned)n);
          } else {
            unsigned char st = mbuf[0] & 0xf0, ch = mbuf[0] & 0x0f;
            if (st == 0xB0 && n >= 3 && mbuf[1] == 0x20) {
              map_of[ch] = mbuf[2];
              fprintf(stderr, "scva-daemon: channel %d -> map %d\n",
                      ch + 1, mbuf[2]);
            } else if (st == 0xC0) {
              unsigned char cc[3];
              cc[0] = (unsigned char)(0xB0 | ch);
              cc[1] = 0x20;
              cc[2] = map_of[ch];
              frame_short(to_engine[1], cc, 3);
            }
            frame_short(to_engine[1], mbuf, (int)n);
          }
        }
        snd_seq_free_event(ev);
        if (snd_seq_event_input_pending(seq, 0) <= 0) break;
      }
    }
    /* then one block of audio, and writing it is what paces us */
    size_t want = (size_t)block * 2 * sizeof(float), got = 0;
    while (got < want) {
      ssize_t r = read(from_engine[0], (char *)audio + got, want - got);
      if (r <= 0) { stop_now = 1; break; }
      got += (size_t)r;
    }
    if (stop_now) break;
    snd_pcm_sframes_t w = snd_pcm_writei(pcm, audio, (snd_pcm_uframes_t)block);
    if (w < 0) {
      if (snd_pcm_recover(pcm, (int)w, 1) < 0) break;
    }
  }

  fprintf(stderr, "\nscva-daemon: stopping\n");
  close(to_engine[1]);
  kill(pid, SIGTERM);
  waitpid(pid, NULL, 0);
  snd_pcm_close(pcm);
  snd_seq_close(seq);
  return 0;
}
