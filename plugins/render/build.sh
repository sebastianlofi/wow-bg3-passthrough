#!/usr/bin/env bash
# Builds wowbg3_render.so, the render plugin (wowbg3_render.c with the shared capture code in
# plugins/capture/), against the pinned Steam sniper sysroot (glibc <= 2.31). Source paths in its
# debug info are written relative to this repository, bg3le's headers as bg3le/ and the sysroot's as
# sniper/ (-ffile-prefix-map).
# Output: $WOWBG3_BUILD/wowbg3-render/wowbg3_render.so (+ .sha256).
set -euo pipefail
source "$(dirname "$0")/../../release/build-env.sh"
sysroot="$WOWBG3_SNIPER/sysroot"
[[ -f "$sysroot/usr/include/vulkan/vk_layer.h" ]] || { echo "no sniper sysroot in $WOWBG3_SNIPER: run patches/bg3le/build.sh first"; exit 1; }
need_clang
out="$WOWBG3_BUILD/wowbg3-render"
mkdir -p "$out"
cd "$WOWBG3_REPO"  # the compile folder in the debug info: "." too
clang --target=x86_64-linux-gnu --sysroot="$sysroot" -std=c11 -D_GNU_SOURCE -O2 -g -fPIC -shared \
    -fvisibility=hidden -Wall -Wextra -Werror -Wl,--no-undefined -Wl,-z,defs -Wl,-soname,wowbg3_render.so \
    "-ffile-prefix-map=$WOWBG3_REPO=." "-ffile-prefix-map=$BG3LE_SRC=bg3le" "-ffile-prefix-map=$WOWBG3_SNIPER=sniper" \
    -I "$BG3LE_SRC/include" -I "$WOWBG3_REPO/plugins/render" \
    -o "$out/wowbg3_render.so.new" "$WOWBG3_REPO/plugins/render/wowbg3_render.c" -ldl -lpthread -lm
check_exports "$out/wowbg3_render.so.new"
max_glibc="$(readelf -V "$out/wowbg3_render.so.new" | grep -o 'GLIBC_2\.[0-9]*' | sort -t. -k2 -n | tail -1)"
echo "max GLIBC: ${max_glibc:-none}"
[[ -z "$max_glibc" || "${max_glibc#GLIBC_2.}" -le 31 ]]
mv "$out/wowbg3_render.so.new" "$out/wowbg3_render.so"
(cd "$out" && sha256sum wowbg3_render.so | tee wowbg3_render.so.sha256)
