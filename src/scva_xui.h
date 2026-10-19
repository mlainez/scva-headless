/* SPDX-License-Identifier: CC0-1.0
 *
 * The plugins' editor on Linux: Tone Map buttons, and the map's instruments
 * and drum kits as two scrolling lists. Plain Xlib and the server's own
 * fixed font, so it needs nothing but libX11. Shared by the LV2 UI and the
 * CLAP editor; each drives it from its own thread and tells it the state.
 */
#ifndef SCVA_XUI_H
#define SCVA_XUI_H
#include "scva_lists.h"

/* What the user picked. Positions are in the current map's lists. */
struct scva_xui_host {
  void *ctx;
  void (*set_map)(void *ctx, int map);
  void (*set_tone)(void *ctx, int pos);
  void (*set_kit)(void *ctx, int pos);
};

struct scva_xui;

/* A window inside parent (an X11 window id), on the display in $DISPLAY.
   N and L must outlive it. NULL if there is no display. */
struct scva_xui *scva_xui_open(unsigned long parent,
                               const struct scva_names *N,
                               const struct scva_lists *L,
                               const struct scva_xui_host *host);
void scva_xui_close(struct scva_xui *u);
unsigned long scva_xui_window(const struct scva_xui *u);
/* Its size now. */
void scva_xui_size(const struct scva_xui *u, int *w, int *h);
/* Below its opening size the lists get shorter, not the window clipped. */
void scva_xui_resize(struct scva_xui *u, int w, int h);
void scva_xui_visible(struct scva_xui *u, int on);
/* The plugin's state; redraws if it changed. A new map scrolls both lists
   to their selection. */
void scva_xui_show(struct scva_xui *u, int map, int tone, int kit);
/* Handles whatever events are queued, then redraws if needed. Call it
   regularly from the thread that opened the window. */
void scva_xui_idle(struct scva_xui *u);
#endif
