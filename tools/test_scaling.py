#!/usr/bin/env python3
"""test_scaling.py: Scaling.lua's numbers, without a game (needs the `lupa` module).

  python3 tools/test_scaling.py
"""
import sys, unittest
from pathlib import Path

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua/Scaling.lua'
try:
    import lupa
    from lupa import LuaRuntime
except ImportError:
    print('test_scaling.py: needs the lupa module (pip install lupa); skipped')
    raise SystemExit(0)


class Scaling(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lua = LuaRuntime(unpack_returned_tuples=True)
        cls.M = cls.lua.execute(LUA.read_text())

    def mob(self, level, wow_hp, hit, w=None, n=4, cap=18, party=None, diff='balanced', map_=0, camp=1):
        w = level if w is None else w
        c = self.lua.table_from({'Level': level, 'WowMaxHp': wow_hp, 'WowDamage': hit, 'Map': map_, 'Members': n, 'Camp': camp, 'WowLevel': w,
                                 'Cap': cap, 'Difficulty': diff})
        if party is not None:
            c['PartyLevel'] = party
        return self.M.Compute(c)

    def test_table(self):
        self.assertAlmostEqual(self.M.Effective(60, 18), 18)
        self.assertAlmostEqual(self.M.Effective(4, 18), 1)

    def test_tables_have_their_lengths(self):
        self.assertEqual(len(self.M.NORMAL_HP), 63)
        self.assertEqual(len(self.M.NORMAL_HIT), 63)
        self.assertEqual(len(self.M.DPR), 18)
        self.assertEqual(len(self.M.MEMBER_HP), 18)

    def test_normal_mob_at_60(self):
        n = self.mob(60, 4059, 120.9)
        self.assertEqual(n['Rank'], 'normal')
        self.assertEqual(n['E'], 18)
        self.assertEqual(n['MaxHp'], 214)     # 26.8 x 8
        self.assertEqual(n['AC'], 20)
        self.assertEqual(n['ToHit'], 12)
        self.assertAlmostEqual(n['Damage'], 0.30 * 0.65 * 147, places=0)

    def test_ranks_from_wow_hp(self):
        self.assertEqual(self.mob(40, 1865, 66.7)['Rank'], 'normal')
        self.assertEqual(self.mob(40, 5000, 170)['Rank'], 'elite')
        self.assertEqual(self.mob(40, 5000, 170, map_=230)['Rank'], 'dungeon_trash')
        self.assertEqual(self.mob(40, 15240, 423)['Rank'], 'dungeon_boss')
        self.assertEqual(self.mob(60, 12000, 500, map_=409)['Rank'], 'raid_trash')
        self.assertEqual(self.mob(60, 1099230, 1922, map_=409)['Rank'], 'raid_boss')

    def test_members_present(self):
        full, one = self.mob(40, 1865, 66.7), self.mob(40, 1865, 66.7, n=1)
        self.assertLess(one['MaxHp'], full['MaxHp'])
        self.assertLess(one['Damage'], full['Damage'])

    def test_party_below_the_table_pulls_mobs_down(self):
        self.assertAlmostEqual(self.mob(60, 4059, 120.9, party=12)['E'], 12)
        self.assertEqual(self.mob(60, 4059, 120.9, party=20)['E'], 18)

    def test_camp(self):
        a, b, c = self.mob(20, 484, 33.3), self.mob(20, 484, 33.3, camp=3), self.mob(20, 484, 33.3, camp=20)
        self.assertAlmostEqual(b['MaxHp'], a['MaxHp'] * 0.62, delta=1)
        self.assertLess(b['AC'] + b['ToHit'], a['AC'] + a['ToHit'] + 1)
        self.assertLess(c['Damage'], b['Damage'])
        lua = self.lua
        me = lua.table_from({'Hostile': True, 'Pos': lua.table_from([0, 0, 0])})
        near = lua.table_from({'Hostile': True, 'Pos': lua.table_from([30, 0, 0])})
        chained = lua.table_from({'Hostile': True, 'Pos': lua.table_from([60, 0, 0])})
        far = lua.table_from({'Hostile': True, 'Pos': lua.table_from([200, 0, 0])})
        neutral = lua.table_from({'Hostile': False, 'Pos': lua.table_from([1, 0, 0])})
        self.assertEqual(self.M.CampOf(me, lua.table_from([me, near, chained, far, neutral])), 3)

    def test_gray_mob_dies_to_a_hit(self):
        n = self.mob(25, 713, 37.5, w=40)
        self.assertLessEqual(n['MaxHp'], 0.6 * 24.5)

    def test_above_the_player(self):
        a, b = self.mob(45, 2218, 86.8, w=40), self.mob(45, 2218, 86.8)
        self.assertGreater(a['MaxHp'], b['MaxHp'])
        self.assertEqual(a['AC'], b['AC'] + 1)

    def test_difficulty(self):
        b, h = self.mob(40, 1865, 66.7), self.mob(40, 1865, 66.7, diff='tactician')
        self.assertEqual(h['ToHit'], b['ToHit'] + 2)
        self.assertAlmostEqual(h['MaxHp'], b['MaxHp'] * 1.3, delta=1)

    def test_boosts(self):
        n = self.mob(1, 42, 2.1, n=1)
        s, bonus = self.M.Boosts(n, 'wolf', 11, 7.0, None)
        self.assertIn('DamageBonus(%d)' % bonus, s)
        self.assertGreaterEqual(7.0 + bonus, 1)     # BG3's AI needs an average hit
        self.assertNotIn('(0)', s)

    def test_low_level_floor(self):
        n = self.mob(1, 42, 2.1, n=1)
        self.assertEqual(n['AC'], 12)
        self.assertEqual(n['ToHit'], 3)


if __name__ == '__main__':
    unittest.main()
