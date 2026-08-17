# Drastic DS — standalone Vulkan host

This project builds one Nintendo Switch NRO, `GBAStationNDSStub.nro`. It is
a Vulkan/NVK DraStic host launched by GBAStation. It has no internal game
browser, SMB support, USB mass-storage support, or automatic resource
extraction.

## Current test paths

The GBAStation launcher supplies the ROM path when it launches this host:

```text
GBAStationNDSStub.nro <rom-path> --return <launcher.nro>
```

`--return` defaults to `sdmc:/switch/GBAStation.nro`; pass `--exit-to-home` to
leave to Homebrew Menu instead. On normal emulator exit the host schedules the
return NRO with `envSetNextLoad`.

For direct emulator debugging, build with a fallback ROM path.  That build
uses this path only when no launcher ROM argument was supplied, so launcher
launches retain their normal behavior. Direct debug launches return to the
homebrew environment instead of attempting to reopen GBAStation:

```text
./build_local.sh --backend vulkan --debug-rom sdmc:/nds/black.nds
```

Game buttons and hotkeys are read only from the same launcher configuration
used by `nds_stub`: `sdmc:/GBAStation/config/config.cfg`. The host accepts typed
records such as `nds.handle.a=s|PAD_A` and `nds.hotkey.menu.pad=s|PAD_LT+PAD_RT`.
There are no fallback host bindings: omitted mapping keys remain unbound.

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

Install the devkitPro Switch build dependencies, and keep the switchVK SDK at
the sibling path `../switchVK/nvk-switch-26.1.4` (or set `NVK_SDK_DIR`):

```sh
pacman -S devkitA64 switch-tools libnx switch-zlib python \
          mingw-w64-ucrt-x86_64-glslang
```

Copy legal DraStic shader assets under
`third_party/drastic/assets/shaders/`, then build:

```sh
bash ./build_local.sh -j 4
```

The output is `GBAStationNDSStub.nro` in this directory.

## Remaining scope

This is a Vulkan-only target using switchVK's loaderless NVK ICD. The build
generates a small Vulkan dispatch layer, the DraStic post-processing SPIR-V,
and the main composite SPIR-V. It does not include a game browser, OpenGL
fallback, USB/SMB libraries, automatic resource extraction, or a launcher.
