#!/usr/bin/env python3
"""test_prices.py: what BG3's trade screen shows for WoW's prices, without a game. Prices.lua and the price
parts of Inventory.lua run as they are, the second under a fake game (needs the `lupa` module).

  python3 tools/test_prices.py
"""
import unittest
from pathlib import Path

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua'
try:
    import lupa
except ImportError:
    print('test_prices.py: needs the lupa module (pip install lupa); skipped')
    raise SystemExit(0)


def section(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


class Rule(unittest.TestCase):
    """Prices.lua: BG3's formula and the values that make it show WoW's numbers."""

    @classmethod
    def setUpClass(cls):
        cls.L = lupa.LuaRuntime(unpack_returned_tuples=True)
        cls.P = cls.L.execute((LUA / 'Prices.lua').read_text())

    def test_his_two_measurements(self):
        self.assertEqual(self.P.Bought(200, 2.5), 500)   # the shank: value 200 showed 500
        self.assertEqual(self.P.Bought(5, 2.5) * 5, 65)  # five jerky at value 5 showed 65: 13 each, not 62.5

    def test_every_sell_price_is_exact_on_every_difficulty(self):
        for base in (1, 1.3, 2, 2.5, 3, 3.7, 4):  # Custom goes 1 to 4 in steps of 0.1
            for sp in range(1, 20001):
                self.assertEqual(self.P.Sold(self.P.Held(sp, 1, base), base), sp, (base, sp))

    def test_a_buy_price_is_as_close_as_a_whole_value_can_be(self):
        for base in (2, 2.5, 3):
            for bp in range(2, 20001):
                self.assertLessEqual(abs(self.P.Bought(self.P.Ware(bp, base), base) - bp), 1, (base, bp))
        # What BG3 can show at 2.5 is exact, so a price it can show comes out as WoW's.
        for bp in (3, 5, 8, 10, 13, 500, 1000, 12345 - 12345 % 5):
            self.assertEqual(self.P.Bought(self.P.Ware(bp, 2.5), 2.5), bp)
        self.assertEqual(self.P.Bought(self.P.Ware(1, 2.5), 2.5), 3)  # the lowest value there is
        for bp in range(1, 3000):  # nothing closer exists
            shown = self.P.Bought(self.P.Ware(bp, 2.5), 2.5)
            best = min(abs(self.P.Bought(v, 2.5) - bp) for v in range(1, bp + 3))
            self.assertEqual(abs(shown - bp), best, bp)

    def test_no_sell_price_no_money(self):
        self.assertEqual(self.P.Held(0, 500, 2.5), 0)
        self.assertEqual(self.P.Sold(0, 2.5), 0)  # BG3 gives a value of 0 a price of 0, not its minimum of 1

    def test_no_buy_price_stays_zero(self):
        self.assertEqual(self.P.Ware(0, 2.5), 0)
        self.assertEqual(self.P.Held(0, 0, 2.5), 0)
        self.assertEqual(self.P.Held(100, 0, 2.5), 0)
        self.assertEqual(self.P.Bought(0, 2.5), 0)

    def test_attitude_and_persuasion_move_both_from_the_base(self):
        ware, held = self.P.Ware(1000, 2.5), self.P.Held(250, 1000, 2.5)
        self.assertEqual((self.P.Bought(ware, 2.5), self.P.Sold(held, 2.5)), (1000, 250))
        p = self.P.Multiplier(2.5, 5, 40)  # 2.5 - 0.5 - 0.2
        self.assertAlmostEqual(p, 1.8)
        self.assertEqual(self.P.Bought(ware, p), 720)
        self.assertEqual(self.P.Sold(held, p), 347)
        self.assertEqual(self.P.Multiplier(2.5, 30, 100), 1)  # BG3's floor
        self.assertAlmostEqual(self.P.Multiplier(2.5, 0, -100), 3.0)  # a poor attitude costs more

    def test_minimum_of_one_each(self):
        self.assertEqual(self.P.Sold(1, 2.5), 1)
        self.assertEqual(self.P.Bought(1, 1), 1)


FAKE = r'''
files = {}
ITEMS = {
  [100] = {e = 100, bp = 1000, sp = 200},
  [101] = {e = 101, bp = 0, sp = 0},
  [102] = {e = 102, bp = 500, sp = 0},
}
ents, holder, entry, replicated = {}, {}, {}, 0
function item(u, value, h, e, tag)
  ents[u] = {Value = {Value = value}, Tag = tag, Replicate = function(_, name)
    if name == "Value" then replicated = replicated + 1 end end}
  holder[u], entry[u] = h, e
end
base = 2.5
Osi = {GetRulesetModifierFloat = function(name)
  if not name:find("f52ca62e", 1, true) then error("wrong modifier") end
  if base == "broken" then error("no query") end
  return base
end}
Ext = {Entity = {Get = function(u) return ents[u] end}}
function q(fn, ...) local ok, v = pcall(fn, ...); if ok then return v end end
function holderOf(u) return holder[u] end
function entryOf(u) return entry[u] end
function eachOurs(f) for u, e in pairs(ents) do if e.Tag then f(u, e.Tag, e) end end end
state, vendorOf = {}, {vendor = "f00"}
WOWBG3MirrorOwned = {vendor = "f00", shut = "f01"}
M = {CopperPerGold = 1, Item = function(e) return ITEMS[e] end}
'''


class Items(unittest.TestCase):
    """Inventory.lua: each BG3 item holds the value of the side it is on."""

    def setUp(self):
        self.L = L = lupa.LuaRuntime(unpack_returned_tuples=True)
        L.execute(FAKE)
        L.globals().Prices = L.execute((LUA / 'Prices.lua').read_text())
        text = (LUA / 'Inventory.lua').read_text()
        L.execute(section(text, 'local TRADE_MODIFIER', 'local makeStats -- below')
                  + section(text, "-- A merchant's: in a vendor", '-- Food and drink spent in BG3')
                  + '\nM.PriceItem = priceItem')

    def value(self, u):
        return self.L.eval('ents["%s"].Value.Value' % u)

    def test_a_ware_shows_the_buy_price_and_the_party_s_copy_the_sell_price(self):
        self.L.execute('''
          item("ware", 0, "vendor", 100, "Vf00:1")
          item("mine", 0, "hero", 100, "S100@hero")
          M.SetPrices()
        ''')
        self.assertEqual((self.value('ware'), self.value('mine')), (400, 500))
        self.assertEqual(self.L.eval('Prices.Bought(ents.ware.Value.Value, 2.5)'), 1000)
        self.assertEqual(self.L.eval('Prices.Sold(ents.mine.Value.Value, 2.5)'), 200)

    def test_a_bought_ware_sells_back_at_the_sell_price(self):
        self.L.execute('''
          item("ware", 400, "vendor", 100, "Vf00:1")
          holder.ware = "hero"
          M.PriceItem("ware", "hero", "Vf00:1") -- onAddedTo: before the reply and the retag
        ''')
        self.assertEqual(self.value('ware'), 500)

    def test_an_item_sold_or_a_refused_ware_changes_side_again(self):
        self.L.execute('''
          item("sword", 500, "hero", 100, "123")
          M.PriceItem("sword", "vendor", "123")
        ''')
        self.assertEqual(self.value('sword'), 400)
        self.L.execute('M.PriceItem("sword", "hero", "123")')
        self.assertEqual(self.value('sword'), 500)

    def test_a_ware_on_a_stand_in_not_opened_yet_is_a_ware(self):
        self.L.execute('item("ware", 500, "shut", 100, "Vf01:1"); M.SetPrices()')
        self.assertEqual(self.value('ware'), 400)
        self.L.execute('item("new", 500, nil, 100, "Vf00:2"); M.SetPrices()')  # made, not moved yet
        self.assertEqual(self.value('new'), 400)

    def test_loot_is_the_party_s(self):
        self.L.execute('item("loot", 0, "corpse", 100, "Lf02:1"); M.SetPrices()')
        self.assertEqual(self.value('loot'), 500)

    def test_a_split_piece_has_no_tag_and_goes_by_its_holder(self):
        self.L.execute('''
          item("p1", 0, "vendor", 100); M.PriceItem("p1", "vendor")
          item("p2", 0, "hero", 100); M.PriceItem("p2", "hero")
        ''')
        self.assertEqual((self.value('p1'), self.value('p2')), (400, 500))

    def test_an_old_save_s_values_are_put_right(self):
        self.L.execute('''
          item("old", 400, "hero", 100, "123")      -- bought when one value stood for both
          item("w", 500, "vendor", 100, "Vf00:1")
          M.SetPrices()
        ''')
        self.assertEqual((self.value('old'), self.value('w')), (500, 400))

    def test_no_sell_price_shows_nothing_and_no_buy_price_stays(self):
        self.L.execute('''
          item("a", 9, "hero", 102, "1"); item("b", 9, "vendor", 102, "Vf00:3")
          item("c", 9, "hero", 101, "2"); item("d", 9, "vendor", 101, "Vf00:4")
          M.SetPrices()
        ''')
        self.assertEqual([self.value(u) for u in 'abcd'], [0, 200, 0, 0])

    def test_the_difficulty_is_the_save_s(self):
        self.L.execute('item("w", 0, "vendor", 100, "Vf00:1"); item("m", 0, "hero", 100, "1")')
        for base, ware, held in ((2, 500, 400), (3, 333, 600), (2.5, 400, 500)):
            self.L.globals().base = base
            self.L.execute('M.SetPrices()')
            self.assertEqual((self.value('w'), self.value('m')), (ware, held), base)
            self.assertEqual(self.L.eval('state.TradeBase'), base)
        self.L.execute('base = 3; M.SetPrices(); base = "broken"; M.SetPrices()')  # a failed read keeps the last
        self.assertEqual(self.value('m'), 600)

    def test_replicated_only_when_it_changes(self):
        self.L.execute('item("m", 0, "hero", 100, "1"); M.SetPrices(); M.SetPrices(); M.SetPrices()')
        self.assertEqual(self.L.eval('replicated'), 1)

    def test_an_item_that_is_not_ours_is_left_alone(self):
        self.L.execute('item("bg3", 77, "hero", nil); M.PriceItem("bg3", "hero")')
        self.assertEqual(self.value('bg3'), 77)
        self.assertEqual(self.L.eval('replicated'), 0)

    def test_one_stats_entry_still_names_an_item(self):
        text = (LUA / 'Inventory.lua').read_text()
        self.assertEqual(text.count('PREFIX_STATS .. tostring(entry)'), 1)  # M.StatsName: no second kind of entry
        self.assertNotIn('TRADE_MARKUP', text)


if __name__ == '__main__':
    unittest.main()
