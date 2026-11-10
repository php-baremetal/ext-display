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
 * Baremetal\Output\Display: the interface (abstract methods) and display_methods[], the same methods
 * implemented on the surface of the object, for the driver classes.
 */
#include "php_display.h"

#include <inttypes.h>
#include <string.h>

#include "zend_exceptions.h"
#include "ext/spl/spl_exceptions.h"

static zend_class_entry *s_display_ce;

zend_class_entry *display_ce(void)
{
    return s_display_ce;
}

/* Coordinates and sizes are clamped, so that the int arithmetic of the primitives cannot overflow. */
#define DISPLAY_COORD_MAX 65536

static int coord(zend_long v)
{
    return v < -DISPLAY_COORD_MAX ? -DISPLAY_COORD_MAX : v > DISPLAY_COORD_MAX ? DISPLAY_COORD_MAX : (int)v;
}

static uint16_t color(zend_long v)
{
    return (uint16_t)(v & 0xFFFF);
}

static void op_failed(const char *what, bm_err_t e)
{
    zend_throw_exception_ex(spl_ce_RuntimeException, 0, "%s: %s failed (error 0x%x)",
        display_panel_name(Z_OBJ(EG(current_execute_data)->This)), what, (unsigned)e);
}

/* Throws the failure of an asynchronous flush that ended since the last call, once; its region goes back into
   the dirty rectangle, so the next flush() sends it again. */
static bool surface_check(display_surface_t *s)
{
    if (!s->async) {
        return true;
    }
    display_rect_t r;
    bm_err_t e = display_flush_take_error(&s->flush, &r);
    if (e == BM_OK) {
        return true;
    }
    display_fb_mark(&s->fb, r.x0, r.y0, r.x1, r.y1);
    op_failed("push", e);
    return false;
}

#define THIS_SURFACE(s)                                          \
    display_surface_t *s = display_surface(Z_OBJ_P(ZEND_THIS)); \
    if (!s || !surface_check(s)) {                               \
        RETURN_THROWS();                                         \
    }

/* THIS_SURFACE for the methods that draw or send: refused while a renderer holds the panel. */
#define THIS_DRAW(s)                                             \
    display_surface_t *s = display_surface(Z_OBJ_P(ZEND_THIS)); \
    if (!s || !display_not_claimed(s) || !surface_check(s)) {    \
        RETURN_THROWS();                                         \
    }

/* Whether the rectangle *arg is clear of the rows not yet sent and of the pending region. */
static bool no_conflict(display_surface_t *s, const void *arg)
{
    bm_hal_lock_enter(s->flush.lock);
    bool conflict = display_flush_conflict(&s->flush, *(const display_rect_t *)arg);
    bm_hal_lock_exit(s->flush.lock);   /* never held across wait(): the driver takes it to report bands */
    return !conflict;
}

/* Before a drawing writes [x0,x1) x [y0,y1) (not clipped): waits until those rows of the flush in flight are
   sent and the pending region does not cover them. Nothing to wait for without the engine or with the copy.
   false (RuntimeException thrown) when the driver timed out. */
static bool guard(display_surface_t *s, int x0, int y0, int x1, int y1)
{
    if (!s->async || s->buffered) {
        return true;
    }
    display_rect_t r = { x0, y0, x1, y1 };
    if (!display_wait_for(s, no_conflict, &r)) {
        zend_throw_exception_ex(spl_ce_RuntimeException, 0, "%s: flush timed out", display_panel_name(s->obj));
        return false;
    }
    return true;
}

#define GUARD(s, x0, y0, x1, y1)                 \
    if (!guard((s), (x0), (y0), (x1), (y1))) {   \
        RETURN_THROWS();                         \
    }

/* Pushes r (clipped) to the panel in bands of whole rows. */
static bool push_region(display_surface_t *s, display_fb_rect_t r)
{
    int rw = r.x1 - r.x0;
    const uint8_t *direct = display_fb_direct(&s->fb, r.x0, r.y0, r.x1);
    if (direct) {
        bm_err_t e = s->ops->push(s->panel, r.x0, r.y0, r.x1, r.y1, direct);
        if (e != BM_OK) {
            op_failed("push", e);
            return false;
        }
        return true;
    }
    int rows = (int)(s->band_px / (size_t)rw);
    for (int y = r.y0; y < r.y1; y += rows) {
        int y1 = y + rows < r.y1 ? y + rows : r.y1;
        display_fb_emit(&s->fb, r.x0, y, r.x1, y1, s->band);
        bm_err_t e = s->ops->push(s->panel, r.x0, y, r.x1, y1, s->band);
        if (e != BM_OK) {
            op_failed("push", e);
            return false;
        }
    }
    return true;
}

/* ---- arginfo, shared by the interface and display_methods; exported for DISPLAY_FE ---- */

/* ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX without `static`: it mirrors Zend's arginfo layout
   (ZEND_TYPE_INIT_CODE, _ZEND_ARG_INFO_FLAGS), so check it against zend_API.h on every PHP upgrade. */
#define DISPLAY_BEGIN_ARGINFO(name, required_num_args, type)                                        \
    const zend_internal_arg_info name[] = {                                                          \
        { (const char *)(uintptr_t)(required_num_args), ZEND_TYPE_INIT_CODE(type, 0, _ZEND_ARG_INFO_FLAGS(0, 0, 0)), NULL },

DISPLAY_BEGIN_ARGINFO(display_arginfo_size, 0, IS_LONG)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_rgb, 3, IS_LONG)
    ZEND_ARG_TYPE_INFO(0, r, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, g, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, b, IS_LONG, 0)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_clear, 1, IS_VOID)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_pixel, 3, IS_VOID)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_line, 5, IS_VOID)
    ZEND_ARG_TYPE_INFO(0, x0, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y0, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, x1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y1, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_rect, 5, IS_VOID)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, w, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, h, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_circle, 4, IS_VOID)
    ZEND_ARG_TYPE_INFO(0, cx, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, cy, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, r, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_text, 4, IS_LONG)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, color, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, bg, IS_LONG, 1, "null")
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, font, Baremetal\\Display\\Font, 1, "null")
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_text_width, 1, IS_LONG)
    ZEND_ARG_TYPE_INFO(0, s, IS_STRING, 0)
    ZEND_ARG_OBJ_INFO_WITH_DEFAULT_VALUE(0, font, Baremetal\\Display\\Font, 1, "null")
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_image, 5, IS_VOID)
    ZEND_ARG_TYPE_INFO(0, x, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, y, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, w, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, h, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, pixels, IS_STRING, 0)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_flush, 0, IS_VOID)
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, x0, IS_LONG, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, y0, IS_LONG, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, x1, IS_LONG, 1, "null")
    ZEND_ARG_TYPE_INFO_WITH_DEFAULT_VALUE(0, y1, IS_LONG, 1, "null")
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_backlight, 1, IS_VOID)
    ZEND_ARG_TYPE_INFO(0, level, IS_LONG, 0)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_void, 0, IS_VOID)
ZEND_END_ARG_INFO()

DISPLAY_BEGIN_ARGINFO(display_arginfo_rate, 0, IS_DOUBLE)
ZEND_END_ARG_INFO()

/* As DISPLAY_BEGIN_ARGINFO, returning an object of class_name (ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX). */
#define DISPLAY_BEGIN_ARGINFO_OBJ(name, required_num_args, class_name)                              \
    const zend_internal_arg_info name[] = {                                                          \
        { (const char *)(uintptr_t)(required_num_args), ZEND_TYPE_INIT_CLASS_CONST(#class_name, 0, _ZEND_ARG_INFO_FLAGS(0, 0, 0)), NULL },

DISPLAY_BEGIN_ARGINFO_OBJ(display_arginfo_on_event, 1, Baremetal\\Source)
    ZEND_ARG_TYPE_INFO(0, event, IS_STRING, 0)
ZEND_END_ARG_INFO()

/* ---- implementations ---- */

PHP_METHOD(Display, width)
{
    ZEND_PARSE_PARAMETERS_NONE();
    THIS_SURFACE(s);
    RETURN_LONG(s->fb.w);
}

PHP_METHOD(Display, height)
{
    ZEND_PARSE_PARAMETERS_NONE();
    THIS_SURFACE(s);
    RETURN_LONG(s->fb.h);
}

PHP_METHOD(Display, refreshRate)
{
    ZEND_PARSE_PARAMETERS_NONE();
    THIS_SURFACE(s);
    RETURN_DOUBLE((double)s->ops->refresh_hz);
}

PHP_METHOD(Display, rgb)
{
    zend_long r, g, b;
    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_LONG(r)
        Z_PARAM_LONG(g)
        Z_PARAM_LONG(b)
    ZEND_PARSE_PARAMETERS_END();
    THIS_SURFACE(s);
    (void)s;
    RETURN_LONG(display_fb_rgb565(coord(r), coord(g), coord(b)));
}

PHP_METHOD(Display, clear)
{
    zend_long c;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(c)
    ZEND_PARSE_PARAMETERS_END();
    THIS_DRAW(s);
    GUARD(s, 0, 0, s->fb.w, s->fb.h);
    display_fb_clear(&s->fb, color(c));
}

PHP_METHOD(Display, pixel)
{
    zend_long x, y, c;
    ZEND_PARSE_PARAMETERS_START(3, 3)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_LONG(c)
    ZEND_PARSE_PARAMETERS_END();
    THIS_DRAW(s);
    GUARD(s, coord(x), coord(y), coord(x) + 1, coord(y) + 1);
    display_fb_pixel(&s->fb, coord(x), coord(y), color(c));
}

PHP_METHOD(Display, line)
{
    zend_long x0, y0, x1, y1, c;
    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_LONG(x0)
        Z_PARAM_LONG(y0)
        Z_PARAM_LONG(x1)
        Z_PARAM_LONG(y1)
        Z_PARAM_LONG(c)
    ZEND_PARSE_PARAMETERS_END();
    THIS_DRAW(s);
    int ax = coord(x0), ay = coord(y0), bx = coord(x1), by = coord(y1);
    GUARD(s, ax < bx ? ax : bx, ay < by ? ay : by, (ax > bx ? ax : bx) + 1, (ay > by ? ay : by) + 1);
    display_fb_line(&s->fb, ax, ay, bx, by, color(c));
}

static void rect_args(INTERNAL_FUNCTION_PARAMETERS, bool fill)
{
    zend_long x, y, w, h, c;
    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_LONG(w)
        Z_PARAM_LONG(h)
        Z_PARAM_LONG(c)
    ZEND_PARSE_PARAMETERS_END();
    THIS_DRAW(s);
    GUARD(s, coord(x), coord(y), coord(x) + coord(w), coord(y) + coord(h));
    (fill ? display_fb_fill_rect : display_fb_rect)(&s->fb, coord(x), coord(y), coord(w), coord(h), color(c));
}

PHP_METHOD(Display, rect)
{
    rect_args(INTERNAL_FUNCTION_PARAM_PASSTHRU, false);
}

PHP_METHOD(Display, fillRect)
{
    rect_args(INTERNAL_FUNCTION_PARAM_PASSTHRU, true);
}

static void circle_args(INTERNAL_FUNCTION_PARAMETERS, bool fill)
{
    zend_long cx, cy, r, c;
    ZEND_PARSE_PARAMETERS_START(4, 4)
        Z_PARAM_LONG(cx)
        Z_PARAM_LONG(cy)
        Z_PARAM_LONG(r)
        Z_PARAM_LONG(c)
    ZEND_PARSE_PARAMETERS_END();
    THIS_DRAW(s);
    GUARD(s, coord(cx) - coord(r), coord(cy) - coord(r), coord(cx) + coord(r) + 1, coord(cy) + coord(r) + 1);
    (fill ? display_fb_fill_circle : display_fb_circle)(&s->fb, coord(cx), coord(cy), coord(r), color(c));
}

PHP_METHOD(Display, circle)
{
    circle_args(INTERNAL_FUNCTION_PARAM_PASSTHRU, false);
}

PHP_METHOD(Display, fillCircle)
{
    circle_args(INTERNAL_FUNCTION_PARAM_PASSTHRU, true);
}

PHP_METHOD(Display, text)
{
    zend_long x, y, c, bg = 0;
    bool bg_null = true;
    zend_string *str;
    zend_object *font = NULL;
    ZEND_PARSE_PARAMETERS_START(4, 6)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_STR(str)
        Z_PARAM_LONG(c)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(bg, bg_null)
        Z_PARAM_OBJ_OF_CLASS_OR_NULL(font, display_font_ce)
    ZEND_PARSE_PARAMETERS_END();
    THIS_DRAW(s);
    const display_fb_font_t *f = display_font_of(font);
    if (!f) {
        RETURN_THROWS();
    }
    /* every column, a line height above and below y: conservative for any glyph of the font */
    GUARD(s, 0, coord(y) - f->line_height, s->fb.w, coord(y) + f->line_height);
    RETURN_LONG(display_fb_text(&s->fb, coord(x), coord(y), ZSTR_VAL(str), ZSTR_LEN(str), color(c),
        bg_null ? -1 : color(bg), f));
}

PHP_METHOD(Display, textWidth)
{
    zend_string *str;
    zend_object *font = NULL;
    ZEND_PARSE_PARAMETERS_START(1, 2)
        Z_PARAM_STR(str)
        Z_PARAM_OPTIONAL
        Z_PARAM_OBJ_OF_CLASS_OR_NULL(font, display_font_ce)
    ZEND_PARSE_PARAMETERS_END();
    THIS_SURFACE(s);
    (void)s;
    const display_fb_font_t *f = display_font_of(font);
    if (!f) {
        RETURN_THROWS();
    }
    RETURN_LONG(display_fb_text_width(ZSTR_VAL(str), ZSTR_LEN(str), f));
}

PHP_METHOD(Display, image)
{
    zend_long x, y, w, h;
    zend_string *pixels;
    ZEND_PARSE_PARAMETERS_START(5, 5)
        Z_PARAM_LONG(x)
        Z_PARAM_LONG(y)
        Z_PARAM_LONG(w)
        Z_PARAM_LONG(h)
        Z_PARAM_STR(pixels)
    ZEND_PARSE_PARAMETERS_END();
    THIS_DRAW(s);
    if (w < 0 || w > DISPLAY_COORD_MAX) {
        zend_argument_value_error(3, "must be between 0 and %d", DISPLAY_COORD_MAX);
        RETURN_THROWS();
    }
    if (h < 0 || h > DISPLAY_COORD_MAX) {
        zend_argument_value_error(4, "must be between 0 and %d", DISPLAY_COORD_MAX);
        RETURN_THROWS();
    }
    uint64_t want = (uint64_t)w * (uint64_t)h * 2;
    if (want != ZSTR_LEN(pixels)) {
        zend_argument_value_error(5, "must be %" PRIu64 " bytes long (w * h * 2), got %zu", want, ZSTR_LEN(pixels));
        RETURN_THROWS();
    }
    if (want) {
        GUARD(s, coord(x), coord(y), coord(x) + (int)w, coord(y) + (int)h);
        display_fb_image(&s->fb, coord(x), coord(y), (int)w, (int)h, (const uint8_t *)ZSTR_VAL(pixels));
    }
}

/* Sends r (clipped): through the engine (after copying it, with the copy), or pushed before returning.
   false (exception thrown) when it did not start. */
static bool send_region(display_surface_t *s, display_fb_rect_t r)
{
    display_rect_t region = { r.x0, r.y0, r.x1, r.y1 };
    if (!s->async) {
        int64_t t0 = bm_hal_now_us();
        if (!push_region(s, r)) {
            return false;
        }
        display_flushed(&s->flush, region, BM_OK, (uint32_t)(bm_hal_now_us() - t0));
        return true;
    }
    if (s->buffered) {
        if (!display_wait_idle(s)) {   /* the panel may still be reading the copy */
            return false;
        }
        size_t n = (size_t)(r.x1 - r.x0) * sizeof(uint16_t);
        for (int y = r.y0; y < r.y1; y++) {
            size_t at = (size_t)y * (size_t)s->fb.w + (size_t)r.x0;
            memcpy(s->copy + at, s->fb.px + at, n);
        }
    }
    display_flush_submit(&s->flush, region);
    return true;
}

PHP_METHOD(Display, flush)
{
    zend_long v[4] = {0, 0, 0, 0};
    bool null[4] = {true, true, true, true};
    ZEND_PARSE_PARAMETERS_START(0, 4)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG_OR_NULL(v[0], null[0])
        Z_PARAM_LONG_OR_NULL(v[1], null[1])
        Z_PARAM_LONG_OR_NULL(v[2], null[2])
        Z_PARAM_LONG_OR_NULL(v[3], null[3])
    ZEND_PARSE_PARAMETERS_END();
    int given = !null[0] + !null[1] + !null[2] + !null[3];
    if (given != 0 && given != 4) {
        zend_argument_count_error("%s::flush() takes all four coordinates or none, %d given",
            ZSTR_VAL(Z_OBJCE_P(ZEND_THIS)->name), given);
        RETURN_THROWS();
    }
    THIS_DRAW(s);
    display_fb_rect_t r;
    if (given == 0) {
        if (!display_fb_take_dirty(&s->fb, &r)) {
            return;
        }
        if (!send_region(s, r)) {
            display_fb_mark(&s->fb, r.x0, r.y0, r.x1, r.y1);   /* sent again by the next flush() */
        }
        return;
    }
    r = (display_fb_rect_t){ coord(v[0]), coord(v[1]), coord(v[2]), coord(v[3]) };
    if (!display_fb_clip(&s->fb, &r)) {
        return;
    }
    if (send_region(s, r)) {
        const display_fb_rect_t *d = &s->fb.dirty;
        if (d->x0 >= r.x0 && d->y0 >= r.y0 && d->x1 <= r.x1 && d->y1 <= r.y1) {
            s->fb.dirty = (display_fb_rect_t){ 0, 0, 0, 0 };
        }
    }
}

PHP_METHOD(Display, flushWait)
{
    ZEND_PARSE_PARAMETERS_NONE();
    THIS_DRAW(s);
    if (!display_wait_idle(s) || !surface_check(s)) {   /* a failure of what it waited for throws here */
        RETURN_THROWS();
    }
}

PHP_METHOD(Display, backlight)
{
    zend_long level;
    ZEND_PARSE_PARAMETERS_START(1, 1)
        Z_PARAM_LONG(level)
    ZEND_PARSE_PARAMETERS_END();
    THIS_SURFACE(s);
    if (level < 0 || level > 255) {
        zend_argument_value_error(1, "must be between 0 and 255");
        RETURN_THROWS();
    }
    if (!display_wait_idle(s)) {
        RETURN_THROWS();
    }
    if (s->ops->backlight) {
        bm_err_t e = s->ops->backlight(s->panel, (int)level);
        if (e != BM_OK) {
            op_failed("backlight", e);
        }
    }
}

static void sleep_to(INTERNAL_FUNCTION_PARAMETERS, bool on)
{
    ZEND_PARSE_PARAMETERS_NONE();
    THIS_SURFACE(s);
    if (s->async && s->flush.vsync) {
        /* A vsync region still pending starts only at a TE pulse, which a panel going off may no longer give, and
           it would only reach a panel that goes off or is set up again (wake resends the whole frame): dropped, as
           when the surface goes. Then only the job in flight is waited for. */
        display_flush_drop_pending(&s->flush);
    }
    if (!display_wait_idle(s)) {
        RETURN_THROWS();
    }
    if (s->ops->sleep) {
        if (on) {
            display_notify(s, DISPLAY_PANEL_SLEEP);   /* a renderer pauses its frames before the panel goes off */
        }
        bm_err_t e = s->ops->sleep(s->panel, on);
        if (e == BM_OK ? !on : on) {
            display_notify(s, DISPLAY_PANEL_WAKE);    /* awake again, or a sleep that failed: frames may go */
        }
        if (e != BM_OK) {
            op_failed(on ? "sleep" : "wake", e);
        }
    }
}

PHP_METHOD(Display, sleep)
{
    sleep_to(INTERNAL_FUNCTION_PARAM_PASSTHRU, true);
}

PHP_METHOD(Display, wake)
{
    sleep_to(INTERNAL_FUNCTION_PARAM_PASSTHRU, false);
}

/* One list, two tables: abstract in the interface, implemented in display_methods. */
#define DISPLAY_ABSTRACT(name, args) ZEND_ABSTRACT_ME(Display, name, display_arginfo_##args)

static const zend_function_entry display_interface_methods[] = {
    DISPLAY_METHOD_LIST(DISPLAY_ABSTRACT)
    PHP_FE_END
};

const zend_function_entry display_methods[] = {
    DISPLAY_FE
    PHP_FE_END
};

const zend_function_entry *display_methods_with(const zend_function_entry *own)
{
    size_t n_own = 0, n_display = 0;
    while (own && own[n_own].fname) {
        n_own++;
    }
    while (display_methods[n_display].fname) {
        n_display++;
    }
    zend_function_entry *all = pecalloc(n_own + n_display + 1, sizeof *all, 1);
    if (n_own) {
        memcpy(all, own, n_own * sizeof *all);
    }
    memcpy(all + n_own, display_methods, n_display * sizeof *all);
    return all;
}

void display_interface_register(void)
{
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Baremetal\\Output", "Display", display_interface_methods);
    s_display_ce = zend_register_internal_interface(&ce);
}
