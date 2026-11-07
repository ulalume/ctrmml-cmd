#!/bin/bash
# Builds the main wasm module, the player module and the player's heap probe
# with Emscripten, then compares main module and player with parity.mjs.
#
#   tests/player/run.sh [parity.mjs options] [<file.mml or dir>...]
#
# BUILD_DIR (default: build-wasm in the repository) is the build directory;
# CMAKE_ARGS are added to its configuration.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
build="${BUILD_DIR:-$repo/build-wasm}"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

# shellcheck disable=SC2086
emcmake cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release -Wno-dev ${CMAKE_ARGS:-} >/dev/null
cmake --build "$build" -j"$jobs" \
	--target ctrmml-cmd-wasm ctrmml-player ctrmml-player-heap-probe >/dev/null
node --expose-gc "$repo/tests/player/parity.mjs" "$build" "$@"
