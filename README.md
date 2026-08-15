# Drastic DS — standalone OpenGL test host

This project builds one Nintendo Switch NRO, `GBAStationDrasticStub.nro`.  It has no
launcher, Vulkan renderer, switchVK SDK, SMB support, or USB mass-storage
support.  The NRO starts the OpenGL DraStic host directly.

## Current test paths

The host currently launches one fixed ROM:

```text
sdmc:/nds/black.nds
```

Place the required user-supplied files on the SD card as follows:

```text
sdmc:/GBAStation/bios/NDS/bios7.bin
sdmc:/GBAStation/bios/NDS/bios9.bin
sdmc:/GBAStation/bios/NDS/firmware.bin
sdmc:/GBAStation/cheats/usrcheat.dat

sdmc:/GBAStation/drastic/cores/libdrastic_arm64.so
sdmc:/GBAStation/drastic/system/game_database.xml
```

The core and game database use the `sdmc:/GBAStation/drastic/` resource layout.
Saves, preferences, shader files, cache, and other existing resources also use
that location.

Run from a game override rather than Album/applet mode: DraStic requires JIT
services and sufficient application memory.

## Build

Install the devkitPro Switch OpenGL dependencies:

```sh
pacman -S devkitA64 switch-tools libnx switch-mesa switch-libdrm_nouveau \
          switch-zlib python mingw-w64-ucrt-x86_64-glslang
```

Copy legal DraStic shader assets under
`third_party/drastic/assets/shaders/`, then build:

```sh
bash ./build_local.sh -j 4
```

The output is `GBAStationDrasticStub.nro` in this directory.

## Remaining scope

This is an OpenGL-only test target. It does not include a game browser,
per-game renderer selection, USB/SMB libraries, automatic resource extraction,
or a graphical launcher.
