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
 * The events of a display: onFlushed() (the end of every region sent, Baremetal\Display\Flushed) and onVsync()
 * (every TE pulse, coalescing, Baremetal\TimerEvent), with their Baremetal\Source handles, and the TE interrupt:
 * a pin's, or the pulses a driver gives with display_te_pulse() (te_driver, since 1.2).
 *
 * Each call makes a record (persistent, freed at MSHUTDOWN like the records of ext-events): the tag, whether it
 * is on, and the surface it belongs to. The surface points at its active record of each kind; a second call
 * replaces it, so the old handle reads running() false. The driver task (finished) and the TE interrupt read the
 * active record without the PHP task, so a record is never freed before MSHUTDOWN.
 *
 * Queued events carry the driver object as $e->device: the first call takes a reference on it, kept until the
 * surface goes (as ext-i2c keeps its emit object), and given back after the events already queued.
 */
#include "php_display.h"

#include <stdatomic.h>
#include <string.h>

#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"
#include "event_bus_php.h"

typedef struct display_src_rec display_src_rec_t;
struct display_src_rec {
    display_surface_t *s;   /* NULL once the surface is gone */
    uint16_t tag;
    bool vsync;             /* onVsync() (else onFlushed()) */
    atomic_bool on;
    display_src_rec_t *next;
};

static display_src_rec_t *s_recs;   /* every record, freed at MSHUTDOWN */

static zend_class_entry *s_flushed_ce, *s_flush_source_ce, *s_vsync_source_ce;

/* What bm_events_emit_data() carries for a Flushed event: the region and the transfer time (12 bytes; int16_t
   coordinates are enough for any panel up to 32767 pixels a side). */
typedef struct {
    int16_t x0, y0, x1, y1;
    uint32_t us;
} display_flushed_data_t;

/* ---- the records and their handles ---- */

static display_src_rec_t *active_of(const display_src_rec_t *rec)
{
    display_surface_t *s = rec->s;
    if (!s) {
        return NULL;
    }
    return __atomic_load_n(rec->vsync ? &s->vsync_rec : &s->flushed_rec, __ATOMIC_ACQUIRE);
}

static void src_stop(void *r)
{
    atomic_store(&((display_src_rec_t *)r)->on, false);   /* the TE interrupt stays: vsync may need it */
}

static void src_start(void *r)
{
    atomic_store(&((display_src_rec_t *)r)->on, true);
}

static bool src_running(void *r)
{
    display_src_rec_t *rec = r;
    return atomic_load(&rec->on) && active_of(rec) == rec;
}

/* No release: the record belongs to the display, a discarded handle leaves the event on. */
static const bm_source_ops_t s_src_ops = { src_stop, src_start, src_running, NULL };

/* ---- fillers ---- */

/* v as a PHP int, saturated at LONG_MAX (the board's long is 32-bit; the PC's is 64-bit, where nothing saturates). */
static long sat_long(uint64_t v)
{
    return v > (uint64_t)LONG_MAX ? LONG_MAX : (long)v;
}

static void flushed_fill(zend_object *event, zend_object *device, const evt_t *slot)
{
    (void)device;
    size_t len;
    const void *p = bm_events_slot_data(slot, &len);
    display_flushed_data_t d;
    if (!p || len != sizeof d) {
        return;
    }
    memcpy(&d, p, sizeof d);   /* the slot data is only 4-byte aligned on the board: never cast it */
    bm_events_set_long(event, s_flushed_ce, "x0", d.x0);
    bm_events_set_long(event, s_flushed_ce, "y0", d.y0);
    bm_events_set_long(event, s_flushed_ce, "x1", d.x1);
    bm_events_set_long(event, s_flushed_ce, "y1", d.y1);
    bm_events_set_long(event, s_flushed_ce, "us", sat_long(d.us));
}

/* TimerEvent::$ticks: this pulse plus the pulses coalesced into the event (ext-events registers its own filler
   only for the tags of every()). Saturates at LONG_MAX, the bound of the board's 32-bit long. */
static void vsync_fill(zend_object *event, zend_object *device, const evt_t *slot)
{
    (void)device;
    uint64_t n = (uint64_t)1 + slot->merged;
    bm_events_set_long(event, bm_events_timer_ce(), "ticks", sat_long(n));
}

/* ---- the hooks ---- */

void display_events_flushed(display_surface_t *s, display_rect_t r, bm_err_t err, uint32_t us)
{
    display_src_rec_t *rec = __atomic_load_n(&s->flushed_rec, __ATOMIC_ACQUIRE);
    if (!rec || !atomic_load(&rec->on) || err != BM_OK) {   /* a failed region is thrown by the next call */
        return;
    }
    display_flushed_data_t d = { (int16_t)r.x0, (int16_t)r.y0, (int16_t)r.x1, (int16_t)r.y1, us };
    bm_events_emit_data(rec->tag, s->obj, &d, sizeof d);   /* any task: the driver's, or PHP's for push */
}

void display_te_event(display_flush_t *f)
{
    display_surface_t *s = f->ctx;
    display_src_rec_t *rec = __atomic_load_n(&s->vsync_rec, __ATOMIC_ACQUIRE);
    if (rec && atomic_load(&rec->on)) {
        bm_events_emit_from_isr(rec->tag, s->obj);
    }
    /* The renderer's hook (display_vsync_hook()): cb and arg read together, and busy until it returns, so that
       the PHP task can remove it and know it no longer runs. */
    bm_hal_lock_enter(f->lock);
    void (*cb)(void *arg) = s->vsync_cb;
    void *arg = s->vsync_arg;
    if (cb) {
        s->vsync_cb_busy++;
    }
    bm_hal_lock_exit(f->lock);
    if (cb) {
        cb(arg);
        bm_hal_lock_enter(f->lock);
        s->vsync_cb_busy--;
        bm_hal_lock_exit(f->lock);
    }
}

static void te_take(display_surface_t *s);

/* The surface has TE pulses: from a pin, or from its driver. */
static bool has_te(const display_surface_t *s)
{
    return s->te >= 0 || s->te_drv;
}

/* Sets (or with cb NULL removes) the hook and waits until a pulse running the previous one has finished it. */
static void vsync_hook_set(display_surface_t *s, void (*cb)(void *arg), void *arg)
{
    for (;;) {
        bm_hal_lock_enter(s->flush.lock);
        s->vsync_cb = cb;
        s->vsync_arg = arg;
        bool idle = s->vsync_cb_busy == 0;
        bm_hal_lock_exit(s->flush.lock);
        if (idle) {
            return;   /* a pulse after this point reads the new hook */
        }
    }
}

bool display_vsync_hook(zend_object *obj, void (*cb)(void *arg), void *arg)
{
    display_surface_t *s = display_surface_find(obj);
    if (!s || !has_te(s) || !s->flush.lock) {
        return false;
    }
    vsync_hook_set(s, cb, arg);
    if (cb) {
        if (s->te_on && !s->te_owner) {
            te_take(s);   /* another object on the same pin had the handler: the hook's surface takes it back */
        }
        if (!display_te_attach(s)) {   /* after the hook: the first pulse already runs it */
            vsync_hook_set(s, NULL, NULL);
            return false;
        }
    }
    return true;
}

static void te_isr(void *arg)
{
    display_flush_te(&((display_surface_t *)arg)->flush);
}

/* The surfaces that want the TE pulses (te_on), the newest first; only the PHP task changes it. A pin has one
   handler (bm_hal_gpio_on_falling() replaces the previous one), so several objects on one panel share it: the
   handler is the te_owner's, and goes to another surface of the list when its owner goes. A driver-signalled
   panel (te_drv) works the same way, with its place in s_drv_te for the handler. */
static display_surface_t *s_te_list;

#define DISPLAY_DRV_TE_MAX 2
/* The surface that takes the pulses of each driver-signalled panel: written on the PHP task (te_take, te_detach),
   read by display_te_pulse() in the driver's interrupt; busy counts the pulses running, so that a detach waits
   for them. */
static struct { void *panel; display_surface_t *_Atomic s; atomic_int busy; } s_drv_te[DISPLAY_DRV_TE_MAX];

/* The place of panel, or a free one (with claim, given to panel), -1 when none; PHP task. A free place has
   neither a panel nor a surface. */
static int drv_slot(void *panel, bool claim)
{
    int free_at = -1;
    for (int i = 0; i < DISPLAY_DRV_TE_MAX; i++) {
        void *p = __atomic_load_n(&s_drv_te[i].panel, __ATOMIC_ACQUIRE);
        if (p == panel && atomic_load(&s_drv_te[i].s)) {
            return i;
        }
        if (free_at < 0 && !atomic_load(&s_drv_te[i].s)) {
            free_at = i;
        }
    }
    if (claim && free_at >= 0) {
        __atomic_store_n(&s_drv_te[free_at].panel, panel, __ATOMIC_RELEASE);
    }
    return free_at;
}

bool display_te_drv_room(void *panel)
{
    return drv_slot(panel, false) >= 0;
}

/* Two surfaces share a TE source: the same pin, or the same driver-signalled panel. */
static bool same_te(const display_surface_t *a, const display_surface_t *b)
{
    return a->te_drv ? b->te_drv && a->panel == b->panel : !b->te_drv && a->te == b->te;
}

static void te_take(display_surface_t *s)
{
    for (display_surface_t *t = s_te_list; t; t = t->te_next) {
        if (t != s && same_te(t, s)) {
            t->te_owner = false;
        }
    }
    s->te_owner = true;
    if (s->te_drv) {
        int i = drv_slot(s->panel, true);   /* display_te_attach() made sure there is one */
        if (i >= 0) {
            atomic_store(&s_drv_te[i].s, s);
        }
        return;
    }
    bm_hal_gpio_on_falling(s->te, te_isr, s);
}

bool display_te_attach(display_surface_t *s)
{
    if (s->te_on) {
        return true;
    }
    if (!has_te(s) || !s->flush.lock || (s->te_drv && !display_te_drv_room(s->panel))) {
        return false;
    }
    s->te_on = true;
    s->te_next = s_te_list;
    s_te_list = s;
    te_take(s);
    return true;
}

void display_te_pulse(void *panel)
{
    for (int i = 0; i < DISPLAY_DRV_TE_MAX; i++) {
        if (__atomic_load_n(&s_drv_te[i].panel, __ATOMIC_ACQUIRE) != panel) {
            continue;
        }
        atomic_fetch_add(&s_drv_te[i].busy, 1);
        /* checked again once busy: a detach empties s, waits for busy, then frees the place; a place given to
           another panel meanwhile is not this pulse's */
        display_surface_t *s = atomic_load(&s_drv_te[i].s);
        if (s && __atomic_load_n(&s_drv_te[i].panel, __ATOMIC_ACQUIRE) == panel) {
            display_flush_te(&s->flush);
        }
        atomic_fetch_sub(&s_drv_te[i].busy, 1);
        return;
    }
}

/* Waits until no pulse of s's panel is running, so that one that read s before the place changed is over. */
static void drv_te_wait(const display_surface_t *s)
{
    for (int i = 0; i < DISPLAY_DRV_TE_MAX; i++) {
        if (__atomic_load_n(&s_drv_te[i].panel, __ATOMIC_ACQUIRE) == s->panel) {
            while (atomic_load(&s_drv_te[i].busy) != 0) {
                /* a pulse in the driver's interrupt: a few microseconds */
            }
        }
    }
}

void display_te_detach(display_surface_t *s)
{
    if (!s->te_on) {
        return;
    }
    for (display_surface_t **pp = &s_te_list; *pp; pp = &(*pp)->te_next) {
        if (*pp == s) {
            *pp = s->te_next;
            break;
        }
    }
    s->te_on = false;
    s->te_next = NULL;
    if (!s->te_owner) {
        return;   /* the handler is another surface's */
    }
    s->te_owner = false;
    for (display_surface_t *t = s_te_list; t; t = t->te_next) {
        if (same_te(t, s)) {
            te_take(t);   /* the newest other surface on the pin: its vsync regions and onVsync() go on */
            if (s->te_drv) {
                drv_te_wait(s);
            }
            return;
        }
    }
    if (!s->te_drv) {
        bm_hal_gpio_off(s->te);
        return;
    }
    for (int i = 0; i < DISPLAY_DRV_TE_MAX; i++) {
        if (atomic_load(&s_drv_te[i].s) == s) {
            atomic_store(&s_drv_te[i].s, NULL);   /* the panel's pulses reach nobody */
            while (atomic_load(&s_drv_te[i].busy) != 0) {
                /* a pulse that read s before: a few microseconds in the driver's interrupt */
            }
            __atomic_store_n(&s_drv_te[i].panel, NULL, __ATOMIC_RELEASE);   /* the place is free */
        }
    }
}

void display_events_detach(display_surface_t *s)
{
    if (s->flush.lock) {
        vsync_hook_set(s, NULL, NULL);
    }
    __atomic_store_n(&s->flushed_rec, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&s->vsync_rec, NULL, __ATOMIC_RELEASE);
    for (display_src_rec_t *rec = s_recs; rec; rec = rec->next) {
        if (rec->s == s) {
            rec->s = NULL;
        }
    }
}

/* ---- onFlushed(), onVsync() ---- */

/* A new active record of s for tag; returns its handle, or NULL with an exception. */
static zend_object *source_new(display_surface_t *s, uint16_t tag, bool vsync)
{
    display_src_rec_t *rec = pemalloc(sizeof *rec, 1);
    rec->s = s;
    rec->tag = tag;
    rec->vsync = vsync;
    atomic_init(&rec->on, true);
    rec->next = s_recs;
    s_recs = rec;
    zend_object *h = bm_events_source_new(vsync ? s_vsync_source_ce : s_flush_source_ce, &s_src_ops, rec);
    if (!h) {
        return NULL;   /* never published: the record stays on the list, inactive */
    }
    if (!s->obj_held) {   /* before the record is published: every event queued carries a live object */
        GC_ADDREF(s->obj);
        s->obj_held = true;
    }
    __atomic_store_n(vsync ? &s->vsync_rec : &s->flushed_rec, rec, __ATOMIC_RELEASE);
    return h;
}

/* The tag of the class named by the method's argument, with the error of the other sources. */
static bool source_tag(zend_object *obj, const char *method, zend_string *event, bool coalesce, uint16_t *tag)
{
    if (!s_flush_source_ce) {
        zend_throw_error(NULL, "%s::%s(): needs ext-events loaded before ext-display", display_panel_name(obj),
            method);
        return false;
    }
    if (!bm_events_source_tag_ex(event, coalesce, tag)) {
        if (!EG(exception)) {
            zend_throw_error(NULL, "%s::%s(): unknown class '%s' or too many sources", display_panel_name(obj),
                method, ZSTR_VAL(event));
        }
        return false;
    }
    return true;
}

static bool class_extends(zend_string *name, zend_class_entry *base)
{
    zend_class_entry *ce = zend_lookup_class(name);   /* resolved already by the tag lookup */
    return ce && base && instanceof_function(ce, base);
}

PHP_METHOD(Display, onFlushed)
{
    zend_string *event;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(event)
    ZEND_PARSE_PARAMETERS_END();
    display_surface_t *s = display_surface(Z_OBJ_P(ZEND_THIS));
    if (!s) {
        RETURN_THROWS();
    }
    uint16_t tag;
    if (!source_tag(s->obj, "onFlushed", event, false, &tag)) {
        RETURN_THROWS();
    }
    if (class_extends(event, s_flushed_ce)) {
        bm_events_set_filler(tag, flushed_fill);   /* a tag has one filler: only for these classes */
    }
    zend_object *h = source_new(s, tag, false);
    if (!h) {
        RETURN_THROWS();
    }
    RETURN_OBJ(h);
}

PHP_METHOD(Display, onVsync)
{
    zend_string *event;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_STR(event)
    ZEND_PARSE_PARAMETERS_END();
    display_surface_t *s = display_surface(Z_OBJ_P(ZEND_THIS));
    if (!s) {
        RETURN_THROWS();
    }
    if (!has_te(s)) {
        zend_value_error("%s::onVsync(): the display has no TE pin", display_panel_name(s->obj));
        RETURN_THROWS();
    }
    if (s->te_drv && !s->te_on && !display_te_drv_room(s->panel)) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0,
            "%s::onVsync(): too many panels with driver frame pulses", display_panel_name(s->obj));
        RETURN_THROWS();
    }
    uint16_t tag;
    if (!source_tag(s->obj, "onVsync", event, true, &tag)) {
        RETURN_THROWS();
    }
    if (class_extends(event, bm_events_timer_ce())) {
        bm_events_set_filler(tag, vsync_fill);
    }
    zend_object *h = source_new(s, tag, true);
    if (!h) {
        RETURN_THROWS();
    }
    (void)display_te_attach(s);   /* after the record: the first pulse already raises the event; room checked */
    RETURN_OBJ(h);
}

/* ---- classes ---- */

void display_events_register(void)
{
    zend_class_entry *event_ce = bm_events_base_ce(), *source_ce = bm_events_source_ce();
    if (!event_ce || !source_ce) {
        return;   /* onFlushed() and onVsync() throw */
    }
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Display", "Flushed", NULL);
    s_flushed_ce = zend_register_internal_class_ex(&ce, event_ce);
    static const char *const fields[] = { "x0", "y0", "x1", "y1", "us" };
    for (size_t i = 0; i < sizeof fields / sizeof fields[0]; i++) {
        zval undef;
        ZVAL_UNDEF(&undef);
        zend_declare_typed_property(s_flushed_ce, zend_string_init_interned(fields[i], strlen(fields[i]), 1),
            &undef, ZEND_ACC_PUBLIC | ZEND_ACC_READONLY, NULL, (zend_type)ZEND_TYPE_INIT_CODE(IS_LONG, 0, 0));
    }
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Display", "FlushSource", NULL);
    s_flush_source_ce = zend_register_internal_class_ex(&ce, source_ce);   /* create_object comes from Source */
    s_flush_source_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Display", "VsyncSource", NULL);
    s_vsync_source_ce = zend_register_internal_class_ex(&ce, source_ce);
    s_vsync_source_ce->ce_flags |= ZEND_ACC_FINAL | ZEND_ACC_NOT_SERIALIZABLE;
}

void display_events_shutdown(void)
{
    while (s_recs) {
        display_src_rec_t *next = s_recs->next;
        pefree(s_recs, 1);
        s_recs = next;
    }
}
