/* SPDX-License-Identifier: CC0-1.0
 *
 * The SysEx messages that put a GS or General MIDI module back to its
 * power-on state. They belong to the MIDI and GS standards, not to any one
 * engine.
 */
#ifndef MIDI_RESET_H
#define MIDI_RESET_H
#include <stddef.h>

/* GS Reset: every part back to its power-on GS state. */
static inline const unsigned char *midi_gs_reset(size_t *len)
{
  static const unsigned char msg[] = {
    0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };
  if (len) *len = sizeof msg;
  return msg;
}

/* GM System On. */
static inline const unsigned char *midi_gm_reset(size_t *len)
{
  static const unsigned char msg[] = { 0xf0, 0x7e, 0x7f, 0x09, 0x01, 0xf7 };
  if (len) *len = sizeof msg;
  return msg;
}
#endif
