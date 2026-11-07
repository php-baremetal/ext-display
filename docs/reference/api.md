---
eyebrow: 'ext-display · Reference'
lede:    'Every method of Baremetal\Output\Display, Baremetal\Display\Font and the display events, with the signatures the extension registers.'
see_also:
  - { href: './errors-and-limits.md', meta: 'Reference' }
  - { href: '../overview.md', meta: 'Overview' }
prev: { label: 'Overview', href: '../overview.md' }
next: { label: 'Errors and limits', href: './errors-and-limits.md' }
---

# PHP API

## Output Display

`Baremetal\Output\Display` is an interface: panel driver classes implement it. Coordinates are pixels from the
top-left corner; rectangles are `x, y, w, h`; regions are `[x0, x1) x [y0, y1)`. Colors are RGB565 integers (the
low 16 bits are used).

| Method | Meaning |
| --- | --- |
| `width(): int`, `height(): int` | The panel's size in pixels. |
| `rgb(int $r, int $g, int $b): int` | A color in the panel's format from 0..255 channels (values outside are clamped). `rgb(255, 0, 0)` is `0xF800`. |
| `clear(int $color): void` | Fills the framebuffer. |
| `pixel(int $x, int $y, int $color): void` | One pixel. |
| `line(int $x0, int $y0, int $x1, int $y1, int $color): void` | A line, both ends included. |
| `rect(int $x, int $y, int $w, int $h, int $color): void` | The outline of a rectangle; a width or height of 1 is a line. |
| `fillRect(int $x, int $y, int $w, int $h, int $color): void` | A filled rectangle. |
| `circle(int $cx, int $cy, int $r, int $color): void` | The outline of a circle; `r` 0 is one pixel. |
| `fillCircle(int $cx, int $cy, int $r, int $color): void` | A filled circle. |
| `text(int $x, int $y, string $s, int $color, ?int $bg = null, ?Font $font = null): int` | UTF-8 text with `y` the top of the line; returns the width drawn. With `bg` the background of each character cell is painted and anti-aliased glyphs blend against it. |
| `textWidth(string $s, ?Font $font = null): int` | The width `text()` would draw. |
| `image(int $x, int $y, int $w, int $h, string $pixels): void` | `w * h` pixels in the panel's format, rows top to bottom, two bytes per pixel with the high byte first. |
| `flush(?int $x0 = null, ?int $y0 = null, ?int $x1 = null, ?int $y1 = null): void` | Sends the framebuffer to the panel; see below. |
| `flushWait(): void` | Returns when no flush is in flight or pending; throws the error of one that failed. |
| `onFlushed(string $event): Baremetal\Source` | Raises `$event` at the end of every region sent; see [Events](#events). |
| `onVsync(string $event): Baremetal\Source` | Raises `$event` at every pulse of the panel's TE pin; see [Events](#events). |
| `backlight(int $level): void` | 0 (off) to 255; nothing on a panel without a backlight. |
| `sleep(): void`, `wake(): void` | The panel's sleep mode; nothing on a panel without one. |
| `refreshRate(): float` | The panel's nominal refresh rate in Hz, or `0.0` when the driver does not say. It is the datasheet or configured rate, not a measurement. |

Every drawing method clips to the screen without errors, and a width or height of 0 or less draws nothing.

## Flush

- `flush()` pushes the rectangle touched since the last flush: the union of everything drawn, clipped to the
  screen. Nothing is sent when nothing changed.
- `flush($x0, $y0, $x1, $y1)` pushes that region, clipped. A region that is empty after clipping sends nothing.
  When it covers everything drawn since the last flush, the next `flush()` has nothing to send.
- Give all four coordinates or none.
- With a driver that has the asynchronous contract, `flush()` starts the region and returns without waiting for
  the transfer. A region given while another one is in flight is pending and starts when that one ends; at most
  one region is pending, and another `flush()` merges into it.
- A drawing waits only when it touches rows of the region in flight not yet sent, or the pending region. With
  the driver's `buffered` option it never waits: `flush()` copies the region first (waiting for the copy still
  being sent, if any).
- With the driver's `vsync` option every region starts at the next pulse of the TE pin.
- `backlight()` waits for the flush in flight and the pending one. `sleep()` and `wake()` wait for the flush in
  flight; with `vsync`, a region still waiting for the TE pulse is dropped first (the panel is going off, or is
  woken and gets the whole frame again from a driver that re-initialises it).
- A push that fails throws: with a synchronous driver in `flush()`, with an asynchronous one at the next call on
  the display. The region goes back into the dirty rectangle, so the next `flush()` sends it again.
- Waiting more than 2 s in all throws `<Panel>: flush timed out`, for example with `vsync` and a TE pulse that
  never comes.

<!-- @code-block language="php" label="partial update" -->
```php
<?php
$d->fillRect(0, 0, 120, 20, $bg);
$d->text(0, 0, date('H:i:s'), $fg, $bg);
$d->flush();                     // only the 120 x 20 band goes to the panel
```
<!-- @endcode-block -->

## Events

`onFlushed()` and `onVsync()` register `$event` as an event source of ext-events (the class must extend
`Baremetal\Event`) and return its handle. The handle (`Baremetal\Display\FlushSource` or
`Baremetal\Display\VsyncSource`, both final subclasses of `Baremetal\Source`) has `stop()`, `start()` and
`running()`; the source belongs to the display, and a discarded handle leaves it on.

| | `onFlushed()` | `onVsync()` |
| --- | --- | --- |
| Raised | at the end of every region sent (none for a region that failed) | at every falling edge of the TE pin |
| Coalesces | no: one event per region | yes: an event waiting absorbs the pulses after it |
| Fields | a subclass of `Baremetal\Display\Flushed` gets `$x0`, `$y0`, `$x1`, `$y1`, `$us` | a subclass of `Baremetal\TimerEvent` gets `$ticks` |
| Needs | nothing | the TE pin (the driver's `te`); without one `\ValueError` |

- `$e->device` is the display object; `$e->time` the end of the flush or the pulse.
- One class per event and per display: a second call replaces the first, whose handle then reports `running()`
  `false` (and `start()` does not bring it back).
- `stop()` of the `onVsync()` handle keeps the TE interrupt, which `vsync` may need.
- The first call keeps the display object alive until the end of the script.

### Display Flushed

<!-- @code-block language="php" label="Baremetal\Display\Flushed" -->
```php
class Baremetal\Display\Flushed extends Baremetal\Event {
    public readonly int $x0;   // the region, [x0, x1) x [y0, y1)
    public readonly int $y0;
    public readonly int $x1;
    public readonly int $y1;
    public readonly int $us;   // the transfer time in microseconds
}
```
<!-- @endcode-block -->

## Display Font

`Baremetal\Display\Font` is final and has no public constructor.

| Method | Meaning |
| --- | --- |
| `Font::get(string $name): Font` | A font of the firmware by name. |
| `Font::names(): array` | The names of every font in the firmware, in name order. |
| `name(): string` | Its name. |
| `height(): int` | The line advance in pixels. |
| `ascent(): int` | Pixels from the top of the line to the baseline. |

<!-- @code-block language="php" label="fonts" -->
```php
<?php
use Baremetal\Display\Font;

$title = Font::get('title');
$y = 10;
$d->text(10, $y, 'Living room', $d->rgb(255, 255, 255), null, $title);
$y += $title->height();
$d->text(10, $y, '21.5 °C', $d->rgb(255, 200, 0));        // the default font
```
<!-- @endcode-block -->
