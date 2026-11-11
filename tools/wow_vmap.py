#!/usr/bin/env python3
"""WoW's collision models (the server's vmaps) as world triangles, for wow_arena.py --models.

wow_vmap.py X Y [--size YARDS] [--no-m2]     cut a square of model triangles round a WoW point, print
                                             counts and timings (cold and warm)
wow_vmap.py --check TILE_Y TILE_X            parse one ADT tile's spawns and models; compare each
                                             placed model with the bounds the server stored for it

Format: vmangos VMAP_7.0 (vmangos/core src/game/vmap: MapTree, ModelInstance, WorldModel, BIH),
checked against the test server's extracted data. Little endian throughout.
  vmap space: x = 32 * 533.33333 - WoW x, y = 32 * 533.33333 - WoW y, z = WoW z (yards).
  MMM.vmtree        'VMAP_7.0', uint8 tiled, 'NODE' + BIH over all spawns, 'GOBJ' (+ spawns when
                    not tiled). Map 0 is tiled: its spawns live in the tiles. A map of buildings
                    only (most dungeons, the tram) is not: its one building's spawn is read from here.
  MMM_YY_XX.vmtile  one per ADT tile, YY = int(32 - WoW y / 533.33), XX = int(32 - WoW x / 533.33)
                    (Northshire Abbey: 000_32_48). 'VMAP_7.0', uint32 n, n x {ModelSpawn, uint32
                    tree node}. A spawn whose bounds reach into several tiles is listed in each.
  ModelSpawn        uint32 flags (1 M2, 2 worldspawn, 4 has bounds, 8 does not break LoS),
                    uint16 adtId, uint32 id, float pos[3], float rot[3] (degrees), float scale,
                    float lo[3], hi[3] (vmap space, when flags & 4: always set in final tiles),
                    uint32 len, char name[len] (WMO doodads' names end in NULs).
  model file        name + '.vmo' (Nsabbey.wmo.vmo, Elwynnpine01.m2.vmo); a name without one has
                    no collision (the extractor writes no file; vmangos skips the spawn).
                    'VMAP_7.0', 'WMOD' uint32 8 uint32 rootWmoId, then if 'GMOD': uint32 groups,
                    per group: float lo[3], hi[3] (model space), uint32 mogpFlags, uint32 groupId,
                      'VERT' uint32 size, uint32 n, float v[n][3]   (n == 0: the group ends here)
                      'TRIM' uint32 size, uint32 m, uint32 idx[m][3]
                      'MBIH' BIH, 'LIQU' uint32 size [uint32 tx, ty, float corner[3], uint32 type,
                             float h[(tx+1)(ty+1)], uint8 flags[tx*ty]],
                    then 'GBIH' BIH. BIH: float lo[3], hi[3], uint32 n, uint32 tree[n], uint32 k,
                    uint32 objects[k]. M2s are stored the same way, as one group.
                    Only collision geometry is in these files; z is up in model space too.
  placement         vmap point = R * (v * scale) + pos, R = Rz(rot[1]) * Ry(rot[0]) * Rx(rot[2])
                    (G3D fromEulerAnglesZYX(rot.y, rot.x, rot.z), the inverse of ModelInstance's
                    iInvRot).
vmangos's own LoS skips M2s by default (Map::isInLineOfSight ignoreM2Model = true): trees, fences
and doodads block nothing in WoW's spell checks; WMOs (buildings, bridges, caves) do.

Cache: model triangles are cut per cell (1/8 of an ADT tile side, 66.7 yards), clipped to the cell,
in WoW world coordinates, and kept in memory and on disk (tmp/vmapcache/: Blizzard-derived, never
in git; WOW_VMAP_CACHE overrides the folder). A cell file records the size and mtime of the .vmtile it
was cut from and is cut again when they change. Plain Python, no numpy: the bridge runs this with
/usr/bin/python3.
"""
import argparse, math, os, struct, sys, time
from array import array
from pathlib import Path

GRID = 533.33333
MID = 32 * GRID            # vmap space x, y = MID - WoW x, MID - WoW y
SPLIT = 8                  # cache cells per ADT tile side
CELL = GRID / SPLIT
MAGIC = b'VMAP_7.0'
CACHE_MAGIC = b'WBVMC003'
M2, HAS_BOUND, NO_LOS = 1, 4, 8
REC = 11                   # floats per cached triangle: 3 corners (WoW x, y, z), the object's size and thickness
VMAPS = (os.environ.get('WOWBG3_TEST_STACK') or '/opt/stacks/wow-bg3') + '/storage/mangosd/extracted-data/vmaps'
CACHE = Path(os.environ.get('WOW_VMAP_CACHE') or Path(__file__).resolve().parents[2] / 'tmp' / 'vmapcache')


class Spawn:
    __slots__ = ('flags', 'id', 'pos', 'rot', 'scale', 'lo', 'hi', 'name', 'node')

    def wow_bounds(self):
        """x0, x1, y0, y1, z0, z1 in WoW yards."""
        lo, hi = self.lo, self.hi
        return MID - hi[0], MID - lo[0], MID - hi[1], MID - lo[1], lo[2], hi[2]


def _spawn(raw, off):
    """One ModelSpawn at `off`: (Spawn, the offset after its name)."""
    s = Spawn()
    s.flags, _adt, s.id, px, py, pz, rx, ry, rz, s.scale = struct.unpack_from('<IHI7f', raw, off)
    off += 38
    s.pos, s.rot = (px, py, pz), (rx, ry, rz)
    if s.flags & HAS_BOUND:
        s.lo, s.hi = struct.unpack_from('<3f', raw, off), struct.unpack_from('<3f', raw, off + 12)
        off += 24
    else:
        s.lo = s.hi = None
    ln, = struct.unpack_from('<I', raw, off)
    s.name = raw[off + 4:off + 4 + ln].split(b'\0', 1)[0].decode('latin-1')
    s.node = 0
    return s, off + 4 + ln


def read_spawns(path):
    raw = path.read_bytes()
    if raw[:8] != MAGIC:
        raise ValueError('%s: not a VMAP_7.0 tile' % path)
    n, = struct.unpack_from('<I', raw, 8)
    off, out = 12, []
    for _ in range(n):
        s, off = _spawn(raw, off)
        s.node, = struct.unpack_from('<I', raw, off)
        off += 4
        out.append(s)
    if off != len(raw):
        raise ValueError('%s: %d bytes left over' % (path, len(raw) - off))
    return out


def read_tree_spawns(path):
    """The spawns a map's .vmtree holds itself: the one building a map without tiles is made of (most dungeons,
    the tram), in the same vmap space as a tile's. [] for a tiled map, whose spawns live in its tiles."""
    raw = path.read_bytes()
    if raw[:8] != MAGIC:
        raise ValueError('%s: not a VMAP_7.0 tree' % path)
    tiled = raw[8]
    off = _skip_bih(raw, _expect(raw, 9, b'NODE'))
    off = _expect(raw, off, b'GOBJ')
    if tiled or off >= len(raw):
        return []
    return [_spawn(raw, off)[0]]   # one spawn, as vmangos reads it (StaticMapTree::InitMap); what follows is not ours


def _expect(raw, off, tag):
    if raw[off:off + len(tag)] != tag:
        raise ValueError('expected %r at %d, found %r' % (tag, off, raw[off:off + len(tag)]))
    return off + len(tag)


def _skip_bih(raw, off):
    n, = struct.unpack_from('<I', raw, off + 24)
    off += 28 + 4 * n
    k, = struct.unpack_from('<I', raw, off)
    return off + 4 + 4 * k


def read_model(path):
    """Groups with triangles: [(lo, hi, verts, idx)]: model-space bounds, flat vertex floats,
    flat vertex indices (3 per triangle)."""
    raw = path.read_bytes()
    off = _expect(raw, _expect(raw, 0, MAGIC), b'WMOD') + 8   # chunk size (8), root WMO id
    groups = []
    if raw[off:off + 4] == b'GMOD':
        count, = struct.unpack_from('<I', raw, off + 4)
        off += 8
        for _ in range(count):
            b = struct.unpack_from('<6f', raw, off)
            off = _expect(raw, off + 32, b'VERT')             # bounds, MOGP flags, group id
            nv, = struct.unpack_from('<I', raw, off + 4)
            off += 8
            if nv == 0:                                       # no geometry: the group ends here
                continue
            verts = struct.unpack_from('<%df' % (3 * nv), raw, off)
            off = _expect(raw, off + 12 * nv, b'TRIM')
            nt, = struct.unpack_from('<I', raw, off + 4)
            off += 8
            idx = struct.unpack_from('<%dI' % (3 * nt), raw, off)
            off = _skip_bih(raw, _expect(raw, off + 12 * nt, b'MBIH'))
            off = _expect(raw, off, b'LIQU')
            size, = struct.unpack_from('<I', raw, off)
            off += 4
            if size:
                tx, ty = struct.unpack_from('<II', raw, off)
                off += 24 + 4 * (tx + 1) * (ty + 1) + tx * ty
            if nt:
                if max(idx) >= nv:
                    raise ValueError('%s: vertex index out of range' % path.name)
                groups.append((b[:3], b[3:], verts, idx))
        off = _skip_bih(raw, _expect(raw, off, b'GBIH'))
    if off != len(raw):
        raise ValueError('%s: %d bytes left over' % (path.name, len(raw) - off))
    return groups


def placement(s):
    """(m, t): WoW world point = t + m * model vertex, m row-major 3 x 3, scale included."""
    yaw, pitch, roll = (math.radians(a) for a in (s.rot[1], s.rot[0], s.rot[2]))
    cy, sy, cp, sp, cr, sr = (math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch),
                              math.cos(roll), math.sin(roll))
    r = ((cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr),
         (sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr),
         (-sp, cp * sr, cp * cr))
    k = s.scale
    m = (-k * r[0][0], -k * r[0][1], -k * r[0][2],      # WoW x = MID - vmap x
         -k * r[1][0], -k * r[1][1], -k * r[1][2],      # WoW y = MID - vmap y
         k * r[2][0], k * r[2][1], k * r[2][2])
    return m, (MID - s.pos[0], MID - s.pos[1], s.pos[2])


def transform(verts, m, t):
    a0, a1, a2, b0, b1, b2, c0, c1, c2 = m
    tx, ty, tz = t
    out = [0.0] * len(verts)
    for i in range(0, len(verts), 3):
        x, y, z = verts[i], verts[i + 1], verts[i + 2]
        out[i] = tx + a0 * x + a1 * y + a2 * z
        out[i + 1] = ty + b0 * x + b1 * y + b2 * z
        out[i + 2] = tz + c0 * x + c1 * y + c2 * z
    return out


def box_bounds(lo, hi, m, t):
    xs, ys = [], []
    for x in (lo[0], hi[0]):
        for y in (lo[1], hi[1]):
            for z in (lo[2], hi[2]):
                xs.append(t[0] + m[0] * x + m[1] * y + m[2] * z)
                ys.append(t[1] + m[3] * x + m[4] * y + m[5] * z)
    return min(xs), max(xs), min(ys), max(ys)


def clip_planes(pts, planes):
    """A convex polygon of (x, y, z) points clipped to half-spaces (axis, limit, keep_above)
    (Sutherland-Hodgman)."""
    for axis, lim, low in planes:
        if len(pts) < 3:
            return []
        out, prev = [], pts[-1]
        pin = prev[axis] >= lim if low else prev[axis] <= lim
        for cur in pts:
            cin = cur[axis] >= lim if low else cur[axis] <= lim
            if cin != pin:
                f = (lim - prev[axis]) / (cur[axis] - prev[axis])
                out.append((prev[0] + f * (cur[0] - prev[0]), prev[1] + f * (cur[1] - prev[1]),
                            prev[2] + f * (cur[2] - prev[2])))
            if cin:
                out.append(cur)
            prev, pin = cur, cin
        pts = out
    return pts if len(pts) >= 3 else []


def clip(pts, x0, x1, y0, y1):
    """A polygon clipped to x0 <= x <= x1, y0 <= y <= y1."""
    return clip_planes(pts, ((0, x0, True), (0, x1, False), (1, y0, True), (1, y1, False)))


def fan(pts):
    """Triangles (9-tuples) of a convex polygon, without slivers of no area."""
    out = []
    a = pts[0]
    for i in range(1, len(pts) - 1):
        b, c = pts[i], pts[i + 1]
        ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
        vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
        nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
        if nx * nx + ny * ny + nz * nz > 1e-10:
            out.append((a[0], a[1], a[2], b[0], b[1], b[2], c[0], c[1], c[2]))
    return out


def clip_tri(t, x0, x1, y0, y1):
    """A triangle (9 floats) clipped to x0 <= x <= x1, y0 <= y <= y1: a list of 9-tuples."""
    pts = clip([(t[0], t[1], t[2]), (t[3], t[4], t[5]), (t[6], t[7], t[8])], x0, x1, y0, y1)
    return fan(pts) if pts else []


def clip_tri_axis(t, axis, lo, hi):
    """A triangle (9 floats) clipped to lo <= coordinate `axis` <= hi: a list of 9-tuples."""
    pts = clip_planes([(t[0], t[1], t[2]), (t[3], t[4], t[5]), (t[6], t[7], t[8])],
                      ((axis, lo, True), (axis, hi, False)))
    return fan(pts) if pts else []


def cell_rect(ci, cj):
    """WoW x0, x1, y0, y1 of cache cell (ci, cj) (vmap x / CELL, vmap y / CELL)."""
    return MID - (ci + 1) * CELL, MID - ci * CELL, MID - (cj + 1) * CELL, MID - cj * CELL


class VMap:
    def __init__(self, vmaps=VMAPS, map_id=0, cache=CACHE):
        self.dir, self.map = Path(vmaps), map_id
        self.cache = Path(cache) / ('%03d' % map_id) if cache else None
        self.models, self.tiles, self.cells = {}, {}, {}
        self.thick = {}
        self.stats = {'cells_cut': 0, 'cells_read': 0, 'models_read': 0}
        tree = self.dir / ('%03d.vmtree' % map_id)
        self.tree = tree
        self.whole = read_tree_spawns(tree) if tree.exists() else []   # a map without tiles: its one building

    def tile_path(self, ty, tx):
        return self.dir / ('%03d_%02d_%02d.vmtile' % (self.map, ty, tx))

    def tile_key(self, ty, tx):
        try:
            st = (self.tree if self.whole else self.tile_path(ty, tx)).stat()
            return st.st_size, st.st_mtime_ns
        except FileNotFoundError:
            return 0, 0

    def spawns(self, ty, tx):
        if (ty, tx) not in self.tiles:
            p = self.tile_path(ty, tx)
            self.tiles[(ty, tx)] = (read_spawns(p) if p.exists() else []) + self.whole
        return self.tiles[(ty, tx)]

    def model(self, name):
        if name not in self.models:
            p = self.dir / (name + '.vmo')
            self.models[name] = read_model(p) if p.exists() else []
            self.stats['models_read'] += 1
        return self.models[name]

    def thickness(self, name):
        """A model's smallest side in its own axes (yards, unscaled): how a piece is turned must not change
        what it is, and a fence at 45 degrees has a world box as wide as it is long."""
        if name not in self.thick:
            lo, hi = [1e9] * 3, [-1e9] * 3
            for _, _, verts, _ in self.model(name):
                for i in range(3):
                    v = verts[i::3]
                    if v:
                        lo[i], hi[i] = min(lo[i], min(v)), max(hi[i], max(v))
            self.thick[name] = max(0.0, min(hi[i] - lo[i] for i in range(3)))
        return self.thick[name]

    # ---- cells ----
    def _file(self, c):
        return self.cache / ('%d_%d.bin' % c)

    def _read(self, c, key):
        try:
            raw = self._file(c).read_bytes()
        except (OSError, TypeError):
            return None
        if len(raw) < 28 or raw[:8] != CACHE_MAGIC or struct.unpack_from('<QQ', raw, 8) != key:
            return None
        n, = struct.unpack_from('<I', raw, 24)
        if len(raw) != 28 + 4 * REC * n:
            return None
        d = array('f')
        d.frombytes(raw[28:])
        if sys.byteorder != 'little':
            d.byteswap()
        return d

    def _write(self, c, key, d):
        if self.cache is None:
            return
        self.cache.mkdir(parents=True, exist_ok=True)
        body = array('f', d)
        if sys.byteorder != 'little':
            body.byteswap()
        tmp = self._file(c).with_suffix('.%d.tmp' % os.getpid())
        tmp.write_bytes(CACHE_MAGIC + struct.pack('<QQI', key[0], key[1], len(d) // REC) + body.tobytes())
        os.replace(tmp, self._file(c))

    def _cut(self, ty, tx, cells):
        """Cut cells of one ADT tile from its spawns: {cell: array of REC floats per triangle}."""
        out = {c: array('f') for c in cells}
        rect = {c: cell_rect(*c) for c in cells}
        ux0, ux1 = min(r[0] for r in rect.values()), max(r[1] for r in rect.values())
        uy0, uy1 = min(r[2] for r in rect.values()), max(r[3] for r in rect.values())
        for s in self.spawns(ty, tx):
            if s.flags & NO_LOS or s.lo is None:
                continue
            bx0, bx1, by0, by1, bz0, bz1 = s.wow_bounds()
            if bx1 < ux0 or bx0 > ux1 or by1 < uy0 or by0 > uy1:
                continue
            hit = [c for c in cells if not (bx1 < rect[c][0] or bx0 > rect[c][1] or by1 < rect[c][2] or by0 > rect[c][3])]
            groups = self.model(s.name) if hit else None
            if not groups:
                continue
            size = max(bx1 - bx0, by1 - by0, bz1 - bz0)
            thick = self.thickness(s.name) * s.scale
            tag = -size if s.flags & M2 else size
            m, t = placement(s)
            for lo, hi, verts, idx in groups:
                gx0, gx1, gy0, gy1 = box_bounds(lo, hi, m, t)
                gcells = [c for c in hit if not (gx1 < rect[c][0] or gx0 > rect[c][1] or gy1 < rect[c][2] or gy0 > rect[c][3])]
                if not gcells:
                    continue
                w = transform(verts, m, t)
                for k in range(0, len(idx), 3):
                    a, b, e = 3 * idx[k], 3 * idx[k + 1], 3 * idx[k + 2]
                    ax, ay, bx, by, ex, ey = w[a], w[a + 1], w[b], w[b + 1], w[e], w[e + 1]
                    x0, x1 = min(ax, bx, ex), max(ax, bx, ex)
                    y0, y1 = min(ay, by, ey), max(ay, by, ey)
                    for c in gcells:
                        cx0, cx1, cy0, cy1 = rect[c]
                        if x1 < cx0 or x0 > cx1 or y1 < cy0 or y0 > cy1:
                            continue
                        tri = (ax, ay, w[a + 2], bx, by, w[b + 2], ex, ey, w[e + 2])
                        if x0 >= cx0 and x1 <= cx1 and y0 >= cy0 and y1 <= cy1:
                            out[c].extend(tri)
                            out[c].append(tag)
                            out[c].append(thick)
                        else:
                            for piece in clip_tri(tri, cx0, cx1, cy0, cy1):
                                out[c].extend(piece)
                                out[c].append(tag)
                                out[c].append(thick)
        return out

    def load(self, cells):
        """Make sure the cells are in memory: from memory, the disk cache, or cut from the vmaps."""
        todo = {}
        for c in cells:
            if c in self.cells:
                continue
            ty, tx = c[1] // SPLIT, c[0] // SPLIT
            key = self.tile_key(ty, tx)
            d = self._read(c, key)
            if d is not None:
                self.cells[c] = d
                self.stats['cells_read'] += 1
            else:
                todo.setdefault((ty, tx, key), []).append(c)
        for (ty, tx, key), group in todo.items():
            for c, d in self._cut(ty, tx, group).items():
                self.cells[c] = d
                self.stats['cells_cut'] += 1
                try:
                    self._write(c, key, d)
                except OSError as e:
                    print('wow_vmap: cache write failed: %s' % e, file=sys.stderr)

    # ---- queries ----
    def triangles(self, x0, x1, y0, y1, z0=-1e9, z1=1e9, m2=True, thickness=None):
        """Collision triangles of every model instance in a WoW box (yards), clipped to its x and y
        sides; z only filters (a triangle wholly above z1 or below z0 is left out).
        Returns (tris, sizes): tris flat, 9 floats per triangle (WoW world x, y, z of 3 corners);
        sizes: per triangle, the largest side of its instance's bounds (yards), negative for M2s.
        m2=False leaves M2 doodads (trees, fences, props) out, like vmangos's LoS.
        thickness: a list to fill per triangle with the smallest side of its instance's model, in the
        model's own axes times its scale (yards), which is how the camera tells a big tree from a post."""
        ci0, ci1 = int(math.floor((MID - x1) / CELL)), int(math.floor((MID - x0) / CELL))
        cj0, cj1 = int(math.floor((MID - y1) / CELL)), int(math.floor((MID - y0) / CELL))
        cells = [(ci, cj) for ci in range(ci0, ci1 + 1) for cj in range(cj0, cj1 + 1)]
        self.load(cells)
        tris, sizes = [], []
        for c in cells:
            d = self.cells[c]
            rx0, rx1, ry0, ry1 = cell_rect(*c)
            inner = rx0 >= x0 and rx1 <= x1 and ry0 >= y0 and ry1 <= y1
            for k in range(0, len(d), REC):
                tag = d[k + 9]
                if tag < 0 and not m2:
                    continue
                az, bz, ez = d[k + 2], d[k + 5], d[k + 8]
                if (az > z1 and bz > z1 and ez > z1) or (az < z0 and bz < z0 and ez < z0):
                    continue
                if not inner:
                    ax, ay, bx, by, ex, ey = d[k], d[k + 1], d[k + 3], d[k + 4], d[k + 6], d[k + 7]
                    tx0, tx1 = min(ax, bx, ex), max(ax, bx, ex)
                    ty0, ty1 = min(ay, by, ey), max(ay, by, ey)
                    if tx1 < x0 or tx0 > x1 or ty1 < y0 or ty0 > y1:
                        continue
                    if not (tx0 >= x0 and tx1 <= x1 and ty0 >= y0 and ty1 <= y1):
                        for piece in clip_tri(d[k:k + 9], x0, x1, y0, y1):
                            tris.extend(piece)
                            sizes.append(tag)
                            if thickness is not None:
                                thickness.append(d[k + 10])
                        continue
                tris.extend(d[k:k + 9])
                sizes.append(tag)
                if thickness is not None:
                    thickness.append(d[k + 10])
        return tris, sizes


def check(vm, ty, tx):
    """Parse a tile's spawns and models; compare placed models with their stored bounds (M2 bounds
    are the placed model's own, so they must match; a WMO's are its looser MODF extents, so the
    placed WMO must lie inside them)."""
    spawns = vm.spawns(ty, tx)
    names = {s.name for s in spawns}
    have = [n for n in names if (vm.dir / (n + '.vmo')).exists()]
    tris = 0
    for n in have:
        tris += sum(len(g[3]) // 3 for g in vm.model(n))
    print('tile %02d_%02d: %d spawns (%d M2, %d WMO, %d no-LoS), %d models, %d with collision files, '
          '%d triangles in those models' % (ty, tx, len(spawns), sum(1 for s in spawns if s.flags & M2),
                                           sum(1 for s in spawns if not s.flags & M2),
                                           sum(1 for s in spawns if s.flags & NO_LOS), len(names), len(have), tris))
    worst = {True: [], False: []}
    placed = 0
    for s in spawns:
        groups = vm.model(s.name)
        if not groups or s.lo is None:
            continue
        m, t = placement(s)
        xs, ys, zs = [], [], []
        for lo, hi, verts, idx in groups:
            w = transform(verts, m, t)
            xs += w[0::3]; ys += w[1::3]; zs += w[2::3]
        placed += sum(len(g[3]) // 3 for g in groups)
        b = s.wow_bounds()
        if s.flags & M2:    # mismatch
            err = max(abs(min(xs) - b[0]), abs(max(xs) - b[1]), abs(min(ys) - b[2]), abs(max(ys) - b[3]),
                      abs(min(zs) - b[4]), abs(max(zs) - b[5]))
        else:               # overshoot
            err = max(0.0, b[0] - min(xs), max(xs) - b[1], b[2] - min(ys), max(ys) - b[3], b[4] - min(zs),
                      max(zs) - b[5])
        worst[bool(s.flags & M2)].append((err, s.name, s.rot))
    for kind, rows in (('M2: placed bounds vs stored', worst[True]), ('WMO: placed outside stored', worst[False])):
        rows.sort()
        if rows:
            print('%s, %d spawns: median %.4f yd, 95%% %.4f, max %.4f (%s rot %s)' % (
                kind, len(rows), rows[len(rows) // 2][0], rows[int(len(rows) * 0.95)][0], rows[-1][0],
                rows[-1][1], tuple(round(v, 1) for v in rows[-1][2])))
    print('placed triangles in the tile\'s spawns: %d' % placed)
    # Spawns of the 8 neighbouring tiles that reach into this tile should be listed here too.
    ids = {s.id for s in spawns}
    x0, x1, y0, y1 = MID - (tx + 1) * GRID, MID - tx * GRID, MID - (ty + 1) * GRID, MID - ty * GRID
    missing = []
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dy or dx:
                for s in vm.spawns(ty + dy, tx + dx):
                    if s.id in ids or s.lo is None:
                        continue
                    b = s.wow_bounds()
                    if b[1] > x0 + 0.01 and b[0] < x1 - 0.01 and b[3] > y0 + 0.01 and b[2] < y1 - 0.01:
                        missing.append((s.name, round(b[1] - b[0], 1)))
    print('neighbour spawns reaching into this tile but not listed in it: %d %s' % (len(missing), missing[:8]))


def main():
    p = argparse.ArgumentParser()
    p.add_argument('x', type=float, nargs='?')
    p.add_argument('y', type=float, nargs='?')
    p.add_argument('--size', type=float, default=70.0, help='square side, yards')
    p.add_argument('--vmaps', default=VMAPS)
    p.add_argument('--map', type=int, default=0)
    p.add_argument('--cache', default=str(CACHE))
    p.add_argument('--no-m2', action='store_true')
    p.add_argument('--check', type=int, nargs=2, metavar=('TILE_Y', 'TILE_X'))
    a = p.parse_args()
    vm = VMap(a.vmaps, a.map, a.cache or None)
    if a.check:
        check(vm, *a.check)
        return
    h = a.size / 2
    for label in ('first', 'again (new process state, disk cache)', 'again (memory)'):
        if label.startswith('again (new'):
            vm = VMap(a.vmaps, a.map, a.cache or None)
        t0 = time.perf_counter()
        tris, sizes = vm.triangles(a.x - h, a.x + h, a.y - h, a.y + h, m2=not a.no_m2)
        dt = time.perf_counter() - t0
        print('%s: %d triangles (%d M2) in %.3f s %s' % (label, len(sizes), sum(1 for s in sizes if s < 0), dt,
                                                        vm.stats))


if __name__ == '__main__':
    main()
