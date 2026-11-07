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

/* The flush engine: starts, merges and tracks the asynchronous flushes of one surface. Plain C. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "bm_events_hal.h"
#include "display_job.h"

typedef struct { int x0, y0, x1, y1; } display_rect_t;      /* same layout as display_fb_rect_t */

typedef struct display_flush display_flush_t;
struct display_flush {
    bm_hal_lock_t *lock;
    void *panel;
    bool (*begin)(void *panel, const display_job_t *job);
    void (*prepare)(display_flush_t *f, display_job_t *job);   /* fills src/stride/direct/read for job's region */
    void (*finished)(display_flush_t *f, display_rect_t r, bm_err_t err, uint32_t us);   /* outside the lock */
    void (*te_event)(display_flush_t *f);                       /* from the TE ISR, outside the lock */
    void *ctx;                                                  /* the surface */
    bool vsync;
    bool inflight;
    int busy;                     /* callbacks running outside the lock (begin, finished, te_event): not idle */
    display_job_t job;            /* the job in flight; job.owner == this */
    int sent_y;
    display_rect_t pend;          /* empty when x0 >= x1 */
    bm_err_t err;                 /* BM_OK, or the first error not yet thrown */
    display_rect_t err_r;
};

bool display_rect_empty(display_rect_t r);
void display_rect_union(display_rect_t *a, display_rect_t b);
/* Under the lock: whether r overlaps rows not yet sent of the job in flight, or the pending region. */
bool display_flush_conflict(const display_flush_t *f, display_rect_t r);
/* Starts r, or merges it into the pending region (in flight, or vsync). */
void display_flush_submit(display_flush_t *f, display_rect_t r);
/* Nothing in flight, pending, or running a callback: the surface can be freed. */
bool display_flush_idle(display_flush_t *f);
/* BM_OK, or the error of a failed job (cleared) and its region in *r. */
bm_err_t display_flush_take_error(display_flush_t *f, display_rect_t *r);
void display_flush_te(display_flush_t *f);                   /* the TE ISR */
/* Forgets the pending region (the job in flight goes on): for a surface that goes away, or a vsync panel put to
   sleep or woken, whose vsync region would otherwise wait for a TE pulse that may never come. */
void display_flush_drop_pending(display_flush_t *f);
