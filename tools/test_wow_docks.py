"""WoW's ships and zeppelins as crossings between docks (tools/wow_docks.py) and the chat command that takes one
(`/wowbg3 cross`), without a game or a server: a small TaxiPathNode.dbc made here, the movement map faked."""
import importlib.util
import math
import struct
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import wow_docks  # noqa: E402


def dbc(rows):
    d = Path(tempfile.mkdtemp())
    body = b''.join(struct.pack('<4I3f2I', i + 1, *r) for i, r in enumerate(rows))
    (d / 'TaxiPathNode.dbc').write_bytes(b'WDBC' + struct.pack('<4I', len(rows), 9, 36, 1) + body + b'\0')
    return d


# path, index, map, x, y, z, flags, delay
NODES = [
    (295, 0, 0, -3709.5, -575.1, 0.0, 2, 60),      # a ship: Wetlands
    (295, 1, 0, -3800.0, -700.0, 0.0, 0, 0),       # open sea
    (295, 2, 1, 6406.2, 823.1, 0.0, 2, 60),        # Darkshore
    (295, 3, 0, -3709.5, -575.1, 0.0, 2, 60),      # home again: the same dock
    (302, 0, 1, 1318.1, -4658.0, 71.9, 2, 60),     # a zeppelin: Durotar
    (302, 1, 0, 2062.4, 293.0, 115.0, 2, 60),      # Tirisfal
    (436, 0, 0, 3133.0, -3433.7, 231.9, 2, 60),    # one stop: no crossing
    (436, 1, 0, 3200.0, -3400.0, 231.9, 0, 0),
]
ROWS = [('176310', "Serenity's Shore", '295'), ('164871', 'Zeppelin', '302'), ('181056', 'Naxxramas', '436'), ('1', 'no path', '999')]


def table():
    return wow_docks.routes(ROWS, wow_docks.read_nodes(dbc(NODES)))


def test_a_route_is_a_path_with_two_different_stops():
    r = table()
    assert [x['Entry'] for x in r] == [164871, 176310]
    ship = r[1]
    assert [(s['Map'], round(s['X'], 1)) for s in ship['Stops']] == [(0, -3709.5), (1, 6406.2)]
    assert ship['Stops'][0]['Wait'] == 60 and ship['Name'] == "Serenity's Shore"


def test_at_a_dock_and_not():
    r = table()
    here = wow_docks.near(r, 0, -3700.0, -560.0)
    assert len(here) == 1 and here[0][0]['Entry'] == 176310 and here[0][1] == 0 and here[0][2] < 20
    assert [s['Map'] for s in wow_docks.other(here[0][0], here[0][1])] == [1]
    assert wow_docks.near(r, 1, -3700.0, -560.0) == []          # the same numbers on the other continent
    assert wow_docks.near(r, 0, -3709.5, -575.1 + wow_docks.REACH + 1) == []


def col(floors):
    """floors(x, y) -> [(height, joined size, flags)]; the largest joined piece is 100 polygons."""
    return lambda x, y: (floors(x, y), 100)


def test_a_ships_dock_is_above_the_water_and_joined_to_the_world():
    # East of the stop: the sea floor (-12), a pier at 5.5 from 14 yards out, a mast's island (3 polygons) nearer.
    def floors(x, y):
        d = x - 100.0
        out = [(-12.0, 100, 0)]
        if abs(y - 200.0) < 4 and d > 13.0:
            out.append((5.5, 100, 0))
        if abs(y - 200.0) < 4 and 5.0 < d <= 13.0:
            out.append((6.0, 3, 0))
        return out
    at = wow_docks.landing(0, 100.0, 200.0, 0.0, column=col(floors))
    assert at is not None and at[2] == 5.5 and 13.0 < math.hypot(at[0] - 100.0, at[1] - 200.0) <= 18.0, at


def test_a_zeppelins_dock_is_beside_it_not_the_ground_far_below():
    def floors(x, y):
        out = [(20.0, 100, 0)]                                   # the ground under the tower
        if math.hypot(x - 100.0, y - 200.0) > 9.0:
            out.append((54.0, 100, 0))                           # the platform, 18 yards under the hull's middle
        return out
    at = wow_docks.landing(1, 100.0, 200.0, 71.9, column=col(floors))
    none = wow_docks.landing(1, 100.0, 200.0, 200.0, column=col(floors))
    assert at is not None and at[2] == 54.0, at
    assert none is None


def test_the_command_is_wired():
    spec = importlib.util.spec_from_file_location('wowbg3cmd', ROOT / 'tools/wowbg3cmd.py')
    w = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(w)
    # A player crosses by the guardian at the dock (DockGuide.lua); the typed command is a test command.
    assert 'cross' in w.VERBS and 'cross' in w.TEST_ONLY
    assert w.verb_of(['cross']) == ('cross', []) and w.verb_of(['cross', 'list']) == ('cross', ['list'])
    assert not any('/wowbg3 cross' in l for l in w.help_lines(False))
    assert any('/wowbg3 cross' in l for l in w.help_lines(True))
    chat = (ROOT / 'tools/wowbg3-chatcmd.py').read_text()
    assert "verb == 'cross'" in chat
    # The guardian's "yes" runs it by the crossing's key, with test commands on or off.
    assert "'cross #%s' % json.loads(dock.read_text())['Key']" in chat and "(replies, seq, None, text, said, True)" in chat
    assert '"wow_docks.py"' in (ROOT / 'release/build-release.py').read_text()


def test_a_docks_area_is_the_servers_own():
    # A map file as the server's extractor writes its area part: the file's header (the area part's offset at
    # byte 8), then 'AREA', flags, the whole file's area, and the 16 x 16 grid unless flag 1 says one area.
    import wow_arena
    d = Path(tempfile.mkdtemp())
    grid = [7] * 256
    x, y = 6418.3, 816.1                      # Auberdine's pier, tile 19 30
    lx, ly = int(16 * (32 - x / wow_arena.GRID)) & 15, int(16 * (32 - y / wow_arena.GRID)) & 15
    grid[lx * 16 + ly] = 442
    (d / '0011930.map').write_bytes(b'MAPS' + b'v1.4' + struct.pack('<2I', 40, 520) + b'\0' * 24
                                    + b'AREA' + struct.pack('<2H', 0, 0) + struct.pack('<256H', *grid))
    (d / '0011830.map').write_bytes(b'MAPS' + b'v1.4' + struct.pack('<2I', 40, 8) + b'\0' * 24 + b'AREA' + struct.pack('<2H', 1, 148))
    assert wow_docks.area_bit(d, 1, x, y) == 442
    assert wow_docks.area_bit(d, 1, x + 40.0, y) == 7
    assert wow_docks.area_bit(d, 1, x + wow_arena.GRID, y) == 148        # the next file north: one area
    assert wow_docks.area_bit(d, 1, x - wow_arena.GRID, y) is None       # no file: open sea


def test_a_dock_is_named_by_its_area_and_zone():
    spec = importlib.util.spec_from_file_location('wowbg3cmd', ROOT / 'tools/wowbg3cmd.py')
    w = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(w)
    w.zones = lambda: [{'id': 17, 'map': 1, 'parent': 0, 'bit': 30, 'name': 'The Barrens'},
                       {'id': 392, 'map': 1, 'parent': 17, 'bit': 301, 'name': 'Ratchet'},
                       {'id': 85, 'map': 0, 'parent': 0, 'bit': 301, 'name': 'Tirisfal Glades'}]
    w.cont = lambda m: {0: 'Eastern Kingdoms', 1: 'Kalimdor'}[m]
    bits = {(1, 10.0): 301, (0, 10.0): 301, (1, 20.0): 30, (1, 30.0): 9999}
    wow_docks.area_bit = lambda maps, m, x, y: bits.get((m, x))
    try:
        assert w.zone_at(1, 10.0, 0.0) == 'Ratchet, The Barrens'       # an area in a zone
        assert w.zone_at(0, 10.0, 0.0) == 'Tirisfal Glades'            # the same value on another map: its own zone
        assert w.zone_at(1, 20.0, 0.0) == 'The Barrens'
        assert w.zone_at(1, 30.0, 0.0) == 'Kalimdor'                   # a value the table does not name
        assert w.zone_at(1, 40.0, 0.0) == 'Kalimdor'                   # no map file there
    finally:
        importlib.reload(wow_docks)


def test_the_guardians_panel_and_its_yes():
    """dock-panel.json: the crossings from the docks near WoW's character, each with where to stand at this pier and
    the far place's name; `cross #KEY` takes exactly that one."""
    import json
    spec = importlib.util.spec_from_file_location('wowbg3cmd', ROOT / 'tools/wowbg3cmd.py')
    w = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(w)
    link = Path(tempfile.mkdtemp())
    raw = bytearray(96)
    struct.pack_into('<3f', raw, 72, -3700.0, -560.0, 6.0)      # at the Wetlands pier
    struct.pack_into('<I', raw, 84, 0)
    (link / 'camera.bin').write_bytes(bytes(raw))
    w.crossings = table
    w.zone_at = lambda m, x, y: {0: 'Menethil Harbor, Wetlands', 1: 'Auberdine, Darkshore'}[m]
    stood = []
    landing = wow_docks.landing
    wow_docks.landing = lambda m, x, y, z, **k: stood.append((m, x, y)) or (x + 3.0, y + 4.0, z + 6.0)
    try:
        ctx = w.Ctx(io=link, link=link, debug=True, out=lambda *_: None)
        w.dock_panel(ctx)
        w.dock_panel(ctx)
        panel = json.loads((link / 'dock-panel.json').read_text())
        assert panel['Map'] == 0 and len(panel['Docks']) == 1
        assert len(stood) == 2      # this pier and the far one (for its name), each worked out once
        d = panel['Docks'][0]
        assert d['Key'] == '295:1' and d['Name'] == 'Auberdine, Darkshore' and d['Kind'] == 'ship'
        assert d['Wow'] == [-3706.5, -571.1, 6.0]
        went = []
        w.tele = lambda ctx, args, at=None: went.append(at)
        w.cross(ctx, ['#295:1'])
        assert went[0][3] == 1 and 'Auberdine' in went[0][4]
        try:
            w.cross(ctx, ['#302:1'])
            raise AssertionError('a vessel that does not stop here took the party')
        except w.CmdError as e:
            assert 'does not stop here' in str(e)
        # Away from every dock the panel is empty: the guardian leaves.
        struct.pack_into('<3f', raw, 72, -3000.0, -560.0, 6.0)
        (link / 'camera.bin').write_bytes(bytes(raw))
        w.dock_panel(ctx)
        assert json.loads((link / 'dock-panel.json').read_text())['Docks'] == []
    finally:
        wow_docks.landing = landing


def test_nothing_of_a_vessel_is_ground():
    """Nobody boards a ship or a zeppelin: BG3's characters stand only on ground the bridge places, and nothing of a
    vessel is in it. The static ground is the server's maps (terrain, buildings, the piers); the only moving game
    objects given a floor are lifts (type 11), by the bridge's own query and by the client's patch; a vessel
    (type 15) is in neither, and no ground is made from a route."""
    bridge = (ROOT / 'tools/world_bridge.py').read_text()
    assert 'where t.type = 11;' in bridge and 'type = 15' not in bridge and 'type=15' not in bridge
    assert 'wow_docks' not in bridge and 'wow_docks' not in (ROOT / 'tools/wow_lifts.py').read_text()
    patch = (ROOT / 'patches/benilla/0139-wowbg3-lifts-for-bg3.patch').read_text()
    assert 'let Drive::Lift(l) = &self.drive else {\n+            return None;' in patch
    assert '`None` for a boat or a zeppelin' in patch
    # The guardian's Lua places a character at a pier, never a platform or a floor.
    guide = (ROOT / 'mod/ScriptExtender/Lua/DockGuide.lua').read_text()
    assert 'PlatformMoveTo' not in guide and 'liftfloors' not in guide
