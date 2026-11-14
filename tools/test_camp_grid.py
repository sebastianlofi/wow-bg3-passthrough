#!/usr/bin/env python3
"""test_camp_grid.py: camp_grid.merge joins a placed sub-level's AI grid to the level's, each subgrid moved by the
template's position and kept byte for byte, ours untouched. Without the camp's grid in our level's file BG3's cursor
found no ground at camp and a click moved no one (BG3 loads only the level's own grid, not the placed sub-level's).
Grids made here with wowbg3_level.write_aigrid, so no game file is needed.

  python3 tools/test_camp_grid.py
"""
import os, struct, sys, tempfile, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import camp_grid  # noqa: E402
import wowbg3_level  # noqa: E402


def grid(name, subs):
    p = os.path.join(tempfile.mkdtemp(), "aigrid.data")
    wowbg3_level.write_aigrid(p, name, subs)
    return open(p, "rb").read()


class CampGrid(unittest.TestCase):
    def setUp(self):
        self.ours = grid("WOWBG3_Test_A", [{"X": -15872, "Y": 0, "Z": -15872, "Kind": "walk"},
                                           {"X": -16000, "Y": -20, "Z": -16000, "Kind": "blank"}])
        self.camp = grid("SCL_Test", [{"X": -81, "Y": -50.0, "Z": -42.5, "Kind": "walk"},
                                      {"X": -17, "Y": 3.5, "Z": 30.0, "Kind": "walk"}])

    def test_camp_subgrids_join_moved(self):
        merged, n = camp_grid.merge(self.ours, self.camp, [-14000.0, 1000.0, -14000.0])
        self.assertEqual(n, 2)
        _, _, subs, layers = camp_grid.parse(merged)
        _, _, ours, our_layers = camp_grid.parse(self.ours)
        _, _, camp, _ = camp_grid.parse(self.camp)
        self.assertEqual(len(subs), 4)
        self.assertEqual(subs[:2], ours)                     # ours first, as they were
        for got, src in zip(subs[2:], camp):
            self.assertEqual(got[0], src[0])                  # id, width, height
            self.assertEqual(got[4], src[4])                  # uuids, sizes, tiles: byte for byte
            self.assertEqual((got[1], got[2], got[3]), (src[1] - 14000.0, src[2] + 1000.0, src[3] - 14000.0))
        self.assertEqual(layers, our_layers)

    def test_count_in_header(self):
        merged, _ = camp_grid.merge(self.ours, self.camp, [0.0, 0.0, 0.0])
        self.assertEqual(struct.unpack_from("<I", merged, 41)[0], 4)

    def test_only_version_21(self):
        bad = struct.pack("<I", 20) + self.ours[4:]
        with self.assertRaises(ValueError):
            camp_grid.parse(bad)


if __name__ == "__main__":
    unittest.main()
