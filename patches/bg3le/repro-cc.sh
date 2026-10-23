#!/usr/bin/env bash
# Compiler launcher for the bg3le sniper build (build.sh sets it as CMAKE_C_COMPILER_LAUNCHER and
# CMAKE_CXX_COMPILER_LAUNCHER for libc++, abseil, protobuf and bg3le), so the library does not depend
# on where or when it is built:
# - two -ffile-prefix-map options: the __FILE__ strings those libraries compile in (assertions, log
#   lines) name /bg3le/... and /bg3le-sniper/... instead of the folders of the building machine;
#   BG3LE_SRC is bg3le's source (with external/third_party: abseil, protobuf, ...), BG3LE_SNIPER_BUILD
#   the sniper build folder (sysroot, llvm-project, libc++, abseil);
# - SOURCE_DATE_EPOCH (default: the bg3le commit's time): __DATE__ and __TIME__ in bg3le's "built on"
#   line are that time, not the time of the build.
# Nothing else changes: same compiler, same flags, same code.
export SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "${BG3LE_SRC:?}" log -1 --format=%ct)}"
exec "$@" "-ffile-prefix-map=${BG3LE_SNIPER_BUILD:?}=/bg3le-sniper" "-ffile-prefix-map=${BG3LE_SRC:?}=/bg3le"
