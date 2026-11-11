#!/usr/bin/env python3
"""test_wow_places.py: every WoW map has its own place in the one BG3 level, without a game or WoW's data.

  python3 tools/test_wow_places.py
"""
import itertools
import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wow_places as P  # noqa: E402

SPEC = json.loads((Path(__file__).resolve().parents[1] / 'release/levels/azeroth_a.json').read_text())


def apart(a, b, gap):
    """Two boxes (x0, y0, z0, x1, y1, z1) keep `gap` between them on at least one axis."""
    return any(a[i] - b[i + 3] >= gap or b[i] - a[i + 3] >= gap for i in range(3))


class Places(unittest.TestCase):
    def test_every_map_with_ground_has_a_place(self):
        # The 1.12 maps the server has ground or movement tiles for: 2 continents, 40 instances and test maps.
        self.assertEqual(len(P.places()), 42)
        for m in (0, 1, 36, 34, 369, 409, 533):
            self.assertIn(m, P.places())

    def test_an_unknown_map_has_none(self):
        with self.assertRaises(KeyError):
            P.centre(999)

    def test_the_eastern_kingdoms_stay_where_they_were(self):
        # The one mapping world mode had before (world_bridge.py FIXED_CENTRE): saves made on it keep fitting.
        self.assertEqual(P.centre(0), (0.0, 1000.0, 0.0))

    def test_no_two_maps_share_space(self):
        boxes = {m: P.box(m) for m in P.places()}
        for a, b in itertools.combinations(sorted(boxes), 2):
            self.assertTrue(apart(boxes[a], boxes[b], P.GAP), (a, b, boxes[a], boxes[b]))

    def test_squares_keep_one_lattice(self):
        # Every place is a whole number of 64 m squares from the first, so one lattice holds every map's squares.
        for m, c in P.places().items():
            self.assertEqual(c[0] % 64, 0, m)
            self.assertEqual(c[2] % 64, 0, m)

    def test_everything_lies_inside_the_levels_bounds(self):
        lo, hi = P.bounds()[:3], P.bounds()[3:]
        for m in P.places():
            b = P.box(m)
            for i in range(3):
                self.assertGreaterEqual(b[i], lo[i] + 64, m)
                self.assertLessEqual(b[i + 3], hi[i] - 64, m)

    def test_the_level_is_no_wider_than_the_one_proven_in_the_harness(self):
        b = P.bounds()
        self.assertEqual((b[0], b[2], b[3], b[5]), (-16000, -16000, 16000, 16000))

    def test_the_places_are_fixed_for_good(self):
        # A place is in every save and every kept cut: these numbers must never change once released.
        # (Proven in the harness: Kalimdor's floor at Shadowglen, the Stockade on the third floor.)
        self.assertEqual(P.centre(1), (0.0, 5096.0, 0.0))
        self.assertEqual(P.centre(34), (-2560.0, 9192.0, -15360.0))
        self.assertEqual(P.bounds(), (-16000, -100, -16000, 16000, 11840, 16000))
        again = dict(P.places())
        P._cache.clear()
        self.assertEqual(P.places(), again)


class Mapping(unittest.TestCase):
    POINTS = ((0, (-8950.1, -132.3, 83.5)), (1, (1676.2, -4315.3, 61.5)), (36, (-16.4, -383.1, 61.8)))

    def test_to_bg3_and_back(self):
        for m, w in self.POINTS:
            back = P.to_wow(m, P.to_bg3(m, w))
            self.assertTrue(all(abs(x - y) < 1e-6 for x, y in zip(w, back)), m)

    def test_eastern_kingdoms_points_are_where_they_were(self):
        # own-level.md: WoW -8950.1 -132.3 83.5 stands at BG3 -121.0 1076.4 8184.0.
        b = P.to_bg3(0, (-8950.1, -132.3, 83.5))
        self.assertTrue(all(abs(x - y) < 0.1 for x, y in zip(b, (-121.0, 1076.4, 8184.0))), b)

    def test_points_measured_in_the_harness(self):
        # Shadowglen (Kalimdor) and the Stockade's entrance, as the game placed the hero.
        for m, w, b in ((1, (10334.0, 833.9, 1326.1), (762.5, 6308.6, -9449.4)), (34, (49.0, 0.5, -16.4), (-2559.6, 9177.0, -15404.8))):
            self.assertTrue(all(abs(x - y) < 0.1 for x, y in zip(P.to_bg3(m, w), b)), m)

    def test_a_point_lies_in_its_maps_box(self):
        x0, y0, z0, x1, y1, z1 = P.box(1)
        b = P.to_bg3(1, (1676.2, -4315.3, 61.5))   # Orgrimmar
        self.assertTrue(x0 <= b[0] <= x1 and y0 <= b[1] <= y1 and z0 <= b[2] <= z1)

    def test_map_at_finds_the_map_of_a_bg3_point(self):
        for m, w in self.POINTS:
            self.assertEqual(P.map_at(P.to_bg3(m, w)), m)
        self.assertIsNone(P.map_at((0.0, -5000.0, 0.0)))

    def test_same_wow_numbers_on_two_maps_are_two_bg3_places(self):
        # What was wrong before: both continents lay over each other.
        a, b = P.to_bg3(0, (100.0, 200.0, 30.0)), P.to_bg3(1, (100.0, 200.0, 30.0))
        self.assertGreater(max(abs(x - y) for x, y in zip(a, b)), 1000.0)


class Level(unittest.TestCase):
    def test_the_released_levels_bounds_hold_every_place(self):
        # release/levels/azeroth_a.json is the level the release builds: it must be the one the places need.
        self.assertEqual(tuple(SPEC['Bounds']), P.bounds())
        self.assertEqual(P.level(SPEC), SPEC)

    def test_the_levels_own_squares_lie_inside_it_and_clear_of_every_place(self):
        b = SPEC['Bounds']
        boxes = [P.box(m) for m in P.places()]
        for q in P.level_squares(SPEC):
            self.assertTrue(b[0] <= q[0] and q[3] <= b[3] and b[2] <= q[2] and q[5] <= b[5], q)
            for box in boxes:
                self.assertTrue(apart(q, box, 1), (q, box))

    def test_two_corner_squares_span_the_level(self):
        # BG3 sizes its spatial grid and region map by the squares at the level's two corners.
        b, sq = SPEC['Bounds'], P.level_squares(SPEC)
        self.assertIn((b[0], b[2]), [(q[0], q[2]) for q in sq])
        self.assertIn((b[3], b[5]), [(q[3], q[5]) for q in sq])


if __name__ == '__main__':
    unittest.main()
