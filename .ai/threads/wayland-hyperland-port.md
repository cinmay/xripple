--- Codex Session ---
ID: 019eea8c-8ecc-7153-a399-d8a299c0938c

# wayland hyprland port

The goal of this project is to port the existing xripple codebase to work with the Wayland compositor Hyprland.

We have to make sure that it is preferment. As this is a live desktop wallpaper application, it needs to be efficient and not consume too much resources.

I'm open to rewriting it in Rust or better yet Zig. Staying in C is also fine. 

We need to adhere to best practices like Yagni, KISS, TDD, Martin Fowler's rule of three etc. We should also make sure to write good documentation and have a clear code structure.
Since we are going for performance, we will prioritize writing efficient code above clean code, but we will still strive to maintain readability and maintainability. 
We will use good full names, not abbreviations or single letter variable names accept for loop counters and such. 

Let's have a discussion about this, pleas ask any clarifying questions or give any suggestions or thoughts you have about this project.

Time: 2026-06-21 16:21:12
--- Codex Run ---

Codex output:
I’ll keep this as architecture discussion, not implementation. I’m going to sanity-check the current Hyprland/Wayland surfaces first so the recommendations are grounded in what the compositor actually exposes today.
I’m also going to read the current C file and build config so the port discussion maps to the actual code, especially the rendering loop and X11 assumptions.
The main architectural question is this:

Do we need to preserve the original “reflect open windows/menus in the water” behavior, or is an animated ripple wallpaper acceptable?

That decides almost everything. The current X11 app reads pixels from the root window and writes directly back to it. Wayland does not have that model. A normal Wayland wallpaper client can render a background layer surface, but it cannot freely read other windows behind/above it. For real live desktop reflection, the efficient path is likely a Hyprland plugin or compositor-side effect, not a standalone wallpaper app.

My recommendation:

1. Keep the first Wayland version as a standalone Hyprland/layer-shell wallpaper client.
2. Render one background surface per monitor using `wlr-layer-shell` on the `background` layer.
3. Start with a CPU `wl_shm` renderer that updates only the ripple band and uses frame callbacks/damage tracking.
4. Add a benchmark mode immediately.
5. Only move to EGL/OpenGL/Vulkan/dmabuf if profiling shows the CPU path is not good enough.

Why: the current effect is mostly row-offset copying over a small bottom band. At 4K with a 256px water band, that is plausibly fine on CPU if we avoid full-screen redraws and busy loops. The current X11 version has an infinite loop with no compositor frame pacing, so the Wayland port should already be more resource-conscious.

Language-wise:

- **C**: smallest jump from current code and best fit for raw Wayland APIs.
- **Zig**: good performance and build story, nice C interop, but Wayland protocol bindings/build glue may take more care.
- **Rust**: strongest ecosystem for Wayland client work and testing, probably best maintainability, but more dependency weight.

If you prefer Zig, I think that is reasonable. I would not rewrite in Rust just for safety unless you are comfortable making Rust the long-term maintenance language.

Suggested structure once we actually start:

```text
src/
  main.zig|main.c|main.rs
  effect.*
  renderer.*
  wayland.*
  config.*
tests/
  effect tests
  benchmark tests
README.md
WAYLAND.md
```

Testing should focus on the pure effect first: deterministic offset calculations, bounds checks, tiny golden-frame output, and a headless benchmark mode like `--benchmark --width 3840 --height 2160 --frames 1000`. Wayland integration will still need manual Hyprland testing.

Clarifying questions:

1. Is live reflection of windows/menus required, or is “animated water over a wallpaper image” enough for v1?
2. Should this target **Hyprland only**, or work on other layer-shell Wayland compositors too?
3. Do you want this to replace `hyprpaper`/`swww`, or layer on top of an existing wallpaper setup?
4. What are your target monitors: resolution, refresh rate, number of displays?
5. Do you want pointer-driven ripples, or only the current passive wave animation?
6. Strong preference among C, Zig, and Rust?

Relevant current docs I checked: Hyprland exposes IPC sockets and events, including layer surface events, but synchronous control socket use needs care: https://wiki.hypr.land/IPC/. The layer-shell protocol is the right normal-client surface model for desktop background layers: https://wayland.app/protocols/wlr-layer-shell-unstable-v1. For later GPU paths, dmabuf/presentation-time may matter: https://wayland.app/protocols/linux-dmabuf-v1 and https://wayland.app/protocols/presentation-time.

Time: 2026-06-21 16:21:12
--- Next Prompt ---
1. The live reflection of windows/menus is very important and the main purpose of this application, it's not simply a live wallpaper.
2. It's fine to target Hyprland only for now. We should not worry about what the future holds, Yagni.
3. It would be ideal if it could layer on top of an existing wallpaper setup.
4. The target monitors are 4K at 60Hz, but it should also work on lower resolutions and refresh rates.
5. Exactly how we achieve the live reflection is up for discussion. Please research different approaches. 
6. I prefer Zig. I'm not too keen on Rust. Rust may be the wrong fit for this project. C is also fine. We can go with C if that works best with the Wayland. Perhaps we can use Zig as the build system so we don't have to deal with C build systems?

I agree that performance monitoring and benchmarking should be a priority from the start.

Should this be en the same repo as xripple, or a new one? The current xripple codebase is quite small, it does one thing ant one thing only. It does it very well. 


Let's continue the discussion.

Time: 2026-06-21 16:36:15
--- Codex Run ---

Codex output:
I’ll research the Hyprland-specific options now, focused on live reflection rather than generic animated wallpaper. The key thing I’m checking is whether an external client can get the needed pixels efficiently, or whether this has to move compositor-side.
**Current Lean**
Given your answers, I’d start with a **Zig standalone Hyprland client** before jumping to a compositor plugin.

Reason: we can probably preserve the core Xripple trick without becoming a Hyprland C++ plugin immediately.

The original code does not capture the whole desktop every frame. It captures **one horizontal scanline per frame** from the area just above the water, stores those rows in a ring buffer, then distorts/copies that buffer into the bottom water band. On Wayland/Hyprland, we can try the same idea:

1. Create a transparent/bottom layer-shell surface anchored to the bottom of each monitor.
2. Put it above the existing wallpaper but below normal windows.
3. Use Hyprland/Wayland screencopy to capture only the source scanline or a narrow source band above the water.
4. Render the distorted water strip into our layer surface.
5. Keep the capture region above the water strip to avoid recursive self-capture.

That gives us live reflection of windows/menus while still layering over an existing wallpaper setup.

**Approaches**
The main options I found:

1. **Standalone layer-shell + screencopy client**
   This is the best first spike. Hyprland has screencopy permissions, and direct screencopy can be allowed/asked/denied in config. Hyprland documents `screencopy` as sensitive compositor access, with examples like `grim`, `wl-screenrec`, and `wf-recorder`: https://wiki.hypr.land/Configuring/Permissions/

   Pros: Zig is viable, no Hyprland plugin ABI pain, works over existing wallpaper, can capture the final visible scene.

   Cons: may require screencopy permission, has async capture latency, and performance must be measured.

2. **Standalone client using Hyprland toplevel export**
   Hyprland has a toplevel export protocol for capturing individual windows: https://raw.githubusercontent.com/hyprwm/hyprland-protocols/main/protocols/hyprland-toplevel-export-v1.xml

   I would not start here. It sounds attractive, but we would need to reproduce window positions, stacking, popups, damage, clipping, monitor transforms, and maybe decorations. That is a lot of compositor logic outside the compositor.

3. **Hyprland plugin**
   This is the most powerful and probably the cleanest long-term performance ceiling. Hyprland plugins can hook render stages, and Hyprland exposes render-related plugin events. The docs list render-stage events and pre-render hooks: https://wiki.hypr.land/Plugins/Development/Event-list/

   Pros: best access to compositor scene, no screencopy permission, lowest-copy path, most control.

   Cons: C++ boundary, Hyprland version coupling, potential compositor crashes, plugin ABI concerns. Hyprland’s own plugin API notes that C++ objects cross the API and ABI compatibility is not guaranteed: https://raw.githubusercontent.com/hyprwm/Hyprland/main/src/plugins/PluginAPI.hpp

   Hyprland’s plugin guidelines also warn that internals may change and function hooks should be a last resort: https://wiki.hypr.land/Plugins/Development/Plugin-Guidelines/

4. **PipeWire / xdg-desktop-portal capture**
   Bad fit for this. It is intended for screensharing, brings UI/permission/session overhead, and is too heavy for a constant desktop effect.

**My Recommendation**
Start in the same repo for now.

The repo is tiny, and this is still conceptually “xripple for Hyprland.” We can preserve `xrp.c` as the legacy X11 implementation and add the Hyprland implementation beside it. If the Hyprland version becomes a full compositor plugin with `hyprpm.toml`, pinned Hyprland versions, and C++ internals, then a split repo may become justified. Not before.

I’d name the first executable something like `xripple-hyprland`, not replace `xripple`.

**Build Direction**
Zig as the build system is reasonable.

For the standalone client, I’d prefer:

```text
build.zig
src/
  main.zig
  wayland_client.zig
  screencopy.zig
  ripple.zig
  render_shm.zig
  benchmark.zig
protocols/
  generated or vendored protocol XML handling
```

If we later need a Hyprland plugin, I’d probably keep the plugin boundary in C++ and let Zig build orchestrate it, but not force Zig to be the plugin language. Hyprland plugins are fundamentally C++-shaped.

**Performance Target**
For 4K 60Hz, the first benchmark should measure:

- screencopy latency per frame
- render time for a 3840x255-ish water strip
- CPU usage while idle desktop is visible
- memory bandwidth from copying rows
- dropped frames
- behavior across multiple monitors

The critical test: can we capture one row or a narrow band at 60Hz without meaningful overhead? If yes, standalone Zig wins. If no, we move toward the plugin.

My proposed next step would be a short design doc plus a proof-of-concept benchmark: no polished config, no image loading, no refactor of the X11 code. Just a Hyprland bottom layer strip that captures the row above it and displays/debug-renders it at 60Hz. That will answer the architecture question with data.

Time: 2026-06-21 16:36:15
--- Next Prompt ---
Ok, go ahead and create a subfolder for the Hyprland version and proceed with your proposal.

Time: 2026-06-21 17:08:37
--- Codex Run ---

Codex output:
I’ll create the Hyprland port in its own subfolder and keep the existing X11 program untouched. First I’m checking which local Wayland/Zig tooling is available so the initial scaffold actually builds here.
Zig is not installed in this environment, but Wayland tooling is. I’m going to make the Hyprland subproject build with plain `make`/`cc` for now, and document the intended Zig build path so we can switch once Zig is available.
The first implementation will be a data-gathering prototype, not the final renderer. I’m going to add a standalone Hyprland client that uses layer-shell plus screencopy, with benchmarking counters built in, and keep the X11 program unchanged.
I’m about to add the new subfolder with a Makefile-driven C prototype. It will vendor the two small protocol XMLs it needs because this machine has `wayland-scanner` but not the wlroots protocol XML package.
The subproject is in place. I’m building it now, which will also validate the vendored protocol XML against `wayland-scanner`.
The first build found a portability issue: this libc exposes `memfd_create` through `sys/mman.h`, not `sys/memfd.h`. I’m tightening that up and rebuilding.
The generated layer-shell code references `xdg_popup` because the protocol includes a popup-parenting request even though this app does not use it. I’m adding generated `xdg-shell` support from the installed `wayland-protocols` package instead of stubbing that symbol.
The prototype builds now. I’m also cleaning up the protocol XML ordering so future builds don’t emit scanner warnings; this does not change the protocol opcodes.
The Hyprland prototype now builds cleanly and the CLI help works. I’m also checking the original X11 build paths, since the repository guidelines call that out even though the legacy source was left alone.
The build creates `hyprland/build/`, so I’m adding a local ignore rule and cleaning generated artifacts. The source remains reproducible via `make -C hyprland`.
I’m doing one more pass on the prototype internals. The first real issue I spotted is screencopy stride handling: the compositor tells us the SHM stride it wants, so the capture buffer should honor that instead of assuming tightly packed rows.
I also added a guard against stalling if both render buffers are still busy; in that case the client schedules the next frame instead of stopping its loop.
The prototype still builds after the stride fix and the CLI help runs. I’m cleaning generated files again so the working tree only contains source files.
I’m adding a short design note as well. The code is useful, but this project needs the architecture decision recorded because the screencopy-vs-plugin fork is the main risk.
The design note is now in the subfolder. I’ll do one final source build check after that documentation-only addition and then report the state.
Implemented the Hyprland prototype in a new [hyprland](/home/cinmay/Documents/xripple/hyprland) subfolder. The original X11 code is untouched.

What’s included:

- [hypripple.c](/home/cinmay/Documents/xripple/hyprland/src/hypripple.c): C99 prototype using layer-shell + screencopy + SHM double buffering.
- [DESIGN.md](/home/cinmay/Documents/xripple/hyprland/DESIGN.md): records the screencopy-first architecture and plugin fallback.
- [README.md](/home/cinmay/Documents/xripple/hyprland/README.md): build/run/benchmark notes.
- Vendored `wlr-layer-shell` and `wlr-screencopy` protocol XMLs.
- [Makefile](/home/cinmay/Documents/xripple/hyprland/Makefile): generates Wayland protocol code and builds `build/hypripple`.

Verification:

- `make -C hyprland` passes cleanly.
- `./hyprland/build/hypripple --help` works.
- Legacy direct build passes: `gcc -O3 -o /tmp/xripple-check xrp.c -lX11 -lm`.
- CMake legacy build was not checked because `cmake` is not installed here.
- Zig is also not installed here, so I used a Makefile/C prototype for now.

I cleaned generated build artifacts afterward. To try it in Hyprland:

```sh
make -C hyprland
./hyprland/build/hypripple --benchmark --max-frames 600 --verbose
```

If captures fail at runtime, the likely first thing to check is Hyprland’s `screencopy` permission for the executable.

Time: 2026-06-21 17:08:37
--- Next Prompt ---

It renders a black strip at the bottom of the screen.

 xripple git:(wayland-hyprland-port) ./hyprland/build/hypripple --benchmark --max-frames 600 --verbose
configured output eDP-1 surface 1920x255, source ring rows 510
output eDP-1: frames=600 captures=600 failed=0 dropped=0 avg_render_ms=0.328 avg_capture_ms=25.985
total: frames=600 captures=600 failed=0 dropped=0 avg_render_ms=0.328 avg_capture_ms=25.985


Time: 2026-06-21 17:23:27
--- Codex Run ---

Codex output:
That black strip is useful signal: captures are succeeding, but the source coordinates are almost certainly wrong on your scaled eDP output. `wlr-screencopy` regions are in output logical coordinates, while the prototype used `wl_output` mode height, which is usually physical pixels. On a 2880x1800 panel scaled to 1920x1200, that puts the capture inside/outside the water strip instead of above it. I’m fixing the coordinate calculation now.
The log confirms this is a scaled output case: the surface is 1920 wide, but the mode height used for capture is likely larger than the logical coordinate space. I’m replacing those mode-height capture coordinates with an estimated logical height derived from mode width vs layer surface width.
The fix builds. It now logs both the raw mode and the estimated logical height so we can verify the coordinate math against your next run.
I’m adding one more verbose line for the first capture geometry, so if it still renders black we can immediately see which logical rows it is sampling.
Fixed the likely cause.

Your log shows a scaled output: layer surface width is `1920`, but `wl_output` mode is probably physical pixels. `wlr-screencopy` capture regions use **logical output coordinates**, so the prototype was sampling too low, likely inside the black water strip or outside the useful source area.

Changed [hypripple.c](/home/cinmay/Documents/xripple/hyprland/src/hypripple.c) to estimate logical output height from:

```text
logical_height = mode_height / (mode_width / surface_width)
```

It now also logs the capture row range in verbose mode.

Rebuilt successfully with:

```sh
make -C hyprland
```

Please rerun:

```sh
./hyprland/build/hypripple --benchmark --max-frames 600 --verbose
```

On a 2880x1800 panel scaled to 1920x1200, the new log should look roughly like:

```text
configured output eDP-1 surface 1920x255, mode 2880x1800, estimated logical height 1200
capture output eDP-1 logical rows 435..944 of 1200, width 1920
```

If it is still black, send that new verbose output; the next thing to inspect is captured pixel content/format.

text
2026-06-21T15:22:23.884582Z ERROR codex_core::tools::router: error=apply_patch verification failed: Failed to find expected lines in /home/cinmay/Documents/xripple/hyprland/src/hypripple.c:
  if (output->app->verbose) {
    fprintf(stderr,
            "configured output %s surface %dx%d, source ring rows %d\n",
            output->name != NULL ? output->name : "(unnamed)",
            output->surface_width,
            output->surface_height,
            output->ring_row_count);
  }

Time: 2026-06-21 17:23:27
--- Next Prompt ---

