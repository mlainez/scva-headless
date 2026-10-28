/* SPDX-License-Identifier: CC0-1.0
 *
 * See audio_waveout.h.
 */
#include "audio_waveout.h"
#include <stdlib.h>
#include <string.h>

int waveout_find(const char *want, UINT *dev)
{
  UINT n = waveOutGetNumDevs(), i;
  char *end;
  long k;
  if (!want) { *dev = WAVE_MAPPER; return 1; }
  k = strtol(want, &end, 10);
  if (end != want && !*end) {
    *dev = (UINT)k;
    return k >= 0 && (UINT)k < n;
  }
  for (i = 0; i < n; ++i) {
    WAVEOUTCAPSA c;
    if (waveOutGetDevCapsA(i, &c, sizeof c) == MMSYSERR_NOERROR &&
        strstr(c.szPname, want)) {
      *dev = i;
      return 1;
    }
  }
  return 0;
}

int waveout_open(struct waveout *w, UINT dev, unsigned int rate, int nbuf,
                 int frames)
{
  WAVEFORMATEX wf;
  int b;

  memset(w, 0, sizeof *w);
  w->nbuf = nbuf;
  w->frames = frames;
  w->ev = CreateEventA(NULL, FALSE, FALSE, NULL);
  memset(&wf, 0, sizeof wf);
  wf.wFormatTag = WAVE_FORMAT_PCM;
  wf.nChannels = 2;
  wf.nSamplesPerSec = rate;
  wf.wBitsPerSample = 16;
  wf.nBlockAlign = (WORD)(wf.nChannels * wf.wBitsPerSample / 8);
  wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
  if (waveOutOpen(&w->h, dev, &wf, (DWORD_PTR)w->ev, 0,
                  CALLBACK_EVENT) != MMSYSERR_NOERROR)
    return WAVEOUT_NO_DEVICE;
  w->hdr = calloc((size_t)nbuf, sizeof *w->hdr);
  w->pcm = calloc((size_t)nbuf, sizeof *w->pcm);
  if (!w->hdr || !w->pcm) return WAVEOUT_NO_MEMORY;
  for (b = 0; b < nbuf; ++b) {
    w->pcm[b] = calloc((size_t)frames * 2, sizeof **w->pcm);
    if (!w->pcm[b]) return WAVEOUT_NO_MEMORY;
    w->hdr[b].lpData = (LPSTR)w->pcm[b];
    w->hdr[b].dwBufferLength = (DWORD)(frames * 2 * sizeof **w->pcm);
    waveOutPrepareHeader(w->h, &w->hdr[b], sizeof w->hdr[b]);
    w->hdr[b].dwFlags |= WHDR_DONE;               /* all free to begin */
  }
  return 0;
}

void waveout_write(struct waveout *w, int b)
{
  w->hdr[b].dwFlags &= ~WHDR_DONE;
  waveOutWrite(w->h, &w->hdr[b], sizeof w->hdr[b]);
}

void waveout_wait(const struct waveout *w, DWORD ms)
{
  WaitForSingleObject(w->ev, ms);
}

void waveout_drain_close(struct waveout *w)
{
  int b;
  for (b = 0; b < w->nbuf; ++b)
    while (!waveout_is_free(w, b)) waveout_wait(w, 100);
  waveOutReset(w->h);
  waveOutClose(w->h);
}
