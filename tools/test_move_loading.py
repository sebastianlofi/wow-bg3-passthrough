#!/usr/bin/env python3
"""test_move_loading.py: when MoveLoading.lua calls BG3's screen fade, without a game: the real Lua text under a
fake Osiris (needs the `lupa` module). What BG3 then draws is for a game to show."""
from pathlib import Path

import lupa

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua/MoveLoading.lua'
FAKE = r'''
files, calls, tick_fn, state, clock, listeners = {}, {}, nil, "Running", 0, {}
Osi = {GetHostCharacter = function() return "HOST" end,
       ScreenFadeTo = function(c, d, s, id) calls[#calls + 1] = "to " .. id .. " " .. d end,
       ClearScreenFade = function(c, d, id, f) calls[#calls + 1] = "clear " .. id end}
Ext = {IO = {LoadFile = function(p) return files[p] end},
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
    print('test_move_loading: ok')


main()
