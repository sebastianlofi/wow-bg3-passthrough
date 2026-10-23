#!/usr/bin/env bash
# Builds wowbg3.so, the main bg3le plugin (wowbg3_plugin.cpp), against the same sniper sysroot,
# libc++ and bg3se headers as the pinned bg3le build (patches/bg3le/build.sh must have run once).
# Output: $WOWBG3_BUILD/wowbg3-plugin/wowbg3.so (+ .sha256).
set -euo pipefail
source "$(dirname "$0")/../release/build-env.sh"
sniper="$WOWBG3_SNIPER"
src="$BG3LE_SRC"
out="$WOWBG3_BUILD/wowbg3-plugin"
need_sniper
[[ -f "$sniper/libcxx/lib/libc++.a" ]] || { echo "no libc++ in $sniper: run patches/bg3le/build.sh first"; exit 1; }
[[ "$(git -C "$src" rev-parse HEAD)" == "$BG3LE_COMMIT" ]]
need_clang
mkdir -p "$out"
flags=(--target=x86_64-linux-gnu "--sysroot=$sniper/sysroot"
  -DBG3LE_NOESIS_FORWARD -DBG3LE_NO_DX11 -DGLM_FORCE_INTRINSICS -DGOOGLE_PROTOBUF_NO_RTTI -DNS_STATIC_LIBRARY
  -DOSI_EOCAPP -DOSI_EXTENSION_BUILD -D_ITERATOR_DEBUG_LEVEL=0
  "-I$src/vendor/compat" "-I$src/vendor/bg3se" "-I$src/vendor/bg3se/BG3Extender"
  "-I$src/external/third_party/Noesis/NoesisGUI-NativeSDK-win-3.1.7-Indie/Include"
  "-I$src/external/third_party/glm" "-I$src/external/third_party/imgui" "-I$src/external/third_party/rapidjson/include"
  "-I$src/external/third_party/optick/src" "-I$src/external/third_party/tinycrypt/lib/include"
  "-I$sniper/sysroot/usr/include/SDL2" "-I$src/src/vendor" "-I$src/external/third_party/protobuf/src"
  "-I$src/external/third_party/protobuf/third_party/utf8_range" "-I$sniper/abseil/include" "-I$src/external/lua"
  "-I$src/external/third_party/Vulkan/include" "-I$src/include"
  -nostdinc++ -isystem "$sniper/libcxx/include/c++/v1" -O2 -DNDEBUG -std=gnu++23 -fPIC -stdlib=libc++
  -D_LIBCPP_ABI_VARIANT_INDEX_TYPE_OPTIMIZATION -msse4.2 -mpopcnt -fdeclspec -fms-extensions
  -fdelayed-template-parsing -Wno-delayed-template-parsing-in-cxx20 -include "$src/vendor/compat/msvc_compat.h"
  -fvisibility=hidden -fvisibility-inlines-hidden -w)
clang++ "${flags[@]}" -c "$WOWBG3_REPO/plugins/wowbg3_plugin.cpp" -o "$out/wowbg3_plugin.o"
# Everything the plugin uses is in it (libc++ static, hidden) or in the sniper glibc;
# bg3le's own functions are found with dlsym at init, so -z defs must pass.
clang++ --target=x86_64-linux-gnu "--sysroot=$sniper/sysroot" -shared -fPIC -o "$out/wowbg3.so.new" \
  "$out/wowbg3_plugin.o" -nostdlib++ "$sniper/libcxx/lib/libc++.a" "$sniper/libcxx/lib/libc++abi.a" \
  -static-libgcc -ldl -lpthread -lm -Wl,-z,defs -Wl,--exclude-libs,ALL -Wl,-soname,wowbg3.so
check_exports "$out/wowbg3.so.new"
check_glibc "$out/wowbg3.so.new"
mv "$out/wowbg3.so.new" "$out/wowbg3.so"
sha256sum "$out/wowbg3.so" | tee "$out/wowbg3.so.sha256"
