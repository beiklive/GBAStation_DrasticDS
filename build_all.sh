#!/usr/bin/env bash
set -euo pipefail

export PATH="/usr/bin:/bin:/mingw64/bin:/ucrt64/bin:${PATH:-}"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITARM=$DEVKITPRO/devkitARM
export DEVKITA64=$DEVKITPRO/devkitA64
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}

APP="$(cd "$(dirname "$0")" && pwd)"
RUNTIME_DIR=${DRASTIC_RUNTIME_DIR:-"$APP/third_party/drastic"}
APK_DIR=${DRASTIC_APK_DIR:-"$(dirname "$APP")/com.dsemu.drastic_r2.6.0.4a-109_minAPI14(arm64-v8a)(nodpi)_drasticds.com"}
ASSETS=${DRASTIC_ASSETS_DIR:-"$RUNTIME_DIR/assets"}
if [[ ! -d "$ASSETS" && -d "$APK_DIR/assets" ]]; then
  ASSETS="$APK_DIR/assets"
fi
DFX_SOURCE="$ASSETS/shaders"

required=(
  "$DFX_SOURCE/None.dfx"
  "$DFX_SOURCE/Linear.dfx"
  "$DFX_SOURCE/Quilez.dfx"
  "$DFX_SOURCE/Scanline.dfx"
  "$DFX_SOURCE/Scale2X.dfx"
  "$DFX_SOURCE/HQ2X.dfx"
  "$DFX_SOURCE/FXAA.dfx"
  "$DFX_SOURCE/FXAA HQ.dfx"
  "$DFX_SOURCE/SMAA.dfx"
  "$DFX_SOURCE/linear.dsd"
  "$DFX_SOURCE/quilez.dsd"
  "$DFX_SOURCE/scanline.dsd"
  "$DFX_SOURCE/scale2x.dsd"
  "$DFX_SOURCE/hq2x.dsd"
  "$DFX_SOURCE/fxaa.dsd"
  "$DFX_SOURCE/fxaa/fxaa_luma.dsd"
  "$DFX_SOURCE/fxaa/fxaa.dsd"
  "$DFX_SOURCE/smaa/smaa_edge.dsd"
  "$DFX_SOURCE/smaa/smaa_weight.dsd"
  "$DFX_SOURCE/smaa/smaa_blend.dsd"
  "$DFX_SOURCE/smaa/SMAA.hlsl"
  "$DFX_SOURCE/smaa/AreaTexRGB.raw"
  "$DFX_SOURCE/smaa/SearchTexRGB.raw"
)
for file in "${required[@]}"; do
  [[ -f "$file" ]] || { echo "Missing OpenGL shader input: $file" >&2; exit 1; }
done

PYTHON3=${PYTHON3:-$(command -v python3 || true)}
[[ -n "$PYTHON3" && -x "$PYTHON3" ]] || { echo "python3 is required." >&2; exit 1; }
GLSLANG=${GLSLANG_VALIDATOR:-$(command -v glslangValidator || true)}
if [[ -z "$GLSLANG" && -x /ucrt64/bin/glslangValidator.exe ]]; then
  GLSLANG=/ucrt64/bin/glslangValidator.exe
fi
if command -v cygpath >/dev/null 2>&1 && [[ "$GLSLANG" =~ ^[A-Za-z]: ]]; then
  GLSLANG=$(cygpath -u "$GLSLANG")
fi
if [[ "$GLSLANG" != *.exe && -f "$GLSLANG.exe" ]]; then
  GLSLANG="$GLSLANG.exe"
fi
[[ -n "$GLSLANG" && -x "$GLSLANG" ]] || {
  echo "glslangValidator is required to validate OpenGL shaders." >&2
  exit 1
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/drasticds-gl.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
DFX_STAGE="$WORK/dfx"

echo "==== clean previous output ===="
make -C "$APP" clean >/dev/null

echo "==== Drastic OpenGL post-FX programs ===="
"$PYTHON3" "$APP/tools/build_dfx.py" \
  --source "$DFX_SOURCE" --output "$DFX_STAGE" --glslang "$GLSLANG"

echo "==== standalone Drastic OpenGL host ===="
make -C "$APP" -j"$JOBS" DFX_GENERATED="$DFX_STAGE"

echo
echo "Done. Copy this NRO to the SD card:"
ls -la "$APP/GBAStationDrasticStub.nro"
echo "Runtime ROM: sdmc:/nds/black.nds"
echo "Runtime BIOS: sdmc:/GBAStation/bios/NDS/{bios7.bin,bios9.bin,firmware.bin}"
echo "Runtime cheat database: sdmc:/GBAStation/cheats/usrcheat.dat"
