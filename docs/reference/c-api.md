---
eyebrow: 'ext-display · Drivers'
lede:    'The C contract between the surface and a panel driver: display_panel_ops_t, the asynchronous flush (display_job_t), display_attach_ex() and display_methods.'
see_also:
  - { href: './api.md', meta: 'PHP API' }
  - { href: './errors-and-limits.md', meta: 'Reference' }
  - { href: 'package:php-baremetal/esp32-display-st77916', meta: 'Driver', label: 'esp32-display-st77916' }
prev: { label: 'Errors and limits', href: './errors-and-limits.md' }
next: { label: 'No next page', href: '#' }
---

# C API for panel drivers

A panel driver owns the bus and the panel's start-up sequence; ext-display owns the framebuffer, the drawing and
the PHP methods. The driver describes its panel with `display_panel_ops_t`, attaches a surface to its PHP object
in the constructor, and puts the `Output\Display` methods in its class (`display_methods_with()` or `DISPLAY_FE`). Headers: `display_panel.h` (the contract),
`display_job.h` (the asynchronous flush, plain C, included by `display_panel.h`) and `display_font.h` (font
formats and the `display_fonts[]` table).

## The panel

<!-- @code-block language="c" label="display_panel.h" -->
```c
typedef enum { DISPLAY_FORMAT_RGB565 = 1, DISPLAY_FORMAT_MONO1 = 2 } display_format_t;

#define DISPLAY_CAP_HW_SCROLL 0x1u

typedef struct {
    uint16_t width, height;
    display_format_t format;
    uint32_t caps;
    float refresh_hz;   /* the panel's nominal refresh rate in Hz; 0 when unknown */
    bm_err_t (*push)(void *panel, int x0, int y0, int x1, int y1, const void *px);
    bm_err_t (*backlight)(void *panel, int level);   /* 0..255; NULL when the panel has no backlight */
    bm_err_t (*sleep)(void *panel, bool on);          /* NULL when there is none */
    /* Asynchronous flush (both NULL for a synchronous panel, which only has push). */
    bool (*begin)(void *panel, const display_job_t *job);
    bool (*wait)(void *panel, uint32_t ms);
} display_panel_ops_t;

typedef struct {
    bool buffered;   /* a copy of the framebuffer, which the panel reads */
    bool vsync;      /* every region starts at the next TE pulse; needs te */
    int te;          /* GPIO of the panel's TE output; -1: no TE pin */
} display_attach_opts_t;
```
<!-- @endcode-block -->

- `push` copies the rectangle `[x0, x1) x [y0, y1)` to the panel. `px` holds its rows, contiguous, two bytes per
  pixel with the high byte first (the order RGB565 panels take on the wire). `px` is valid only during the
  call; `push` returns when the data is sent. A region is pushed in bands of whole rows of up to 14 400 bytes,
  so one `flush()` may call `push` several times.
- A `bm_err_t` other than `BM_OK` from `push`, `backlight` or `sleep` becomes a `\RuntimeException`
  `<Panel>: push failed (error 0x..)` (or `backlight`, `sleep`, `wake`).
- `refresh_hz` is what `refreshRate()` returns: the panel's nominal refresh rate in Hz, `0` (the value a designated initialiser leaves) when the driver does not know it. Nothing in ext-display uses it.
- `begin` and `wait` make the flush asynchronous; see [Asynchronous flush](#asynchronous-flush). A driver without
  them stays synchronous with `push`.
- `format` must be `DISPLAY_FORMAT_RGB565`. `caps` is reserved for hardware features; nothing uses
  `DISPLAY_CAP_HW_SCROLL` yet.

## Functions

| Declaration | Meaning |
| --- | --- |
| `bm_err_t display_attach_ex(zend_object *obj, const display_panel_ops_t *ops, void *panel, const display_attach_opts_t *opts)` | Binds a new surface (framebuffer cleared to 0) to `obj`; attaching again replaces it (the old surface first waits for its flush and lets its TE pin go). With `ops->begin` the flushes are asynchronous and `ops->wait` is required; a synchronous panel ignores `buffered` and `vsync`. `opts` `NULL`: no copy, no vsync, no TE pin. `BM_ERR_NO_MEM` without memory for the framebuffer or the copy, `BM_ERR_INVALID_ARG` for `vsync` without `te` (the driver throws `\ValueError` `<Class>: vsync needs the TE pin (te:)`), `BM_ERR_INVALID_STATE` for a zero size, no `push`, a format other than RGB565, `begin` without `wait`, or a surface that cannot be recorded. `ops` and `panel` must outlive the object. |
| `bm_err_t display_attach(zend_object *obj, const display_panel_ops_t *ops, void *panel)` | `display_attach_ex(obj, ops, panel, NULL)`. |
| `void display_quiesce(void *panel)` | Waits until no surface attached with `panel` has a flush in flight or pending (a `vsync` region still waiting for the TE is dropped): call it before the driver frees the panel or stops its task. PHP task only. |
| `extern const zend_function_entry display_methods[]` | The `Output\Display` methods, terminated by `ZEND_FE_END`, with the interface's arginfo. |
| `const zend_function_entry *display_methods_with(const zend_function_entry *own)` | `own` followed by `display_methods`, in a table that lives until the process ends: use it when the class's method table is built at start-up. |
| `DISPLAY_FE` | The same methods as `zend_function_entry` initialisers, for a static table: `static const zend_function_entry t[] = { PHP_ME(...) DISPLAY_FE PHP_FE_END };`. Use it when the table must exist before start-up, as in an ext-spi driver descriptor (`spi_driver_desc_t.methods`). |
| `void display_invalidate(void *panel)` | Marks the whole screen of every surface attached with `panel` as changed, so the next `flush()` sends the full frame. For a driver whose panel lost its picture, for example after a re-initialisation in `sleep(panel, false)`. |
| `zend_class_entry *display_ce(void)` | `Baremetal\Output\Display`, for `zend_class_implements()`. |
| `const char *display_panel_name(zend_object *obj)` | The short class name, for messages. |

Call `display_attach()` while a script runs (from the constructor), not at module start-up. The surface is found by object and freed with it, when the object's `free_obj` calls `zend_object_std_dtor()`
(as every object handler must); whatever is left is freed at module shutdown. A `clone` has no surface: give the
driver class `clone_obj = NULL`.

## Asynchronous flush

A driver with `begin` and `wait` sends regions while PHP goes on. ext-display keeps the state (the region in
flight, the rows already sent, the pending region, the last error) and decides when a drawing must wait; the
driver only moves bytes.

<!-- @code-block language="c" label="display_job.h" -->
```c
typedef struct display_job display_job_t;
struct display_job {
    int x0, y0, x1, y1;          /* the region, [x0,x1) x [y0,y1) */
    const uint16_t *src;         /* rows of `stride` pixels: the framebuffer, or its copy (buffered) */
    int stride;
    bool direct;                 /* full width and in the panel's order: rows y0..y1 of src go out as they are */
    void (*read)(const display_job_t *job, int y0, int y1, uint8_t *out);
    void *owner;                 /* ext-display's; the driver passes the job back unchanged */
};

void display_rows_sent(const display_job_t *job, int y);
void display_job_done(const display_job_t *job, bm_err_t err, uint32_t us);
```
<!-- @endcode-block -->

| Entry | Contract |
| --- | --- |
| `bool begin(void *panel, const display_job_t *job)` | Copies `*job` (it is not valid after the call), hands it to the driver's task (a queue) and returns at once. ext-display calls it from the PHP task, from the driver's task (the pending region, inside `display_job_done()`) and from the TE interrupt (`vsync`), so it must be callable from an ISR. One job at a time: ext-display never begins another before `display_job_done()` of the last. `false` when the driver cannot take the job: the job fails with `BM_ERR_INVALID_STATE`, thrown at the next call. |
| `bool wait(void *panel, uint32_t ms)` | Blocks until a band or a job of this panel ends, or `ms` pass; `false` on timeout. There is no special case for a panel with nothing in flight: block up to `ms`. ext-display calls it in a loop with `ms` 20, checking its own state in between, under one deadline of 2000 ms for the whole wait (then `<Panel>: flush timed out`); it ignores the result. So a `vsync` region waiting for the TE neither spins nor hangs on a dead TE pin. PHP task only. |
| `void display_rows_sent(const display_job_t *job, int y)` | From the driver, after each band: the rows of the job above `y` are on the panel (a drawing on them no longer waits). Any task. |
| `void display_job_done(const display_job_t *job, bm_err_t err, uint32_t us)` | From the driver, once, at the end of the job: `err` `BM_OK` or the failure (thrown at the next call on the display), `us` the transfer time (the `$us` of `Flushed`). It may call `begin` for the pending region and raises the `onFlushed()` event, so call it from the driver's task, without holding the driver's own locks. |

- For a `direct` job the driver sends rows `y0..y1` of `src` (with `stride` pixels per row) as they are. Otherwise
  it calls `read(job, y0, y1, out)` for each band, which gathers those rows of the region into `out` (contiguous,
  two bytes per pixel in the panel's order); `read` only reads the job and runs on any task.
- The TE interrupt is not the driver's: give the pin to `display_attach_ex()` (`te`), and ext-display attaches
  `bm_hal_gpio_on_falling()` of the events HAL itself (with `vsync`, or at the first `onVsync()`), and detaches it
  with `bm_hal_gpio_off()` when the surface goes. Any panel with a TE pin gets `onVsync()` this way, and `vsync`
  too when it has `begin` and `wait`.
- A pin has one interrupt handler. When several objects of one panel use the same TE pin, the last one to attach it
  receives the pulses; when it goes, the handler passes to another live object on that pin, and the pin is released
  only when no object uses it.
- When the surface goes (the object is freed or attached again), a region still pending is dropped (it will never
  be shown; a `vsync` one would wait for a pulse), then the job in flight ends, then the TE interrupt is detached.
  `display_quiesce()` does the same for a `vsync` region, and a pending region without `vsync` starts as usual.
- ext-display protects its state with an events HAL lock (`bm_hal_lock_*`), usable from an interrupt, and never
  holds it while it calls `begin`, `wait` or its own callbacks.
- Before freeing the panel or stopping its task, the driver calls `display_quiesce(panel)` (the object's
  `free_obj` and a second `__construct`): `begin` must keep working until then.

## A minimal driver

<!-- @code-block language="c" label="my_panel.c" -->
```c
#include "display_panel.h"

typedef struct { int bus; } my_panel_t;
static my_panel_t s_panel;

static bm_err_t my_push(void *panel, int x0, int y0, int x1, int y1, const void *px)
{
    my_panel_t *p = panel;
    /* set the column/row window [x0, x1) x [y0, y1), then send (x1 - x0) * (y1 - y0) * 2 bytes of px */
    return my_bus_write_window(p->bus, x0, y0, x1, y1, px);
}

static zend_object_handlers my_handlers;

static zend_object *my_create(zend_class_entry *ce)
{
    zend_object *obj = zend_objects_new(ce);
    object_properties_init(obj, ce);
    obj->handlers = &my_handlers;
    return obj;
}

static const display_panel_ops_t my_ops = {
    .width = 240, .height = 240, .format = DISPLAY_FORMAT_RGB565, .push = my_push,
};

PHP_METHOD(MyPanel, __construct)
{
    ZEND_PARSE_PARAMETERS_NONE();
    /* bring the panel up on its bus here */
    if (display_attach(Z_OBJ_P(ZEND_THIS), &my_ops, &s_panel) != BM_OK) {
        zend_throw_error(NULL, "MyPanel: out of memory for the framebuffer");
    }
}

ZEND_BEGIN_ARG_INFO_EX(arginfo_my_ctor, 0, 0, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry my_own_methods[] = {
    PHP_ME(MyPanel, __construct, arginfo_my_ctor, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

void my_panel_register(void)   /* from the driver's MINIT, after ext-display's */
{
    zend_class_entry ce;
    INIT_NS_CLASS_ENTRY(ce, "Acme\\Display", "MyPanel", display_methods_with(my_own_methods));
    zend_class_entry *pce = zend_register_internal_class(&ce);
    pce->ce_flags |= ZEND_ACC_FINAL;
    pce->create_object = my_create;
    memcpy(&my_handlers, zend_get_std_object_handlers(), sizeof my_handlers);
    my_handlers.clone_obj = NULL;   /* a clone would have no surface */
    zend_class_implements(pce, 1, display_ce());
}
```
<!-- @endcode-block -->

## Fonts

`display_font.h` declares the Adafruit GFX types (`GFXfont`, `GFXglyph`, read as `fontconvert` writes them),
the extended `display_font_t` (several codepoint ranges, 1, 2 or 4 bits per pixel, written by
`phpbm font:bake`), and the table the SDK generates in `fonts.c`:

<!-- @code-block language="c" label="display_font.h" -->
```c
struct display_font_entry { const char *name; const void *gfx; const void *ext; };   /* one of the two is NULL */
extern const struct display_font_entry display_fonts[];
extern const size_t display_fonts_count;
```
<!-- @endcode-block -->

Glyphs follow Adafruit GFX: `yOffset` from the baseline to the glyph's top row, `xOffset` from the cursor,
`xAdvance` to the next character; bitmap rows top to bottom, `bpp` bits per pixel packed MSB first. A GFX font's
ascent is the highest glyph's `-yOffset`.

For an asynchronous driver, `my_ops` also sets `.begin` and `.wait`, the constructor calls `display_attach_ex()`
with the options it takes (`buffered`, `vsync`, `te`), and its `free_obj` calls `display_quiesce()` before it
stops the task that sends the bands.

## Byte order

By default the framebuffer stores pixels already swapped into the panel's byte order (`DISPLAY_FB_PRESWAP=1`),
so a full-width region goes to `push` straight from the framebuffer, with no copy. Building with
`-DDISPLAY_FB_PRESWAP=0` keeps them in the CPU's order instead and swaps them while filling each band. The
pre-swapped order makes a full-screen flush faster (see [Frame rates](errors-and-limits.md#frame-rates)).
