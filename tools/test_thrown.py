#!/usr/bin/env python3
"""test_thrown.py: WoW's thrown weapons as BG3's throwable dagger, without a game. The real Lua text of the parts of
Inventory.lua named below runs under a fake game (needs the `lupa` module).

  python3 tools/test_thrown.py
"""
import collections, json, sys, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
LUA = (HERE.parent / 'mod/ScriptExtender/Lua/Inventory.lua').read_text()
sys.path.insert(0, str(HERE))
import wow_items
try:
    import lupa
except ImportError:
    print('test_thrown.py: needs the lupa module (pip install lupa); skipped')
    raise SystemExit(0)

RULES = json.loads((HERE / 'wow_items_rules.json').read_text())


def section(start, end):
    i = LUA.index(start)
    return LUA[i:LUA.index(end, i)]


def thrown(name='Small Throwing Knife', price=15, count=200, stack=200, dmin=1, dmax=3, delay=2000, sell=0):
    it = collections.defaultdict(int, entry=2947, name=name, item_level=3, inventory_type=25, subclass=16, buy_price=price,
                                 buy_count=count, stackable=stack, dmg_min1=dmin, dmg_max1=dmax, delay=delay, sell_price=sell)
    it['class'] = 2
    return wow_items.map_item(RULES, it, {})


class Table(unittest.TestCase):
    def test_every_thrown_weapon_is_a_dagger(self):
        for name in ('Small Throwing Knife', 'Crude Throwing Axe', 'Silver Star'):
            row = thrown(name)
            self.assertEqual((row['b'], row['k'], row['s']), ('WPN_Dagger', 'weapon', 'Thrown'))

    def test_no_item_is_a_dart(self):
        self.assertNotIn('WPN_Dart', json.dumps(RULES['weapons']))

    def test_damage_from_wow_stays(self):
        row = thrown('Flightblade Throwing Axe', dmin=54, dmax=102, delay=2200)
        self.assertEqual(row['en'], 2)
        self.assertIn('WeaponEnchantment(2)', row['bo'])

    def test_the_price_of_one_is_as_it_was(self):
        row = thrown()  # the choice of what one BG3 dagger stands for is not made here
        self.assertEqual((row['bp'], row['bc'], row['sp']), (1, 200, 0))


FAKE = r'''
ENTRIES = {[1] = {e = 1, k = "weapon", s = "Sword", b = "WPN_Longsword"}, [2] = {e = 2, k = "weapon", s = "Thrown", b = "WPN_Dagger"},
           [3] = {e = 3, k = "weapon", s = "Bow", b = "WPN_Longbow"}, [4] = {e = 4, k = "armor", s = "Shield", b = "ARM_Shield"}}
hands, entry_of, equips, suppressed_ = {}, {}, {}, {}
M = {Item = function(e) return ENTRIES[e] end, Suppress = function(u) suppressed_[u] = true end, Props = nil,
     EffectBoosts = function() return {} end, Create = function() error("nothing is made here") end}
state = {Created = 0}
wowOwner, pendingItem, suffixed = {}, {}, {}
function note() end
function reconcileStacks() end
function keepsOwn() return false end
function holderOf() return "hero" end
function equipped(u) return hands.main == u or hands.off == u or hands.ranged == u end
function entryOf(u) return entry_of[u] end
function uuidOf(s) return s end
function q(f, ...) local ok, v = pcall(f, ...); if ok then return v end end
SLOT = {[1] = "main", [2] = "main", [3] = "ranged", [4] = "off"}
Osi = {
  GetEquippedItem = function(_, s) return s == "MeleeMainHand" and hands.main or s == "MeleeOffHand" and hands.off or nil end,
  GetStackAmount = function() return nil end,
  Equip = function(_, u) equips[#equips + 1] = u; hands[SLOT[entry_of[u]]] = u end,
  Unequip = function(_, u) for k, v in pairs(hands) do if v == u then hands[k] = nil end end end,
}
Ext = {Entity = {Get = function(u) return {Vars = {}} end}}
'''


class Hands(unittest.TestCase):
    """reconcileMember: one BG3 main hand for WoW's main hand and WoW's ranged slot."""

    def setUp(self):
        L = self.L = lupa.LuaRuntime(unpack_returned_tuples=True)
        L.execute(FAKE)
        body = section('local function reconcileMember', '    reconcileStacks(uuid, stacks, have, m.Items)')
        L.execute(section('function M.ThrownWaits', 'local function reconcileMember')
                  + body.replace('local function reconcileMember', 'function reconcileMember') + '\nend')
        L.execute('entry_of = {sword = 1, knife = 2, bow = 3, shield = 4}')

    def go(self, items, passes=3):
        lua = ', '.join('{Guid = "%s", Entry = %d, Bag = 255, Slot = %d}' % i for i in items)
        self.L.execute('''
          local have = {sword = "sword", knife = "knife", bow = "bow", shield = "shield"}
          for _ = 1, %d do reconcileMember("hero", {Items = {%s}}, have) end''' % (passes, lua))

    def equips(self):
        return list(self.L.globals().equips.values())

    def test_wow_sword_and_knife_do_not_fight_for_the_hand(self):
        self.go([('sword', 1, 15), ('knife', 2, 17)], 5)
        self.assertEqual(self.equips(), ['sword'])
        self.assertEqual(self.L.eval('hands.main'), 'sword')

    def test_order_of_wows_list_does_not_matter(self):
        self.go([('knife', 2, 17), ('sword', 1, 15)], 5)
        self.assertEqual(self.equips(), ['sword'])

    def test_knife_alone_goes_in_the_hand(self):
        self.go([('knife', 2, 17)])
        self.assertEqual(self.L.eval('hands.main'), 'knife')

    def test_shield_in_wow_does_not_hold_the_knife_back(self):
        self.go([('shield', 4, 16), ('knife', 2, 17)])
        self.assertEqual(self.L.eval('hands.main'), 'knife')
        self.assertEqual(self.L.eval('hands.off'), 'shield')

    def test_sword_put_on_in_wow_leaves_wows_ranged_slot_alone(self):
        self.L.execute('hands.main = "knife"')
        self.go([('sword', 1, 15), ('knife', 2, 17)])
        self.assertEqual(self.L.eval('hands.main'), 'sword')
        self.assertTrue(self.L.eval('suppressed_.knife'))  # no unequip goes to WoW for it

    def test_a_bow_keeps_its_own_slot(self):
        self.go([('sword', 1, 15), ('bow', 3, 17)])
        self.assertEqual(sorted(self.equips()), ['bow', 'sword'])


class OldSave(unittest.TestCase):
    """M.Redress: a thrown weapon saved in the dart's form is made again from WoW's list."""

    def setUp(self):
        L = self.L = lupa.LuaRuntime(unpack_returned_tuples=True)
        L.execute(FAKE)
        L.execute(r'''
          PREFIX_STATS = "WOWBG3_I_"; ENTRY_VAR = "WowBg3Entry"; BASE_VAR = "WowBg3Base"
          deleted, dressed = {}, {}
          M.Dress = function(u) dressed[#dressed + 1] = u; return true end
          M.Used, M.SaveUsed = {[2] = true, [1] = true}, function() end
          Osi.RequestDelete = function(u) deleted[#deleted + 1] = u end
          math.type = math.type or function() return "integer" end
          OURS = {old = {Vars = {WowBg3Entry = 2}, Data = {StatsId = "WOWBG3_I_2"}},
                  new = {Vars = {WowBg3Entry = 2, WowBg3Base = "WPN_Dagger"}, Data = {StatsId = "WOWBG3_I_2"}},
                  sword = {Vars = {WowBg3Entry = 1}, Data = {StatsId = "WOWBG3_I_1"}}}
          function eachOurs(f) for u, e in pairs(OURS) do f(u, "tag", e) end end
        ''')
        L.execute(section('function M.OldThrown', '\n') + '\n' +
                  section('function M.Redress()', 'M.Fresh = {}'))

    def test_only_the_old_thrown_form_goes(self):
        self.L.execute('M.Remade = 0; M.Redress()')
        self.assertEqual(list(self.L.globals().deleted.values()), ['old'])
        self.assertEqual(sorted(self.L.globals().dressed.values()), ['new', 'sword'])
        self.assertTrue(self.L.eval('suppressed_.old'))  # its leaving the hand is no unequip for WoW


if __name__ == '__main__':
    unittest.main()
