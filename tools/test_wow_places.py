#!/usr/bin/env python3
"""test_wow_places.py: every WoW map has its own place in the one BG3 level, without a game or WoW's data.

  python3 tools/test_wow_places.py
"""
import itertools
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wow_places as P  # noqa: E402

LAYOUTS = ('stacked', 'flat')


def apart(a, b, gap):
    """Two boxes (x0, y0, z0, x1, y1, z1) keep `gap` between them on at least one axis."""
    return any(a[i] - b[i + 3] >= gap or b[i] - a[i + 3] >= gap for i in range(3))


class Places(unittest.TestCase):
    def test_every_map_with_ground_has_a_place(self):
        # The 1.12 maps the server has ground or movement tiles for: 2 continents, 40 instances and test maps.
        for layout in LAYOUTS:
            self.assertEqual(len(P.places(layout)), 42)
            for m in (0, 1, 36, 34, 369, 409, 533):
                self.assertIn(m, P.places(layout))

    def test_an_unknown_map_has_none(self):
        with self.assertRaises(KeyError):
            P.centre(999)

    def test_the_eastern_kingdoms_stay_where_they_are(self):
        # Today's one mapping (world_bridge.py FIXED_CENTRE): ground cut for it and saves made on it keep fitting.
        for layout in LAYOUTS:
            self.assertEqual(P.centre(0, layout), (0.0, 1000.0, 0.0))

    def test_no_two_maps_share_space(self):
        for layout in LAYOUTS:
            boxes = {m: P.box(m, layout) for m in P.places(layout)}
            for a, b in itertools.combinations(sorted(boxes), 2):
                self.assertTrue(apart(boxes[a], boxes[b], P.GAP), (layout, a, b, boxes[a], boxes[b]))

    def test_squares_keep_one_lattice(self):
        # Every place is a whole number of 64 m squares from the first, so one lattice holds every map's squares.
        for layout in LAYOUTS:
            for m, c in P.places(layout).items():
                self.assertEqual(c[0] % 64, 0, (layout, m))
                self.assertEqual(c[2] % 64, 0, (layout, m))

    def test_everything_lies_inside_the_levels_bounds(self):
        for layout in LAYOUTS:
            lo, hi = P.bounds(layout)[:3], P.bounds(layout)[3:]
            for m in P.places(layout):
                b = P.box(m, layout)
                for i in range(3):
                    self.assertGreaterEqual(b[i], lo[i] + 64, (layout, m))
                    self.assertLessEqual(b[i + 3], hi[i] - 64, (layout, m))

    def test_stacked_is_no_wider_than_the_level_proven_in_the_harness(self):
        b = P.bounds('stacked')
        self.assertEqual((b[0], b[2], b[3], b[5]), (-16000, -16000, 16000, 16000))

    def test_the_layouts_are_fixed_for_good(self):
        # A place is in every save and every kept cut: these numbers must never change once released.
        self.assertEqual(P.centre(1, 'stacked'), (0.0, 1000.0 + P.FLOOR, 0.0))
        self.assertEqual(P.centre(36, 'stacked'), P.centre(36, 'stacked'))
        again = dict(P.places('stacked'))
        P._cache.clear()
        self.assertEqual(P.places('stacked'), again)


class Mapping(unittest.TestCase):
    def test_to_bg3_and_back(self):
        for layout in LAYOUTS:
            for m, w in ((0, (-8950.1, -132.3, 83.5)), (1, (1676.2, -4315.3, 61.5)), (36, (-16.4, -383.1, 61.8))):
                b = P.to_bg3(m, w, layout)
                back = P.to_wow(m, b, layout)
                self.assertTrue(all(abs(x - y) < 1e-6 for x, y in zip(w, back)), (layout, m))

    def test_eastern_kingdoms_points_are_todays(self):
        # own-level.md: WoW -8950.1 -132.3 83.5 stands at BG3 -121.0 1076.4 8184.0.
        b = P.to_bg3(0, (-8950.1, -132.3, 83.5))
        self.assertTrue(all(abs(x - y) < 0.1 for x, y in zip(b, (-121.0, 1076.4, 8184.0))), b)

    def test_a_point_lies_in_its_maps_box(self):
        for layout in LAYOUTS:
            x0, y0, z0, x1, y1, z1 = P.box(1, layout)
            b = P.to_bg3(1, (1676.2, -4315.3, 61.5), layout)   # Orgrimmar
            self.assertTrue(x0 <= b[0] <= x1 and y0 <= b[1] <= y1 and z0 <= b[2] <= z1)

    def test_map_at_finds_the_map_of_a_bg3_point(self):
        for layout in LAYOUTS:
            for m, w in ((0, (-8950.1, -132.3, 83.5)), (1, (1676.2, -4315.3, 61.5)), (36, (-16.4, -383.1, 61.8))):
                self.assertEqual(P.map_at(P.to_bg3(m, w, layout), layout), m)
            self.assertIsNone(P.map_at((0.0, -5000.0, 0.0), layout))

    def test_same_wow_numbers_on_two_maps_are_two_bg3_places(self):
        # What was wrong before: both continents lay over each other.
        for layout in LAYOUTS:
            a, b = P.to_bg3(0, (100.0, 200.0, 30.0), layout), P.to_bg3(1, (100.0, 200.0, 30.0), layout)
            self.assertGreater(max(abs(x - y) for x, y in zip(a, b)), 1000.0)


if __name__ == '__main__':
    unittest.main()
