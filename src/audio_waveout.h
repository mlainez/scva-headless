/* SPDX-License-Identifier: CC0-1.0
 *
 * A waveOut device with a ring of 16-bit stereo buffers, which is what every
 * Windows sound device accepts. The caller fills a free buffer and writes
 * it; the device signals an event as each one comes back.
 */
#ifndef AUDIO_WAVEOUT_H
#define AUDIO_WAVEOUT_H
#include <windows.h>

struct waveout {
  HWAVEOUT h;
  HANDLE ev;                    /* signalled as buffers come back */
  WAVEHDR *hdr;
  short **pcm;                  /* frames * 2 samples each, interleaved */
  int nbuf, frames;
};

#define WAVEOUT_NO_DEVICE (-1)
#define WAVEOUT_NO_MEMORY (-2)

/* Opens the default device at rate with nbuf buffers of frames each, all
   free. 0, or one of the errors above. */
int waveout_open(struct waveout *w, unsigned int rate, int nbuf, int frames);

static inline int waveout_is_free(const struct waveout *w, int b)
{
  return (w->hdr[b].dwFlags & WHDR_DONE) != 0;
}

/* Queues buffer b, filled, to be played. */
void waveout_write(struct waveout *w, int b);

/* Waits up to ms for a buffer to come back. */
void waveout_wait(const struct waveout *w, DWORD ms);

/* Lets every queued buffer play out, then closes the device. */
void waveout_drain_close(struct waveout *w);
#endif
