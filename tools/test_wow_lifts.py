#!/usr/bin/env python3
"""test_wow_lifts.py: where a WoW lift's car is at any moment, where it rests and how it travels, without a
game, a server or WoW's data (a made-up TransportAnimation.dbc; WBDBC=FOLDER also checks the real one there).

  python3 tools/test_wow_lifts.py
"""
import math
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wow_lifts as L  # noqa: E402


def write_dbc(path, rows):
    """rows: (id, entry, ms, x, y, z, sequence)."""
    recs = b''.join(struct.pack('<3I3fI', *r) for r in rows)
    path.write_bytes(struct.pack('<4s4I', b'WDBC', len(rows), 7, 28, 1) + recs + b'\0')


# A lift like Undercity's: rests at the top, goes down 40 yd, rests, comes back; ends where it began.
LIFT = [(0, (0.0, 0.0, 0.0)), (5000, (0.0, 0.0, 0.0)), (9000, (0.0, 0.0, -10.0)), (13000, (0.0, 0.0, -40.0)),
        (20000, (0.0, 0.0, -40.0)), (28000, (0.0, 0.0, 0.0)), (30000, (0.0, 0.0, 0.0))]
NO_TURN = (0.0, 0.0, 0.0, 1.0)
QUARTER = (0.0, 0.0, math.sin(math.pi / 4), math.cos(math.pi / 4))  # 90 degrees about WoW's up


def near(a, b, eps=1e-4):
    return all(abs(x - y) <= eps for x, y in zip(a, b))


class Paths(unittest.TestCase):
    def read(self, rows):
        with tempfile.TemporaryDirectory() as d:
            write_dbc(Path(d) / 'TransportAnimation.dbc', rows)
            return L.read_paths(d)

    def test_grouped_by_entry_in_time_order(self):
        p = self.read([(1, 700, 4000, 0.0, 0.0, -8.0, 0), (2, 700, 0, 0.0, 0.0, 0.0, 0),
                       (3, 701, 0, 0.0, 0.0, 0.0, 0), (4, 701, 1000, 1.0, 0.0, 0.0, 0)])
        self.assertEqual(sorted(p), [700, 701])
        self.assertEqual(p[700], [(0, (0.0, 0.0, 0.0)), (4000, (0.0, 0.0, -8.0))])

    def test_a_path_that_cannot_run_a_cycle_is_left_out(self):
        # The client's rule (benilla-formats elevators.rs): under two frames, no frame at 0, or a period of 0.
        p = self.read([(1, 1, 0, 0.0, 0.0, 0.0, 0),
                       (2, 2, 100, 0.0, 0.0, 0.0, 0), (3, 2, 200, 0.0, 0.0, 1.0, 0),
                       (4, 3, 0, 0.0, 0.0, 0.0, 0), (5, 3, 0, 0.0, 0.0, 1.0, 0)])
        self.assertEqual(p, {})

    def test_not_a_dbc(self):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'TransportAnimation.dbc').write_bytes(b'nope' + bytes(40))
            with self.assertRaises(ValueError):
                L.read_paths(d)


class Sample(unittest.TestCase):
    def test_period_is_the_last_frames_time(self):
        self.assertEqual(L.period(LIFT), 30000)

    def test_at_a_keyframe_and_between(self):
        self.assertEqual(L.sample(LIFT, (10.0, 20.0, 30.0), NO_TURN, 0), ((10.0, 20.0, 30.0), False))
        pos, moving = L.sample(LIFT, (10.0, 20.0, 30.0), NO_TURN, 7000)
        self.assertTrue(near(pos, (10.0, 20.0, 25.0)))
        self.assertTrue(moving)
        pos, moving = L.sample(LIFT, (10.0, 20.0, 30.0), NO_TURN, 15000)
        self.assertTrue(near(pos, (10.0, 20.0, -10.0)))
        self.assertFalse(moving)

    def test_the_cycle_wraps(self):
        a = L.sample(LIFT, (0.0, 0.0, 0.0), NO_TURN, 7000)
        self.assertEqual(L.sample(LIFT, (0.0, 0.0, 0.0), NO_TURN, 7000 + 3 * 30000), a)

    def test_the_spawn_turns_the_offset(self):
        # WoW axes: a quarter turn about up takes +x to +y.
        frames = [(0, (0.0, 0.0, 0.0)), (1000, (10.0, 0.0, 0.0))]
        pos, _ = L.sample(frames, (100.0, 200.0, 5.0), QUARTER, 500)
        self.assertTrue(near(pos, (100.0, 205.0, 5.0)))


class Stops(unittest.TestCase):
    def test_where_and_when_the_car_rests(self):
        # The rest over the cycle's seam (28 s to 30 s, then 0 to 5 s) is one stop.
        self.assertEqual(L.stops(LIFT), [(28000, 5000, (0.0, 0.0, 0.0)), (13000, 20000, (0.0, 0.0, -40.0))])

    def test_a_car_that_never_rests_has_no_stop(self):
        self.assertEqual(L.stops([(0, (0.0, 0.0, 0.0)), (1000, (0.0, 0.0, 5.0)), (2000, (0.0, 0.0, 0.0))]), [])

    def test_at_stop(self):
        self.assertEqual(L.at_stop(LIFT, 29000), 0)
        self.assertEqual(L.at_stop(LIFT, 2000), 0)
        self.assertEqual(L.at_stop(LIFT, 5000), None)   # leaving
        self.assertEqual(L.at_stop(LIFT, 16000), 1)
        self.assertEqual(L.at_stop(LIFT, 9000), None)


class Legs(unittest.TestCase):
    def test_a_leg_runs_from_one_stop_to_the_next(self):
        legs = L.legs(LIFT)
        self.assertEqual([(g['From'], g['To'], g['Start'], g['End']) for g in legs],
                         [(0, 1, 5000, 13000), (1, 0, 20000, 28000)])
        # Its spans keep the path's own speeds: 10 yd in 4 s, then 30 yd in 4 s.
        self.assertEqual(legs[0]['Spans'], [(5000, 9000, (0.0, 0.0, 0.0), (0.0, 0.0, -10.0)),
                                            (9000, 13000, (0.0, 0.0, -10.0), (0.0, 0.0, -40.0))])

    def test_span_speed_in_yards_a_second(self):
        self.assertAlmostEqual(L.speed((9000, 13000, (0.0, 0.0, -10.0), (0.0, 0.0, -40.0))), 7.5)

    def test_stops_and_legs_fill_the_cycle(self):
        rest = sum((b - a) % L.period(LIFT) for a, b, _ in L.stops(LIFT))
        ride = sum(g['End'] - g['Start'] for g in L.legs(LIFT))
        self.assertEqual(rest + ride, L.period(LIFT))


class Mapping(unittest.TestCase):
    def test_wow_to_bg3_is_world_modes_mapping(self):
        # world_bridge.py World.to_bg3: BG3 = centre + (dy, dz, -dx) * 0.9144.
        p = L.to_bg3((110.0, 220.0, 35.0), centre=(0.0, 1000.0, 0.0), origin=(100.0, 200.0, 30.0))
        self.assertTrue(near(p, (20 * 0.9144, 1000 + 5 * 0.9144, -10 * 0.9144)))

    def test_a_lifts_height_in_bg3_metres(self):
        up = L.to_bg3((0.0, 0.0, 0.0), (0.0, 1000.0, 0.0), (0.0, 0.0, 0.0))
        down = L.to_bg3((0.0, 0.0, -40.0), (0.0, 1000.0, 0.0), (0.0, 0.0, 0.0))
        self.assertAlmostEqual(up[1] - down[1], 40 * 0.9144, places=4)


class Deck(unittest.TestCase):
    """A car's floor from its collision model."""
    @staticmethod
    def quad(x0, y0, x1, y1, z, up=True):
        """A level quad as the server's collision models wind one: clockwise seen from the side it faces."""
        v = [x0, y0, z, x1, y0, z, x1, y1, z, x0, y1, z]
        return (v, [0, 2, 1, 0, 3, 2]) if up else (v, [0, 1, 2, 0, 2, 3])

    def model(self, *parts):
        verts, idx = [], []
        for v, i in parts:
            idx += [k + len(verts) // 3 for k in i]
            verts += v
        return [((0, 0, 0), (0, 0, 0), tuple(verts), tuple(idx))]

    def test_the_floor_is_where_most_level_area_lies(self):
        # A 6 x 8 yd floor at z 0.5 with a small step at z 2 and a wall.
        wall = ([3.0, -4.0, 0.5, 3.0, 4.0, 0.5, 3.0, 4.0, 3.0], [0, 1, 2])
        d = L.deck(self.model(self.quad(-3, -4, 3, 4, 0.5), self.quad(-1, -1, 1, 1, 2.0), wall))
        self.assertAlmostEqual(d['Floor'], 0.5)
        self.assertAlmostEqual(d['Area'], 48.0)
        self.assertAlmostEqual(d['Radius'], 5.0)
        self.assertEqual(d['Box'], (-3, -4, 3, 4))

    def test_the_top_of_the_plate_not_its_underside_or_the_roof(self):
        # Undercity's car: a plate with its top at -0.46 and its larger underside at -0.75; a roof's underside above.
        d = L.deck(self.model(self.quad(-7, -7, 7, 7, -0.46), self.quad(-8, -8, 8, 8, -0.75, up=False), self.quad(-8, -8, 8, 8, 6.0, up=False)))
        self.assertAlmostEqual(d['Floor'], -0.46)
        self.assertEqual(d['Box'], (-7, -7, 7, 7))

    def test_a_door_has_no_floor(self):
        door = ([0.0, -2.0, 0.0, 0.0, 2.0, 0.0, 0.0, 2.0, 6.0], [0, 1, 2])
        self.assertIsNone(L.deck(self.model(door)))
        self.assertIsNone(L.deck(self.model(self.quad(-0.5, -0.5, 0.5, 0.5, 0.0))))   # a square yard: nothing to ride


class Decks(unittest.TestCase):
    def test_every_rideable_lift_by_entry_in_metres(self):
        with tempfile.TemporaryDirectory() as d:
            v = Path(d)
            name = b'Car.m2'
            (v / 'temp_gameobject_models').write_bytes(
                struct.pack('<II', 455, len(name)) + name + bytes(24) + struct.pack('<II', 462, 7) + b'Door.m2' + bytes(24))
            (v / 'Car.m2.vmo').write_bytes(b'car')
            (v / 'Door.m2.vmo').write_bytes(b'door')
            self.assertEqual(L.models(v), {455: 'Car.m2', 462: 'Door.m2'})
            quad = Deck.quad(-3, -4, 3, 4, -0.75)
            car = [((0, 0, 0), (0, 0, 0), tuple(quad[0]), tuple(quad[1]))]
            read = lambda p: car if p.name == 'Car.m2.vmo' else []   # noqa: E731
            got = L.decks([(20649, 455), (20650, 462), (20652, 455), (1, 999)], v, read)
        self.assertEqual(sorted(got), [20649, 20652])                 # the cars; not the door, not a model without a file
        self.assertAlmostEqual(got[20649]['Radius'], 5.0 * 0.9144, places=3)
        self.assertAlmostEqual(got[20649]['Floor'], -0.75 * 0.9144, places=3)


class Carve(unittest.TestCase):
    """WoW's static floor under a resting car is the car's: the square's tiles there are not walkable."""
    def section(self, rec, floor):
        one = struct.pack('<Bf', 1, floor) + (struct.pack('<f', floor) if rec == 9 else b'')
        return one * (128 * 128)

    def walkable(self, section, rec, r, c):
        return section[(r * 128 + c) * rec] & 1

    def test_stops_in_the_world(self):
        pts = L.stop_points(LIFT, (100.0, 200.0, 50.0), NO_TURN)
        self.assertEqual(pts, [(100.0, 200.0, 50.0), (100.0, 200.0, 10.0)])

    def test_the_floor_under_the_car_goes_and_nothing_else(self):
        for rec in (5, 9):
            s, n = L.carve(self.section(rec, 962.2), rec, 0.0, 0.0, [(32.0, 962.0, 32.0, 4.0)])
            self.assertEqual(self.walkable(s, rec, 64, 64), 0)             # under the car
            self.assertEqual(self.walkable(s, rec, 64, 64 + 9), 1)         # 4.5 m beside it
            self.assertEqual(self.walkable(s, rec, 0, 0), 1)
            self.assertLess(abs(n * 0.25 - 3.14159 * 16) / 50.3, 0.08, rec)   # a disc of 4 m

    def test_the_tile_under_the_car_goes_far_from_the_cars_floor(self):
        # BG3 stands a walker on the tile nearest its height: 12 cm from the platform's is too near.
        for rec in (5, 9):
            s, _ = L.carve(self.section(rec, 962.18), rec, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)])
            k = (64 * 128 + 64) * rec
            for o in range(1, rec, 4):
                self.assertAlmostEqual(struct.unpack_from('<f', s, k + o)[0], 962.18 + L.AWAY_M, places=2)
            self.assertAlmostEqual(struct.unpack_from('<f', s, 1)[0], 962.18, places=2)   # a tile elsewhere

    def test_not_past_the_highest_a_subgrid_holds(self):
        s, n = L.carve(self.section(5, 962.18), 5, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)], top=1000.0)
        k = (64 * 128 + 64) * 5
        self.assertEqual(s[k] & 1, 0)
        self.assertAlmostEqual(struct.unpack_from('<f', s, k + 1)[0], 962.18, places=2)
        self.assertGreater(n, 0)

    def test_the_ground_beside_the_car_is_seams(self):
        # BG3 steps from a square's subgrid onto the platform's only on seam tiles (flags 4 and 8).
        s, _ = L.carve(self.section(5, 962.2), 5, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)])
        self.assertEqual(s[(64 * 128 + 64 + 8) * 5], 1 | 12)       # 4.25 m from the middle: the ring beside the floor
        self.assertEqual(s[(64 * 128 + 64 + 11) * 5], 1)           # 5.75 m: plain ground
        self.assertEqual(s[(64 * 128 + 64) * 5] & 13, 0)            # under the car: taken away, no seam
        # A floor at another height beside the shaft is no way onto the car.
        s, _ = L.carve(self.section(5, 970.0), 5, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)])
        self.assertEqual(s[(64 * 128 + 64 + 8) * 5], 1)
        # The walk and fall copies of the tiles get no seam bits.
        s, _ = L.carve(self.section(5, 962.2), 5, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)], seam=0)
        self.assertEqual(s[(64 * 128 + 64 + 8) * 5], 1)

    def test_the_ground_slopes_up_to_the_cars_floor(self):
        # The tram's deck lies 0.7 m over the station floor: WoW walks up that step, BG3 only a slope.
        deck, floor = 962.9, 962.2
        for rec in (5, 9):
            for seam in (12, 0):                                    # the walk and fall copies slope the same way
                s, _ = L.carve(self.section(rec, floor), rec, 0.0, 0.0, [(32.0, deck, 32.0, 4.0)], seam=seam)
                def h(c):
                    return [struct.unpack_from('<f', s, (64 * 128 + c) * rec + o)[0] for o in range(1, rec, 4)]
                last = deck
                for c in range(64 + 8, 64 + 16):                    # outwards from the floor's edge (4.25 m ... 7.75 m)
                    for v in h(c):
                        self.assertLessEqual(v, last + 1e-4)
                        self.assertLessEqual(last - v, 0.5 * L.RISE + 1e-4, (rec, c))   # 0.2 m a tile at most
                    last = h(c)[0]
                self.assertAlmostEqual(h(64 + 8)[0], deck, places=3)                 # the ring of seams: the car's floor
                self.assertAlmostEqual(h(64 + 9)[0], deck, places=3)
                self.assertAlmostEqual(h(64 + 11)[0], deck - 0.7554 * L.RISE, places=3)   # 1.75 m out: three quarters of a metre down the slope
                self.assertAlmostEqual(h(64 + 14)[0], floor, places=3)               # the slope is 1.75 m long: plain ground
                self.assertAlmostEqual(h(64 + 30)[0], floor, places=3)
        # A car resting below the ground beside it: the slope goes down to it.
        s, _ = L.carve(self.section(5, 962.2), 5, 0.0, 0.0, [(32.0, 961.9, 32.0, 4.0)])
        v = struct.unpack_from('<f', s, (64 * 128 + 64 + 8) * 5 + 1)[0]
        self.assertTrue(961.9 < v < 962.2)
        # Level with the ground (Undercity's car): no tile's height changes.
        s, _ = L.carve(self.section(5, 962.3), 5, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)])
        self.assertAlmostEqual(struct.unpack_from('<f', s, (64 * 128 + 64 + 8) * 5 + 1)[0], 962.3, places=4)
        # A floor more than a step away is another storey, not the way on.
        s, _ = L.carve(self.section(5, 964.0), 5, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)])
        self.assertAlmostEqual(struct.unpack_from('<f', s, (64 * 128 + 64 + 8) * 5 + 1)[0], 964.0, places=4)

    def test_the_slope_is_faces_too(self):
        # BG3 stands a character on WoW's faces, not on the tiles: the slope has to be there for its rays.
        deck, floor = 962.9, 962.2
        faces = L.slope_faces(self.section(5, floor), 5, 0.0, 0.0, [(32.0, deck, 32.0, 4.0)])
        self.assertTrue(faces)
        def height(x, z):
            for f in faces:
                a, b, c = f[0:3], f[3:6], f[6:9]
                d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
                u = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
                v = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
                if u >= -1e-6 and v >= -1e-6 and u + v <= 1 + 1e-6:
                    return u * a[1] + v * b[1] + (1 - u - v) * c[1]
        self.assertAlmostEqual(height(36.6, 32.1), deck, places=2)               # the ring beside the car: its floor's height
        self.assertAlmostEqual(height(37.9, 32.1), deck - 0.9 * L.RISE, places=2)   # 0.9 m down the slope
        self.assertIsNone(height(40.0, 32.1))                                    # past the slope: WoW's own floor
        self.assertIsNone(height(34.0, 32.1))                                    # under the car: the platform's
        for f in faces:                                                          # every face looks up
            u, v = [f[3 + k] - f[k] for k in range(3)], [f[6 + k] - f[k] for k in range(3)]
            self.assertGreater(u[2] * v[0] - u[0] * v[2], 0)
            self.assertTrue(all(floor - 1e-4 <= f[k] <= deck + 1e-4 for k in (1, 4, 7)))
        # A car's floor level with the ground or under it: no faces (WoW's own floor is the one to stand on).
        self.assertEqual(L.slope_faces(self.section(5, 962.3), 5, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)]), [])
        self.assertEqual(L.slope_faces(self.section(5, 962.6), 5, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)]), [])
        self.assertEqual(L.slope_faces(self.section(5, 960.0), 5, 0.0, 0.0, [(32.0, 962.3, 32.0, 4.0)]), [])

    def test_another_floor_in_the_same_column_stays(self):
        s, n = L.carve(self.section(5, 990.0), 5, 0.0, 0.0, [(32.0, 962.0, 32.0, 4.0)])
        self.assertEqual(n, 0)
        self.assertEqual(self.walkable(s, 5, 64, 64), 1)

    def test_a_car_at_the_squares_edge(self):
        s, n = L.carve(self.section(5, 962.0), 5, 0.0, 0.0, [(-1.0, 962.0, 10.0, 4.0)])
        self.assertEqual(self.walkable(s, 5, 20, 0), 0)
        self.assertGreater(n, 0)


class Live(unittest.TestCase):
    """benilla's wowlifts.bin (patch 0139), as crates/benilla-app/src/wowbg3_lifts.rs writes it."""
    def record(self):
        return struct.pack('<Q4I6fI', 0xF12000500A900001, 20649, 1, 16667, 5000, 1.0, 2.0, 3.0, 1.0, 2.0, 40.0, 250)

    def test_a_whole_file(self):
        raw = b'WBL1' + struct.pack('<II', 9, 2) + self.record() * 2 + struct.pack('<I', 9)
        lifts = L.read_live(raw)
        self.assertEqual(len(lifts), 2)
        self.assertEqual(lifts[0], {'Guid': 0xF12000500A900001, 'Entry': 20649, 'Moving': True, 'Period': 16667, 'Cycle': 5000,
                                    'Pos': (1.0, 2.0, 3.0), 'To': (1.0, 2.0, 40.0), 'ToMs': 250})

    def test_no_lifts(self):
        self.assertEqual(L.read_live(b'WBL1' + struct.pack('<III', 3, 0, 3)), [])

    def test_a_torn_or_foreign_file_is_none(self):
        raw = b'WBL1' + struct.pack('<II', 9, 2) + self.record() * 2 + struct.pack('<I', 8)
        self.assertIsNone(L.read_live(raw))            # the two seqs differ: written while read
        self.assertIsNone(L.read_live(raw[:-10]))
        self.assertIsNone(L.read_live(b'nope' + bytes(12)))


@unittest.skipUnless(os.environ.get('WBDBC'), 'WBDBC=FOLDER with the real TransportAnimation.dbc')
class RealData(unittest.TestCase):
    def test_every_path_holds_together(self):
        paths = L.read_paths(os.environ['WBDBC'])
        self.assertGreater(len(paths), 10)
        for entry, frames in paths.items():
            T = L.period(frames)
            stops, legs = L.stops(frames), L.legs(frames)
            for i, (a, b, off) in enumerate(stops):
                mid = (a + ((b - a) % T) // 2) % T
                pos, moving = L.sample(frames, (0.0, 0.0, 0.0), NO_TURN, mid)
                self.assertTrue(near(pos, off, 1e-3), (entry, i))
                self.assertFalse(moving, (entry, i))
            if stops:
                rest = sum((b - a) % T or T for a, b, _ in stops)
                self.assertEqual(rest + sum(g['End'] - g['Start'] for g in legs), T, entry)
            for g in legs:
                for s in g['Spans']:
                    self.assertLess(L.speed(s), 100.0, (entry, s))


if __name__ == '__main__':
    unittest.main()
