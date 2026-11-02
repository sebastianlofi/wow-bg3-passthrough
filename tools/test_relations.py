#!/usr/bin/env python3
"""test_relations.py: Relations.lua, without a game (needs the `lupa` module)."""
import sys
import unittest
from pathlib import Path

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua/Relations.lua'
try:
    from lupa import LuaRuntime
except ImportError:
    print('test_relations.py: needs the lupa module (pip install lupa); skipped')
    sys.exit(0)

HOSTILE = 'ACT0a_TUT_LowerDeckDevil_cb3a8427-8890-430c-9e9e-70db3a9de7a8'
NEUTRAL = 'Neutral NPC_cfb709b3-220f-9682-bcfb-6f0d8837462e'
RED, YELLOW, GREEN = {'Hostile': True}, {'Hostile': False}, {'Hostile': False, 'Friendly': True}


class Relations(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.R = self.lua.execute(LUA.read_text())
        self.plan = self.t({'Hostile': HOSTILE, 'Neutral': NEUTRAL})

    def t(self, v):
        return self.lua.table_from(v)

    def test_a_neutral_mob_is_an_enemy_in_bg3_so_a_left_click_attacks_it(self):
        self.assertEqual(self.R.FactionOf(self.t(YELLOW), self.plan), HOSTILE)

    def test_a_hostile_mob_is_an_enemy(self):
        self.assertEqual(self.R.FactionOf(self.t(RED), self.plan), HOSTILE)

    def test_a_friendly_npc_keeps_the_neutral_faction_so_a_left_click_talks(self):
        self.assertEqual(self.R.FactionOf(self.t(GREEN), self.plan), NEUTRAL)

    def test_without_a_hostile_faction_a_neutral_mob_stays_neutral(self):
        plan = self.t({'Neutral': NEUTRAL})
        self.assertEqual(self.R.FactionOf(self.t(YELLOW), plan), NEUTRAL)
        self.assertIsNone(self.R.FactionOf(self.t(RED), plan))

    def test_only_a_hostile_mob_joins_a_combat_on_sight(self):
        self.assertIs(self.R.JoinsOnSight(self.t(RED)), True)
        self.assertIs(self.R.JoinsOnSight(self.t(YELLOW)), False)
        self.assertIs(self.R.JoinsOnSight(self.t(GREEN)), False)

    def test_a_provoked_neutral_mob_forgets_after_calm_out_of_every_fight(self):
        calm = self.R.CALM
        m = self.t(YELLOW)
        self.assertIs(self.R.Calms(m, 100, 100 + calm + 1, False, False, False), True)
        self.assertIs(self.R.Calms(m, 100, 100 + calm, False, False, False), False)

    def test_it_stays_provoked_while_fighting_or_dead(self):
        m, late = self.t(YELLOW), 100 + self.R.CALM + 1
        self.assertIs(self.R.Calms(m, 100, late, True, False, False), False)
        self.assertIs(self.R.Calms(m, 100, late, False, True, False), False)
        self.assertIs(self.R.Calms(m, 100, late, False, False, True), False)

    def test_hostile_mobs_friendly_npcs_and_unprovoked_ones_never_calm_here(self):
        late = 100 + self.R.CALM + 1
        self.assertIs(self.R.Calms(self.t(RED), 100, late, False, False, False), False)
        self.assertIs(self.R.Calms(self.t(GREEN), 100, late, False, False, False), False)
        self.assertIs(self.R.Calms(self.t(YELLOW), None, late, False, False, False), False)


if __name__ == '__main__':
    unittest.main()
