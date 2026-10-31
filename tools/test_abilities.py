#!/usr/bin/env python3
"""Tests of wow_abilities.py's spell copies: no game, no game data."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
import wow_abilities as w

POWER = {'level_cap': 17, 'hero_hp': {'1': 10, '18': 147}, 'wow_hp': {'1': 52, '60': 3200}, 'turn_share': 0.2, 'area_cap': 0.7,
         'every_cap': {'turn': 1, 'recharge': 1.5, 'fight': 2}, 'rank_cap': {'normal': 1, 'elite': 1.25, 'boss': 2}, 'dc_elite': 1, 'dc_boss': 2}


def row(**k):
    return dict({'every': 'turn', 'cap': '1d6'}, **k)


def test_costs_and_cooldown():
    f = w.copy_fields({'UseCosts': 'ActionPoint:1;SpellSlotsGroup:1:1:2', 'Level': '2', 'Cooldown': 'OncePerRest'}, row(every='fight'))
    assert f['UseCosts'] == 'ActionPoint:1' and f['Level'] == '0' and f['Cooldown'] == 'OncePerCombat'
    assert w.copy_fields({}, row())['UseCosts'] == 'ActionPoint:1'
    assert w.copy_fields({'UseCosts': 'BonusActionPoint:1;Rage:1'}, row())['UseCosts'] == 'BonusActionPoint:1'
    assert w.copy_fields({'UseCosts': 'ActionPoint:1'}, row(every='recharge'))['RechargeValues'] == '5-6'


def test_damage():
    base = {'SpellSuccess': 'DealDamage(3d6,Fire,Magical);DealDamage(1d4,Cold);ApplyStatus(BURNING,100,2)', 'SpellFail': 'DealDamage((3d6)/2,Fire,Magical)'}
    f = w.copy_fields(base, row(dice='1d8', type='Cold'))
    assert f['SpellSuccess'] == 'DealDamage(1d8,Cold,Magical);ApplyStatus(BURNING,100,2)', f
    assert f['SpellFail'] == 'DealDamage(1d8/2,Cold,Magical)', f
    # A BG3 spell's own number is cut to the level's cap; a weapon's stays the weapon's.
    assert w.copy_fields({'SpellSuccess': 'DealDamage(8d6,Fire)'}, row())['SpellSuccess'] == 'DealDamage(1d6,Fire)'
    assert w.copy_fields({'SpellSuccess': 'DealDamage(LevelMapValue(BreathWeapon),Fire)'}, row())['SpellSuccess'] == 'DealDamage(1d6,Fire)'
    assert 'SpellSuccess' not in w.copy_fields({'SpellSuccess': 'DealDamage(max(1,MainMeleeWeapon), Slashing)'}, row())


def test_saves_and_riders():
    f = w.copy_fields({'SpellRoll': 'not SavingThrow(Ability.Wisdom, SourceSpellDC(), AdvantageOnFrightened())', 'SpellSuccess': 'ApplyStatus(FEAR,100,2)'}, row())
    assert f['SpellRoll'] == 'not SavingThrow(Ability.Wisdom,@DC@, AdvantageOnFrightened())', f
    f = w.copy_fields({'SpellRoll': 'Attack(AttackType.MeleeWeaponAttack)', 'SpellSuccess': 'DealDamage(MainMeleeWeapon,Slashing)'}, row(rider=['STUNNED', 1, 'Constitution']))
    assert f['SpellSuccess'].endswith(';IF(not SavingThrow(Ability.Constitution,@DC@)):ApplyStatus(STUNNED,100,1)'), f
    f = w.copy_fields({}, row(rider=['BURNING', 2, None]))
    assert f['SpellProperties'] == 'ApplyStatus(BURNING,100,2)', f


def test_level_one_is_gentle():
    # WoW level 1 to 4 is BG3's level 1: a save of 9, one die of 4 from a spell that can fire every turn.
    assert w.dc(4, 'normal', POWER) == 9
    assert w.dice(500, 4, POWER)[0] == '1d4'
    assert w.recharge(1000, 'nuke', False, 4, None) == 'recharge'
    assert w.recharge(1000, 'nuke', True, 4, None) == 'fight'


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_'):
            fn()
            print('ok', name)
