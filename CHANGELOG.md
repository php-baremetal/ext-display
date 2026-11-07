# Changelog

## 1.0.0

- First release.
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
