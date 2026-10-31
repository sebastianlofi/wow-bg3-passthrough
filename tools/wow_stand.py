#!/usr/bin/env python3
"""wow_stand.py: the height a WoW player stands at on a map point, for a move that names only X and Y.

The terrain is not it where a city's floors lie over the terrain (Orgrimmar 50 70: terrain -2.8, street 12.3),
and the top surface is not it where a roof is (a Razor Hill inn: ground 16.5, roof 45.6). The movement map's
polygons in that column that join the rest of the walkable world are the candidates: a closed-off roof is an
island of a few polygons. Orgrimmar's roofs and the terrain under it join too, so the lowest one wins, not
counting a cave below the terrain or bare terrain with a built floor over it. A bridge over open ground is
such a floor too, and upstairs wins where the ground floor is bare terrain. The height is moved onto the
surface WoW draws there.

  wow_stand.py MAP X Y    prints the height and the movement map's heights in that column
"""
import collections, math, os, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wow_arena  # noqa: E402

SNAP_YD = 4.0   # the polygons are a coarse fit: the surface drawn may lie this far from them


def data_dir(env, name):
    return Path(os.environ.get(env) or (os.environ.get('WOWBG3_TEST_STACK') or '/opt/stacks/wow-bg3') + '/storage/mangosd/extracted-data/' + name)


def read_tile(path):
    """[(corners in WoW space, neighbours, flags)] per polygon of a Detour tile; None for one nobody walks."""
    raw = path.read_bytes()
    hdr = struct.unpack_from('<15i3f3f3ff', raw, 20)
    poly_count, vert_count = hdr[6], hdr[7]
    off = 120
    verts = struct.unpack_from('<%df' % (3 * vert_count), raw, off)
    off += 12 * vert_count
    out = []
    for i in range(poly_count):
        rest = struct.unpack_from('<I6H6HHBB', raw, off + 32 * i)[1:]
        vidx, nei, flags, nv, area = rest[:6], rest[6:12], rest[12], rest[13], rest[14]
        if (area >> 6) != 0 or flags == 0 or nv < 3:
            out.append(None)
        else:
            out.append(([(verts[3 * v + 2], verts[3 * v], verts[3 * v + 1]) for v in vidx[:nv]], nei[:nv], flags))
    return out


def joined(tiles):
    """Union-find over the polygons of these tiles: (find, polygons per root). Polygons of two tiles join
    along an outer edge (Detour's 0x8000 neighbours) with the same middle at about the same height."""
    parent = {}

    def find(a):
        parent.setdefault(a, a)
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    outer = collections.defaultdict(list)
    for key, polys in tiles.items():
        for i, p in enumerate(polys):
            if p is None:
                continue
            find((key, i))
            pts, nei, _ = p
            for e, n in enumerate(nei):
                if n & 0x8000:
                    a, b = pts[e], pts[(e + 1) % len(pts)]
                    outer[(round(a[0] + b[0]), round(a[1] + b[1]))].append(((key, i), (a[2] + b[2]) / 2))
                elif n and polys[n - 1] is not None:
                    parent[find((key, i))] = find((key, n - 1))
    for edges in outer.values():
        for a, za in edges:
            for b, zb in edges:
                if a[0] != b[0] and abs(za - zb) < 3.0:
                    parent[find(a)] = find(b)
    return find, collections.Counter(find(a) for a in list(parent))


def column(mmaps, map_id, x, y):
    """[(height, polygons joined to it, polygon flags)] of the movement map in this column, and the size of
    the largest joined piece in the tiles read; the centre tile and whichever of its neighbours exist."""
    tx, ty = int(32 - x / wow_arena.GRID), int(32 - y / wow_arena.GRID)
    tiles = {}
    for i in range(tx - 1, tx + 2):
        for j in range(ty - 1, ty + 2):
            p = Path(mmaps) / ('%03d%02d%02d.mmtile' % (map_id, i, j))
            if p.exists():
                tiles[(i, j)] = read_tile(p)
    if (tx, ty) not in tiles:
        return [], 0
    find, size = joined(tiles)
    out = []
    for i, p in enumerate(tiles[(tx, ty)]):
        if p is not None and wow_arena.inside(x, y, p[0]):
            h = wow_arena.poly_height(x, y, p[0])
            if h is not None:
                out.append((h, size[find(((tx, ty), i))], p[2]))
    return out, max(size.values())


def height(map_id, x, y, terrain=None):
    """The height to stand at on (x, y); the terrain's where the movement map has nothing there."""
    maps, mmaps, vmaps = data_dir('WOW_BG3_MAPS', 'maps'), data_dir('WOW_BG3_MMAPS', 'mmaps'), data_dir('WOW_BG3_VMAPS', 'vmaps')
    terrain = terrain or wow_arena.Terrain(maps, map_id)
    th = terrain.height(x, y)
    wow_arena.ensure_mmtiles(mmaps, maps, map_id, x, y, 0.0)
    col, _ = column(mmaps, map_id, x, y)
    dry = [c for c in col if not wow_arena.Poly([], c[2]).water]
    col = dry or col
    if not col:
        return th
    biggest = max(c[1] for c in col)
    joined_hs = sorted(c[0] for c in col if c[1] == biggest)
    above = [h for h in joined_hs if h >= th - SNAP_YD] or joined_hs   # not a cave under the point
    import wow_vmap
    tris, _ = wow_vmap.VMap(vmaps, map_id).triangles(x - 3, x + 3, y - 3, y + 3, m2=False)
    floors = [f[0] for f in wow_arena.Surfaces(tris).floors(x, y)]

    def on_model(h):
        return any(abs(z - h) <= SNAP_YD for z in floors)
    best = above[0]
    if abs(best - th) <= SNAP_YD and not on_model(best):
        over = [h for h in above[1:] if on_model(h)]
        best = over[0] if over else best   # bare terrain with a built floor over it: the terrain under a city
    drawn = floors + ([] if terrain.hole(x, y) else [th])
    near = [z for z in drawn if abs(z - best) <= SNAP_YD]
    return min(near, key=lambda z: abs(z - best)) if near else best


if __name__ == '__main__':
    m, wx, wy = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
    print('%.2f' % height(m, wx, wy), [(round(h, 2), n) for h, n, _ in column(data_dir('WOW_BG3_MMAPS', 'mmaps'), m, wx, wy)[0]])
