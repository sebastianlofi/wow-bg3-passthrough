#!/usr/bin/env bash
# The shipped copy of fetch.sh's python: without pip, tests, idle, tk, headers, caches, and numpy's
# f2py and numpy-config scripts (their #! line names the build folder); shared objects stripped. Output: $WOWBG3_BUILD/release/python/py (build-release.py --python). An older py/ is kept
# in $WOWBG3_BUILD/old/.
set -euo pipefail
source "$(dirname "$0")/../build-env.sh"
cd "$WOWBG3_BUILD/release/python"
[ -d py ] && { mkdir -p "$WOWBG3_BUILD/old"; mv py "$WOWBG3_BUILD/old/py-$(date +%Y%m%d-%H%M%S)"; }
rsync -a --exclude='/include' --exclude='/share' --exclude='/lib/tcl9.0' --exclude='/lib/tk9.0' --exclude='/lib/libtcl*' \
  --exclude='/lib/itcl*' --exclude='/lib/thread*' --exclude='/lib/pkgconfig' --exclude='/bin/pip*' --exclude='/bin/idle*' --exclude='/bin/pydoc*' \
  --exclude='/bin/f2py' --exclude='/bin/numpy-config' \
  --exclude='/lib/python3.12/test' --exclude='/lib/python3.12/idlelib' --exclude='/lib/python3.12/tkinter' --exclude='/lib/python3.12/ensurepip' \
  --exclude='/lib/python3.12/turtledemo' --exclude='/lib/python3.12/lib2to3' --exclude='/lib/python3.12/pydoc_data' \
  --exclude='/lib/python3.12/site-packages/pip*' --exclude='_tkinter*.so' --exclude='__pycache__' python/ py/
find py -name '*.so*' -type f -exec strip --strip-unneeded {} + 2>/dev/null || true
./py/bin/python3 -c "import numpy, PIL.Image, lz4.block, lz4.frame, zstandard, json, ssl, sqlite3; print('ok', numpy.__version__)"
du -sh py
