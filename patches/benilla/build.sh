#!/usr/bin/env bash
# Builds WOW-BG3's benilla: the pinned upstream benilla plus the patches in this folder, applied in
# name order to $BENILLA_SRC (cloned at the pinned commit when missing; it must be at that commit
# and otherwise clean). Patches stack, so only the newest applied patch reverse-applies cleanly: the
# script finds it from the end and applies the ones after it.
#   WOW_BG3_SNIPER=1 (default): links against Steam Runtime 3 "sniper" (glibc 2.31, as bg3le's release
#     build) with the host clang through release/sniper-cc.sh, so it runs inside Steam's runtime
#     container as BG3 does. 0: a plain host build.
#   WOW_BG3_PLAYER=1 (default): the player build (--no-default-features: no dev instruments, no debug
#     panel, no build-machine source paths in the binary). 0: the developer build.
# Output: $WOWBG3_BUILD/benilla-target-sniper-player/release/benilla (folder name follows the two
# settings). A previous binary is kept in $WOWBG3_BUILD/old/ first. CARGO_BUILD_JOBS defaults to 2
# (one rustc can take 7 GB).
set -euo pipefail
source "$(dirname "$0")/../../release/build-env.sh"
export CARGO_TARGET_DIR="$WOWBG3_BUILD/benilla-target"
sniper="${WOW_BG3_SNIPER:-1}"
if [[ "$sniper" == 1 ]]; then
    need_sniper
    export BENILLA_SNIPER_SYSROOT="$WOWBG3_SNIPER/sysroot"
    export CARGO_TARGET_DIR="$WOWBG3_BUILD/benilla-target-sniper"
    export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="$WOWBG3_REPO/release/sniper-cc.sh"
    export CC_x86_64_unknown_linux_gnu="$WOWBG3_REPO/release/sniper-cc.sh"
    export CXX_x86_64_unknown_linux_gnu="$WOWBG3_REPO/release/sniper-c++.sh"
    export PKG_CONFIG_SYSROOT_DIR="$BENILLA_SNIPER_SYSROOT"
    export PKG_CONFIG_LIBDIR="$BENILLA_SNIPER_SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig:$BENILLA_SNIPER_SYSROOT/usr/share/pkgconfig"
    # pkg-config variables (x11-dl bakes X11's libdir into the binary) name the runtime's folders,
    # not the sysroot's; -I and -L still point into the sysroot.
    export PKG_CONFIG_FDO_SYSROOT_RULES=1
fi
[[ "${WOW_BG3_PLAYER:-1}" == 1 ]] && export CARGO_TARGET_DIR="$CARGO_TARGET_DIR-player"
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-2}"
export PATH="$CARGO_HOME/bin:$PATH"
# No build-machine paths in the binary: sources under these folders are recorded relative to them
# (panic and log locations name /cargo/..., /rustup/..., /benilla/...). Replaces RUSTFLAGS.
remap=("--remap-path-prefix=$CARGO_HOME=/cargo" "--remap-path-prefix=$RUSTUP_HOME=/rustup" "--remap-path-prefix=$BENILLA_SRC=/benilla")
CARGO_ENCODED_RUSTFLAGS="$(IFS=$'\x1f'; echo "${remap[*]}")"
export CARGO_ENCODED_RUSTFLAGS
clone_pinned "$BENILLA_URL" "$BENILLA_COMMIT" "$BENILLA_SRC"
cd "$BENILLA_SRC"
patches=("$WOWBG3_REPO"/patches/benilla/[0-9][0-9][0-9][0-9]-*.patch)
first=0
for ((i=${#patches[@]}-1; i>=0; i--)); do
    if git apply -R --check "${patches[i]}" 2>/dev/null; then first=$((i+1)); break; fi
done
for ((i=0; i<first; i++)); do echo "applied already: $(basename "${patches[i]}")"; done
for ((i=first; i<${#patches[@]}; i++)); do
    git apply --check "${patches[i]}"
    git apply "${patches[i]}"
    echo "applied: $(basename "${patches[i]}")"
done
bin="$CARGO_TARGET_DIR/release/benilla"
if [[ -f "$bin" ]]; then
    keep="$WOWBG3_BUILD/old/benilla-bin-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$keep"
    cp -p "$bin" "$keep/benilla"
fi
features=()
[[ "${WOW_BG3_PLAYER:-1}" == 1 ]] && features=(--no-default-features)
cargo "+$RUST_TOOLCHAIN" build --locked --release -p benilla "${features[@]}"
sha256sum "$bin"
if [[ "$sniper" == 1 ]]; then check_glibc "$bin"; fi
