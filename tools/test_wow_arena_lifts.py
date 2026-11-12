#!/usr/bin/env python3
"""test_wow_arena_lifts.py: a lift joins the floors at the two ends of its shaft (wow_arena.py lift_polys), and the
bridge names a square's lifts to its cut (world_bridge.py lift_args), without a game or WoW's data.

  python3 tools/test_wow_arena_lifts.py
"""
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wow_arena as A  # noqa: E402
import world_bridge as B  # noqa: E402
from test_wow_arena_main import quad as tile_quad, write_tile  # noqa: E402


class LiftPolys(unittest.TestCase):
    def setUp(self):
        # A city's floor at z -43 (two polygons sharing an edge), the room over it at z 55, a far yard at z 60: read
        # from a movement tile, as the cut reads them, and joined by the tile's own links (wow_arena.poly_sets).
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        f = Path(d.name) / 'city.mmtile'
        write_tile(f, [tile_quad(0, 0, 10, 10, -43.0, [0, 2, 0, 0]), tile_quad(10, 0, 20, 10, -43.0, [0, 0, 0, 1]),
                       tile_quad(12, 0, 20, 10, 55.0, [0, 0, 0, 0]), tile_quad(200, 200, 210, 210, 60.0, [0, 0, 0, 0])])
        self.polys = A.detour_polys(f)
        self.low, self.top, self.far = self.polys[:2], self.polys[2:3], self.polys[3:]

    def ids(self, polys):
        return {id(p) for p in polys}

    def test_no_lift_joins_nothing(self):
        self.assertEqual(A.lift_polys(self.polys, None), set())
        self.assertEqual(A.lift_polys(self.polys, []), set())

    def test_the_sets_at_both_stops(self):
        # The car rests beside the city's floor and beside the room: x 25, radius 5 yd.
        got = A.lift_polys(self.polys, [(25.0, 5.0, -43.2, 5.0), (25.0, 5.0, 55.3, 5.0)])
        self.assertEqual(got, self.ids(self.low + self.top))      # both polygons of the city's floor: one set

    def test_a_floor_at_another_height_is_not_at_the_stop(self):
        got = A.lift_polys(self.polys, [(25.0, 5.0, 10.0, 5.0)])
        self.assertEqual(got, set())

    def test_a_floor_out_of_the_cars_reach_is_not(self):
        got = A.lift_polys(self.polys, [(60.0, 5.0, -43.0, 5.0)])
        self.assertEqual(got, set())

    def test_the_origins_set_and_the_lifts(self):
        # A session that came from below (--under): its own floors, and what its lift reaches.
        under = A.main_polys(self.polys, (5.0, 5.0, -43.0)) | A.lift_polys(self.polys, [(25.0, 5.0, 55.3, 5.0)])
        self.assertEqual(under, self.ids(self.low + self.top))


class LiftArgs(unittest.TestCase):
    def test_a_squares_lifts_and_no_others(self):
        stops = {0: [(1544.2, 240.8, -41.26, 8.4), (1544.2, 240.8, 55.24, 8.4), (-5164.2, 650.4, 349.0, 9.3)], 1: [(1.0, 2.0, 3.0, 5.0)]}
        with mock.patch.object(B, 'LIFT_STOPS', stops):
            got = B.lift_args(0, 1560.0, 250.0)
            self.assertEqual(got, ['--lift', '1544.20', '240.80', '-41.26', '8.40', '--lift', '1544.20', '240.80', '55.24', '8.40'])
            self.assertEqual(B.lift_args(0, 1700.0, 250.0), [])       # a square away
            self.assertEqual(B.lift_args(36, 1560.0, 250.0), [])      # another map

    def test_the_cut_names_them(self):
        class Flat:
            def height(self, x, y):
                return 61.7
        with mock.patch.object(B, 'LIFT_STOPS', {0: [(10.0, 5.0, -41.0, 8.0)]}):
            args = B.cut_args(Flat(), 0, B.centre_of(0), B.FIXED_ORIGIN, (0, 0), None)
        self.assertEqual(args[-5:], ['--lift', '10.00', '5.00', '-41.00', '8.00'])


if __name__ == '__main__':
    unittest.main()
