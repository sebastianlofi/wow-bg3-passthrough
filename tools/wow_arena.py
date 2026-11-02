#!/usr/bin/env python3
"""Cut a WoW arena into BG3-sized collision data (collision hooks, patch 0003).

wow_arena.py OUT_DIR --wow X Y [--size 40] --bg3 X Y Z [--mirror 1|0] [--islands largest|border]
             [--maps DIR] [--mmaps DIR] [--models [all|wmo]] [--walls auto|all|none] [--vmaps DIR]
             [--margin 0.5] [--max-triangles 60000] [--map 0|1]
             [--slope-limit 50] [--steep-polys keep|drop] [--storeys 1|0] [--under X Y Z]

Reads the test server's published movement maps (Detour tiles) and terrain heights for map MAP
(0 Eastern Kingdoms, the default; 1 Kalimdor)
and writes, for a square of SIZE metres centred on the WoW point:
  tiles.bin      (N*N) records <B f>: walkable flag, floor height in BG3 world metres, 0.5 m tiles,
                 rows along BG3 +Z, columns along BG3 +X, starting at the arena's min corner.
                 Walkable where the movement maps have a polygon near the terrain; the floor is the
                 surface WoW draws at the tile's middle: the terrain (Terrain.height, WoW's own mesh)
                 or, with --models, a model's floor (a bridge deck, a rock top) when the movement
                 map's polygon lies nearer to it. Detour polygon heights alone were up to a yard or
                 more off WoW's ground on hills (Tav's feet sank or floated).
  spans.bin      (N*N) records <f f>: the ground's lowest and highest point in each tile (BG3 metres;
                 world_bridge.py can give BG3 these as the tile's MinHeight and MaxHeight)
  slopes.bin     (N*N) bytes: the angle (whole degrees from level) of the surface the tile's floor
                 sits on - WoW's terrain mesh or the model face under it - and 255 where there is
                 none we know (deep water, an ADT hole). Diagnostics; --slope-limit uses the same
                 numbers to decide what a WoW player may walk on.
  storey.bin     (N*N) records <u8 walkable, f32 floor> like tiles.bin: the square's upper storey
                 (--storeys 1, see STOREY_CLEAR), a second floor over the ground's for a second pool
                 subgrid that lies one tile further +X (world mode keeps one subgrid to a lattice
                 slot): record column c is the ground's column c + 1, the last one the next
                 square's first. Flags 1 walkable, 4 | 8 a seam (where it meets the ground: SubgridEdge
                 and SubgridIntersection), 8 alone on the rest inside the square (see the code); on base
                 storey_base_m (arena.json), and where the storey has no floor its tile lies
                 STOREY_FILL_M over that base. Not written with --storeys 0.
  storeyN.bin    The same for level N (N from 2), when a column holds more floors than the levels below
                 took (--storey-levels, default 4; see STOREY_OFFSETS for each level's subgrid offset,
                 arena.json storey_levels lists them). seamsN.bin overlays level N where level N + 1
                 meets it, as seams.bin does the ground.
  seams.bin      (N*N) records <u8 flags, f32 floor> for the ground's own section where the storey
                 meets it: 4 | 8 a seam on a ground tile, 1 | 4 | 8 a tile of the storey's copied
                 into the ground at that floor (BG3 m), 8 alone a tile under the storey, 0 elsewhere.
                 Not written with --storeys 0.
  triangles.bin  float32 x 9 per triangle in BG3 world metres: WoW's terrain mesh (four triangles per
                 4.17 yd cell, ADT holes left out), flat tiles where a walkable floor is on no other
                 surface, plus 4.5 m walls along walkable/blocked tile edges
  kinds.bin      one byte per triangle: 0 terrain, 4 flat floor WoW draws nothing at, 1 wall, 2 WMO,
                 3 M2, 5 M2 at least BIG_M2_YD thick (the plugin's camera clearance bumps into
                 terrain and those; benilla's patch 0092 fades the thinner ones)
                 With --models, WoW's own collision models follow (link/tools/wow_vmap.py: the
                 server's vmaps, WMO buildings and M2 doodads such as trees and fences), clipped to
                 the square plus --margin metres; `--models wmo` leaves the M2s out, as WoW's spell
                 LoS does. Order in the file: terrain, walls, models (counts in arena.json).
                 --walls auto keeps the synthetic walls only where no model is within 1.5 m at wall
                 height (the models are the real walls; what is left marks steep slopes, water
                 edges and the like); all keeps every wall, none drops them. Over --max-triangles
                 (the plugin takes 400000 for up to 6 squares) the smallest objects' triangles go
                 first.
  arena.json     corners, counts, mapping
Mapping: WoW delta (x, y, z) yards -> (y, z, -x) * 0.9144 metres, added to the BG3 centre (--mirror 1,
the default: BG3 renders its world left-handed, so only a mirrored mapping makes BG3's
picture under WoW's camera match WoW's; see the collision-hooks notes). --mirror 0 is bridge/src/coords.rs
at zero yaw, (-y, z, -x), which the patch 0003 proofs used. Size up to 120 m (wowbg3 plugin limits).
Output is derived from Blizzard data: keep it out of git.
"""
import argparse, json, math, os, struct, subprocess
from pathlib import Path

YD = 0.9144
GRID = 533.33333
TILE = 0.5

# vmangos's NavTerrain flags on a movement-map polygon (src/game/Maps/MoveMapSharedDefines.h).
NAV_GROUND = 0x01
NAV_WATER = 0x08
NAV_STEEP_SLOPES = 0x10   # "Slopes above player climb angle": NPCs climb them, players do not.
# WoW's own ground rule, as the 1.12 client enforces it: a surface is walkable only while its normal
# is within 50 degrees of up, and the player slides back down anything steeper. The number is the
# client's own (benilla's reimplementation of its character controller: GROUND_COS = 0.642788 =
# cos 50 degrees, "walkable iff the surface normal is within 50 degrees of up, the reference's limit;
# steeper slides back", benilla-app/src/player/state.rs). vmangos's movement-map generator works to
# the same rule with 52 degrees ("Player slope angle is fix (client side controlled)",
# contrib/mmap/src/TileWorker.cpp) and marks every face between it and the NPC limit (75 degrees on
# terrain, 61 on models) NAV_STEEP_SLOPES, which the server then excludes for players
# (Map::GetWalkHitPosition, PathFinder::ExcludeSteepSlopes).
# The client's companion rule, which the tiles do not carry: over something steeper the player still
# climbs a rise of up to 1 yd (benilla STEP_UP_HEIGHT; 2 yd for everything that is not the player).
# BG3's own rule between neighbour tiles is the walker's MovementStepUpHeight, set by Arena.lua.
PLAYER_SLOPE_DEG = 50.0
# Upper storeys (--storeys): a second floor per tile, for a second pool subgrid stacked over the
# square, so a walker can go upstairs. A storey surface is a movement-map polygon at least
# STOREY_CLEAR yd over the tile's ground floor (a walker's headroom) or, where the ground layer has
# no floor, one 4 yd or more over the terrain (what the ground layer leaves out). It is kept only
# where a walker reaches it from the ground floor stepping under STOREY_STEP_M between neighbour
# tiles (BG3's own rule; Arena.lua's STEP_UP); a storey reaching the square's edge is followed
# STOREY_MARGIN tiles into the neighbours (over the movement map's own polygons) for stairs up to it
# there. Roofs, tower tops and the other surfaces nobody walks up to stay out, and so does any
# storey not on a model face (so none without --models).
STOREY_CLEAR = 2.0
STOREY_STEP_M = 1.0
STOREY_MARGIN = 32
# The movement map's polygons are a coarse, eroded fit: on a narrow stair a tile's middle can miss
# them, and over a landing they sit a step above the treads. So each tile keeps every model face
# near its polygon, the walk up takes the one that goes on from its neighbour, and a tile with no
# polygon there may take a stair's or a ramp's face (STOREY_RAMP_DEG up to the slope limit; never a
# level one, so a sill or a wall's top leads nowhere).
STOREY_RAMP_DEG = 15.0
# Where a storey has no floor its tile lies this far (m) over its base, out of every height lookup.
STOREY_FILL_M = 1000.0
# A piece of storey joins the ground within this much (m) of the top of its way up.
STOREY_JOIN_M = 0.5
# Where each upper level's subgrid lies from its square, in tiles (+X, +Z): world mode keeps one subgrid
# to a lattice slot, so no two may share an offset, and all of them stay inside the cut's one-tile ring.
STOREY_OFFSETS = ((1, 0), (1, 1), (0, 1), (-1, 0), (0, -1), (-1, -1), (1, -1), (-1, 1))
# An upper level over the first with fewer tiles is left out.
STOREY_MIN_LEVEL = 8
# A session whose origin lies this far (yd) under the terrain runs its cuts with --under: the models go down to
# its floors (normally 30 yd under the terrain, and every square would carry them) and its storeys start from the
# origin's own polygons, since Undercity's reach the rest of the world by lift, which the movement map has not.
UNDER_CLEAR = 30.0

# An M2 at least this thick (yards, the smallest side of its collision model in its own axes, times its
# scale, so how it is turned does not matter) is big: the camera
# bumps into it. benilla's patch 0092 draws the same line (phase_out::SMALL_YD). Counted over Elwynn's and
# Durotar's M2s: trees 52% big, rocks 62%, posts 1%; no fence, barrel or bush. Height counts too: a low wide
# piece (a planter) is seen over, and pulling the camera in for it would be wrong.
BIG_M2_YD = 4.0

def detour_polys(path):
    raw = path.read_bytes()
    magic, dtv, mmv, size, liquids = struct.unpack_from('<IIIIB', raw, 0)
    off = 20
    hdr = struct.unpack_from('<15i3f3f3ff', raw, off)
    assert hdr[0] == 0x444E4156, 'not a Detour tile'  # 'DNAV'
    poly_count, vert_count = hdr[6], hdr[7]
    off += 100
    verts = struct.unpack_from(f'<{3 * vert_count}f', raw, off)
    off += 12 * vert_count
    polys = []
    for i in range(poly_count):
        first, *rest = struct.unpack_from('<I6H6HHBB', raw, off + 32 * i)
        vidx, flags, nv, area = rest[0:6], rest[12], rest[13], rest[14]
        if (area >> 6) != 0 or flags == 0 or nv < 3:   # off-mesh connections or unwalkable
            continue
        # Recast space (x, y, z) = WoW (y, z, x)
        pts = [(verts[3 * v + 2], verts[3 * v], verts[3 * v + 1]) for v in vidx[:nv]]
        polys.append(Poly(pts, flags))
    return polys

def main_polys(polys, at=None):
    """The polygons (their id()) in the largest set the movement map joins by shared edges; with `at`
    (x, y, z), in the set of the polygon nearest that point instead (empty if none lies over it)."""
    par = list(range(len(polys)))
    def find(a):
        while par[a] != a:
            par[a] = par[par[a]]
            a = par[a]
        return a
    edges = {}
    for i, p in enumerate(polys):
        for k in range(len(p)):
            a, b = p[k], p[(k + 1) % len(p)]
            ka, kb = (round(a[0], 1), round(a[1], 1), round(a[2])), (round(b[0], 1), round(b[1], 1), round(b[2]))
            j = edges.setdefault((ka, kb) if ka < kb else (kb, ka), i)
            if j != i:
                par[find(i)] = find(j)
    size = {}
    for i in range(len(polys)):
        size[find(i)] = size.get(find(i), 0) + 1
    big = max(size, key=size.get) if size else None
    if at:
        over = [(abs(poly_height(at[0], at[1], p) - at[2]), i) for i, p in enumerate(polys) if inside(at[0], at[1], p)]
        big = find(min(over)[1]) if over else None
    return {id(p) for i, p in enumerate(polys) if find(i) == big}

class Poly(list):
    """A movement-map polygon: its corners in WoW space plus vmangos's NavTerrain flags."""
    def __init__(self, pts, flags):
        super().__init__(pts)
        self.flags = flags

    @property
    def water(self):
        """Deep water (WoW swims there): a water polygon with no ground under it. BG3 walks its surface."""
        return bool(self.flags & NAV_WATER) and not self.flags & NAV_GROUND

    @property
    def steep(self):
        """The generator found the ground here above the player's climb angle (NPCs only)."""
        return bool(self.flags & NAV_STEEP_SLOPES)

def inside(px, py, pts):
    hit = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i][0], pts[i][1]
        xj, yj = pts[j][0], pts[j][1]
        if (yi > py) != (yj > py) and px < (xj - xi) * (py - yi) / (yj - yi) + xi:
            hit = not hit
        j = i
    return hit

def poly_height(px, py, pts):
    a = pts[0]
    for k in range(1, len(pts) - 1):
        b, c = pts[k], pts[k + 1]
        d = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(d) < 1e-9:
            continue
        w1 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / d
        w2 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / d
        w3 = 1 - w1 - w2
        if min(w1, w2, w3) >= -1e-4:
            return w1 * a[2] + w2 * b[2] + w3 * c[2]
    return sum(p[2] for p in pts) / len(pts)

class Terrain:
    """WoW's ground from the server's height maps (vmangos GridMap .map files): the surface WoW draws.
    Each 4.17 yd cell is four triangles fanned round its middle vertex (V9 corners, V8 middle), as the
    client meshes an MCNK chunk (benilla terrain.rs height_at) and the server interpolates
    (GridMap::getHeightFromFloat). Heights come as floats, int16 or int8 (the extractor's
    float-to-int option) or one flat height; ADT holes (cave mouths) per 2 x 2 cells."""
    def __init__(self, maps, map_id=0):
        self.maps, self.cache, self.map = maps, {}, int(map_id)

    def grid(self, gx, gy):
        g = self.cache.get((gx, gy))
        if g is None:
            raw = (self.maps / f'{self.map:03}{gx:02}{gy:02}.map').read_bytes()
            hoff, = struct.unpack_from('<I', raw, 16)
            holes_off, holes_size = struct.unpack_from('<II', raw, 32)
            four, flags, base, top = struct.unpack_from('<4sIff', raw, hoff)
            assert four == b'MHGT', 'not a vmangos height map'
            at = hoff + 16
            if flags & 1:                       # MAP_HEIGHT_NO_HEIGHT: one flat height
                v9, v8 = [base] * 16641, [base] * 16384
            elif flags & 6:                     # MAP_HEIGHT_AS_INT16 (2) or AS_INT8 (4)
                fmt, size, top_value = ('H', 2, 65535) if flags & 2 else ('B', 1, 255)
                k = (top - base) / top_value
                v9 = [base + k * v for v in struct.unpack_from('<16641' + fmt, raw, at)]
                v8 = [base + k * v for v in struct.unpack_from('<16384' + fmt, raw, at + 16641 * size)]
            else:
                v9 = struct.unpack_from('<16641f', raw, at)
                v8 = struct.unpack_from('<16384f', raw, at + 16641 * 4)
            holes = struct.unpack_from('<256H', raw, holes_off) if holes_size == 512 and holes_off else None
            g = self.cache[(gx, gy)] = (v9, v8, holes)
        return g

    @staticmethod
    def _cell_hole(holes, a, b):
        return bool(holes) and bool(holes[(a >> 3) * 16 + (b >> 3)] & (1 << (((a & 7) >> 1) * 4 + ((b & 7) >> 1))))

    def _at(self, x, y):
        fx, fy = 32 - x / GRID, 32 - y / GRID
        gx, gy = int(fx), int(fy)
        u, v = 128 * (fx - gx), 128 * (fy - gy)
        a, b = min(int(u), 127), min(int(v), 127)
        return gx, gy, a, b, u - a, v - b

    def height(self, x, y):
        """WoW's terrain height (yards) at WoW (x, y); holes are ignored (the mesh around them)."""
        gx, gy, a, b, x, y = self._at(x, y)
        v9, v8, _ = self.grid(gx, gy)
        h1, h2 = v9[a * 129 + b], v9[(a + 1) * 129 + b]
        h3, h4 = v9[a * 129 + b + 1], v9[(a + 1) * 129 + b + 1]
        h5 = 2 * v8[a * 128 + b]
        if x + y < 1:
            if x > y:
                return h1 + (h2 - h1) * x + (h5 - h1 - h2) * y
            return h1 + (h5 - h1 - h3) * x + (h3 - h1) * y
        if x > y:
            return h5 - h4 + (h2 + h4 - h5) * x + (h4 - h2) * y
        return h5 - h4 + (h4 - h3) * x + (h3 + h4 - h5) * y

    CELL_YD = GRID / 128   # one terrain cell, 4.167 yd a side

    def slope(self, x, y):
        """The angle (degrees from level) of the terrain face WoW draws at (x, y): the surface the
        client's movement rule tests. The same four triangles per cell as height(), so this is the
        drawn mesh's own slope, not a movement-map polygon's simplified one."""
        gx, gy, a, b, x, y = self._at(x, y)
        v9, v8, _ = self.grid(gx, gy)
        h1, h2 = v9[a * 129 + b], v9[(a + 1) * 129 + b]
        h3, h4 = v9[a * 129 + b + 1], v9[(a + 1) * 129 + b + 1]
        h5 = 2 * v8[a * 128 + b]
        if x + y < 1:
            du, dv = (h2 - h1, h5 - h1 - h2) if x > y else (h5 - h1 - h3, h3 - h1)
        else:
            du, dv = (h2 + h4 - h5, h4 - h2) if x > y else (h4 - h3, h3 + h4 - h5)
        return math.degrees(math.atan(math.hypot(du, dv) / self.CELL_YD))

    def hole(self, x, y):
        gx, gy, a, b, _, _ = self._at(x, y)
        return self._cell_hole(self.grid(gx, gy)[2], a, b)

    def triangles(self, x0, x1, y0, y1):
        """WoW's terrain triangles (9 floats: WoW x, y, z of 3 corners, yards) of every cell reaching
        into the box, ADT holes left out."""
        out = []
        X = lambda i: (32 - i / 128) * GRID  # noqa: E731  full grid index -> WoW coordinate
        i0, i1 = int(math.floor(128 * (32 - x1 / GRID))), int(math.floor(128 * (32 - x0 / GRID)))
        j0, j1 = int(math.floor(128 * (32 - y1 / GRID))), int(math.floor(128 * (32 - y0 / GRID)))
        for i in range(i0, i1 + 1):
            gx, a = divmod(i, 128)
            xa, xb, xm = X(i), X(i + 1), X(i + 0.5)
            for j in range(j0, j1 + 1):
                gy, b = divmod(j, 128)
                v9, v8, holes = self.grid(gx, gy)
                if self._cell_hole(holes, a, b):
                    continue
                ya, yb, ym = X(j), X(j + 1), X(j + 0.5)
                p1, p2 = (xa, ya, v9[a * 129 + b]), (xb, ya, v9[(a + 1) * 129 + b])
                p3, p4 = (xa, yb, v9[a * 129 + b + 1]), (xb, yb, v9[(a + 1) * 129 + b + 1])
                p5 = (xm, ym, v8[a * 128 + b])
                for p, q in ((p1, p2), (p2, p4), (p4, p3), (p3, p1)):
                    out.append(p5 + p + q)
        return out


def tri_z(t, x, y):
    """z of triangle t (9 floats) at column (x, y), None outside it or for a vertical face."""
    ax, ay, bx, by, cx, cy = t[0], t[1], t[3], t[4], t[6], t[7]
    d = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy)
    if abs(d) < 1e-9:
        return None
    l1 = ((by - cy) * (x - cx) + (cx - bx) * (y - cy)) / d
    l2 = ((cy - ay) * (x - cx) + (ax - cx) * (y - cy)) / d
    l3 = 1 - l1 - l2
    if min(l1, l2, l3) < -1e-6:
        return None
    return l1 * t[2] + l2 * t[5] + l3 * t[8]


def wow_vmap_clip(t, x0, x1, y0, y1):
    """A WoW-space triangle clipped to a box (wow_vmap.clip_tri; imported late: plain arenas need no vmaps)."""
    import wow_vmap
    return wow_vmap.clip_tri(t, x0, x1, y0, y1)


class Surfaces:
    """Model faces one could stand on (WoW space, at most 60 degrees from level), bucketed by
    column: floors of buildings, bridge decks, rock tops."""
    CELL = 2.0

    def __init__(self, tris):
        self.tris, self.cells, self.tilt, self.up = tris, {}, {}, {}
        for k in range(0, len(tris), 9):
            ax, ay, az, bx, by, bz, cx, cy, cz = tris[k:k + 9]
            ux, uy, uz, vx, vy, vz = bx - ax, by - ay, bz - az, cx - ax, cy - ay, cz - az
            nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
            n = math.sqrt(nx * nx + ny * ny + nz * nz)
            if n < 1e-9 or abs(nz) < 0.5 * n:
                continue
            self.tilt[k] = math.degrees(math.acos(min(1.0, abs(nz) / n)))
            self.up[k] = nz > 0     # WoW winds its collision faces outward: floors up, ceilings down
            for i in range(int(math.floor(min(ax, bx, cx) / self.CELL)), int(math.floor(max(ax, bx, cx) / self.CELL)) + 1):
                for j in range(int(math.floor(min(ay, by, cy) / self.CELL)), int(math.floor(max(ay, by, cy) / self.CELL)) + 1):
                    self.cells.setdefault((i, j), []).append(k)

    def heights(self, x, y):
        """(height, angle from level) of every model face one could stand on in this column."""
        out = []
        for k in self.cells.get((int(math.floor(x / self.CELL)), int(math.floor(y / self.CELL))), ()):
            z = tri_z(self.tris[k:k + 9], x, y)
            if z is not None:
                out.append((z, self.tilt[k]))
        return out

    def floors(self, x, y):
        """(height, angle from level) of the faces in this column that face up: floors and treads, not
        the ceilings and stair undersides above a room."""
        out = []
        for k in self.cells.get((int(math.floor(x / self.CELL)), int(math.floor(y / self.CELL))), ()):
            z = tri_z(self.tris[k:k + 9], x, y) if self.up[k] else None
            if z is not None:
                out.append((z, self.tilt[k]))
        return out

def ensure_mmtiles(mmaps, maps, map_id, wx, wy, reach):
    """The movement tiles the square needs. The release's portable server
    makes maps and vmaps at its first start but no movement maps (data/mmaps stays empty), and a tile
    is walkable here only on a movement map polygon: without them every tile was blocked, BG3 found no
    valid position in world mode's ground and refused every CreateAt (no stand-ins, no fights). With a
    portable server (WOW_BG3_SERVER) each missing tile the square reaches is built once with its
    `wowbg3-server.sh mmtile` (MoveMapGenerator --tile, 6 to 10 s each, in parallel) and kept in the
    server's data; the running server loads it too. The Docker test server has every tile."""
    server = os.environ.get('WOW_BG3_SERVER')
    script = Path(server) / 'wowbg3-server.sh' if server else None
    if script is None or not script.is_file():
        return
    want = {(int(32 - x / GRID), int(32 - y / GRID)) for x in (wx - reach, wx + reach) for y in (wy - reach, wy + reach)}
    jobs = []
    for tx, ty in sorted(want):
        name = f'{map_id:03}{tx:02}{ty:02}'
        if (Path(mmaps) / f'{name}.mmtile').exists() or (Path(mmaps) / f'{name}.none').exists():
            continue
        if not (Path(maps) / f'{name}.map').exists():
            continue   # no ADT there: nothing to walk on
        # wowbg3-server.sh mmtile MAP X Y writes MAP Y X: its X is our ty, its Y our tx.
        jobs.append((name, subprocess.Popen(['bash', str(script), 'mmtile', str(map_id), str(ty), str(tx)],
                                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)))
    for name, p in jobs:
        p.wait()
        if not (Path(mmaps) / f'{name}.mmtile').exists():
            try:
                (Path(mmaps) / f'{name}.none').touch()   # the generator made nothing: do not try again
            except OSError:
                pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument('out')
    # WOW_BG3_MAPS, _VMAPS, _MMAPS (the release's server data, release/tools/relenv.py).
    p.add_argument('--wow', type=float, nargs=2, required=True)
    p.add_argument('--bg3', type=float, nargs=3, required=True)
    p.add_argument('--size', type=float, default=40.0)
    p.add_argument('--mirror', type=int, choices=[0, 1], default=1)
    p.add_argument('--maps', default=os.environ.get('WOW_BG3_MAPS') or (os.environ.get('WOWBG3_TEST_STACK') or '/opt/stacks/wow-bg3') + '/storage/mangosd/extracted-data/maps')
    p.add_argument('--mmaps', default=os.environ.get('WOW_BG3_MMAPS') or (os.environ.get('WOWBG3_TEST_STACK') or '/opt/stacks/wow-bg3') + '/storage/mangosd/extracted-data/mmaps')
    # largest: keep only the largest walkable area (a lone arena). border: one square of world mode's
    # treadmill, whose ground continues in its neighbours: keep every area that reaches the square's
    # border or is large; only small enclosed islands (inside hollow tree trunks) are blocked.
    p.add_argument('--islands', choices=['largest', 'border'], default='largest')
    p.add_argument('--models', nargs='?', const='all', choices=['all', 'wmo'], default=None)
    p.add_argument('--vmaps', default=os.environ.get('WOW_BG3_VMAPS') or (os.environ.get('WOWBG3_TEST_STACK') or '/opt/stacks/wow-bg3') + '/storage/mangosd/extracted-data/vmaps')
    p.add_argument('--walls', choices=['auto', 'all', 'none'], default='auto')
    p.add_argument('--margin', type=float, default=0.5)
    p.add_argument('--max-triangles', type=int, default=60000)
    p.add_argument('--map', type=int, choices=[0, 1], default=0)
    # WoW's own ground rule in the stamp: no tile whose surface is steeper than this (degrees) is
    # walkable, so BG3's movement circle covers what a WoW player can reach. 0 turns it off (the
    # earlier state: every movement-map polygon counted, NPC-only steep slopes included).
    p.add_argument('--slope-limit', type=float,
                   default=float(os.environ.get('WOW_BG3_SLOPE_LIMIT') or PLAYER_SLOPE_DEG))
    # Also drop tiles whose movement-map polygon is marked NAV_STEEP_SLOPES, where we have no surface
    # of our own to test: deep water, an ADT hole, or --models off (a model floor then sits on the
    # terrain under it). The flag is the generator's verdict over its simplified polygons, coarser
    # than the mesh rule above, so where that rule tested the surface it wins.
    p.add_argument('--steep-polys', choices=['keep', 'drop'],
                   default=os.environ.get('WOW_BG3_STEEP_POLYS') or 'drop')
    # Upper storeys (storey.bin, see STOREY_CLEAR). 0 writes exactly what was written before them.
    p.add_argument('--storeys', type=int, choices=[0, 1], default=int(os.environ.get('WOW_BG3_STOREYS') or 1))
    p.add_argument('--storey-levels', type=int, default=int(os.environ.get('WOW_BG3_STOREY_LEVELS') or 4))
    # A session under the terrain (Undercity, Ironforge): WoW point X Y Z of its origin. See UNDER_CLEAR.
    p.add_argument('--under', type=float, nargs=3, metavar=('X', 'Y', 'Z'))
    a = p.parse_args()
    assert 2 <= a.size <= 120
    sx = 1.0 if a.mirror else -1.0  # BG3 x = sx * WoW y
    out = Path(a.out); out.mkdir(parents=True, exist_ok=True)
    wx, wy = a.wow
    terrain = Terrain(Path(a.maps), a.map)
    wz = terrain.height(wx, wy)
    bx, by, bz = a.bg3
    n = int(round(a.size / TILE))
    half = n * TILE / 2

    # BG3 offset (dx_b, dz_b) metres -> WoW point: dx_b = sx*dy_w*YD, dz_b = -dx_w*YD
    def to_wow(dxb, dzb):
        return wx - dzb / YD, wy + sx * dxb / YD
    def to_bg3(x, y, z):
        return (bx + sx * (y - wy) * YD, by + (z - wz) * YD, bz - (x - wx) * YD)

    gx, gy = int(32 - wx / GRID), int(32 - wy / GRID)
    ensure_mmtiles(a.mmaps, a.maps, a.map, wx, wy, half / YD + 3)
    polys = []
    for tx in range(gx - 1, gx + 2):
        for ty in range(gy - 1, gy + 2):
            f = Path(a.mmaps) / f'{a.map:03}{tx:02}{ty:02}.mmtile'
            if f.exists():
                polys.extend(detour_polys(f))
    reach = half / YD + 3
    near = [pts for pts in polys
            if min(q[0] for q in pts) < wx + reach and max(q[0] for q in pts) > wx - reach
            and min(q[1] for q in pts) < wy + reach and max(q[1] for q in pts) > wy - reach]
    # Polygons bucketed by 4-yard cells of their bounding boxes, so large arenas stay fast.
    CELL = 4.0
    buckets = {}
    for pts in near:
        x0, x1 = min(q[0] for q in pts), max(q[0] for q in pts)
        y0, y1 = min(q[1] for q in pts), max(q[1] for q in pts)
        for i in range(int(math.floor(x0 / CELL)), int(math.floor(x1 / CELL)) + 1):
            for j in range(int(math.floor(y0 / CELL)), int(math.floor(y1 / CELL)) + 1):
                buckets.setdefault((i, j), []).append(pts)

    # The square plus a one-tile ring around it (index -1 and n): walls along the square's border
    # need the neighbours' ground (world mode's squares meet without gaps in their walls).
    # Per tile: WoW's terrain height and the movement map's polygon height nearest to it (walkable
    # when there is one within 4 yd).
    ring = [(r, c) for r in range(-1, n + 1) for c in range(-1, n + 1)]   # rows BG3 +Z, columns +X
    col, terr, mesh, water, steep_poly = {}, {}, {}, {}, {}
    poly_hs = {}    # every polygon height in the tile's middle column but deep water's (storeys)
    # Under the terrain the storeys are the origin's own set of polygons: the ruins or the mountain over it are not.
    under_set = main_polys(polys, a.under) if a.under else None
    for r, c in ring:
        x, y = to_wow(-half + (c + 0.5) * TILE, -half + (r + 0.5) * TILE)
        th = terrain.height(x, y)
        best, wet, stp = None, False, False
        hs = poly_hs[(r, c)] = []
        for pts in buckets.get((int(math.floor(x / CELL)), int(math.floor(y / CELL))), ()):
            if inside(x, y, pts):
                h = poly_height(x, y, pts)
                if best is None or abs(h - th) < abs(best - th):
                    best, wet, stp = h, pts.water, pts.steep
                if not pts.water and (under_set is None or id(pts) in under_set):
                    hs.append(h)
            # Around the middle too, for the storey: a narrow door's polygon can miss a tile's middle.
            elif a.storeys and not pts.water and (under_set is None or id(pts) in under_set):
                for ox, oy in ((0.3, 0.0), (-0.3, 0.0), (0.0, 0.3), (0.0, -0.3)):
                    if inside(x + ox, y + oy, pts):
                        hs.append(poly_height(x + ox, y + oy, pts))
                        break
        col[(r, c)], terr[(r, c)], mesh[(r, c)], water[(r, c)] = (x, y), th, best, wet
        steep_poly[(r, c)] = stp

    # WoW's collision models, cut from the vmaps for the square plus the margin, in WoW space. The z
    # range keeps what is near the ground: 30 yards below the lowest terrain to 100 above the highest
    # (the plugin refuses a set spanning more than 400 m in height).
    wt, sizes, thick, surfaces = [], [], [], None
    if a.models:
        import wow_vmap
        reach_yd = (half + a.margin) / YD
        zlo, zhi = min(terr.values()) - 30, max(terr.values()) + 100
        if a.under:
            zlo = min([zlo] + [h - 5.0 for hs in poly_hs.values() for h in hs])
            zhi = min(zhi, zlo + 390.0 / YD)
        wt, sizes = wow_vmap.VMap(a.vmaps, a.map).triangles(wx - reach_yd, wx + reach_yd, wy - reach_yd, wy + reach_yd,
                                                        zlo, zhi, m2=a.models == 'all', thickness=thick)
        surfaces = Surfaces(wt)

    # Floors: the surface WoW draws in the tile's middle column nearest to the movement map's polygon
    # (terrain, or a model's floor with --models), so Tav stands where WoW's ground is drawn. With no
    # surface within 2 yd of the polygon the terrain wins (Detour polygons on steep slopes stray that
    # far); over an ADT hole the polygon's own height stays ('polygon' tiles, with flat triangles of
    # their own below), as does deep water's surface. Blocked tiles lie on the terrain.
    walk_ring, floor_ring, off_surface, on_terrain = {}, {}, set(), set()
    floor_kind = {'terrain': 0, 'model': 0, 'polygon': 0, 'water': 0}
    tilt_ring = {}          # the angle of the surface each tile's floor sits on (None: none we know)
    floor_yd = {}           # the same floors in WoW yards
    steep_cut = {'mesh': 0, 'poly': 0}
    for rc in ring:
        x, y = col[rc]
        th, best = terr[rc], mesh[rc]
        ts = terrain.slope(x, y)
        # Walkable: a movement-map polygon within 4 yd of the terrain. (Admitting polygons on model
        # floors farther off made the Northshire abbey's roofs and upper floors walkable islands;
        # city streets yards above the terrain, as in parts of Stormwind, stay blocked.)
        w = best is not None and abs(best - th) < 4.0
        models = surfaces.heights(x, y) if surfaces is not None and w else []
        f, tilt = th, ts
        if w and water[rc] and best > th:
            # Deep water (WoW swims): its surface, as before (BG3 cannot swim), with flat triangles.
            f, tilt = best, None
            off_surface.add(rc)
        elif w:
            hole = terrain.hole(x, y)
            cands = ([] if hole else [(th, ts)]) + models
            f, tilt = min(cands, key=lambda c: abs(c[0] - best)) if cands else (best, None)
            if abs(f - best) > 2.0:
                if hole:
                    f, tilt = best, None   # nothing drawn there that we know of: the polygon's height
                    off_surface.add(rc)
                else:
                    f, tilt = th, ts       # a coarse polygon on a steep slope: the terrain is the ground
        if w and rc not in off_surface and f == th:
            on_terrain.add(rc)
        # WoW's own ground rule: the client walks ground up to the player's climb angle and slides
        # the player back down anything steeper, so neither may BG3. The rule is tested on the
        # surface the floor sits on (WoW's drawn terrain mesh, or the model face under the tile),
        # which is finer than the movement map's own simplified polygons. Where we have no surface
        # to test (a deep-water surface, an ADT hole, any model floor with --models off) the
        # movement map's steep flag decides.
        if w and a.slope_limit > 0:
            if tilt is not None and tilt > a.slope_limit:
                w = False
                steep_cut['mesh'] += 1
            elif a.steep_polys == 'drop' and steep_poly[rc] and (tilt is None or surfaces is None):
                w = False
                steep_cut['poly'] += 1
        tilt_ring[rc] = tilt
        if w and 0 <= rc[0] < n and 0 <= rc[1] < n:
            floor_kind['water' if water[rc] and rc in off_surface else 'polygon' if rc in off_surface
                       else 'terrain' if f == th else 'model'] += 1
        walk_ring[rc] = w
        floor_ring[rc] = by + (f - wz) * YD
        floor_yd[rc] = f
    walk = [[walk_ring[(r, c)] for c in range(n)] for r in range(n)]
    floor = [[floor_ring[(r, c)] for c in range(n)] for r in range(n)]

    # Hollow collision shells (big tree trunks) leave walkable islands inside a blocked ring.
    # largest: keep only the largest 4-connected walkable area. border: keep areas that reach the
    # border (they go on in the next square) or are large. Islands become blocked, at ground height.
    seen, comps = [[False] * n for _ in range(n)], []
    for r0 in range(n):
        for c0 in range(n):
            if walk[r0][c0] and not seen[r0][c0]:
                seen[r0][c0], stack, comp = True, [(r0, c0)], []
                while stack:
                    r, c = stack.pop()
                    comp.append((r, c))
                    for rr, cc in ((r + 1, c), (r - 1, c), (r, c + 1), (r, c - 1)):
                        if 0 <= rr < n and 0 <= cc < n and walk[rr][cc] and not seen[rr][cc]:
                            seen[rr][cc] = True
                            stack.append((rr, cc))
                comps.append(comp)
    comps.sort(key=len, reverse=True)
    if a.islands == 'largest':
        drop = comps[1:]
    else:
        drop = [comp for comp in comps if len(comp) < 400
                and not any(r in (0, n - 1) or c in (0, n - 1) for r, c in comp)]
    islands = sum(len(comp) for comp in drop)
    for comp in drop:
        for r, c in comp:
            walk[r][c] = False
            walk_ring[(r, c)] = False

    with open(out / 'tiles.bin', 'wb') as f:
        for r in range(n):
            for c in range(n):
                f.write(struct.pack('<Bf', 1 if walk[r][c] else 0, floor[r][c]))

    # The upper storey (see STOREY_CLEAR): per ring tile the lowest polygon a storey may stand on,
    # moved onto the model face WoW draws there (within 2 yd, the same as the ground's floors) and
    # held to the slope rule; then only what a walker reaches from the ground floor.
    storey, levels = {}, []
    if a.storeys:
        limit = a.slope_limit if a.slope_limit > 0 else 90.0
        cand = {}       # ring tile -> the heights a storey may stand at there (model faces)
        for rc in ring:
            if surfaces is None:
                break
            hs = poly_hs[rc]
            low = floor_yd[rc] + STOREY_CLEAR if walk_ring[rc] else terr[rc] + 4.0
            up = [h for h in hs if h > low] if walk_ring[rc] else [h for h in hs if h >= low]
            # A cellar under the ground counts too (Stormwind's sanctum, under the mage quarter), and so
            # does the way down to it: every polygon a walker's headroom under the tile's floor, or under
            # what the ground layer can take where it has none. (Six yards here left a band of stairs in
            # no layer, and the Lion's Pride Inn's cellar, 6.5 yd under its hall, out of reach.)
            under = floor_yd[rc] - STOREY_CLEAR if walk_ring[rc] else terr[rc] - 4.0
            up += [h for h in hs if h < under]
            x, y = col[rc]
            faces = [fc for fc in surfaces.floors(x, y) if fc[1] <= limit]
            # Around the middle too: a middle on the line between two triangles finds neither, and a
            # diagonal of such tiles was a crack across a street.
            if up and not any(any(abs(fc[0] - h) <= 2.0 for h in up) for fc in faces):
                for ox, oy in ((0.15, 0.05), (-0.15, -0.05), (0.05, -0.15), (-0.05, 0.15)):
                    faces += [fc for fc in surfaces.floors(x + ox, y + oy) if fc[1] <= limit]
            # A storey stands on a model (a building's floor, its stairs, a bridge): a polygon with no
            # model face under it is the movement map's coarse fit of a hillside straying up.
            # (every polygon, not only the lowest: Orgrimmar's streets run over its tunnels; the walk
            # below takes the height that goes on from its neighbour.)
            opts = {round(z, 3) for z, t in faces if any(abs(z - h) <= 2.0 for h in up)}
            opts |= {round(z, 3) for z, t in faces if t >= STOREY_RAMP_DEG and z >= low}
            # The same for a stair going down, only with a polygon near to say someone walks there.
            opts |= {round(z, 3) for z, t in faces if t >= STOREY_RAMP_DEG and z < under
                     and any(abs(z - h) <= 2.0 for h in hs)}
            if opts:
                cand[rc] = sorted(opts)
        def nbrs(rc):
            r, c = rc
            return ((r + 1, c), (r - 1, c), (r, c + 1), (r, c - 1))
        def joins(f1, f2):
            return abs(f1 - f2) * YD < STOREY_STEP_M
        ground = {rc: floor_yd[rc] for rc in ring if walk_ring[rc]}
        opts_all = dict(cand)
        def step_to(rc, h):
            # The height at rc a walker coming from height h stands on, or None.
            best = min(cand[rc], key=lambda z: abs(z - h))
            return best if joins(best, h) else None
        def reach(start=(), keep=None):
            # What a walk from below reached stays: the seeds only add (a roof must not take a door's
            # tile from the stair that leads to it).
            got, stack = dict(keep or {}), []
            for rc, z in start:
                got[rc] = z
                stack.append(rc)
            for rc in cand:
                if rc in got:
                    continue
                for q in nbrs(rc):
                    if q in ground and (z := step_to(rc, ground[q])) is not None:
                        got[rc] = z
                        stack.append(rc)
                        break
            # Breadth first, so each tile takes the height of the nearest way up.
            i = 0
            while i < len(stack):
                rc = stack[i]
                i += 1
                for q in nbrs(rc):
                    if q in cand and q not in got and (z := step_to(q, got[rc])) is not None:
                        got[q] = z
                        stack.append(q)
            return got
        main = []
        def reach_main(seen):
            # Still unreached: a floor whose way up lies farther off (a city's level over buried
            # terrain, Orgrimmar; rooms off Stormwind's streets). It stands where the movement map joins
            # its polygon to the main part of the world.
            if not any(rc not in seen and rc in col for rc in cand):
                return seen
            if not main:
                main.append(under_set if under_set is not None else main_polys(polys))
            start = []
            for rc in cand:
                if rc not in seen and rc in col:
                    x, y = col[rc]
                    hs = [poly_height(x, y, pts) for pts in buckets.get((int(math.floor(x / CELL)), int(math.floor(y / CELL))), ())
                          if id(pts) in main[0] and inside(x, y, pts)]
                    zs = [z for z in cand[rc] if any(abs(z - h) <= 2.0 for h in hs)]
                    if zs:
                        start.append((rc, zs[0]))
            return reach(start, seen) if start else seen
        seen = reach()
        if any(rc not in seen for rc in cand):
            # A storey unreached at the square's edge: its stairs may lie in a neighbour. The margin's
            # tiles from the movement map alone (ground: the polygon nearest the terrain within 4 yd;
            # storey: as above, on the polygons' own heights), then the walk again over both.
            m = STOREY_MARGIN
            mreach = (half + m * TILE) / YD + 3
            mbuckets = {}
            for pts in polys:
                x0, x1 = min(q[0] for q in pts), max(q[0] for q in pts)
                y0, y1 = min(q[1] for q in pts), max(q[1] for q in pts)
                if pts.water or x0 > wx + mreach or x1 < wx - mreach or y0 > wy + mreach or y1 < wy - mreach:
                    continue
                for i in range(int(math.floor(x0 / CELL)), int(math.floor(x1 / CELL)) + 1):
                    for j in range(int(math.floor(y0 / CELL)), int(math.floor(y1 / CELL)) + 1):
                        mbuckets.setdefault((i, j), []).append(pts)
            for r in range(-1 - m, n + 1 + m):
                for c in range(-1 - m, n + 1 + m):
                    if -1 <= r <= n and -1 <= c <= n:
                        continue
                    x, y = to_wow(-half + (c + 0.5) * TILE, -half + (r + 0.5) * TILE)
                    try:
                        th = terrain.height(x, y)
                    except (OSError, struct.error):
                        continue
                    hs = [poly_height(x, y, pts) for pts in mbuckets.get((int(math.floor(x / CELL)), int(math.floor(y / CELL))), ())
                          if inside(x, y, pts)]
                    low = [h for h in hs if abs(h - th) < 4.0]
                    f0 = min(low, key=lambda h: abs(h - th)) if low else None
                    up = [h for h in hs if (h - f0 > STOREY_CLEAR if f0 is not None else h - th >= 4.0)]
                    if f0 is not None:
                        ground[(r, c)] = f0
                    if up:
                        cand[(r, c)] = sorted(up)
            opts_all = dict(cand)
            seen = reach()
        seen = reach_main(seen)
        # Level by level (STOREY_OFFSETS): each one stands on the level below as the first stands on the
        # ground, and takes the floors over it that the level below left.
        levels, taken = [], {}
        below = {rc: floor_yd[rc] for rc in ring if walk_ring[rc]}
        for level in range(min(a.storey_levels, len(STOREY_OFFSETS))):
            if level:
                cand = {}
                for rc, zs in opts_all.items():
                    # Over or under what the lower levels took: the walk reaches a deck before the
                    # street beneath it as often as the other way round.
                    left = [z for z in zs if all(abs(z - t) > STOREY_CLEAR for t in taken.get(rc, ()))]
                    if left:
                        cand[rc] = left
                ground = below
                seen = reach()
                seen = reach_main(seen)
            # Where a level meets the one below (a walker steps from one onto the other): both tiles
            # are seams, the only tiles BG3's pathfinder changes subgrids on (flags 4 SubgridEdge and 8
            # SubgridIntersection in the plugin's Place). It joins at the top of each way up only (the
            # highest of the joins that touch): below that it would repeat the lower stair,
            # and a point on the stair takes the higher twin, so those tiles, the ones by a lower floor
            # of their height, go.
            where, top_of = {}, {}
            for rc, z in seen.items():
                for q in nbrs(rc):
                    if q in below and joins(below[q], z):
                        where[q] = below[q]
            for q0 in where:
                if q0 not in top_of:
                    group, t = [q0], where[q0]
                    top_of[q0] = None
                    for q in group:
                        t = max(t, where[q])
                        for i in range(-2, 3):
                            for j in range(-2, 3):
                                m = (q[0] + i, q[1] + j)
                                if m in where and m not in top_of:
                                    top_of[m] = None
                                    group.append(m)
                    for q in group:
                        top_of[q] = t
            def top_near(rc):
                # The top of the way up beside this tile (its joins that touch), or None.
                return max((top_of[(rc[0] + i, rc[1] + j)] for i in range(-3, 4) for j in range(-3, 4)
                            if (rc[0] + i, rc[1] + j) in top_of), default=None)
            def by_below(rc, z):
                return any((rc[0] + i, rc[1] + j) in below and abs(below[(rc[0] + i, rc[1] + j)] - z) * YD < STOREY_STEP_M
                           for i in range(-2, 3) for j in range(-2, 3))
            # (Between upper levels a stair may pass back and forth, so every join stays.)
            for rc in list(seen) if not level else ():
                t = top_near(rc)
                if t is not None and seen[rc] < t - STOREY_JOIN_M / YD and by_below(rc, seen[rc]):
                    del seen[rc]
            seam_up, seam_down = set(), {}
            for rc, z in list(seen.items()):
                for q in nbrs(rc):
                    if q in below and joins(below[q], z) and (level or below[q] >= top_near(q) - STOREY_JOIN_M / YD):
                        seam_up.add(rc)
                        seam_down.setdefault(q, None)
                        # BG3 changes subgrids where two overlap only on a tile both have, each way:
                        # the lower tile goes into this level at its height and this level's into the
                        # lower one (its section only, not tiles.bin) at this one's, seams in both.
                        if q not in seen:
                            seen[q] = below[q]
                            seam_up.add(q)
                        if rc not in below:
                            seam_down[rc] = by + (z - wz) * YD
            dc, dr = STOREY_OFFSETS[level]
            inside_ = {rc: by + (z - wz) * YD for rc, z in seen.items() if dr <= rc[0] < n + dr and dc <= rc[1] < n + dc}
            if level and len(inside_) < STOREY_MIN_LEVEL:
                break
            levels.append({'seen': seen, 'floors': inside_, 'seam_up': seam_up, 'seam_down': seam_down, 'off': (dc, dr)})
            below = dict(seen)
            for rc, z in seen.items():
                taken.setdefault(rc, []).append(z)
        # The sections as the plugin takes them, each on its base (arena.json storey_levels). BG3 takes
        # a point's tile from the subgrid it last looked in, whatever the height, unless that tile has
        # the Intersection or the Edge bit: then the tile holding the height wins (at the nearest, a
        # blocked tile counts too, so the tiles without a floor lie far overhead). So every tile inside
        # the square (the perimeter has Edge) has Intersection in each level and under them in the
        # ground: flag 8 without 4, which makes no crossing.
        def seams_file(name, off, seam, over):
            with open(out / name, 'wb') as f:
                for r in range(n):
                    for c in range(n):
                        rc = (r + off[1], c + off[0])
                        h = seam.get(rc)
                        fl = 12 if h is None else 13
                        if rc not in seam:
                            fl = 8 if rc in over and 0 < r < n - 1 and 0 < c < n - 1 else 0
                        f.write(struct.pack('<Bf', fl, math.nan if h is None else h))
        for i, lv in enumerate(levels):
            # A level's floors, and what the level above copies into it, stand on its base.
            incoming = list(levels[i + 1]['seam_down'].values()) if i + 1 < len(levels) else []
            low = list(lv['floors'].values()) + [h for h in incoming if h is not None]
            lv['base'] = math.floor(min(low) * 50) / 50 if low else None
            dc, dr = lv['off']
            with open(out / ('storey.bin' if i == 0 else 'storey%d.bin' % (i + 1)), 'wb') as f:
                for r in range(n):
                    for c in range(n):
                        rc = (r + dr, c + dc)
                        z = lv['floors'].get(rc)
                        inner = 8 if 0 < r < n - 1 and 0 < c < n - 1 else 0
                        if z is not None:
                            f.write(struct.pack('<Bf', 1 | (12 if rc in lv['seam_up'] else inner), z))
                        else:
                            f.write(struct.pack('<Bf', inner, lv['base'] + STOREY_FILL_M if lv['base'] is not None else math.nan))
            if i + 1 < len(levels):
                seams_file('seams%d.bin' % (i + 1), lv['off'], levels[i + 1]['seam_down'], ())
        over = set()
        for lv in levels:
            over |= set(lv['floors'])
        seams_file('seams.bin', (0, 0), levels[0]['seam_down'] if levels else {}, over)
        storey = levels[0]['floors'] if levels else {}
        storey_base = levels[0]['base'] if levels else None
    # spans.bin: each tile's ground height range (lowest, highest in BG3 metres) over its corners and
    # middle: the terrain's on terrain floors, the floor alone elsewhere (models, water, polygons).
    corner_h = {}
    def corner_height(r, c):
        if (r, c) not in corner_h:
            corner_h[(r, c)] = terrain.height(*to_wow(-half + c * TILE, -half + r * TILE))
        return corner_h[(r, c)]
    with open(out / 'slopes.bin', 'wb') as f:
        for r in range(n):
            for c in range(n):
                t = tilt_ring[(r, c)]
                f.write(bytes([255 if t is None else min(90, int(round(t)))]))
    with open(out / 'spans.bin', 'wb') as f:
        for r in range(n):
            for c in range(n):
                lo = hi = floor[r][c]
                if walk[r][c] and (r, c) in on_terrain:
                    hs = [corner_height(r + dr, c + dc) for dr in (0, 1) for dc in (0, 1)] + [terr[(r, c)]]
                    lo, hi = by + (min(hs) - wz) * YD, by + (max(hs) - wz) * YD
                f.write(struct.pack('<ff', lo, hi))

    def bg3_tri(t, up=False):
        p0, p1, p2 = to_bg3(*t[0:3]), to_bg3(*t[3:6]), to_bg3(*t[6:9])
        if up:   # the face's normal up in BG3
            ux, uz, vx, vz = p1[0] - p0[0], p1[2] - p0[2], p2[0] - p0[0], p2[2] - p0[2]
            if uz * vx - ux * vz < 0:
                p1, p2 = p2, p1
        elif a.mirror:   # the mirrored mapping turns faces inside out: keep WoW's winding
            p1, p2 = p2, p1
        return p0 + p1 + p2

    # WoW's terrain mesh, cut to the square plus the margin (the neighbours' cuts meet it exactly).
    tris = []
    reach_yd = (half + a.margin) / YD
    box = (wx - reach_yd, wx + reach_yd, wy - reach_yd, wy + reach_yd)
    for t in terrain.triangles(*box):
        for piece in wow_vmap_clip(t, *box):
            tris.append(bg3_tri(piece, up=True))
    mesh_tris = len(tris)
    corner = lambda r, c: (bx - half + c * TILE, bz - half + r * TILE)  # noqa: E731
    # Walkable floors on no WoW surface we have (a movement-map polygon with nothing drawn near it):
    # two flat triangles at the tile's floor, so physics holds characters there too.
    for r, c in sorted(off_surface):
        if 0 <= r < n and 0 <= c < n and walk[r][c]:
            x0, z0 = corner(r, c)
            y = floor[r][c]
            tris.append((x0, y, z0, x0, y, z0 + TILE, x0 + TILE, y, z0 + TILE))
            tris.append((x0, y, z0, x0 + TILE, y, z0 + TILE, x0 + TILE, y, z0))
    terrain_tris = len(tris)

    model, covered = [bg3_tri(wt[k:k + 9]) for k in range(0, len(wt), 9)], set()
    if a.models and a.walls == 'auto':
        # Tiles with model surface at wall height (floor - 1 m to floor + 4.5 m), sampled about
        # every 0.7 m on the part of each triangle in that height band, then grown by 3 tiles
        # (1.5 m: the navmesh's blocked area reaches past the real obstacle by the agent radius
        # and a tile).
        import wow_vmap
        band_lo, band_hi = min(floor_ring.values()) - 1.0, max(floor_ring.values()) + 4.5
        ox, oz = bx - half - 32 * TILE, bz - half - 32 * TILE   # keeps int() a floor down to ring -1
        touched = set()
        for t in model:
            ylo, yhi = min(t[1], t[4], t[7]), max(t[1], t[4], t[7])
            if yhi < band_lo or ylo > band_hi:
                continue
            for q in ((t,) if band_lo <= ylo and yhi <= band_hi else wow_vmap.clip_tri_axis(t, 1, band_lo, band_hi)):
                e = max(math.dist(q[0:3], q[3:6]), math.dist(q[3:6], q[6:9]), math.dist(q[6:9], q[0:3]))
                steps = int(e / 0.7) + 1
                for i in range(steps + 1):
                    u = i / steps
                    for j in range(steps + 1 - i):
                        v = j / steps
                        py = q[1] + u * (q[4] - q[1]) + v * (q[7] - q[1])
                        rc = (int((q[2] + u * (q[5] - q[2]) + v * (q[8] - q[2]) - oz) / TILE) - 32,
                              int((q[0] + u * (q[3] - q[0]) + v * (q[6] - q[0]) - ox) / TILE) - 32)
                        f = floor_ring.get(rc)
                        if f is not None and f - 1.0 <= py <= f + 4.5:
                            touched.add(rc)
        covered = {(r + dr, c + dc) for r, c in touched for dr in range(-3, 4) for dc in range(-3, 4)}
    keep_walls = a.walls if a.walls != 'auto' else ('uncovered' if a.models else 'all')

    walls = walls_dropped = 0
    for r in range(n):          # 4 m walls where walkable meets blocked
        for c in range(n):
            if walk[r][c]:
                continue
            x0, z0 = corner(r, c)
            y0 = floor[r][c] - 0.5
            y1 = y0 + 4.5
            for (dr, dc, ax, az, bx2, bz2) in ((0, -1, x0, z0, x0, z0 + TILE), (0, 1, x0 + TILE, z0, x0 + TILE, z0 + TILE),
                                                (-1, 0, x0, z0, x0 + TILE, z0), (1, 0, x0, z0 + TILE, x0 + TILE, z0 + TILE)):
                rr, cc = r + dr, c + dc
                if walk_ring.get((rr, cc), False):
                    if keep_walls == 'none' or keep_walls == 'uncovered' and ((r, c) in covered or (rr, cc) in covered):
                        walls_dropped += 1
                        continue
                    tris.append((ax, y0, az, bx2, y0, bz2, bx2, y1, bz2))
                    tris.append((ax, y0, az, bx2, y1, bz2, ax, y1, az))
                    walls += 1
    wall_tris = len(tris) - terrain_tris

    # Over the budget the smallest objects go first (then, within one object, the smallest triangles).
    budget = max(0, a.max_triangles - len(tris))
    model_dropped = 0
    if len(model) > budget:
        def area(t):
            ux, uy, uz = t[3] - t[0], t[4] - t[1], t[5] - t[2]
            vx, vy, vz = t[6] - t[0], t[7] - t[1], t[8] - t[2]
            return (uy * vz - uz * vy) ** 2 + (uz * vx - ux * vz) ** 2 + (ux * vy - uy * vx) ** 2
        keep = sorted(range(len(model)), key=lambda i: (abs(sizes[i]), area(model[i])), reverse=True)[:budget]
        model_dropped = len(model) - budget
        model = [model[i] for i in sorted(keep)]
        sizes = [sizes[i] for i in sorted(keep)]
        thick = [thick[i] for i in sorted(keep)]
    tris.extend(model)
    with open(out / 'triangles.bin', 'wb') as f:
        for t in tris:
            f.write(struct.pack('<9f', *t))
    # kinds.bin: one byte per triangle: 0 terrain mesh, 4 flat floor where WoW draws nothing (water
    # surface, hole), 1 wall, 2 WMO, 3 M2, 5 big M2. The design rule for what stands between the camera
    # and her: rocks and big trees the camera bumps into, bushes, skinny trees and posts fade out whole.
    # The line is drawn on WoW's collision, the smallest side of the model in its own axes.
    kinds = (bytes([0]) * mesh_tris + bytes([4]) * (terrain_tris - mesh_tris) + bytes([1]) * wall_tris
             + bytes((5 if t >= BIG_M2_YD else 3) if s < 0 else 2 for s, t in zip(sizes, thick)))
    assert len(kinds) == len(tris)
    (out / 'kinds.bin').write_bytes(kinds)

    blocked = sum(1 for r in range(n) for c in range(n) if not walk[r][c])
    meta = {'wow_centre': [wx, wy, wz], 'bg3_centre': [bx, by, bz], 'size_m': n * TILE, 'tiles_per_side': n,
            'tile_m': TILE, 'min_corner_bg3': [bx - half, bz - half], 'blocked_tiles': blocked,
            'island_tiles_blocked': islands, 'triangles': len(tris), 'wall_quads': walls,'detour_polys_near': len(near),
            'terrain_triangles': terrain_tris, 'wall_triangles': wall_tris, 'model_triangles': len(model),
            'model_m2_triangles': sum(1 for s in sizes if s < 0), 'model_triangles_dropped': model_dropped,
            'model_big_m2_triangles': sum(1 for s, t in zip(sizes, thick) if s < 0 and t >= BIG_M2_YD),
            'wall_quads_dropped': walls_dropped, 'models': a.models or 'off', 'walls': keep_walls,
            'walkable_floors_on': floor_kind,
            'slope_limit_deg': a.slope_limit, 'steep_polys': a.steep_polys,
            'slope_blocked_tiles': steep_cut,
            'storeys': a.storeys, 'storey_tiles': len(storey), 'storey_base_m': storey_base if a.storeys else None,
            **({'storey_levels': [{'tiles': len(lv['floors']), 'base': lv['base'], 'off': lv['off']} for lv in levels]} if a.storeys else {}),
            'storey_range_m': [min(storey.values()), max(storey.values())] if storey else None,
            'walkable_slope_deg_max': max([tilt_ring[(r, c)] for r in range(n) for c in range(n)
                                           if walk[r][c] and tilt_ring[(r, c)] is not None] or [0.0]),
            'floor_range_m': [min(min(row) for row in floor), max(max(row) for row in floor)],
            'mirror': bool(a.mirror),
            'mapping': 'WoW delta (x,y,z) yd -> BG3 (%sy, z, -x) * 0.9144 m + centre' % ('' if a.mirror else '-')}
    (out / 'arena.json').write_text(json.dumps(meta, indent=1))
    # Full-resolution text preview: '.' walkable, 's' blocked by the slope rule (too steep for a WoW
    # player), '#' blocked otherwise; top row is BG3 max Z, left is min X.
    def mark(r, c):
        if walk[r][c]:
            return '.'
        t = tilt_ring[(r, c)]
        return 's' if a.slope_limit > 0 and t is not None and t > a.slope_limit else '#'
    (out / 'preview.txt').write_text('\n'.join(''.join(mark(r, c) for c in range(n))
                                               for r in range(n - 1, -1, -1)) + '\n')
    print(json.dumps(meta))

if __name__ == '__main__':
    main()
