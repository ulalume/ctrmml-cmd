#!/bin/bash
# Builds player-reference natively, and the main wasm module, the player module
# and the player's heap probe with Emscripten, then compares the player with
# the reference with parity.mjs.
#
#   tests/player/run.sh [parity.mjs options] [<file.mml or dir>...]
#
# NATIVE_BUILD_DIR (default: build in the repository) and BUILD_DIR (default:
# build-wasm) are the build directories; CMAKE_ARGS are added to both
# configurations.
set -euo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
native="${NATIVE_BUILD_DIR:-$repo/build}"
build="${BUILD_DIR:-$repo/build-wasm}"
jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

# shellcheck disable=SC2086
cmake -S "$repo" -B "$native" -DCMAKE_BUILD_TYPE=Release -Wno-dev ${CMAKE_ARGS:-} >/dev/null
cmake --build "$native" -j"$jobs" --target player-reference >/dev/null
# shellcheck disable=SC2086
emcmake cmake -S "$repo" -B "$build" -DCMAKE_BUILD_TYPE=Release -Wno-dev \
	"-DCTRMML_PLAYER_REFERENCE=$native/player-reference" ${CMAKE_ARGS:-} >/dev/null
cmake --build "$build" -j"$jobs" \
	--target ctrmml-cmd-wasm ctrmml-player ctrmml-player-heap-probe >/dev/null
node --expose-gc "$repo/tests/player/parity.mjs" --reference "$native/player-reference" "$build" "$@"
