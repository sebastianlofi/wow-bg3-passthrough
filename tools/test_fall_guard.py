#!/usr/bin/env python3
"""The fall guard leaves a walker on a WoW face alone (world_bridge.py: face_under)."""
import struct, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import world_bridge  # noqa: E402

FLOOR = struct.pack('<9f', 0, 10, 0, 10, 10, 0, 0, 10, 10)
HILL = struct.pack('<9f', 0, 25, 0, 10, 25, 0, 0, 25, 10)


class FaceUnder(unittest.TestCase):
    def test_on_a_cave_floor_under_a_hill(self):
        self.assertAlmostEqual(world_bridge.face_under(HILL + FLOOR, 2, 10.1, 2), 10.0)

    def test_nothing_near_is_a_fall(self):
        self.assertIsNone(world_bridge.face_under(HILL + FLOOR, 2, 2.0, 2))

    def test_beside_the_face(self):
        self.assertIsNone(world_bridge.face_under(FLOOR, 9, 10.1, 9))

    def test_a_sloped_face_at_its_own_height(self):
        ramp = struct.pack('<9f', 0, 0, 0, 10, 10, 0, 0, 0, 10)
        self.assertAlmostEqual(world_bridge.face_under(ramp, 5, 5.0, 1), 5.0)
        self.assertIsNone(world_bridge.face_under(ramp, 5, 9.0, 1))


if __name__ == '__main__':
    unittest.main()
