#!/usr/bin/env python3
"""The fall alarm: what counts as a fall, and that nothing is ever moved (world_bridge.py: World.fall_alarm)."""
import contextlib, io, struct, sys, tempfile, types, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import world_bridge  # noqa: E402

FLOOR = struct.pack('<9f', 0, 10, 0, 10, 10, 0, 0, 10, 10)
HILL = struct.pack('<9f', 0, 25, 0, 10, 25, 0, 0, 25, 10)
WALL = struct.pack('<9f', 0, 0, 5, 10, 0, 5, 10, 30, 5)


class Faces(unittest.TestCase):
    def test_every_face_of_the_column(self):
        self.assertEqual(sorted(world_bridge.faces_over(HILL + FLOOR, 2, 2)), [10.0, 25.0])

    def test_beside_every_face(self):
        self.assertEqual(world_bridge.faces_over(FLOOR, 9, 9), [])

    def test_a_wall_holds_nobody(self):
        self.assertEqual(world_bridge.faces_over(WALL, 5, 5), [])

    def test_the_line_between_two_triangles_is_no_crack(self):
        other = struct.pack('<9f', 10, 10, 0, 10, 10, 10, 0, 10, 10)
        self.assertTrue(world_bridge.faces_over(FLOOR + other, 5, 5))


class Fall(unittest.TestCase):
    def test_on_a_cave_floor_under_a_hill(self):
        self.assertEqual(world_bridge.face_under(HILL + FLOOR, 2, 10.1, 2), 10.0)

    def test_in_the_air_over_a_floor_is_no_fall(self):
        self.assertEqual(world_bridge.face_under(HILL + FLOOR, 2, 18.0, 2), 10.0)

    def test_under_every_face_is(self):
        self.assertIsNone(world_bridge.face_under(HILL + FLOOR, 2, 2.0, 2))

    def test_nothing_in_the_column_is(self):
        self.assertIsNone(world_bridge.face_under(FLOOR, 9, 2.0, 9))


class Alarm(unittest.TestCase):
    """The hill's tile floor at 25, a cave's floor at 10 under it, and a walker going down."""

    def world(self, tris=HILL + FLOOR, placed=True):
        return types.SimpleNamespace(
            centre=(0.0, 0.0, 0.0), run='r', placed={0: (0, 0)} if placed else {}, cells={(0, 0): {'tris': tris}},
            floor_at=lambda x, z: (True, 25.0) if placed else None, in_level=lambda p: False)

    def run_it(self, world, walker, y, x=2.0, z=2.0):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            world_bridge.World.fall_alarm(world, walker, x, y, z)
        return out.getvalue()

    def walker(self):
        return world_bridge.Walker('u', 'Tav')

    def test_a_walk_through_the_cave_says_nothing(self):
        world, w = self.world(), self.walker()
        for _ in range(3):
            w.faces_read = 0.0
            self.assertEqual(self.run_it(world, w, 10.1), '')

    def test_a_shove_down_a_shaft_into_the_cave_says_nothing(self):
        world, w = self.world(), self.walker()
        for y in (25.0, 20.0, 15.0, 12.0, 10.1):
            w.faces_read = 0.0
            self.assertEqual(self.run_it(world, w, y), '')

    def test_under_all_of_it_says_so_once_in_a_while(self):
        world, w = self.world(), self.walker()
        self.assertIn('fall alarm: Tav', self.run_it(world, w, 2.0))
        w.faces_read = 0.0
        self.assertEqual(self.run_it(world, w, 2.0), '')

    def test_it_never_writes_a_command(self):
        with tempfile.TemporaryDirectory() as d:
            io_dir, world_bridge.IO = world_bridge.IO, Path(d)
            try:
                self.run_it(self.world(), self.walker(), 2.0)
                self.assertEqual(list(Path(d).iterdir()), [])
            finally:
                world_bridge.IO = io_dir


if __name__ == '__main__':
    unittest.main()
