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
 * The RGB565 framebuffer behind Baremetal\Output\Display: see display_fb.h.
 */
#include "display_fb.h"

#include <limits.h>
#include <string.h>

void display_fb_init(display_fb_t *fb, uint16_t *px, int w, int h)
{
    fb->px = px;
    fb->w = w;
    fb->h = h;
    fb->dirty = (display_fb_rect_t){ 0, 0, 0, 0 };
}

uint16_t display_fb_rgb565(int r, int g, int b)
{
    r = r < 0 ? 0 : r > 255 ? 255 : r;
    g = g < 0 ? 0 : g > 255 ? 255 : g;
    b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

uint16_t display_fb_get(const display_fb_t *fb, int x, int y)
{
    if (x < 0 || y < 0 || x >= fb->w || y >= fb->h) {
        return 0;
    }
    return display_fb_load(fb->px[(size_t)y * fb->w + x]);
}

void display_fb_mark(display_fb_t *fb, int x0, int y0, int x1, int y1)
{
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    display_fb_rect_t *d = &fb->dirty;
    if (d->x0 >= d->x1) {
        *d = (display_fb_rect_t){ x0, y0, x1, y1 };
        return;
    }
    if (x0 < d->x0) d->x0 = x0;
    if (y0 < d->y0) d->y0 = y0;
    if (x1 > d->x1) d->x1 = x1;
    if (y1 > d->y1) d->y1 = y1;
}

bool display_fb_clip(const display_fb_t *fb, display_fb_rect_t *r)
{
    if (r->x0 < 0) r->x0 = 0;
    if (r->y0 < 0) r->y0 = 0;
    if (r->x1 > fb->w) r->x1 = fb->w;
    if (r->y1 > fb->h) r->y1 = fb->h;
    return r->x0 < r->x1 && r->y0 < r->y1;
}

/* Marks the part of a box that is on screen. */
static void mark_box(display_fb_t *fb, int x0, int y0, int x1, int y1)
{
    display_fb_rect_t r = { x0, y0, x1, y1 };
    if (display_fb_clip(fb, &r)) {
        display_fb_mark(fb, r.x0, r.y0, r.x1, r.y1);
    }
}

bool display_fb_take_dirty(display_fb_t *fb, display_fb_rect_t *out)
{
    *out = fb->dirty;
    fb->dirty = (display_fb_rect_t){ 0, 0, 0, 0 };
    return out->x0 < out->x1 && out->y0 < out->y1;
}

/* Writes a pixel without marking it dirty. */
static inline void plot(display_fb_t *fb, int x, int y, uint16_t stored)
{
    if (x >= 0 && y >= 0 && x < fb->w && y < fb->h) {
        fb->px[(size_t)y * fb->w + x] = stored;
    }
}

static void span(display_fb_t *fb, int x, int y, int w, uint16_t stored)
{
    if (y < 0 || y >= fb->h) {
        return;
    }
    int x1 = x + w;
    if (x < 0) x = 0;
    if (x1 > fb->w) x1 = fb->w;
    uint16_t *row = fb->px + (size_t)y * fb->w;
    for (int i = x; i < x1; i++) {
        row[i] = stored;
    }
}

void display_fb_clear(display_fb_t *fb, uint16_t c)
{
    display_fb_fill_rect(fb, 0, 0, fb->w, fb->h, c);
}

void display_fb_pixel(display_fb_t *fb, int x, int y, uint16_t c)
{
    if (x >= 0 && y >= 0 && x < fb->w && y < fb->h) {
        fb->px[(size_t)y * fb->w + x] = display_fb_store(c);
        display_fb_mark(fb, x, y, x + 1, y + 1);
    }
}

void display_fb_fill_rect(display_fb_t *fb, int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    display_fb_rect_t r = { x, y, x + w, y + h };
    if (!display_fb_clip(fb, &r)) {
        return;
    }
    uint16_t s = display_fb_store(c);
    for (int j = r.y0; j < r.y1; j++) {
        span(fb, r.x0, j, r.x1 - r.x0, s);
    }
    display_fb_mark(fb, r.x0, r.y0, r.x1, r.y1);
}

void display_fb_rect(display_fb_t *fb, int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    display_fb_fill_rect(fb, x, y, w, 1, c);
    display_fb_fill_rect(fb, x, y + h - 1, w, 1, c);
    display_fb_fill_rect(fb, x, y, 1, h, c);
    display_fb_fill_rect(fb, x + w - 1, y, 1, h, c);
}

void display_fb_line(display_fb_t *fb, int x0, int y0, int x1, int y1, uint16_t c)
{
    uint16_t s = display_fb_store(c);
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    mark_box(fb, x0 < x1 ? x0 : x1, y0 < y1 ? y0 : y1, (x0 > x1 ? x0 : x1) + 1, (y0 > y1 ? y0 : y1) + 1);
    for (;;) {
        plot(fb, x0, y0, s);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* Midpoint circle: calls back once per octant point (x, y) with x <= y. */
#define CIRCLE_POINTS(r, BODY)                                    \
    do {                                                          \
        int f_ = 1 - (r), ddx_ = 1, ddy_ = -2 * (r), x = 0, y = (r); \
        BODY;                                                     \
        while (x < y) {                                           \
            if (f_ >= 0) {                                        \
                y--;                                              \
                ddy_ += 2;                                        \
                f_ += ddy_;                                       \
            }                                                     \
            x++;                                                  \
            ddx_ += 2;                                            \
            f_ += ddx_;                                           \
            BODY;                                                 \
        }                                                         \
    } while (0)

void display_fb_circle(display_fb_t *fb, int cx, int cy, int r, uint16_t c)
{
    if (r < 0) {
        return;
    }
    uint16_t s = display_fb_store(c);
    CIRCLE_POINTS(r, {
        plot(fb, cx + x, cy + y, s); plot(fb, cx - x, cy + y, s);
        plot(fb, cx + x, cy - y, s); plot(fb, cx - x, cy - y, s);
        plot(fb, cx + y, cy + x, s); plot(fb, cx - y, cy + x, s);
        plot(fb, cx + y, cy - x, s); plot(fb, cx - y, cy - x, s);
    });
    mark_box(fb, cx - r, cy - r, cx + r + 1, cy + r + 1);
}

void display_fb_fill_circle(display_fb_t *fb, int cx, int cy, int r, uint16_t c)
{
    if (r < 0) {
        return;
    }
    uint16_t s = display_fb_store(c);
    CIRCLE_POINTS(r, {
        span(fb, cx - x, cy + y, 2 * x + 1, s); span(fb, cx - x, cy - y, 2 * x + 1, s);
        span(fb, cx - y, cy + x, 2 * y + 1, s); span(fb, cx - y, cy - x, 2 * y + 1, s);
    });
    mark_box(fb, cx - r, cy - r, cx + r + 1, cy + r + 1);
}

void display_fb_image(display_fb_t *fb, int x, int y, int w, int h, const uint8_t *bytes)
{
    if (w <= 0 || h <= 0 || x >= fb->w || y >= fb->h) {
        return;
    }
    /* end points in 64 bits: x + w cannot overflow */
    int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if (x1 <= 0 || y1 <= 0) {
        return;
    }
    display_fb_rect_t r = { x, y, x1 > fb->w ? fb->w : (int)x1, y1 > fb->h ? fb->h : (int)y1 };
    if (!display_fb_clip(fb, &r)) {
        return;
    }
    for (int j = r.y0; j < r.y1; j++) {
        const uint8_t *src = bytes + ((size_t)(j - y) * w + (r.x0 - x)) * 2;
        uint16_t *dst = fb->px + (size_t)j * fb->w;
        for (int i = r.x0; i < r.x1; i++, src += 2) {
            dst[i] = display_fb_store((uint16_t)((src[0] << 8) | src[1]));
        }
    }
    display_fb_mark(fb, r.x0, r.y0, r.x1, r.y1);
}

void display_fb_emit(const display_fb_t *fb, int x0, int y0, int x1, int y1, uint8_t *out)
{
    for (int j = y0; j < y1; j++) {
        const uint16_t *row = fb->px + (size_t)j * fb->w;
#if DISPLAY_FB_PRESWAP
        memcpy(out, row + x0, (size_t)(x1 - x0) * 2);
        out += (size_t)(x1 - x0) * 2;
#else
        for (int i = x0; i < x1; i++) {
            *out++ = (uint8_t)(row[i] >> 8);
            *out++ = (uint8_t)row[i];
        }
#endif
    }
}

const uint8_t *display_fb_direct(const display_fb_t *fb, int x0, int y0, int x1)
{
#if DISPLAY_FB_PRESWAP
    if (x0 == 0 && x1 == fb->w) {
        return (const uint8_t *)(fb->px + (size_t)y0 * fb->w);
    }
#else
    (void)fb; (void)x0; (void)y0; (void)x1;
#endif
    return NULL;
}

static uint16_t blend(uint16_t fg, uint16_t bg, unsigned v, unsigned max)
{
    unsigned r = (((fg >> 11) & 0x1F) * v + ((bg >> 11) & 0x1F) * (max - v) + max / 2) / max;
    unsigned g = (((fg >> 5) & 0x3F) * v + ((bg >> 5) & 0x3F) * (max - v) + max / 2) / max;
    unsigned b = ((fg & 0x1F) * v + (bg & 0x1F) * (max - v) + max / 2) / max;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void draw_glyph(display_fb_t *fb, int cx, int base, const display_fb_font_t *f, const GFXglyph *g,
                       uint16_t fg, int bg)
{
    unsigned bpp = f->bpp, max = (1u << bpp) - 1;
    uint16_t sfg = display_fb_store(fg);
    const uint8_t *bits = f->bitmap + g->bitmapOffset;
    int x0 = cx + g->xOffset, y0 = base + g->yOffset;
    size_t bit = 0;
    for (int j = 0; j < g->height; j++) {
        for (int i = 0; i < g->width; i++, bit += bpp) {
            unsigned v = (bits[bit / 8] >> (8 - bpp - bit % 8)) & max;
            if (bg >= 0) {
                if (v) {   /* v == 0: the cell is already bg; a box that overhangs keeps the neighbour's ink */
                    plot(fb, x0 + i, y0 + j, display_fb_store(blend(fg, (uint16_t)bg, v, max)));
                }
            } else if (v * 2 > max) {
                plot(fb, x0 + i, y0 + j, sfg);
            }
        }
    }
    mark_box(fb, x0, y0, x0 + g->width, y0 + g->height);
}

int display_fb_text(display_fb_t *fb, int x, int y, const char *s, size_t len, uint16_t fg, int bg,
                    const display_fb_font_t *f)
{
    const char *p = s, *end = s + len;
    int64_t cx = x;
    int base = y + f->ascent;
    while (p < end) {
        const GFXglyph *g = display_fb_glyph(f, display_fb_utf8(&p, end));
        int adv = display_text_advance(f, g);
        if (cx >= fb->w) {   /* past the right edge: only the width is left to count */
            cx += adv;
            continue;
        }
        if (bg >= 0) {
            display_fb_fill_rect(fb, (int)cx, y, adv, f->line_height, (uint16_t)bg);
        }
        if (g) {
            draw_glyph(fb, (int)cx, base, f, g, fg, bg);
        } else {
            display_fb_rect(fb, (int)cx, y + 1, adv - 1, f->line_height - 2, fg);
        }
        cx += adv;
    }
    return cx - x > INT_MAX ? INT_MAX : (int)(cx - x);
}

int display_fb_text_width(const char *s, size_t len, const display_fb_font_t *f)
{
    const char *p = s, *end = s + len;
    int64_t w = 0;
    while (p < end) {
        const GFXglyph *g = display_fb_glyph(f, display_fb_utf8(&p, end));
        w += display_text_advance(f, g);
    }
    return w > INT_MAX ? INT_MAX : (int)w;
}
