# Repository Guidelines

## Project Structure & Module Organization

This repository contains a small C/X11 program. The main implementation lives in `xrp.c`, which builds the `xripple` executable. Build configuration is in `CMakeLists.txt`. User-facing project notes and historical instructions are in `README.md`; licensing is in `LICENSE`. There is currently no separate `src/`, `tests/`, or `assets/` directory, so keep new files at the root only when they apply to the whole project.

## Build, Test, and Development Commands

- `gcc -O3 -o xripple xrp.c -lX11 -lm`: quick direct build using GCC.
- `mkdir -p build && cd build && cmake .. && make`: configure and build with CMake.
- `./build/xripple` or `./xripple`: run the program after building, depending on where the executable was produced.

The program depends on X11 headers/libraries and a window manager that exposes the root window as the desktop.

## Coding Style & Naming Conventions

The project is C99, as configured in `CMakeLists.txt`. Follow the existing compact C style: two-space indentation inside blocks, braces on the function/control line, and simple lowercase function names such as `doit`. Existing globals use short lowercase names (`disp`, `waterh`, `depth`); prefer clearer names for new code when practical, but avoid broad rewrites unrelated to the change. Keep comments short and useful, especially around X11 resource handling.

## Testing Guidelines

No automated test framework is currently present. For changes, verify at minimum that both build paths still compile:

- direct GCC command from the README
- CMake configure and build from a clean `build/` directory

For behavioral changes, run `xripple` in a compatible X11 session and confirm it draws the ripple effect without startup errors or obvious display corruption.

## Commit & Pull Request Guidelines

Recent history uses short, descriptive commit subjects such as `Added cmake build instructions` and `Updated build system to use CMake`. Match that style with a concise subject that names the visible change. Pull requests should include a brief summary, the build commands run, any runtime environment used for manual testing, and screenshots or screen recordings when display behavior changes.

## Security & Configuration Tips

Avoid committing generated binaries, local build directories, or machine-specific editor files. Keep X11 display assumptions documented when changing startup, rendering, or dependency behavior.
