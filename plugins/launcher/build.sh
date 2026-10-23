#!/usr/bin/env bash
# Builds wowbg3-launcher.so (wowbg3_launcher.c) against the Steam runtime's sniper sysroot
# (glibc 2.31). C only, no libc++. Output: $WOWBG3_BUILD/wowbg3-launcher/wowbg3-launcher.so
# (+ .sha256), or WOWBG3_LAUNCHER_OUT. Tests without BG3: test.sh (see its header).
set -euo pipefail
source "$(dirname "$0")/../../release/build-env.sh"
sniper="$WOWBG3_SNIPER"
out="${WOWBG3_LAUNCHER_OUT:-$WOWBG3_BUILD/wowbg3-launcher}"
need_sniper
need_clang
mkdir -p "$out"
clang --target=x86_64-linux-gnu "--sysroot=$sniper/sysroot" -std=gnu11 -O2 -g0 -fPIC -shared \
  -fvisibility=hidden -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unknown-warning-option -Wno-format-truncation \
  -I"$BG3LE_SRC/include" "$WOWBG3_REPO/plugins/launcher/wowbg3_launcher.c" \
  -o "$out/wowbg3-launcher.so.new" -ldl -lpthread -Wl,-z,defs -Wl,-soname,wowbg3-launcher.so
check_exports "$out/wowbg3-launcher.so.new"
check_glibc "$out/wowbg3-launcher.so.new"
mv "$out/wowbg3-launcher.so.new" "$out/wowbg3-launcher.so"
sha256sum "$out/wowbg3-launcher.so" | tee "$out/wowbg3-launcher.so.sha256"
