#!/usr/bin/env bash
# C/C++ compiler and linker driver for the sniper builds of benilla (patches/benilla/build.sh) and the
# fight bridge (bridge/build.sh): the host clang with Steam Runtime 3 (sniper) as its sysroot, so every
# C object and the final link see sniper's glibc 2.31, its GCC 10 runtime (crtbegin, libgcc_s) and its
# libraries, never the host's. The Debian triple makes clang pick the sysroot's GCC install; any
# --target from the caller (the cc crate passes x86_64-unknown-linux-gnu) is dropped for it.
# sniper-c++.sh calls this with --cxx first: clang++.
sr="${BENILLA_SNIPER_SYSROOT:?set by the build script}"
cc=clang
if [[ "${1:-}" == --cxx ]]; then cc=clang++; shift; fi
args=()
skip=0
for a in "$@"; do
    if [[ $skip == 1 ]]; then skip=0; continue; fi
    case "$a" in
        --target=*) ;;
        -target) skip=1 ;;
        *) args+=("$a") ;;
    esac
done
exec "$cc" --target=x86_64-linux-gnu --sysroot="$sr" "${args[@]}"
