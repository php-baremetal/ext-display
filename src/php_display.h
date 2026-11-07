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
 * Internal glue for the display extension: the surface bound to a driver object, the Font class and the
 * registration entry points called from MINIT. Internal to the package (src/); drivers include
 * display_panel.h.
 */
#pragma once

#include "php.h"
#include "display_panel.h"
#include "display_fb.h"
#include "display_flush.h"

/* Bytes of the band that collects a region for the panel; a region is pushed in bands of whole rows. */
#ifndef DISPLAY_BAND_BYTES
#define DISPLAY_BAND_BYTES 14400
#endif

extern zend_module_entry display_module_entry;

/* How long a drawing, flushWait(), sleep(), wake() or backlight() waits in all for the flush before it throws
   "<Class>: flush timed out" (measured with bm_hal_now_us()), and the ms given to each ops->wait() meanwhile. */
#define DISPLAY_WAIT_MS 2000
#define DISPLAY_WAIT_SLICE_MS 20

typedef struct display_surface display_surface_t;
struct display_surface {
    const display_panel_ops_t *ops;
    void *panel;
    zend_object *obj;    /* the driver object (the key of the surface table) */
    display_fb_t fb;
    uint8_t *band;      /* DISPLAY_BAND_BYTES, or less for a small screen */
    size_t band_px;
    bool async;          /* ops->begin: flushes go through the engine below */
    display_flush_t flush;
    bool buffered;
    uint16_t *copy;      /* buffered: what the panel reads, w*h pixels like fb.px; NULL otherwise */
    int te;              /* GPIO of the TE pin, -1 without one */
    bool te_on;          /* the surface wants the TE pulses (vsync, or the first onVsync()) */
    bool te_owner;       /* the pin's interrupt handler is this surface's: one handler per pin, the last to attach */
    display_surface_t *te_next;   /* the list of the surfaces with te_on (php_display_events.c) */
    bool obj_held;       /* a reference on obj, taken by the first onFlushed()/onVsync(): queued events carry it */
    struct display_src_rec *flushed_rec;   /* the active record of onFlushed(), NULL without one */
    struct display_src_rec *vsync_rec;     /* the active record of onVsync(), NULL without one */
};

/* Waits until done(s, arg) holds, calling ops->wait(panel, DISPLAY_WAIT_SLICE_MS) in between (its result is
   ignored), for at most DISPLAY_WAIT_MS in all; false on the deadline, without throwing. */
bool display_wait_for(display_surface_t *s, bool (*done)(display_surface_t *s, const void *arg), const void *arg);
/* Waits until the surface has no flush in flight or pending; false (RuntimeException thrown) on a timeout. */
bool display_wait_idle(display_surface_t *s);
/* The end of a region sent to the panel, asynchronous or not (the engine's finished callback). */
void display_flushed(display_flush_t *f, display_rect_t r, bm_err_t err, uint32_t us);

/* The surface of a driver object; throws Error "<Class> is not initialised" and returns NULL without one. */
display_surface_t *display_surface(zend_object *obj);

void display_surfaces_startup(void);
void display_surfaces_shutdown(void);

/* php_display_events.c: Baremetal\Display\Flushed, FlushSource, VsyncSource (after display_interface_register()). */
void display_events_register(void);
/* MSHUTDOWN, after the surfaces: frees the records of onFlushed() and onVsync(). */
void display_events_shutdown(void);
/* The surface's TE interrupt, attached once (vsync, or the first onVsync()). The pin has one handler: the
   surface takes it from another surface on the same pin. */
void display_te_attach(display_surface_t *s);
/* The surface goes: no more events from it, and its records stop pointing at it. Before it is quiesced. */
void display_events_detach(display_surface_t *s);
/* Detaches the TE interrupt (after the surface is quiesced: a vsync region still needs the pulses until then).
   When another live surface wants the same pin, the handler goes to it; the pin is released only when none does. */
void display_te_detach(display_surface_t *s);
/* Engine hooks: the end of a region (raises the onFlushed() event) and a TE pulse (the onVsync() event). */
void display_events_flushed(display_surface_t *s, display_rect_t r, bm_err_t err, uint32_t us);
void display_te_event(display_flush_t *f);

void display_interface_register(void);   /* Baremetal\Output\Display */
void display_font_class_register(void);  /* Baremetal\Display\Font */

extern zend_class_entry *display_font_ce;
/* The font of a Font object, or the default one for NULL; throws RuntimeException and returns NULL when
   there is no default font. */
const display_fb_font_t *display_font_of(zend_object *font);
