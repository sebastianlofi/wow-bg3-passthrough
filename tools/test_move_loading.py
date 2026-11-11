#!/usr/bin/env python3
"""test_move_loading.py: when MoveLoading.lua calls BG3's screen fade, without a game: the real Lua text under a
fake Osiris (needs the `lupa` module). What BG3 then draws is for a game to show."""
import json
import struct
from pathlib import Path

import lupa

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua/MoveLoading.lua'
FAKE = r'''
files, calls, tick_fn, state, clock, listeners = {}, {}, nil, "Running", 0, {}
Osi = {GetHostCharacter = function() return "HOST" end,
       ScreenFadeTo = function(c, d, s, id) calls[#calls + 1] = "to " .. id .. " " .. d end,
       ClearScreenFade = function(c, d, id, f) calls[#calls + 1] = "clear " .. id end}
Ext = {IO = {LoadFile = function(p) return files[p] end},
       -- Hero.lua: the far switch's cover asks it whether a place is in WoW's world; every place of these cases is.
       Require = function(name) return {InWorld = function() return true end} end,
       Log = {Print = function(s) end},
       Osiris = {RegisterListener = function(name, arity, when, fn) listeners[name] = fn end},
       Utils = {MonotonicTime = function() return clock end, GetGameState = function() return state end},
       Events = {Tick = {Subscribe = function(self, f) tick_fn = f end}}}
function run(n, ms) for _ = 1, n do clock = clock + (ms or 100); tick_fn() end end
'''
F = 'wow-bg3/link/wowmove.flag'


def fresh(flag=None):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.execute(FAKE)
    g = L.globals()
    if flag:
        g.files[F] = flag
    L.execute(LUA.read_text()).StartServer()
    return g


def calls(g):
    return list(g.calls.values())


def main():
    # A flag from before this load: its fade cleared, no new one.
    g = fresh('1\n')
    g.run(20)
    assert calls(g) == ['clear WOWBG3_Move_1'], calls(g)

    # A short move: one fade though the click and the bridge both write the flag; cleared at its end.
    g = fresh()
    g.run(5)
    g.files[F] = '2\n'
    g.run(5)
    g.files[F] = '3\n'
    g.run(5)
    assert calls(g) == ['to WOWBG3_Move_1 0.5'], calls(g)
    g.files[F] = None
    g.run(5)
    assert calls(g) == ['to WOWBG3_Move_1 0.5', 'clear WOWBG3_Move_1'], calls(g)

    # A long move (25 s): BG3 lifts a fade ~10 s after it is black, so a new one comes every 5 s, black at
    # once; at the end every one is cleared, the next move's ids go on counting.
    g = fresh()
    g.run(5)
    g.files[F] = '6\n'
    g.run(250)
    tos = [c for c in calls(g) if c.startswith('to ')]
    assert tos[0] == 'to WOWBG3_Move_1 0.5' and len(tos) == 5, tos
    assert all(c.endswith(' 0.1') for c in tos[1:]), tos
    assert len(set(tos)) == len(tos), tos
    g.files[F] = None
    g.run(5)
    clears = [c for c in calls(g) if c.startswith('clear ')]
    assert clears == ['clear WOWBG3_Move_%d' % i for i in range(1, 6)], clears
    g.files[F] = '7\n'
    g.run(5)
    assert calls(g)[-1] == 'to WOWBG3_Move_6 0.5', calls(g)

    # BG3's own events for our fades are heard (and others' ignored) without failing.
    assert g.listeners['ScreenFadeCleared'] is not None and g.listeners['ScreenFadeDone'] is not None
    g.listeners['ScreenFadeCleared'](1, 'WOWBG3_Move_6')
    g.listeners['ScreenFadeDone'](1, 'GLO_Camp_StartRest_Fade_Out')

    # A bridge killed mid-move: cleared after 120 s, once.
    g = fresh()
    g.run(5)
    g.files[F] = '4\n'
    g.run(5)
    g.run(130, 1000)
    c = calls(g)
    assert c[-1].startswith('clear ') and sum(x.startswith('to ') for x in c) == sum(x.startswith('clear ') for x in c), c
    g.run(20, 1000)
    assert calls(g) == c, calls(g)

    # Not while BG3 is not running.
    g = fresh()
    g.state = 'Menu'
    g.files[F] = '5\n'
    g.run(20)
    assert calls(g) == []
    switches()
    print('test_move_loading: ok')


C = 'wow-bg3/link/camera.bin'
W = 'wow-bg3/link/world.json'
HERO, AST = (-2600.0, 120.0, -1000.0), (-1900.0, 121.0, -1000.0)   # t58: Astarion 700 m east of the hero


def wow_of(p):
    # world.json's mapping below: WoW (-9460, 60) at the centre.
    return -9460.0 - (p[2] + 1000.0) / 0.9144, 60.0 + (p[0] + 2600.0) / 0.9144


def camera(at, flags, seq=1):
    x, y = wow_of(at)
    raw = bytearray(96)
    raw[0:4] = b'WBC1'
    struct.pack_into('<I3f', raw, 4, seq, x, y, 70.0)
    struct.pack_into('<I', raw, 64, seq)
    struct.pack_into('<3fII', raw, 72, x, y, 60.0, 0, flags)
    return bytes(raw)


def switch_world(active=True):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.execute(FAKE)
    g = L.globals()
    worlds = {}

    def parse(text):
        v = worlds.get(text)
        return None if v is None else L.table_from({'Active': v['Active'], 'WowOrigin': L.table_from(v['WowOrigin']),
                                                    'Centre': L.table_from(v['Centre'])})
    g.Ext.Json = L.table_from({'Parse': parse})
    text = json.dumps({'Active': active, 'WowOrigin': [-9460.0, 60.0, 56.0], 'Centre': list(HERO)})
    worlds[text] = json.loads(text)
    g.files[W] = text
    g.hosts = L.table_from({'HERO': L.table_from(HERO), 'AST': L.table_from(AST),
                            'NEAR': L.table_from((HERO[0] + 40, HERO[1], HERO[2]))})
    L.execute('host = "HERO"\n'
              'Osi.GetHostCharacter = function() return host end\n'
              'Osi.GetPosition = function(c) local p = hosts[c]; return p[1], p[2], p[3] end')
    g.files[C] = camera(HERO, 1 | 4)
    L.execute(LUA.read_text()).StartServer()
    g.run(5)
    return g


def switches():
    # t58's far switch: covered at once, through the eye still at the hero (loaded there), the eye at Astarion
    # with the view out but nothing loaded, until benilla says the world round the eye is loaded there.
    g = switch_world()
    g.host = 'AST'
    g.run(5)
    assert calls(g) == ['to WOWBG3_Move_1 0.2'], calls(g)
    g.run(20)
    g.files[C] = camera(AST, 1 | 2, 2)
    g.run(20)
    assert calls(g) == ['to WOWBG3_Move_1 0.2'], calls(g)
    g.files[C] = camera(AST, 1 | 2 | 4, 3)
    g.run(5)
    assert calls(g) == ['to WOWBG3_Move_1 0.2', 'clear WOWBG3_Move_1'], calls(g)
    # A torn camera record (seq and seq again differ) counts as nothing.
    g = switch_world()
    g.host = 'AST'
    g.run(5)
    raw = bytearray(camera(AST, 1 | 2 | 4, 5))
    struct.pack_into('<I', raw, 64, 4)
    g.files[C] = bytes(raw)
    g.run(10)
    assert calls(g) == ['to WOWBG3_Move_1 0.2'], calls(g)
    # A near switch, or one with world mode off: no cover.
    g = switch_world()
    g.host = 'NEAR'
    g.run(10)
    assert calls(g) == [], calls(g)
    g = switch_world(active=False)
    g.host = 'AST'
    g.run(10)
    assert calls(g) == [], calls(g)
    # No world from benilla: renewed every 5 s like a move's, then 20 s at most.
    g = switch_world()
    g.host = 'AST'
    g.run(5)
    g.run(230)
    c = calls(g)
    assert [x for x in c if x.startswith('to ')] == ['to WOWBG3_Move_1 0.2'] + ['to WOWBG3_Move_%d 0.1' % i for i in (2, 3, 4, 5)], c
    assert [x for x in c if x.startswith('clear ')] == ['clear WOWBG3_Move_%d' % i for i in (1, 2, 3, 4, 5)], c
    # A move and a switch share the cover: the switch's end leaves the move's black, the move's end clears it.
    g = switch_world()
    g.files[F] = '8\n'
    g.run(5)
    g.host = 'AST'
    g.run(5)
    g.files[C] = camera(AST, 1 | 2 | 4, 2)
    g.run(10)
    assert calls(g) == ['to WOWBG3_Move_1 0.5'], calls(g)
    g.files[F] = None
    g.run(5)
    assert calls(g) == ['to WOWBG3_Move_1 0.5', 'clear WOWBG3_Move_1'], calls(g)


main()
