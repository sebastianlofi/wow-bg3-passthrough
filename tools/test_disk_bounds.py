#!/usr/bin/env python3
"""test_disk_bounds.py: what the mod keeps on a player's disk stays bounded. tavtex.py's prune_cache (bakes unused for
a while, then the least recently used past the cap; detail maps no bake names), crashreport.py's prune (the newest
reports) and wowsave.py's empty_bin (snapshots set aside, a month in the release). The functions are lifted out of
their files, so neither file's imports (numpy) are needed.

  python3 tools/test_disk_bounds.py
"""
import ast, datetime, os, re, shutil, tempfile, time, unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent


def lift(path, names, extra=None):
    """The named top-level functions and assignments of a file, run in a fresh namespace."""
    tree = ast.parse(path.read_text())
    keep = [n for n in tree.body if (isinstance(n, ast.FunctionDef) and n.name in names)
            or (isinstance(n, ast.Assign) and any(getattr(t, 'id', '') in names for t in n.targets))]
    ns = {'os': os, 're': re, 'time': time, 'Path': Path}
    ns.update(extra or {})
    exec(compile(ast.Module(body=keep, type_ignores=[]), str(path), 'exec'), ns)
    return ns


def write(p, size, age_days=0.0):
    p.write_bytes(b'x' * size)
    t = time.time() - age_days * 86400
    os.utime(p, (t, t))


class Tavtex(unittest.TestCase):
    def setUp(self):
        self.ns = lift(HERE / 'tavtex.py', {'prune_cache', 'CACHE_CAP', 'CACHE_IDLE_DAYS'})
        self.d = Path(tempfile.mkdtemp())

    def test_idle_bakes_go_and_used_ones_stay(self):
        write(self.d / 'aaaa.tvt', 10, 40)
        write(self.d / 'aaaa-p256.tvt', 10, 40)
        write(self.d / 'bbbb.tvt', 10, 1)
        write(self.d / 'bbbb-g.json', 0, 1)
        gone, _ = self.ns['prune_cache'](str(self.d))
        self.assertEqual(gone, 1)
        self.assertEqual(sorted(p.name for p in self.d.iterdir()), ['bbbb-g.json', 'bbbb.tvt'])

    def test_over_the_cap_the_least_recently_used_go(self):
        self.ns['CACHE_CAP'] = 25
        write(self.d / 'old1.tvt', 10, 3)
        write(self.d / 'old2.tvt', 10, 2)
        write(self.d / 'new1.tvt', 10, 0)
        gone, freed = self.ns['prune_cache'](str(self.d))
        self.assertEqual((gone, freed), (1, 10))
        self.assertFalse((self.d / 'old1.tvt').exists())
        self.assertTrue((self.d / 'old2.tvt').exists() and (self.d / 'new1.tvt').exists())

    def test_detail_maps_go_with_the_last_bake_naming_them(self):
        write(self.d / 'aaaa.tvt', 10, 40)
        (self.d / 'aaaa-g.json').write_text('{"detail": "d-11.tvt"}')
        os.utime(self.d / 'aaaa-g.json', (time.time() - 40 * 86400,) * 2)
        write(self.d / 'bbbb.tvt', 10, 1)
        (self.d / 'bbbb-g.json').write_text('{"detail": "d-22.tvt"}')
        write(self.d / 'd-11.tvt', 5)
        write(self.d / 'd-22.tvt', 5)
        self.ns['prune_cache'](str(self.d))
        self.assertFalse((self.d / 'd-11.tvt').exists())
        self.assertTrue((self.d / 'd-22.tvt').exists())

    def test_a_small_young_cache_is_left_alone(self):
        write(self.d / 'aaaa.tvt', 10, 1)
        self.assertEqual(self.ns['prune_cache'](str(self.d)), (0, 0))


class Reports(unittest.TestCase):
    def test_the_newest_reports_stay(self):
        ns = lift(HERE / 'crashreport.py', {'prune', 'KEEP'})
        d = Path(tempfile.mkdtemp())
        for i in range(8):
            write(d / ('WOW-BG3-report-2026010%d.zip' % i), 1, 8 - i)
        write(d / 'notes.txt', 1, 30)
        ns['prune'](d)
        left = sorted(p.name for p in d.iterdir())
        self.assertEqual(len([n for n in left if n.endswith('.zip')]), ns['KEEP'])
        self.assertIn('WOW-BG3-report-20260107.zip', left)
        self.assertIn('notes.txt', left)

    def test_the_engines_newest_crash_files_stay(self):
        ns = lift(HERE / 'crashreport.py', {'prune_crashes', 'KEEP_CRASHES'})
        d = Path(tempfile.mkdtemp())
        for i in range(14):
            write(d / ('crash-17000000%02d.txt' % i), 1, 14 - i)
        write(d / 'fx-trail.txt', 1, 30)
        write(d / 'fps-journal.csv', 1, 30)
        self.assertEqual(ns['prune_crashes'](d), 4)
        left = sorted(p.name for p in d.iterdir())
        self.assertEqual(len([n for n in left if n.startswith('crash-')]), ns['KEEP_CRASHES'])
        self.assertIn('crash-1700000013.txt', left)
        self.assertNotIn('crash-1700000000.txt', left)
        self.assertIn('fx-trail.txt', left)
        self.assertIn('fps-journal.csv', left)
        self.assertEqual(ns['prune_crashes'](d / 'none'), 0)

    def test_the_coordinator_prunes_them_at_each_start_of_the_engine(self):
        self.assertIn("crashreport.prune_crashes(DATA / 'benilla/Diagnostics')", (HERE / 'wowbg3-coord.py').read_text())


class SnapshotBin(unittest.TestCase):
    def ns(self, root, days):
        ns = lift(HERE / 'wowsave.py', {'empty_bin', 'BIN_DAYS'}, {'ROOT': root, 'datetime': datetime, 'shutil': shutil})
        ns['BIN_DAYS'] = days
        return ns

    def fill(self):
        root = Path(tempfile.mkdtemp())
        day = lambda n: (datetime.date.today() - datetime.timedelta(days=n)).strftime('%Y%m%d')
        names = ['wowsave-pruned-' + day(40), 'wowsave-rehomed-' + day(31), 'wowsave-pruned-' + day(30),
                 'wowsave-pruned-' + day(0), 'xprate-' + day(90), 'wowsave-pruned-notadate']
        for n in names:
            (root / '#recycle_bin' / n).mkdir(parents=True)
            (root / '#recycle_bin' / n / 'a.sql').write_text('x')
        return root, names

    def test_the_release_keeps_a_month(self):
        root, names = self.fill()
        gone = self.ns(root, 30)['empty_bin']()
        self.assertEqual(sorted(gone), sorted(names[:2]))
        self.assertEqual(sorted(p.name for p in (root / '#recycle_bin').iterdir()), sorted(names[2:]))

    def test_the_source_tree_keeps_everything(self):
        root, names = self.fill()
        self.assertEqual(self.ns(root, None)['empty_bin'](), [])
        self.assertEqual(len(list((root / '#recycle_bin').iterdir())), len(names))

    def test_the_release_sets_it(self):
        self.assertIn("sets['BIN_DAYS'] = 30", (HERE / 'relenv.py').read_text())


if __name__ == '__main__':
    unittest.main()
