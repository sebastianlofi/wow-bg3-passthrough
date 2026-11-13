#!/usr/bin/env bash
# A portable CPython for the release's tools (python-build-standalone, pinned, sha256 checked) with
# numpy, Pillow, lz4, zstandard (Tav's textures, the quest cursors, the item icons).
# Output: $WOWBG3_BUILD/release/python/python. Then pack.sh. An older copy is kept in $WOWBG3_BUILD/old/.
set -euo pipefail
source "$(dirname "$0")/../build-env.sh"
d="$WOWBG3_BUILD/release/python"; mkdir -p "$d"; cd "$d"
TAG=20261003; NAME="cpython-3.12.15+$TAG-x86_64-unknown-linux-gnu-install_only.tar.gz"
URL="https://github.com/astral-sh/python-build-standalone/releases/download/$TAG/${NAME//+/%2B}"
[ -f "$NAME" ] || curl -sSfL -o "$NAME" "$URL"
curl -sSfL "https://github.com/astral-sh/python-build-standalone/releases/download/$TAG/SHA256SUMS" -o SHA256SUMS
grep " $NAME\$" SHA256SUMS | sha256sum -c -
[ -d python ] && { mkdir -p "$WOWBG3_BUILD/old"; mv python "$WOWBG3_BUILD/old/python-$(date +%Y%m%d-%H%M%S)"; }
tar xzf "$NAME"
./python/bin/python3 -m pip install -q --no-cache-dir numpy==2.5.4 pillow==12.3.0 lz4==4.4.5 zstandard==0.25.0
./python/bin/python3 -c "import numpy, PIL, lz4, zstandard, sys; print(sys.version.split()[0], numpy.__version__, PIL.__version__)"
du -sh python
