#!/usr/bin/env python3
"""test_inventory_moves.py: an item moved between the party's inventories stays where the player put it (the maintainer,
2026-11-14: "changing items between inventories should be fully supported and should respect the placement within
them"). Food moved to another member had been deleted with the giver's stack and made again where BG3 sends new food
(the Camp Supply Sack), and a part split off a stack went back to its owner. Inventory.lua's onReplies, returnPiece and
movePiece run as they are under a fake game (needs the `lupa` module).

  python3 tools/test_inventory_moves.py
"""
import sys, unittest
from pathlib import Path

try:
    import lupa
except ImportError:
    print('test_inventory_moves.py: needs the lupa module (pip install lupa); skipped')
    sys.exit(0)

LUA = Path(__file__).resolve().parent.parent / 'mod' / 'ScriptExtender' / 'Lua'
TAV, GALE = 'tav-0000-0000-0000-000000000001', 'gale-000-0000-0000-000000000002'


def section(text, start, end):
    i = text.index(start)
    return text[i:text.index(end, i)]


FAKE = r'''
VAR = "WowBg3Item"
tick, runId = 100, "run"
vars, amounts, deleted, reqs, printed = {}, {}, {}, {}, {}
pending, pendingItem, parkWait, pieceOwner, suppress = {}, {}, {}, {}, {}
state = {}
M = {Barter = {}, Refused = {}, Item = function(e) if e == 159 then return {k = "object"} end return {k = "armor"} end}
M.Suppress = function(u) suppress[u] = true end
M.Request = function(r) reqs[#reqs + 1] = r end
function q(f, ...) local ok, v = pcall(f, ...); if ok then return v end end
function uuidOf(s) return s end
function entryOf(u) return 159 end
function stackTag(e, m) return "S" .. tostring(e) .. "@" .. m end
function ours() local o = {} for u, t in pairs(vars) do if not deleted[u] then o[t] = u end end return o end
function noteSold() end
function tellRefused() end
function note() end
function setParked() end
function dropPiece(u) pieceOwner[u] = nil; deleted[u] = true end
BARTER_OPS = {}
Ext = {Entity = {Get = function(u) if deleted[u] then return nil end return {Vars = setmetatable({}, {
          __index = function(_, k) return vars[u] end, __newindex = function(_, k, v) vars[u] = v end})} end},
       Utils = {Print = function(s) printed[#printed + 1] = s end}}
Osi = {GetStackAmount = function(u) return amounts[u] end, SetStackAmount = function(u, n) amounts[u] = n end,
       RequestDelete = function(u) deleted[u] = true end, ShowNotification = function() end}
lastInv = {Members = {["%s"] = {Items = {{Guid = "10", Entry = 159, Count = 4}}}, ["%s"] = {Items = {}}}}
''' % (TAV, GALE)


class Moves(unittest.TestCase):
    def setUp(self):
        self.L = L = lupa.LuaRuntime(unpack_returned_tuples=True)
        L.execute(FAKE)
        text = (LUA / 'Inventory.lua').read_text()
        L.execute(section(text, 'local function onReplies', '-- ---- vendors and corpses').replace('local function', 'function'))
        L.execute(section(text, 'local function returnPiece', '-- A vendor ware or a corpse').replace('local function', 'function'))

    def reply(self, r, rep):
        L = self.L
        L.execute('pending[%d] = %s' % (rep['Seq'], r))
        L.globals().inv_ = L.eval('{Replies = {{Seq = %d, RunId = "run", Ok = %s, Guid = %s}}}' % (
            rep['Seq'], 'true' if rep['Ok'] else 'false', '"%s"' % rep['Guid'] if rep.get('Guid') else 'nil'))
        L.execute('onReplies(inv_)')

    def test_a_whole_stack_given_is_the_receivers_where_it_lies(self):
        L = self.L
        L.execute('vars.water = "S159@%s"; amounts.water = 4' % TAV)
        self.reply('{Op = "give", Member = "%s", To = "%s", Guid = "10", Item = "water"}' % (TAV, GALE), {'Seq': 1, 'Ok': True})
        self.assertEqual(L.eval('vars.water'), 'S159@' + GALE)
        self.assertIsNone(L.eval('deleted.water'))

    def test_a_whole_stack_joins_the_receivers_own(self):
        L = self.L
        L.execute('vars.water = "S159@%s"; amounts.water = 4; vars.gwater = "S159@%s"; amounts.gwater = 2' % (TAV, GALE))
        self.reply('{Op = "give", Member = "%s", To = "%s", Guid = "10", Item = "water"}' % (TAV, GALE), {'Seq': 1, 'Ok': True})
        self.assertEqual(L.eval('amounts.gwater'), 6)
        self.assertTrue(L.eval('deleted.water'))

    def test_a_part_moved_goes_in_wow_and_stays(self):
        L = self.L
        L.execute('amounts.part = 1; pieceOwner.part = "%s"; movePiece("part", "%s", "%s")' % (TAV, TAV, GALE))
        ops = [(L.eval('reqs[%d].Op' % i), L.eval('reqs[%d].Member' % i)) for i in (1, 2)]
        self.assertEqual(ops, [('del', TAV), ('add', GALE)])
        self.assertEqual(L.eval('reqs[2].Count'), 1)
        self.assertIsNone(L.eval('deleted.part'))
        self.assertEqual(L.eval('pieceOwner.part'), GALE)
        # WoW said yes: the part is Gale's stack where it lies (none of hers yet)
        L.execute('pending[2] = reqs[2]')
        L.globals().inv_ = L.eval('{Replies = {{Seq = 2, RunId = "run", Ok = true, Guid = "11"}}}')
        L.execute('onReplies(inv_)')
        self.assertEqual(L.eval('vars.part'), 'S159@' + GALE)


if __name__ == '__main__':
    unittest.main()
