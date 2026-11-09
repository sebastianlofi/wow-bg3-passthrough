#!/usr/bin/env bash
# Builds bg3le (the Linux Script Extender) with WOW-BG3's patches for Steam Runtime 3 (sniper):
# clones the pinned bg3le into $BG3LE_SRC when missing, fetches its locked third-party sources
# (fetch-sources.py, externals.lock.json), applies the patches of this folder in build.lock.json's
# order (apply-patches.py), runs bg3le's own source checks and its sniper build (Docker, the SDK image
# in sdk-image.lock). Output: $WOWBG3_SNIPER/bg3le/libbg3le.so; its sha256 should be build.lock.json's
# library_sha256. Every object is compiled through repro-cc.sh, so no folder of the building machine
# ends up in the library or in the libc++ the plugins link. Needs: clang 23.1.5, bsdtar, lua, Docker.
set -euo pipefail
source "$(dirname "$0")/../../release/build-env.sh"
export CMAKE_BUILD_PARALLEL_LEVEL="${CMAKE_BUILD_PARALLEL_LEVEL:-2}"
export BG3LE_SNIPER_BUILD="$WOWBG3_SNIPER"
export BG3LE_SNIPER_IMAGE="$SNIPER_SDK"
[[ "$BG3LE_SNIPER_IMAGE" == registry.gitlab.steamos.cloud/steamrt/sniper/sdk@sha256:* ]]
here="$WOWBG3_REPO/patches/bg3le"
export CMAKE_C_COMPILER_LAUNCHER="$here/repro-cc.sh" CMAKE_CXX_COMPILER_LAUNCHER="$here/repro-cc.sh"
clone_pinned "$BG3LE_URL" "$BG3LE_COMMIT" "$BG3LE_SRC"
python3 "$here/fetch-sources.py"
python3 "$here/apply-patches.py"
cd "$BG3LE_SRC"
python3 tools/check-vendor-patches.py
bash tools/check-prelude.sh
python3 "$here/check-view-contract.py"
llvm_source="$BG3LE_SNIPER_BUILD/llvm-project"
if [[ ! -d "$llvm_source/.git" ]]; then
    git clone --depth 1 --branch llvmorg-23.1.5 --filter=blob:none --sparse https://github.com/llvm/llvm-project.git "$llvm_source"
fi
[[ "$(git -C "$llvm_source" rev-parse HEAD)" == 392dfff97d29b59d77e5cac6d4b6ef2585fe7117 ]]
[[ "$(git -C "$llvm_source" describe --tags)" == llvmorg-23.1.5 ]]
need_clang
if [[ -f "$BG3LE_SNIPER_BUILD/sysroot/.image" ]]; then
    [[ "$(cat "$BG3LE_SNIPER_BUILD/sysroot/.image")" == "$(docker image inspect --format '{{.Id}}' "$BG3LE_SNIPER_IMAGE")" ]]
fi
bash tools/build-sniper.sh
# A libc++, abseil or protobuf left from a build without repro-cc.sh would bring its folders back.
for f in bg3le/libbg3le.so libcxx/lib/libc++.a libcxx/lib/libc++abi.a; do
    if grep -a -q -F -e "$WOWBG3_BUILD" -e "$WOWBG3_REPO" -e "$BG3LE_SRC" -e "$BG3LE_SNIPER_BUILD" "$BG3LE_SNIPER_BUILD/$f"; then
        echo "$f names a build folder: move libcxx, abseil, protobuf-build and bg3le out of $BG3LE_SNIPER_BUILD, build again" >&2
        exit 1
    fi
done
sha256sum "$BG3LE_SNIPER_BUILD/bg3le/libbg3le.so"
