# ext-display

A drawing surface for the displays on your board. A panel driver (for example `php-baremetal/esp32-display-st77916`)
gives you an object that implements `Baremetal\Output\Display`: you draw lines, rectangles, circles, images and
text into a framebuffer, then `flush()` sends what changed to the panel (with drivers that support it, without
waiting for the transfer: `flushWait()` waits). `onFlushed()` and `onVsync()` raise ext-events events at the
end of every flush and at every pulse of the panel's TE pin. Text uses bitmap fonts:
`Baremetal\Display\Font` finds the fonts built into the firmware, `default` included (DejaVu Sans Mono 16 px,
U+0020 to U+00FF).

ext-display is platform-neutral: it uses the events HAL contract of ext-events for its lock and the TE
interrupt, and panel drivers plug in through the C contract in `display_panel.h`.

## Install

You normally get ext-display through a panel driver package, which requires it. To require it yourself, in a
project created with `phpbm init`:

```bash
composer require php-baremetal/ext-display
```

## Example

```php
<?php
use Baremetal\Output\Display;

function hello(Display $d): void
{
    $d->clear($d->rgb(0, 0, 40));
    $d->fillCircle(intdiv($d->width(), 2), intdiv($d->height(), 2), 40, $d->rgb(255, 160, 0));
    $w = $d->textWidth('Hello');
    $d->text(intdiv($d->width() - $w, 2), 20, 'Hello', $d->rgb(255, 255, 255));
    $d->flush();                         // nothing reaches the panel before this
}
```

## Documentation

- [Overview](docs/overview.md): requirements, install, fonts, notes.
- [PHP API](docs/reference/api.md): `Output\Display`, the display events and `Display\Font`.
- [Errors and limits](docs/reference/errors-and-limits.md)
- [C API for panel drivers](docs/reference/c-api.md)

## The default font

`fonts/default.h` was generated with:

```bash
phpbm font:bake DejaVuSansMono.ttf --name=default --size=16 --range=0x20-0x7E,0xA0-0xFF --out=fonts
```

from `ttf/DejaVuSansMono.ttf` of DejaVu fonts 2.37 (U+007F to U+009F are control codes and are not in the font).

## License

MIT. `fonts/default.h` is DejaVu Sans Mono, under the Bitstream Vera and DejaVu fonts license; both texts are
in `LICENSE`.
