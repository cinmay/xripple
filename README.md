# Xripple

This is the Xripple program originally written by Carsten Haitzler The Rasterman.
It draws ripples on the screen using X11 and mirrors menu's and windows in the ripples.

## Prerequisites

It draws on the root window, so it only works with window managers that use the root window for the desktop.
It's been tested with Fluxbox and AwesomeVM.

## Basic compilation

If you just want to compile the program, you can use gcc or zig etc.
Just run the following command:

```bash
gcc -O3 -o xripple xrp.c -lX11 -lm
```

## Building with CMake

```bash
# Create a build directory
mkdir build
cd build

# Configure and build
cmake ..
make

# Run the program
./xripple

```

## Original README

Well there's thers' the usual disclaimer on this code.. if itt stuffs anything
of yours up, like files, networks, hardware etc.... don't blame me. I take no
responsability for it, so use it at your own risk.

Okay.. it's simple to compile just type:

xmkmf
make

and it's compiled.. you'll have an xriipple executable there ready to run.
to run it, just type:

xripple

there aren't any command-line options.. it's simple.. only 133 lines of code.

Well enjoy....

```
	Rasterman.
              ___  ___  _________ ___  ___  __    __  ___   __   __
             /   \/   \/    \    \   \/   \/  \  /  \/   \ /  \ /  \
    __ _____/  o  \ o  \  __/  __/___/  o  \   \/   |  o  \    \   |______ __
   / // ___/  _  _/     \   \  |  ___) _  _/  _  _  |      \    \  |___  // //
  /_//____/    \ \  /\   \_  \ |     \  \ \|  |\/|  |  /\   \      |____//_//
  `-'`---/     /  \ \/    \  / |      \ /  \  |  |  |  \/    \\    |----``-'
         \____/\__/ /\____/_/  |______//\__/  |  |  |  /\____/|\   |
               `---'        `--'           `--'  `--'-'    `--' `--'
The Rasterman ........ http://www.rasterman.com/
================================================================================

```
