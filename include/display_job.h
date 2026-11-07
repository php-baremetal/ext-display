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
 * The asynchronous flush contract between ext-display and a panel driver. Plain C (no php.h), so the flush
 * engine can be tested on the host. Included by display_panel.h.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "bm_err.h"

#ifndef BM_ERR_TIMEOUT
#define BM_ERR_TIMEOUT 0x107   /* same value as ESP_ERR_TIMEOUT */
#endif

typedef struct display_job display_job_t;
struct display_job {
    int x0, y0, x1, y1;          /* the region, [x0,x1) x [y0,y1) */
    const uint16_t *src;         /* rows of `stride` pixels: the framebuffer, or its copy (buffered) */
    int stride;
    bool direct;                 /* full width and in the panel's order: rows y0..y1 of src go out as they are */
    /* Gathers rows [y0,y1) of the region into out (contiguous, panel order); for a job that is not direct. */
    void (*read)(const display_job_t *job, int y0, int y1, uint8_t *out);
    void *owner;                 /* ext-display's; the driver passes the job back unchanged */
};

/* From the driver, for the job it was given (its own copy): rows below y are on the panel / the job ended. */
void display_rows_sent(const display_job_t *job, int y);
void display_job_done(const display_job_t *job, bm_err_t err, uint32_t us);
