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
 * display extension module: at MINIT it registers Baremetal\Output\Display and Baremetal\Display\Font.
 * The surfaces of the driver objects live in a table keyed by object (a weak reference: the surface is
 * freed with its object, and whatever is left at MSHUTDOWN, after its flush in flight ends).
 */
#include "php_display.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zend_exceptions.h"
#include "zend_weakrefs.h"
#include "ext/spl/spl_exceptions.h"
#include "event_bus_php.h"

static HashTable s_surfaces;
static bool s_replacing;   /* display_attach_ex() is freeing the old surface of a live object */

/* The locks of the surfaces freed: events-hal has no call to destroy a lock, so a new surface reuses one instead
   of creating it (a runtime that builds a display per request stays bounded by the surfaces alive at once). */
static bm_hal_lock_t **s_free_locks;
static size_t s_free_n, s_free_cap;

static bm_hal_lock_t *lock_get(void)
{
    return s_free_n > 0 ? s_free_locks[--s_free_n] : bm_hal_lock_create();
}

static void lock_put(bm_hal_lock_t *lock)
{
    if (!lock) {
        return;
    }
    if (s_free_n == s_free_cap) {
        size_t cap = s_free_cap ? s_free_cap * 2 : 4;
        bm_hal_lock_t **a = realloc(s_free_locks, cap * sizeof *a);
        if (!a) {
            return;   /* no memory for the list: this lock is leaked, as before the list */
        }
        s_free_locks = a;
        s_free_cap = cap;
    }
    s_free_locks[s_free_n++] = lock;
}

bool display_wait_for(display_surface_t *s, bool (*done)(display_surface_t *s, const void *arg), const void *arg)
{
    /* One deadline for the whole wait: a vsync region waits for the TE with no job in flight, and the driver's
       wait() may return at once then; a dead TE pin still ends here. */
    int64_t deadline = bm_hal_now_us() + (int64_t)DISPLAY_WAIT_MS * 1000;
    for (;;) {
        if (done(s, arg)) {
            return true;
        }
        if (bm_hal_now_us() >= deadline) {
            return false;
        }
        (void)s->ops->wait(s->panel, DISPLAY_WAIT_SLICE_MS);
    }
}

static bool surface_idle(display_surface_t *s, const void *arg)
{
    (void)arg;
    return !s->async || display_flush_idle(&s->flush);
}

/* Waits until s has no flush in flight or pending, without throwing: false on the deadline. */
static bool wait_idle_quiet(display_surface_t *s)
{
    return display_wait_for(s, surface_idle, NULL);
}

/* For a surface that goes or a panel about to be freed: a vsync region still pending would only start at a TE
   pulse, so it is dropped (it will never be shown); then the job in flight ends. */
static bool quiesce(display_surface_t *s)
{
    if (s->async && s->flush.vsync) {
        display_flush_drop_pending(&s->flush);
    }
    return wait_idle_quiet(s);
}

bool display_wait_idle(display_surface_t *s)
{
    if (!wait_idle_quiet(s)) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "%s: flush timed out", display_panel_name(s->obj));
        return false;
    }
    return true;
}

void display_flushed(display_flush_t *f, display_rect_t r, bm_err_t err, uint32_t us)
{
    /* The region already left the dirty rectangle, and the engine keeps a failure for
       display_flush_take_error(): only the onFlushed() event is left. */
    display_events_flushed(f->ctx, r, err, us);
}

static void surface_free(zval *zv)
{
    display_surface_t *s = Z_PTR_P(zv);
    display_events_detach(s);   /* no more events raised from here on */
    if (s->async) {
        /* Any async surface, vsync or not: what has not started is never shown. Without vsync a pending region
           would start when the job in flight ends; dropping it only makes the wait shorter. */
        display_flush_drop_pending(&s->flush);
    }
    bool idle = wait_idle_quiet(s);              /* the job in flight; the TE is still attached meanwhile */
    display_te_detach(s);
    if (!idle) {
        /* The driver timed out with a flush still running: it may still write through job->owner (s->flush)
           and its DMA may still read the framebuffer or the copy. Leaked on purpose: a leak, not a corruption. */
        return;
    }
    while (s->flush.lock && !display_flush_idle(&s->flush)) {
        /* a TE pulse handler that started before the detach is still running (it starts nothing: no region is
           pending) */
    }
    if (s->obj_held && s_replacing) {
        /* Attached again: the object lives on, its reference goes after the events already queued with it. When
           the object itself is being freed (or at shutdown) there is nothing left to give back. */
        bm_events_release_later(s->obj);
    }
    free(s->fb.px);
    free(s->copy);
    free(s->band);
    lock_put(s->flush.lock);   /* nothing uses it any more: the TE handler and the driver's job are gone */
    free(s);
}

void display_surfaces_startup(void)
{
    zend_hash_init(&s_surfaces, 4, NULL, surface_free, 1);
}

void display_surfaces_shutdown(void)
{
    zend_hash_destroy(&s_surfaces);   /* the free locks stay listed: a later MINIT reuses them */
}

const char *display_panel_name(zend_object *obj)
{
    const char *name = ZSTR_VAL(obj->ce->name);
    const char *sep = strrchr(name, '\\');
    return sep ? sep + 1 : name;
}

/* The job's rows [y0,y1), gathered in the panel's order; for the driver, any task. Reads only the job. */
static void surface_read(const display_job_t *job, int y0, int y1, uint8_t *out)
{
    display_fb_t src = { .px = (uint16_t *)job->src, .w = job->stride };
    display_fb_emit(&src, job->x0, y0, job->x1, y1, out);
}

/* Under the engine's lock: where the panel reads the job's region from. */
static void surface_prepare(display_flush_t *f, display_job_t *job)
{
    display_surface_t *s = f->ctx;
    job->src = s->copy ? s->copy : s->fb.px;
    job->stride = s->fb.w;
    job->direct = display_fb_direct(&s->fb, job->x0, job->y0, job->x1) != NULL;
    job->read = surface_read;
}

bm_err_t display_attach_ex(zend_object *obj, const display_panel_ops_t *ops, void *panel,
                           const display_attach_opts_t *opts)
{
    if (!obj || !ops || !ops->push || ops->width == 0 || ops->height == 0 || ops->format != DISPLAY_FORMAT_RGB565 ||
        (ops->begin && !ops->wait)) {
        return BM_ERR_INVALID_STATE;
    }
    display_attach_opts_t o = opts ? *opts : (display_attach_opts_t){ .buffered = false, .vsync = false, .te = -1 };
    if (o.vsync && o.te < 0) {
        return BM_ERR_INVALID_ARG;
    }
    bool async = ops->begin != NULL;
    bool buffered = async && o.buffered;   /* a synchronous panel pushes inside flush(): nothing to copy for */
    size_t pixels = (size_t)ops->width * ops->height;
    size_t band_px = DISPLAY_BAND_BYTES / 2 < pixels ? DISPLAY_BAND_BYTES / 2 : pixels;
    if (band_px < ops->width) {
        band_px = ops->width;
    }
    /* malloc, not emalloc: the framebuffer and its copy stay out of memory_limit and outlive the request's heap */
    display_surface_t *s = calloc(1, sizeof *s);
    uint16_t *px = calloc(pixels, sizeof(uint16_t));
    uint16_t *copy = buffered ? calloc(pixels, sizeof(uint16_t)) : NULL;
    uint8_t *band = malloc(band_px * 2);
    bool locked = async || o.te >= 0;   /* the engine, or the TE interrupt of onVsync() */
    bm_hal_lock_t *lock = locked ? lock_get() : NULL;
    if (!s || !px || !band || (buffered && !copy) || (locked && !lock)) {
        free(s);
        free(px);
        free(copy);
        free(band);
        lock_put(lock);
        return BM_ERR_NO_MEM;
    }
    s->ops = ops;
    s->panel = panel;
    s->obj = obj;
    s->band = band;
    s->band_px = band_px;
    s->async = async;
    s->buffered = buffered;
    s->copy = copy;
    s->te = o.te;
    display_fb_init(&s->fb, px, ops->width, ops->height);
    s->flush.lock = lock;
    s->flush.panel = panel;
    s->flush.begin = ops->begin;
    s->flush.prepare = surface_prepare;
    s->flush.finished = display_flushed;
    s->flush.te_event = display_te_event;
    s->flush.ctx = s;
    s->flush.vsync = async && o.vsync;   /* a synchronous panel ignores vsync (it has no begin) */
    s_replacing = true;
    zend_weakrefs_hash_del(&s_surfaces, obj);   /* the old surface detaches its TE pin before this one takes it */
    s_replacing = false;
    if (!zend_weakrefs_hash_add_ptr(&s_surfaces, obj, s)) {
        free(px);
        free(copy);
        free(band);
        lock_put(lock);
        free(s);
        return BM_ERR_INVALID_STATE;
    }
    if (async && o.vsync) {
        display_te_attach(s);
    }
    return BM_OK;
}

bm_err_t display_attach(zend_object *obj, const display_panel_ops_t *ops, void *panel)
{
    return display_attach_ex(obj, ops, panel, NULL);
}

void display_quiesce(void *panel)
{
    display_surface_t *s;
    ZEND_HASH_FOREACH_PTR(&s_surfaces, s) {
        if (s->panel == panel) {
            quiesce(s);
        }
    } ZEND_HASH_FOREACH_END();
}

void display_invalidate(void *panel)
{
    display_surface_t *s;
    ZEND_HASH_FOREACH_PTR(&s_surfaces, s) {
        if (s->panel == panel) {
            display_fb_mark(&s->fb, 0, 0, s->fb.w, s->fb.h);
            display_notify(s, DISPLAY_PANEL_LOST);   /* a renderer holds the panel: it sends the whole frame */
        }
    } ZEND_HASH_FOREACH_END();
}

void display_notify(display_surface_t *s, display_panel_event_t event)
{
    if (s->owner[0] && s->notify) {
        s->notify(s->notify_arg, event);
    }
}

display_surface_t *display_surface(zend_object *obj)
{
    display_surface_t *s = display_surface_find(obj);
    if (!s) {
        zend_throw_error(NULL, "%s is not initialised", display_panel_name(obj));
    }
    return s;
}

display_surface_t *display_surface_find(zend_object *obj)
{
    return obj ? zend_hash_index_find_ptr(&s_surfaces, zend_object_to_weakref_key(obj)) : NULL;
}

void display_panel_of(zend_object *obj, const display_panel_ops_t **ops, void **panel)
{
    display_surface_t *s = display_surface_find(obj);
    *ops = s ? s->ops : NULL;
    *panel = s ? s->panel : NULL;
}

bool display_claim(zend_object *obj, const char *owner)
{
    return display_claim_ex(obj, owner, NULL, NULL);
}

bool display_claim_ex(zend_object *obj, const char *owner, void (*notify)(void *arg, display_panel_event_t event),
                      void *arg)
{
    display_surface_t *s = owner && *owner ? display_surface_find(obj) : NULL;
    if (!s || s->owner[0]) {
        return false;
    }
    /* The flush in flight ends first (a vsync region still waiting for its TE pulse is dropped: it would draw over
       the renderer). Nothing is claimed when the driver does not finish. */
    if (!quiesce(s)) {
        display_fb_mark(&s->fb, 0, 0, s->fb.w, s->fb.h);   /* a dropped vsync region is sent again by the next flush() */
        return false;
    }
    if (s->async) {
        /* A failed push from before the claim would otherwise be thrown at the first drawing after the release:
           its region is sent again by that flush instead. */
        display_rect_t r;
        if (display_flush_take_error(&s->flush, &r) != BM_OK) {
            display_fb_mark(&s->fb, r.x0, r.y0, r.x1, r.y1);
        }
    }
    snprintf(s->owner, sizeof s->owner, "%s", owner);
    s->notify = notify;
    s->notify_arg = arg;
    return true;
}

void display_release(zend_object *obj)
{
    display_surface_t *s = display_surface_find(obj);
    if (s && s->owner[0]) {
        s->owner[0] = '\0';
        s->notify = NULL;
        s->notify_arg = NULL;
        display_fb_mark(&s->fb, 0, 0, s->fb.w, s->fb.h);   /* the renderer changed the picture: send it all */
    }
}

bool display_not_claimed(const display_surface_t *s)
{
    if (!s->owner[0]) {
        return true;
    }
    zend_throw_exception_ex(spl_ce_LogicException, 0, "%s: the panel is driven by %s; draw in a %s Canvas",
        display_panel_name(s->obj), s->owner, s->owner);
    return false;
}

PHP_MINIT_FUNCTION(display)
{
    display_surfaces_startup();
    display_interface_register();
    display_events_register();
    display_font_class_register();
    return SUCCESS;
}

PHP_MSHUTDOWN_FUNCTION(display)
{
    display_surfaces_shutdown();
    display_events_shutdown();
    return SUCCESS;
}

static const zend_function_entry display_functions[] = {
    PHP_FE_END
};

zend_module_entry display_module_entry = {
    STANDARD_MODULE_HEADER,
    "display",
    display_functions,
    PHP_MINIT(display),
    PHP_MSHUTDOWN(display),
    NULL,   /* RINIT */
    NULL,   /* RSHUTDOWN */
    NULL,   /* MINFO */
    "1.1.0",
    STANDARD_MODULE_PROPERTIES
};
