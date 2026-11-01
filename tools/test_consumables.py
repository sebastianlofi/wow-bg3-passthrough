#!/usr/bin/env python3
"""test_consumables.py: WoW consumables by what their spell does, without a game or a database.

  python3 tools/test_consumables.py
"""
import collections, json, sys, tempfile, types, unittest
from unittest import mock
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import inventory_bridge
import wow_items

RULES = json.loads((HERE / 'wow_items_rules.json').read_text())
AURA = wow_items.EFFECT_APPLY_AURA


def spell(entry, *effects):
    """effects: (effect, aura, misc, points[, trigger])."""
    s = collections.defaultdict(int, entry=entry, name='spell %d' % entry, description='')
    for n, e in enumerate(effects, 1):
        s['effect%d' % n], s['effectApplyAuraName%d' % n], s['effectMiscValue%d' % n] = e[:3]
        s['effectBasePoints%d' % n], s['effectDieSides%d' % n] = e[3] - 1, 0
        if len(e) > 4:
            s['effectTriggerSpell%d' % n] = e[4]
    return s


def row_of(name, ilvl, *spells, category=0, price=100):
    """A consumable whose first spell is spells[0]; the others are what it triggers."""
    it = collections.defaultdict(int, entry=1, name=name, item_level=ilvl, spellid_1=spells[0]['entry'],
                                 spellcategory_1=category, buy_price=price, stackable=20)
    it['class'] = 0
    row = wow_items.map_item(RULES, it, {})
    wow_items.add_effects(RULES, row, it, {s['entry']: s for s in spells})
    return row


def base(name, ilvl, *effects, **kw):
    return row_of(name, ilvl, spell(1, *effects), **kw)['b']


class Mana(unittest.TestCase):
    def test_mana_potions_give_a_slot_by_wow_amount(self):
        want = {160: '', 320: '', 520: '_Greater', 800: '_Greater', 1200: '_Superior', 1800: '_Supreme'}
        for amount, tier in want.items():
            self.assertEqual(base('Mana Potion', 30, (30, 0, 0, amount)), 'ALCH_Solution_Elixir_Meditation' + tier)

    def test_rejuvenation_counts_as_mana(self):
        self.assertEqual(base('Major Rejuvenation Potion', 60, (30, 0, 0, 1600), (10, 0, 0, 1600)),
                         'ALCH_Solution_Elixir_Meditation_Supreme')

    def test_what_a_mage_makes_for_free_stays_at_the_first_tier(self):
        self.assertEqual(base('Mana Ruby', 58, (30, 0, 0, 1100)), 'ALCH_Solution_Elixir_Meditation')
        self.assertEqual(base('Major Healthstone', 58, (10, 0, 0, 1440)), 'OBJ_Potion_Healing')

    def test_mageblood_is_a_slot_but_water_stays_water(self):
        self.assertEqual(base('Mageblood Potion', 50, (AURA, 85, 0, 12)), 'ALCH_Solution_Elixir_Meditation')
        self.assertEqual(base('Refreshing Spring Water', 5, (AURA, 85, 0, 42), category=59), 'CONS_DRINK_Water')


class Others(unittest.TestCase):
    def test_rage_and_energy(self):
        self.assertEqual(base('Rage Potion', 14, (30, 0, 1, 300)), 'ALCH_Solution_Elixir_Bloodlust')
        self.assertEqual(base('Mighty Rage Potion', 51, (30, 0, 1, 600), (AURA, 29, 0, 60)), 'ALCH_Solution_Elixir_Bloodlust')
        self.assertEqual(base('Thistle Tea', 15, (30, 0, 3, 100)), 'ALCH_Solution_Elixir_Alertness')

    def test_stat_elixirs_by_size(self):
        self.assertEqual(base("Elixir of Lion's Strength", 5, (AURA, 29, 0, 4)), 'ALCH_Solution_Elixir_Alertness')
        self.assertEqual(base("Elixir of Ogre's Strength", 30, (AURA, 29, 0, 8)), 'ALCH_Solution_Elixir_Enlarge')
        self.assertEqual(base('Elixir of Giants', 48, (AURA, 29, 0, 25)), 'OBJ_Potion_Of_Hill_Giant_Strength')
        self.assertEqual(base('Elixir of Greater Agility', 48, (AURA, 29, 1, 25)), 'ALCH_Solution_Elixir_Criticals')
        self.assertEqual(base('Elixir of Greater Intellect', 47, (AURA, 29, 3, 25)), 'ALCH_Solution_Elixir_ArcaneAcuity')
        self.assertEqual(base('Elixir of Fortitude', 35, (AURA, 34, 0, 120)), 'OBJ_Potion_Of_Heroism')

    def test_a_penalty_is_no_buff(self):
        self.assertEqual(base('Severed Voodoo Claw', 5, (AURA, 13, 1, -5)), 'OBJ_Bone')
        self.assertEqual(base('Raptor Punch', 18, (AURA, 29, 3, 4), (AURA, 29, 2, -5)), 'ALCH_Solution_Elixir_Alertness')

    def test_resistance_by_school(self):
        school = {2: 'Radiant', 4: 'Fire', 8: 'Poison', 16: 'Cold', 32: 'Necrotic', 64: 'Force'}
        for bit, name in school.items():
            self.assertEqual(base('Protection Potion', 30, (AURA, 69, bit, 900)), 'OBJ_Potion_Of_%s_Resistance' % name)
        self.assertEqual(base('Elixir of Defense', 26, (AURA, 22, 1, 150)), 'ALCH_Solution_Elixir_Barkskin')
        self.assertEqual(base('Minor Magic Resistance Potion', 22, (AURA, 143, 126, 25)), 'OBJ_Potion_Of_Force_Resistance')
        self.assertEqual(base('Magic Resistance Potion', 42, (AURA, 143, 126, 50)), 'OBJ_Potion_Of_Invulnerability')

    def test_heal_by_amount(self):
        self.assertEqual(base('Minor Healing Potion', 5, (10, 0, 0, 80)), 'OBJ_Potion_Healing')
        self.assertEqual(base('Healing Potion', 22, (10, 0, 0, 320)), 'OBJ_Potion_Healing_Greater')
        self.assertEqual(base('Superior Healing Potion', 45, (10, 0, 0, 800)), 'OBJ_Potion_Healing_Superior')
        self.assertEqual(base('Whipper Root Tuber', 55, (10, 0, 0, 1400)), 'OBJ_Potion_Healing_Supreme')
        self.assertEqual(base('Linen Bandage', 1, (AURA, 8, 0, 11)), 'OBJ_Potion_Healing')

    def test_makes_an_item_or_no_rule_stays_a_plain_item(self):
        self.assertEqual(base('Bundle of Cards', 1, (24, 0, 0, 1), (AURA, 29, 1, 30)), 'OBJ_Bone')
        self.assertEqual(base('Sweet Surprise', 1, (AURA, 135, 127, 44)), 'OBJ_Bone')


class Food(unittest.TestCase):
    def fed(self, name, ilvl, *well_fed):
        food = spell(1, (AURA, 84, 0, 100), (AURA, 23, 0, 1, 2))
        return row_of(name, ilvl, food, spell(2, *well_fed), category=11, price=100)

    def test_well_fed_is_read_through_the_trigger(self):
        self.assertEqual(self.fed('Monster Omelet', 45, (AURA, 29, 2, 12), (AURA, 29, 4, 12))['b'], 'OBJ_Potion_Of_Heroism')
        self.assertEqual(self.fed('Grilled Squid', 45, (AURA, 29, 1, 10))['b'], 'ALCH_Solution_Elixir_Criticals')
        self.assertEqual(self.fed('Smoked Desert Dumplings', 55, (AURA, 29, 0, 20))['b'], 'ALCH_Solution_Elixir_Enlarge')

    def test_a_small_well_fed_stays_food(self):
        self.assertEqual(self.fed('Spiced Wolf Meat', 7, (AURA, 29, 2, 2), (AURA, 29, 4, 2))['b'], 'CONS_FOOD_Bread_Loaf')

    def test_supply_and_price_do_not_change(self):
        plain = self.fed('Spiced Wolf Meat', 45, (AURA, 29, 2, 2))
        buffed = self.fed('Spiced Wolf Meat', 45, (AURA, 29, 2, 12))
        self.assertNotEqual(plain['b'], buffed['b'])
        for k in ('sv', 'bp', 'sp'):
            self.assertEqual(plain[k], buffed[k])
        self.assertNotIn('us', buffed)

    def test_food_with_regeneration_or_mana_stays_food(self):
        self.assertEqual(self.fed('Nightfin Soup', 45, (AURA, 24, 0, 8))['b'], 'CONS_FOOD_Dinner')
        self.assertEqual(self.fed('Poached Sunscale Salmon', 45, (AURA, 8, 1, 6))['b'], 'CONS_FOOD_Dinner')

    def test_chili_breathes_fire(self):
        food = spell(1, (AURA, 42, 0, 1, 2))
        self.assertEqual(row_of('Dragonbreath Chili', 40, food, spell(2, (2, 0, 0, 65)), category=11)['b'], 'OBJ_Potion_Of_Fire_Breath')

    def test_a_loop_of_triggers_ends(self):
        self.assertEqual(row_of('Loop', 5, spell(1, (AURA, 23, 0, 1, 2)), spell(2, (AURA, 23, 0, 1, 1)))['b'], 'OBJ_Bone')


class Kept(unittest.TestCase):
    def test_alcohol_and_named_water_keep_their_rules(self):
        self.assertEqual(row_of('Volatile Rum', 1, spell(11009, (100, 0, 0, 20)))['b'], 'CONS_DRINK_Beer')
        rum = row_of('Rumsey Rum', 1, spell(1, (AURA, 29, 2, 10), (64, 0, 0, 1, 2)), spell(2, (100, 0, 0, 20)))
        self.assertNotEqual(rum['b'], 'OBJ_Potion_Of_Heroism')
        spirit = row_of('Bottled Alterac Spring Water', 55, spell(1, (AURA, 85, 0, 735), (AURA, 29, 4, 25)), category=59)
        self.assertEqual(spirit['b'], 'CONS_DRINK_Water')

    def test_an_elixir_named_for_water_is_no_water(self):
        self.assertEqual(base('Elixir of Water Breathing', 18, (AURA, 82, 0, 1)), 'ALCH_Solution_Elixir_Darkvision')

    def test_only_a_usable_base_is_marked_for_spending(self):
        self.assertEqual(row_of('Mana Potion', 30, spell(1, (30, 0, 0, 520))).get('us'), 1)
        self.assertNotIn('us', row_of('Sweet Surprise', 1, spell(1, (AURA, 135, 127, 44))))

    def test_only_consumables_follow(self):
        it = collections.defaultdict(int, entry=1, name='Ore', spellid_1=1)
        it['class'] = 7
        row = wow_items.map_item(RULES, it, {})
        wow_items.add_effects(RULES, row, it, {1: spell(1, (30, 0, 0, 800))})
        self.assertEqual(row['b'], 'OBJ_Bone')

    def test_schema_matches_the_coordinator(self):
        text = (HERE / 'wowbg3-coord.py').read_text()
        self.assertIn('int(m.group(1)) >= %d' % wow_items.ITEMS_SCHEMA, text)


class HeroInTheList(unittest.TestCase):
    """A save writes hero.json without the hero: a list without it cost the hero everything (Inventory.lua)."""

    def test_hero_stays_listed(self):
        with tempfile.TemporaryDirectory() as tmp:
            io = Path(tmp)
            (io / 'party-chars.json').write_text(json.dumps({'A': 'Astarion'}))
            bridge = types.SimpleNamespace(char='Hero', extra={}, hero=None)
            with mock.patch.object(inventory_bridge, 'IO', io), mock.patch.object(inventory_bridge, 'LINK', io):
                for hero, want in (('G', 'G'), (None, 'G'), ('A', 'G'), ('T', 'T')):
                    (io / 'hero.json').write_text(json.dumps({'Uuid': hero} if hero else {'State': 'Save'}))
                    self.assertEqual(inventory_bridge.Bridge.members(bridge), {want: 'Hero', 'A': 'Astarion'})


if __name__ == '__main__':
    unittest.main()
