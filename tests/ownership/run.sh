#!/bin/sh
# Builds the ownership harness against a plugin source tree (default: this
# checkout) with ASan/UBSan and runs it. macOS only; uses synthetic stubs for
# 4D and the smart-card boundary, never real readers or cards.
#   tests/ownership/run.sh [SOURCE_ROOT]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "${1:-$HERE/../..}" && pwd)
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

FLAGS="-g -O0 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-objc-arc -Wno-everything"

clang++ -x objective-c++ -std=c++17 $FLAGS -I"$SRC" -I"$SRC/4D Plugin API" -I"$SRC/support" \
  -I"$SRC/support/libpafe" -I"$SRC/include" -c "$HERE/harness.mm" -o "$OUT/harness.o"
clang++ -x objective-c++ -std=c++17 $FLAGS -I"$SRC" -I"$SRC/4D Plugin API" -I"$SRC/support" \
  -I"$SRC/include" -c "$SRC/support/4DPlugin-JSON.cpp" -o "$OUT/json.o"
clang++ -x objective-c++ $FLAGS -I"$SRC/4D Plugin API" \
  -c "$SRC/4D Plugin API/4DPluginAPI.c" -o "$OUT/api.o"
clang++ $FLAGS "$OUT/harness.o" "$OUT/json.o" "$OUT/api.o" "$SRC/a/libjsoncpp.a" \
  -framework Foundation -framework CoreGraphics -framework Security -o "$OUT/harness"

ASAN_OPTIONS=abort_on_error=0:halt_on_error=1 "$OUT/harness"
