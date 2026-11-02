#!/usr/bin/env python3
"""test_stock.py: a vendor's stock and purse until the next long rest, without a game. Stock.lua and the parts of
Inventory.lua named below run as they are under a fake game (needs the `lupa` module).

  python3 tools/test_stock.py
"""
import json, sys, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
LUA = HERE.parent / 'mod/ScriptExtender/Lua'
sys.path.insert(0, str(HERE))
try:
    import lupa
except ImportError:
    print('test_stock.py: needs the lupa module (pip install lupa); skipped')
    raise SystemExit(0)
import inventory_bridge as I  # noqa: E402


def section(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def rt():
    return lupa.LuaRuntime(unpack_returned_tuples=True)


class Rule(unittest.TestCase):
    def setUp(self):
        self.L = rt()
        self.L.globals().S = self.L.execute((LUA / 'Stock.lua').read_text())
        self.L.execute('''
          rows = {[117] = {k = "object", st = 20, bc = 5}, [2512] = {k = "weapon", st = 200, bc = 200},
                  [999] = {k = "weapon", st = 1}, [858] = {k = "object", st = 5}, [2553] = {k = "object", st = 1}}
          function lotOf(e) return rows[e] end
          l = S.New()
          function left(wares)
            local out = {}
            for _, w in ipairs(S.Wares(l, "f1", wares, lotOf)) do out[w.Entry] = w.Count end
            return out
          end
          BREAD = {{Idx = 0, Entry = 117, Price = 25, Stock = -1, Max = 0}}
          SWORD = {{Idx = 1, Entry = 999, Price = 100, Stock = 3, Max = 3}}
        ''')

    def count(self, wares, entry):
        return self.L.eval('left(%s)[%d]' % (wares, entry))

    def unlimited(self, entry):
        return self.count('{{Idx = 0, Entry = %d, Price = 9, Stock = -1, Max = 0}}' % entry, entry)

    def test_a_consumable_starts_at_three_whatever_wow_s_lot(self):
        self.assertEqual(self.unlimited(117), 3)  # WoW's lot of 5: part of it, at the price of one
        self.assertEqual(self.unlimited(858), 3)

    def test_gear_and_what_does_not_stack_is_one(self):
        self.assertEqual(self.unlimited(999), 1)
        self.assertEqual(self.unlimited(2553), 1)

    def test_ammunition_is_one_of_wow_s_purchases(self):
        self.assertEqual(self.unlimited(2512), 200)

    def test_a_limited_ware_starts_at_wow_s_maxcount(self):
        self.assertEqual(self.count('SWORD', 999), 3)

    def test_what_is_bought_stays_gone_and_the_rest_stays(self):
        self.L.execute('S.Bought(l, "f1", 117, 2)')
        self.assertEqual(self.count('BREAD', 117), 1)
        self.L.execute('S.Bought(l, "f1", 117, 1)')
        self.assertIsNone(self.count('BREAD', 117))

    def test_one_vendor_s_sales_are_not_another_s(self):
        self.L.execute('S.Bought(l, "f2", 117, 3)')
        self.assertEqual(self.count('BREAD', 117), 3)

    def test_wow_s_timed_stock_does_not_refill_a_limited_ware(self):
        self.L.execute('S.Bought(l, "f1", 999, 2)')
        # WoW's own count came back on its timer, or was already lower: what BG3 counted stands.
        self.assertEqual(self.count('{{Idx = 1, Entry = 999, Price = 100, Stock = 3, Max = 3}}', 999), 1)
        self.assertEqual(self.count('{{Idx = 1, Entry = 999, Price = 100, Stock = 1, Max = 3}}', 999), 1)

    def test_a_long_rest_brings_back_stock_and_purse(self):
        self.L.execute('S.Bought(l, "f1", 117, 3); S.SetPurse(l, "f1", 7); S.Rest(l)')
        self.assertEqual(self.count('BREAD', 117), 3)
        self.assertEqual(self.L.eval('S.PurseOf(l, "f1", 30)'), self.L.eval('S.Purse(30)'))

    def test_a_pocket_keeps_its_own_count(self):
        self.L.execute('S.Bought(l, "f1", 117, 5)')
        self.assertEqual(self.count('{{Idx = 0, Entry = 117, Price = 0, Stock = -1, Count = 2}}', 117), 2)

    def test_keys_survive_json_as_strings(self):
        self.L.execute('S.Bought(l, "123", 117, 2); S.SetPurse(l, "123", 40)')
        self.assertEqual(self.L.eval('l.V.g123.B.e117'), 2)
        self.assertEqual(self.L.eval('S.PurseOf(l, "123", 30)'), 40)

    def test_the_purse_table(self):
        # sales from BG3's tables, gear from WoW's: the numbers in the report.
        for level, want in ((1, 1500), (5, 1500), (10, 6100), (20, 21800), (30, 38800), (40, 49100), (50, 71500), (60, 158700)):
            self.assertEqual(self.L.eval('S.Purse(%d)' % level), want, level)

    def test_a_higher_vendor_never_holds_less(self):
        ps = [self.L.eval('S.Purse(%d)' % w) for w in range(1, 64)]
        self.assertEqual(ps, sorted(ps))
        self.assertEqual(self.L.eval('S.Purse(nil)'), self.L.eval('S.Purse(30)'))

    def test_bg3_s_numbers(self):
        self.assertAlmostEqual(self.L.eval('S.Sales(1)'), 12.5 * 5 * 6.5 / (1.3 * 3.2 * 10 / 2.5))
        self.assertEqual(self.L.eval('S.BG3Level(60)'), 12)
        self.assertAlmostEqual(self.L.eval('S.Sales(2.5)'), (self.L.eval('S.Sales(2)') + self.L.eval('S.Sales(3)')) / 2)
        self.assertEqual(self.L.eval('S.BG3Level(1)'), 1)


FAKE = r'''
tick, pending, pendingItem, state = 1000, {}, {}, {Errors = {}}
gold, piles, vars = {}, {}, {}
function q(f, ...) local ok, v = pcall(f, ...); if ok then return v end end
function uuidOf(s) return s end
function goldOf(u) return gold[u] or 0 end
function setGold(u, n) gold[u] = n end
GOLD = "gold"
Ext = {Entity = {Get = function(u) if u ~= "gone" then return {Vars = setmetatable({}, {
          __index = function(_, k) return vars[u .. k] end, __newindex = function(_, k, v) vars[u .. k] = v end})} end end},
       Json = {Stringify = function(t) return encode(t) end, Parse = function(s) return decode(s) end},
       Vars = {RegisterUserVariable = function() end}}
Osi = {DB_Players = {Get = function() return {{"tav"}, {"gale"}} end},
       GetItemByTemplateInInventory = function() return nil end}
M = {Barter = {}, Item = function(e) return ({[117] = {bc = 5}, [999] = {bc = 1}})[e] end}
BARTER_OPS = {buy = true, buypart = true, sell = true}
vendorOf, trading, lastInv = {farley = "f1"}, {}, {Vendors = {f1 = {{Idx = 0, Entry = 117, Price = 25, Max = 0, Level = 30}}}}
'''


class Game(unittest.TestCase):
    """Inventory.lua's ledger, purse and replies."""

    def setUp(self):
        self.L = L = rt()
        L.globals().encode = lambda t: json.dumps(self.tod(t))
        L.globals().decode = lambda s: L.table_from(json.loads(s)) if False else self.tol(json.loads(s))
        L.execute(FAKE)
        L.globals().Stock = L.execute((LUA / 'Stock.lua').read_text())
        text = (LUA / 'Inventory.lua').read_text()
        ledger = section(text, 'local STOCK_VAR', '-- Replies that name a new WoW item')
        fund = section(text, "local funded, placed", 'local function fillVendors')
        L.execute((ledger + fund).replace('local function', 'function').replace('local ledger', 'ledger')
                  .replace('local funded', 'funded').replace('local STOCK', 'STOCK').replace('local STOCK_OPS', 'STOCK_OPS'))
        L.execute('ledger = readLedger()')

    def tod(self, t):
        if lupa.lua_type(t) != 'table':
            return t
        return {str(k): self.tod(v) for k, v in t.items()}

    def tol(self, d):
        if isinstance(d, dict):
            t = self.L.table()
            for k, v in d.items():
                t[k] = self.tol(v)
            return t
        return d

    def open(self):
        self.L.execute('trading.farley = 1000; fundMerchant("farley")')

    def test_a_trade_opens_with_the_purse_not_a_fortune(self):
        self.open()
        self.assertEqual(self.L.eval('gold.farley'), 38800)

    def test_a_spent_purse_is_not_filled_again_in_the_trade(self):
        self.open()
        self.L.execute('fundMerchant("farley")')  # seen in place
        self.L.execute('gold.farley = 0; tick = tick + 500; fundMerchant("farley"); lookPurses()')
        self.assertEqual(self.L.eval('gold.farley'), 0)
        self.assertEqual(self.L.eval('Stock.PurseOf(ledger, "f1", 30)'), 0)

    def test_a_purse_is_kept_between_trades(self):
        self.open()
        self.L.execute('fundMerchant("farley"); gold.farley = 500; lookPurses(); trading.farley = nil; lookPurses()')
        self.L.execute('gold.farley = 0; tick = tick + 100')  # emptied after the trade
        self.open()
        self.assertEqual(self.L.eval('gold.farley'), 500)

    def test_buying_from_a_vendor_fills_its_purse(self):
        self.open()
        self.L.execute('fundMerchant("farley"); gold.farley = gold.farley + 1000; lookPurses()')
        self.assertEqual(self.L.eval('Stock.PurseOf(ledger, "f1", 30)'), 39800)

    def test_a_stand_in_gone_says_nothing_about_the_purse(self):
        self.open()
        self.L.execute('fundMerchant("farley"); gold.farley = 500; lookPurses()')
        self.L.execute('vendorOf.gone = "f1"; placed.gone = true; lookPurses()')
        self.assertEqual(self.L.eval('Stock.PurseOf(ledger, "f1", 30)'), 500)

    def test_counts_go_in_the_save_and_a_load_brings_that_save_s_back(self):
        self.L.execute('noteSold({Op = "buy", Npc = "f1", Entry = 117, Count = 1})')
        saved = self.L.eval('vars.tavWowBg3Stock')
        self.L.execute('noteSold({Op = "buypart", Npc = "f1", Entry = 117, Count = 2})')
        self.assertEqual(self.L.eval('ledger.V.gf1.B.e117'), 7)
        self.L.execute('vars.tavWowBg3Stock = %r; vars.galeWowBg3Stock = %r; ledger = readLedger()' % (saved, saved))
        self.assertEqual(self.L.eval('ledger.V.gf1.B.e117'), 5)

    def test_a_save_from_before_this_has_full_shops(self):
        self.assertEqual(self.L.eval('next(ledger.V)'), None)

    def test_a_long_rest_writes_through(self):
        self.L.execute('noteSold({Op = "buy", Npc = "f1", Entry = 117, Count = 1}); M.Restock()')
        self.assertEqual(self.L.eval('next(ledger.V)'), None)
        self.assertEqual(json.loads(self.L.eval('vars.tavWowBg3Stock'))['V'], {})
        self.assertEqual(self.L.eval('state.Restocks'), 1)

    def test_a_stolen_ware_is_gone_too_and_a_conjuring_is_nobody_s_stock(self):
        self.L.execute('noteSold({Op = "add", Npc = "f1", Entry = 117, Count = 5})')
        self.L.execute('noteSold({Op = "add", Entry = 8079, Count = 3})')
        self.assertEqual(self.L.eval('ledger.V.gf1.B.e117'), 5)
        self.assertEqual(self.L.eval('ledger.V.gf1.B.e8079'), None)

    def test_a_refused_buy_does_not_fill_the_purse(self):
        self.open()
        self.L.execute('fundMerchant("farley"); gold.farley = gold.farley + 900')  # BG3 took the price, WoW said full bag
        self.L.execute('M.Refunded("f1", 900); lookPurses()')
        self.assertEqual(self.L.eval('Stock.PurseOf(ledger, "f1", 30)'), 38800)

    def test_a_refusal_after_the_trade_goes_on_the_ledger(self):
        self.L.execute('Stock.SetPurse(ledger, "f1", 1000); M.Refunded("f1", 400)')
        self.assertEqual(self.L.eval('Stock.PurseOf(ledger, "f1", 30)'), 600)

    def test_a_refused_sale_gives_the_purse_back_its_pay(self):
        self.L.execute('Stock.SetPurse(ledger, "f1", 1000); M.Refunded("f1", -40)')
        self.assertEqual(self.L.eval('Stock.PurseOf(ledger, "f1", 30)'), 1040)

    def test_a_barter_s_requests_carry_wow_s_prices(self):
        text = (LUA / 'Inventory.lua').read_text()
        self.L.execute(section(text, 'function M.NotePrice', '-- A character')
                       + '''
          M.Item = function(e) return ({[117] = {sp = 1, bc = 5}, [999] = {sp = 300}})[e] end
          lastInv.Members = {tav = {Money = 1000, Items = {{Guid = "9A", Entry = 999, Count = 2}}}}
          M.Barter.tav = {At = 1}
          a = {Op = "sell", Member = "tav", Guid = "9A", Count = 1}; M.NotePrice(a)
          c = {Op = "sell", Member = "tav", Guid = "9A"}; M.NotePrice(c)
          d = {Op = "buy", Member = "tav", Npc = "f1", Entry = 117, Count = 2}; M.NotePrice(d)
          e = {Op = "buypart", Member = "tav", Npc = "f1", Entry = 117, Count = 3, Lot = 5}; M.NotePrice(e)
        ''')
        self.assertEqual([self.L.eval(x + '.Wow') for x in 'acde'], [300, 600, 50, 15])
        self.assertEqual((self.L.eval('M.Barter.tav.Money'), self.L.eval('M.Barter.tav.S'), self.L.eval('M.Barter.tav.T')), (1000, 900, 65))


class SellMoney(unittest.TestCase):
    """The Gold part of Inventory.lua: a sale to a short purse pays WoW's money what BG3 paid."""

    def setUp(self):
        self.L = L = rt()
        L.execute(r'''
          tick, pending, reqs = 100, {}, {}
          gold = 1000
          M, goldOwed, goldTried = {Barter = {}, CopperPerGold = 1, Request = function(r) reqs[#reqs + 1] = r end}, {}, {}
          BARTER_WAIT, BARTER_OPS = 600, {buy = true, buypart = true, sell = true}
          function goldOf() return gold end
          function setGold(_, want) gold = want end
          Osi = {AddGold = function(_, n) gold = gold + n end}
        ''')
        text = (LUA / 'Inventory.lua').read_text()
        L.execute('function look(uuid, m)\n' + section(text, "    -- Gold: WoW's money", 'function M.Reconcile(inv)')
                  .split('local function eachHeld')[0])

    def test_the_vendor_had_100_for_a_500_sale(self):
        # BG3 paid what the purse held (gold 1100); WoW's own sell handling paid its 500 (money 1500).
        self.L.execute('gold = 1100; M.Barter.hero = {At = 100}; look("hero", {Money = 1500})')
        self.assertEqual(self.L.eval('reqs[1].Op'), 'money')
        self.assertEqual(self.L.eval('reqs[1].Delta'), -400)
        self.L.execute('look("hero", {Money = 1100})')
        self.assertEqual(self.L.eval('M.Barter.hero'), None)
        self.assertEqual(self.L.eval('gold'), 1100)


class FillHolder(unittest.TestCase):
    """fillHolder: a sold-out ware leaves the stand-in and is not made again; the one just bought stays the buyer's."""

    def setUp(self):
        self.L = L = rt()
        L.execute(r'''
          tick, pendingItem, deleted, sideUuid, sideCount, asked, made, gone = 1000, {}, {}, {}, {}, {}, {}, {}
          holders, stacks, ents, inv, nextId = {}, {}, {}, {}, 0
          M = {Refused = {}, SideTag = {}, Suppress = function() end, Item = function(e) return ({[117] = {bc = 5}})[e] end}
          M.Create = function(e, standin, n, tag)
            nextId = nextId + 1; local u = "u" .. nextId
            made[#made + 1] = {tag, n}; holders[u] = standin; stacks[u] = n; ents[u] = true; inv[tag] = u; return u
          end
          function ours() return inv end
          function holderOf(u) return holders[u] end
          function note() end
          function q(f, ...) local ok, v = pcall(f, ...); if ok then return v end end
          function buying() return true end
          function askFor() end
          Ext = {Entity = {Get = function(u) return ents[u] end}}
          Osi = {RequestDelete = function(u) gone[#gone + 1] = u; for t, x in pairs(inv) do if x == u then inv[t] = nil end end; ents[u] = nil end,
                 GetStackAmount = function(u) return stacks[u] end, SetStackAmount = function(u, n) stacks[u] = n end,
                 ToInventory = function() end}
        ''')
        text = (LUA / 'Inventory.lua').read_text()
        L.execute(section(text, 'local function fillHolder', 'local corpseGold').replace('local function', 'function'))

    def test_sold_out_leaves_the_stand_in(self):
        self.L.execute('fillHolder("farley", "Vf1:", {{Idx = 0, Entry = 117, Count = 5}}); fillHolder("farley", "Vf1:", {})')
        self.assertEqual(self.L.eval('#gone'), 1)

    def test_just_bought_stays_with_the_buyer(self):
        self.L.execute('fillHolder("farley", "Vf1:", {{Idx = 0, Entry = 117, Count = 5}}); holders.u1 = "tav"; fillHolder("farley", "Vf1:", {})')
        self.assertEqual(self.L.eval('#gone'), 0)

    def test_nothing_new_while_the_buy_is_on_its_way(self):
        self.L.execute('''fillHolder("farley", "Vf1:", {{Idx = 0, Entry = 117, Count = 5}})
          inv["Vf1:0"] = nil; ents.u1 = nil; asked.u1 = true; pendingItem.u1 = 1000
          fillHolder("farley", "Vf1:", {{Idx = 0, Entry = 117, Count = 5}})''')
        self.assertEqual(self.L.eval('#made'), 1)

    def test_part_left_is_what_it_is_made_with(self):
        self.L.execute('''fillHolder("farley", "Vf1:", {{Idx = 0, Entry = 117, Count = 3}})''')
        self.assertEqual(self.L.eval('made[1][2]'), 3)

    def test_a_long_rest_makes_it_again(self):
        self.L.execute('''fillHolder("farley", "Vf1:", {{Idx = 0, Entry = 117, Count = 5}})
          inv["Vf1:0"] = nil; ents.u1 = nil; asked.u1 = true
          fillHolder("farley", "Vf1:", {{Idx = 0, Entry = 117, Count = 5}})''')
        self.assertEqual(self.L.eval('#made'), 2)


class Server(unittest.TestCase):
    """The bridge reads patch 0021's two new words; a line without them still reads."""

    def lines(self, *lines):
        s = I.Server.__new__(I.Server)
        s.call = lambda *a: [l.split() for l in lines]
        return s.vendor('f1', 'Tav')

    def test_new_words(self):
        w = self.lines('WOWBG3 vendor Tav f1 item 0 117 25 2 5 3 30')[0]
        self.assertEqual((w['Stock'], w['Max'], w['Level']), (2, 3, 30))

    def test_old_server(self):
        self.assertNotIn('Level', self.lines('WOWBG3 vendor Tav f1 item 0 117 25 -1 5')[0])


if __name__ == '__main__':
    unittest.main()
