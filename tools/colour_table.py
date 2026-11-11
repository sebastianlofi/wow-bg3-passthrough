#!/usr/bin/env python3
"""The render plugin's colour table: where it comes from and how it reaches the plugin.

The plugin feeds WoW's picture into BG3's own rendering. For that it must know, for every colour WoW displays, the
light BG3's display transform shows as that colour. That is this table: 17 x 17 x 17 displayed colours, log2 of the
light for each channel, measured once against BG3 (plugins/capture/fxbg-table.bin in the repository).

The plugin reads it from the player's data folder (capture/fxbg-table.bin). Without it every colour reads as the same
light: WoW's picture goes into BG3 as one grey, none of WoW's light reaches BG3's characters (black figures with a pale
ring round them), and nothing says why. For a long time the file reached that folder by hand alone, and a cleared data
folder or a new install had none. So: the release carries the file (release/build-release.py), the coordinator puts it
in place at every start of world mode (place(), from wowbg3-coord.py), the plugin says NO COLOUR TABLE in its log for
as long as it has none, and tools/test_colour_table.py fails when any of that is taken out.
"""
import math
import os
import struct
from pathlib import Path

NAME = 'fxbg-table.bin'
VALUES = 17 * 17 * 17 * 3   # FB_TAB in plugins/capture/wowbg3_fxbg.inc.c
SIZE = VALUES * 4


def problem(path):
    """Why the file at `path` is no colour table, or None when it is one."""
    path = Path(path)
    if not path.is_file():
        return 'missing'
    data = path.read_bytes()
    if len(data) != SIZE:
        return '%d bytes, not %d' % (len(data), SIZE)
    v = struct.unpack('<%df' % VALUES, data)
    if not all(math.isfinite(x) for x in v):
        return 'holds values that are not numbers'
    if max(v) - min(v) < 1.0:
        return 'every colour reads as the same light (an empty table)'
    return None


def place(shipped, capture):
    """The release's table into the data folder's capture/ when that one is missing or differs.

    Returns (ok, text): ok False when the plugin is left without a usable table."""
    shipped, capture = Path(shipped), Path(capture)
    table = capture / NAME
    bad = problem(shipped)
    if bad:
        have = problem(table)
        if have:
            return False, 'NO COLOUR TABLE: the release has none (%s: %s) and neither has %s (%s); the picture will be wrong' % (
                shipped, bad, capture, have)
        return True, 'colour table: the release has none (%s: %s); keeping the one in %s' % (shipped, bad, capture)
    if table.is_file() and table.read_bytes() == shipped.read_bytes():
        return True, ''
    capture.mkdir(parents=True, exist_ok=True)
    tmp = capture / (NAME + '.tmp')
    tmp.write_bytes(shipped.read_bytes())
    os.replace(tmp, table)
    return True, 'colour table placed in %s' % capture
