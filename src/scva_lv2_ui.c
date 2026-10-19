/* SPDX-License-Identifier: CC0-1.0
 *
 * The LV2 plugin's editor: scva_xui.c's window, talking to the plugin
 * through its control ports - Tone Map, Drum Kit (a GS program number) and
 * Instrument (a position in the map's tones), which it writes and is told of.
 * It reads the names itself, from where the plugin does, so it can run in a
 * process of its own, as Carla runs it.
 */
#define _GNU_SOURCE
#include "scva_lists.h"
#include "scva_xui.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lv2/core/lv2.h>
#include <lv2/ui/ui.h>

#define SCVA_UI_URI "https://github.com/mlainez/scva-headless#ui"

/* the plugin's ports, as in lv2/scva.ttl */
#define PORT_MAP  3
#define PORT_KIT  4
#define PORT_INST 5

struct ui {
  LV2UI_Write_Function write;
  LV2UI_Controller ctl;
  struct scva_names names;
  struct scva_lists lists;
  struct scva_xui *x;
  int map, kit_prog, tone;
};

static void put(struct ui *u, uint32_t port, int v)
{
  float f = (float)v;
  u->write(u->ctl, port, sizeof f, 0, &f);
}

/* The kit's position in the map's list; one the map lacks shows Standard,
   which is what the plugin plays for it. */
static int kit_pos(const struct ui *u)
{
  int m = u->map;
  int k = scva_lists_find(&u->names, u->lists.kit[m], u->lists.nkit[m], 0,
                          u->kit_prog);
  if (k < 0) k = scva_lists_find(&u->names, u->lists.kit[m], u->lists.nkit[m],
                                 0, 0);
  return k;
}

static void show(struct ui *u)
{
  scva_xui_show(u->x, u->map, u->tone, kit_pos(u));
}

/* A pick shows at once: hosts need not echo a UI's own writes back. A new
   map keeps the instrument - its bank and program in the new map's list,
   as the CLAP editor does - and both go to the plugin. */
static void on_map(void *ctx, int map)
{
  struct ui *u = ctx;
  int n = (int)u->lists.ntone[u->map];
  if (n) {
    const struct scva_patch *p = &u->names.patch[
      u->lists.tone[u->map][u->tone < n ? u->tone : n - 1]];
    u->tone = scva_lists_carry(&u->names, p, u->lists.tone[map],
                               u->lists.ntone[map]);
  }
  u->map = map;
  put(u, PORT_MAP, map);
  put(u, PORT_INST, u->tone);
  show(u);
}

static void on_tone(void *ctx, int pos)
{
  struct ui *u = ctx;
  u->tone = pos;
  put(u, PORT_INST, pos);
}

static void on_kit(void *ctx, int pos)
{
  struct ui *u = ctx;
  u->kit_prog = u->names.patch[u->lists.kit[u->map][pos]].prog;
  put(u, PORT_KIT, u->kit_prog);
}

static LV2UI_Handle instantiate(const LV2UI_Descriptor *d, const char *plugin,
                                const char *bundle, LV2UI_Write_Function write,
                                LV2UI_Controller ctl, LV2UI_Widget *widget,
                                const LV2_Feature *const *features)
{
  struct ui *u;
  struct scva_xui_host host;
  const LV2UI_Resize *resize = NULL;
  void *parent = NULL;
  char here[1024], dir[1024];
  size_t n;
  int i, w, h;
  (void)d; (void)plugin;

  for (i = 0; features && features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_UI__parent)) parent = features[i]->data;
    if (!strcmp(features[i]->URI, LV2_UI__resize)) resize = features[i]->data;
  }
  u = calloc(1, sizeof *u);
  if (!u) return NULL;
  u->write = write;
  u->ctl = ctl;

  snprintf(here, sizeof here, "%s", bundle ? bundle : "");
  n = strlen(here);
  while (n > 1 && here[n - 1] == '/') here[--n] = '\0';
  scva_names_load(&u->names, scva_core_dir(here, dir, sizeof dir));
  if (!scva_lists_build(&u->lists, &u->names)) {
    scva_names_free(&u->names);
    free(u);
    return NULL;
  }
  host.ctx = u;
  host.set_map = on_map;
  host.set_tone = on_tone;
  host.set_kit = on_kit;
  u->x = scva_xui_open((unsigned long)(uintptr_t)parent, &u->names, &u->lists,
                       &host);
  if (!u->x) {
    fprintf(stderr, "scva.lv2 ui: no X display\n");
    scva_lists_free(&u->lists);
    scva_names_free(&u->names);
    free(u);
    return NULL;
  }
  show(u);
  scva_xui_size(u->x, &w, &h);
  if (resize) resize->ui_resize(resize->handle, w, h);
  *widget = (LV2UI_Widget)(uintptr_t)scva_xui_window(u->x);
  return u;
}

static void cleanup(LV2UI_Handle h)
{
  struct ui *u = h;
  scva_xui_close(u->x);
  scva_lists_free(&u->lists);
  scva_names_free(&u->names);
  free(u);
}

static void port_event(LV2UI_Handle h, uint32_t port, uint32_t size,
                       uint32_t format, const void *buffer)
{
  struct ui *u = h;
  int v;
  if (format != 0 || size != sizeof(float)) return;
  v = (int)(*(const float *)buffer + 0.5f);
  if (port == PORT_MAP && v >= 0 && v < SCVA_MAPS) u->map = v;
  else if (port == PORT_KIT) u->kit_prog = v;
  else if (port == PORT_INST && v >= 0) u->tone = v;
  else return;
  show(u);
}

static int idle(LV2UI_Handle h)
{
  struct ui *u = h;
  scva_xui_idle(u->x);
  return 0;
}

static const LV2UI_Idle_Interface idle_iface = { idle };

static const void *extension_data(const char *uri)
{
  if (!strcmp(uri, LV2_UI__idleInterface)) return &idle_iface;
  return NULL;
}

static const LV2UI_Descriptor descriptor = {
  SCVA_UI_URI, instantiate, cleanup, port_event, extension_data
};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor *lv2ui_descriptor(uint32_t index)
{
  return index == 0 ? &descriptor : NULL;
}
