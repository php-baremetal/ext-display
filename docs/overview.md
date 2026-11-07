---
eyebrow: 'Package · Extension'
lede:    'A framebuffer with drawing primitives, bitmap-font text and an asynchronous region flush behind Baremetal\Output\Display, with events at the end of a flush and at every refresh of the panel. Panel drivers plug in underneath; your script only sees the interface.'
see_also:
  - { href: './reference/api.md', meta: 'PHP API' }
  - { href: './reference/c-api.md', meta: 'Drivers' }
  - { href: 'package:php-baremetal/esp32-display-st77916', meta: 'Driver', label: 'esp32-display-st77916' }
prev: { label: 'No previous page', href: '#' }
next: { label: 'PHP API', href: './reference/api.md' }
---

# Display surface

ext-display gives every display panel the same PHP API. A panel driver creates an object that implements
`Baremetal\Output\Display`; drawing calls (`clear()`, `line()`, `fillRect()`, `text()`, `image()`, ...) write
into a framebuffer in memory, and `flush()` sends the part that changed to the panel. `Baremetal\Display\Font`
picks one of the bitmap fonts built into the firmware.

## Requirements

| Requires | Why |
| --- | --- |
| `php-baremetal/bm-common` | The shared `bm_err.h` error codes of the panel contract. |
| `php-baremetal/ext-events` `^1.1` | `onFlushed()` and `onVsync()` are event sources (`Baremetal\Source` handles, `Baremetal\Event` classes). |
| `php-baremetal/events-hal` `^1.4` | The lock shared with the driver task and the TE interrupt, and the GPIO interrupt of the TE pin. |

ext-display conflicts with php-baremetal/ext-spi older than 1.1.0: those releases still register `Output\Display`, which
ext-display now owns.

ext-display has no platform code: the lock and the GPIO interrupt come from the events HAL contract, which the
SDK implements. The panel driver brings the bus (for example ext-spi) and the panel's start-up sequence. The PHP module is `display`; it starts before the bus extensions, so a driver
finds `Output\Display` already registered.

## Install

<!-- @steps -->
- **Install conductor once per machine**

  `composer global require php-baremetal/conductor`

- **Create the project**

  `phpbm init my-display`

- **Require a panel driver**

  `composer require php-baremetal/esp32-display-st77916` (it requires ext-display).

- **Build and flash**

  `phpbm flash`, then `phpbm monitor` for the console.
<!-- @endsteps -->

The resulting `composer.json`, for a Waveshare ESP32-S3-Touch-LCD-1.85B:

<!-- @code-block language="json" label="composer.json" -->
```json
{
  "name": "acme/my-display",
  "type": "project",
  "repositories": [
    { "type": "composer", "url": "https://repository.php-baremetal.com", "canonical": true }
  ],
  "require": {
    "php-baremetal/conductor": "^1.0",
    "php-baremetal/ext-spi": "^1.0",
    "php-baremetal/esp32-display-st77916": "^1.0"
  },
  "config": {
    "allow-plugins": { "php-baremetal/conductor": true },
    "platform": { "php": "8.5.11" }
  },
  "extra": {
    "baremetal": {
      "sdk": "esp32@^2.0",
      "board": "waveshare-esp32-s3-touch-lcd-1.85b",
      "runtime": "init-loop",
      "storage": "embedded"
    }
  }
}
```
<!-- @endcode-block -->

## Drawing

<!-- @code-block language="php" label="draw.php" -->
```php
<?php
use Baremetal\Display\Font;
use Baremetal\Output\Display;

function draw(Display $d): void
{
    $bg = $d->rgb(0, 0, 0);
    $d->clear($bg);
    $d->rect(0, 0, $d->width(), $d->height(), $d->rgb(80, 80, 80));   // outline
    $d->line(0, 0, $d->width() - 1, $d->height() - 1, $d->rgb(0, 255, 0));
    $d->text(10, 10, 'Température 21 °C', $d->rgb(255, 255, 255), $bg);
    $d->flush();                                                       // the union of all of the above
}
```
<!-- @endcode-block -->

- Drawing only writes the framebuffer. `flush()` sends the rectangle touched since the last flush (nothing if
  nothing changed); `flush($x0, $y0, $x1, $y1)` sends that region.
- Every primitive is clipped to the screen; a width or height of 0 or less draws nothing.
- Colors are RGB565 integers: `rgb()` converts from 8-bit channels.

## Asynchronous flush

With a driver that supports it (esp32-display-st77916 does), `flush()` starts the transfer and returns at once;
the panel reads the framebuffer while your script goes on.

- A drawing that touches rows not yet sent waits until they are; a drawing elsewhere does not wait. With a
  `clear()` every frame the next frame waits for the whole previous one, as before; with small dirty rectangles
  the waits are rare and short.
- A `flush()` while another one is in flight is pending: it starts as soon as the first one ends. At most one
  region is pending; another `flush()` merges into it (the rectangle that holds both).
- `flushWait()` returns when nothing is in flight or pending. `sleep()`, `wake()` and `backlight()` wait for the
  flush first; with `vsync`, `sleep()` and `wake()` drop a region still waiting for the TE pulse instead of
  waiting for it.
- A transfer that fails throws its `push failed` error at the next call on the display (a drawing, `flush()`,
  `flushWait()`, `sleep()`, `wake()`, `backlight()`), and its region goes back into the dirty rectangle.
- A driver without the asynchronous contract keeps the synchronous `flush()`: it returns when the region is on the
  panel, and `flushWait()` returns at once.

<!-- @code-block language="php" label="draw while the frame goes out" -->
```php
<?php
$d->fillRect($x, 0, 60, 60, $fg);
$d->flush();                       // returns before the DMA ends
$status = read_sensors();          // runs during the transfer
$d->flushWait();                   // before anything that must see the frame on the panel
```
<!-- @endcode-block -->

### Driver options

Drivers that support them take these options next to their own (see the driver's README for the names of its
constructor):

| Option | Default | Effect |
| --- | --- | --- |
| `buffered` | `false` | A second framebuffer (`width * height * 2` bytes, 259 200 for 360 x 360): `flush()` copies the region there and the panel reads the copy, so drawings never wait. A `flush()` while the copy is still being sent waits for it. |
| `vsync` | `false` | Every region starts at the next pulse of the panel's TE (tearing effect) pin, so the panel never shows half of an old frame. Needs the TE pin: without one the constructor throws `\ValueError`. Without a pulse within 2 s, a wait for the flush throws `flush timed out`. |
| `te` | `-1` | The GPIO of the panel's TE output, needed by `vsync` and `onVsync()`; `-1` means none. Boards that describe their display give it (`onboard()`). |

The TE pin belongs to the display: a `watch_gpio()` on the same pin takes the interrupt away from it.

## Events

Two methods turn the display into an event source of [ext-events](package:php-baremetal/ext-events). Each returns
a `Baremetal\Source` handle (`stop()`, `start()`, `running()`); the source belongs to the display, so a discarded
handle leaves the event on. One class per event and per display: a second call replaces the first, and the old
handle's `running()` turns `false`.

| Method | Raised | Coalesces | Fields |
| --- | --- | --- | --- |
| `onFlushed(string $event): Baremetal\Source` | at the end of every region sent | no | with a subclass of `Baremetal\Display\Flushed`: `$x0`, `$y0`, `$x1`, `$y1` (the region, `[x0, x1) x [y0, y1)`) and `$us` (the transfer time in microseconds) |
| `onVsync(string $event): Baremetal\Source` | at every pulse of the TE pin | yes | with a subclass of `Baremetal\TimerEvent`: `$ticks` (1 plus the pulses merged while the event waited) |

`$e->device` is the display, and `$e->time` the end of the flush or the pulse. `onVsync()` needs the TE pin, not
the `vsync` option. A region that fails raises no `Flushed` event: its error is thrown by the next call.

<!-- @code-block language="php" label="the panel as metronome" -->
```php
<?php
use Baremetal\Events;
use Baremetal\Spi\Driver\St77916;
use Baremetal\TimerEvent;

final class Frame extends TimerEvent {}

$lcd = St77916::onboard(vsync: true);
$lcd->onVsync(Frame::class);
Events::listen(Frame::class, function (Frame $e) use ($lcd): void {
    draw($lcd);          // $e->ticks > 1: frames were missed
    $lcd->flush();       // starts at the next pulse
});
```
<!-- @endcode-block -->

<!-- @code-block language="php" label="timing each region" -->
```php
<?php
use Baremetal\Display\Flushed;

final class Sent extends Flushed {}

$lcd->onFlushed(Sent::class);
Events::listen(Sent::class, function (Sent $e): void {
    printf("%dx%d in %d us\n", $e->x1 - $e->x0, $e->y1 - $e->y0, $e->us);
});
```
<!-- @endcode-block -->

The first `onFlushed()` or `onVsync()` keeps the display object alive until the end of the script, since queued
events carry it.

## Frame pacing

`refreshRate()` returns the panel's nominal refresh rate in Hz (`0.0` when the driver does not know it). With
[ext-events](package:php-baremetal/ext-events) (whose `every()` takes a float since 1.1.0) it gives one frame per
panel refresh:

<!-- @code-block language="php" label="one frame per refresh" -->
```php
<?php
every(1000 / $lcd->refreshRate(), Frame::class);   // milliseconds between two refreshes
```
<!-- @endcode-block -->

The rate is nominal: it drifts against the real one, so a timer started from it slowly slips relative to the
panel's own scan. With the TE pin, `onVsync()` follows the real scan instead (see [Events](#events)). Check
`refreshRate() > 0` first, since `1000 / 0.0` is a division by zero.

## Fonts

Fonts are compiled into the firmware. `Font::names()` lists them and `Font::get('<name>')` returns one; without
a `font:` argument `text()` and `textWidth()` use `default`, DejaVu Sans Mono 16 px from U+0020 to U+00FF,
which ext-display ships.

The project's `extra.baremetal.fonts` chooses the rest:

<!-- @code-block language="json" label="composer.json (extra.baremetal)" -->
```json
"fonts": {
  "default": true,
  "dir": "project-fonts",
  "bake": [
    { "name": "title", "source": "fonts-src/Roboto.ttf", "size": 28, "bpp": 2, "chars": "Home °C%" }
  ]
}
```
<!-- @endcode-block -->

- `default: false` leaves the default font out of the firmware.
- Every `.h` in `dir` is a font named after the file: an Adafruit GFX header (`fontconvert`) or one made by
  `phpbm font:bake`.
- `bake` entries are rasterised from TTF or BDF at build time; `bpp` 2 or 4 gives anti-aliased text.
- Font packages (`font-*`) add theirs by name.

Text is UTF-8; `y` is the top of the line. Anti-aliased glyphs blend against `bg` when it is given; without it a
pixel is drawn when it is more than half covered. A character the font does not have is drawn as a hollow box
half the line height wide.

## Notes

- The framebuffer takes `width * height * 2` bytes outside `memory_limit` (259 200 bytes for 360 x 360), freed
  with the panel object.
- With `buffered`, the copy takes as much again, also outside `memory_limit`.
- See [Errors and limits](./reference/errors-and-limits.md) for every message and size.
