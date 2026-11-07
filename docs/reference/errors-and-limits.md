---
eyebrow: 'ext-display · Reference'
lede:    'Every exception ext-display throws, and the sizes and rules behind them.'
see_also:
  - { href: './api.md', meta: 'PHP API' }
  - { href: './c-api.md', meta: 'Drivers' }
prev: { label: 'PHP API', href: './api.md' }
next: { label: 'C API for panel drivers', href: './c-api.md' }
---

# Errors and limits

ext-display throws `\RuntimeException` for a failing panel, a flush that times out and a missing font,
`\ValueError` and `\ArgumentCountError` for invalid arguments, and `\Error` for an object without a surface and
for an event class it cannot register. `<Panel>` below is the
short class name of the driver (`St77916`).

## Exceptions

| Exception | Message | Thrown by |
| --- | --- | --- |
| `\RuntimeException` | `<Panel>: push failed (error 0x..)` | `flush()`: the driver could not send a region (for example a DMA timeout). With an asynchronous driver, the next call on the display after the failure: a drawing, `flush()`, `flushWait()`, `sleep()`, `wake()`, `backlight()`. |
| `\RuntimeException` | `<Panel>: flush timed out` | a drawing, `flush()`, `flushWait()`, `sleep()`, `wake()` or `backlight()` that waits more than 2000 ms in all for the flush (for example a `vsync` region whose TE pulse never comes). |
| `\RuntimeException` | `<Panel>: backlight failed (error 0x..)` | `backlight()` |
| `\RuntimeException` | `<Panel>: sleep failed (error 0x..)`, `<Panel>: wake failed (error 0x..)` | `sleep()`, `wake()` |
| `\RuntimeException` | `unknown font '<name>' (available: default, title, icons)` | `Font::get()`: the list is every font in the firmware. |
| `\RuntimeException` | `font '<name>' has an unsupported bpp <n>` | `Font::get()`: an extended font whose bits per pixel are not 1, 2 or 4. |
| `\RuntimeException` | `no default font: set extra.baremetal.fonts.default to true or pass a font` | `text()`, `textWidth()` without `font:` in a firmware built with `fonts.default` set to `false`. |
| `\ValueError` | `<Class>::image(): Argument #3 ($w) must be between 0 and 65536` (`#4 ($h)` likewise) | `image()` |
| `\ValueError` | `<Class>::image(): Argument #5 ($pixels) must be N bytes long (w * h * 2), got M` | `image()` |
| `\ValueError` | `<Class>::backlight(): Argument #1 ($level) must be between 0 and 255` | `backlight()` |
| `\ArgumentCountError` | `<Class>::flush() takes all four coordinates or none, N given` | `flush()` with one to three coordinates. |
| `\ValueError` | `<Class>: vsync needs the TE pin (te:)` | the driver's constructor with `vsync` and no TE pin (for example `Baremetal\Spi\Driver\St77916: vsync needs the TE pin (te:)`). |
| `\ValueError` | `<Panel>::onVsync(): the display has no TE pin` | `onVsync()` on a display built without a TE pin. |
| `\Error` | `<Panel>::onFlushed(): unknown class '<name>' or too many sources` (`onVsync()` likewise) | `onFlushed()`, `onVsync()`: the class does not exist or the event source table is full, as for the other event sources. |
| `\Error` | `<Panel> is not initialised` | any method of an object without a surface: its driver did not attach one, or the object is a `clone`. |
| `\Error` | `Call to private Baremetal\Display\Font::__construct() ...` | `new Font()`: use `Font::get()`. |
| `\Error` | `Trying to clone an uncloneable object of class Baremetal\Display\Font` | `clone` of a font. |

The error code in the messages is the driver's `bm_err_t` (on ESP32 an `esp_err_t`, such as `0x107` for
`ESP_ERR_TIMEOUT`).

## Build errors for fonts

| Message | When |
| --- | --- |
| `font '<name>' is defined twice (<source1>, <source2>)` | Two fonts with the same name, from any source, `default` included. |
| `font '<name>': source '<path>' not found` | A `bake` source is missing. |
| `font '<name>': '<file>' is not a TTF or BDF font` | The source cannot be read. |
| `font '<name>': size is required for a TTF source` | A TTF `bake` entry without `size`. |
| warning `font '<name>': <n> characters not in the source (...)` | Characters of `chars` the source does not have are skipped. |
| `font file '<path>' is not a GFX font header` | A project or package `.h` without a `GFXfont` or `display_font_t`. |
| `fonts need php-baremetal/ext-display: composer require php-baremetal/ext-display` | Fonts configured in a project without ext-display. |

## Limits

| What | Value |
| --- | --- |
| Framebuffer | `width * height * 2` bytes, allocated with the panel object, outside `memory_limit`. |
| Transfer band | Up to 14 400 bytes; a region is pushed in bands of whole rows (one push when it fits). An asynchronous driver chooses its own bands. |
| Copy (`buffered`) | Another `width * height * 2` bytes, allocated with the panel object, outside `memory_limit`. |
| Flushes | One region in flight and one pending; another `flush()` merges into the pending one. |
| Waiting for a flush | 2000 ms in all, then `flush timed out`. |
| Events | One `onFlushed()` and one `onVsync()` class per display; `Flushed` events are not coalesced (one slot each, dropped and counted by ext-events when the pool is full), `onVsync()` events are. |
| Coordinates and sizes | Clamped to -65536..65536 before drawing; `image()` refuses a `w` or `h` outside 0..65536. |
| Panel formats | RGB565. A monochrome format is defined in the C contract but not implemented yet. |
| Font bitmaps | 1, 2 or 4 bits per pixel; at most 64 KB of bitmap per font. |
| Missing character | A hollow box half the line height wide. |
| Text width | Characters past the right edge are counted, not drawn; widths saturate at 2147483647. |
| Invalid UTF-8 | Each invalid byte counts as U+FFFD. |

## Frame rates

Measured on a Waveshare ESP32-S3-Touch-LCD-1.85B (ST77916, 360 x 360 RGB565 over QSPI at 40 MHz) with PHP 8.5,
on 2026-11-05, with a timing loop in PHP using `hrtime()`:

| Operation | Swap at send (`-DDISPLAY_FB_PRESWAP=0`) | Pre-swapped framebuffer (default) |
| --- | --- | --- |
| `clear()` + `flush()` of the whole screen | 57.0 ms (17.5 fps) | 38.0 ms (26.3 fps) |
| `clear()` only (framebuffer) | 10.4 ms | 10.4 ms |
| 60 x 60 square moved + region flush | 2.0 ms (500 fps) | 2.0 ms (500 fps) |
| Full-width 360 x 60 band + flush | 9.0 ms (111 fps) | 7.0 ms (143 fps) |

- The figures are multiples of 1 ms because each band waits for the DMA on a FreeRTOS tick.
- A full flush moves 259,200 bytes in 9 bands of 40 rows.
- Colours were checked on the panel with both byte orders.

The table above is the synchronous `flush()` of 2026-11-05. With the asynchronous flush (same board, pre-swapped
framebuffer, 2026-11-06; flush task with two ping-pong bands of 20 rows):

| Operation | `flush()` returns after | Transfer (`flushWait()`) |
| --- | --- | --- |
| Whole screen | 1.5 ms | 30.0 ms (38.0 ms synchronous) |
| Whole screen, `buffered: true` (the frame is copied first) | 13.6 ms | 33.5 ms |
| 60 x 60 square | 0.6 ms | 2.1 ms |
| 60 x 60 square, `buffered: true` | 0.7 ms | 1.2 ms |

| Loop | Rate |
| --- | --- |
| `clear()` + `flush()` of the whole screen | 34.4 fps (30.0 fps with `buffered: true`: the copy runs on the PHP task) |
| `06-display/display-hello`, `06-display/display-hello-event-driven` (paced by `onVsync()`) | 60.0 fps |

- A drawing right after a whole-screen `flush()` waits for the rows it touches: about 28 ms for another full-screen
  `fillRect()`, nothing with `buffered: true`.
- `onVsync()`: 60 events per second, `$ticks` 1, delivered about 0.4 ms after the TE pulse. With `vsync: true` the
  region that starts at the same pulse delays the event by about its transfer time (about 2.5 ms for a 20 x 60
  band).
- `vsync: true`: a 60 x 120 rectangle moving at 240 px/s never tears; without it, its edge breaks now and then near
  its top.
- A QMI8658 polled at 100 Hz keeps its 100 samples per second next to a 60 fps display.
