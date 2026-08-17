#!/usr/bin/env bash
set -euo pipefail

export PATH="/usr/bin:/bin:/mingw64/bin:/ucrt64/bin:${PATH:-}"
SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
JOBS=${JOBS:-}
BACKEND=${BACKEND:-all}
DEBUG_ROM_PATH=${DEBUG_ROM_PATH:-}
TMP_ROOT=${DRASTIC_TMPDIR:-"$SCRIPT_DIR/.tmp"}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -j|--jobs) JOBS=${2:?missing job count}; shift 2 ;;
    -b|--backend) BACKEND=${2:?missing backend}; shift 2 ;;
    --debug-rom) DEBUG_ROM_PATH=${2:?missing ROM path}; shift 2 ;;
    -h|--help) echo "Usage: ./build_local.sh [-j JOBS] [--backend all|vulkan|opengl] [--debug-rom sdmc:/path/game.nds]"; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
done
if [[ -n "$JOBS" && ! "$JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo "Invalid job count: $JOBS" >&2
  exit 2
fi
case "$BACKEND" in all|vulkan|opengl) ;; *) echo "Invalid backend: $BACKEND" >&2; exit 2 ;; esac

export JOBS=${JOBS:-$(nproc)}
export BACKEND
export DEBUG_ROM_PATH
mkdir -p "$TMP_ROOT"
export TMPDIR="$TMP_ROOT"
export TMP="$TMP_ROOT"
export TEMP="$TMP_ROOT"
exec bash "$SCRIPT_DIR/build_all.sh"
