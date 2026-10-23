#!/usr/bin/env bash
# build-all.sh VERSION: every part of the release from source, then the zip and its sources archive
# (docs/build.md). Steps already done are skipped where the part exists; set REBUILD=1 to redo them.
# Output: $WOWBG3_BUILD/dist/WOW-BG3-VERSION.zip and WOW-BG3-VERSION-sources.tar.gz.
set -euo pipefail
source "$(dirname "$0")/build-env.sh"
v="${1:?VERSION, e.g. 0.1.0}"
B="$WOWBG3_BUILD"
R="$WOWBG3_REPO"
step() { echo; echo "== $*"; }
have() { [[ "${REBUILD:-0}" != 1 && -e "$1" ]]; }

step "bg3le (sysroot, libc++, libbg3le.so)"
have "$WOWBG3_SNIPER/bg3le/libbg3le.so" || bash "$R/patches/bg3le/build.sh"
step "plugins"
bash "$R/plugins/launcher/build.sh"
bash "$R/plugins/build.sh"
bash "$R/plugins/render/build.sh"
bash "$R/plugins/early/build.sh"
step "fight bridge"
bash "$R/bridge/build.sh"
step "benilla (player build for sniper)"
have "$B/benilla-target-sniper-player/release/benilla" || bash "$R/patches/benilla/build.sh"
step "portable server"
source "$R/server/pins.sh"
if ! have "$WOWBG3_SERVER_BUILD/wowbg3-server"; then
    have "$WOWBG3_SERVER_BUILD/out/.complete" || bash "$R/server/build-portable.sh"
    bash "$R/server/pack-portable.sh"
fi
step "python and FastLZ"
have "$B/release/python/py/bin/python3" || { bash "$R/release/python/fetch.sh"; bash "$R/release/python/pack.sh"; }
bash "$R/tools/fastlz/build.sh"
step "licence texts and upstream sources"
python3 "$R/release/licences/collect.py" fetch texts sources
step "release $v"
python3 "$R/release/build-release.py" --version "$v" --out "$B/dist" \
    --launcher "$B/wowbg3-launcher/wowbg3-launcher.so" --plugin "$B/wowbg3-plugin/wowbg3.so" \
    --server "$WOWBG3_SERVER_BUILD/wowbg3-server" --benilla "$B/benilla-target-sniper-player/release/benilla" \
    --fight-bridge "$B/link-target-sniper/release/wow-bg3-link" \
    --python "$B/release/python/py" --fastlz "$B/release/libfastlz.so" --sources "$B/licences/sources"
