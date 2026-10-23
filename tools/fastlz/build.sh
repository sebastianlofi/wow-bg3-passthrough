#!/usr/bin/env bash
# Builds libfastlz.so (FastLZ, MIT; bg3tex.py reads BG3's FastLZ-packed textures with it) for the
# sniper runtime. Output: $WOWBG3_BUILD/release/libfastlz.so.
set -euo pipefail
source "$(dirname "$0")/../../release/build-env.sh"
need_sniper
mkdir -p "$WOWBG3_BUILD/release"
cd "$WOWBG3_REPO/tools/fastlz"
clang --target=x86_64-linux-gnu "--sysroot=$WOWBG3_SNIPER/sysroot" -O2 -shared -fPIC fastlz.c -o "$WOWBG3_BUILD/release/libfastlz.so"
sha256sum "$WOWBG3_BUILD/release/libfastlz.so"
