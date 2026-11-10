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
`display_job.h` (the asynchronous flush, plain C, included by `display_panel.h`), `display_font.h` (font
formats and the `display_fonts[]` table) and `display_text.h` (UTF-8 and glyph lookup, see [Fonts](#fonts)).

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
    /* since 1.1, last in the struct */
    const display_band_ops_t *band;   /* NULL: no band path */
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
- `band` (since 1.1) is the optional [band path](#band-path). Always initialise `display_panel_ops_t` with designated
  initialisers: a field you do not name is `NULL`, so a driver written for 1.0 has no band path.
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
| `void display_invalidate(void *panel)` | Marks the whole screen of every surface attached with `panel` as changed, so the next `flush()` sends the full frame. For a driver whose panel lost its picture, for example after a re-initialisation in `sleep(panel, false)`. A renderer that holds the panel gets `DISPLAY_PANEL_LOST` (see Renderers) and sends its whole frame again. PHP task only. |
| `void display_panel_of(zend_object *obj, const display_panel_ops_t **ops, void **panel)` | Since 1.1. The ops and the driver's `panel` behind a display object, for a [renderer](#renderers); `*ops` is `NULL` when `obj` is not a display or has no surface. |
| `bool display_claim(zend_object *obj, const char *owner)` | Since 1.1. Takes the panel for `owner`; see [Renderers](#renderers). PHP task only. |
| `bool display_claim_ex(zend_object *obj, const char *owner, void (*notify)(void *arg, display_panel_event_t event), void *arg)` | Since 1.1. `display_claim()` with a callback for sleep, wake and a lost picture (`display_panel_event_t`); see [Renderers](#renderers). PHP task only. |
| `void display_release(zend_object *obj)` | Since 1.1. Gives the panel back: the next `flush()` sends the full frame. |
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

## Band path

Since 1.1 a driver can offer a second way to put a picture on its panel, for a renderer that composes outside the
framebuffer and sends it band by band. The surface's own `flush()` never uses it. Point `display_panel_ops_t::band`
at a `display_band_ops_t` that lives as long as the panel (a copy in the driver's state when a field depends on the
panel's settings, as `scan` does). The type is in `display_band.h`, which `display_panel.h`
includes; it is plain C (no `php.h`), so a renderer's core can be built and tested without PHP:

<!-- @code-block language="c" label="display_band.h" -->
```c
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
    bool own_ram;           /* the panel keeps its picture; false: the driver copies bands into its frame */
    display_scan_t scan;    /* the refresh order; DISPLAY_SCAN_UNKNOWN when not set */
    void *(*buf_alloc)(void *panel, size_t bytes);
    void (*buf_free)(void *panel, void *px);
    bm_err_t (*begin)(void *panel, int x0, int y0, int x1, int y1);
    bm_err_t (*push)(void *panel, int y0, int rows, const void *px, void (*done)(void *arg), void *arg);
    bm_err_t (*end)(void *panel);
} display_band_ops_t;
```
<!-- @endcode-block -->

A region `[x0, x1) x [y0, y1)` is sent as `begin`, then `push` for each band from top to bottom, then `end`. The rules:

- **Threading.** `begin`, `push`, `end`, `buf_alloc` and `buf_free` are called by one task of the renderer, never from an
  ISR and never concurrently with each other.
- **Coordinates.** The region goes to `begin` in panel coordinates. The `y0` of `push` is a panel row inside the
  region. Every band spans the region's full width (`x1 - x0`), and the bands go top to bottom. `rows * (x1 - x0) * 2`
  never exceeds `bytes_max`; `rows` in the struct is the height the driver advises.
- **Buffers.** The renderer gets the band buffers from `buf_alloc(panel, bytes)` (`bytes <= bytes_max`; `NULL` without
  memory), so the driver picks DMA-capable internal RAM and its alignment. `px` of `push` is such a buffer, RGB565,
  high byte first like `ops->push`, with contiguous rows of `x1 - x0` pixels. `buf_free(panel, px)` takes it back, after
  its `done` ran or its `push` failed.
- **Pipelining.** `push` may be called again before the previous `done`. The driver accepts at least 2 bands in
  flight; when it cannot take one more it blocks inside `push` until it can. `done(arg)` runs exactly once for each
  accepted `push`, in push order, in a task (never in an ISR: a driver whose DMA ends in an ISR defers `done` to a
  task), and must be short.
- **Errors.** A `push` that returns an error did not take the band: `done` is not called and `px` is free at once.
  A panel that failed for good (a transfer that never ended, for example) makes every later `begin` and `push`
  return `BM_ERR_INVALID_STATE`, and only such a panel returns that code: the renderer takes it as final and stops
  sending (Slate throws `\RuntimeException` `Slate: the panel refused the bands (error 0x103)`). Any other error is
  transient, and the renderer may send the band again later.
- **`end`.** It returns once every band of the region has been accepted; it does not wait for `done`. The next
  `begin` may follow at once, and the driver processes regions in order.
- **`own_ram`.** `true` when the panel keeps what it received. `false`: the driver copies the bands into its own
  frame, and the picture may appear at the driver's next frame (a buffer swap at the TE pulse, for example) after
  `end`.
- **TE.** The band path does not wait for TE; a renderer paces itself on the TE pulses (`display_vsync_hook()`,
  below, or `onVsync()`).
- **`scan`.** The order in which the panel refreshes its picture from its RAM, once per TE period, in panel
  coordinates: the driver derives it from its orientation (a panel mirrored vertically scans `DISPLAY_SCAN_BOTTOM_UP`,
  one with its axes swapped scans across the columns). A renderer that paces itself on TE uses it to write each band
  where the refresh is not reading at that moment, ahead of it or behind it, so that a moving picture does not tear.
  `DISPLAY_SCAN_UNKNOWN` (0, what a driver that does not name the field gets) means it does not: the renderer
  sends its bands as they come. Set it only for a panel with its own RAM (`own_ram`) whose TE pulse starts the
  refresh. The driver itself still never waits.
- **Sleep, wake, backlight.** They stay callable while a renderer holds the panel and ext-display does not wait for
  the renderer's bands. A driver with a band path therefore executes `sleep` and `wake` after the bands it already
  accepted, in the order they came. ext-display tells the renderer before `sleep` and after `wake`
  (`display_claim_ex()`, below). A driver whose panel loses its picture (a re-initialisation on wake) calls
  `display_invalidate()`, which tells the renderer to send its whole frame again; a driver that does not call it
  keeps what the bands wrote, also while the panel sleeps.
- The TE pin stays as it is: the surface attached with `te:` raises `onVsync()` as before.
- Use the band path on a panel only while it is claimed (below): the surface's flushes and the band path are not
  meant to be used together.

## Renderers

A renderer that drives the panel itself (a scene compositor, for example) works on the panel behind a display
object and keeps the display's drawing methods out of the way:

1. `display_panel_of(obj, &ops, &panel)` gives the ops and the driver's panel pointer; check `ops->band`.
2. `display_claim(obj, "Slate")` takes the panel for an owner: a short name used in messages (31 bytes are kept).
   The flushes in flight end first (as in `display_quiesce()`). It returns `false`, claiming nothing, when
   another owner holds the panel, when `obj` has no surface, or when the flush in flight does not end in time.
3. While the panel is claimed, `clear()`, `pixel()`, `line()`, `rect()`, `fillRect()`, `circle()`, `fillCircle()`,
   `text()`, `image()`, `flush()` and `flushWait()` throw `\LogicException`
   `<Class>: the panel is driven by <owner>; draw in a <owner> Canvas`. `width()`, `height()`, `rgb()`,
   `textWidth()`, `backlight()`, `refreshRate()`, `sleep()`, `wake()`, `onFlushed()` and `onVsync()` keep
   working; `onFlushed()` raises nothing while the panel is claimed, because no flush happens.
4. The renderer keeps a reference on `obj` for as long as it holds the claim: the surface, the owner and the
   `ops` and `panel` of `display_panel_of()` go with the object. A driver does not attach a claimed object again
   (that drops the claim).
5. `display_claim_ex(obj, owner, notify, arg)` is `display_claim()` with a callback for what happens to the panel
   while it is claimed (`display_claim()` is `display_claim_ex()` with `notify` `NULL`):

   <!-- @code-block language="c" label="display_panel.h" -->
   ```c
   typedef enum {
       DISPLAY_PANEL_SLEEP = 1,   /* sleep() is about to turn the panel off */
       DISPLAY_PANEL_WAKE,        /* wake() turned it on again, or a sleep() failed */
       DISPLAY_PANEL_LOST,        /* the panel lost its picture (display_invalidate()) */
   } display_panel_event_t;
   bool display_claim_ex(zend_object *obj, const char *owner,
                         void (*notify)(void *arg, display_panel_event_t event), void *arg);
   ```
   <!-- @endcode-block -->

   `notify(arg, event)` runs on the PHP task inside `sleep()`, `wake()` and `display_invalidate()`, until
   `display_release()`; it must be short and must not call PHP. `DISPLAY_PANEL_SLEEP` comes before the driver's
   `sleep` op (a renderer pauses its frames: a panel that is off shows nothing, and its TE pulses stop);
   `DISPLAY_PANEL_WAKE` after a `wake` that succeeded, and after a `sleep` that failed; `DISPLAY_PANEL_LOST` when the
   driver calls `display_invalidate()`, so a driver that re-initialises the panel on wake gives `LOST`, then `WAKE`.
   Slate redraws the whole screen on `LOST` and starts no new frame between `SLEEP` and `WAKE` (one under way finishes).
6. `display_release(obj)` gives the panel back. The renderer has changed the picture, so the next `flush()` sends
   the full frame. A failed push of the display's own flush that was still unreported at the claim is not thrown
   later: its region is sent again by that flush.

A renderer that paces itself on the panel's TE pulses sets a hook:

<!-- @code-block language="c" label="display_panel.h" -->
```c
bool display_vsync_hook(zend_object *obj, void (*cb)(void *arg), void *arg);
```
<!-- @endcode-block -->

- `cb(arg)` runs in the TE interrupt at every pulse, so it does only ISR-safe work, such as
  `bm_hal_queue_send_isr()` to wake the renderer's task.
- It coexists with `onVsync()` and with the vsync flushes of the surface, and keeps running while the panel is
  claimed.
- One hook per display object: a new call replaces it, `cb` `NULL` removes it. When the call returns, the previous
  hook is not running and does not run again (the call waits for a pulse in progress), so the renderer can free
  `arg` right after removing it.
- The first hook attaches the TE interrupt; it then stays attached, as after `onVsync()`. The hook goes with the
  surface (the object freed, or attached again).
- A TE pin has one interrupt handler. Setting the hook takes the pin for `obj`. When another display object on the
  same pin takes it later (its first `onVsync()`, or an attach with `vsync`), the hook stops getting pulses until
  it is set again. A renderer should not depend on the pulses alone: Slate falls back to its own timer when the
  pulses stop.
- `false` when `obj` is not a display or the display has no TE pin (`te:`); the renderer then paces itself
  another way. PHP task only.

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

`display_text.h` (header only, plain C) reads text the way `text()` does, for a renderer that draws glyphs
itself: `display_text_utf8()` (next codepoint, U+FFFD for an invalid or truncated sequence),
`display_text_font_entry()` / `_gfx()` / `_ext()` (a `display_text_font_t` view of either format, `false` for an
extended font whose `bpp` is not 1, 2 or 4), `display_text_glyph()` (the glyph of a codepoint, or `NULL`) and
`display_text_advance()` (a missing glyph is drawn as a box half the line height wide).

For an asynchronous driver, `my_ops` also sets `.begin` and `.wait`, the constructor calls `display_attach_ex()`
with the options it takes (`buffered`, `vsync`, `te`), and its `free_obj` calls `display_quiesce()` before it
stops the task that sends the bands.

## Byte order

By default the framebuffer stores pixels already swapped into the panel's byte order (`DISPLAY_FB_PRESWAP=1`),
so a full-width region goes to `push` straight from the framebuffer, with no copy. Building with
`-DDISPLAY_FB_PRESWAP=0` keeps them in the CPU's order instead and swaps them while filling each band. The
pre-swapped order makes a full-screen flush faster (see [Frame rates](errors-and-limits.md#frame-rates)).
