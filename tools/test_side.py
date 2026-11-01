#!/usr/bin/env python3
"""A save keeps its `/wowbg3 faction` side (lifecycle.py: side_now, side_of_save, set_side)."""
import sys, tempfile, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import lifecycle  # noqa: E402


class Side(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.old = lifecycle.IO, lifecycle.wowsave.SNAP
        lifecycle.IO, lifecycle.wowsave.SNAP = root, root / 'snapshots'
        lifecycle.wowsave.SNAP.mkdir()
        self.file = root / 'link/faction.txt'

    def tearDown(self):
        lifecycle.IO, lifecycle.wowsave.SNAP = self.old
        self.tmp.cleanup()

    def test_no_file_is_own_and_stays_absent(self):
        self.assertEqual(lifecycle.side_now(), 'own')
        lifecycle.set_side('own')
        self.assertFalse(self.file.exists())

    def test_new_side_takes_a_new_seq(self):
        lifecycle.set_side('alliance')
        self.assertEqual(self.file.read_text(), '1 alliance\n')
        lifecycle.set_side('alliance')
        self.assertEqual(self.file.read_text(), '1 alliance\n')
        lifecycle.set_side('own')
        self.assertEqual(self.file.read_text(), '2 own\n')
        self.assertEqual(lifecycle.side_now(), 'own')

    def test_save_without_a_side_is_own(self):
        lifecycle.wowsave.write_meta('old', {'Id': 'old'})
        self.assertEqual(lifecycle.side_of_save('old'), 'own')
        self.assertEqual(lifecycle.side_of_save('missing'), 'own')
        self.assertEqual(lifecycle.side_of_save(None), 'own')

    def test_save_keeps_its_side(self):
        lifecycle.wowsave.write_meta('goldshire', {'Id': 'goldshire', 'Side': 'alliance'})
        self.assertEqual(lifecycle.side_of_save('goldshire'), 'alliance')

    def test_bad_values_are_own(self):
        lifecycle.wowsave.write_meta('bad', {'Id': 'bad', 'Side': 'pirate'})
        self.assertEqual(lifecycle.side_of_save('bad'), 'own')
        self.assertEqual(lifecycle.side_of_save('../bad'), 'own')
        self.file.parent.mkdir()
        self.file.write_text('3 pirate\n')
        self.assertEqual(lifecycle.side_now(), 'own')


if __name__ == '__main__':
    unittest.main()
