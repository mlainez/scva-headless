/* SPDX-License-Identifier: CC0-1.0
 *
 * A stand-in for SCCore.dll: the same seventeen exports, no synthesis. Every
 * call is logged as one line, so a test can see exactly what a front end asks
 * of the engine, and TG_Process writes a known signal, so a test can check
 * what reaches the output.
 *
 * Built two ways, with mingw-w64:
 *
 *   FAKE_LINUX  no imports at all: loads with this project's own PE loader,
 *               in either width, and logs to file descriptor 3 by raw Linux
 *               system calls. The test opens fd 3 before starting the tool.
 *   (default)   imports kernel32 only: loads with LoadLibrary or the PE
 *               loader on Windows (or wine), and appends to the file named by
 *               SCVA_FAKE_LOG.
 *
 * Log lines start with "@<image base>" so that the two engines of a two-port
 * song can be told apart; the tests rename the bases A, B... in order of
 * appearance.
 *
 * The signal: frame n of an instance's output is
 *     left  =  (n % 64) / 256
 *     right = -(n % 64) / 256
 * counted from that instance's first TG_Process call.
 */
#ifdef FAKE_LINUX
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define EXPORT __declspec(dllexport)

extern IMAGE_DOS_HEADER __ImageBase;

/* ---- output -------------------------------------------------------------- */

static char line[2048];
static int len;

#ifdef FAKE_LINUX
static void sys_write(const char *p, int n)
{
#if defined(__x86_64__)
  long ret;
  __asm__ volatile("syscall" : "=a"(ret) : "a"(1L), "D"(3L), "S"(p), "d"((long)n)
                   : "rcx", "r11", "memory");
  (void)ret;
#else
  long ret;
  __asm__ volatile("int $0x80" : "=a"(ret) : "a"(4L), "b"(3L), "c"(p), "d"((long)n)
                   : "memory");
  (void)ret;
#endif
}

static void sys_exit(int code)
{
#if defined(__x86_64__)
  __asm__ volatile("syscall" : : "a"(231L), "D"((long)code) : "rcx", "r11", "memory");
#else
  __asm__ volatile("int $0x80" : : "a"(252L), "b"((long)code) : "memory");
#endif
  for (;;) {}
}
#else
static HANDLE logh = INVALID_HANDLE_VALUE;

static void sys_write(const char *p, int n)
{
  DWORD done;
  if (logh == INVALID_HANDLE_VALUE) {
    char path[1024];
    if (!GetEnvironmentVariableA("SCVA_FAKE_LOG", path, sizeof path)) return;
    logh = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (logh == INVALID_HANDLE_VALUE) return;
  }
  WriteFile(logh, p, (DWORD)n, &done, NULL);
}

static void sys_exit(int code) { ExitProcess((UINT)code); }
#endif

static void put_str(const char *s) { while (*s && len < (int)sizeof line - 1) line[len++] = *s++; }

static void put_hex(unsigned long long v, int digits)
{
  static const char hex[] = "0123456789abcdef";
  while (digits--) if (len < (int)sizeof line - 1) line[len++] = hex[(v >> (4 * digits)) & 15];
}

static void put_int(long v)
{
  char tmp[24];
  int k = 0;
  unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
  if (v < 0) put_str("-");
  do { tmp[k++] = (char)('0' + u % 10); u /= 10; } while (u);
  while (k--) if (len < (int)sizeof line - 1) line[len++] = tmp[k];
}

/* Consecutive TG_Process calls of one size are logged as one line,
   "TG_Process <frames> <count>", written before the next other call. A
   daemon rendering into a device with no clock calls it constantly. */
static int pend_frames, pend_count;
static void flush_process(void);

static int is_process(const char *fn)
{
  const char *p = "TG_Process";
  while (*p && *fn == *p) { ++fn; ++p; }
  return !*p && !*fn;
}

static void begin(const char *fn)
{
  if (pend_count && !is_process(fn)) flush_process();
  len = 0;
  put_str("@");
  put_hex((unsigned long long)(size_t)&__ImageBase, sizeof(void *) * 2);
  put_str(" ");
  put_str(fn);
}

static void finish(void)
{
  line[len++] = '\n';
  sys_write(line, len);
}

/* ---- the thread block ---------------------------------------------------------
   Code built by MSVC reads the thread's TEB: its self pointer, and the stack
   bounds that stack probes compare against. On Windows every thread has one;
   in a Linux host it exists only if the loader gave this thread one. Read it
   the same way here, so a thread without one faults as it would with the
   real core, and bounds that do not contain this stack are reported. */

static void check_teb(void)
{
  void *self, *base, *limit;
  char here;
#if defined(__x86_64__)
  __asm__ volatile("mov %%gs:0x30, %0" : "=r"(self));
  __asm__ volatile("mov %%gs:0x08, %0" : "=r"(base));
  __asm__ volatile("mov %%gs:0x10, %0" : "=r"(limit));
#else
  __asm__ volatile("mov %%fs:0x18, %0" : "=r"(self));
  __asm__ volatile("mov %%fs:0x04, %0" : "=r"(base));
  __asm__ volatile("mov %%fs:0x08, %0" : "=r"(limit));
#endif
  if (!self || (char *)&here >= (char *)base || (char *)&here < (char *)limit) {
    len = 0;
    put_str("BADTEB\n");
    sys_write(line, len);
    sys_exit(99);
  }
}

/* ---- the engine's exports ------------------------------------------------- */

static unsigned frames;                 /* this instance's output position */
static struct { int a, b; } config;

EXPORT int TG_initialize(int arg)
{
  begin("TG_initialize"); put_str(" "); put_int(arg); finish();
  return 0;
}

EXPORT void TG_setSampleRate(float rate)
{
  begin("TG_setSampleRate"); put_str(" "); put_int((long)(rate + 0.5f)); finish();
}

EXPORT void TG_setMaxBlockSize(int frames_)
{
  begin("TG_setMaxBlockSize"); put_str(" "); put_int(frames_); finish();
}

EXPORT int TG_XPsetSystemConfig(const int *cfg)
{
  config.a = cfg[0];
  config.b = cfg[1];
  begin("TG_XPsetSystemConfig"); put_str(" "); put_int(cfg[0]); put_str(" ");
  put_int(cfg[1]); finish();
  return 0;
}

EXPORT void TG_XPgetCurSystemConfig(int *cfg)
{
  cfg[0] = config.a;
  cfg[1] = config.b;
  begin("TG_XPgetCurSystemConfig"); finish();
}

EXPORT int TG_activate(float rate, int max_block)
{
  begin("TG_activate"); put_str(" "); put_int((long)(rate + 0.5f)); put_str(" ");
  put_int(max_block); finish();
  return 0;
}

EXPORT void TG_deactivate(void) { begin("TG_deactivate"); finish(); }

EXPORT void TG_terminate(void)
{
  begin("TG_terminate"); finish();
  sys_exit(0);                          /* the real core calls exit() */
}

EXPORT void TG_ShortMidiIn(unsigned int msg, int when)
{
  check_teb();
  begin("TG_ShortMidiIn"); put_str(" ");
  put_hex(msg & 0xff, 2); put_str(" "); put_hex((msg >> 8) & 0xff, 2); put_str(" ");
  put_hex((msg >> 16) & 0xff, 2);
  put_str(" when="); put_int(when); finish();
}

EXPORT void TG_LongMidiIn(const unsigned char *msg, int when)
{
  int i;
  check_teb();
  begin("TG_LongMidiIn");
  for (i = 0; i < 1000; ++i) {
    put_str(" "); put_hex(msg[i], 2);
    if (msg[i] == 0xf7) break;
  }
  put_str(" when="); put_int(when); finish();
}

static void flush_process(void)
{
  int count = pend_count;
  pend_count = 0;
  begin("TG_Process"); put_str(" "); put_int(pend_frames); put_str(" ");
  put_int(count); finish();
}

EXPORT void TG_Process(float *left, float *right, int n)
{
  int i;
  check_teb();
  for (i = 0; i < n; ++i) {
    float v = (float)((frames + (unsigned)i) % 64u) / 256.0f;
    left[i] = v;
    right[i] = -v;
  }
  frames += (unsigned)n;
  if (pend_count && n != pend_frames) flush_process();
  pend_frames = n;
  ++pend_count;
}

#ifndef FAKE_NO_VOICES
EXPORT int TG_XPgetCurTotalRunningVoices(void)
{
  begin("TG_XPgetCurTotalRunningVoices"); finish();
  return 7;
}
#endif

EXPORT int TG_isFatalError(void) { return 0; }
EXPORT const char *TG_getErrorStrings(int k) { (void)k; return ""; }
EXPORT void TG_flushMidi(void) { begin("TG_flushMidi"); finish(); }
EXPORT void TG_PMidiIn(void) { begin("TG_PMidiIn"); finish(); }
EXPORT void TG_setInterruptThreadIdAtThisTime(void) {}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
  (void)h; (void)reason; (void)reserved;
  return TRUE;
}
