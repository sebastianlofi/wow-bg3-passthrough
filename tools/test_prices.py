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


GOLD = r"""
tick, pending, reqs, sets, adds = 100, {}, {}, 0, 0
gold, blocked = 1000, false
M, goldOwed, goldTried = {Barter = {}, RefundOf = {}, CopperPerGold = 1, Request = function(r) reqs[#reqs + 1] = r end}, {}, {}
BARTER_WAIT, BARTER_OPS = 600, {buy = true, buypart = true, sell = true}
function goldOf() return gold end
function setGold(_, want) sets = sets + 1; if not blocked then gold = want end end
Osi = {AddGold = function(_, n) adds = adds + n; gold = gold + n end}
"""


class Gold(unittest.TestCase):
    """Inventory.lua: BG3's gold and WoW's money after a barter, a refusal, an open trade."""

    def setUp(self):
        self.L = L = lupa.LuaRuntime(unpack_returned_tuples=True)
        L.execute(GOLD)
        text = (LUA / 'Inventory.lua').read_text()
        L.execute('function look(uuid, m)\n' + section(text, "    -- Gold: WoW's money", 'function M.Reconcile(inv)'))

    def look(self, money):
        self.L.execute('look("hero", {Money = %d})' % money)

    def test_a_barter_pays_the_screen_s_price(self):
        self.L.execute('gold = 928; M.Barter.hero = {At = 100}')
        self.look(900)  # WoW charged its 100, the screen said 72
        self.assertEqual(self.L.eval('reqs[1].Delta'), 28)
        self.look(900)  # the reply is not in yet: not asked twice
        self.assertEqual(self.L.eval('#reqs'), 1)
        self.look(928)
        self.assertEqual(self.L.eval('M.Barter.hero'), None)

    def test_two_buys_in_one_barter_both_count(self):
        self.L.execute('gold = 928; M.Barter.hero = {At = 100}')
        self.look(900)
        self.L.execute('gold = 856')  # the second buy, before the first's money came back
        self.look(828)  # WoW: 100 more, the first's 28 not yet: still 28 short, so nothing more to ask
        self.assertEqual(self.L.eval('#reqs'), 1)
        self.look(856)  # the 28 arrives
        self.assertEqual(self.L.eval('M.Barter.hero'), None)
        self.L.execute('gold = 784; M.Barter.hero = {At = 100}')  # a third, on its own
        self.look(756)
        self.assertEqual(self.L.eval('reqs[2].Delta'), 28)
        self.look(784)
        self.assertEqual(self.L.eval('M.Barter.hero'), None)

    def test_a_refused_buy_comes_back_in_an_open_trade(self):
        self.L.execute('blocked = true; gold = 928')  # refused: WoW's money stays 1000
        self.look(1000)
        self.assertEqual((self.L.eval('sets'), self.L.eval('adds')), (1, 0))  # GetGold lags: the set may have worked
        self.look(1000)
        self.assertEqual((self.L.eval('gold'), self.L.eval('adds')), (1000, 72))
        self.look(1000)
        self.assertEqual(self.L.eval('adds'), 72)

    def test_a_set_that_works_gets_no_add_on_top(self):
        self.L.execute('gold = 928')
        self.look(1000)
        self.look(1000)
        self.assertEqual((self.L.eval('gold'), self.L.eval('adds')), (1000, 0))

    def test_what_is_owed_stays_out_of_the_next_barter(self):
        self.L.execute('blocked = true; gold = 928; Osi.AddGold = function() end')  # not even AddGold gets through
        self.look(1000)
        self.look(1000)
        self.L.execute('gold = 856; M.Barter.hero = {At = 100}')  # the next buy, 72 on the screen; WoW takes 100
        self.look(900)
        self.assertEqual(self.L.eval('reqs[1].Delta'), 28)  # 928, not 856
        self.look(928)
        self.assertEqual(self.L.eval('M.Barter.hero'), None)
        self.L.execute('blocked = false')  # the trade is over
        self.look(928)
        self.look(928)
        self.assertEqual(self.L.eval('gold'), 928)

    def test_gold_above_wow_s_money_waits_for_the_end_of_the_trade(self):
        self.L.execute('blocked = true; gold = 1100')
        for _ in range(3):
            self.look(1000)
        self.assertEqual((self.L.eval('gold'), self.L.eval('adds')), (1100, 0))
        self.L.execute('blocked = false')
        self.look(1000)
        self.assertEqual(self.L.eval('gold'), 1000)


class Buying(unittest.TestCase):
    """Inventory.lua: what counts as a buy."""

    def setUp(self):
        self.L = L = lupa.LuaRuntime(unpack_returned_tuples=True)
        text = (LUA / 'Inventory.lua').read_text()
        L.execute("""
          bought, tags, stacks, entries = {}, {}, {}, {}
          function uuidOf(s) return s end
          function suppressed() return false end
          function tagOf(u) return tags[u] end
          function entryOf(u) return entries[u] end
          function q(f, ...) local ok, v = pcall(f, ...); if ok then return v end end
          function askFor(u, tag, n) bought[#bought + 1] = {u, tag, n} end
          Osi = {GetStackAmount = function(u) return stacks[u] end}
          M = {SideTag = {}}
          lastInv = {Vendors = {f1 = {{Idx = 4, Entry = 414}}}}
          picking, pieceOwner, vendorOf, corpseOf, trading = {}, {}, {f = "f1", s = "f1"}, {}, {f = 50}
          tick = 100
          function buying(u) return trading[u] ~= nil and tick >= trading[u] end
        """ + section(text, 'local function wareTag', 'local function notePieces').replace('local function', 'function')
                  + section(text, 'local function onRemovedFrom', 'local function research').replace('local function', 'function'))

    def test_a_ware_leaving_an_open_trade_is_a_buy(self):
        self.L.execute('tags.w = "Vf1:4"; stacks.w = 5; onRemovedFrom("w", "f")')
        self.assertEqual(self.L.eval('bought[1][3]'), 5)

    def test_a_ware_leaving_a_stand_in_with_no_trade_is_not(self):
        self.L.execute('tags.w = "Vf1:4"; stacks.w = 5; onRemovedFrom("w", "s")')
        self.assertEqual(self.L.eval('#bought'), 0)

    def test_a_trade_still_opening_clears_its_stock_and_buys_nothing(self):
        self.L.execute('trading.f = 145; tags.w = "Vf1:4"; stacks.w = 5; onRemovedFrom("w", "f")')
        self.assertEqual(self.L.eval('#bought'), 0)

    def test_the_settle_rule_is_the_one_tested(self):
        text = (LUA / 'Inventory.lua').read_text()
        self.assertIn('local function buying(standin) return trading[standin] ~= nil and tick >= trading[standin] end', text)
        self.assertIn('trading[standin] = tick + TRADE_SETTLE', text)

    def test_a_part_with_no_tag_leaving_is_no_buy_of_its_own(self):
        self.L.execute('entries.p = 414; stacks.p = 2; onRemovedFrom("p", "f")')
        self.assertEqual(self.L.eval('#bought'), 0)  # settleWares counts it


class Parts(unittest.TestCase):
    """Inventory.lua settleWares: part of a ware bought shows as its stack getting smaller."""

    def setUp(self):
        self.L = L = lupa.LuaRuntime(unpack_returned_tuples=True)
        text = (LUA / 'Inventory.lua').read_text()
        L.execute("""
          reqs, stacks, asked, trading, vendorOf = {}, {w = 5, a = 1}, {}, {f = 50}, {f = "f1"}
          tick = 100
          lastInv = {Vendors = {f1 = {{Idx = 0, Entry = 414, Price = 125}, {Idx = 1, Entry = 2512, Price = 10}}}}
          function ours() return {["Vf1:0"] = "w", ["Vf1:1"] = "a"} end
          function holderOf() return "f" end
          function q(f, ...) local ok, v = pcall(f, ...); if ok then return v end end
          Osi = {GetStackAmount = function(u) return stacks[u] end}
          Hero = {Uuid = function() return "hero" end}
          M = {Barter = {}, Request = function(r) reqs[#reqs + 1] = r end,
               Item = function(e) if e == 414 then return {bc = 5} end return {bc = 200} end}
        """ + section(text, 'local wareSeen, partBought', 'M.SettleWares = settleWares').replace('local function', 'function'))

    def test_a_first_look_buys_nothing(self):
        self.L.execute('settleWares(); settleWares()')
        self.assertEqual(self.L.eval('#reqs'), 0)

    def test_part_of_a_stack(self):
        self.L.execute('settleWares(); stacks.w = 3; settleWares(); settleWares()')
        self.assertEqual(self.L.eval('#reqs'), 1)
        self.assertEqual((self.L.eval('reqs[1].Op'), self.L.eval('reqs[1].Count'), self.L.eval('reqs[1].Lot')), ('buypart', 2, 5))
        self.assertEqual(self.L.eval('M.Barter.hero.At'), 100)

    def test_a_ware_bg3_will_not_stack_is_not_bought_for_being_short(self):
        self.L.execute('settleWares(); settleWares(); settleWares()')  # arrows: 1 where WoW's lot is 200
        self.assertEqual(self.L.eval('#reqs'), 0)

    def test_not_before_the_trade_has_settled(self):
        self.L.execute('trading.f = 200; settleWares(); stacks.w = 3; settleWares()')
        self.assertEqual(self.L.eval('#reqs'), 0)


class Merchant(unittest.TestCase):
    """Inventory.lua: a merchant's gold is there during its trade only. Its amount: test_stock.py."""

    def setUp(self):
        self.L = L = lupa.LuaRuntime(unpack_returned_tuples=True)
        L.execute("""
          sold, trading, vendorOf, corpseOf = {}, {}, {}, {}
          function goldOf(u) return sold[u] or 0 end
          function setGold(u, n) sold[u] = n end
          function fundMerchant(u) setGold(u, 500) end
          function pass()
            for standin in pairs(vendorOf) do
              if trading[standin] then fundMerchant(standin)
              elseif not corpseOf[standin] and goldOf(standin) > 0 then setGold(standin, 0) end
            end
          end
        """)

    def test_open_funded_closed_emptied(self):
        L = self.L
        L.execute('vendorOf.farley = "f1"; pass()')
        self.assertEqual(L.eval('goldOf("farley")'), 0)
        L.execute('trading.farley = true; pass()')
        self.assertEqual(L.eval('goldOf("farley")'), 500)
        L.execute('trading.farley = nil; pass()')
        self.assertEqual(L.eval('goldOf("farley")'), 0)

    def test_a_corpse_keeps_its_loot_money(self):
        self.L.execute('vendorOf.dead = "f2"; corpseOf.dead = "f2"; sold.dead = 37; pass()')
        self.assertEqual(self.L.eval('goldOf("dead")'), 37)

    def test_the_real_pass_is_the_one_tested(self):
        text = (LUA / 'Inventory.lua').read_text()
        self.assertIn("""        if trading[standin] then fundMerchant(standin)
        elseif not corpseOf[standin] and goldOf(standin) > 0 then setGold(standin, 0) end""", text)


if __name__ == '__main__':
    unittest.main()
