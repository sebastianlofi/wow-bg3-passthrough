#!/usr/bin/env python3
"""test_camp.py: camp trips and spent supplies, without a game. The real Lua text of Camp.lua and of the
parts of WorldMode.lua and Inventory.lua named below runs under a fake game (needs the `lupa` module).

  python3 tools/test_camp.py
"""
import json, sys, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
LUA = HERE.parent / 'mod/ScriptExtender/Lua'
sys.path.insert(0, str(HERE))
try:
    import lupa
except ImportError:
    print('test_camp.py: needs the lupa module (pip install lupa); skipped')
    raise SystemExit(0)

HERO = 'S_Hero_11111111-2222-3333-4444-555555555555'
HERO_ID = HERO[-36:]
FAKE = r'''
files, ticks_cb, listeners, teleports, printed = {}, {}, {}, {}, {}
in_camp, position, requests, stack = false, {0, 0, 0}, {}, {}
monotonic = 0
Ext = {
  IO = {LoadFile = function(n) return files[n] end, SaveFile = function(n, v) files[n] = v end},
  Json = {Parse = function(v) return py_parse(v) end, Stringify = function(v) return py_dump(v) end},
  Events = {Tick = {Subscribe = function(_, f) ticks_cb[#ticks_cb + 1] = f end}},
  Osiris = {RegisterListener = function(n, a, w, f) listeners[n] = f end},
  Utils = {GetGameState = function() return "Running" end, Print = function(s) printed[#printed + 1] = s end,
           MonotonicTime = function() return monotonic end},
  Log = {Print = function() end},
}
Osi = {
  DB_PlayerInCamp = {Get = function(_, h) if in_camp then return {{h}} end return {} end},
  GetPosition = function() return position[1], position[2], position[3] end,
  TeleportToPosition = function(u, x, y, z) teleports[#teleports + 1] = {x, y, z}; position = {x, y, z} end,
  GetStackAmount = function(u) return stack[u] end,
  SetStackAmount = function(u, n) stack[u] = n end,
}
function tick(n) for _ = 1, n do for _, f in ipairs(ticks_cb) do f() end end end
'''


def runtime():
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.execute(FAKE)
    def to_lua(v):
        if isinstance(v, dict):
            t = L.table()
            for k, x in v.items():
                t[k] = to_lua(x)
            return t
        if isinstance(v, list):
            t = L.table()
            for i, x in enumerate(v):
                t[i + 1] = to_lua(x)
            return t
        return v
    def to_py(t):
        if not lupa.lua_type(t) == 'table':
            return t
        keys = list(t.keys())
        if keys and all(isinstance(k, int) for k in keys):
            return [to_py(t[k]) for k in sorted(keys)]
        return {k: to_py(t[k]) for k in keys}
    L.globals().py_parse = lambda s: to_lua(json.loads(s))
    L.globals().py_dump = lambda t: json.dumps(to_py(t))
    return L


def section(text, start, end):
    i = text.index(start)
    j = text.index(end, i)
    return text[i:j]


class Trip(unittest.TestCase):
    """Camp.lua: the record of a trip."""

    def setUp(self):
        self.L = L = runtime()
        L.execute('''
          files["wow-bg3/camp.json"] = '{"On": true}'
          files["wow-bg3/link/world.json"] = '{"Active": true}'
          package.loaded = package.loaded or {}
          Ext.Require = function(n) return {Uuid = function() return "%s" end, Hopping = function() return false end} end
        ''' % HERO_ID)
        self.camp = L.execute((LUA / 'Camp.lua').read_text().replace('local M = {}', 'local M = {}; Camp = M', 1))
        L.execute('Camp.Start("run")')

    def state(self):
        return json.loads(self.L.globals().files['wow-bg3/camp-state.json'])

    def teleport(self, old, new):
        self.L.globals().position = self.L.table_from(new)
        self.L.globals().listeners['Teleported'](HERO, 'x', *old, *new, 'Waypoint')

    def test_camp_and_back(self):
        L, frm = self.L, [-2600.0, 120.0, -1000.0]
        self.teleport(frm, [384.0, 6.0, -752.0])
        L.globals().in_camp = True
        L.eval('tick(10)')
        s = self.state()
        self.assertTrue(s['InCamp'])
        self.assertEqual(s['From'], frm)
        self.assertTrue(L.eval('Camp.InTrip()'))
        # The story takes the party back to the spot: no correction.
        self.teleport([384.0, 6.0, -752.0], frm)
        L.globals().in_camp = False
        L.eval('tick(40)')
        s = self.state()
        self.assertFalse(s['InCamp'])
        self.assertEqual(s['Off'], 0)
        self.assertEqual(len(L.globals().teleports), 0)
        self.assertFalse(L.eval('Camp.InTrip()'))

    def test_world_modes_start_is_no_travel(self):
        L = self.L
        L.globals().files['wow-bg3/link/world.json'] = '{"Active": true, "Centre": [-2600.0, 120.0, -1000.0]}'
        self.teleport([-46.6, 2.8, -402.8], [-2600.0, 121.0, -1000.0])
        L.eval('tick(200)')
        self.assertEqual(len(L.globals().teleports), 0)
        self.assertFalse(self.state()['InCamp'])

    def test_wrong_return_is_put_right(self):
        L, frm = self.L, [-2600.0, 120.0, -1000.0]
        self.teleport(frm, [384.0, 6.0, -752.0])
        L.globals().in_camp = True
        L.eval('tick(10)')
        L.globals().in_camp = False  # left, but still at camp's position
        L.eval('tick(40)')
        self.assertEqual(list(L.globals().teleports[1].values()), frm)
        self.assertFalse(self.state()['InCamp'])

    def test_native_travel_in_the_level_is_undone(self):
        self.teleport([0.0, 0.0, 0.0], [500.0, 0.0, 0.0])
        self.L.eval('tick(400)')
        self.assertFalse(self.state()['InCamp'])
        self.assertFalse(self.L.eval('Camp.InTrip()'))
        self.assertEqual(list(self.L.globals().teleports[1].values()), [0.0, 0.0, 0.0])

    def test_short_teleport_ignored(self):
        self.teleport([0.0, 0.0, 0.0], [50.0, 0.0, 0.0])
        self.L.globals().in_camp = True
        self.L.eval('tick(10)')
        self.assertFalse(self.state()['InCamp'])


class Publish(unittest.TestCase):
    """WorldMode.lua publishTav: no record of camp's position ever reaches the WoW side."""

    def setUp(self):
        self.L = L = runtime()
        text = (LUA / 'WorldMode.lua').read_text()
        body = section(text, '    local camp = {Switch', '        local rec = string.pack("<c4I4fffffffI4I4", "TAV1"')
        L.execute(r'''
          runId, ticks, jobs, M, state = "run", 0, {}, {}, {Centre = {-2600, 120, -1000}, WowOrigin = {0, 0, 0}}
          Camp = {On = function() return true end, JUMP = 150.0, PENDING = 180}
          MAX_SECONDS = 900
          sent, tavSeq = {}, 0
          where = {-2600, 120, -1000}
          Probe = {HostPosition = function() return where end}
          Hero = {Entity = function() return {TurnBased = nil} end}
          function bg3Muted() return false end
          function toWow(p) return p end
          function arena(c) end
          function readJson(n) if not state_file then return nil end return Ext.Json.Parse(state_file) end
          coroutine.create = function(f) return f end
          string_pack = string.pack
        ''')
        L.execute(r'''
          function make()
        ''' + body + r'''
            sent[#sent + 1] = {w[1], w[2], w[3], flags}
          end
          return publishTav, camp
          end
          pub, camp_table = make()
        ''')

    def run_ticks(self, n):
        for _ in range(n):
            self.L.execute('ticks = ticks + 1; if ticks % 3 == 0 then pub() end')

    def sent(self):
        return [list(v.values()) for v in self.L.globals().sent.values()]

    def test_jump_held_until_camp_says(self):
        L = self.L
        self.run_ticks(6)
        n = len(self.sent())
        self.assertGreater(n, 0)
        L.execute('where = {384, 6, -752}')
        self.run_ticks(30)  # Camp.lua has not spoken yet: nothing goes out
        self.assertEqual(len(self.sent()), n)
        L.execute('''state_file = '{"RunId": "run", "Seq": 1, "InCamp": true}' ''')
        self.run_ticks(6)
        rows = self.sent()
        self.assertGreater(len(rows), n)
        self.assertEqual(rows[-1][:3], [-2600, 120, -1000])  # the spot left, never the camp
        self.assertEqual(rows[-1][3] & 2, 2)  # WoW's sound quiet
        self.assertTrue(L.eval('M.InCamp()'))
        self.assertFalse(any(r[0] == 384 for r in rows))

    def test_back_resumes(self):
        L = self.L
        self.run_ticks(6)
        L.execute('where = {384, 6, -752}')
        L.execute('''state_file = '{"RunId": "run", "Seq": 1, "InCamp": true}' ''')
        self.run_ticks(6)
        L.execute('where = {-2599, 120, -1000}')
        L.execute('''state_file = '{"RunId": "run", "Seq": 2, "InCamp": false}' ''')
        self.run_ticks(6)
        self.assertFalse(L.eval('M.InCamp()'))
        self.assertEqual(self.sent()[-1][:3], [-2599, 120, -1000])
        self.assertEqual(self.sent()[-1][3] & 2, 0)

    def test_long_teleport_goes_out_after_the_hold(self):
        L = self.L
        self.run_ticks(6)
        L.execute('where = {-2000, 120, -1000}')
        self.run_ticks(180)
        n = len(self.sent())
        self.run_ticks(60)
        self.assertGreater(len(self.sent()), n)
        self.assertEqual(self.sent()[-1][0], -2000)

    def test_undone_travel_never_goes_out(self):
        L = self.L
        self.run_ticks(6)
        n = len(self.sent())
        L.execute('where = {384, 6, -752}')
        self.run_ticks(185)  # Camp.lua's PENDING and its check
        L.execute('where = {-2600, 120, -1000}')
        self.run_ticks(60)
        self.assertGreater(len(self.sent()), n)
        self.assertFalse(any(r[0] == 384 for r in self.sent()))


class Spend(unittest.TestCase):
    """Inventory.lua reconcileStacks: food spent in BG3 reaches WoW and does not come back."""

    def setUp(self):
        self.L = L = runtime()
        text = (LUA / 'Inventory.lua').read_text()
        body = section(text, 'local stackLast, stackSpent', "-- A party member's own clothes")
        L.execute(r'''
          Ext.Entity = {Get = function(u) if stack[u] then return {} end end}
          M, state = {Items = nil, Spent = 0}, {Created = 0}
          tick = 0
          reqs, created = {}, {}
          Camp = {On = function() return true end}
          pendingItem = {}
          function q(f, ...) local ok, v = pcall(f, ...); if ok then return v end end
          function stackTag(e, m) return "S" .. e .. "@" .. m end
          stackWant = {}
          M.Item = function(e) if e == 4540 then return {sv = 10} end return {} end
          M.Request = function(r) reqs[#reqs + 1] = r end
          M.Suppress = function() end
          M.Create = function(e, u, n, tag) created[#created + 1] = n; stack["made"] = n; return "made" end
          M.Refused = {}
          function holderOf() return "hero" end
          function entryOf() return 4540 end
          function note() end
          function tagOf(u) return "S4540@hero" end
          function eachOurs(f) for u in pairs(stack) do f(u) end end
        ''')
        L.execute(body + '\nreconcile = reconcileStacks\npieces = pieceOwner')

    def go(self, total, have=True):
        L = self.L
        L.execute('''
          tick = tick + 200
          reconcile("hero", {[4540] = %d}, %s, {{Entry = 4540, Guid = "G1", Count = %d}})
        ''' % (total, '{["S4540@hero"] = "u"}' if have else '{}', total))

    def test_spent_reaches_wow_and_stays_spent(self):
        L = self.L
        L.execute('stack["u"] = 20')
        self.go(20)
        self.assertEqual(len(L.globals().reqs), 0)
        L.execute('stack["u"] = 16')  # BG3 took 4 for a long rest
        self.go(20)
        self.assertEqual(len(L.globals().reqs), 0)  # not until it stays gone
        self.go(20)
        r = L.globals().reqs[1]
        self.assertEqual((r['Op'], r['Guid'], r['Count']), ('del', 'G1', 4))
        self.assertEqual(L.eval('stack["u"]'), 16)
        self.go(20)  # WoW's list not updated yet: still 16, no second request
        self.assertEqual(L.eval('stack["u"]'), 16)
        self.assertEqual(len(L.globals().reqs), 1)
        self.go(16)  # WoW's list has it
        self.assertEqual(L.eval('stack["u"]'), 16)
        self.assertEqual(len(L.globals().reqs), 1)

    def test_whole_stack_gone_is_not_made_again(self):
        L = self.L
        L.execute('stack["u"] = 4')
        self.go(4)
        L.execute('stack["u"] = nil')  # BG3 used the last of it: the entity is gone
        self.go(4, have=False)
        self.go(4, have=False)
        r = L.globals().reqs[1]
        self.assertEqual(r['Count'], 4)
        self.assertEqual(len(L.globals().created), 0)

    def test_gone_for_one_pass_costs_nothing(self):
        L = self.L
        L.execute('stack["u"] = 4')
        self.go(4)
        L.execute('stack["u"] = nil')
        self.go(4, have=False)
        L.execute('stack["u"] = 4')
        self.go(4)
        self.go(4)
        self.assertEqual(len(L.globals().reqs), 0)
        self.assertEqual(len(L.globals().created), 0)
        self.assertEqual(L.eval('stack["u"]'), 4)

    def test_refused_del_comes_back(self):
        L = self.L
        L.execute('stack["u"] = 20')
        self.go(20)
        L.execute('stack["u"] = 16')
        self.go(20)
        self.go(20)
        L.execute('monotonic = 20000')
        self.go(20)  # WoW still says 20 long after: WoW is the truth
        self.assertEqual(L.eval('stack["u"]'), 20)

    def test_wow_side_change_is_followed(self):
        L = self.L
        L.execute('stack["u"] = 10')
        self.go(10)
        self.go(14)  # bought more
        self.assertEqual(L.eval('stack["u"]'), 14)
        self.assertEqual(len(L.globals().reqs), 0)
        self.go(12)  # WoW ate two
        self.assertEqual(L.eval('stack["u"]'), 12)
        self.assertEqual(len(L.globals().reqs), 0)

    def test_sold_out_then_bought_is_no_spending(self):
        L = self.L
        L.execute('stack["u"] = 4')
        self.go(4)
        L.execute('reconcile("hero", {}, {}, {})')  # sold: WoW lists none
        self.go(2, have=False)  # bought 2 later
        self.assertEqual(len(L.globals().reqs), 0)
        self.assertEqual(list(L.globals().created.values()), [2])

    def test_split_or_move_destroys_nothing(self):
        L = self.L
        L.execute('stack["u"] = 20')
        self.go(20)
        L.execute('stack["u"] = 15; stack["piece"] = 5')  # 5 split off, or in another member's stack
        self.go(20)
        self.assertEqual(len(L.globals().reqs), 0)
        self.assertEqual(L.eval('stack["u"]'), 20)  # the old rule, as with the switch off

    def test_part_eaten_after_a_split_counts_only_that(self):
        L = self.L
        L.execute('stack["u"] = 20')
        self.go(20)
        L.execute('stack["u"] = 15; stack["piece"] = 3')  # 5 moved, 2 of them eaten
        self.go(20)
        self.go(20)
        self.assertEqual(L.globals().reqs[1]['Count'], 2)

    def test_a_part_split_off_is_no_spending(self):
        # A piece has no tag, so eachOurs does not see it: it still is the party's, in the barter columns.
        L = self.L
        L.execute('stack["u"] = 20')
        self.go(20)
        L.execute('function eachOurs(f) f("u") end; stack["u"] = 15; stack["piece"] = 5; pieces["piece"] = "hero"')
        self.go(20)
        self.go(20)
        self.assertEqual(len(L.globals().reqs), 0)
        self.assertEqual(L.eval('stack["u"]'), 15)  # not put back while it is out
        L.execute('stack["piece"] = nil; pieces["piece"] = nil; stack["u"] = 20')  # it came back on the stack
        self.go(20)
        self.assertEqual(len(L.globals().reqs), 0)

    def test_a_thrown_piece_is_spent_when_it_is_gone(self):
        L = self.L
        L.execute('stack["u"] = 5')
        self.go(5)
        L.execute('function eachOurs(f) f("u") end; stack["u"] = 4; stack["piece"] = 1; pieces["piece"] = "hero"')
        self.go(5)
        self.assertEqual(L.eval('stack["u"]'), 4)  # in the air: not put back, or every throw would be free
        L.execute('stack["piece"] = nil; pieces["piece"] = nil')  # it broke where it landed
        self.go(5)
        self.go(5)
        self.assertEqual(L.globals().reqs[1]['Count'], 1)
        self.assertEqual(L.eval('stack["u"]'), 4)

    def test_other_items_untouched(self):
        L = self.L
        L.execute('''
          stack["x"] = 5
          reconcile("hero", {[99] = 5}, {["S99@hero"] = "x"}, {})
          stack["x"] = 3
          reconcile("hero", {[99] = 5}, {["S99@hero"] = "x"}, {})
        ''')
        self.assertEqual(L.eval('stack["x"]'), 5)
        self.assertEqual(len(L.globals().reqs), 0)


class Supply(unittest.TestCase):
    def test_rule_covers_every_tier(self):
        rules = json.loads((HERE / 'wow_items_rules.json').read_text())
        tiers = rules['camp_supply']
        value = lambda il: next(v for top, v in tiers if il <= top or top == 999)
        self.assertEqual([value(i) for i in (5, 15, 25, 35, 45, 55)], [20, 20, 20, 25, 30, 40])
        # Items of a tier for a long rest of 40, and what it costs at WoW's vendor price (copper; sold in lots of 5).
        cost = {il: -(-40 // value(il)) * p // 5 for il, p in zip((5, 15, 25, 35, 45, 55), (25, 125, 500, 1000, 2000, 4000))}
        self.assertEqual(cost, {5: 10, 15: 50, 25: 200, 35: 400, 45: 800, 55: 800})

    def row(self, name, ilvl, category=0, spell=0, sell=1):
        sys.path.insert(0, str(HERE))
        import collections
        import wow_items
        rules = json.loads((HERE / 'wow_items_rules.json').read_text())
        it = collections.defaultdict(int, entry=1, name=name, item_level=ilvl, spellcategory_1=category, spellid_1=spell, sell_price=sell, stackable=20)
        it['class'] = 0
        return wow_items.map_item(rules, it, {})

    def test_supply_follows_the_price_of_one(self):
        import collections
        import wow_items
        rules = json.loads((HERE / 'wow_items_rules.json').read_text())
        def sv(price):
            it = collections.defaultdict(int, entry=1, name='Bread', buy_price=price * 5, buy_count=5, item_level=5, spellcategory_1=11, stackable=20)
            it['class'] = 0
            return wow_items.map_item(rules, it, {})['sv']
        self.assertEqual([sv(p) for p in (5, 25, 100, 200, 400, 800)], [5, 15, 30, 40, 55, 80])

    def test_water_is_bg3_water_at_any_level(self):
        self.assertEqual(self.row('Morning Glory Water', 55, 59)['b'], 'CONS_DRINK_Water')

    def test_a_price_is_for_one_not_for_what_one_purchase_gives(self):
        import collections
        import wow_items
        rules = json.loads((HERE / 'wow_items_rules.json').read_text())
        it = collections.defaultdict(int, entry=8950, name='Homemade Cherry Pie', buy_price=4000, buy_count=5, sell_price=200, stackable=20)
        it['class'] = 0
        r = wow_items.map_item(rules, it, {})
        self.assertEqual((r['bp'], r['bc'], r['sp']), (800, 5, 200))
        it.update(buy_price=10, buy_count=200)  # an arrow is not free
        self.assertEqual(wow_items.map_item(rules, it, {})['bp'], 1)
        it.update(buy_price=500, buy_count=1)
        r = wow_items.map_item(rules, it, {})
        self.assertEqual(r['bp'], 500)
        self.assertNotIn('bc', r)

    def test_food_heals_by_level_and_counts(self):
        self.assertEqual((self.row('Tough Hunk of Bread', 5, 11)['b'], self.row('Tough Hunk of Bread', 5, 11)['sv']), ('CONS_FOOD_Bread_Bun', 20))
        self.assertEqual(self.row('Homemade Cherry Pie', 55, 11)['b'], 'CONS_FOOD_Dinner')
        self.assertEqual(self.row('Morning Glory Dew', 55, 59)['b'], 'CONS_FOOD_Stew')

    def test_alcohol_makes_drunk_and_is_a_small_supply(self):
        r = self.row('Volatile Rum', 1, 0, 11009)
        self.assertEqual((r['b'], r['sv']), ('CONS_DRINK_Beer', 5))
        self.assertEqual(self.row('Cherry Grog', 35, 59, 11009)['b'], 'CONS_DRINK_Beer')

    def test_conjured_is_flat_and_sells_for_nothing(self):
        r = self.row('Conjured Crystal Water', 65, 59, sell=0)
        self.assertEqual((r['sv'], r['sp'], r['bp']), (5, 0, 0))

    def test_named_like_food_is_not_a_supply(self):
        self.assertNotIn('sv', self.row('Elixir of Water Breathing', 18))


class ListGaps(unittest.TestCase):
    """A far move's teleport can read the inventory list's console lines: no word on what WoW holds."""

    def server(self, *lines):
        import inventory_bridge as ib
        srv = ib.Server.__new__(ib.Server)
        srv.call = lambda *words: [l.split() for l in lines]
        return srv

    def test_whole_list_passes(self):
        srv = self.server('WOWBG3 inv Tav begin 7', 'WOWBG3 inv Tav item 255 28 4F 117 4 0 0 0', 'WOWBG3 inv Tav end 1')
        money, items = srv.list('Tav')
        self.assertEqual((money, items[0]['Count']), (7, 4))

    def test_lines_missing_is_an_error(self):
        for lines in ((), ('WOWBG3 inv Tav begin 7',), ('WOWBG3 inv Tav begin 7', 'WOWBG3 inv Tav end 1')):
            with self.assertRaises(RuntimeError):
                self.server(*lines).list('Tav')

    def bridge(self, lists):
        import inventory_bridge as ib, tempfile
        b = ib.Bridge.__new__(ib.Bridge)
        b.vendors, b.pockets, b.not_vendor, b.char, b.camp_on = {}, set(), {}, 'Tav', False
        b.be = type('Be', (), {'vendor': lambda _, g, n: lists.pop(0) if lists else []})()
        d = Path(tempfile.mkdtemp())
        (d / 'mirror.json').write_text(json.dumps({'Mobs': [{'Guid': 'n1', 'Friendly': True}]}))
        ib.LINK, self._link = d, ib.LINK
        self.addCleanup(lambda: setattr(ib, 'LINK', self._link))
        return ib, b

    def test_a_vendor_refused_from_afar_is_asked_again(self):
        ib, b = self.bridge([[], [{'Idx': 0, 'Entry': 117, 'Price': 25, 'Stock': -1}]])
        b.prefetch()
        self.assertEqual(b.vendors, {})
        b.prefetch()  # too soon
        self.assertEqual(b.not_vendor['n1'][0], 1)
        b.not_vendor['n1'] = (1, 0)
        self.assertTrue(b.prefetch())
        self.assertEqual(b.vendors['n1'][0]['Entry'], 117)

    def test_one_who_sells_nothing_stops_being_asked(self):
        ib, b = self.bridge([])
        for _ in range(ib.LIST_TRIES + 3):
            b.prefetch()
            b.not_vendor['n1'] = (b.not_vendor['n1'][0], 0)
        self.assertEqual(b.not_vendor['n1'][0], ib.LIST_TRIES)

    def test_a_missing_list_deletes_nothing(self):
        L = runtime()
        text = (LUA / 'Inventory.lua').read_text()
        L.execute("""
          M, state, deleted, Ext = {}, {}, {}, {}
          Osi = {RequestDelete = function(u) deleted[#deleted + 1] = u end}
          pendingItem = {}
          function ours() return {["S117@hero"] = "stack", ["123"] = "sword", ["6F"] = "axe", ["V1A:0"] = "ware"} end
          function isWowGuid(tag) return tag:match("^%x+$") ~= nil end
          swept = {}
          function sweepStrays() end
          listedAt, LISTED_WAIT, tick = {}, 900, 0
          stackLast, stackSpent, dropSeen = {}, {}, {}
          function goldOf() return 0 end
          function reconcileMember(uuid, m) if m.Items then wowOwner["123"] = uuid; wowOwner["6F"] = uuid; stackWant["S117@hero"] = true end end
          M.Suppress = function() end
        """)
        L.execute(section(text, 'function M.Reconcile(inv)', "-- WoW's own words"))
        L.execute('M.Reconcile({Members = {hero = {Char = "Tav", Error = "offline"}}})')
        self.assertEqual(L.eval('#deleted'), 0)
        L.execute('M.Reconcile({Members = {hero = {Char = "Tav", Items = {}}}})')
        self.assertEqual(L.eval('#deleted'), 0)
        L.execute('M.Reconcile({Members = {}})')  # just listed and now not: the wait at a rest's end
        self.assertEqual(L.eval('#deleted'), 0)
        L.execute('tick = LISTED_WAIT; M.Reconcile({Members = {}})')
        self.assertEqual(L.eval('#deleted'), 3)  # the stack and both guids, one with a hex letter; no ware

    def test_a_loads_bare_copies_go_and_the_stack_stays(self):
        L = runtime()
        text = (LUA / 'Inventory.lua').read_text()
        L.execute(r'''
          M, deleted = {Suppress = function() end}, {}
          function q(f, ...) local ok, v = pcall(f, ...); if ok then return v end end
          local function holds(...)
            local list = {}
            for i, u in ipairs({...}) do list[i] = {Item = {Uuid = {EntityUuid = u}}} end
            return {InventoryOwner = {PrimaryInventory = {InventoryContainer = {Items = list}}}}
          end
          ents = {hero = holds("stack", "sack", "potion"), sack = holds("stray", "food", "bun", "statless")}
          ents.bun, ents.statless = {Data = {StatsId = ""}}, {Data = {StatsId = ""}}
          Osi.GetTemplate = function(u) return u == "bun" and "CONS_GEN_Food_Bread_Bun_A_e604" or "BOOK_x" end
          Ext.Entity = {Get = function(u) return ents[u] end}
          Osi.RequestDelete = function(u) deleted[u] = true end
          tags = {stack = "S117@hero", food = "S118@hero"}
          function tagOf(u) return tags[u] end
          function entryOf(u) if u == "stack" or u == "stray" or u == "food" then return 117 end end
        ''')
        L.execute(section(text, 'local function eachHeld', 'function M.Reconcile(inv)') + '\nsweep = sweepStrays')
        L.execute('sweep("hero")')
        self.assertEqual(sorted(L.eval('deleted').keys()), ['bun', 'stray'])


if __name__ == '__main__':
    unittest.main()
