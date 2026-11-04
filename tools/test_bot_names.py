#!/usr/bin/env python3
"""A companion's WoW character: never one the server's `bot add` would answer `refused taken` for."""
import argparse, sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lifecycle  # noqa: E402
import wowsave  # noqa: E402

HERO = {'Uuid': 'h', 'Name': 'Tav', 'Char': 'Tava', 'Hero': True}
GALE = {'Uuid': 'g', 'Name': 'Gale'}


class Resolve(unittest.TestCase):
    def setUp(self):
        self.was = wowsave.all_names, wowsave.accounts, wowsave.exists, lifecycle.log
        self.db = {'Gale': 'WB3PALH3HQZV', 'Tava': 'WB3PKVXNLU5J', 'Shadowheart': 'WB3BSHADOWHEART'}
        wowsave.all_names = lambda: list(self.db)
        wowsave.accounts = lambda names: {n: self.db[n] for n in names}
        wowsave.exists = lambda name: name in self.db
        lifecycle.log = lambda *a: None
        self.life = lifecycle.Life(argparse.Namespace(launcher='x', tag='t', char=''))

    def tearDown(self):
        wowsave.all_names, wowsave.accounts, wowsave.exists, lifecycle.log = self.was

    def name(self, member, new_game=False, snap=None):
        return self.life.resolve([HERO, member], new_game, snap)[0][member['Uuid']]

    def test_another_heros_name_is_not_a_bots(self):
        self.assertEqual(self.name(GALE), 'Galea')

    def test_a_save_that_names_it_moves_off_it(self):
        self.assertEqual(self.name(dict(GALE, Char='Gale')), 'Galea')

    def test_a_snapshot_that_names_it_moves_off_it(self):
        self.assertEqual(self.name(GALE, snap={'g': 'Gale'}), 'Galea')

    def test_the_bot_made_earlier_comes_back(self):
        self.db['Galea'] = 'WB3BGALEA'
        self.assertEqual(self.name(GALE), 'Galea')
        self.assertEqual(self.name(dict(GALE, Char='Galea')), 'Galea')

    def test_a_later_party_add_gets_the_same(self):
        self.assertEqual(self.life.resolve([GALE], False)[0]['g'], 'Galea')

    def test_a_bots_own_name_stays(self):
        self.assertEqual(self.name({'Uuid': 's', 'Name': 'Shadowheart', 'Char': 'Shadowheart'}), 'Shadowheart')
        self.assertEqual(self.name({'Uuid': 's', 'Name': 'Shadowheart'}), 'Shadowheart')

    def test_a_new_name_stays(self):
        self.assertEqual(self.name({'Uuid': 'k', 'Name': 'Karlach'}), 'Karlach')

    def test_that_heros_own_playthrough_keeps_it(self):
        out, make = self.life.resolve([{'Uuid': 'g', 'Name': 'Gale', 'Char': 'Gale', 'Hero': True}], False)
        self.assertEqual((out['g'], make), ('Gale', set()))


if __name__ == '__main__':
    unittest.main()
