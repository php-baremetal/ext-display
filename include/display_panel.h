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
 * The contract between the drawing surface and a panel driver. See docs/reference/c-api.md.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "php.h"
#include "bm_err.h"
#include "display_job.h"
#include "display_band.h"

#ifndef BM_ERR_INVALID_ARG
#define BM_ERR_INVALID_ARG 0x102   /* same value as ESP_ERR_INVALID_ARG */
#endif

typedef enum { DISPLAY_FORMAT_RGB565 = 1, DISPLAY_FORMAT_MONO1 = 2 } display_format_t;

#define DISPLAY_CAP_HW_SCROLL 0x1u

/* display_band_ops_t, the band path: display_band.h (plain C, without php.h, for renderers built apart). */

typedef struct {
    uint16_t width, height;
    display_format_t format;
    uint32_t caps;
    float refresh_hz;   /* the panel's nominal refresh rate in Hz; 0 when unknown */
    /* Copies the rectangle [x0,x1) x [y0,y1) to the panel; px holds its rows, contiguous, in the panel's format. */
    bm_err_t (*push)(void *panel, int x0, int y0, int x1, int y1, const void *px);
    bm_err_t (*backlight)(void *panel, int level);   /* 0..255; NULL when the panel has no backlight */
    bm_err_t (*sleep)(void *panel, bool on);          /* NULL when there is none */
    /* Asynchronous flush (both NULL for a synchronous panel, which only has push). */
    bool (*begin)(void *panel, const display_job_t *job);   /* copies *job and returns at once; callable from an
                                                               ISR; false when it cannot take it (the job fails) */
    bool (*wait)(void *panel, uint32_t ms);                  /* blocks until a band or a job of this panel ends, or
                                                               ms pass; false on timeout. ext-display calls it in
                                                               a loop with a short ms under its own 2000 ms
                                                               deadline, and ignores the result */
    /* since 1.1, at the end so that a driver built for 1.0 (designated initialisers) leaves it NULL */
    const display_band_ops_t *band;   /* NULL: no band path */
} display_panel_ops_t;

/* Options of display_attach_ex(). */
typedef struct {
    bool buffered;   /* a copy of the framebuffer: flush() copies the region there and the panel reads the copy,
                        so drawings never wait for a flush */
    bool vsync;      /* every region starts at the next TE pulse; needs te */
    int te;          /* GPIO of the panel's TE output; -1: no TE pin */
} display_attach_opts_t;

/* Called by the driver's constructor: binds a surface (framebuffer) to the PHP object. ops and panel must
   outlive the object. Attaching again replaces the surface. With ops->begin the flushes are asynchronous
   (ops->wait is then required); a synchronous panel (no begin) ignores buffered and vsync. opts NULL: no copy,
   no vsync, no TE pin. The TE pin belongs to ext-display: it attaches bm_hal_gpio_on_falling() there (with vsync,
   or at the first onVsync()) and detaches it when the surface goes. BM_ERR_NO_MEM without memory for
   the framebuffer or the copy, BM_ERR_INVALID_ARG for vsync without te, BM_ERR_INVALID_STATE for a zero size,
   a format other than RGB565, or begin without wait. */
bm_err_t display_attach_ex(zend_object *obj, const display_panel_ops_t *ops, void *panel,
                           const display_attach_opts_t *opts);
/* display_attach_ex(obj, ops, panel, NULL). */
bm_err_t display_attach(zend_object *obj, const display_panel_ops_t *ops, void *panel);

/* Waits until no surface attached with this panel has a flush in flight or pending: for the driver, before it
   frees the panel. PHP task only. */
void display_quiesce(void *panel);

/* Marks the whole screen of every surface attached with this panel as changed, so the next flush() sends the
   full frame: for a driver whose panel lost its picture (a re-initialisation on wake, for example). A renderer
   that holds the panel gets DISPLAY_PANEL_LOST (display_claim_ex()). PHP task only. */
void display_invalidate(void *panel);

/* For a renderer that drives the panel itself (Slate), since ext-display 1.1. Gives the panel behind a display
   object: its ops and the driver's panel pointer; *ops is NULL when obj is not a display (or has no surface). */
void display_panel_of(zend_object *obj, const display_panel_ops_t **ops, void **panel);
/* Takes the panel for owner (a short name for messages, e.g. "Slate"; at most 31 bytes are kept): the flushes in
   flight end first (display_quiesce()); from now the drawing methods of the display object, flush() and
   flushWait() throw \LogicException "<Class>: the panel is driven by <owner>; draw in a <owner> Canvas".
   width(), height(), rgb(), textWidth(), backlight(), refreshRate(), sleep(), wake(), onFlushed() and onVsync()
   keep working. false when another owner holds the panel, when obj has no surface, or when the flush in flight
   did not end in time. PHP task only. The renderer holds a reference on obj for as long as it holds the claim
   (ops and panel from display_panel_of() are valid only while obj lives), and a driver does not attach a claimed
   object again (that drops the claim). */
bool display_claim(zend_object *obj, const char *owner);
/* What happened to a claimed panel, for the renderer that holds it (since ext-display 1.1). */
typedef enum {
    DISPLAY_PANEL_SLEEP = 1,   /* sleep() is about to turn the panel off: what is sent until DISPLAY_PANEL_WAKE
                                  may not show (a renderer starts no new frame; one under way may finish) */
    DISPLAY_PANEL_WAKE,        /* wake() turned the panel on again, or a sleep() failed: frames may go again */
    DISPLAY_PANEL_LOST,        /* the panel lost its picture (display_invalidate()): send the whole frame again */
} display_panel_event_t;
/* display_claim() with a callback: notify(arg, event) runs on the PHP task, inside sleep(), wake() and
   display_invalidate(), for as long as the claim lasts (display_release() ends it); it must be short and must not
   call PHP. SLEEP comes before the driver's sleep op; WAKE after a wake op that succeeded (and after a sleep op
   that failed); a driver that re-initialises the panel on wake calls display_invalidate() inside it, so LOST
   comes before WAKE. notify NULL: display_claim(). */
bool display_claim_ex(zend_object *obj, const char *owner, void (*notify)(void *arg, display_panel_event_t event),
                      void *arg);
/* Gives the panel back: the next flush() sends the full frame. */
void display_release(zend_object *obj);

/* For a renderer that paces itself on the panel's TE pulses (Slate), since ext-display 1.1: cb(arg) runs in the TE
   interrupt at every pulse, so it may only do ISR-safe work (e.g. bm_hal_queue_send_isr()); it coexists with
   onVsync() and with the vsync flushes. One hook per display object: a new one replaces the previous, cb NULL
   removes it. Once a call returns, the previous hook is not running and does not run again (the call waits for a
   pulse in progress). The TE interrupt is attached at the first hook and stays attached, as with onVsync(); the
   hook goes with the surface (a new display_attach() of obj, or obj freed). A TE pin has one handler: setting the
   hook takes it for obj, and when another display object on the same pin takes it later (its first onVsync(), or
   an attach with vsync) the hook stops getting pulses; set it again to take the pin back. false when obj is not
   a display or the display has no TE pin. PHP task only. */
bool display_vsync_hook(zend_object *obj, void (*cb)(void *arg), void *arg);

/* The methods of Output\Display, for the driver class's method table (terminated by ZEND_FE_END). */
extern const zend_function_entry display_methods[];

/* The same methods as initialisers, for a static method table:
   static const zend_function_entry my_methods[] = { PHP_ME(...) DISPLAY_FE PHP_FE_END }; */
#define DISPLAY_METHOD_LIST(M)                                                                       \
    M(width, size) M(height, size) M(rgb, rgb) M(clear, clear) M(pixel, pixel) M(line, line)        \
    M(rect, rect) M(fillRect, rect) M(circle, circle) M(fillCircle, circle) M(text, text)            \
    M(textWidth, text_width) M(image, image) M(flush, flush) M(flushWait, void)                      \
    M(backlight, backlight) M(sleep, void) M(wake, void) M(refreshRate, rate)                        \
    M(onFlushed, on_event) M(onVsync, on_event)

#define DISPLAY_DECLARE_METHOD(name, args) ZEND_METHOD(Display, name);
DISPLAY_METHOD_LIST(DISPLAY_DECLARE_METHOD)
#undef DISPLAY_DECLARE_METHOD

/* Sizes are 1 + the number of arguments. */
extern const zend_internal_arg_info display_arginfo_size[1], display_arginfo_rgb[4], display_arginfo_clear[2],
    display_arginfo_pixel[4], display_arginfo_line[6], display_arginfo_rect[6], display_arginfo_circle[5],
    display_arginfo_text[7], display_arginfo_text_width[3], display_arginfo_image[6], display_arginfo_flush[5],
    display_arginfo_backlight[2], display_arginfo_void[1], display_arginfo_rate[1],
    display_arginfo_on_event[2];

#define DISPLAY_FE_ENTRY(name, args) ZEND_FENTRY(name, ZEND_MN(Display_##name), display_arginfo_##args, ZEND_ACC_PUBLIC)
#define DISPLAY_FE DISPLAY_METHOD_LIST(DISPLAY_FE_ENTRY)

/* own (terminated by ZEND_FE_END) followed by display_methods, in a table that lives until the process
   ends: for a driver whose method table is built at startup. */
const zend_function_entry *display_methods_with(const zend_function_entry *own);

/* Baremetal\Output\Display, for zend_class_implements(). */
zend_class_entry *display_ce(void);

/* The short class name of obj ("St77916" for Baremetal\Spi\Driver\St77916), for messages. */
const char *display_panel_name(zend_object *obj);
