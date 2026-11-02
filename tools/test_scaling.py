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

    def mob(self, level, wow_hp, hit, cap=18, diff='balanced', map_=0, **party):
        c = self.lua.table_from({'Level': level, 'WowMaxHp': wow_hp, 'WowDamage': hit, 'Map': map_,
                                 'Cap': cap, 'Difficulty': diff, **party})
        return self.M.Compute(c)

    def test_table(self):
        self.assertAlmostEqual(self.M.Effective(60, 18), 18)
        self.assertAlmostEqual(self.M.Effective(4, 18), 1)

    def test_tables_have_their_lengths(self):
        self.assertEqual(len(self.M.NORMAL_HP), 63)
        self.assertEqual(len(self.M.NORMAL_HIT), 63)
        self.assertEqual(len(self.M.MEMBER_HP), 18)

    def test_normal_mob_at_60(self):
        n = self.mob(60, 4059, 120.9)
        self.assertEqual(n['Rank'], 'normal')
        self.assertEqual(n['E'], 18)
        self.assertEqual(n['MaxHp'], 184)     # (6 + 1.37 x 18) x 6
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

    def test_nothing_of_the_party_goes_in(self):
        base = self.mob(40, 1865, 66.7)
        for party in ({'Members': 1}, {'Camp': 14}, {'PartyLevel': 1}, {'WowLevel': 60}, {'WowLevel': 1, 'PartyLevel': 20, 'Members': 2}):
            other = self.mob(40, 1865, 66.7, **party)
            for k in ('E', 'MaxHp', 'Damage', 'AC', 'ToHit', 'DC', 'Save'):
                self.assertEqual(other[k], base[k], (k, party))

    def test_normal_hp_only_rises_with_level(self):
        hp = [self.mob(w, self.M.NORMAL_HP[w], self.M.NORMAL_HIT[w])['MaxHp'] for w in range(1, 61)]
        self.assertEqual(hp, sorted(hp))

    def test_a_mobs_numbers_follow_its_own_level(self):
        low, high = self.mob(10, 187, 14), self.mob(50, 2769, 98.2)
        self.assertLess(low['E'], high['E'])
        self.assertLess(low['MaxHp'], high['MaxHp'])
        self.assertLess(low['AC'], high['AC'])

    def test_a_hit_under_one_comes_off_to_hit(self):
        weak = self.mob(1, 42, 0.02)
        self.assertLess(weak['Damage'], 1)
        self.assertEqual(weak['ToHit'], 3 - round(9 * (1 - weak['Damage'])))
        self.assertEqual(self.mob(60, 4059, 120.9)['ToHit'], 12)   # a real hit: nothing comes off

    def test_difficulty(self):
        b, h = self.mob(40, 1865, 66.7), self.mob(40, 1865, 66.7, diff='tactician')
        self.assertEqual(h['ToHit'], b['ToHit'] + 2)
        self.assertAlmostEqual(h['MaxHp'], b['MaxHp'] * 1.3, delta=1)

    def test_boosts(self):
        n = self.mob(1, 42, 2.1)
        s, bonus = self.M.Boosts(n, 'wolf', 11, 7.0, None)
        self.assertIn('DamageBonus(%d)' % bonus, s)
        self.assertGreaterEqual(7.0 + bonus, 1)     # BG3's AI needs an average hit
        self.assertNotIn('(0)', s)

    def test_low_level_floor(self):
        n = self.mob(1, 42, 2.1)
        self.assertEqual(n['AC'], 12)
        self.assertEqual(n['ToHit'], 3)


if __name__ == '__main__':
    unittest.main()
