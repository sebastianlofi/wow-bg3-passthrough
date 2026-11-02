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


def test_stats_text():
    copies = {'abc': {'B': 'Projectile_FireBolt', 'Dc': 11, 'St': 'Projectile', 'F': {'SpellRoll': 'SavingThrow(Ability.Dexterity, @DC@)', 'DisplayName': 'WowBg3_Aabc;1'}}}
    s = w.stats_text(copies, 'new entry "Wall"\r\n')
    assert s.startswith('new entry "Wall"\r\n\r\nnew entry "WOWBG3_AB_abc"\r\ntype "SpellData"\r\ndata "SpellType" "Projectile"\r\nusing "Projectile_FireBolt"\r\n')
    assert 'SavingThrow(Ability.Dexterity, 11)' in s and 'data "DisplayName" "WowBg3_Aabc;1"' in s
    assert 'SavingThrow(Ability.Dexterity, SourceSpellDC())' in w.stats_text(copies, '', 'SourceSpellDC()')


RULES = w.load_rules(w.HERE / 'wow_abilities_rules.json')


def spell(e, name, **k):
    s = {c: 0 for c in w.SPELL}
    s.update(entry=e, name=name, nameSubtext='', description='', **k)
    return s


SPELLS = {100: spell(100, 'Fireball', school=2, effect1=2, effectBasePoints1=20, effectDieSides1=5, rangeIndex=4, castingTimeIndex=4),
          300: spell(300, 'Frost Nova', school=4, effect1=6, effectApplyAuraName1=26, mechanic=7)}


def mob(entry=42, casts=(100,)):
    return {'entry': entry, 'name': 'Test mob', 'level_min': 20, 'level_max': 20, 'type': 7, 'pet_family': 0, 'rank': 0, 'unit_class': 1,
            'damage_school': 0, 'events': [], 'auras': '', 'spawn_spell_id': 0, 'spawns': 3, 'faction': 14,
            'list': [{'spellId': c, 'probability': 100, 'castTarget': 1, 'delayRepeatMin': 5, 'delayRepeatMax': 8} for c in casts]}


def given(rules, m=None):
    return sorted(r['b'] for r in w.map_mob(m or mob(), SPELLS, rules)[1])


def test_overlay_merges_over_the_rules():
    over = {'_comment': 'a mod pack', 'power': {'dc_boss': 9}, 'picks': [{'w': ['Fireball'], 'tiers': [[1, 'Projectile_RayOfFrost']]}],
            'overrides': {'mobs': {'42': {'_name': 'Test mob', 'spells': {'100': None}}}}}
    r = w.merge_rules(RULES, over)
    assert r['power']['dc_boss'] == 9 and r['power']['level_cap'] == RULES['power']['level_cap']
    assert r['picks'][0]['tiers'][0][1] == 'Projectile_RayOfFrost' and len(r['picks']) == len(RULES['picks']) + 1
    assert '_comment' not in r and r['overrides']['mobs']['42']['spells'] == {'100': None}
    assert RULES['overrides']['mobs'] == {}, 'the shipped rules stay as they are'
    # A broken overlay is left out, not fatal.
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.json') as f:
        f.write('{not json')
        f.flush()
        assert w.load_rules(w.HERE / 'wow_abilities_rules.json', [f.name]) == RULES


def test_overrides_by_entry_and_spell_id():
    assert given(RULES) == ['Projectile_FireBolt']
    ov = lambda o: w.merge_rules(RULES, {'overrides': o})
    # Removed for this mob only; every other mob keeps it.
    r = ov({'mobs': {'42': {'spells': {'100': None}}}})
    assert given(r) == [] and given(r, mob(43)) == ['Projectile_FireBolt']
    # Another BG3 spell, with a rung's options; for every mob by spell id.
    r = ov({'spells': {'100': {'b': 'Projectile_RayOfFrost', 'every': 'fight'}}})
    rows = w.map_mob(mob(), SPELLS, r)[1]
    assert [(x['b'], x['src'], x['every']) for x in rows] == [('Projectile_RayOfFrost', 'override', 'fight')]
    # The mob's own entry wins over every mob's; true hands it back to the rules.
    r = ov({'spells': {'100': None}, 'mobs': {'42': {'spells': {'100': True}}}})
    assert given(r) == ['Projectile_FireBolt'] and given(r, mob(43)) == []
    # Given a spell it does not cast in WoW (a boss fight's extra), past the ability limit.
    r = ov({'mobs': {'42': {'spells': {'300': 'Target_Shove'}}}})
    assert given(r) == ['Projectile_FireBolt', 'Target_Shove']
    lim = w.merge_rules(r, {'power': {'abilities': {k: 0 for k in RULES['power']['abilities']}, 'abilities_low': 0}})
    assert given(lim) == ['Target_Shove']
    # Names are comments only: a name key does nothing.
    assert given(ov({'mobs': {'Test mob': {'spells': {'100': None}}}, 'spells': {'Fireball': None}})) == ['Projectile_FireBolt']


def test_local_seed_keeps_every_spell():
    import gzip, json, tempfile
    world = {'mobs': {'42': dict(mob(), places=[[0, 1, 2]], subname=''), '7': dict(mob(7, ()), places=[], subname='')},
             'spells': {str(k): v for k, v in SPELLS.items()}}
    with tempfile.TemporaryDirectory() as d:
        w.seed(world, d + '/s.gz')
        small = json.load(gzip.open(d + '/s.gz', 'rt'))
        w.seed(world, d + '/f.gz', full=True)
        full = json.load(gzip.open(d + '/f.gz', 'rt'))
    assert set(small['spells']) == {'100'} and set(small['mobs']) == {'42'}
    assert set(full['spells']) == {'100', '300'} and set(full['mobs']) == {'42', '7'} and full['SeedFormat'] == w.SEED_FORMAT
    assert 'places' not in full['mobs']['42'] and 'description' not in full['spells']['100']


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_'):
            fn()
            print('ok', name)
