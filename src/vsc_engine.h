/* SPDX-License-Identifier: CC0-1.0
 *
 * Roland Virtual Sound Canvas 3.2's synthesizer, run in this process.
 *
 * The engine ships as vsc.sys, the Windows NT kernel driver from the VSC 3.2
 * CD. It is a 32-bit PE image whose COFF symbol table was left in, so its
 * internal functions are called by name and the driver half of it - device
 * objects, IRPs, the ioctl switch - is never run. The tone sets are the
 * vscNN.dat files beside it; the driver descrambles them itself.
 *
 * This is the engine as it is: fixed rates, fixed blocks of 16-bit stereo,
 * MIDI that lands on block boundaries. synth_vsc.c adapts it to synth.h.
 */
#ifndef VSC_ENGINE_H
#define VSC_ENGINE_H
#include <stddef.h>
#include <stdint.h>

#if !defined(__i386__)
#error "vsc.sys is 32-bit x86 code: build with -m32"
#endif

enum vsc_cpu { VSC_CPU_AUTO, VSC_CPU_MMX, VSC_CPU_FPU };

/* The settings VSC keeps under HKCU\Software\Roland\VSC Driver. */
struct vsc_config {
  int rate;            /* SamplingRate: 11025, 22050 or 44100 */
  int polyphony;       /* MaxVoices: 1-128 */
  int volume;          /* Volume: 0-100 */
  int cpu_load_limit;  /* CpuLoadLimit: 0-90, percent */
  enum vsc_cpu cpu;    /* not a VSC setting: which code path to allow */
  /* Return gains of the three effects. VSCAPI hard-codes 1.26 for all. */
  float rev_gain, cho_gain, delay_gain;
};

/* VSC 3.2's own defaults, what VSCAPI falls back to with no registry. */
#define VSC_CONFIG_DEFAULT { .rate = 22050, .polyphony = 64, .volume = 80, \
  .cpu_load_limit = 70, .cpu = VSC_CPU_AUTO, \
  .rev_gain = 1.26f, .cho_gain = 1.26f, .delay_gain = 1.26f }

struct vsc;

/* Map vsc.sys, then load one tone set (vsc55.dat, vsc88.dat or vsc88pro.dat)
   and start the synth. NULL on failure, with the reason in err. */
struct vsc *vsc_open(const char *sys_path, const char *dat_path,
                     const struct vsc_config *cfg, char *err, size_t errlen);
/* Frames vsc_render produces per call: 125, 250 or 500. The engine takes no
   timestamps, so MIDI lands on these boundaries. */
int  vsc_block_frames(const struct vsc *v);
int  vsc_rate(const struct vsc *v);
/* The code path the engine's CPU check chose: 0 x87, 1 MMX, 2 SSE. */
int  vsc_cpu_kind(const struct vsc *v);
void vsc_short(struct vsc *v, uint32_t msg);       /* status in the low byte */
/* A complete SysEx, F0 through F7. Returns 0 if it is longer than the 256
   bytes the driver's interface carries. */
int  vsc_sysex(struct vsc *v, const unsigned char *msg, size_t len);
/* Back to power-on state: every part, effect and controller. */
void vsc_reset(struct vsc *v);
/* One block of interleaved 16-bit stereo into out. */
void vsc_render(struct vsc *v, int16_t *out);
void vsc_close(struct vsc *v);
#endif
