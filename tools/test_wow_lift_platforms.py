#!/usr/bin/env python3
"""test_wow_lift_platforms.py: the platforms BG3 keeps for WoW's lifts, and the mod's driver for them (Lifts.lua's
own text under lupa), without a game.

  python3 tools/test_wow_lift_platforms.py
"""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wow_lift_platforms as LP  # noqa: E402
import wow_places as P  # noqa: E402
import wowbg3_level  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
LUA = ROOT / 'mod/ScriptExtender/Lua/Lifts.lua'
try:
    import lupa
except ImportError:
    lupa = None


class Family(unittest.TestCase):
    def test_the_level_asks_for_them(self):
        spec = json.loads((ROOT / 'release/levels/azeroth_a.json').read_text())
        self.assertIs(spec.get('LiftPlatforms'), True)
        self.assertEqual(spec['Name'], LP.LEVEL)

    def test_every_platform_has_its_own_ids(self):
        inst = LP.instances()
        self.assertEqual(len(inst), len(LP.RADII) * LP.EACH)
        self.assertEqual(len({i['MapKey'] for i in inst}), len(inst))
        self.assertEqual({i['Root'] for i in inst}, {s['Root'] for s in LP.specs()})

    def test_parked_inside_the_level_and_off_every_wow_map(self):
        b = P.bounds()
        boxes = [P.box(m) for m in P.places()]
        for r in LP.RADII:
            for i in range(LP.EACH):
                x, y, z = LP.park(r, i)
                lo, hi = (x - r - 1, y - 1, z - r - 1), (x + r + 1, y + 1, z + r + 1)
                self.assertTrue(b[0] < lo[0] and hi[0] < b[3] and b[1] < lo[1] and hi[1] < b[4] and b[2] < lo[2] and hi[2] < b[5], (r, i))
                for box in boxes:
                    self.assertTrue(any(lo[k] > box[k + 3] or hi[k] < box[k] for k in range(3)), (r, i, box))

    def test_parked_platforms_do_not_touch(self):
        spots = [(LP.park(r, i), r) for r in LP.RADII for i in range(LP.EACH)]
        for a in range(len(spots)):
            for c in range(a + 1, len(spots)):
                (p, r1), (q, r2) = spots[a], spots[c]
                self.assertGreater(((p[0] - q[0]) ** 2 + (p[2] - q[2]) ** 2) ** 0.5, r1 + r2 + 2.0)

    def test_a_floor_is_a_disc_of_its_radius(self):
        for spec in LP.specs():
            sg = spec['Subgrids'][0]
            raw = wowbg3_level.tiles(sg['W'], sg['H'], sg['Kind'], sg['Radius'])
            walk = sum(1 for k in range(sg['W'] * sg['H']) if struct.unpack_from('<Q', raw, k * 16)[0] == 0)
            want = 3.14159 * sg['Radius'] ** 2
            self.assertLess(abs(walk * 0.25 - want) / want, 0.06, spec['Name'])
            self.assertEqual(sg['X'], -sg['W'] * 0.25)      # the disc's middle is the platform's own origin

    def test_the_smallest_floor_a_car_fits_in(self):
        self.assertEqual(LP.pick(7.68), 8.0)       # Undercity's car, 8.4 yd
        self.assertEqual(LP.pick(5.03), 5.0)       # within the slack
        self.assertEqual(LP.pick(5.4), 6.5)
        self.assertIsNone(LP.pick(14.0))

    def test_the_level_builds_with_them(self):
        spec = json.loads((ROOT / 'release/levels/azeroth_a.json').read_text())
        spec['LevelTemplates'] = LP.instances()
        with tempfile.TemporaryDirectory() as d:
            wowbg3_level.build(spec, d)
            text = (Path(d) / 'LevelTemplates/_merged.lsx').read_text()
            self.assertEqual(text.count('"LevelTemplate"'), len(LP.RADII) * LP.EACH)
        for s in LP.specs():
            with tempfile.TemporaryDirectory() as d:
                wowbg3_level.build(s, d)
                flag = [line for line in (Path(d) / 'meta.lsx').read_text().splitlines() if 'IsPlatformLevel' in line][0]
                self.assertIn('value="True"', flag)
                self.assertFalse((Path(d) / 'Triggers').exists())
        self.assertEqual(wowbg3_level.root_templates_lsx(LP.specs()).count('"SubLevelName"'), len(LP.RADII))


class ModTable(unittest.TestCase):
    def test_lifts_lua_has_the_same_family(self):
        self.assertIn(LP.lua(), LUA.read_text())


@unittest.skipIf(lupa is None, 'needs the lupa module')
class Driver(unittest.TestCase):
    """Lifts.lua's own text: the record it reads, the floor's place, the order a platform gets."""
    def setUp(self):
        L = lupa.LuaRuntime(unpack_returned_tuples=True)
        L.execute('Ext = {}; Osi = {}')
        self.M = L.execute(LUA.read_text())
        self.L = L

    @staticmethod
    def record(moving, pos, to, to_ms):
        return struct.pack('<Q4I6fI', 0xF1200050AF00AF65, 20655, 1 if moving else 0, 16667, 5964, *pos, *to, to_ms)

    def parse(self, raw):
        return self.L.eval('function(M, raw) return M.Parse(raw) end')(self.M, raw)

    def test_parse(self):
        raw = b'WBL1' + struct.pack('<II', 7, 1) + self.record(True, (1544.2, 240.8, 27.2), (1544.2, 240.8, 55.7), 1036) + struct.pack('<I', 7)
        rec = self.parse(raw)[1]
        self.assertEqual((rec.Guid, rec.Entry, rec.Moving, rec.Period, rec.Cycle, rec.ToMs), ('f1200050af00af65', 20655, True, 16667, 5964, 1036))
        self.assertAlmostEqual(rec.Pos[3], 27.2, places=4)
        self.assertAlmostEqual(rec.To[3], 55.7, places=4)
        self.assertIsNone(self.parse(raw[:-4] + struct.pack('<I', 8)))      # the two seqs differ
        self.assertIsNone(self.parse(b'nope'))

    def test_the_floors_place_is_world_modes_mapping_raised_by_the_floor(self):
        p = self.L.eval('function(M) return M.ToBg3({1544.2, 240.8, 55.7}, {0, 1000, 0}, -0.686) end')(self.M)
        self.assertAlmostEqual(p[1], 240.8 * 0.9144, places=3)
        self.assertAlmostEqual(p[2], 1000 + 55.7 * 0.9144 - 0.686, places=3)
        self.assertAlmostEqual(p[3], -1544.2 * 0.9144, places=3)

    def test_a_moving_car_orders_its_spans_end_at_the_speed_that_arrives_with_it(self):
        order = self.L.eval("""function(M)
            local l = {Moving = true, To = {0, 0, 55.7}, ToMs = 2000}
            return M.Order(l, {0, 1000 + 27.2 * 0.9144, 0}, {0, 1000, 0}, 0)
        end""")(self.M)
        self.assertAlmostEqual(order.To[2], 1000 + 55.7 * 0.9144, places=3)
        self.assertAlmostEqual(order.Speed, (55.7 - 27.2) * 0.9144 / 2.0, places=3)

    def test_a_platform_on_its_resting_car_gets_no_order(self):
        order = self.L.eval("""function(M)
            local l = {Moving = false, To = {0, 0, 55.7}, ToMs = 4000}
            return M.Order(l, {0, 1000 + 55.7 * 0.9144, 0}, {0, 1000, 0}, 0)
        end""")(self.M)
        self.assertIsNone(order)

    def test_a_platform_off_its_resting_car_is_there_in_a_quarter_second(self):
        order = self.L.eval("""function(M)
            local l = {Moving = false, To = {0, 0, 0}, ToMs = 4000}
            return M.Order(l, {0, 1000 + 10, 0}, {0, 1000, 0}, 0)
        end""")(self.M)
        self.assertAlmostEqual(order.Speed, 40.0, places=3)

    def test_size(self):
        size = self.L.eval('function(M, r) return M.Size(r) end')
        self.assertEqual(self.M.PLATFORMS[size(self.M, 7.68)].Radius, 8.0)
        self.assertIsNone(size(self.M, 14.0))


if __name__ == '__main__':
    unittest.main()
