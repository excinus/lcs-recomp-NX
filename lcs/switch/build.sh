#!/usr/bin/env bash
# Builds LCSNative.nro for the Nintendo Switch and stages the SD card layout.
#
# Inside devkitPro's container (what CI does):
#   docker run --rm -v "$PWD:/src" -w /src devkitpro/devkita64:latest lcs/switch/build.sh
# Or with a local devkitPro install (DEVKITPRO set, switch-dev and the
# switch-sdl2 / switch-ffmpeg / switch-mesa portlibs installed):
#   lcs/switch/build.sh
#
# Output: out/lcs-switch/LCSNative.nro and out/switch-sd/ (copy its contents to
# the root of the SD card).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${DEVKITPRO:?Set DEVKITPRO (devkitPro install, e.g. /opt/devkitpro)}"
BUILD_DIR="${BUILD_DIR:-$ROOT/out/lcs-switch}"
STAGE_DIR="${STAGE_DIR:-$ROOT/out/switch-sd}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
VERSION="${LCS_SWITCH_VERSION:-}"

CMAKE="${CMAKE:-cmake}"
GENERATOR=()
if command -v ninja >/dev/null 2>&1; then GENERATOR=(-G Ninja); fi

EXTRA=()
if [[ -n "$VERSION" ]]; then EXTRA+=("-DLCS_SWITCH_VERSION=$VERSION"); fi
if command -v ccache >/dev/null 2>&1; then EXTRA+=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache); fi

"$CMAKE" -S "$ROOT" -B "$BUILD_DIR" "${GENERATOR[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    "${EXTRA[@]}"
"$CMAKE" --build "$BUILD_DIR" --target LCSNative_nro -j "$JOBS"

NRO="$BUILD_DIR/lcs/LCSNative.nro"
APP_DIR="$STAGE_DIR/switch/LCSNative"
rm -rf "$STAGE_DIR"
mkdir -p "$APP_DIR/game"
cp "$NRO" "$APP_DIR/LCSNative.nro"
# The game makes LCSNative.ini from this on first start, so an update never
# overwrites the player's settings.
cp "$ROOT/lcs/switch/LCSNative.ini" "$APP_DIR/LCSNative.default.ini"
cp "$ROOT/lcs/switch/lcs_env.txt" "$APP_DIR/lcs_env.txt"
cp "$ROOT/lcs/switch/game_README.txt" "$APP_DIR/game/README.txt"
cp "$ROOT/lcs/switch/README_SD.txt" "$APP_DIR/README.txt"
echo "Built $NRO"
echo "SD card layout in $STAGE_DIR"
