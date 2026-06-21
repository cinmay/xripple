# Hyprland Xripple Prototype

This folder contains the first Hyprland-specific port of `xripple`.

The prototype keeps the original X11 code untouched and tests the proposed
Wayland architecture:

- create one bottom layer-shell surface per output
- position that surface over the existing wallpaper and below normal windows
- capture one source scanline above the water band with screencopy
- store captured rows in a ring buffer
- render the distorted water band from that buffer
- collect simple timing counters from the start

This is intentionally not a generic Wayland wallpaper project. The target is
Hyprland first, and preserving live reflection is the core requirement.

## Build

```sh
make -C hyprland
```

Requirements:

- C99 compiler
- `pkg-config`
- `wayland-client`
- `wayland-scanner`
- `wayland-protocols`

The wlroots layer-shell and screencopy protocol XML files are vendored under
`protocols/` so the prototype can build on systems that do not package them
separately.

## Run

Run from inside a Hyprland session:

```sh
./hyprland/build/hypripple
```

Benchmark a fixed number of rendered frames:

```sh
./hyprland/build/hypripple --benchmark --max-frames 600
```

Useful options:

```text
--water-height N   Height of the ripple band in logical pixels, default 255
--max-frames N     Exit after rendering N total frames
--benchmark        Print benchmark-oriented stats on exit
--verbose          Print setup and runtime diagnostics
```

Hyprland may require screencopy permission depending on its permissions config.
If the app starts but captures fail, check the `screencopy` permission for the
executable.

## Current Limitations

- The renderer is CPU and shared-memory based.
- Fractional scale handling is not final.
- Only XRGB/ARGB 32-bit screencopy formats are accepted.
- The prototype favors measurement and correctness over polish.

The next decision point is empirical: if scanline screencopy plus CPU rendering
stays cheap at 4K 60Hz, this standalone approach remains the simplest path. If
it does not, the next candidate is a Hyprland render plugin.
