#!/usr/bin/env python3
"""test_item_strings.py: BG3's string table holds about 28,000 new handles (bg3le never reallocates it), so an
item's strings go in when it is made, not all 170,772 at a load. Inventory.lua's parts run as they are under a
fake game (needs the `lupa` module).

  python3 tools/test_item_strings.py
"""
import unittest
from pathlib import Path

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua'
try:
    import lupa
except ImportError:
    print('test_item_strings.py: needs the lupa module (pip install lupa); skipped')
    raise SystemExit(0)

ROOM = 28000  # what bg3le's refusals in a fresh game's log leave: 170772 asked, 142403 refused


def section(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


class Strings(unittest.TestCase):
    def setUp(self):
        self.text = (LUA / 'Inventory.lua').read_text()
        self.L = lupa.LuaRuntime(unpack_returned_tuples=True)
        self.L.execute("""
            strings = {}
            client = false
            Ext = {Utils = {MonotonicTime = function() return 0 end}, Log = {Print = function() end},
                   Loca = {UpdateTranslatedString = function(h, t) strings[h] = t end}}
            M = {Used = {}, Props = {[7] = {n = 'of the Bear', t = 'Strength'}}, Pools = {{id = 1, p = {7}}}}
            rows = {}
            for e = 1, 17693 do rows[e] = {e = e, n = 'Item ' .. e, k = 'object', rp = 1} end
            function M.Items() return rows end
            function M.Item(e) return rows[e] end
            function loadUsed() M.Used[11] = true; M.Used[22] = true end
        """)
        self.L.execute(section(self.text, 'function M.Variant', '-- What a member wears')
                       + section(self.text, 'function M.NameHandle', '-- Entries ever used')
                       .split('\n\n')[0]
                       + '\n' + section(self.text, 'function M.MakeStrings', '-- Name and icon on a BG3 item').split('-- ---- making')[0])

    def strings(self):
        return dict(self.L.globals().strings.items())

    def test_a_load_makes_only_the_used_entries(self):
        self.L.eval('M.MakeAll()')
        self.assertEqual(sorted(self.strings()), ['WowBg3_D11', 'WowBg3_D22', 'WowBg3_N11', 'WowBg3_N22'])

    def test_a_save_full_of_items_stays_in_the_room(self):
        self.L.execute('function loadUsed() for e = 1, 13000 do M.Used[e] = true end end')
        self.L.eval('M.MakeAll()')
        self.assertLess(len(self.strings()), ROOM)

    def test_only_one_pass_per_state(self):
        self.L.eval('M.MakeAll()')
        self.L.execute('strings = {}')
        self.L.eval('M.MakeAll()')
        self.assertEqual(self.strings(), {})

    def test_a_suffix_gets_its_own_strings_when_it_is_used(self):
        self.L.eval('M.MakeStrings(M.Variant(M.Item(5), 7))')
        self.assertEqual(self.strings()['WowBg3_N5_7'], 'Item 5 of the Bear')
        self.assertEqual(self.strings()['WowBg3_D5_7'], 'Strength')

    def test_every_place_that_names_a_suffix_makes_it_first(self):
        self.assertNotIn('MakeAll(true)', self.text)
        for dressing in ('pcall(M.Dress, u, v)',):
            at = self.text.index(dressing)
            self.assertIn('pcall(M.MakeStrings, v)', self.text[at - 80:at])
        create = section(self.text, 'function M.Create', 'local rt = assert')
        self.assertIn('M.MakeStrings(row)', create)
        self.assertIn('M.MakeStrings, row', section(self.text, 'function M.Redress', 'M.Dress(u, row) then'))


if __name__ == '__main__':
    unittest.main()
