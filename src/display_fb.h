/*
   +----------------------------------------------------------------------+
   | php-baremetal: ext-display                                           |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@php-baremetal.com>         |
   +----------------------------------------------------------------------+
*/

/*
 * The RGB565 framebuffer: primitives clipped to the screen, the dirty rectangle, UTF-8 text with GFX and
 * extended fonts. Plain C, no Zend: tools/host-c/cases/ext-display tests it on the PC.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display_font.h"

/* 1 (default): the framebuffer holds pixels in the panel's byte order (swapped at every write), so a
   full-width region is pushed straight from it. Measured on a 360x360 QSPI panel: a full flush takes 38 ms
   instead of 57 ms.
   0 (-DDISPLAY_FB_PRESWAP=0): native order, swapped by display_fb_emit() at every push. */
#ifndef DISPLAY_FB_PRESWAP
#define DISPLAY_FB_PRESWAP 1
#endif

typedef struct { int x0, y0, x1, y1; } display_fb_rect_t;   /* [x0,x1) x [y0,y1) */

typedef struct {
    uint16_t *px;            /* w*h pixels, rows top to bottom */
    int w, h;
    display_fb_rect_t dirty; /* empty when x0 >= x1 */
} display_fb_t;

/* A GFX or extended font, seen the same way. GFX fonts have one range (gfx_first..gfx_last, ranges NULL). */
typedef struct {
    const uint8_t *bitmap;
    const GFXglyph *glyph;
    const display_font_range_t *ranges;
    uint16_t range_count;
    uint32_t gfx_first, gfx_last;
    uint8_t bpp, line_height, ascent;
} display_fb_font_t;

static inline uint16_t display_fb_store(uint16_t c)
{
#if DISPLAY_FB_PRESWAP
    return (uint16_t)((c << 8) | (c >> 8));
#else
    return c;
#endif
}

static inline uint16_t display_fb_load(uint16_t v)
{
    return display_fb_store(v);   /* the swap is its own inverse */
}

void display_fb_init(display_fb_t *fb, uint16_t *px, int w, int h);
uint16_t display_fb_rgb565(int r, int g, int b);
uint16_t display_fb_get(const display_fb_t *fb, int x, int y);   /* native color; 0 off screen */

void display_fb_clear(display_fb_t *fb, uint16_t c);
void display_fb_pixel(display_fb_t *fb, int x, int y, uint16_t c);
void display_fb_line(display_fb_t *fb, int x0, int y0, int x1, int y1, uint16_t c);
void display_fb_rect(display_fb_t *fb, int x, int y, int w, int h, uint16_t c);
void display_fb_fill_rect(display_fb_t *fb, int x, int y, int w, int h, uint16_t c);
void display_fb_circle(display_fb_t *fb, int cx, int cy, int r, uint16_t c);
void display_fb_fill_circle(display_fb_t *fb, int cx, int cy, int r, uint16_t c);
/* w*h pixels, two bytes each in the panel's order (high byte first). */
void display_fb_image(display_fb_t *fb, int x, int y, int w, int h, const uint8_t *bytes);

/* Adds a rectangle (already clipped) to the dirty one. */
void display_fb_mark(display_fb_t *fb, int x0, int y0, int x1, int y1);
/* Takes the dirty rectangle and empties it; false when nothing changed. */
bool display_fb_take_dirty(display_fb_t *fb, display_fb_rect_t *out);
/* Clips r to the screen; false when nothing is left. */
bool display_fb_clip(const display_fb_t *fb, display_fb_rect_t *r);
/* Writes the rows of [x0,x1) x [y0,y1) contiguously in the panel's byte order (2 bytes per pixel). */
void display_fb_emit(const display_fb_t *fb, int x0, int y0, int x1, int y1, uint8_t *out);
/* The framebuffer rows y0..y1 as they go to the panel, when no copy is needed (full width, pre-swapped);
   NULL otherwise. */
const uint8_t *display_fb_direct(const display_fb_t *fb, int x0, int y0, int x1);

/* The next codepoint of [*p, end), advancing *p; an invalid or truncated sequence is U+FFFD (one byte). */
uint32_t display_fb_utf8(const char **p, const char *end);

void display_fb_font_gfx(display_fb_font_t *f, const GFXfont *gfx);
/* false for a bpp other than 1, 2 or 4: the font cannot be drawn. */
bool display_fb_font_ext(display_fb_font_t *f, const display_font_t *ext);
/* The glyph of a codepoint, or NULL. */
const GFXglyph *display_fb_glyph(const display_fb_font_t *f, uint32_t cp);

/* Draws s with y the top of the line; bg < 0 is transparent. Returns the width in pixels. */
int display_fb_text(display_fb_t *fb, int x, int y, const char *s, size_t len, uint16_t fg, int bg,
                    const display_fb_font_t *f);
int display_fb_text_width(const char *s, size_t len, const display_fb_font_t *f);
