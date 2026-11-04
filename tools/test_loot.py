#!/usr/bin/env python3
"""A corpse's loot (inventory_bridge.py: loot, take): BG3's takes from one corpse go to WoW in one call, each with
the looter's place at BG3's take (server patch 0025); the corpse is listed once after them, and a list the server
refused keeps what the corpse was last listed with."""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import inventory_bridge as I  # noqa: E402


class Be:
    """The server: a corpse with items; out of reach once `far` is set."""
    def __init__(self, items):
        self.items, self.far, self.calls = dict(items), False, []

    def loot(self, npc, name=None):
        self.calls.append(('list', npc))
        if self.far:
            return None, 'far'
        return {'Money': 0, 'Items': [{'Idx': i, 'Entry': e, 'Count': 1} for i, e in sorted(self.items.items())]}, ''

    def take_many(self, name, npc, entries):
        self.calls.append(('take', [i for i, _ in entries]))
        self.places = [at for _, at in entries]
        out = {}
        for i, _ in entries:
            if self.far:
                out[str(i)] = (False, 'far', {})
            elif self.items.pop(int(i), None) is None:
                out[str(i)] = (False, 'gone', {})
            else:
                out[str(i)] = (True, '', {'Guid': 'g'})
        return out


def bridge(be):
    b = I.Bridge.__new__(I.Bridge)
    b.be, b.char, b.pockets, b.vendors, b.loot, b.relist = be, 'Tav', set(), {}, {}, {}
    b.todo, b.taken = [], {}
    b.urgent = False
    return b


def req(op, **kw):
    return dict(kw, Op=op, Member='m', Npc='c')


class Loot(unittest.TestCase):
    def test_take_all_is_one_call_then_one_list(self):
        be = Be({0: 7098, 1: 4865, 2: 117})
        b = bridge(be)
        self.assertTrue(b.handle(req('loot'), {'m': 'Tav'})[0])
        be.calls = []
        b.todo = [req('take', Idx=i, Seq=s, At=[-502.9, -4325.2, 41.3 + s]) for s, i in ((3, 1), (4, 0), (5, 2))]
        for r in b.todo:
            self.assertTrue(b.handle(r, {'m': 'Tav'})[0])
        self.assertEqual(be.calls, [('take', [1, 0, 2])])
        self.assertEqual([p[2] for p in be.places], [44.3, 45.3, 46.3])
        b.relist_loot()
        self.assertEqual(be.calls[-1], ('list', 'c'))
        self.assertEqual(b.loot['c']['Items'], [])
        self.assertTrue(b.urgent)

    def test_a_refused_list_keeps_the_last(self):
        be = Be({0: 7098, 2: 117})
        b = bridge(be)
        b.handle(req('loot'), {'m': 'Tav'})
        self.assertTrue(b.handle(req('take', Idx=0, Seq=3), {'m': 'Tav'})[0])
        be.far = True  # the looter walked on before the list after the take
        b.relist_loot()
        # the taken pelt is gone (WoW said so), the jerky WoW still has stays
        self.assertEqual([r['Entry'] for r in b.loot['c']['Items']], [117])
        ok, why, _ = b.handle(req('loot'), {'m': 'Tav'})
        self.assertEqual((ok, why), (False, 'far'))
        self.assertEqual([r['Entry'] for r in b.loot['c']['Items']], [117])

    def test_the_token_carries_each_place(self):
        self.assertEqual(I.Server.take_token([(1, [-502.86, -4325.22, 41.32]), (0, None), ('money', [1, 2, 3])]),
                         '1@-502.86/-4325.22/41.32,0,money@1.00/2.00/3.00')

    def test_the_console_takes_the_token_and_a_long_batch_is_split(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('wc', str(Path(__file__).resolve().parent / 'world-console.py'))
        wc = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(wc)
        s = I.Server.__new__(I.Server)
        sent = []
        def call(*w):
            line = wc.line_for(['wowbg3'] + [str(x) for x in w])
            sent.append(line)
            return [l.split() for l in ('WOWBG3 loot Karlach %s take %s ok - 0' % (w[3], e.split('@')[0]) for e in w[4].split(','))] if line else []
        s.call = call
        at = [-1713.57, 1681.98, 133.09]
        res = s.take_many('Karlach', 'f1300005e800af03', [(i, at) for i in range(12)] + [('money', at)])
        self.assertTrue(all(ok for ok, _, _ in res.values()) and len(res) == 13, res)
        self.assertTrue(len(sent) > 1 and all(l and len(l) <= 240 for l in sent), sent)
        self.assertIsNone(wc.line_for(['wowbg3', 'loot', 'take', 'Karlach', 'f13', '1@x/2/3']))

    def test_server_lines_per_entry(self):
        s = I.Server.__new__(I.Server)
        s.call = lambda *a: [l.split() for l in (
            'WOWBG3 loot Tav f13 take 1 ok 4000000000000870 0', 'WOWBG3 loot Tav f13 take 0 refused far')]
        self.assertEqual(s.take_many('Tav', 'f13', [(1, None), (0, None)]),
                         {'1': (True, '', {'Guid': '4000000000000870'}), '0': (False, 'far', {})})
        s.call = lambda *a: [['WOWBG3', 'loot', 'Tav', 'f13', 'refused', 'missing']]
        self.assertEqual(s.take_many('Tav', 'f13', [(2, None)]), {'2': (False, 'missing', {})})

    def test_a_refused_take_lists_nothing(self):
        be = Be({0: 7098})
        b = bridge(be)
        be.far = True
        self.assertEqual(b.handle(req('take', Idx=0, Seq=3), {'m': 'Tav'})[:2], (False, 'far'))
        self.assertEqual(b.relist, {})


if __name__ == '__main__':
    unittest.main()
