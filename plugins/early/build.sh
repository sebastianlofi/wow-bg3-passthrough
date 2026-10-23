#!/usr/bin/env bash
# Builds wowbg3-early.so (wowbg3_early.c, cursor.c: WoW's quest cursors in BG3) against the sniper
# sysroot bg3le uses (patches/bg3le/build.sh must have run once).
# Output: $WOWBG3_BUILD/wowbg3-early/wowbg3-early.so (+ .sha256).
set -euo pipefail
source "$(dirname "$0")/../../release/build-env.sh"
sniper="$WOWBG3_SNIPER"; out="$WOWBG3_BUILD/wowbg3-early"
need_sniper
mkdir -p "$out"
for f in wowbg3_early cursor; do
  clang --target=x86_64-linux-gnu "--sysroot=$sniper/sysroot" -I"$BG3LE_SRC/include" \
    -I"$sniper/sysroot/usr/include/SDL2" -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Werror \
    -c "$WOWBG3_REPO/plugins/early/$f.c" -o "$out/$f.o"
done
clang --target=x86_64-linux-gnu "--sysroot=$sniper/sysroot" -shared -fPIC -o "$out/wowbg3-early.so.new" \
  "$out/wowbg3_early.o" "$out/cursor.o" -ldl -Wl,-z,defs -Wl,-soname,wowbg3-early.so
check_exports "$out/wowbg3-early.so.new"
check_glibc "$out/wowbg3-early.so.new"
mv "$out/wowbg3-early.so.new" "$out/wowbg3-early.so"
sha256sum "$out/wowbg3-early.so" | tee "$out/wowbg3-early.so.sha256"
