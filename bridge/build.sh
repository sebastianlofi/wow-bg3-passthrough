#!/usr/bin/env bash
# Builds wow-bg3-link, the fight bridge, for Steam's sniper runtime (glibc 2.31) with the host clang
# as linker (release/sniper-cc.sh). Output: $WOWBG3_BUILD/link-target-sniper/release/wow-bg3-link.
# Tests (host build): cargo +1.98.1 test --locked (in this folder).
set -euo pipefail
source "$(dirname "$0")/../release/build-env.sh"
need_sniper
export CARGO_TARGET_DIR="$WOWBG3_BUILD/link-target-sniper"
export BENILLA_SNIPER_SYSROOT="$WOWBG3_SNIPER/sysroot"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="$WOWBG3_REPO/release/sniper-cc.sh"
export CC_x86_64_unknown_linux_gnu="$WOWBG3_REPO/release/sniper-cc.sh"
export CARGO_BUILD_JOBS="${CARGO_BUILD_JOBS:-2}" PATH="$CARGO_HOME/bin:$PATH"
# No build-machine paths in the binary: sources under these folders are recorded relative to them
# (panic locations name /cargo/..., /rustup/..., /wow-bg3/...). Replaces RUSTFLAGS.
remap=("--remap-path-prefix=$CARGO_HOME=/cargo" "--remap-path-prefix=$RUSTUP_HOME=/rustup" "--remap-path-prefix=$WOWBG3_REPO=/wow-bg3")
CARGO_ENCODED_RUSTFLAGS="$(IFS=$'\x1f'; echo "${remap[*]}")"
export CARGO_ENCODED_RUSTFLAGS
cd "$WOWBG3_REPO/bridge"
cargo "+$RUST_TOOLCHAIN" build --locked --release --bin wow-bg3-link
bin="$CARGO_TARGET_DIR/release/wow-bg3-link"
sha256sum "$bin"
check_glibc "$bin"
