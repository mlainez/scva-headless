/* SPDX-License-Identifier: CC0-1.0
 *
 * See scva_xui.h.
 */
#include "scva_xui.h"
#include "scva_map.h"
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdlib.h>
#include <string.h>

/* Colours, filled from the server at open. */
enum { C_BG, C_FACE, C_EDGE, C_TEXT, C_SEL, C_SELTEXT, C_LIST, C_BAR, C_N };
static const char *const c_spec[C_N] = {
  "#d6d6d6", "#f2f2f2", "#8a8a8a", "#000000", "#3a6ea5", "#ffffff",
  "#ffffff", "#b4b4b4"
};

static const char *const map_name[SCVA_MAPS] = {
  "Default", "SC-55", "SC-88", "SC-88Pro", "SC-8820"
};
/* the buttons left to right, as the hardware came out */
static const int map_order[SCVA_MAPS] = {
  SCVA_MAP_DEFAULT, SCVA_MAP_55, SCVA_MAP_88, SCVA_MAP_88PRO, SCVA_MAP_8820
};

/* One scrolling list: the map's tones or kits. */
struct xlist {
  int x, y, w, h;          /* the box, scrollbar included */
  int top;                 /* first row shown */
  int sel;                 /* selected position */
};

struct scva_xui {
  Display *dpy;
  Window win;
  Pixmap back;
  GC gc;
  XFontStruct *font;
  unsigned long c[C_N];
  int w, h, bw, bh;        /* window, and the backbuffer's size */
  int lh, asc, pad;        /* text line height, font ascent, spacing */

  const struct scva_names *N;
  const struct scva_lists *L;
  struct scva_xui_host host;

  int map;
  struct xlist list[2];    /* 0 instruments, 1 drum kits */
  int bx[SCVA_MAPS], by, bwid[SCVA_MAPS], bht;  /* the map buttons */
  int drag;                /* list whose scrollbar is held, or -1 */
  int dirty;
};

#define BAR 14             /* scrollbar width at 96 DPI */

static uint32_t count(const struct scva_xui *u, int i)
{
  return i ? u->L->nkit[u->map] : u->L->ntone[u->map];
}

static const char *label(const struct scva_xui *u, int i, int pos, char *buf,
                         size_t cap)
{
  const uint32_t *l = i ? u->L->kit[u->map] : u->L->tone[u->map];
  scva_patch_label(&u->N->patch[l[pos]], buf, cap);
  return buf;
}

static int rows(const struct scva_xui *u, const struct xlist *l)
{
  int r = (l->h - 4) / u->lh;
  return r < 1 ? 1 : r;
}

static void clamp_top(struct scva_xui *u, int i)
{
  struct xlist *l = &u->list[i];
  int n = (int)count(u, i), r = rows(u, l);
  if (l->top > n - r) l->top = n - r;
  if (l->top < 0) l->top = 0;
}

static void reveal(struct scva_xui *u, int i)
{
  struct xlist *l = &u->list[i];
  int r = rows(u, l);
  if (l->sel < l->top) l->top = l->sel;
  else if (l->sel >= l->top + r) l->top = l->sel - r / 2;
  clamp_top(u, i);
}

static void layout(struct scva_xui *u)
{
  int i, x = u->pad, lw, top, colw;
  char *lbl = "Tone Map";
  u->by = u->pad;
  u->bht = u->lh + 2 * u->pad;
  x += XTextWidth(u->font, lbl, (int)strlen(lbl)) + 2 * u->pad;
  for (i = 0; i < SCVA_MAPS; ++i) {
    const char *t = map_name[map_order[i]];
    u->bwid[i] = XTextWidth(u->font, t, (int)strlen(t)) + 3 * u->pad;
    u->bx[i] = x;
    x += u->bwid[i] + u->pad / 2;
  }
  top = u->by + u->bht + 2 * u->pad + u->lh;          /* below the headings */
  colw = (u->w - 3 * u->pad);
  lw = colw * 3 / 5;
  u->list[0].x = u->pad;
  u->list[0].w = lw;
  u->list[1].x = 2 * u->pad + lw;
  u->list[1].w = colw - lw;
  for (i = 0; i < 2; ++i) {
    u->list[i].y = top;
    u->list[i].h = u->h - top - u->pad;
    if (u->list[i].h < u->lh + 4) u->list[i].h = u->lh + 4;
    clamp_top(u, i);
  }
}

static void text(struct scva_xui *u, int x, int y, const char *s, int col)
{
  XSetForeground(u->dpy, u->gc, u->c[col]);
  XDrawString(u->dpy, u->back, u->gc, x, y + u->asc, s, (int)strlen(s));
}

static void box(struct scva_xui *u, int x, int y, int w, int h, int fill)
{
  XSetForeground(u->dpy, u->gc, u->c[fill]);
  XFillRectangle(u->dpy, u->back, u->gc, x, y, (unsigned)w, (unsigned)h);
  XSetForeground(u->dpy, u->gc, u->c[C_EDGE]);
  XDrawRectangle(u->dpy, u->back, u->gc, x, y, (unsigned)w - 1, (unsigned)h - 1);
}

static void draw_list(struct scva_xui *u, int i)
{
  struct xlist *l = &u->list[i];
  int n = (int)count(u, i), r = rows(u, l), k, bar = BAR * u->lh / 13;
  char buf[SCVA_NAME_MAX + 16];
  XRectangle clip;

  box(u, l->x, l->y, l->w, l->h, C_LIST);
  clip.x = (short)(l->x + 1);
  clip.y = (short)(l->y + 1);
  clip.width = (unsigned short)(l->w - bar - 2);
  clip.height = (unsigned short)(l->h - 2);
  XSetClipRectangles(u->dpy, u->gc, 0, 0, &clip, 1, Unsorted);
  for (k = 0; k < r && l->top + k < n; ++k) {
    int pos = l->top + k, y = l->y + 2 + k * u->lh;
    if (pos == l->sel) {
      XSetForeground(u->dpy, u->gc, u->c[C_SEL]);
      XFillRectangle(u->dpy, u->back, u->gc, l->x + 1, y,
                     (unsigned)(l->w - bar - 2), (unsigned)u->lh);
    }
    text(u, l->x + u->pad / 2 + 2, y, label(u, i, pos, buf, sizeof buf),
         pos == l->sel ? C_SELTEXT : C_TEXT);
  }
  XSetClipMask(u->dpy, u->gc, None);

  /* the scrollbar: the thumb is the shown rows' share of the list */
  {
    int bx = l->x + l->w - bar - 1, by = l->y + 1, bh = l->h - 2;
    int th = n > r ? bh * r / n : bh, ty = by;
    if (th < u->lh) th = u->lh;
    if (n > r) ty = by + (bh - th) * l->top / (n - r);
    XSetForeground(u->dpy, u->gc, u->c[C_BG]);
    XFillRectangle(u->dpy, u->back, u->gc, bx, by, (unsigned)bar, (unsigned)bh);
    box(u, bx + 1, ty, bar - 2, th, C_BAR);
  }
}

static void draw(struct scva_xui *u)
{
  int i;
  if (u->bw != u->w || u->bh != u->h) {
    if (u->back) XFreePixmap(u->dpy, u->back);
    u->back = XCreatePixmap(u->dpy, u->win, (unsigned)u->w, (unsigned)u->h,
                            (unsigned)DefaultDepth(u->dpy, DefaultScreen(u->dpy)));
    u->bw = u->w;
    u->bh = u->h;
  }
  XSetForeground(u->dpy, u->gc, u->c[C_BG]);
  XFillRectangle(u->dpy, u->back, u->gc, 0, 0, (unsigned)u->w, (unsigned)u->h);

  text(u, u->pad, u->by + u->pad, "Tone Map", C_TEXT);
  for (i = 0; i < SCVA_MAPS; ++i) {
    int on = map_order[i] == u->map;
    box(u, u->bx[i], u->by, u->bwid[i], u->bht, on ? C_SEL : C_FACE);
    text(u, u->bx[i] + u->pad + u->pad / 2, u->by + u->pad,
         map_name[map_order[i]], on ? C_SELTEXT : C_TEXT);
  }
  text(u, u->list[0].x, u->list[0].y - u->lh - u->pad / 2,
       "Instrument (part 1)", C_TEXT);
  text(u, u->list[1].x, u->list[1].y - u->lh - u->pad / 2,
       "Drum Kit (part 10)", C_TEXT);
  draw_list(u, 0);
  draw_list(u, 1);
  XCopyArea(u->dpy, u->back, u->win, u->gc, 0, 0, (unsigned)u->w,
            (unsigned)u->h, 0, 0);
  u->dirty = 0;
}

static int in(int px, int py, int x, int y, int w, int h)
{
  return px >= x && px < x + w && py >= y && py < y + h;
}

/* The scrollbar at y: the thumb's top follows the pointer. */
static void scroll_to(struct scva_xui *u, int i, int py)
{
  struct xlist *l = &u->list[i];
  int n = (int)count(u, i), r = rows(u, l);
  if (n <= r) return;
  l->top = (py - l->y) * n / l->h - r / 2;
  clamp_top(u, i);
  u->dirty = 1;
}

static void press(struct scva_xui *u, XButtonEvent *e)
{
  int i, bar = BAR * u->lh / 13;
  if (e->button == Button1)
    for (i = 0; i < SCVA_MAPS; ++i)
      if (in(e->x, e->y, u->bx[i], u->by, u->bwid[i], u->bht)) {
        if (map_order[i] != u->map && u->host.set_map)
          u->host.set_map(u->host.ctx, map_order[i]);
        return;
      }
  for (i = 0; i < 2; ++i) {
    struct xlist *l = &u->list[i];
    if (!in(e->x, e->y, l->x, l->y, l->w, l->h)) continue;
    if (e->button == Button4 || e->button == Button5) {
      l->top += e->button == Button4 ? -3 : 3;
      clamp_top(u, i);
      u->dirty = 1;
    } else if (e->button == Button1 && e->x >= l->x + l->w - bar - 1) {
      u->drag = i;
      scroll_to(u, i, e->y);
    } else if (e->button == Button1) {
      int pos = l->top + (e->y - l->y - 2) / u->lh;
      if (pos >= 0 && pos < (int)count(u, i) && pos != l->sel) {
        l->sel = pos;
        u->dirty = 1;
        if (i == 0 && u->host.set_tone) u->host.set_tone(u->host.ctx, pos);
        if (i == 1 && u->host.set_kit) u->host.set_kit(u->host.ctx, pos);
      }
    }
    return;
  }
}

static unsigned long colour(Display *d, const char *spec, unsigned long dflt)
{
  XColor c;
  Colormap cm = DefaultColormap(d, DefaultScreen(d));
  if (XParseColor(d, cm, spec, &c) && XAllocColor(d, cm, &c)) return c.pixel;
  return dflt;
}

/* The fixed font every X server has, a larger cut on a dense screen. */
static XFontStruct *pick_font(Display *d)
{
  int s = DefaultScreen(d), mm = DisplayHeightMM(d, s);
  int dpi = mm > 0 ? DisplayHeight(d, s) * 254 / (mm * 10) : 96;
  const char *const big[] = {
    "-misc-fixed-medium-r-normal--20-*-*-*-*-*-iso8859-1", "10x20", NULL };
  const char *const small[] = {
    "-misc-fixed-medium-r-normal--13-*-*-*-*-*-iso8859-1", "fixed", NULL };
  const char *const *try = dpi >= 140 ? big : small;
  XFontStruct *f = NULL;
  for (; *try && !f; ++try) f = XLoadQueryFont(d, *try);
  return f ? f : XLoadQueryFont(d, "fixed");
}

struct scva_xui *scva_xui_open(unsigned long parent,
                               const struct scva_names *N,
                               const struct scva_lists *L,
                               const struct scva_xui_host *host)
{
  struct scva_xui *u = calloc(1, sizeof *u);
  int i, s;
  if (!u) return NULL;
  u->dpy = XOpenDisplay(NULL);
  if (!u->dpy) { free(u); return NULL; }
  s = DefaultScreen(u->dpy);
  u->font = pick_font(u->dpy);
  if (!u->font) { XCloseDisplay(u->dpy); free(u); return NULL; }
  u->N = N;
  u->L = L;
  u->host = *host;
  u->drag = -1;
  u->asc = u->font->ascent;
  u->lh = u->font->ascent + u->font->descent + 2;
  u->pad = u->lh / 2;
  u->w = 46 * u->lh;
  u->h = 34 * u->lh;
  for (i = 0; i < C_N; ++i)
    u->c[i] = colour(u->dpy, c_spec[i],
                     i == C_TEXT || i == C_EDGE ? BlackPixel(u->dpy, s)
                                                : WhitePixel(u->dpy, s));
  u->win = XCreateSimpleWindow(u->dpy, parent ? (Window)parent
                                              : RootWindow(u->dpy, s),
                               0, 0, (unsigned)u->w, (unsigned)u->h, 0,
                               u->c[C_EDGE], u->c[C_BG]);
  XSelectInput(u->dpy, u->win, ExposureMask | ButtonPressMask |
               ButtonReleaseMask | Button1MotionMask | StructureNotifyMask);
  u->gc = XCreateGC(u->dpy, u->win, 0, NULL);
  XSetFont(u->dpy, u->gc, u->font->fid);
  layout(u);
  XMapRaised(u->dpy, u->win);
  XFlush(u->dpy);
  u->dirty = 1;
  return u;
}

void scva_xui_close(struct scva_xui *u)
{
  if (!u) return;
  if (u->back) XFreePixmap(u->dpy, u->back);
  XFreeGC(u->dpy, u->gc);
  XFreeFont(u->dpy, u->font);
  XDestroyWindow(u->dpy, u->win);
  XCloseDisplay(u->dpy);
  free(u);
}

unsigned long scva_xui_window(const struct scva_xui *u) { return u->win; }

void scva_xui_size(const struct scva_xui *u, int *w, int *h)
{
  *w = u->w;
  *h = u->h;
}

void scva_xui_resize(struct scva_xui *u, int w, int h)
{
  if (w < 1 || h < 1) return;
  XResizeWindow(u->dpy, u->win, (unsigned)w, (unsigned)h);
  u->w = w;
  u->h = h;
  layout(u);
  u->dirty = 1;
}

void scva_xui_visible(struct scva_xui *u, int on)
{
  if (on) XMapRaised(u->dpy, u->win);
  else XUnmapWindow(u->dpy, u->win);
  XFlush(u->dpy);
}

void scva_xui_show(struct scva_xui *u, int map, int tone, int kit)
{
  int i, nt, nk;
  if (map < 0 || map >= SCVA_MAPS) map = SCVA_MAP_DEFAULT;
  if (map != u->map) {
    u->map = map;
    u->list[0].sel = -1;
    u->list[1].sel = -1;
    u->dirty = 1;
  }
  nt = (int)count(u, 0);
  nk = (int)count(u, 1);
  if (tone >= nt) tone = nt - 1;
  if (kit >= nk) kit = nk - 1;
  for (i = 0; i < 2; ++i) {
    int want = i ? kit : tone;
    if (want != u->list[i].sel) {
      u->list[i].sel = want;
      if (want >= 0) reveal(u, i);
      u->dirty = 1;
    }
  }
}

void scva_xui_idle(struct scva_xui *u)
{
  while (XPending(u->dpy)) {
    XEvent e;
    XNextEvent(u->dpy, &e);
    switch (e.type) {
    case Expose:
      if (e.xexpose.count == 0) u->dirty = 1;
      break;
    case ConfigureNotify:
      if (e.xconfigure.width != u->w || e.xconfigure.height != u->h) {
        u->w = e.xconfigure.width;
        u->h = e.xconfigure.height;
        layout(u);
        u->dirty = 1;
      }
      break;
    case ButtonPress:
      press(u, &e.xbutton);
      break;
    case ButtonRelease:
      u->drag = -1;
      break;
    case MotionNotify:
      if (u->drag >= 0) scroll_to(u, u->drag, e.xmotion.y);
      break;
    default:
      break;
    }
  }
  if (u->dirty) draw(u);
  XFlush(u->dpy);
}
