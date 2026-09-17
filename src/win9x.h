/* SPDX-License-Identifier: CC0-1.0
 *
 * mingw implements C99 snprintf on top of _vscprintf, and _vscprintf calls
 * GetModuleHandleW to work out which msvcrt it is talking to. That call is a
 * stub on Windows 9x, so the binary ends up importing something the system it
 * targets cannot honour. msvcrt's own _vsnprintf has been there since Windows
 * 95; it differs only in leaving the buffer unterminated when the text does
 * not fit, which is what the wrapper below fixes.
 *
 * Include this before anything that formats into a buffer.
 */
#ifndef SCVA_WIN9X_H
#define SCVA_WIN9X_H

#if defined(_WIN32) && defined(__i386__)
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>

static int scva_vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap)
{
  int n;
  if (!cap) return 0;
  n = _vsnprintf(buf, cap, fmt, ap);
  buf[cap - 1] = '\0';                  /* _vsnprintf does not, when it fills */
  return n < 0 ? (int)cap - 1 : n;
}

static int scva_snprintf(char *buf, size_t cap, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = scva_vsnprintf(buf, cap, fmt, ap);
  va_end(ap);
  return n;
}
#define snprintf  scva_snprintf
#define vsnprintf scva_vsnprintf
#endif
#endif
