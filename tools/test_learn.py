#!/usr/bin/env python3
"""test_learn.py: /wowbg3 learn and unlearn, without a game. wowbg3cmd.py and the real text of Learn.lua talk through
real files, with a fake game under the Lua (needs the `lupa` module).

  python3 tools/test_learn.py
"""
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

HERE = Path(__file__).resolve().parent
LUA = HERE.parent / 'mod/ScriptExtender/Lua'
sys.path.insert(0, str(HERE))
try:
    import lupa
except ImportError:
    print('test_learn.py: needs the lupa module (pip install lupa); skipped')
    raise SystemExit(0)
import wowbg3cmd as C  # noqa: E402

HERO = '11111111-2222-3333-4444-555555555555'
FAKE = r'''
ticks_cb, spells, calls, printed = {}, {}, {}, {}
state, hero, stuck = "Running", "%s", {}
STATS = {Target_Grease = "SpellData", Target_Bless = "SpellData", Projectile_Grease_Bomb = "SpellData", BLESS = "StatusData"}
Ext = {
  IO = {LoadFile = function(n) return py_load(n) end, SaveFile = function(n, v) py_save(n, v) end},
  Json = {Parse = function(v) return py_parse(v) end, Stringify = function(v) return py_dump(v) end},
  Events = {Tick = {Subscribe = function(_, f) ticks_cb[#ticks_cb + 1] = f end}},
  Utils = {GetGameState = function() return state end, Print = function(s) printed[#printed + 1] = s end},
  Log = {Print = function() end},
  Require = function() return {Uuid = function() return hero end} end,
  Stats = {
    Get = function(n)
      if STATS[n] == nil then error("no stat " .. n) end
      return {ModifierList = STATS[n]}
    end,
    GetStats = function(list)
      local out = {}
      for n, l in pairs(STATS) do if l == list then out[#out + 1] = n end end
      return out
    end,
  },
}
Osi = {
  HasSpell = function(u, s) if spells[s] then return 1 end return 0 end,
  AddSpell = function(u, s, notify, container)
    calls[#calls + 1] = table.concat({"add", u, s, notify, container}, " ")
    spells[s] = true
  end,
  RemoveSpell = function(u, s, container)
    calls[#calls + 1] = table.concat({"remove", u, s, container}, " ")
    if not stuck[s] then spells[s] = nil end
  end,
}
function tick(n) for _ = 1, n do for _, f in ipairs(ticks_cb) do f() end end end
''' % HERO


class Case(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.io = Path(self.tmp.name)
        (self.io / 'm3-server.json').write_text(json.dumps({'RunId': 'r1'}))
        (self.io / 'party.json').write_text(json.dumps({'RunId': 'r1'}))
        self.lines, self.errors = [], []
        self.L = lupa.LuaRuntime(unpack_returned_tuples=True)
        g = self.L.globals()

        def path(name):
            self.assertTrue(name.startswith('wow-bg3/'), name)
            return self.io / name[len('wow-bg3/'):]

        def load(name):
            return path(name).read_text() if path(name).exists() else None
        g.py_load = load
        g.py_save = lambda name, text: path(name).write_text(text)
        g.py_parse = lambda s: self.to_lua(json.loads(s))
        g.py_dump = lambda t: json.dumps(self.to_py(t))
        self.L.execute(FAKE)
        self.M = self.L.execute((LUA / 'Learn.lua').read_text())
        self.M.Start('r1')
        # Each wait of the tool gives the game some ticks.
        p = mock.patch.object(C.time, 'sleep', lambda _: g.tick(7))
        p.start()
        self.addCleanup(p.stop)

    def to_lua(self, v):
        if isinstance(v, dict):
            t = self.L.table()
            for k, x in v.items():
                t[k] = self.to_lua(x)
            return t
        return v

    def to_py(self, t):
        if lupa.lua_type(t) != 'table':
            return t
        return {k: self.to_py(t[k]) for k in t.keys()}

    def ctx(self, debug=True):
        return C.Ctx(io=self.io, debug=debug, out=lambda line, error=False: (self.errors if error else self.lines).append(line))

    def run_(self, text, debug=True):
        C.run(self.ctx(debug), text)

    def refused(self, text, debug=True):
        with self.assertRaises(C.CmdError) as e:
            self.run_(text, debug)
        return str(e.exception)

    def calls(self):
        c = self.L.globals().calls
        return [c[k] for k in sorted(c.keys())]

    def has(self, spell):
        return self.L.globals().spells[spell] is True


class Grant(Case):
    def test_learn_is_addspell_on_the_hero(self):
        self.run_('learn Target_Grease')
        self.assertEqual(self.calls(), ['add %s Target_Grease 0 1' % HERO])
        self.assertTrue(self.has('Target_Grease'))
        self.assertIn('now has Target_Grease', self.lines[-1])

    def test_learn_twice_adds_once(self):
        self.run_('learn Target_Grease')
        self.run_('learn Target_Grease')
        self.assertEqual(len(self.calls()), 1)
        self.assertIn('already has', self.lines[-1])

    def test_unlearn_is_removespell_on_the_hero(self):
        self.run_('learn Target_Grease')
        self.run_('unlearn Target_Grease')
        self.assertEqual(self.calls()[-1], 'remove %s Target_Grease 1' % HERO)
        self.assertFalse(self.has('Target_Grease'))
        self.assertIn('no longer has', self.lines[-1])

    def test_unlearn_what_the_hero_does_not_have(self):
        self.assertIn('does not have', self.refused('unlearn Target_Bless'))
        self.assertEqual(self.calls(), [])

    def test_a_spell_from_elsewhere_stays_and_says_so(self):
        self.L.execute('spells.Target_Bless = true; stuck.Target_Bless = true')
        self.assertIn('comes from elsewhere', self.refused('unlearn Target_Bless'))


class Refused(Case):
    def test_an_unknown_name_gets_bg3s_names_for_it(self):
        why = self.refused('learn Grease')
        self.assertIn('no spell Grease', why)
        self.assertIn('Target_Grease', why)
        self.assertIn('Projectile_Grease_Bomb', why)
        self.assertEqual(self.calls(), [])

    def test_a_name_like_nothing(self):
        self.assertIn('no spell Nonsense', self.refused('learn Nonsense'))

    def test_a_stat_that_is_no_spell(self):
        self.assertIn('as StatusData, not a spell', self.refused('learn BLESS'))
        self.assertEqual(self.calls(), [])

    def test_no_hero(self):
        self.L.execute('hero = nil')
        self.assertIn('no hero', self.refused('learn Target_Grease'))

    def test_words_the_tool_refuses_before_any_file(self):
        for text in ('learn', 'learn a b', 'unlearn', 'learn Target-Grease'):
            with self.assertRaises(C.Usage, msg=text):
                self.run_(text)
        self.assertFalse((self.io / 'learn-command.json').exists())

    def test_behind_the_test_commands_switch(self):
        for verb in ('learn', 'unlearn'):
            self.assertIn('test commands are off', self.refused(verb + ' Target_Grease', debug=False))
        self.assertFalse((self.io / 'learn-command.json').exists())
        self.assertEqual(self.calls(), [])

    def test_no_game_running(self):
        (self.io / 'party.json').unlink()
        self.assertIn('not running a game', self.refused('learn Target_Grease'))

    def test_no_answer_is_said(self):
        self.L.execute('state = "Save"')
        self.assertIn('no answer from the mod', self.refused('learn Target_Grease'))
        self.assertEqual(self.calls(), [])


class Isolated(Case):
    def test_the_fights_command_files_stay_untouched(self):
        for name in ('stand-in-command.json', 'party-command.json', 'arena-command.json'):
            (self.io / name).write_text('{"RunId": "r1", "Sequence": 5}')
        before = {p.name: p.read_text() for p in self.io.iterdir() if 'party.json' != p.name}
        self.run_('learn Target_Grease')
        for name, text in before.items():
            self.assertEqual((self.io / name).read_text(), text, name)
        self.assertEqual(sorted(p.name for p in self.io.iterdir() if p.name not in before and p.name != 'party.json'),
                         ['learn-command.json', 'learn-result.json'])

    def test_a_load_does_not_run_the_last_command_again(self):
        self.run_('learn Target_Grease')
        self.run_('unlearn Target_Grease')
        n = len(self.calls())
        # bg3le builds the Lua states again at a load: Start runs again over the same file.
        self.L.execute('ticks_cb = {}')
        self.M.Start('r1')
        self.L.globals().tick(50)
        self.assertEqual(len(self.calls()), n)

    def test_chat_commands_take_a_lock_of_their_own(self):
        spec = importlib.util.spec_from_file_location('wowbg3_chatcmd', HERE / 'wowbg3-chatcmd.py')
        chat = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(chat)
        self.assertEqual(chat.kind_of('learn Target_Grease'), 'learn')
        self.assertEqual(chat.kind_of('unlearn Target_Grease'), 'learn')
        self.assertIn('learn', chat.LOCKS)

    def test_help_says_it_only_when_on(self):
        self.assertTrue(any('/wowbg3 learn SPELL' in l for l in C.help_lines(True)))
        self.assertFalse(any('/wowbg3 learn SPELL' in l for l in C.help_lines(False)))


if __name__ == '__main__':
    unittest.main()
