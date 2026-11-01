#!/usr/bin/env python3
"""Part of a WoW stack costs the price of one each (inventory_bridge.py: buypart)."""
import sys, unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import inventory_bridge as I  # noqa: E402


class Be:
    def __init__(self, money, full=False):
        self.m, self.full, self.items = money, full, []

    def money(self, name, delta):
        if self.m + delta < 0:
            return False, 'notenough', {}
        self.m += delta
        return True, '', {'Money': self.m}

    def add(self, name, entry, count):
        if self.full:
            return False, 'full', {}
        self.items.append((entry, count))
        return True, '', {'Guid': 'g'}


def ask(be, count=2, price=4000, lot=5):
    b = I.Bridge.__new__(I.Bridge)
    b.be, b.char, b.pockets, b.vendors, b.loot = be, 'Tav', set(), {}, {}
    b.urgent = False
    req = {'Op': 'buypart', 'Member': 'm', 'Npc': 'n', 'Entry': 8950, 'Count': count, 'Price': price, 'Lot': lot}
    return b.handle(req, {'m': 'Tav'})


class BuyPart(unittest.TestCase):
    def test_two_of_a_five_stack(self):
        be = Be(5000)
        self.assertTrue(ask(be)[0])
        self.assertEqual((be.m, be.items), (5000 - 1600, [(8950, 2)]))

    def test_no_price_gives_nothing(self):
        be = Be(5000)
        self.assertFalse(ask(be, price=0)[0])
        self.assertEqual((be.m, be.items), (5000, []))

    def test_a_whole_purchase_is_no_part(self):
        be = Be(5000)
        self.assertFalse(ask(be, count=5)[0])
        self.assertEqual((be.m, be.items), (5000, []))

    def test_not_enough_money_buys_nothing(self):
        be = Be(100)
        self.assertFalse(ask(be)[0])
        self.assertEqual((be.m, be.items), (100, []))

    def test_a_full_bag_gives_the_money_back(self):
        be = Be(5000, full=True)
        self.assertFalse(ask(be)[0])
        self.assertEqual(be.m, 5000)


if __name__ == '__main__':
    unittest.main()
