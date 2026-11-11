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
