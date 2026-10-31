#!/usr/bin/env python3
"""test_wow_waypoints.py: the waypoint rules and the chat verb, without a game, a server or WoW's data
(made-up DBC files; WBDBC=FOLDER also checks the real TaxiNodes.dbc and TaxiPath.dbc there).

  python3 tools/test_wow_waypoints.py
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
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import wow_waypoints as W  # noqa: E402
import wowbg3cmd as C  # noqa: E402


def write_dbc(path, rows, fields):
    """rows: lists of int, float or str, `fields` long."""
    strs = b'\0'
    packed = []
    for row in rows:
        rec = b''
        for v in row:
            if isinstance(v, str):
                rec += struct.pack('<I', len(strs))
                strs += v.encode() + b'\0'
            elif isinstance(v, float):
                rec += struct.pack('<f', v)
            else:
                rec += struct.pack('<I', v)
        packed.append(rec)
    path.write_bytes(struct.pack('<4s4I', b'WDBC', len(rows), fields, fields * 4, len(strs)) + b''.join(packed) + strs)


def node(i, m, x, y, z, name, horde, alliance):
    return [i, m, float(x), float(y), float(z), name] + [0] * 8 + [horde, alliance]


NODES = [
    node(1, 0, -8889, 0, 94, 'Northshire Abbey', 308, 0),      # no flight master
    node(2, 0, -8840, 489, 109, 'Stormwind, Elwynn', 0, 541),
    node(5, 0, -9429, -2231, 68, 'Lakeshire, Redridge', 0, 541),
    node(6, 0, -4821, -1155, 502, 'Ironforge, Dun Morogh', 0, 541),  # under its terrain
    node(18, 0, -14444, 509, 26, 'Booty Bay, Stranglethorn', 2224, 0),
    node(19, 0, -14473, 464, 36, 'Booty Bay, Stranglethorn', 0, 541),
    node(23, 1, 1677, -4315, 61, 'Orgrimmar, Durotar', 2224, 0),
    node(26, 1, 6341, 557, 16, 'Auberdine, Darkshore', 0, 3837),
    node(34, 1, -1965, -5824, -1, 'Transport, Booty Bay - Ratchet', 0, 0),
    node(59, 30, 574, -46, 37, 'Dun Baldar, Alterac Valley', 0, 541),
    node(80, 1, -894, -3773, 11, 'Ratchet, The Barrens', 2224, 541),
    node(85, 0, 3109, -4285, 109, 'Northpass Tower, Eastern Plaguelands', 3574, 541),
]
AREAS = [
    [14, 0, 0, 'Azeroth', 16000.0, -19200.0, 7466.0, -16000.0],
    [1, 0, 12, 'Elwynn', 1066.0, -2133.0, -7466.0, -9600.0],
    [4, 1, 14, 'Durotar', -1962.0, -7250.0, 1808.0, -1716.0],
    [13, 1, 0, 'Kalimdor', 17066.0, -19733.0, 12800.0, -11733.0],
]


def mask_of(*ids):
    words = [0] * 8
    for i in ids:
        words[(i - 1) // 32] |= 1 << ((i - 1) % 32)
    return ' '.join(str(w) for w in words) + ' '


class Rules(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        write_dbc(d / 'TaxiNodes.dbc', NODES, 16)
        write_dbc(d / 'WorldMapArea.dbc', AREAS, 8)
        cls.dir = d
        cls.nodes = W.nodes(d)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def ids(self, mask, team):
        return [p['id'] for p in W.waypoints(self.nodes, mask, team)]

    def test_mask_bits(self):
        self.assertEqual(W.known_ids(mask_of(1, 2, 32, 33, 64, 65)), {1, 2, 32, 33, 64, 65})
        self.assertEqual(W.known_ids(''), set())
        self.assertEqual(W.known_ids(None), set())
        self.assertEqual(W.known_ids('NULL'), set())

    def test_read_dbc_fields(self):
        n = next(n for n in self.nodes if n['id'] == 80)
        self.assertEqual((n['map'], n['name'], n[W.HORDE], n[W.ALLIANCE]), (1, 'Ratchet, The Barrens', 2224, 541))
        self.assertAlmostEqual(n['x'], -894.0)

    def test_only_discovered(self):
        self.assertEqual(self.ids(mask_of(2), W.ALLIANCE), [2])
        self.assertEqual(self.ids('', W.ALLIANCE), [])

    def test_faction_and_twins(self):
        both = mask_of(18, 19, 23, 26, 80)
        self.assertEqual(self.ids(both, W.ALLIANCE), [19, 26, 80])
        self.assertEqual(self.ids(both, W.HORDE), [18, 23, 80])

    def test_not_waypoints(self):
        self.assertEqual(self.ids(mask_of(1, 34, 59, 85), W.ALLIANCE), [])
        self.assertEqual(self.ids(mask_of(1, 34, 59, 85), W.HORDE), [])

    def test_race_team(self):
        self.assertEqual([W.team_of(r) for r in (1, 2, 3, 4, 5, 6, 7, 8)],
                         [W.ALLIANCE, W.HORDE, W.ALLIANCE, W.ALLIANCE, W.HORDE, W.HORDE, W.ALLIANCE, W.HORDE])
        self.assertIsNone(W.team_of(99))

    def test_choose(self):
        points = W.waypoints(self.nodes, mask_of(2, 5, 19), W.ALLIANCE)
        self.assertEqual([p['id'] for p in W.choose(points, 'stormwind')], [2])
        self.assertEqual([p['id'] for p in W.choose(points, 'REDRIDGE')], [5])
        self.assertEqual([p['id'] for p in W.choose(points, '19')], [19])
        self.assertEqual(W.choose(points, '23'), [])
        self.assertEqual(W.choose(points, 'nowhere'), [])
        self.assertEqual(len(W.choose(points, ',')), 3)

    def test_pins(self):
        points = W.waypoints(self.nodes, mask_of(2, 23), W.ALLIANCE) + W.waypoints(self.nodes, mask_of(23), W.HORDE)
        rows = W.pins(points, W.map_rects(self.dir))
        self.assertEqual(sorted((f, i) for f, i, *_ in rows),
                         [('Azeroth', 2), ('Durotar', 23), ('Elwynn', 2), ('Kalimdor', 23)])
        f, _, x, y, _ = next(r for r in rows if r[0] == 'Elwynn')
        m = {'left': 1066.0, 'right': -2133.0, 'top': -7466.0, 'bottom': -9600.0}
        self.assertAlmostEqual(x, C.to_map(m, -8840.0, 489.0)[0], places=2)
        self.assertAlmostEqual(y, C.to_map(m, -8840.0, 489.0)[1], places=2)

    def test_nearest_floor(self):
        self.assertEqual(W.nearest_floor(100.0, [97.0, 130.0]), 97.0)
        self.assertEqual(W.nearest_floor(-43.0, [60.0, -40.0, -70.0]), -40.0)  # Undercity under its ruins
        self.assertEqual(W.nearest_floor(100.0, [150.0]), 100.0)
        self.assertEqual(W.nearest_floor(100.0, []), 100.0)

    def test_switch(self):
        with mock.patch.dict(os.environ, {}, clear=True):
            self.assertFalse(W.enabled())
        with tempfile.TemporaryDirectory() as home:
            conf = Path(home) / 'settings.conf'
            conf.write_text('waypoints = 0\n')
            with mock.patch.dict(os.environ, {'WOWBG3_HOME': home}, clear=True):
                self.assertFalse(W.enabled())
                conf.write_text('# waypoints = 1\nwaypoints = 1 # yes\n')
                self.assertTrue(W.enabled())
            with mock.patch.dict(os.environ, {'WOWBG3_HOME': home, 'WOWBG3_WAYPOINTS': '0'}, clear=True):
                self.assertFalse(W.enabled())
        with mock.patch.dict(os.environ, {'WOWBG3_WAYPOINTS': '1'}, clear=True):
            self.assertTrue(W.enabled())


class Verb(unittest.TestCase):
    """`/wowbg3 waypoint ...` through wowbg3cmd.run, up to the world bridge's files (a stand-in answers them)."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        self.dbc, self.io = d / 'dbc', d / 'io'
        self.link = self.io / 'link'
        self.dbc.mkdir()
        self.link.mkdir(parents=True)
        write_dbc(self.dbc / 'TaxiNodes.dbc', NODES, 16)
        write_dbc(self.dbc / 'WorldMapArea.dbc', AREAS, 8)
        (self.io / 'm3-client.json').write_text(json.dumps({'RunId': 'r1'}))
        (self.link / 'world.json').write_text(json.dumps({'Active': True}))
        self.camera(-8840.0, 489.0, 0)
        self.tav()
        self.lines, self.errors = [], []
        self.mask, self.race, self.flight, self.health = mask_of(2, 26), 1, '', 80
        self.sent = []
        for p in (mock.patch.object(C, 'sql', self.sql), mock.patch.object(C, 'console', self.console),
                  mock.patch.object(C, 'dbc_dir', lambda: str(self.dbc)),
                  mock.patch.dict(os.environ, {'WOWBG3_WAYPOINTS': '1'})):
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(self.tmp.cleanup)

    def camera(self, x, y, m):
        raw = bytearray(96)
        raw[:4] = b'WBC1'
        struct.pack_into('<3f', raw, 72, x, y, 50.0)
        struct.pack_into('<II', raw, 84, m, 1)
        (self.link / 'camera.bin').write_bytes(bytes(raw))

    def tav(self, flags=0):
        raw = bytearray(44)
        raw[:4] = b'TAV1'
        struct.pack_into('<I', raw, 36, flags)
        (self.link / 'tav.bin').write_bytes(bytes(raw))

    def console(self, line, seconds=0):
        self.sent.append(line)
        return ''

    def sql(self, q, db=''):
        if q.startswith('show columns'):
            return 'known_taxi_mask\tlongtext\tYES\t\tNULL\t\ncurrent_taxi_path\ttext\tYES\t\tNULL\t\n'
        if 'known_taxi_mask' in q:
            return '%d\t%d\t%s\t%s\n' % (self.race, self.health, self.mask, self.flight)
        if q.startswith('select level'):
            return '10 1\n'
        if 'c.online = 1' in q:
            return 'Hero\tWB3PAAAAAAAA\n'
        if 'game_tele' in q:
            return 'Stormwind\t0\t-8842\t500\t110\nTeldrassil\t1\t8700\t900\t20\nAshenvale\t1\t2800\t-300\t110\n'
        raise AssertionError(q)

    def run_cmd(self, text):
        ctx = C.Ctx(io=self.io, link=self.link, char='Hero', out=lambda l, error=False: (self.errors if error else self.lines).append(l), chat=True)
        C.run(ctx, text)

    def bridge(self, then_camera):
        """The world bridge and benilla for one move: answer the request and stand there."""
        def go():
            path = self.io / 'world-move.json'
            for _ in range(200):
                if path.exists():
                    break
                time.sleep(0.02)
            self.request = json.loads(path.read_text())
            (self.io / 'world-move-result.json').write_text(json.dumps({'RunId': 'r1', 'Seq': self.request['Seq'], 'OK': True}))
            self.camera(*then_camera)
        t = threading.Thread(target=go)
        t.start()
        self.addCleanup(t.join)

    def test_off_refuses(self):
        with mock.patch.dict(os.environ, {'WOWBG3_WAYPOINTS': '0'}):
            with self.assertRaisesRegex(C.CmdError, 'waypoints are off'):
                self.run_cmd('waypoint')
            self.assertFalse(any('waypoint' in l for l in C.help_lines(True)))
        self.assertEqual(self.sent, [])

    def test_not_a_test_command(self):
        ctx = C.Ctx(io=self.io, link=self.link, char='Hero', debug=False, out=lambda l, error=False: self.lines.append(l))
        C.run(ctx, 'waypoint')
        self.assertIn('2 Stormwind, Elwynn (Eastern Kingdoms)', ' '.join(self.lines))

    def test_list(self):
        self.run_cmd('waypoint')
        text = '\n'.join(self.lines)
        self.assertIn('Stormwind, Elwynn', text)
        self.assertIn('Auberdine, Darkshore', text)
        self.assertNotIn('Orgrimmar', text)
        self.assertEqual(self.sent, ['saveall'])  # discoveries since the last save count

    def test_pins_lines(self):
        self.run_cmd('waypoint pins')
        self.assertEqual(self.lines[-1], 'WBWP;end;3')
        self.assertTrue(all(l.startswith('WBWP;') for l in self.lines))
        self.assertEqual(self.lines[0].split(';')[:4], ['WBWP', 'pin', 'Azeroth', '2'])

    def test_same_continent_move(self):
        self.camera(-9429.0, -2231.0, 0)
        self.bridge((-8840.0, 489.0, 0))
        self.run_cmd('waypoint go stormwind')
        r = self.request
        self.assertEqual((r['Map'], r['What'], r['Snap']), (0, 'Stormwind, Elwynn', True))
        self.assertEqual(r['Wow'], [node(2, 0, -8840, 489, 109, '', 0, 0)[2], 489.0, 109.0])
        self.assertNotIn('Via', r)
        self.assertIn('there', self.lines[-1])

    def test_other_continent_goes_through_a_named_place(self):
        self.bridge((6341.0, 557.0, 1))
        self.run_cmd('waypoint go 26')
        r = self.request
        self.assertEqual((r['Map'], r['Char'], r['Via']), (1, 'Hero', 'Teldrassil'))
        self.assertTrue(r['Snap'])

    def test_undiscovered_refused(self):
        with self.assertRaisesRegex(C.CmdError, 'discovered no flight point'):
            self.run_cmd('waypoint go lakeshire')
        self.assertFalse((self.io / 'world-move.json').exists())

    def test_other_team_refused(self):
        self.mask, self.race = mask_of(2, 23), 2
        with self.assertRaisesRegex(C.CmdError, 'discovered no flight point'):
            self.run_cmd('waypoint go stormwind')

    def test_ambiguous(self):
        with self.assertRaisesRegex(C.CmdError, 'which one'):
            self.run_cmd('waypoint go ,')

    def test_in_fight_refused(self):
        self.tav(flags=1)
        with self.assertRaisesRegex(C.CmdError, '^waypoint: not during a BG3 fight'):
            self.run_cmd('waypoint go stormwind')
        self.assertFalse((self.io / 'world-move.json').exists())

    def test_at_camp_refused(self):
        (self.io / 'camp-state.json').write_text(json.dumps({'RunId': 'r1', 'Seq': 1, 'InCamp': True}))
        for text in ('waypoint go stormwind', 'tp 50 50'):
            with self.assertRaisesRegex(C.CmdError, 'leave camp first'):
                self.run_cmd(text)
        self.assertFalse((self.io / 'world-move.json').exists())

    def test_camp_record_of_another_run_or_a_finished_trip(self):
        for body in ({'RunId': 'old', 'InCamp': True}, {'RunId': 'r1', 'InCamp': False}):
            (self.io / 'camp-state.json').write_text(json.dumps(body))
            self.assertFalse(C.at_camp(self.io))

    def test_on_a_flight_refused(self):
        self.flight = '12 0 1 2'
        with self.assertRaisesRegex(C.CmdError, 'not on a WoW flight'):
            self.run_cmd('waypoint go stormwind')

    def test_dead_in_wow_refused_before_any_move(self):
        self.health = 0
        with self.assertRaisesRegex(C.CmdError, 'dead in WoW'):
            self.run_cmd('waypoint go stormwind')
        self.assertFalse((self.io / 'world-move.json').exists())

    def test_underground_capital_says_why(self):
        self.mask = mask_of(2, 6)
        with self.assertRaisesRegex(C.CmdError, 'no ground inside'):
            self.run_cmd('waypoint go ironforge')
        self.run_cmd('waypoint')
        self.assertNotIn('Ironforge', '\n'.join(self.lines))

    def test_null_path_is_not_a_flight(self):
        self.flight = 'NULL'
        self.camera(-9429.0, -2231.0, 0)
        self.bridge((-8840.0, 489.0, 0))
        self.run_cmd('waypoint go stormwind')

    def test_usage(self):
        for text in ('waypoint go', 'waypoint nonsense', 'waypoint list extra'):
            with self.assertRaises(C.Usage):
                self.run_cmd(text)


@unittest.skipUnless(os.environ.get('WBDBC'), 'WBDBC: a folder with the real TaxiNodes.dbc and TaxiPath.dbc')
class RealData(unittest.TestCase):
    def setUp(self):
        self.dir = Path(os.environ['WBDBC'])
        self.nodes = W.nodes(self.dir)
        self.by_id = {n['id']: n for n in self.nodes}

    def test_format(self):
        self.assertEqual(self.by_id[2]['name'], 'Stormwind, Elwynn')
        self.assertEqual((self.by_id[23][W.HORDE] != 0, self.by_id[23][W.ALLIANCE] != 0), (True, False))
        self.assertEqual((self.by_id[2][W.HORDE] != 0, self.by_id[2][W.ALLIANCE] != 0), (False, True))

    def test_no_master_is_exact(self):
        used = set()
        for i, _, _ in W.read_dbc(self.dir / 'TaxiPath.dbc'):
            used |= {i[1], i[2]}
        on_map = [n for n in self.nodes if n['map'] in W.CONTINENTS and (n[W.HORDE] or n[W.ALLIANCE])]
        self.assertEqual({n['id'] for n in on_map if n['id'] not in used}, {1, 3, 9, 15, 57})
        for n in on_map:
            if n['id'] not in W.NO_MASTER:
                self.assertFalse(n['name'].startswith(('Transport,', 'Generic,')), n)
                self.assertNotEqual((n['x'], n['y']), (0.0, 0.0), n)
                self.assertLess(abs(n['x']), 15000, n)

    def test_capitals_by_race(self):
        # What WoW gives a new character: its race's capital, always a waypoint of its team.
        capitals = {W.ALLIANCE: (2, 26, 27), W.HORDE: (22, 23)}
        for team, ids in capitals.items():
            for i in ids:
                self.assertTrue(W.can_be_waypoint(self.by_id[i], team), i)

    def test_the_underground_capitals_are_not_waypoints(self):
        self.assertEqual(self.by_id[6]['name'].split(',')[0], 'Ironforge')
        self.assertEqual(self.by_id[11]['name'].split(',')[0], 'Undercity')
        self.assertFalse(W.can_be_waypoint(self.by_id[6], W.ALLIANCE))
        self.assertFalse(W.can_be_waypoint(self.by_id[11], W.HORDE))


if __name__ == '__main__':
    unittest.main()
