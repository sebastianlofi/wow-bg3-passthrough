#!/usr/bin/env bash
# A portable CPython for the release's tools (python-build-standalone, pinned, sha256 checked) with
# numpy, Pillow, lz4, zstandard (Tav's textures, the quest cursors, the item icons).
# Output: $WOWBG3_BUILD/release/python/python. Then pack.sh. An older copy is kept in $WOWBG3_BUILD/old/.
set -euo pipefail
source "$(dirname "$0")/../build-env.sh"
d="$WOWBG3_BUILD/release/python"; mkdir -p "$d"; cd "$d"
TAG=20261003; NAME="cpython-3.12.15+$TAG-x86_64-unknown-linux-gnu-install_only.tar.gz"
URL="https://github.com/astral-sh/python-build-standalone/releases/download/$TAG/${NAME//+/%2B}"
# The archive's sha256, kept here: a checksum fetched from the same page as the download proves nothing about it.
SHA256=f937814031eab4698ca6d07ec606ede1825768f3f3e99af76d9db3900bee03c5
[ -f "$NAME" ] || curl -sSfL -o "$NAME" "$URL"
echo "$SHA256  $NAME" | sha256sum -c -
[ -d python ] && { mkdir -p "$WOWBG3_BUILD/old"; mv python "$WOWBG3_BUILD/old/python-$(date +%Y%m%d-%H%M%S)"; }
tar xzf "$NAME"
./python/bin/python3 -m pip install -q --no-cache-dir numpy==2.5.4 pillow==12.3.0 lz4==4.4.5 zstandard==0.25.0
./python/bin/python3 -c "import numpy, PIL, lz4, zstandard, sys; print(sys.version.split()[0], numpy.__version__, PIL.__version__)"
du -sh python
