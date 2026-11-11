#!/usr/bin/env python3
"""wow_docks.py: WoW's ships and zeppelins as routes between docks, from the server's and the client's own data.

BG3 has no ship to ride in our level (its platforms do not turn, and a ship does), so a crossing is a move from
one dock to the other. Nothing of a route is typed in here:

  * a ship or zeppelin is a game object of type 15 whose first data field names a taxi path
    (the server's gameobject_template);
  * the path's nodes are in the client's TaxiPathNode.dbc (1.12: id, path, index, map, x, y, z, flags, delay);
    a node with flag 2 is a stop: the vessel waits there for `delay` seconds;
  * a route is a path with two different stops. A stop is where the vessel floats or hangs, beside the dock, not on
    it: the dock is the nearest place there a player can stand (`landing`: the movement map's dry polygons that
    join the walkable world, at a height a gangway reaches).

  routes(rows, nodes)            [{Entry, Name, Path, Stops: [{Map, X, Y, Z, Wait}]}]
  near(routes, map, x, y)        [(route, stop index, yards)] within REACH of the point, nearest first
  landing(map, x, y, z)          (x, y, z) to stand at beside a stop, or None
  area_bit(maps, map, x, y)      the server's area value there (AreaTable.dbc AreaBit), for a dock's name
  wow_docks.py table             every route and where its docks are (needs the server's data folders)
"""
import math
import os
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

REACH = 90.0      # yards from a stop within which a player counts as at its dock (a pier's or a tower's length)
STOP = 2          # TaxiPathNode flag: the vessel stops here
RINGS = (6.0, 10.0, 14.0, 18.0, 24.0, 30.0, 38.0, 48.0, 60.0)   # yards out from the stop a dock is looked for
SPOKES = 24
SEA = 5.0         # a stop below this height is a ship's, on the water: its dock is above the water
ABOVE_WATER = 1.5
DECK = 15.0       # how far over a stop a dock may lie
TOWER = 24.0      # how far under a zeppelin's stop its platform may lie: the stop is the hull's middle, and the
                  # towers' platforms lie 17 to 19 yards under it (Orgrimmar, Undercity, Grom'gol); the ground is 50 under


def read_nodes(dbc_dir):
    """{path: [(index, map, x, y, z, flags, delay)]} in the path's order."""
    b = (Path(dbc_dir) / 'TaxiPathNode.dbc').read_bytes()
    magic, n, _, size, _ = struct.unpack_from('<4s4I', b, 0)
    if magic != b'WDBC':
        raise ValueError('TaxiPathNode.dbc: not a DBC file')
    out = {}
    for i in range(n):
        _, path, index, m, x, y, z, flags, delay = struct.unpack_from('<4I3f2I', b, 20 + i * size)
        out.setdefault(path, []).append((index, m, x, y, z, flags, delay))
    for v in out.values():
        v.sort()
    return out


def routes(rows, nodes):
    """rows: [(entry, name, taxi path)] of the server's type 15 game objects. A path with fewer than two
    different stops is no crossing (Naxxramas hangs at one)."""
    out = []
    for entry, name, path in rows:
        stops = []
        for _, m, x, y, z, flags, delay in nodes.get(int(path), []):
            if not flags & STOP:
                continue
            if any(s['Map'] == m and math.hypot(s['X'] - x, s['Y'] - y) < 1.0 for s in stops):
                continue   # a round trip names its first dock again at the end
            stops.append({'Map': m, 'X': x, 'Y': y, 'Z': z, 'Wait': delay})
        if len(stops) >= 2:
            out.append({'Entry': int(entry), 'Name': str(name), 'Path': int(path), 'Stops': stops})
    return sorted(out, key=lambda r: r['Entry'])


def near(all_routes, map_id, x, y, reach=REACH):
    out = []
    for r in all_routes:
        for i, s in enumerate(r['Stops']):
            d = math.hypot(s['X'] - x, s['Y'] - y)
            if s['Map'] == map_id and d <= reach:
                out.append((r, i, d))
    return sorted(out, key=lambda t: t[2])


class Columns:
    """The movement map's columns round one place, each tile read and joined once (wow_stand.column reads its nine
    tiles at every call: 216 samples a dock took 16 s)."""
    def __init__(self, maps, mmaps, map_id):
        self.maps, self.mmaps, self.map_id = maps, mmaps, map_id
        self.tiles, self.sets, self.ensured = {}, {}, set()

    def tile(self, i, j):
        import wow_stand
        if (i, j) not in self.tiles:
            p = Path(self.mmaps) / ('%03d%02d%02d.mmtile' % (self.map_id, i, j))
            self.tiles[(i, j)] = wow_stand.read_tile(p) if p.exists() else None
        return self.tiles[(i, j)]

    def __call__(self, x, y):
        import wow_arena
        import wow_stand
        tx, ty = int(32 - x / wow_arena.GRID), int(32 - y / wow_arena.GRID)
        if (tx, ty) not in self.ensured:
            self.ensured.add((tx, ty))
            wow_arena.ensure_mmtiles(self.mmaps, self.maps, self.map_id, x, y, 0.0)
        if (tx, ty) not in self.sets:
            tiles = {(i, j): self.tile(i, j) for i in range(tx - 1, tx + 2) for j in range(ty - 1, ty + 2)}
            tiles = {k: v for k, v in tiles.items() if v is not None}
            self.sets[(tx, ty)] = (tiles,) + tuple(wow_stand.joined(tiles)) if (tx, ty) in tiles else None
        got = self.sets[(tx, ty)]
        if got is None:
            return [], 0
        tiles, find, size = got
        out = []
        for n, p in enumerate(tiles[(tx, ty)]):
            if p is not None and wow_arena.inside(x, y, p[0]):
                h = wow_arena.poly_height(x, y, p[0])
                if h is not None:
                    out.append((h, size[find(((tx, ty), n))], p[2]))
        return out, max(size.values())


def landing(map_id, x, y, z, maps=None, mmaps=None, column=None):
    """Where a player stands beside a stop: on the nearest ring round it that has a dry polygon of the movement
    map joined to the walkable world at a dock's height, the point of that ring nearest the stop's own height
    over the water (a ship) or under the hull (a zeppelin). None: nothing to stand on within the rings.
    `column(x, y)` -> ([(height, joined size, flags)], the largest joined piece): the movement map (a test's own)."""
    import wow_arena
    if column is None:
        import wow_stand
        column = Columns(maps or wow_stand.data_dir('WOW_BG3_MAPS', 'maps'), mmaps or wow_stand.data_dir('WOW_BG3_MMAPS', 'mmaps'), map_id)
    low, high = (z + ABOVE_WATER, z + DECK) if z < SEA else (z - TOWER, z + DECK)
    for r in RINGS:
        found = []
        for k in range(SPOKES):
            a = 2.0 * math.pi * k / SPOKES
            px, py = x + r * math.cos(a), y + r * math.sin(a)
            col, biggest = column(px, py)
            for h, size, flags in col:
                if wow_arena.Poly([], flags).water or size < biggest or not low <= h <= high:
                    continue
                found.append((abs(h - z), px, py, h))
        if found:
            _, px, py, h = min(found)
            return (px, py, h)
    return None


def area_bit(maps, map_id, x, y):
    """The server's area value at a point, or None: its map file's 16 x 16 area grid (GridMap::getArea), a value of
    AreaTable.dbc's AreaBit column. WoW's zone maps (WorldMapArea) are rectangles that overlap, and a dock at a
    zone's edge lies in two or three of them."""
    import wow_arena
    try:
        raw = (Path(maps) / ('%03d%02d%02d.map' % (map_id, int(32 - x / wow_arena.GRID), int(32 - y / wow_arena.GRID)))).read_bytes()
        off, = struct.unpack_from('<I', raw, 8)
        four, flags, whole = struct.unpack_from('<4sHH', raw, off)
        if four != b'AREA':
            return None
        if flags & 1:       # MAP_AREA_NO_AREA: one area for the whole file
            return whole
        lx, ly = int(16 * (32 - x / wow_arena.GRID)) & 15, int(16 * (32 - y / wow_arena.GRID)) & 15
        return struct.unpack_from('<H', raw, off + 8 + 2 * (lx * 16 + ly))[0]
    except (OSError, struct.error):
        return None


def other(route, index):
    """The stops a vessel goes on to from one of its stops (one for every route in WoW but the ferry's round)."""
    return [s for i, s in enumerate(route['Stops']) if i != index]


def main(argv):
    if argv[1:2] != ['table']:
        print(__doc__)
        return 2
    import wowbg3cmd
    rows = [l.split('\t') for l in wowbg3cmd.sql('SELECT entry, name, data0 FROM gameobject_template WHERE type=15', 'mangos').strip().split('\n') if l]
    for r in routes(rows, read_nodes(wowbg3cmd.dbc_dir())):
        print('%d %s (path %d)' % (r['Entry'], r['Name'], r['Path']))
        for s in r['Stops']:
            at = landing(s['Map'], s['X'], s['Y'], s['Z'])
            print('  map %d stop %.1f %.1f %.1f wait %d s -> dock %s' % (s['Map'], s['X'], s['Y'], s['Z'], s['Wait'],
                                                                      '%.1f %.1f %.1f' % at if at else 'NONE'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
