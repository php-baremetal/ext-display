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

#include <string.h>

#include "display_flush.h"

bool display_rect_empty(display_rect_t r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }

void display_rect_union(display_rect_t *a, display_rect_t b)
{
    if (display_rect_empty(b)) return;
    if (display_rect_empty(*a)) { *a = b; return; }
    if (b.x0 < a->x0) a->x0 = b.x0;
    if (b.y0 < a->y0) a->y0 = b.y0;
    if (b.x1 > a->x1) a->x1 = b.x1;
    if (b.y1 > a->y1) a->y1 = b.y1;
}

static bool overlap(display_rect_t a, display_rect_t b)
{
    return !display_rect_empty(a) && !display_rect_empty(b) && a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 &&
           b.y0 < a.y1;
}

/* Under the lock. Returns the job to hand to begin() once the lock is released. */
static display_job_t start_locked(display_flush_t *f, display_rect_t r)
{
    memset(&f->job, 0, sizeof f->job);
    f->job.x0 = r.x0; f->job.y0 = r.y0; f->job.x1 = r.x1; f->job.y1 = r.y1;
    f->job.owner = f;
    f->prepare(f, &f->job);
    f->inflight = true;
    f->sent_y = r.y0;
    return f->job;
}

/* Under the lock: keeps err (the first one) and adds r to the regions failed since PHP last took the error. */
static void fail_locked(display_flush_t *f, bm_err_t err, display_rect_t r)
{
    if (f->err == BM_OK) { f->err = err; f->err_r = r; }
    else display_rect_union(&f->err_r, r);
}

/* begin() refused the job: it never ran. */
static void begin_failed(display_flush_t *f, display_rect_t r)
{
    bm_hal_lock_enter(f->lock);
    f->inflight = false;
    fail_locked(f, BM_ERR_INVALID_STATE, r);
    bm_hal_lock_exit(f->lock);
}

/* Outside the lock, with f->busy held by the caller. */
static void run_begin(display_flush_t *f, const display_job_t *job)
{
    display_rect_t r = {job->x0, job->y0, job->x1, job->y1};
    if (!f->begin(f->panel, job)) begin_failed(f, r);
}

/* Ends a stretch of callbacks begun under the lock with f->busy++; f may be freed as soon as this returns. */
static void busy_done(display_flush_t *f)
{
    bm_hal_lock_enter(f->lock);
    f->busy--;
    bm_hal_lock_exit(f->lock);
}

bool display_flush_conflict(const display_flush_t *f, display_rect_t r)
{
    display_rect_t unsent = {f->job.x0, f->sent_y, f->job.x1, f->job.y1};
    return (f->inflight && overlap(r, unsent)) || overlap(r, f->pend);
}

void display_flush_submit(display_flush_t *f, display_rect_t r)
{
    if (display_rect_empty(r)) return;
    display_job_t job;
    bm_hal_lock_enter(f->lock);
    if (f->inflight || f->vsync) {
        display_rect_union(&f->pend, r);
        bm_hal_lock_exit(f->lock);
        return;
    }
    job = start_locked(f, r);
    f->busy++;
    bm_hal_lock_exit(f->lock);
    run_begin(f, &job);
    busy_done(f);
}

void display_rows_sent(const display_job_t *job, int y)
{
    display_flush_t *f = job->owner;
    bm_hal_lock_enter(f->lock);
    f->sent_y = y;
    bm_hal_lock_exit(f->lock);
}

void display_job_done(const display_job_t *job, bm_err_t err, uint32_t us)
{
    display_flush_t *f = job->owner;
    display_rect_t done = {job->x0, job->y0, job->x1, job->y1};
    display_job_t next;
    bool start = false;

    bm_hal_lock_enter(f->lock);
    f->inflight = false;
    if (err != BM_OK) fail_locked(f, err, done);
    if (!f->vsync && !display_rect_empty(f->pend)) {
        display_rect_t p = f->pend;
        f->pend = (display_rect_t){0, 0, 0, 0};
        next = start_locked(f, p);
        start = true;
    }
    f->busy++;   /* not idle until the callbacks return: the surface must outlive them */
    bm_hal_lock_exit(f->lock);

    if (f->finished) f->finished(f, done, err, us);
    if (start) run_begin(f, &next);
    busy_done(f);
}

bool display_flush_idle(display_flush_t *f)
{
    bm_hal_lock_enter(f->lock);
    bool idle = !f->inflight && display_rect_empty(f->pend) && f->busy == 0;
    bm_hal_lock_exit(f->lock);
    return idle;
}

bm_err_t display_flush_take_error(display_flush_t *f, display_rect_t *r)
{
    bm_hal_lock_enter(f->lock);
    bm_err_t e = f->err;
    if (r) *r = f->err_r;
    f->err = BM_OK;
    bm_hal_lock_exit(f->lock);
    return e;
}

void display_flush_drop_pending(display_flush_t *f)
{
    bm_hal_lock_enter(f->lock);
    f->pend = (display_rect_t){0, 0, 0, 0};
    bm_hal_lock_exit(f->lock);
}

void display_flush_te(display_flush_t *f)
{
    display_job_t job;
    bool start = false;

    bm_hal_lock_enter(f->lock);
    if (f->vsync && !f->inflight && !display_rect_empty(f->pend)) {
        display_rect_t p = f->pend;
        f->pend = (display_rect_t){0, 0, 0, 0};
        job = start_locked(f, p);
        start = true;
    }
    f->busy++;
    bm_hal_lock_exit(f->lock);

    /* The onVsync() event first: its $time is the pulse, before begin() and the driver's queue send. */
    if (f->te_event) f->te_event(f);
    if (start) run_begin(f, &job);
    busy_done(f);
}
