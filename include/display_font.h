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
 * Bitmap font formats read by ext-display and the display_fonts[] table the SDK generates (fonts.c).
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* Adafruit GFX (fontconvert output), read as is. */
typedef struct { uint16_t bitmapOffset; uint8_t width, height, xAdvance; int8_t xOffset, yOffset; } GFXglyph;
typedef struct { uint8_t *bitmap; GFXglyph *glyph; uint16_t first, last; uint8_t yAdvance; } GFXfont;

/* php-baremetal extension: several codepoint ranges and 1, 2 or 4 bits per pixel. */
typedef struct { uint32_t first, last; uint16_t glyph; } display_font_range_t;   /* glyph = index of `first` */
typedef struct {
    const uint8_t *bitmap; const GFXglyph *glyph; const display_font_range_t *ranges;
    uint16_t range_count; uint8_t bpp; uint8_t line_height; uint8_t ascent;
} display_font_t;

/* One font of the firmware: exactly one of gfx (const GFXfont *) and ext (const display_font_t *) is set.
   The generated fonts.c defines the struct too, under the same guard. */
#ifndef DISPLAY_FONT_ENTRY_DEFINED
#define DISPLAY_FONT_ENTRY_DEFINED
struct display_font_entry { const char *name; const void *gfx; const void *ext; };
#endif
extern const struct display_font_entry display_fonts[];
extern const size_t display_fonts_count;
