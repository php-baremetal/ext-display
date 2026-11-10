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
 * Reading text with the fonts of display_font.h: UTF-8 decoding, a GFX or extended font seen the same way, glyph
 * lookup and advance. Header only (static inline), plain C, no framebuffer: ext-display draws with it, and a
 * renderer that draws text on its own uses it to get the same metrics.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "display_font.h"

/* A GFX or extended font, seen the same way. GFX fonts have one range (gfx_first..gfx_last, ranges NULL). */
typedef struct {
    const uint8_t *bitmap;
    const GFXglyph *glyph;
    const display_font_range_t *ranges;
    uint16_t range_count;
    uint32_t gfx_first, gfx_last;
    uint8_t bpp, line_height, ascent;
} display_text_font_t;

/* The next codepoint of [*p, end) (*p < end), advancing *p; an invalid or truncated sequence is U+FFFD (one
   byte). */
static inline uint32_t display_text_utf8(const char **p, const char *end)
{
    const unsigned char *s = (const unsigned char *)*p;
    size_t left = (size_t)(end - *p);
    unsigned char b = s[0];
    uint32_t cp;
    size_t n;
    if (b < 0x80) {
        *p += 1;
        return b;
    }
    if (b >= 0xC2 && b <= 0xDF) {
        n = 2; cp = b & 0x1F;
    } else if (b >= 0xE0 && b <= 0xEF) {
        n = 3; cp = b & 0x0F;
    } else if (b >= 0xF0 && b <= 0xF4) {
        n = 4; cp = b & 0x07;
    } else {
        *p += 1;
        return 0xFFFD;
    }
    if (left < n) {
        *p += 1;
        return 0xFFFD;
    }
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *p += 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if ((n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
        *p += 1;
        return 0xFFFD;
    }
    *p += n;
    return cp;
}

/* A GFX font: 1 bpp, ascent = the highest glyph's -yOffset. */
static inline void display_text_font_gfx(display_text_font_t *f, const GFXfont *gfx)
{
    int top = 0;
    for (unsigned i = 0; gfx->last >= gfx->first && i <= (unsigned)(gfx->last - gfx->first); i++) {
        if (gfx->glyph[i].yOffset < top) {
            top = gfx->glyph[i].yOffset;
        }
    }
    *f = (display_text_font_t){
        .bitmap = gfx->bitmap, .glyph = gfx->glyph, .ranges = NULL, .range_count = 0,
        .gfx_first = gfx->first, .gfx_last = gfx->last,
        .bpp = 1, .line_height = gfx->yAdvance, .ascent = (uint8_t)-top,
    };
}

/* An extended font; false for a bpp other than 1, 2 or 4: the font cannot be drawn. */
static inline bool display_text_font_ext(display_text_font_t *f, const display_font_t *ext)
{
    if (ext->bpp != 1 && ext->bpp != 2 && ext->bpp != 4) {
        return false;
    }
    *f = (display_text_font_t){
        .bitmap = ext->bitmap, .glyph = ext->glyph, .ranges = ext->ranges, .range_count = ext->range_count,
        .gfx_first = 1, .gfx_last = 0,
        .bpp = ext->bpp, .line_height = ext->line_height, .ascent = ext->ascent,
    };
    return true;
}

/* One entry of display_fonts[]; false when it has no font, no bitmap or glyph table, or cannot be drawn. */
static inline bool display_text_font_entry(display_text_font_t *f, const struct display_font_entry *e)
{
    if (!e) {
        return false;
    }
    if (e->gfx) {
        const GFXfont *g = (const GFXfont *)e->gfx;
        if (!g->bitmap || !g->glyph) {
            return false;
        }
        display_text_font_gfx(f, g);
        return true;
    }
    const display_font_t *x = (const display_font_t *)e->ext;
    return x && x->bitmap && x->glyph && display_text_font_ext(f, x);
}

/* The glyph of a codepoint, or NULL. */
static inline const GFXglyph *display_text_glyph(const display_text_font_t *f, uint32_t cp)
{
    if (!f->ranges) {
        return cp >= f->gfx_first && cp <= f->gfx_last ? &f->glyph[cp - f->gfx_first] : NULL;
    }
    for (uint16_t i = 0; i < f->range_count; i++) {
        const display_font_range_t *r = &f->ranges[i];
        if (cp >= r->first && cp <= r->last) {
            return &f->glyph[r->glyph + (cp - r->first)];
        }
    }
    return NULL;
}

/* The advance of a glyph; a missing one (g NULL, drawn as a box) is half the line height wide. */
static inline int display_text_advance(const display_text_font_t *f, const GFXglyph *g)
{
    return g ? g->xAdvance : f->line_height / 2;
}
