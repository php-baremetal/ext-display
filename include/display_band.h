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
 * The band path of the panel contract (display_panel_ops_t::band), plain C: display_panel.h includes it, and a
 * renderer's C core (built and tested without PHP) can include it alone. See docs/reference/c-api.md.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bm_err.h"

/* The band path, since ext-display 1.1: an optional way to send a picture composed outside the framebuffer, in
   bands of whole rows. A renderer that drives the panel itself (see display_claim()) uses it; the surface's own
   flush() does not. Rules, for the driver and for the renderer:
   - Threading: begin, push, end, buf_alloc and buf_free are called by one task of the renderer (never an ISR), never
     concurrently with each other.
   - Coordinates: the region goes to begin in panel coordinates; the y0 of push is a panel row inside the region;
     every band spans the region's full width (x1 - x0), rows go top to bottom, and rows * (x1 - x0) * 2 never
     exceeds bytes_max.
   - Pipelining: push may be called again before the previous done. The driver accepts at least 2 bands in
     flight; when it cannot take one more it blocks inside push until it can. done(arg) runs exactly once for
     every accepted push, in push order, in a task (never an ISR: a driver whose DMA ends in an ISR defers done
     to a task), and must be short.
   - end returns once every band of the region has been accepted; it does not wait for done. The next begin may
     follow at once; regions are processed in order.
   - A push that returns an error did not take the band: done is NOT called and px is free at once.
   - A panel that failed for good (a transfer that never ended, for example) makes every later begin and push return
     BM_ERR_INVALID_STATE, and nothing else does: the renderer takes that code as final and stops sending (Slate
     fails with an error). Any other error is transient: the renderer may send the band again later.
   - The band path does not wait for TE: a renderer paces itself on the TE pulses (display_vsync_hook() in
     display_panel.h, or onVsync()).
   - own_ram false: the driver copies the bands into its own frame; the picture may appear at the driver's next
     frame (a buffer swap at the TE pulse, for example) after end.
   - sleep, wake and backlight of the panel may be called while a renderer holds the panel; a driver with a band
     path executes sleep and wake after the bands it already accepted (same order). ext-display does not wait
     for the renderer's bands; it tells the renderer that holds the panel (display_claim_ex() in display_panel.h)
     before sleep and after wake. A panel that loses its picture (a re-initialisation on wake, for example): the
     driver calls display_invalidate(), which tells that renderer to send the whole frame again; a driver that
     does not call it keeps what the bands wrote, also while the panel sleeps.
   - scan: the order in which the panel refreshes its picture from its RAM, in panel coordinates (the driver
     derives it from its orientation). A renderer that paces itself on TE uses it to write each band where the
     refresh is not reading ("racing the beam"), so the picture does not tear; DISPLAY_SCAN_UNKNOWN (0, the value of
     a driver that does not set it) means it does not. The driver still never waits for TE: the waiting is the
     renderer's. */
typedef enum {
    DISPLAY_SCAN_UNKNOWN = 0,
    DISPLAY_SCAN_TOP_DOWN,     /* row 0 first; the refresh starts at the TE pulse (its falling edge) */
    DISPLAY_SCAN_BOTTOM_UP,    /* the last row first */
    DISPLAY_SCAN_LEFT_RIGHT,   /* column 0 first (a panel turned by 90 degrees) */
    DISPLAY_SCAN_RIGHT_LEFT,   /* the last column first */
} display_scan_t;

typedef struct {
    uint16_t rows;          /* rows per band the driver advises (e.g. 40) */
    uint32_t bytes_max;     /* largest band the driver accepts, in bytes */
    bool own_ram;           /* the panel keeps its picture (ST77916); false: the driver copies bands into its frame */
    display_scan_t scan;    /* the refresh order (see above); DISPLAY_SCAN_UNKNOWN when not set */
    /* The buffers of the bands come from the driver, which picks DMA-capable internal RAM and its alignment.
     * bytes <= bytes_max; NULL when there is no memory. buf_free() takes back a buffer from buf_alloc(), once its done
     * ran (or its push failed). */
    void *(*buf_alloc)(void *panel, size_t bytes);
    void (*buf_free)(void *panel, void *px);
    /* A region [x0,x1) x [y0,y1), in panel coordinates, is about to be sent in bands, top to bottom. */
    bm_err_t (*begin)(void *panel, int x0, int y0, int x1, int y1);
    /* Sends rows [y0, y0+rows) (panel rows, inside the region) from px (a buffer of buf_alloc(); RGB565, high byte
     * first, contiguous rows of x1-x0 pixels). Returns once the band is accepted; done(arg) runs when px may be
     * reused or freed. On an error nothing was taken: no done (BM_ERR_INVALID_STATE: the panel failed for good). */
    bm_err_t (*push)(void *panel, int y0, int rows, const void *px, void (*done)(void *arg), void *arg);
    bm_err_t (*end)(void *panel);   /* every band of the region has been given to push */
} display_band_ops_t;
