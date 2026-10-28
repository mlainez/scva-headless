/* SPDX-License-Identifier: CC0-1.0
 *
 * Maps vsc.sys and drives the synthesizer inside it. See vsc_engine.h.
 *
 * Only the engine is used. DriverEntry is not run: it would create a device
 * object and route ioctls to VscDispatch, which does nothing but unpack an
 * IRP's buffer and call the same functions this file calls directly:
 *
 *   ioctl 0x9C402404  tgInit(flag, rate)
 *   ioctl 0x9C40241C  ConfigWaveBuf(dat, size)
 *   ioctl 0x9C402400  InitSynth()
 *   ioctl 0x9C402410  RxMidiMsg(&msg)
 *   ioctl 0x9C402414  RxMidiLongMsg(buf256)
 *   ioctl 0x9C402418  FillAudioBuffer(gain, out)
 *   ioctl 0x9C402420  CloseWaveBuf()
 *
 * The engine's code reaches four of the driver's nineteen imports: the pool
 * allocator for the tone set, and the performance counter for its CPU load
 * limiter. The rest belong to the IRP and device plumbing and are bound to
 * stubs of the right shape that are never called.
 */
#include "vsc_engine.h"
#include "pe_loader.h"
#include "win9x.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STDCALL  __attribute__((stdcall))
#define FASTCALL __attribute__((fastcall))

typedef STDCALL int  (*fn_tgInit)(int flag, int rate);
typedef STDCALL int  (*fn_ConfigWaveBuf)(const void *dat, int size);
typedef STDCALL void (*fn_void)(void);
typedef STDCALL int  (*fn_InitSynth)(void);
typedef STDCALL void (*fn_RxMidiMsg)(const uint32_t *msg);
typedef STDCALL void (*fn_RxMidiLongMsg)(const unsigned char *msg);
typedef STDCALL void (*fn_FillAudioBuffer)(float gain, int16_t *out);
typedef STDCALL void (*fn_SetSynthConfData)(const void *conf);

/* SynthConfData, the 0x74-byte block CSynth::SetVscConfData passes to ioctl
   0x9C40240C. Each field is applied only if its bit is set in mask; the bit
   is beside it. The values VSCAPI sends are in apply_conf. */
struct synth_conf {
  uint32_t mask;
  float pitch_offset;         /* 0x00000001  cents, to keep pitch across rates */
  float cpu_power;            /* 0x00000004  load limit, fraction of the CPU */
  uint32_t power_limit;       /* 0x00000008  load limiter on */
  uint32_t voices;            /* 0x00000010  voices allowed right now */
  uint32_t voice_limit;       /* 0x00000020  maximum polyphony */
  uint32_t use_chorus;        /* 0x00000040 */
  uint32_t use_reverb;        /* 0x00000080 */
  float amp_adj;              /* 0x00000100  gain on every voice */
  float rev_adj;              /* 0x00000200  gain on reverb */
  float cho_adj;              /* 0x00000400  gain on chorus */
  float master_volume;        /* 0x00000800 */
  uint32_t mmx_enable;        /* 0x00001000 */
  uint32_t use_tvf;           /* 0x00002000 */
  uint32_t use_delay;         /* 0x00004000 */
  float delay_adj;            /* 0x00008000  gain on delay */
  float tvf_lfo_adj;          /* 0x00010000  depth of the filter LFO */
  uint32_t unknown44;         /* 0x00020000 */
  uint32_t unknown48;
  uint32_t efx_limit;         /* 0x00080000 */
  /* Per-part receive switches, one bit per part. */
  uint32_t rcv_pc_bank;       /* 0x00100000 */
  uint32_t rcv_volume;        /* 0x00200000 */
  uint32_t rcv_panpot;        /* 0x00400000 */
  uint32_t rcv_expression;    /* 0x00800000 */
  uint32_t rcv_reverb_send;   /* 0x01000000 */
  uint32_t rcv_chorus_send;   /* 0x02000000 */
  uint32_t rcv_delay_send;    /* 0x04000000 */
  uint32_t rcv_reset;         /* 0x08000000 */
  uint32_t rcv_cancel_on_reset; /* 0x10000000 */
};
_Static_assert(sizeof(struct synth_conf) == 0x74, "SynthConfData is 0x74 bytes");

struct vsc {
  struct pe_image *img;
  fn_tgInit tgInit;
  fn_ConfigWaveBuf ConfigWaveBuf;
  fn_void CloseWaveBuf;
  fn_InitSynth InitSynth;
  fn_RxMidiMsg RxMidiMsg;
  fn_RxMidiLongMsg RxMidiLongMsg;
  fn_FillAudioBuffer FillAudioBuffer;
  fn_SetSynthConfData SetSynthConfData;
  struct vsc_config cfg;
  volatile int32_t *serviceInterval;
  volatile int32_t *samplingRate;
  volatile int32_t *workCpu;          /* GetCpuKind's verdict, set by tgInit */
  /* waveInfo+0x14. FillAudioBuffer converts each block into the caller's
     buffer and then offers it to the driver's queue of pending write IRPs,
     unless this is nonzero. There is no queue here, so it stays set. */
  volatile int32_t *skipWriteQueue;
};

/* ------------------------------------------------------------ x87 state */

/* The driver runs in the calling thread's FPU state, which on Windows NT is
   53-bit precision (control word 0x27F); Linux starts threads at 64-bit
   (0x37F). The engine's x87 path rounds differently under the two, so each
   call into it switches to the Windows word and back. */
static uint16_t fpu_enter(void)
{
  uint16_t old, win = 0x27f;
  __asm__ volatile("fnstcw %0" : "=m"(old));
  __asm__ volatile("fldcw %0" : : "m"(win));
  return old;
}

static void fpu_leave(uint16_t old)
{
  __asm__ volatile("fldcw %0" : : "m"(old));
}

/* ------------------------------------------------------- kernel imports */

static STDCALL void *k_ExAllocatePoolWithTag(int type, uint32_t n, uint32_t tag)
{
  (void)type; (void)tag;
  return calloc(1, n ? n : 1);
}
static STDCALL void k_ExFreePool(void *p) { free(p); }

/* Feeds the CPU load limiter. A counter that never moves reads as zero load,
   so no voice is ever dropped for time and a render is the same on any
   machine. */
static STDCALL int64_t k_KeQueryPerformanceCounter(int64_t *freq)
{
  if (freq) *freq = 3579545;
  return 0;
}

static STDCALL int  k_KeSetEvent(void *e, int incr, int wait)
{ (void)e; (void)incr; (void)wait; return 0; }
static STDCALL void k_KeClearEvent(void *e) { (void)e; }
static STDCALL void k_IoAcquireCancelSpinLock(uint8_t *irql) { if (irql) *irql = 0; }
static STDCALL void k_IoReleaseCancelSpinLock(uint8_t irql) { (void)irql; }
static STDCALL int  k_ObReferenceObjectByHandle(void *h, uint32_t access, void *type,
                                                int mode, void **obj, void *info)
{ (void)h; (void)access; (void)type; (void)mode; (void)info; if (obj) *obj = NULL; return -1; }
static STDCALL void k_IoDeleteDevice(void *d) { (void)d; }
static STDCALL int  k_IoCreateSymbolicLink(void *a, void *b) { (void)a; (void)b; return -1; }
static STDCALL void k_RtlInitUnicodeString(void *s, const void *src) { (void)s; (void)src; }
static STDCALL int  k_ZwClose(void *h) { (void)h; return 0; }
static STDCALL int  k_IoDeleteSymbolicLink(void *s) { (void)s; return 0; }
static STDCALL void *k_MmMapLockedPages(void *mdl, int mode) { (void)mdl; (void)mode; return NULL; }
static STDCALL int  k_IoCreateDevice(void *a, uint32_t b, void *c, uint32_t d,
                                     uint32_t e, uint8_t f, void **dev)
{ (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; if (dev) *dev = NULL; return -1; }
static FASTCALL long k_InterlockedExchange(volatile long *p, long v)
{ return __sync_lock_test_and_set(p, v); }
static FASTCALL void k_IofCompleteRequest(void *irp, int8_t boost) { (void)irp; (void)boost; }
static FASTCALL long k_ObfDereferenceObject(void *o) { (void)o; return 0; }
static void *k_ExEventObjectType;   /* a data import: a POBJECT_TYPE variable */

static const struct pe_binding kernel[] = {
  {"ExAllocatePoolWithTag", (void *)k_ExAllocatePoolWithTag},
  {"ExFreePool", (void *)k_ExFreePool},
  {"KeQueryPerformanceCounter", (void *)k_KeQueryPerformanceCounter},
  {"KeSetEvent", (void *)k_KeSetEvent},
  {"KeClearEvent", (void *)k_KeClearEvent},
  {"IoAcquireCancelSpinLock", (void *)k_IoAcquireCancelSpinLock},
  {"IoReleaseCancelSpinLock", (void *)k_IoReleaseCancelSpinLock},
  {"ObReferenceObjectByHandle", (void *)k_ObReferenceObjectByHandle},
  {"IoDeleteDevice", (void *)k_IoDeleteDevice},
  {"IoCreateSymbolicLink", (void *)k_IoCreateSymbolicLink},
  {"RtlInitUnicodeString", (void *)k_RtlInitUnicodeString},
  {"ZwClose", (void *)k_ZwClose},
  {"IoDeleteSymbolicLink", (void *)k_IoDeleteSymbolicLink},
  {"MmMapLockedPages", (void *)k_MmMapLockedPages},
  {"IoCreateDevice", (void *)k_IoCreateDevice},
  {"InterlockedExchange", (void *)k_InterlockedExchange},
  {"IofCompleteRequest", (void *)k_IofCompleteRequest},
  {"ObfDereferenceObject", (void *)k_ObfDereferenceObject},
  {"ExEventObjectType", (void *)&k_ExEventObjectType},
  {NULL, NULL}
};

/* ------------------------------------------------------------ the image */

static void *symbol(const struct pe_image *img, const char *name)
{
  return pe_symbol_coff(img, name, PE_SYM_EXTERNAL, PE_COFF_IMAGE);
}

/* The CPU check reads CR4 to see whether the OS saves SSE state, which
   faults outside ring 0. The host OS always does, so the read becomes an
   all-ones result: or eax,-1 in the same three bytes. */
static int patch_cr4(void *ctx, struct pe_image *img,
                     const struct pe_patch_area *area, char *err, size_t errlen)
{
  unsigned char *fn = symbol(img, "_GetCpuKind@0"), *hit = NULL;
  int i, count = 0;
  (void)ctx; (void)area;
  if (!fn) { snprintf(err, errlen, "no symbol _GetCpuKind@0"); return 0; }
  for (i = 0; i < 0x100; ++i)
    if (fn[i] == 0x0f && fn[i + 1] == 0x20 && fn[i + 2] == 0xe0) { hit = fn + i; ++count; }
  if (count != 1) { snprintf(err, errlen, "CR4 read not found where expected"); return 0; }
  hit[0] = 0x83; hit[1] = 0xc8; hit[2] = 0xff;
  return 1;
}

static unsigned char *read_file(const char *path, size_t *n)
{
  FILE *f = fopen(path, "rb");
  unsigned char *b = NULL;
  long len;
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) > 0) {
    rewind(f);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    else *n = (size_t)len;
  }
  fclose(f);
  return b;
}

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); goto fail; } while (0)

/* Every voice count, level multiplier and receive switch in the image starts
   at zero; VSCAPI fills them in after InitSynth. Without this the engine
   renders silence, or with the receive switches clear, every part stays on
   the piano it powered up with. These are the values CSynth::SetVscConfData
   sends, with the registry settings at their defaults unless overridden.
   The load limiter is on, as in VSC, but it measures time with
   KeQueryPerformanceCounter, which never advances here, so it never acts. */
static void apply_conf(struct vsc *v)
{
  struct synth_conf c;
  int rate = v->cfg.rate;
  memset(&c, 0, sizeof c);
  c.mask = 0x1fffffffu & ~0x00001000u & ~0x00020000u & ~0x00040000u;
  c.pitch_offset = rate == 44100 ? -1200.0f : rate == 11025 ? 1200.0f : 0.0f;
  c.cpu_power = (float)v->cfg.cpu_load_limit * 0.01f;
  c.power_limit = 1;
  c.voices = 128;
  c.voice_limit = (uint32_t)v->cfg.polyphony;
  c.use_chorus = c.use_reverb = c.use_tvf = c.use_delay = 1;
  c.amp_adj = 1.26f;
  c.rev_adj = v->cfg.rev_gain;
  c.cho_adj = v->cfg.cho_gain;
  c.delay_adj = v->cfg.delay_gain;
  c.master_volume = (float)v->cfg.volume * 0.01f;
  c.tvf_lfo_adj = 0.25f;
  c.efx_limit = 1;
  c.rcv_pc_bank = c.rcv_volume = c.rcv_panpot = c.rcv_expression = 0xffff;
  c.rcv_reverb_send = c.rcv_chorus_send = c.rcv_delay_send = 0xffff;
  c.rcv_reset = c.rcv_cancel_on_reset = 1;
  v->SetSynthConfData(&c);
}

struct vsc *vsc_open(const char *sys_path, const char *dat_path,
                     const struct vsc_config *cfg, char *err, size_t errlen)
{
  struct vsc *v = calloc(1, sizeof *v);
  struct pe_options opt;
  unsigned char *dat = NULL;
  volatile int32_t *mmx, *sse;
  size_t datn = 0;
  uint16_t cw;
  int ok, rate = cfg->rate;

  if (!v) { snprintf(err, errlen, "out of memory"); return NULL; }
  if (rate != 11025 && rate != 22050 && rate != 44100)
    FAIL("rate must be 11025, 22050 or 44100");
  if (cfg->polyphony < 1 || cfg->polyphony > 128) FAIL("polyphony must be 1 to 128");
  if (cfg->volume < 0 || cfg->volume > 100) FAIL("volume must be 0 to 100");
  if (cfg->cpu_load_limit < 0 || cfg->cpu_load_limit > 90)
    FAIL("CPU load limit must be 0 to 90");
  v->cfg = *cfg;

  memset(&opt, 0, sizeof opt);
  opt.resolve = pe_resolve_table;
  opt.ctx = (void *)kernel;
  opt.no_entry = 1;
  opt.patch = patch_cr4;
  v->img = pe_load(sys_path, &opt, err, errlen);
  if (!v->img) goto fail;

#define SYM(field, name) \
  if (!(*(void **)&v->field = symbol(v->img, name))) FAIL("%s: no symbol %s", sys_path, name)
  SYM(tgInit, "_tgInit@8");
  SYM(ConfigWaveBuf, "_ConfigWaveBuf@8");
  SYM(CloseWaveBuf, "_CloseWaveBuf@0");
  SYM(InitSynth, "_InitSynth@0");
  SYM(RxMidiMsg, "_RxMidiMsg@4");
  SYM(RxMidiLongMsg, "_RxMidiLongMsg@4");
  SYM(FillAudioBuffer, "_FillAudioBuffer@8");
  SYM(SetSynthConfData, "_SetSynthConfData@4");
  SYM(serviceInterval, "_serviceInterval");
  SYM(samplingRate, "_samplingRate");
  SYM(workCpu, "_workCpu");
  SYM(skipWriteQueue, "_waveInfo");
#undef SYM
  v->skipWriteQueue += 5;
  mmx = symbol(v->img, "_mmxEnable");
  sse = symbol(v->img, "_sseEnable");
  if (!mmx || !sse) FAIL("%s: missing CPU-check symbols", sys_path);
  if (cfg->cpu == VSC_CPU_FPU) *mmx = 0;
  if (cfg->cpu != VSC_CPU_AUTO) *sse = 0;

  if (!(dat = read_file(dat_path, &datn))) FAIL("cannot read %s", dat_path);
  if (datn < 0x34 || memcmp(dat, "Roland VSC T-2", 14)) FAIL("%s is not a VSC tone set", dat_path);

  cw = fpu_enter();
  v->tgInit(0, rate);
  ok = v->ConfigWaveBuf(dat, (int)datn);
  if (ok) {
    v->InitSynth();
    apply_conf(v);
  }
  fpu_leave(cw);
  /* the engine has copied the tone set into its own pool */
  free(dat); dat = NULL;
  if (!ok) FAIL("the engine rejected %s", dat_path);
  if (*v->samplingRate != rate) FAIL("the engine is at %d Hz, not %d", (int)*v->samplingRate, rate);
  return v;
fail:
  free(dat);
  vsc_close(v);
  return NULL;
}

int vsc_block_frames(const struct vsc *v) { return (int)*v->serviceInterval; }
int vsc_rate(const struct vsc *v) { return (int)*v->samplingRate; }
int vsc_cpu_kind(const struct vsc *v) { return (int)*v->workCpu; }

void vsc_short(struct vsc *v, uint32_t msg)
{
  uint16_t cw = fpu_enter();
  v->RxMidiMsg(&msg);
  fpu_leave(cw);
}

int vsc_sysex(struct vsc *v, const unsigned char *msg, size_t len)
{
  unsigned char buf[256];
  uint16_t cw;
  if (len < 2 || len > sizeof buf || msg[0] != 0xf0) return 0;
  memset(buf, 0, sizeof buf);
  memcpy(buf, msg, len);
  cw = fpu_enter();
  v->RxMidiLongMsg(buf);
  fpu_leave(cw);
  return 1;
}

void vsc_reset(struct vsc *v)
{
  uint16_t cw = fpu_enter();
  v->InitSynth();
  apply_conf(v);
  fpu_leave(cw);
}

void vsc_render(struct vsc *v, int16_t *out)
{
  uint16_t cw = fpu_enter();
  *v->skipWriteQueue = 1;
  v->FillAudioBuffer(1.0f, out);
  fpu_leave(cw);
}

void vsc_close(struct vsc *v)
{
  if (!v) return;
  if (v->img) {
    if (v->CloseWaveBuf) v->CloseWaveBuf();
    pe_unload(v->img);
  }
  free(v);
}
