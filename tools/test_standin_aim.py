#!/usr/bin/env python3
"""test_standin_aim.py: the stand-in under WoW's model, without a game.

StandInAim.lua (needs the `lupa` module) and plugins/wowbg3_standin_aim.h (needs a C++ compiler, `c++`);
each part is skipped when its tool is missing. Also compiles the Lua files the change touches."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LUA_DIR = ROOT / 'mod/ScriptExtender/Lua'
HEADER = ROOT / 'plugins/wowbg3_standin_aim.h'
try:
    from lupa import LuaRuntime
except ImportError:
    LuaRuntime = None


@unittest.skipIf(LuaRuntime is None, 'needs the lupa module (pip install lupa)')
class Lua(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.A = self.lua.execute((LUA_DIR / 'StandInAim.lua').read_text())
        self.by = self.lua.table_from({'f130000c3a002fc1': 'uuid-lizard'})

    def test_target_needs_wows_unit_and_a_living_stand_in(self):
        T = self.A.Target
        self.assertEqual(T(4, 'f130000c3a002fc1', False, self.by), 'uuid-lizard')
        self.assertEqual(T(2, 'F130000C3A002FC1', False, self.by), 'uuid-lizard')  # a friendly NPC, any case
        self.assertIsNone(T(4, 'f130000c3a002fc2', False, self.by))  # no stand-in (or a dead one)
        self.assertIsNone(T(1 | 4, 'f130000c3a002fc1', False, self.by))  # WoW's UI under the pointer
        self.assertIsNone(T(4, 'f130000c3a002fc1', True, self.by))  # a BG3 window is open
        self.assertIsNone(T(4, None, False, self.by))
        self.assertIsNone(T(None, 'f130000c3a002fc1', False, self.by))

    def run_steps(self, targets, feet=None):
        calls, lines = [], []
        st = self.lua.table()
        feet = feet or (lambda u: self.lua.table(1.0, 2.0, 3.0))

        def call(mode, x, y, z):
            calls.append((mode, x, y, z))
            return self.lua.table_from({'Err': 0, 'R': self.lua.table(1, 5, 2, 7)})
        for t in targets:
            lines.append(self.A.Step(st, t, feet, call))
        return st, calls, lines

    def test_named_every_tick_while_it_holds_and_none_once(self):
        st, calls, lines = self.run_steps(['u1', 'u1', None, None])
        self.assertEqual([c[0] for c in calls], [1, 1, 0])
        self.assertEqual(calls[0], (1, 1.0, 2.0, 3.0))
        self.assertEqual(lines[0], 'stand-in aim on u1')
        self.assertIsNone(lines[1])
        self.assertTrue(lines[2].startswith('stand-in aim off u1'))
        self.assertIn('5 turned to a stand-in', lines[2])
        self.assertIsNone(lines[3])
        self.assertIsNone(st.Uuid)

    def test_another_unit_is_named_at_once(self):
        st, calls, lines = self.run_steps(['u1', 'u2'])
        self.assertEqual([c[0] for c in calls], [1, 1])
        self.assertEqual(lines, ['stand-in aim on u1', 'stand-in aim on u2'])

    def test_a_stand_in_without_an_entity_is_none(self):
        st, calls, lines = self.run_steps(['u1', 'u2'], feet=lambda u: self.lua.table(1.0, 2.0, 3.0) if u == 'u1' else None)
        self.assertEqual([c[0] for c in calls], [1, 0])
        self.assertTrue(lines[1].startswith('stand-in aim off u1'))

    def test_a_refused_call_keeps_no_counts(self):
        st = self.lua.table()
        line = self.A.Step(st, 'u1', lambda u: self.lua.table(0, 0, 0), lambda *a: None)
        self.assertEqual(line, 'stand-in aim on u1')
        self.assertTrue(self.A.Step(st, None, None, lambda *a: None).endswith('off u1'))

    def test_changed_lua_files_compile(self):
        for name in ('StandInAim.lua', 'WowCursor.lua', 'WorldMode.lua', 'WowNative.lua'):
            ok, err = self.lua.execute('local f, e = load(...); return f ~= nil, tostring(e)',
                                       (LUA_DIR / name).read_text())
            self.assertTrue(ok, '%s: %s' % (name, err))


DRIVER = r'''
#include "wowbg3_standin_aim.h"
#include <cstdio>
#include <cmath>
using namespace wowbg3_aim;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    const float feet[3] = {-2598.9f, 119.9f, -1000.7f};
    // The cursor's own query on WoW's model: ground behind the small stand-in first.
    Hit ground[] = {{{-2600.0f, 119.9f, -1001.5f}, 0.40f, 0x10}};
    CHECK(!picks_character(ground, 1));
    CHECK(!picks_character(nullptr, 0));
    Hit charFirst[] = {{{-2600.0f, 119.9f, -1001.5f}, 0.40f, 0x10}, {{-2598.8f, 120.6f, -1000.6f}, 0.38f, 0x200}};
    CHECK(picks_character(charFirst, 2));
    Hit body[] = {{{-2598.8f, 120.2f, -1000.6f}, 0.30f, 0x4000}};
    CHECK(picks_character(body, 1));  // a body BG3 picks (its loot) is left alone
    // The redirected cast: its first hit is a living character beside the stand-in's feet.
    CHECK(hits_stand_in(charFirst, 2, feet));
    CHECK(!hits_stand_in(ground, 1, feet));
    CHECK(!hits_stand_in(body, 1, feet));  // only a living stand-in is aimed at
    Hit other[] = {{{-2596.0f, 120.6f, -1000.6f}, 0.20f, 0x200}};  // another character 2.9 m off
    CHECK(!hits_stand_in(other, 1, feet));
    Hit wallFirst[] = {{{-2599.5f, 121.0f, -1000.7f}, 0.10f, 0x10}, {{-2598.8f, 120.6f, -1000.6f}, 0.38f, 0x200}};
    CHECK(!hits_stand_in(wallFirst, 2, feet));  // hidden from BG3's camera: no pick
    Hit high[] = {{{-2598.8f, 124.0f, -1000.6f}, 0.10f, 0x200}};
    CHECK(!hits_stand_in(high, 1, feet));
    // The segment: from the eye toward lift over the feet, as long as the query's.
    const float a[3] = {-2610.0f, 131.8f, -1000.7f}, b[3] = {-2610.0f + 300.0f, 131.8f - 400.0f, -1000.7f};
    float e[3];
    CHECK(aim_end(a, b, feet, 1.0f, e));
    const float len = std::sqrt((b[0]-a[0])*(b[0]-a[0]) + (b[1]-a[1])*(b[1]-a[1]) + (b[2]-a[2])*(b[2]-a[2]));
    const float el = std::sqrt((e[0]-a[0])*(e[0]-a[0]) + (e[1]-a[1])*(e[1]-a[1]) + (e[2]-a[2])*(e[2]-a[2]));
    CHECK(std::fabs(el - len) < 0.01f);
    // The target point lies on the new segment.
    const float t[3] = {feet[0], feet[1] + 1.0f, feet[2]};
    float cx = (e[1]-a[1])*(t[2]-a[2]) - (e[2]-a[2])*(t[1]-a[1]);
    float cy = (e[2]-a[2])*(t[0]-a[0]) - (e[0]-a[0])*(t[2]-a[2]);
    float cz = (e[0]-a[0])*(t[1]-a[1]) - (e[1]-a[1])*(t[0]-a[0]);
    CHECK(std::sqrt(cx*cx + cy*cy + cz*cz) / el < 0.01f);
    const float same[3] = {feet[0], feet[1] + 1.0f, feet[2]};
    CHECK(!aim_end(same, b, feet, 1.0f, e));
    CHECK(kLiftCount == 4 && kLifts[0] == 1.0f);
    std::printf("%s\n", fails ? "FAILED" : "OK");
    return fails ? 1 : 0;
}
'''


@unittest.skipIf(shutil.which('c++') is None, 'needs a C++ compiler (c++)')
class Header(unittest.TestCase):
    def test_header(self):
        with tempfile.TemporaryDirectory() as d:
            src, exe = os.path.join(d, 'aim.cpp'), os.path.join(d, 'aim')
            Path(src).write_text(DRIVER)
            r = subprocess.run(['c++', '-std=c++17', '-Wall', '-Werror', '-I', str(HEADER.parent), src, '-o', exe],
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            r = subprocess.run([exe], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertIn('OK', r.stdout)


if __name__ == '__main__':
    unittest.main()
