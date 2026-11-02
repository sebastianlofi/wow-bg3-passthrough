#!/usr/bin/env python3
"""test_turn_queue.py: TurnQueue.lua, without a game (needs the `lupa` module)."""
import sys
import unittest
from pathlib import Path

LUA = Path(__file__).resolve().parent.parent / 'mod/ScriptExtender/Lua/TurnQueue.lua'
try:
    from lupa import LuaRuntime
except ImportError:
    print('test_turn_queue.py: needs the lupa module (pip install lupa); skipped')
    sys.exit(0)


class TurnQueue(unittest.TestCase):
    def setUp(self):
        self.lua = LuaRuntime(unpack_returned_tuples=True)
        self.T = self.lua.execute(LUA.read_text())
        self.q = self.T.New()

    def pending(self):
        return list(self.q.pending.values())

    def unheard(self, active=True, tick=15, actor=None, busy=False, step=10):
        o = {'Active': active, 'Tick': tick, 'Busy': busy, 'Step': step}
        if actor is not None:
            o['TurnActor'] = actor
        return self.T.Unheard(self.q, self.lua.table_from(o))

    def test_a_turn_over_before_it_was_heard_blocks_nothing(self):
        # The order in BG3's monitor rings: next TurnStarted, then the last TurnEnded.
        self.T.Started(self.q, 'hero')
        self.T.Started(self.q, 'e1')
        self.assertTrue(self.T.Ended(self.q, 'hero'))
        self.assertEqual(self.pending(), ['e1'])
        self.assertFalse(self.q.heroOpen)

    def test_only_the_oldest_hero_entry_goes(self):
        for who in ('hero', 'e1', 'hero'):
            self.T.Started(self.q, who)
        self.T.Ended(self.q, 'hero')
        self.assertEqual(self.pending(), ['e1', 'hero'])

    def test_an_enemys_end_changes_nothing(self):
        self.T.Started(self.q, 'hero')
        self.assertFalse(self.T.Ended(self.q, 'e1'))
        self.assertEqual(self.pending(), ['hero'])
        self.assertTrue(self.q.heroOpen)

    def test_fight_begun_inside_her_turn_needs_a_second_report(self):
        self.assertIsNone(self.unheard(tick=15))
        self.assertIsNone(self.unheard(tick=15))  # the same report read again
        self.assertEqual(self.unheard(tick=30), 'no-event')

    def test_the_event_arriving_with_the_second_report_wins(self):
        self.assertIsNone(self.unheard(tick=15))
        self.T.Started(self.q, 'hero')
        self.assertIsNone(self.unheard(tick=30))
        self.assertEqual(self.pending(), ['hero'])

    def test_a_flag_that_drops_starts_over(self):
        self.assertIsNone(self.unheard(tick=15))
        self.assertIsNone(self.unheard(active=False, tick=30))
        self.assertIsNone(self.unheard(tick=45))
        self.assertEqual(self.unheard(tick=60), 'no-event')

    def test_nothing_while_her_turn_is_heard_or_another_is_watched(self):
        self.assertIsNone(self.unheard(tick=15, actor='hero'))
        self.assertIsNone(self.unheard(tick=30, actor='hero'))
        self.assertIsNone(self.unheard(tick=45, actor='e1', busy=True))
        self.assertIsNone(self.unheard(tick=60, actor='e1', busy=True))

    def test_a_bg3_turn_inside_hers(self):
        self.T.Started(self.q, 'hero')
        self.q.pending[1] = None
        self.assertIsNone(self.unheard(tick=15, actor='e1'))
        self.assertEqual(self.unheard(tick=30, actor='e1'), 'overlap')

    def test_wow_ended_her_turn_and_bg3_is_slow(self):
        self.T.Started(self.q, 'hero')
        self.q.pending[1] = None
        for i in range(8):
            self.assertIsNone(self.unheard(tick=15 * i))
        self.assertEqual(self.unheard(tick=135), 'end-ignored')

    def test_bg3_ended_it_after_wow_did(self):
        self.T.Started(self.q, 'hero')
        self.q.pending[1] = None
        self.assertIsNone(self.unheard(tick=15))
        self.T.Ended(self.q, 'hero')
        self.assertIsNone(self.unheard(active=False, tick=30))
        self.assertEqual(self.q.endWait, 0)


if __name__ == '__main__':
    unittest.main()
