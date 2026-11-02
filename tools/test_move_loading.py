#!/usr/bin/env python3
"""test_move_loading.py: MoveLoading.lua's opening and closing, without a game: the real Lua text under a fake
UI (needs the `lupa` module). The state machine's real effect is for a game to show."""
import xml.dom.minidom
from pathlib import Path

import lupa

HERE = Path(__file__).resolve().parent
MOD = HERE.parent / 'mod'
FAKE = r'''
files, log, fired, tick_fn, state, clock = {}, {}, {}, nil, "Running", 0
local loading = false
local function node(t) t.Type = "ls.UIWidget"; return t end
local function command(accepts)
    return {CanExecute = function(self, e) return accepts end,
            Execute = function(self, e)
                fired[#fired + 1] = e
                if e == "OpenWowMoveLoading" and not stuck then loading = true end
                if e == "CloseWowMoveLoading" then loading = false end
            end}
end
Ext = {IO = {LoadFile = function(p) return files[p] end, SaveFile = function(p, s) files[p] = s end},
       Log = {Print = function(s) log[#log + 1] = s end},
       Utils = {MonotonicTime = function() return clock end, GetGameState = function() return state end},
       Events = {Tick = {Subscribe = function(self, f) tick_fn = f end}},
       UI = {GetRoot = function()
           local kids = {node{DataContext = {}}, node{DataContext = {CustomEvent = command(true)}}}
           if loading then kids[3] = node{DataContext = {LoadingHints = {}, CustomEvent = command(true)}} end
           return {Type = "Canvas", VisualChildrenCount = #kids, VisualChild = function(self, i) return kids[i] end}
       end}}
function run(n, ms) for _ = 1, n do clock = clock + (ms or 100); tick_fn() end end
'''


def fresh(flag=None):
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.execute(FAKE)
    g = L.globals()
    if flag:
        g.files['wow-bg3/link/wowmove.flag'] = flag
    m = L.execute((MOD / 'ScriptExtender/Lua/MoveLoading.lua').read_text())
    m.Start()
    return L, g


def ui(g):
    return g.files['wow-bg3/link/bg3moveui.txt']


def fired(g):
    return list(g.fired.values())


def main():
    for name in ('Keyboard', 'Controller'):
        xml.dom.minidom.parse(str(MOD / 'GUI/StateMachines' / (name + '.xaml')))

    # A flag from before this start is no move.
    L, g = fresh('1.0\n')
    g.run(30)
    assert fired(g) == [] and ui(g) == '0\n'

    # A move: opened once, "1" only when the widget is seen, closed at its end.
    L, g = fresh()
    g.files['wow-bg3/link/wowmove.flag'] = '2.0\n'
    g.run(10)
    assert fired(g) == ['OpenWowMoveLoading'] and ui(g) == '0\n'
    g.run(10)
    assert ui(g) == '1\n' and fired(g) == ['OpenWowMoveLoading']
    g.files['wow-bg3/link/wowmove.flag'] = None
    g.run(10)
    assert fired(g)[-1] == 'CloseWowMoveLoading'
    g.run(10)
    assert ui(g) == '0\n' and len(fired(g)) == 2

    # A bridge killed mid-move: closed like the cover, after 120 s.
    L, g = fresh()
    g.files['wow-bg3/link/wowmove.flag'] = '3.0\n'
    g.run(20)
    g.run(130, 1000)
    assert fired(g)[-1] == 'CloseWowMoveLoading'
    g.run(10)
    assert ui(g) == '0\n'

    # The state does not open (the machine not loaded): never "1", the cover stays, and the log says so.
    L, g = fresh()
    g.stuck = True
    g.files['wow-bg3/link/wowmove.flag'] = '4.0\n'
    g.run(100)
    assert ui(g) == '0\n' and any('did not open' in s for s in g.log.values())

    # Outside a running game the UI is not touched.
    L, g = fresh()
    g.state = 'Menu'
    g.files['wow-bg3/link/wowmove.flag'] = '5.0\n'
    g.run(30)
    assert fired(g) == []
    print('test_move_loading: ok')


main()
