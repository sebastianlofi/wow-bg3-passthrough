#!/usr/bin/env python3
"""test_move_loading.py: when MoveLoading.lua calls BG3's screen fade, without a game: the real Lua text under a
fake Osiris (needs the `lupa` module). What BG3 then draws is for a game to show."""
from pathlib import Path

import lupa

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua/MoveLoading.lua'
FAKE = r'''
files, calls, tick_fn, state, clock = {}, {}, nil, "Running", 0
Osi = {GetHostCharacter = function() return "HOST" end,
       ScreenFadeTo = function(c, d, s, id) calls[#calls + 1] = "to" end,
       ClearScreenFade = function(c, d, id, f) calls[#calls + 1] = "clear" end}
Ext = {IO = {LoadFile = function(p) return files[p] end},
       Log = {Print = function(s) end},
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
    assert calls(g) == ['clear']

    # A move: one fade though the click and the bridge both write the flag; cleared at its end.
    g = fresh()
    g.run(5)
    g.files[F] = '2\n'
    g.run(5)
    g.files[F] = '3\n'
    g.run(5)
    assert calls(g) == ['to']
    g.files[F] = None
    g.run(5)
    assert calls(g) == ['to', 'clear']

    # A bridge killed mid-move: cleared after 120 s, once.
    g = fresh()
    g.run(5)
    g.files[F] = '4\n'
    g.run(5)
    g.run(130, 1000)
    assert calls(g) == ['to', 'clear']

    # Not while BG3 is not running.
    g = fresh()
    g.state = 'Menu'
    g.files[F] = '5\n'
    g.run(20)
    assert calls(g) == []
    print('test_move_loading: ok')


main()
