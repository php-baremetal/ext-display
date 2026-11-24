# Changelog

## 1.2.0

- Added: the `display-surface` option (bool, default `true`, `PHPBM_OPTION_DISPLAY_SURFACE`). `false`: a driver
  attaches without the drawing surface and nothing is allocated for it (no framebuffer, band or copy; with the SDK,
  the turned MIPI-DSI panel's surface framebuffer is not reserved either); a renderer still claims and drives the
  panel; every drawing method, `image()`, `flush()` and `flushWait()` throws `\Error` naming the option, also on a
  panel whose surface would draw in the driver's framebuffer.
- Added: the display_drivers registry (`display_driver_desc_t`): a panel driver outside the bus extensions gets its class `Baremetal\Display\Driver\<Name>`, an Output\Display.
- Added: `display_panel_ops_t::fb`: a driver whose panel scans a framebuffer by itself gives it, the surface draws there and allocates none, and flush() only makes the changed rectangle visible.
- Added: `display_attach_opts_t::te_driver` and `display_te_pulse()`: the frame pulses of a panel without a TE pin (an RGB panel's vsync) feed onVsync(), the vsync flushes and display_vsync_hook().
- Added: a double buffer on the band path (`display_band_ops_t::frames`, `present`, `show_main`): the bands go to
  a hidden frame, `present()` shows it from the panel's next frame and the next `begin()` waits for the switch (a
  switch that does not come within the driver's timeout gives `BM_ERR_TIMEOUT`, a transient error); the renderer
  does not race such a panel; `display_release()` calls `show_main()`, which shows the main frame (the
  surface's) again with the last picture. `display_attach_ex()` refuses an inconsistent set.
- Added: `display_band_ops_t::carry` (optional, frames 2 only): the driver copies a rectangle from the frame shown
  into the hidden frame, in place or from an offset, in order with the bands, so a renderer brings the hidden frame up
  to date without composing the changes of the frame before again, and moves a scrolled content without composing
  it. `display_attach_ex()` refuses it with one frame.
- Added: `display_band_ops_t::cpu_order`: the driver takes its bands in the CPU's byte order, so the renderer does
  not swap them.

## 1.1.0

- Added: `display_text.h`, header-only text helpers (UTF-8 decoding, a GFX or extended font seen the same way, glyph lookup and advance), so a renderer that draws text itself gets the same metrics as `text()`.
- Added: the band path of the panel contract (`display_band_ops_t`, optional: `display_panel_ops_t::band`, `NULL` for none), for renderers that compose outside the framebuffer, and `display_panel_of()` / `display_claim()` / `display_release()`: while a renderer holds the panel the drawing methods throw `\LogicException`. The band ops are declared in `display_band.h` (plain C, included by `display_panel.h`).
- Added: `display_vsync_hook()` in the C API: a renderer's callback in the TE interrupt at every pulse, beside `onVsync()`.
- Added: `display_band_ops_t::scan` (`display_scan_t`), the order in which the panel refreshes its picture, so a
  renderer paced on TE can write its bands where the refresh is not reading and the picture does not tear;
  `DISPLAY_SCAN_UNKNOWN` (0) when the driver does not set it.
- Added: `display_claim_ex()`, `display_claim()` with a callback that tells the renderer holding the panel about
  `sleep()` (before), `wake()` (after) and a lost picture (`display_invalidate()`), so it pauses while the panel is
  off and sends its whole frame again after a re-initialisation.
- Added: the band contract's failure rule: a panel that failed for good returns `BM_ERR_INVALID_STATE` from every
  later `begin` and `push`, and only then; the renderer takes that code as final.

## 1.0.0

- First release.
- Added: ide/ext-display.stub, the PHP declarations of the package for IDEs (conductor copies it into vendor/php-baremetal/ide-stubs/).
- Asynchronous flush: with a driver that has `begin`/`wait`, `flush()` starts the region and returns; a drawing
  waits only for the rows not yet sent that it touches; a `flush()` during another one is pending (one region,
  merged). A failed transfer throws `<Panel>: push failed (error 0x..)` at the next call; a wait longer than
  2000 ms in all throws `<Panel>: flush timed out`. A display freed or attached again drops its pending region.
  `sleep()`, `wake()` and `backlight()` wait for the flush first; with `vsync`, `sleep()` and `wake()` drop a
  region still waiting for the TE pulse instead of waiting for it.
- `flushWait(): void`: returns when no flush is in flight or pending.
- Driver options through `display_attach_ex()` and `display_attach_opts_t`: `buffered` (a copy of the framebuffer,
  so drawings never wait), `vsync` (each region starts at the next TE pulse; without a TE pin the driver throws
  `\ValueError`) and `te` (the TE pin, whose interrupt ext-display owns; several objects of one panel share the
  pin, and freeing one of them leaves the pulses to the others).
- Events: `onFlushed(string $event): Baremetal\Source` (the end of every region sent; `Baremetal\Display\Flushed`
  with `$x0`, `$y0`, `$x1`, `$y1`, `$us`) and `onVsync(string $event): Baremetal\Source` (every TE pulse,
  coalescing; `Baremetal\TimerEvent` with `$ticks`), with the handles `Baremetal\Display\FlushSource` and
  `Baremetal\Display\VsyncSource`. The `onVsync()` event is raised at the pulse, before a `vsync` region starts. `onVsync()` without a TE pin throws `\ValueError`
  `<Panel>::onVsync(): the display has no TE pin`; an unknown class `\Error`
  `<Panel>::onFlushed(): unknown class '...' or too many sources`.
- C contract for drivers: `display_job_t`, the `begin`/`wait` entries of `display_panel_ops_t` (`begin` callable
  from an interrupt; `wait` blocks until a band or job ends or `ms` pass, false on timeout; ext-display waits in
  short slices under one 2000 ms deadline, so a dead TE pin ends in `flush timed out`),
  `display_rows_sent()` / `display_job_done()`, `display_attach_ex()` and `display_quiesce()`.
- Requires `php-baremetal/ext-events` `^1.1` and `php-baremetal/events-hal` `^1.4`.
