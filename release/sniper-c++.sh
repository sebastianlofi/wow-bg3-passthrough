#!/usr/bin/env bash
# The C++ driver of the sniper builds: sniper-cc.sh as clang++.
exec "$(dirname "$0")/sniper-cc.sh" --cxx "$@"
