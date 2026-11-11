#!/usr/bin/env python3
"""wow_places.py: where each WoW map lies in the one BG3 level.

Every WoW map (the two continents, every dungeon, raid and battleground, the tram) has its own coordinates,
and they overlap: the same numbers are a place in Kalimdor and a place in the Eastern Kingdoms. So each map
gets a place of its own in the level: a centre, and from it world mode's mapping

  BG3 = centre(map) + (wow y, wow z, -wow x) * 0.9144

The Eastern Kingdoms keep the centre world mode has always used. Every centre is a whole number of 64 m
squares from it, so one lattice holds every map's squares. Two layouts:

  stacked   the level keeps its width; Kalimdor lies FLOOR metres above the Eastern Kingdoms and every
            other map in rows on a third floor
  flat      everything on one floor in a wider level

  places(layout)            {map: centre}
  centre(map, layout)       KeyError for a map WoW's data has no ground for
  box(map, layout)          (x0, y0, z0, x1, y1, z1) in BG3: all the map's ground can lie in
  bounds(layout)            the level's bounds that hold every box
  to_bg3(map, wow, layout)  to_wow(map, bg3, layout)  map_at(bg3, layout)

  wow_places.py table [LAYOUT]

A place is in every save and every kept cut: TILES and the rules below must not change once released.
"""
import math
import sys

YD = 0.9144
CELL = 64
TILE = 533.33333 * YD      # one WoW map tile in metres
BASE = (0.0, 1000.0, 0.0)  # the Eastern Kingdoms' centre (world_bridge.py FIXED_CENTRE): WoW's sea level at y 1000
FLOOR = 4096.0             # metres between floors of the stacked layout
# A map's ground lies this far under and over its centre at most. Measured over every map's movement tiles:
# -504 to 1970 yd (Kalimdor both); only the two unused test maps (13, 29) reach lower, to -1132 yd.
BELOW, ABOVE = 1000.0, 2500.0
GAP = 512.0                # metres kept between two maps (past any BG3 sight or spell reach)
LAYOUT = 'stacked'
EDGE = 16000 - 128         # the stacked level's usable half width

# The 1.12 client's maps with ground: the tiles (first index from WoW x, second from WoW y, both 0 to 63)
# that the server's height maps or movement tiles exist for: (first lo, first hi, second lo, second hi).
TILES = {
    0: (20, 61, 24, 44), 1: (0, 55, 0, 50),
    13: (29, 34, 29, 34), 29: (29, 34, 29, 34), 30: (29, 35, 30, 34), 33: (30, 34, 25, 29),
    34: (31, 32, 31, 32), 35: (31, 32, 31, 32), 36: (30, 35, 30, 35), 37: (29, 33, 29, 34),
    43: (31, 32, 30, 32), 44: (31, 32, 31, 32), 47: (27, 28, 27, 29), 48: (32, 33, 31, 32),
    70: (31, 32, 31, 32), 90: (32, 33, 30, 32), 109: (32, 33, 31, 32), 129: (26, 29, 27, 32),
    169: (25, 40, 24, 39), 189: (27, 32, 28, 33), 209: (27, 33, 29, 31), 229: (31, 32, 31, 33),
    230: (29, 32, 31, 33), 249: (31, 32, 31, 32), 269: (25, 36, 17, 32), 289: (29, 32, 30, 33),
    309: (52, 56, 33, 37), 329: (24, 27, 36, 40), 349: (29, 32, 31, 33), 369: (31, 32, 27, 32),
    389: (31, 32, 31, 32), 409: (29, 31, 32, 34), 429: (30, 32, 30, 33), 449: (31, 32, 31, 32),
    450: (31, 32, 31, 32), 451: (0, 2, 0, 63), 469: (44, 47, 32, 35), 489: (27, 30, 27, 30),
    509: (46, 53, 26, 30), 529: (28, 31, 28, 31), 531: (46, 50, 26, 30), 533: (24, 27, 37, 42),
}
CONTINENTS = (0, 1)
_cache = {}


def _flat_box(m):
    """(x0, z0, x1, z1): the map's tiles in BG3 metres round a centre of 0."""
    a0, a1, b0, b1 = TILES[m]
    return (32 - (b1 + 1)) * TILE, -(32 - a0) * TILE, (32 - b0) * TILE, -(32 - (a1 + 1)) * TILE


def _up(v):
    return math.ceil(v / CELL) * CELL


def _rows(maps, xlo, xhi, zlo):
    """{map: (x offset, z offset)}: boxes left to right in rows, GAP apart, each offset whole squares."""
    out, x, z, depth = {}, xlo, zlo, 0.0
    for m in maps:
        x0, z0, x1, z1 = _flat_box(m)
        if x > xlo and _up(x - x0) + x1 > xhi:
            x, z, depth = xlo, z + depth + GAP, 0.0
        ox, oz = _up(x - x0), _up(z - z0)
        out[m] = (ox, oz)
        x = ox + x1 + GAP
        depth = max(depth, oz + z1 - z)
    return out


def places(layout=LAYOUT):
    """{map: centre} for a layout."""
    if layout in _cache:
        return _cache[layout]
    rest = sorted(m for m in TILES if m not in CONTINENTS)
    out = {0: BASE}
    if layout == 'stacked':
        out[1] = (BASE[0], BASE[1] + FLOOR, BASE[2])
        for m, (ox, oz) in _rows(rest, -EDGE, EDGE, -EDGE).items():
            out[m] = (BASE[0] + ox, BASE[1] + 2 * FLOOR, BASE[2] + oz)
    elif layout == 'flat':
        ex0, ez0, ex1, ez1 = _flat_box(0)
        kx0, kz0, kx1, kz1 = _flat_box(1)
        kx = _up(ex1 + GAP - kx0)
        out[1] = (BASE[0] + kx, BASE[1], BASE[2])
        zlo = _up(max(ez1, kz1) + GAP)
        for m, (ox, oz) in _rows(rest, -_up(-ex0), kx + kx1, zlo).items():
            out[m] = (BASE[0] + ox, BASE[1], BASE[2] + oz)
    else:
        raise ValueError('no layout %r' % layout)
    _cache[layout] = out
    return out


def centre(m, layout=LAYOUT):
    return places(layout)[int(m)]


def box(m, layout=LAYOUT):
    c = centre(m, layout)
    x0, z0, x1, z1 = _flat_box(int(m))
    return c[0] + x0, c[1] - BELOW, c[2] + z0, c[0] + x1, c[1] + ABOVE, c[2] + z1


def bounds(layout=LAYOUT):
    """(x0, y0, z0, x1, y1, z1) of a level that holds every map, with room at the edges."""
    boxes = [box(m, layout) for m in places(layout)]
    lo = [min(b[i] for b in boxes) for i in range(3)]
    hi = [max(b[i + 3] for b in boxes) for i in range(3)]
    if layout == 'stacked':
        return -16000, -100, -16000, 16000, int(_up(hi[1] + 128)), 16000
    return (-int(_up(128 - lo[0])), -100, -int(_up(128 - lo[2])),
            int(_up(hi[0] + 128)), int(_up(hi[1] + 128)), int(_up(hi[2] + 128)))


def to_bg3(m, w, layout=LAYOUT):
    c = centre(m, layout)
    return c[0] + w[1] * YD, c[1] + w[2] * YD, c[2] - w[0] * YD


def to_wow(m, b, layout=LAYOUT):
    c = centre(m, layout)
    return -(b[2] - c[2]) / YD, (b[0] - c[0]) / YD, (b[1] - c[1]) / YD


def map_at(b, layout=LAYOUT):
    """The map whose box a BG3 point lies in, or None."""
    for m in places(layout):
        x0, y0, z0, x1, y1, z1 = box(m, layout)
        if x0 <= b[0] <= x1 and y0 <= b[1] <= y1 and z0 <= b[2] <= z1:
            return m
    return None


def main(argv):
    if len(argv) >= 2 and argv[1] == 'table':
        layout = argv[2] if len(argv) > 2 else LAYOUT
        print(layout, 'bounds', bounds(layout))
        for m in sorted(places(layout)):
            b = box(m, layout)
            print('%4d  centre %9.0f %7.0f %9.0f   x %7.0f..%7.0f  z %7.0f..%7.0f' % ((m,) + centre(m, layout) + (b[0], b[3], b[2], b[5])))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
