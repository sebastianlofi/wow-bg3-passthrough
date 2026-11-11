#!/usr/bin/env python3
"""test_world_move.py: a move on the same continent (world_bridge.py World.reorigin), without a game, a server or
WoW's data: which pool subgrid goes where, what goes stale, when world.json turns Active, when Stop and Start
take over; and the cuts made ahead (made-up wow_arena.py).

  python3 tools/test_world_move.py
"""
import json
import os
import struct
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import world_bridge as B  # noqa: E402
import wow_waypoints as W  # noqa: E402

TARGET = (-8840.0, 489.0, 109.0)


class Flat:
    def height(self, x, y):
        return 100.0


class Fake(B.World):
    """A World with no cuts and no mod: `answers` says how each mod command ends, `sent` keeps them."""
    def __init__(self, tmp):
        self.a = SimpleNamespace(pool=6, floor=2, layers=[-15.0], maps='maps')
        with mock.patch.object(B, 'run_id', lambda: 'r1'), mock.patch.object(B, 'player_map', lambda: 0), \
                mock.patch.object(B.wow_arena, 'Terrain', lambda *_: Flat()):
            super().__init__(self.a)
        self.land((0.0, 0.0, 100.0))
        self.answers, self.sent = [], []
        self.storeys = {}  # square -> how many storeys its cut has

    def precut(self, squares):
        pass

    def cut(self, ij):
        if ij not in self.cells:
            n = self.storeys.get(ij, 0)
            self.cells[ij] = {'ij': ij, 'min': self.cell_min(ij), 'base': 90.0, 'tiles': b'', 'section': b'new', 'tris': b'',
                              'kinds': b'', 'storeys': [{'min': self.cell_min(ij + (k + 1,)), 'base': 95.0, 'section': b'up', 'tiles': 9,
                                                         'floors': bytes(5 * B.TILES * B.TILES)} for k in range(n)] or None}
        return self.cells[ij]

    def command(self, action, **fields):
        self.sent.append((action, fields))
        return len(self.sent)

    def wait_done(self, seq, timeout):
        return self.answers.pop(0) if self.answers else (True, None)

    def write_triangles(self, squares):
        self.triangles = list(squares)


class Base(unittest.TestCase):
    def setUp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.tmp = Path(d.name)
        (self.tmp / 'link').mkdir()
        for name, v in (('IO', self.tmp), ('LINK', self.tmp / 'link'), ('MOVE_RESULT', self.tmp / 'result.json'), ('CELLS', self.tmp / 'cells')):
            p = mock.patch.object(B, name, v)
            p.start()
            self.addCleanup(p.stop)
        p = mock.patch.object(B, 'hero', lambda *_: {'Uuid': 'hero-1', 'Name': 'Tav'})
        p.start()
        self.addCleanup(p.stop)
        p = mock.patch.object(B, 'has_ground', lambda maps, m: m in (0, 1, 36))
        p.start()
        self.addCleanup(p.stop)
        self.w = Fake(self.tmp)

    def camera(self, x, y):
        raw = bytearray(96)
        raw[:4] = b'WBC1'
        struct.pack_into('<3f', raw, 72, x, y, 100.0)
        struct.pack_into('<II', raw, 84, 0, 1)
        (self.tmp / 'link/camera.bin').write_bytes(bytes(raw))

    def active(self):
        return json.loads((self.tmp / 'link/world.json').read_text())['Active']


class BareMaps(unittest.TestCase):
    """A WoW map of buildings only has no height maps; its floors still count as ground."""
    def setUp(self):
        d = tempfile.TemporaryDirectory()
        self.addCleanup(d.cleanup)
        self.maps, self.vmaps = Path(d.name) / 'maps', Path(d.name) / 'vmaps'
        self.maps.mkdir()
        self.vmaps.mkdir()
        (self.maps / '0003232.map').write_bytes(b'')
        (self.vmaps / '034.vmtree').write_bytes(b'')
        p = mock.patch.dict(os.environ, {'WOW_BG3_VMAPS': str(self.vmaps)})
        p.start()
        self.addCleanup(p.stop)

    def test_has_ground(self):
        self.assertTrue(B.has_ground(self.maps, 0))     # height maps
        self.assertTrue(B.has_ground(self.maps, 34))    # the Stockade: a collision tree only
        self.assertFalse(B.has_ground(self.maps, 999))  # neither

    def test_its_terrain_is_one_hole_over_every_floor(self):
        t = B.wow_arena.Terrain(self.maps, 34)
        self.assertTrue(t.bare)
        self.assertEqual(t.height(54.2, 0.3), B.wow_arena.NO_TERRAIN)
        self.assertTrue(t.hole(54.2, 0.3))
        self.assertEqual(t.triangles(0, 60, -30, 30), [])
        self.assertEqual(t.slope(54.2, 0.3), 0.0)
        self.assertFalse(B.wow_arena.Terrain(self.maps, 0).bare)

    def test_its_squares_are_cut_as_under_the_terrain(self):
        # The Stockade's entrance, z -18: under NO_TERRAIN by more than UNDER_CLEAR, so the cut takes its own floors.
        args = B.cut_args(B.wow_arena.Terrain(self.maps, 34), 34, B.centre_of(34), B.FIXED_ORIGIN, (0, 0), (54.2, 0.3, -18.3))
        self.assertIn('--under', args)


class OwnPlaces(Base):
    """Every WoW map has its own centre (wow_places.py)."""
    def test_a_map_has_its_own_centre(self):
        self.assertEqual(B.centre_of(0), B.FIXED_CENTRE)
        self.assertNotEqual(B.centre_of(1), B.FIXED_CENTRE)
        self.assertNotEqual(B.centre_of(36), B.centre_of(1))
        with self.assertRaises(KeyError):
            B.centre_of(999)

    def test_the_same_wow_point_on_two_maps_is_two_bg3_places(self):
        a, b = Fake(self.tmp), Fake(self.tmp)
        a.centre, b.centre = B.centre_of(0), B.centre_of(1)
        pa, pb = a.to_bg3((100.0, 200.0, 30.0)), b.to_bg3((100.0, 200.0, 30.0))
        self.assertGreater(abs(pa[1] - pb[1]), 1000.0)
        self.assertEqual(a.wow_xy(pa[0], pa[2]), b.wow_xy(pb[0], pb[2]))


class Moves(Base):
    def test_a_new_square_takes_a_free_subgrid(self):
        w = self.w
        w.placed = {0: (-1, -1), 1: (0, -1), 2: (-1, 0), 3: (0, 0)}
        w.members = [(None, B.FIXED_CENTRE, False)]
        self.assertEqual(w.moves([(0, 0), (1, 0)]), [(4, (1, 0))])


class SeenMoves(Base):
    """main() starts counting moves from world-move.json's Seq only when the file is this BG3 run's."""
    def write(self, body):
        B.MOVE.write_text(json.dumps(body))

    def setUp(self):
        super().setUp()
        p = mock.patch.object(B, 'MOVE', self.tmp / 'world-move.json')
        p.start()
        self.addCleanup(p.stop)

    def test_this_runs_request_was_done_before_the_load(self):
        self.write({'RunId': 'r1', 'Seq': 4, 'Wow': list(TARGET)})
        self.assertEqual(B.moves_done('r1'), 4)
        self.assertIsNone(B.move_request('r1', B.moves_done('r1')))

    def test_another_runs_request_says_nothing(self):
        self.write({'RunId': 'old', 'Seq': 7, 'Wow': list(TARGET)})
        self.assertEqual(B.moves_done('r1'), 0)
        self.write({'RunId': 'r1', 'Seq': 1, 'Wow': list(TARGET)})  # a fresh BG3's first move
        self.assertEqual(B.move_request('r1', 0)['Seq'], 1)

    def test_no_file_or_a_broken_one(self):
        self.assertEqual(B.moves_done('r1'), 0)
        B.MOVE.write_text('{')
        self.assertEqual(B.moves_done('r1'), 0)
        self.write([1, 2])
        self.assertEqual(B.moves_done('r1'), 0)


class Cover(Base):
    """After a move its cover stays until benilla's player stands at the new place (or COVER_WAIT_S)."""
    def setUp(self):
        super().setUp()
        self.lifted = []
        p = mock.patch.object(B, 'move_cover', lambda on: self.lifted.append(on))
        p.start()
        self.addCleanup(p.stop)
        self.w.land(TARGET)

    def test_held_until_benilla_is_there(self):
        now = time.time()
        self.assertFalse(B.cover_done(self.w, now))  # no camera stream yet
        self.camera(5000.0, 5000.0)
        self.assertFalse(B.cover_done(self.w, now))
        self.assertEqual(self.lifted, [])
        self.camera(TARGET[0] + 3, TARGET[1] - 2)
        self.assertTrue(B.cover_done(self.w, now))
        self.assertEqual(self.lifted, [False])

    def test_lifted_after_the_wait_without_benilla(self):
        self.camera(5000.0, 5000.0)
        self.assertTrue(B.cover_done(self.w, time.time() - B.COVER_WAIT_S - 1))
        self.assertEqual(self.lifted, [False])


class CoverTime(Base):
    """The move's cover is black about a second after its flag; the flag the player's command wrote counts."""
    def test_the_commands_flag_counts(self):
        flag = self.tmp / 'link/wowmove.flag'
        t = time.time() - 0.7
        flag.write_text('%.3f\n' % t)
        self.assertAlmostEqual(B.move_cover(True), t + B.COVER_BLACK_S, places=2)
        self.assertTrue(flag.exists())

    def test_no_flag_or_an_old_one_starts_now(self):
        flag = self.tmp / 'link/wowmove.flag'
        now = time.time()
        self.assertGreaterEqual(B.move_cover(True), now + B.COVER_BLACK_S)
        flag.write_text('%.3f\n' % (now - 600))
        self.assertGreaterEqual(B.move_cover(True), now + B.COVER_BLACK_S)
        B.move_cover(False)
        self.assertFalse(flag.exists())

class Move(Base):
    def request(self, **fields):
        return dict({'Seq': 3, 'Wow': list(TARGET), 'Map': 0}, **fields)

    def run_move(self, m):
        w = self.w
        w.placed = {0: (-1, -1), 1: (0, -1), 2: (-1, 0), 3: (0, 0)}
        started = []
        class New:
            def __init__(self, a, map_id=None):
                self.at = None

            def start(self, at):
                self.at = at
                started.append(at)
        with mock.patch.object(B, 'tav_flags', lambda *_: 0), mock.patch.object(B, 'camp_state', lambda *_: False), \
                mock.patch.object(B, 'move_cover', lambda *_: None), mock.patch.object(B.wow_arena, 'Terrain', lambda *_: Flat()), \
                mock.patch.object(B, 'World', New):
            result = w.move(m)
        return result, started, json.loads(B.MOVE_RESULT.read_text())

    def test_same_continent_is_stop_and_start_too(self):
        result, started, answer = self.run_move(self.request())
        self.assertIsNot(result, self.w)
        self.assertEqual([a for a, _ in self.w.sent], ['Stop'])
        self.assertEqual(started, [TARGET])
        self.assertEqual(answer['Value'], {'Origin': list(TARGET), 'Map': 0})
        self.assertFalse(self.active())

    def test_another_continent(self):
        with mock.patch.object(self.w, 'arrive', lambda t: None):
            result, started, answer = self.run_move(self.request(Map=1, Arrived=True))
        self.assertEqual([a for a, _ in self.w.sent], ['Stop'])
        self.assertEqual(answer['Value']['Map'], 1)


LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua'
try:
    import lupa
except ImportError:
    lupa = None


@unittest.skipIf(lupa is None, 'needs the lupa module')
class ArenaWait(unittest.TestCase):
    """WorldMode.lua's arena(): an action only the client carries out (Arena.lua ClientOnly) waits for the client's
    result alone; every other one for both. The real Lua text of both, under a fake file store."""
    def setUp(self):
        self.L = L = lupa.LuaRuntime(unpack_returned_tuples=True)
        L.execute(r'''
          files, ticks, runId = {}, 0, "run"
          Ext = {IO = {LoadFile = function(n) return files[n] end, SaveFile = function(n, v) files[n] = v end},
                 Json = {Parse = function(v) return load("return " .. v)() end,
                         Stringify = function(t) return string.format("{RunId = %q, Sequence = %d, Action = %q}",
                             t.RunId, t.Sequence, t.Action) end},
                 Require = function() return {} end, IsClient = function() return true end}
          function readJson(name)
            local raw = Ext.IO.LoadFile("wow-bg3/" .. name)
            if not raw then return nil end
            local ok, v = pcall(Ext.Json.Parse, raw)
            return ok and type(v) == "table" and v or nil
          end
          function answer(side, seq) files["wow-bg3/arena-" .. side .. "-result.json"] =
              string.format("{RunId = 'run', Sequence = %d, OK = true, Value = %q}", seq, side) end
        ''')
        arena_text = (LUA / 'Arena.lua').read_text()
        i = arena_text.index('M.ClientOnly = ')
        L.execute('Arena = {}; ' + arena_text[i:arena_text.index('\n\n', i)].replace('M.ClientOnly', 'Arena.ClientOnly'))
        text = (LUA / 'WorldMode.lua').read_text()
        body = text[text.index('    local function command(stem, c, results)'):text.index('    local function standIn(c)')]
        L.execute('function make()\n' + body + '\nreturn arena end\narena = make()')

    def run_arena(self, action, sides):
        L = self.L
        L.globals().action = action
        L.execute('co = coroutine.create(function() return arena({Action = action}) end); ok, value = coroutine.resume(co)')
        seq = L.eval('readJson("arena-command.json").Sequence')
        for side in sides:
            L.globals().answer(side, seq)
        for _ in range(400):
            if L.eval('coroutine.status(co)') == 'dead':
                break
            L.execute('ticks = ticks + 1; ok, value = coroutine.resume(co)')
        return L.eval('ok'), L.eval('value')

    def test_lead_waits_for_the_client_alone(self):
        self.assertEqual(self.run_arena('Lead', ['client']), (True, 'client'))
        self.assertEqual(self.run_arena('PickRegionOff', ['client']), (True, 'client'))

    def test_place_still_waits_for_both(self):
        ok, why = self.run_arena('Place', ['client'])
        self.assertFalse(ok)
        self.assertIn('no result', why)
        self.assertEqual(self.run_arena('Place', ['client', 'server']), (True, 'server'))


@unittest.skipIf(lupa is None, 'needs the lupa module')
class StatusReport(unittest.TestCase):
    """WorldMode.lua's world-status.json: a job of the mod's own (extend) ending after a command's job keeps the
    command's Seq there, so the bridge's wait_done still sees it (0.1.3013.61: a move's report lost)."""
    def setUp(self):
        self.L = L = lupa.LuaRuntime(unpack_returned_tuples=True)
        L.execute(r'''
          saved = nil
          Ext = {IO = {SaveFile = function(n, v) saved = v end}, Json = {Stringify = function(t) return t end}}
          M, runId, ticks, jobs, clicks, rclicks, state = {}, "run", 0, {}, 0, 0, nil
        ''')
        text = (LUA / 'WorldMode.lua').read_text()
        body = text[text.index('    -- report: how the bridge'):text.index("    -- BG3's own mute")]
        L.execute('function make()\n' + body + '\nreturn jobDone, writeStatus, function() return lastExtra end end\n'
                  'jobDone, writeStatus = make()')

    def status(self):
        s = self.L.globals().saved
        return {k: s[k] for k in ('Last', 'Seq', 'OK', 'Error')}, s['Chore']

    def test_an_extend_after_a_command_keeps_its_seq(self):
        self.L.execute('jobDone({name = "Stop", seq = 7}, true)')
        self.L.execute('jobDone({name = "extend"}, false, "Extend: no result")')
        report, chore = self.status()
        self.assertEqual(report, {'Last': 'Stop', 'Seq': 7, 'OK': True, 'Error': None})
        self.assertEqual((chore['Name'], chore['OK'], chore['Error']), ('extend', False, 'Extend: no result'))
        self.L.execute('writeStatus()')  # the 600-tick status too
        self.assertEqual(self.status()[0]['Seq'], 7)

    def test_the_next_command_replaces_the_report(self):
        self.L.execute('jobDone({name = "extend"}, true); jobDone({name = "Stream", seq = 8}, false, "Place refused")')
        report, chore = self.status()
        self.assertEqual(report, {'Last': 'Stream', 'Seq': 8, 'OK': False, 'Error': 'Place refused'})
        self.assertIsNone(chore)


FAKE_ARENA = """import sys, pathlib
out = pathlib.Path(sys.argv[1])
if '--fail' in sys.argv:
    sys.exit(3)
(out / 'arena.json').write_text('{}')
"""


class Cache(Base):
    def setUp(self):
        super().setUp()
        fake = self.tmp / 'fake'
        fake.mkdir()
        (fake / 'wow_arena.py').write_text(FAKE_ARENA)
        (fake / 'wow_stand.py').write_text('')
        (fake / 'wow_vmap.py').write_text('')
        for p in (mock.patch.object(B.wow_arena, '__file__', str(fake / 'wow_arena.py')), mock.patch.object(B, 'CUT_CACHE', None),
                  mock.patch.dict(os.environ, {'WOW_CUT_CACHE': str(self.tmp / 'cuts')})):
            p.start()
            self.addCleanup(p.stop)

    def args(self, ij=(0, 0), at=TARGET):
        return B.cut_args(Flat(), 0, B.FIXED_CENTRE, B.FIXED_ORIGIN, ij, at)

    def test_the_bridge_reads_what_prewarm_cut(self):
        self.w.land(TARGET)
        cmd, out = B.World.cut_cmd(self.w, (0, 0))
        self.assertTrue(cmd)
        self.assertEqual(out, B.CELLS / '0_0')
        self.assertTrue(B.store_cut(self.args()))
        cmd, out = B.World.cut_cmd(self.w, (0, 0))
        self.assertIsNone(cmd)
        self.assertTrue((out / 'arena.json').exists())

    def test_another_square_is_another_cut(self):
        B.store_cut(self.args())
        self.assertIsNone(B.cached_cut(self.args((1, 0))))

    def test_sessions_over_the_ground_share_a_cut_and_one_under_it_has_its_own(self):
        B.store_cut(self.args())
        self.assertIsNotNone(B.cached_cut(self.args(at=(TARGET[0] + 40, TARGET[1], 100.0))))
        under = self.args(at=(TARGET[0], TARGET[1], 100.0 - B.wow_arena.UNDER_CLEAR - 5))
        self.assertIn('--under', under)
        self.assertIsNone(B.cached_cut(under))

    def test_a_failed_cut_leaves_nothing(self):
        self.assertFalse(B.store_cut(self.args() + ['--fail']))
        self.assertIsNone(B.cached_cut(self.args() + ['--fail']))
        root, stamp = B.CUT_CACHE
        self.assertEqual(list((root / stamp).iterdir()), [])

    def test_new_cutting_code_prunes_the_old_cuts(self):
        B.store_cut(self.args())
        root, old = B.CUT_CACHE
        (Path(B.wow_arena.__file__).with_name('wow_vmap.py')).write_text('# changed')
        B.CUT_CACHE = None
        self.assertIsNone(B.cached_cut(self.args()))
        B.prune_cuts()
        self.assertFalse((root / old).exists())

    def test_prewarm_cuts_what_a_move_there_asks_for(self):
        point = {'id': 2, 'map': 0, 'x': -8840.25, 'y': 489.5, 'z': 109.125, 'name': 'Stormwind', W.ALLIANCE: 541, W.HORDE: 0}
        stored = []
        with mock.patch.object(W, 'nodes', lambda _: [point]), mock.patch.object(W, 'stand_z', lambda m, x, y, z, t: 104.5), \
                mock.patch.object(B.wow_arena, 'ensure_mmtiles', lambda *_, **__: None), mock.patch.object(B.wow_arena, 'Terrain', lambda *_: Flat()), \
                mock.patch.object(B, 'store_cut', stored.append):
            self.assertEqual(W.prewarm('dbc', 2), 0)
        self.assertEqual(len(stored), 4)
        # A session landing on the point asks for the same squares of the level's lattice, whatever came before it.
        self.w.land((point['x'], point['y'], 104.5))
        S = self.w.spot()
        node = B.node_of(self.w.centre, S[0], S[2])
        first = (node[0] - 1, node[1] - 1)
        self.assertEqual(self.args(first, self.w.at), stored[0])
        self.assertNotIn('--under', stored[0])   # over the ground: the cut is the same for every session
        B.store_cut(stored[0])
        self.assertIsNone(B.World.cut_cmd(self.w, first)[0])

    def test_squares_as_the_window_wants_them(self):
        core, rest = W.square_order(4)
        self.w.a.floor = 4
        h = self.w.walker('h', 'h')
        h.node = (0, 0)
        want, block = self.w.window(h, self.w.centre[0], self.w.centre[2])
        self.assertEqual(sorted(core), sorted(want))
        self.assertEqual(sorted(rest), sorted(block))
        self.assertEqual(W.square_order(2)[1], [])


class FarSight(Base):
    """Far Sight follows the player's control on its own thread, whatever the ground's round waits for (0.1.3013.60: a
    round building two movement tiles for 68 s held the F2 to Astarion 699 m off until after the F1 back)."""
    AST = 'c7c13742-bacd-460a-8f65-f864fe41f255'

    def setUp(self):
        super().setUp()
        self.party((-701.0, 121.0, -350.0))
        self.control('hero-1')
        self.sight = B.FarSight(self.w)

    def party(self, pos):
        (self.tmp / 'party.json').write_text(json.dumps({'RunId': 'r1', 'Members': [
            {'Uuid': 'hero-1', 'Name': 'Tav', 'Hero': True, 'Pos': [-1400.0, 120.0, -350.0]},
            {'Uuid': self.AST, 'Name': 'Astarion', 'Pos': list(pos)}]}))

    def control(self, uuid):
        (self.tmp / 'link/mirror-status.json').write_text(json.dumps({'Tav': {'Controlled': uuid}}))

    def sent(self):
        try:
            return json.loads((self.tmp / 'link/farsight.json').read_text())
        except FileNotFoundError:
            return None

    def test_on_the_member_and_back_on_the_hero(self):
        self.sight.step()
        self.assertIsNone(self.sent())
        self.control(self.AST)
        self.sight.step()
        fs = self.sent()
        self.assertEqual((fs['Seq'], fs['On'], fs['Uuid'], fs['Name'], fs['Map']), (1, True, self.AST, 'Astarion', 0))
        wx, wy = self.w.wow_xy(-701.0, -350.0)
        self.assertEqual(fs['Wow'][:2], [round(wx, 2), round(wy, 2)])
        self.sight.step()
        self.assertEqual(self.sent()['Seq'], 1)
        self.control('hero-1')
        self.sight.step()
        self.assertEqual((self.sent()['Seq'], self.sent()['On']), (2, False))

    def test_a_member_in_the_level_gets_none(self):
        (self.tmp / 'world-status.json').write_text(json.dumps({'RunId': 'r1', 'LevelBounds': [-800, -500, 300, 100]}))
        self.control(self.AST)
        self.sight.step()
        self.assertIsNone(self.sent())

    def test_a_move_holds_it(self):
        self.control(self.AST)
        with self.sight.held:
            self.sight.step()
            self.assertIsNone(self.sent())
        self.sight.step()
        self.assertTrue(self.sent()['On'])

    def test_a_round_waiting_for_its_cuts_does_not_hold_it(self):
        raw = bytearray(44)
        raw[:4] = b'TAV1'
        struct.pack_into('<I7fII', raw, 4, 1, 0.0, 0.0, 100.0, -1400.0, 120.0, -350.0, 0.0, 0, 1)
        (self.tmp / 'link/tav.bin').write_bytes(bytes(raw))
        cutting, done = threading.Event(), threading.Event()

        def precut(squares):  # the movement tiles of a place never visited: up to a minute
            cutting.set()
            done.wait(10)
        self.w.precut = precut
        round_ = threading.Thread(target=self.w.follow)
        round_.start()
        self.addCleanup(round_.join)
        self.addCleanup(done.set)
        self.assertTrue(cutting.wait(5))
        self.sight.start()
        self.addCleanup(self.sight.stop)
        self.control(self.AST)   # F2 while the round cuts
        end = time.time() + 2.0
        while time.time() < end and not self.sent():
            time.sleep(0.05)
        self.assertTrue(round_.is_alive())
        self.assertEqual(self.sent()['Uuid'], self.AST)

    def test_stop_turns_it_off(self):
        self.control(self.AST)
        self.sight.step()
        self.sight.stop()
        self.assertFalse(self.sent()['On'])


if __name__ == '__main__':
    unittest.main()
