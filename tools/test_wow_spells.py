#!/usr/bin/env python3
"""test_wow_spells.py: our spell stats hold together, and the switch's rules, without a game. WowSpells.lua runs
as it is under a fake game (needs the `lupa` module).

  python3 tools/test_wow_spells.py
"""
import re, unittest
from pathlib import Path

MOD = Path(__file__).resolve().parent.parent / 'mod'
DATA = MOD / 'Public/WOWBG3/Stats/Generated/Data'
try:
    import lupa
except ImportError:
    print('test_wow_spells.py: needs the lupa module (pip install lupa); skipped')
    raise SystemExit(0)

FAKE = r'''
Ext = {
  IO = {LoadFile = function() return nil end},
  Json = {Parse = function() error("no json") end},
  Require = function() return {UuidOf = function(s) return s end} end,
  Utils = {Print = function() end},
}
'''


def entries():
    out = {}
    for f in DATA.glob('*.txt'):
        for chunk in re.split(r'\n(?=new entry )', f.read_text()):
            m = re.match(r'new entry "([^"]+)"', chunk)
            out[m.group(1)] = chunk
    return out


class Stats(unittest.TestCase):
    def test_no_empty_file(self):
        self.assertEqual(sorted(f.name for f in DATA.glob('*.txt')), ['Passive_WowBg3.txt', 'Spell_WowBg3.txt', 'Status_WowBg3.txt'])
        for f in DATA.glob('*.txt'):
            self.assertTrue(f.read_text().strip())

    def test_references(self):
        e = entries()
        text = '\n'.join(e.values())
        for s in re.findall(r'ApplyStatus\((?:SWAP,)?(WOWBG3_\w+)', text):
            self.assertIn(s, e)
        for s in re.findall(r'UnlockSpell\((\w+)', text):
            self.assertIn(s, e)
        for s in re.findall(r'data "Passives" "(\w+)"', text):
            self.assertIn(s, e)


class Rules(unittest.TestCase):
    def setUp(self):
        self.lua = lupa.LuaRuntime()
        self.lua.execute(FAKE)
        self.m = self.lua.execute((MOD / 'ScriptExtender/Lua/WowSpells.lua').read_text())

    def wanted(self, levels, on=True):
        t = self.lua.table_from(levels)
        return sorted(self.m.Wanted(t, on).keys())

    def test_rows_exist(self):
        e = entries()
        for levels in self.m.ROWS.values():
            for ps in levels.values():
                for p in ps.values():
                    self.assertIn(p, e)

    def test_off_gives_nothing(self):
        self.assertEqual(self.wanted({'Wizard': 12}, False), [])

    def test_by_level(self):
        self.assertEqual(self.wanted({'Wizard': 1}), [])
        self.assertEqual(self.wanted({'Wizard': 2}), ['WowBg3_Unlock_FrostArmor'])
        self.assertEqual(self.wanted({'Sorcerer': 3}), ['WowBg3_Unlock_Conjure', 'WowBg3_Unlock_FrostArmor', 'WowBg3_Unlock_Nova_Cha'])
        self.assertIn('WowBg3_Unlock_IceBarrier', self.wanted({'Wizard': 5}))
        self.assertEqual(self.wanted({'Fighter': 12}), [])

    def test_conjured_by_wow_level(self):
        f = self.m.ConjuredFor
        self.assertEqual(tuple(f(1)), (5349, 5350))
        self.assertEqual(tuple(f(14)), (1113, 2288))
        self.assertEqual(tuple(f(60)), (22895, 8079))


if __name__ == '__main__':
    unittest.main()
