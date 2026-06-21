# Hyprland Port Design Notes

## Goal

Preserve the original `xripple` purpose on Hyprland: a water band at the bottom
of the desktop that reflects live windows and menus. This is not just an
animated wallpaper.

## Current Prototype

The first implementation is a standalone Hyprland/Wayland client:

1. Bind `wl_compositor`, `wl_shm`, `zwlr_layer_shell_v1`,
   `zwlr_screencopy_manager_v1`, and each `wl_output`.
2. Create one layer-shell surface per output.
3. Place each surface on the `bottom` layer, anchored left/right/bottom, with no
   exclusive zone and an empty input region.
4. Before the first visible frame, prefill the two-water-height source ring with
   one screencopy of the whole source band above the water.
5. After every later frame callback, request a configurable chunk of rows from
   the same source band.
6. Store captured rows in the ring buffer.
7. Render the ripple strip into a double-buffered SHM surface.

This keeps the app layered over the existing wallpaper while staying below
normal windows.

Screencopy regions are requested in logical output coordinates, but the returned
buffer may be in physical pixels on scaled outputs. The prototype stores the
requested logical capture size with every frame and downsamples the returned
buffer into the logical ripple ring.

## Why Screencopy First

The original X11 program samples the visible desktop and writes a distorted
version into the root window. The closest standalone Wayland equivalent is:

- capture a small region of the final output
- draw the distorted result into a layer surface

The startup prefill avoids a black warm-up band. After that, the default capture
chunk is 32 rows per update. At 3840 pixels wide, one XRGB scanline is about
15 KiB, so 32 rows is about 480 KiB per capture. The larger recurring cost may
still be rendering and submitting the water strip, roughly:

```text
3840 * 255 * 4 * 60 = ~235 MiB/s
```

That is plausible on CPU, but must be measured on the target system.

## Known Risks

- Hyprland may require explicit `screencopy` permission.
- Screencopy may add latency or synchronization overhead even for a one-row
  capture.
- Fractional scale and transformed outputs need more work.
- SHM rendering may become too expensive on multi-monitor 4K setups.
- If this approach cannot hit the performance target, a Hyprland render plugin
  is the next option.

## Plugin Fallback

A Hyprland plugin would render compositor-side, avoid screencopy permission, and
likely offer the best performance ceiling. The tradeoff is maintenance cost:
Hyprland plugins cross C++ internals and can be sensitive to compositor version
changes.

The current prototype should answer whether that cost is necessary.

## Immediate Benchmark Questions

- Does one-row screencopy complete reliably at 60Hz?
- How do 16-row, 32-row, and full-band capture compare?
- What is average and worst-case render time for a 4K-wide 255px water band?
- Does CPU usage stay acceptable at idle?
- Does the layer ordering correctly place the effect above wallpaper and below
  normal windows?
- Are menus and moving windows reflected with acceptable latency?
