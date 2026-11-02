#!/usr/bin/env python3
"""test_join_watch.py: JoinWatch.lua, without a game (needs the `lupa` module)."""
import sys
import unittest
from pathlib import Path

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua/JoinWatch.lua'
try:
    from lupa import LuaRuntime
except ImportError:
    print('test_join_watch.py: needs the lupa module (pip install lupa); skipped')
    sys.exit(0)

GUID, UUID = 'f1300001dc013c3e', '9cb15bf2-f0f0-cd66-1bed-f14fa09d940e'


class JoinWatch(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.J = self.lua.execute(LUA.read_text())

    def table(self, v):
        if isinstance(v, dict):
            return self.lua.table_from({k: self.table(x) for k, x in v.items()})
        return v

    def member(self, status, mirror=True, guid=GUID, uuid=UUID):
        return self.J.Member(self.table(status) if status is not None else None, guid, uuid, mirror)

    def test_a_mirror_bg3_has_in_the_partys_combat_is_a_member(self):
        st = {'Mobs': {GUID: {'Uuid': UUID, 'InCombat': True, 'Dead': False}}}
        self.assertIs(self.member(st), True)

    def test_a_mirror_bg3_killed_is_a_member_so_the_kill_reaches_wow(self):
        st = {'Mobs': {GUID: {'Uuid': UUID, 'InCombat': False, 'Dead': True}}}
        self.assertIs(self.member(st), True)

    def test_a_mirror_out_of_the_fight_is_not_one(self):
        st = {'Mobs': {GUID: {'Uuid': UUID, 'InCombat': False, 'Dead': False}}}
        self.assertIs(self.member(st), False)

    def test_a_mirror_gone_or_remade_is_not_the_one_this_fight_knows(self):
        self.assertIs(self.member({'Mobs': {}}), False)
        st = {'Mobs': {GUID: {'Uuid': 'another', 'InCombat': True}}}
        self.assertIs(self.member(st), False)

    def test_a_spawned_stand_in_reads_from_fight_spawned(self):
        st = {'Mobs': {}, 'FightSpawned': {UUID: {'InCombat': True, 'Dead': False}}}
        self.assertIs(self.member(st, mirror=False), True)
        st = {'Mobs': {}, 'FightSpawned': {UUID: {'InCombat': False, 'Dead': False}}}
        self.assertIs(self.member(st, mirror=False), False)
        self.assertIs(self.member({'Mobs': {}}, mirror=False), False)

    def test_an_unread_status_says_nothing(self):
        self.assertIsNone(self.member(None))


if __name__ == '__main__':
    unittest.main()
