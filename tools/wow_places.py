#!/usr/bin/env python3
"""wow_places.py: where each WoW map lies in the one BG3 level.

Every WoW map (the two continents, every dungeon, raid and battleground, the tram) has its own coordinates,
and they overlap: the same numbers are a place in Kalimdor and a place in the Eastern Kingdoms. So each map
gets a place of its own in the level: a centre, and from it world mode's mapping

  BG3 = centre(map) + (wow y, wow z, -wow x) * 0.9144

The level is one floor, every map side by side (the maintainer, 2026-11-14: "one huge flat map instead of laid out on top
of each other"; stacked floors put a member left in the Eastern Kingdoms under one in Kalimdor, same spot from above,
for BG3's minimap). The Eastern Kingdoms stay where world mode has always had them (their saves and cuts with them);
Kalimdor east of the level's old edge, past the parked pool; every other map in rows north of Kalimdor, from the
Eastern Kingdoms' east edge to Kalimdor's; Development Land (451, a strip 31 km long) north of them all. The camp's corner, the start square
and the pool stay where they were. Every centre is a whole number of 64 m squares from the first, so one lattice
holds every map's squares. (A wide level tried once before failed only because the plugin's ray proxy, a scenery
object, was no longer under the middle of the level's grid: release/levels/*.json puts it there, PROXY_AT.)

  places()              {map: centre}
  centre(map)           KeyError for a map WoW's data has no ground for
  box(map)              (x0, y0, z0, x1, y1, z1) in BG3: all the map's ground can lie in
  bounds()              the level's bounds that hold every box
  to_bg3(map, wow)      to_wow(map, bg3)      map_at(bg3)
  level(spec)           a level's spec (release/levels/*.json) with bounds that hold every place

  wow_places.py table
  wow_places.py level SPEC.json   the fitted spec on stdout

A place is in every save and every kept cut: TILES and the rules below must not change once released.
"""
import math
import sys

YD = 0.9144
CELL = 64
TILE = 533.33333 * YD      # one WoW map tile in metres
BASE = (0.0, 1000.0, 0.0)  # the Eastern Kingdoms' centre (world_bridge.py FIXED_CENTRE): WoW's sea level at y 1000
# Where the rows of the other maps begin (east of the Eastern Kingdoms' place), and Kalimdor's west edge, past the
# parked pool (release/levels: BlankGrid X0 15648).
BAND_X0 = 4416.0
POOL_X0 = 15648.0
KALIMDOR_X0 = 16512.0
WIDE = (451,)              # maps wider than the band: their own strip along the north (z past every other place)
# A map's ground lies this far under and over its centre at most. Measured over every map's movement tiles:
# -504 to 1970 yd (Kalimdor both); only the two unused test maps (13, 29) reach lower, to -1132 yd.
BELOW, ABOVE = 1000.0, 2500.0
GAP = 512.0                # metres kept between two maps (past any BG3 sight or spell reach)
EDGE = 16000 - 128         # the level's usable half width

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


def _fill(maps, regions):
    """{map: (x offset, z offset)}: _rows over several regions (x0, x1, z0, z1) in turn: a map whose row would leave
    its region's depth goes on in the next one (the last one has no depth limit)."""
    out, ri = {}, 0
    x0r, x1r, z0r, z1r = regions[0]
    x, z, depth = x0r, z0r, 0.0
    for m in maps:
        bx0, bz0, bx1, bz1 = _flat_box(m)
        while True:
            if x > x0r and _up(x - bx0) + bx1 > x1r:
                x, z, depth = x0r, z + depth + GAP, 0.0
            ox, oz = _up(x - bx0), _up(z - bz0)
            if _up(x - bx0) + bx1 <= x1r and oz + bz1 <= z1r:
                break
            ri += 1
            x0r, x1r, z0r, z1r = regions[ri]
            x, z, depth = x0r, z0r, 0.0
        out[m] = (ox, oz)
        x = ox + bx1 + GAP
        depth = max(depth, oz + bz1 - z)
    return out


def places():
    """{map: centre}: one floor (BASE's height), side by side."""
    if not _cache:
        _cache[0] = BASE
        k0 = _flat_box(1)[0]
        _cache[1] = (BASE[0] + _up(KALIMDOR_X0 - k0), BASE[1], BASE[2])
        # Deepest first (then by number): rows of like depth waste the least of the level.
        rest = sorted((m for m in TILES if m not in CONTINENTS and m not in WIDE),
                      key=lambda m: (-(_flat_box(m)[3] - _flat_box(m)[1]), m))
        k = _flat_box(1)
        east, north_k = _cache[1][0] + k[2], _cache[1][2] + k[3]
        # The band between the Eastern Kingdoms and Kalimdor first (its pool corner kept clear), then north of Kalimdor.
        regions = [(BAND_X0, POOL_X0 - GAP, -EDGE, north_k), (BAND_X0, east, north_k + GAP, 10 ** 9)]
        for m, (ox, oz) in _fill(rest, regions).items():
            _cache[m] = (BASE[0] + ox, BASE[1], BASE[2] + oz)
        north = max(max(c[2] + _flat_box(m)[3] for m, c in list(_cache.items())), EDGE + 128)
        for m, (ox, oz) in _rows(list(WIDE), -EDGE, 10 ** 9, north + GAP).items():
            _cache[m] = (BASE[0] + ox, BASE[1], BASE[2] + oz)
    return _cache


def centre(m):
    return places()[int(m)]


def box(m):
    c = centre(m)
    x0, z0, x1, z1 = _flat_box(int(m))
    return c[0] + x0, c[1] - BELOW, c[2] + z0, c[0] + x1, c[1] + ABOVE, c[2] + z1


def bounds():
    """(x0, y0, z0, x1, y1, z1) of the level: its old south-west corner (the camp, the start square), east and north
    as far as the places reach, as high as their ground."""
    top = max(box(m)[4] for m in places())
    east = max(box(m)[3] for m in places())
    north = max(box(m)[5] for m in places())
    return -16000, -100, -16000, int(_up(east + 128)), int(_up(top + 128)), int(_up(north + 128))


def proxy_at():
    """Where the plugin's ray proxy (a static scenery object) goes: under the middle of the level's grid, where the
    plugin's first probe column is (scene_proxy: a 7 x 7 lattice over the grid's bounds, its middle first)."""
    b = bounds()
    return [(b[0] + b[3]) / 2.0, -40.0, (b[2] + b[5]) / 2.0]


def to_bg3(m, w):
    c = centre(m)
    return c[0] + w[1] * YD, c[1] + w[2] * YD, c[2] - w[0] * YD


def to_wow(m, b):
    c = centre(m)
    return -(b[2] - c[2]) / YD, (b[0] - c[0]) / YD, (b[1] - c[1]) / YD


def map_at(b):
    """The map whose box a BG3 point lies in, or None."""
    for m in places():
        x0, y0, z0, x1, y1, z1 = box(m)
        if x0 <= b[0] <= x1 and y0 <= b[1] <= y1 and z0 <= b[2] <= z1:
            return m
    return None


def level(spec):
    """A level's spec with the bounds that hold every place: the far corner square BG3 sizes its maps by at the new
    north-east corner, the ray proxy under the grid's middle. The start square, the parked pool, the near corner and
    the camp stay where they are."""
    b = bounds()
    subs = [dict(q) for q in spec['Subgrids']]
    far = max(subs, key=lambda q: (q['X'], q['Z']))
    far['X'], far['Z'] = b[3] - CELL, b[5] - CELL
    scenery = [dict(o, Pos=proxy_at()) if 'RayProxy' in o.get('Name', '') else o for o in spec.get('Scenery', [])]
    return dict(spec, Bounds=list(b), Subgrids=subs, Scenery=scenery)


def level_squares(spec):
    """[(x0, y0, z0, x1, y1, z1)]: every square a level's spec places itself (corners, start, parked pool)."""
    g = spec['BlankGrid']
    nx, nz = g.get('NX', g.get('N')), g.get('NZ', g.get('N'))
    sq = [(q['X'], q['Y'], q['Z']) for q in spec['Subgrids']]
    sq += [(g['X0'] + CELL * i, g['Y'], g['Z0'] + CELL * j) for i in range(nx) for j in range(nz)]
    return [(x, y, z, x + CELL, y, z + CELL) for x, y, z in sq]


def main(argv):
    if len(argv) == 3 and argv[1] == 'level':
        import json
        print(json.dumps(level(json.load(open(argv[2]))), indent=1))
        return 0
    if len(argv) == 2 and argv[1] == 'table':
        print('bounds', bounds())
        for m in sorted(places()):
            b = box(m)
            print('%4d  centre %9.0f %7.0f %9.0f   x %7.0f..%7.0f  z %7.0f..%7.0f' % ((m,) + centre(m) + (b[0], b[3], b[2], b[5])))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
