#!/usr/bin/env python3
"""wow_arena.main_polys: which movement-map polygons count as joined to the walkable world.

Two Detour tiles written here: the mainland in one, and in the other a town with a pier, which meets the mainland
along the tiles' border only. The two tiles' copies of the border corners differ by hundredths of a yard in height,
as the generator leaves them (Auberdine: 13.51 and 13.49).
"""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wow_arena  # noqa: E402

OUTER = 0x8000


def write_tile(path, polys):
    """polys: [([(x, y, z) WoW corners], [neighbour per edge: 0, index + 1, or OUTER])]."""
    verts, recs = [], []
    for pts, nei in polys:
        idx = []
        for x, y, z in pts:
            idx.append(len(verts))
            verts.append((y, z, x))            # Recast space (x, y, z) = WoW (y, z, x)
        pad = 6 - len(pts)
        recs.append(struct.pack('<I6H6HHBB', 0, *(idx + [0] * pad), *(list(nei) + [0] * pad), 1, len(pts), 0))
    hdr = [0x444E4156, 7, 0, 0, 0, 0, len(polys), len(verts)] + [0] * 7
    raw = struct.pack('<IIIIB3x', 0, 0, 0, 0, 0) + struct.pack('<15i3f3f3ff', *hdr, *([0.0] * 10))
    raw += b''.join(struct.pack('<3f', *v) for v in verts) + b''.join(recs)
    Path(path).write_bytes(raw)


def quad(x0, y0, x1, y1, z, nei, zs=None):
    zs = zs or [z] * 4
    return [(x0, y0, zs[0]), (x1, y0, zs[1]), (x1, y1, zs[2]), (x0, y1, zs[3])], nei


class MainPolys(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        # The mainland: five squares in a row along x, the last one's far edge on the border x = 50.
        land = [quad(10 * i, 0, 10 * i + 10, 10, 13.5, [0, i + 2 if i < 4 else OUTER, 0, i if i else 0],
                     [13.5, 13.51, 13.51, 13.5] if i == 4 else None) for i in range(5)]
        # The town over the border, then its pier: three squares, the first one's near edge on the border,
        # its corners there a hair lower than the mainland's copy of them.
        town = [quad(50 + 10 * i, 0, 60 + 10 * i, 10, 13.5, [0, i + 2 if i < 2 else 0, 0, i if i else OUTER],
                     [13.49, 13.5, 13.5, 13.49] if i == 0 else None) for i in range(3)]
        # And a rock off the coast that nothing joins.
        town.append(quad(200, 200, 210, 210, 3.0, [0, 0, 0, 0]))
        write_tile(d / 'land.mmtile', land)
        write_tile(d / 'town.mmtile', town)
        self.polys = wow_arena.detour_polys(d / 'land.mmtile') + wow_arena.detour_polys(d / 'town.mmtile')

    def tearDown(self):
        self.tmp.cleanup()

    def test_a_pier_over_the_tile_border_is_of_the_main_set(self):
        main = wow_arena.main_polys(self.polys)
        self.assertEqual(len(main), 8)          # the mainland's five and the town's three; not the rock
        pier = self.polys[7]
        self.assertEqual(pier[0][0], 70)
        self.assertIn(id(pier), main)
        self.assertNotIn(id(self.polys[8]), main)

    def test_the_set_of_a_point(self):
        self.assertEqual(wow_arena.main_polys(self.polys, (75.0, 5.0, 13.5)), wow_arena.main_polys(self.polys))
        rock = wow_arena.main_polys(self.polys, (205.0, 205.0, 3.0))
        self.assertEqual(rock, {id(self.polys[8])})
        self.assertEqual(wow_arena.main_polys(self.polys, (500.0, 500.0, 0.0)), set())


if __name__ == '__main__':
    unittest.main()
