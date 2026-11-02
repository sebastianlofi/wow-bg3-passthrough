# Shared settings of the WOW-BG3 build scripts (sourced, never run). Set any of these in the
# environment first to change it; docs/build.md explains them.
#
#   WOWBG3_BUILD   where builds, downloads and toolchains go (default: build/ in this repository)
#   WOWBG3_SNIPER  bg3le's Steam Runtime 3 (sniper) build: sysroot, libc++, abseil, libbg3le.so
#   BG3LE_SRC      bg3le's source at the pinned commit, with patches/bg3le applied
#   BENILLA_SRC    benilla's source at the pinned commit, with patches/benilla applied
#   CARGO_HOME, RUSTUP_HOME   the Rust toolchain used for benilla and the fight bridge
#   MARIADB_TAR    the pinned MariaDB bintar (server/pins.sh)
#   WOWBG3_VERSION the release version build-all.sh and build-release.py default to (0.1.0)
WOWBG3_REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
: "${WOWBG3_BUILD:=$WOWBG3_REPO/build}"
: "${WOWBG3_SNIPER:=$WOWBG3_BUILD/bg3le-sniper}"
: "${BG3LE_SRC:=$WOWBG3_BUILD/src/bg3le}"
: "${BENILLA_SRC:=$WOWBG3_BUILD/src/benilla}"
: "${CARGO_HOME:=$WOWBG3_BUILD/cargo-home}"
: "${RUSTUP_HOME:=$WOWBG3_BUILD/rustup-home}"
: "${MARIADB_TAR:=$WOWBG3_BUILD/dl/mariadb-11.4.8-linux-systemd-x86_64.tar.gz}"
: "${WOWBG3_VERSION:=0.1.0}"
export WOWBG3_REPO WOWBG3_BUILD WOWBG3_SNIPER BG3LE_SRC BENILLA_SRC CARGO_HOME RUSTUP_HOME MARIADB_TAR WOWBG3_VERSION

# Pinned upstream sources and toolchains.
BG3LE_URL=https://github.com/lenonk/bg3le.git
BG3LE_COMMIT=5c729b72296defd4a09556ee723b12b76a0d445d
BENILLA_URL=https://github.com/samwhosung/benilla.git
BENILLA_COMMIT=d3ac9f75bf946bf9896a01deceb1b6dd539ace64
VMANGOS_URL=https://github.com/vmangos/core.git
VMANGOS_COMMIT=53d24d5c748bef5e0cde7b1155bcb3d979af614c
RUST_TOOLCHAIN=1.98.1
CLANG_VERSION=22.1.8
SNIPER_SDK="$(cat "$WOWBG3_REPO/patches/bg3le/sdk-image.lock")"

# clone_pinned URL COMMIT DIR: a checkout of COMMIT in DIR (made when DIR does not exist yet).
clone_pinned() {
    local url="$1" commit="$2" dir="$3"
    if [[ ! -d "$dir/.git" && ! -f "$dir/.git" ]]; then
        mkdir -p "$dir"
        git -C "$dir" init -q
        git -C "$dir" remote add origin "$url"
        git -C "$dir" fetch -q --depth 1 origin "$commit"
        git -C "$dir" checkout -q --detach FETCH_HEAD
    fi
    [[ "$(git -C "$dir" rev-parse HEAD)" == "$commit" ]] || { echo "$dir is not at $commit" >&2; return 1; }
}

need_clang() {
    [[ "$(clang --version)" == *"clang version $CLANG_VERSION"* ]] || { echo "needs clang $CLANG_VERSION" >&2; return 1; }
}

need_sniper() {
    [[ -f "$WOWBG3_SNIPER/sysroot/.image" ]] || { echo "no sniper sysroot in $WOWBG3_SNIPER: run patches/bg3le/build.sh first" >&2; return 1; }
}

# check_glibc FILE: the file asks for nothing newer than glibc 2.31 (Steam Runtime 3, sniper).
check_glibc() {
    local newest
    newest="$(objdump -T "$1" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
    echo "newest glibc symbol: ${newest:-none}"
    [[ -z "$newest" || "$(printf '%s\n2.31\n' "$newest" | sort -V | tail -1)" == 2.31 ]] || { echo "needs glibc $newest > 2.31" >&2; return 1; }
}

# check_exports FILE: a bg3le plugin exports bg3le_plugin_init and nothing else.
check_exports() {
    local exported
    exported="$(nm -D --defined-only "$1" | awk '{print $3}' | grep -v '^_init$\|^_fini$' || true)"
    [[ "$exported" == bg3le_plugin_init ]] || { echo "unexpected exports: $exported" >&2; return 1; }
}
