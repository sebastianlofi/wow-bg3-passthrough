#!/usr/bin/env python3
"""wow_abilities.py: what each WoW mob casts, as BG3 abilities its stand-in can use.

  wow_abilities.py export OUT.json [--db CMD]    read the server's database (read-only selects) and the
                                                 client's DBCs into one world file
  wow_abilities.py seed WORLD.json OUT.json.gz   the part of a world file the table needs, for the release
  wow_abilities.py make OUT.json --bg3 D --seed F   build from a seed, no database: what a first start runs
  wow_abilities.py make OUT.json --data D --bg3 D [--db CMD]    export and build, when no seed shipped
  wow_abilities.py build WORLD.json --out FILE --bg3 D|STATS.json [--rules FILE]
                                                 the table the mod reads (link/abilities.json)
  wow_abilities.py show WORLD.json NAME|ENTRY    one mob's rows, for tuning the rules
  wow_abilities.py stats WORLD.json              coverage and the most common unmapped spells

Three layers, later ones winning (rules in wow_abilities_rules.json next to this file):
  1. meaning: every WoW spell record gets a tag from its effects, auras, mechanic, dispel type and
     school (stun, root, poison, nuke:fire ...); a ladder per tag says which BG3 spell a mob of that level
     uses. Nothing per creature, so every mob with a WoW spell gets something.
  2. picks: by WoW spell name, the choices a player should recognise.
  3. overrides: by creature entry or name, rulings from a review of the table.
Power is decided here, not in BG3's data: see ladder floors, dc(), recharge() and dice() below.

Sources: creature_template, creature_spells, creature_ai_events and creature_ai_scripts (spell lists and
EventAI), spell_template, and the player's own DBCs. The world file and the table hold names and numbers from
the player's own server and client; both are generated on the player's machine, neither is ever committed.
The seed holds the same server's numbers and spell names, goes in the release beside the server's own SQL, and
is not committed either.

The copies themselves go in stats files (abilities-stats.txt, -scaled.txt with SourceSpellDC()): BG3's own loader
reads them through Ext.IO.AddPathOverride on a small file of its own, whose text they start with. Abilities.lua."""
import argparse, gzip, hashlib, json, math, os, re, struct, subprocess, sys
from collections import Counter, defaultdict
from pathlib import Path

HERE = Path(__file__).resolve().parent

CREATURE = ['entry', 'patch', 'name', 'subname', 'level_min', 'level_max', 'type', 'pet_family', '`rank`', 'unit_class',
            'damage_school', 'spell_list_id', 'ai_name', 'script_name', 'auras', 'faction', 'npc_flags', 'spawn_spell_id']
LIST_SLOT = ['spellId', 'probability', 'castTarget', 'delayRepeatMin', 'delayRepeatMax']
EVENTS = ['creature_id', 'event_type', 'event_chance', 'event_param1', 'event_param2', 'event_param3', 'event_param4',
          'action1_script', 'action2_script', 'action3_script']
SPELL = ['entry', 'build', 'name', 'nameSubtext', 'description', 'school', 'mechanic', 'dispel', 'castingTimeIndex', 'recoveryTime',
         'categoryRecoveryTime', 'durationIndex', 'rangeIndex', 'powerType', 'manaCost', 'dmgClass', 'attributes', 'spellLevel',
         'baseLevel', 'targets'] + \
        ['%s%d' % (c, i) for i in (1, 2, 3) for c in (
            'effect', 'effectApplyAuraName', 'effectBasePoints', 'effectDieSides', 'effectDicePerLevel', 'effectRealPointsPerLevel',
            'effectMechanic', 'effectImplicitTargetA', 'effectRadiusIndex', 'effectMiscValue', 'effectTriggerSpell', 'effectAmplitude')]
TEXT = {'name', 'subname', 'ai_name', 'script_name', 'auras', 'nameSubtext', 'description'}


def db_query(db_cmd, cols, table):
    """Rows of one select. Split on newlines only: spell texts hold bare carriage returns."""
    q = 'select %s from %s;' % (', '.join(cols), table)
    if db_cmd:
        r = subprocess.run(db_cmd, input=q.encode(), capture_output=True)
        code, out, err = r.returncode, r.stdout, r.stderr
    else:
        sys.path.insert(0, str(HERE))
        import wowserver
        r = wowserver.db_sh('mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" --batch mangos', q)
        code, out, err = r.returncode, r.stdout.encode(errors='replace'), r.stderr.encode(errors='replace')
    if code:
        sys.exit('database: %s' % err.decode(errors='replace').strip())
    lines = out.decode(errors='replace').rstrip('\n').split('\n')
    head = lines[0].split('\t')
    for line in lines[1:]:
        v = dict(zip(head, line.split('\t')))
        for k in v:
            if k not in TEXT:
                try:
                    v[k] = float(v[k]) if '.' in v[k] else int(v[k])
                except ValueError:
                    pass
        yield v


def dbc_rows(data_dir, name):
    """A DBC's rows as tuples of uint32, plus its string reader."""
    sys.path.insert(0, str(HERE))
    from wow_mpq import WowData
    raw = WowData(str(data_dir)).read('DBFilesClient\\%s.dbc' % name)
    if raw is None:
        sys.exit('no %s.dbc in %s' % (name, data_dir))
    _, n, f, rs, ss = struct.unpack('<4s4I', raw[:20])
    strs = raw[20 + n * rs:]
    text = lambda o: strs[o:strs.index(b'\0', o)].decode('latin-1') if 0 < o < ss else ''
    return [struct.unpack_from('<%dI' % f, raw, 20 + r * rs) for r in range(n)], text


def _float(u):
    return struct.unpack('<f', struct.pack('<I', u))[0]


def load_dbcs(data_dir):
    """Number tables of the spell records, the factions (who is hostile to players) and the map rectangles."""
    d = {}
    rows, _ = dbc_rows(data_dir, 'SpellRange')
    d['range'] = {r[0]: round(_float(r[2]), 1) for r in rows}
    rows, _ = dbc_rows(data_dir, 'SpellCastTimes')
    d['cast'] = {r[0]: r[1] for r in rows}
    rows, _ = dbc_rows(data_dir, 'SpellDuration')
    d['duration'] = {r[0]: struct.unpack('<i', struct.pack('<I', r[1]))[0] for r in rows}
    rows, _ = dbc_rows(data_dir, 'SpellRadius')
    d['radius'] = {r[0]: round(_float(r[1]), 1) for r in rows}
    rows, _ = dbc_rows(data_dir, 'FactionTemplate')
    d['faction'] = {r[0]: [r[3], r[4], r[5]] for r in rows}  # our mask, friend mask, hostile mask
    rows, text = dbc_rows(data_dir, 'WorldMapArea')
    d['zones'] = [[r[1], text(r[3]), _float(r[4]), _float(r[5]), _float(r[6]), _float(r[7])] for r in rows if r[2]]
    rows, text = dbc_rows(data_dir, 'Map')
    d['maps'] = {r[0]: text(r[4]) for r in rows}
    return d


# ---- layer 1: what a WoW spell does, from its record
EF_SCHOOL, EF_AURA, EF_LEECH, EF_HEAL, EF_WEAPON = 2, 6, 9, 10, 17
EF_WEAPONS = {17, 31, 58, 121}
EF_SUMMON = {28, 41, 42, 56, 73, 74, 87, 88, 89, 90, 93, 97, 112}
SCHOOLS = {0: 'physical', 1: 'holy', 2: 'fire', 3: 'nature', 4: 'frost', 5: 'shadow', 6: 'arcane'}
ENEMY_TARGETS = {6, 15, 16, 22, 28, 53, 54}   # implicit targets on enemies (a chain, an area, a cone)
AREA_TARGETS = {15, 16, 22, 24, 28, 37, 53}   # around, at a spot, a cone
SELF_TARGETS = {1}


def effects(sp):
    for i in (1, 2, 3):
        if sp.get('effect%d' % i):
            yield {'e': sp['effect%d' % i], 'a': sp['effectApplyAuraName%d' % i], 'b': sp['effectBasePoints%d' % i] + 1,
                   'die': sp['effectDieSides%d' % i], 'm': sp['effectMechanic%d' % i], 't': sp['effectImplicitTargetA%d' % i],
                   'r': sp['effectRadiusIndex%d' % i], 'trig': sp['effectTriggerSpell%d' % i], 'misc': sp['effectMiscValue%d' % i],
                   'amp': sp['effectAmplitude%d' % i], 'lvl': sp['effectRealPointsPerLevel%d' % i]}


# Aura type -> tag on an enemy (harm) and on an ally or self (help). Sign of the amount decides the rest.
HARD_AURAS = {12: 'stun', 26: 'root', 27: 'silence', 7: 'fear', 5: 'confuse', 6: 'charm', 25: 'pacify', 60: 'pacify', 67: 'disarm', 11: 'taunt'}
MECHANIC = {1: 'charm', 2: 'disorient', 5: 'fear', 7: 'root', 9: 'silence', 10: 'sleep', 11: 'snare', 12: 'stun', 13: 'freeze', 14: 'knockout',
            15: 'bleed', 17: 'polymorph', 18: 'banish', 20: 'shackle', 23: 'turn', 24: 'horror', 26: 'interrupt', 27: 'daze', 30: 'sap'}
HARD_MECHANICS = ('polymorph', 'sleep', 'banish', 'horror', 'shackle', 'knockout', 'sap', 'freeze', 'disorient', 'turn', 'charm')
DISPEL = {1: 'magic', 2: 'curse', 3: 'disease', 4: 'poison'}


def tag_spell(sp, spells, depth=0):
    """The tag of one WoW spell: the most telling thing it does. Damage is worked out separately."""
    fx = list(effects(sp))
    school = SCHOOLS.get(sp['school'], 'physical')
    dispel = DISPEL.get(sp['dispel'])
    es = {f['e'] for f in fx}
    auras = {f['a']: f for f in fx if f['e'] in (EF_AURA, 27, 35) and f['a']}
    harmful = any(f['t'] in ENEMY_TARGETS for f in fx)
    mech = MECHANIC.get(sp['mechanic']) or next((MECHANIC[f['m']] for f in fx if f['m'] in MECHANIC), None)
    area = any(f['t'] in AREA_TARGETS and f['r'] for f in fx)
    if es & EF_SUMMON:
        return 'summon'
    if auras and all(f['t'] in SELF_TARGETS for f in auras.values()) and set(auras) & set(HARD_AURAS):
        return 'script'
    if 96 in es:
        return 'charge'
    if 29 in es:
        return 'leap'
    if 5 in es or 43 in es:
        return 'blink' if any(f['t'] in SELF_TARGETS or f['t'] == 0 for f in fx) else 'teleport_other'
    if mech in HARD_MECHANICS:
        return 'knockout' if mech in ('sap',) else mech
    for a in (7, 12, 5, 6, 26, 27, 25, 60, 67, 11):
        if a in auras:
            tag = HARD_AURAS[a]
            if a == 12 and mech == 'daze':
                return 'daze'
            return tag + ('_area' if area and tag in ('root', 'fear', 'stun', 'silence') else '')
    if 10 in es or 67 in es:
        return 'heal_area' if any(f['t'] in (20, 30, 31, 33, 34, 37) for f in fx) else 'heal'
    if 8 in auras:
        return 'hot'
    if 9 in es or 53 in auras:
        return 'leech'
    if 98 in es:
        return 'knockback'
    if 124 in es or 70 in es:
        return 'pull'
    if 68 in es:
        return 'interrupt'
    if 19 in es:
        return 'extra_attack'
    if 38 in es:
        return 'dispel'
    if 8 in es or 62 in es or 64 in auras:
        return 'mana_drain'
    if 42 in auras or 43 in auras:
        t = auras.get(42, {}).get('trig')
        if t and t in spells and depth < 3:
            return 'proc_' + tag_spell(spells[t], spells, depth + 1)
        return 'proc_damage'
    if 23 in auras:
        t = auras[23]['trig']
        if t and t in spells and depth < 3:
            return 'pulse_' + tag_spell(spells[t], spells, depth + 1)
        return 'pulse'
    if 3 in auras or 89 in auras:
        if mech == 'bleed':
            return 'bleed'
        kind = dispel if dispel in ('poison', 'disease', 'curse') else None
        return (kind or 'dot') + ('_area' if area or 27 in es else ':' + school if not kind else '')
    if 27 in es:
        return 'zone:' + school
    if 16 in auras or 18 in auras:
        return 'stealth'
    if 33 in auras or 9 in auras and auras[9]['b'] < 0 or 138 in auras and auras[138]['b'] < 0:
        if EF_SCHOOL in es:  # a nuke with a slow on it (Frostbolt) is first a nuke
            return ('nuke_area:' if area else 'nuke:') + school
        return 'snare' if 33 in auras else 'slow_attacks'
    for a, f in auras.items():
        neg = f['b'] < 0
        if a in (22, 83, 101, 142) and (neg or harmful) and not (f['t'] in SELF_TARGETS):
            return 'armor_down'
        if a in (99, 13, 79, 124) and neg:
            return 'attack_down'
        if a in (29, 80) and neg:
            return 'curse' if dispel == 'curse' else 'stat_down'
        if a in (14, 87) and not neg and harmful:
            return 'vulnerable'
        if a == 118 and neg:
            return 'heal_down'
        if a == 54 and neg:
            return 'hit_down'
        if a == 65 and neg:
            return 'slow_casts'
    if dispel == 'curse' and harmful:
        return 'curse'
    if dispel in ('poison', 'disease') and harmful:
        return dispel
    for a, f in auras.items():
        if a in (15,):
            return 'thorns:' + school
        if a in (69, 97):
            return 'absorb'
        if a in (16, 18):
            return 'stealth'
        if a in (138, 9, 65):
            return 'haste'
        if a in (99, 13, 79):
            return 'attack_up_group' if 35 in es or f['t'] in (20, 30, 31, 45) else 'attack_up'
        if a in (22, 83, 101, 142):
            return 'armor_up'
        if a in (31, 129):
            return 'sprint'
        if a in (47, 49, 51):
            return 'block_up'
        if a in (37, 38, 39, 40, 41, 77):
            return 'immune'
        if a in (28, 74):
            return 'reflect'
        if a in (36, 56):
            return 'shapeshift'
        if a in (34, 133, 80, 29):
            return 'stat_up'
        if a in (84, 85, 110):
            return 'regen'
        if a in (61,):
            return 'scale'
    if es & EF_WEAPONS:
        return 'cleave' if area or any(f['t'] in (15, 16, 24, 8) for f in fx) else ('shot' if sp['dmgClass'] == 3 else 'strike')
    if EF_SCHOOL in es:
        if school == 'physical':
            return 'physical_area' if area else ('shot' if sp['dmgClass'] == 3 else 'strike')
        return ('nuke_area:' if area else 'nuke:') + school
    if 78 in es:
        return 'strike'
    if 77 in es or 3 in es or 64 in es:
        return 'script'
    return 'other'


def spell_damage(sp):
    """Average damage of one hit (or the whole of a damage over time) from the record, at the record's level."""
    total = 0
    fx = list(effects(sp))
    for f in fx:
        avg = f['b'] + (f['die'] - 1) / 2 if f['die'] > 1 else f['b']
        if f['e'] == EF_SCHOOL or f['e'] == 9:
            total += avg
        elif f['e'] in EF_WEAPONS and f['e'] != 31:
            total += avg
        elif f['e'] == EF_AURA and f['a'] == 3:
            ticks = 1
            total += avg * ticks
    return max(0, total)


# ---- layers 2 and 3, and power
RANKS = {0: 'normal', 1: 'elite', 2: 'elite', 3: 'boss', 4: 'normal'}
HARD_STATUS = re.compile(r'STUNNED|SLEEP|FEAR|POLYMORPH|DOMINATE|HOLD_PERSON|PARALY|BANISH|FLESH_TO_STONE|CHARM|ENTOMB|CONFUS')
FAMILIES = {1: 'wolf', 2: 'cat', 3: 'spider', 4: 'bear', 5: 'boar', 6: 'crocolisk', 7: 'carrion bird', 8: 'crab', 9: 'gorilla',
            11: 'raptor', 12: 'tallstrider', 20: 'scorpid', 21: 'turtle', 24: 'bat', 25: 'hyena', 26: 'owl', 27: 'wind serpent'}
TYPES = {1: 'beast', 2: 'dragonkin', 3: 'demon', 4: 'elemental', 5: 'giant', 6: 'undead', 7: 'humanoid', 8: 'critter', 9: 'mechanical', 10: 'other'}
ARMED_TYPES = {7, 3, 6, 5}  # stand-ins with a weapon in hand (human, goblin; zombie and others hit like them)
DICE = [(4, 2.5), (6, 3.5), (8, 4.5), (10, 5.5), (12, 6.5)]


def curve(table, level):
    pts = sorted((int(k), v) for k, v in table.items())
    if level <= pts[0][0]:
        return pts[0][1]
    for (a, x), (b, y) in zip(pts, pts[1:]):
        if level <= b:
            return x + (y - x) * (level - a) / (b - a)
    return pts[-1][1]


def effective_level(mob, power):
    lvl = (mob['level_min'] + mob['level_max']) // 2
    rank = RANKS.get(mob['rank'], 'normal')
    return lvl + (power['elite_levels'] if rank == 'elite' else 0) + (99 if rank == 'boss' else 0), lvl, rank


def scaled_level(lvl, power):
    """The effective BG3 level of a WoW level (Scaling.lua M.Effective, the balance research's table)."""
    return min(18, 1 + max(0, lvl - 4) * (power['level_cap'] - 1) / 56)


def dc(lvl, rank, power):
    """A save the party can make: BG3's own curve at the effective level (Scaling.lua), not the stand-in's stats.
    With the switch `scaling` on, the stand-in's SpellSaveDC boost says the same and Abilities.lua uses that."""
    v = round(8.4 + 0.63 * scaled_level(lvl, power))
    return v + (power['dc_elite'] if rank == 'elite' else power['dc_boss'] if rank == 'boss' else 0)


def is_area(b):
    return b.startswith(('Zone_', 'Shout_')) or b in ('Projectile_Fireball', 'Projectile_Bomb', 'Target_FlameStrike')


def dice(wow_damage, lvl, power, every='turn', rank='normal', b=''):
    """WoW's damage at the ratio of a member's BG3 HP to a WoW hero's HP, cut to the normal hit of the balance
    research (20% of a member's HP at the effective level) if it can fire every turn, more for what it can't,
    less for what hits several; as the nearest plain dice."""
    hero = curve(power['hero_hp'], scaled_level(lvl, power))
    want = wow_damage * hero / curve(power['wow_hp'], lvl)
    cap = power['turn_share'] * hero * power['every_cap'][every] * power['rank_cap'][rank]
    if is_area(b):
        cap *= power['area_cap']
    want = max(1.0, min(want, cap))
    best = None
    for n in range(1, 11):
        for size, mean in DICE:
            d = abs(n * mean - want) + 0.15 * n   # few dice beat exact ones
            if best is None or d < best[0]:
                best = (d, '%dd%d' % (n, size), n * mean)
    return best[1], best[2]


def recharge(repeat_ms, kind, hard, lvl, wanted):
    """How often BG3 lets it fire. A fight is rounds of 6 s: WoW's cooldowns over one are rounds here."""
    if hard and lvl < 20:
        return 'fight'
    if lvl <= 5:              # a level 1 party: nothing but its basic attack every turn
        return wanted if wanted == 'fight' else 'fight' if repeat_ms and repeat_ms > 25000 else 'recharge'
    if wanted:
        return wanted
    if repeat_ms and repeat_ms > 25000:
        return 'fight'
    if repeat_ms and repeat_ms > 9000:
        return 'recharge'
    return 'turn'


def when_ok(when, mob):
    if not when:
        return True
    if 'word' in when and not re.search(when['word'], mob['name'], re.I):
        return False
    if 'family' in when and mob['pet_family'] != when['family']:
        return False
    if 'type' in when and mob['type'] != when['type']:
        return False
    if 'rank' in when and mob['rank'] < when['rank']:
        return False
    return True


def rung(tiers, lvl):
    got = None
    for t in tiers:
        if lvl >= t[0]:
            got = t
    return got


def resolve(sp, tag, mob, rules, lvl):
    """(BG3 spell, options, source, why): picks by the spell's name first, a mob-specific pick before a general
    one; then the meaning ladder. Below a hard control's floor, the soft version."""
    power = rules['power']
    name = sp['name']
    if name in rules['skip']['names'] or tag in rules['skip']['tags']:
        return None, {}, 'skip', None
    hard_floor = power['hard_tags'].get(tag)
    picks = sorted((p for p in rules['picks'] if name in p['w'] and when_ok(p.get('when'), mob)), key=lambda p: not p.get('when'))
    for p in picks:
        r = rung(p['tiers'], lvl)
        if r:
            return r[1], dict(r[2]) if len(r) > 2 else {}, 'star' if p.get('star') else 'pick', p.get('why')
    if picks and hard_floor is None:
        return None, {}, 'too_strong', picks[0]['tiers'][0][0]
    if hard_floor is not None and lvl < hard_floor:
        soft = power['hard_soft'][tag]
        r = rung(rules['ladders'][soft], lvl)
        if r:
            return r[1], dict(r[2]) if len(r) > 2 else {}, 'soft', 'Softened: at level %d a real %s would be a lost turn with no way back; it lands from level %d.' % (lvl, tag.replace('_area', ''), hard_floor)
        return None, {}, 'too_strong', hard_floor
    ladder = rules['ladders'].get(tag)
    if ladder is None:
        return None, {}, 'unmapped', None
    r = rung(ladder, lvl)
    if r:
        return r[1], dict(r[2]) if len(r) > 2 else {}, 'rule', None
    return None, {}, 'too_strong', ladder[0][0] if ladder else None


def mob_casts(mob):
    """Every WoW spell of a mob: its spell list, its EventAI, its spawn aura; (spell, how often, condition)."""
    out = {}
    for s in mob['list']:
        id_ = s['spellId']
        rep = max(s['delayRepeatMin'], s['delayRepeatMax']) * 1000
        out.setdefault(id_, {'repeat': rep, 'cond': None, 'when': 'list'})
    for e in mob['events']:
        id_ = e['spell']
        ev = e['event']
        c = {'repeat': 0, 'cond': None, 'when': 'ai'}
        if ev in (0, 1):
            c['repeat'] = max(e['p'][2], e['p'][3])
        elif ev == 2:
            c['cond'] = 'HasHPPercentageLessThan(%d)' % e['p'][0] if e['p'][0] else None
            c['when'] = 'hp'
        elif ev == 4:
            c['when'] = 'aggro'
        elif ev == 6:
            c['when'] = 'death'
        elif ev == 11:
            c['when'] = 'spawn'
        elif ev == 12:
            c['tcond'] = 'HasHPPercentageLessThan(%d)' % e['p'][0] if e['p'][0] else None
            c['when'] = 'target_hp'
        elif ev == 13:
            c['when'] = 'target_casting'
        elif ev == 14:
            c['when'] = 'friend_hurt'
        prev = out.get(id_)
        if not prev or (prev['when'] == 'list' and c['cond']):
            out[id_] = c
    if mob.get('auras'):
        for a in re.findall(r'\d+', str(mob['auras'])):
            out.setdefault(int(a), {'repeat': 0, 'cond': None, 'when': 'aura'})
    if mob.get('spawn_spell_id'):
        out.setdefault(mob['spawn_spell_id'], {'repeat': 0, 'cond': None, 'when': 'spawn'})
    return out


def map_mob(mob, spells, rules):
    power = rules['power']
    eff, lvl, rank = effective_level(mob, power)
    rows, seen = [], set()
    for id_, how in mob_casts(mob).items():
        sp = spells.get(id_) or spells.get(str(id_))
        if not sp:
            rows.append({'w': id_, 'wn': '#%d' % id_, 'tag': 'missing', 'src': 'unmapped', 'when': how['when']})
            continue
        tag = tag_spell(sp, spells)
        b, opt, src, why = resolve(sp, tag, mob, rules, eff)
        row = {'w': id_, 'wn': sp['name'], 'tag': tag, 'src': src, 'when': how['when']}
        if how['when'] == 'death':
            row.update(src='later', why='WoW casts this as it dies; BG3 has death bursts, a later step.', b=None)
            rows.append(row)
            continue
        if b in power['off_spells']:
            row.update(src='later', why='Summons and reactions wait: no WoW model for what they bring.', b=None)
            rows.append(row)
            continue
        if not b:
            row['floor'] = why if src == 'too_strong' else None
            row['b'] = None
            rows.append(row)
            continue
        ours = mob['rank'] > 0
        hard = b in power['hard_spells'] or bool(opt.get('rider') and HARD_STATUS.search(opt['rider'][0]))
        row.update(b=b, why=why, hard=hard, dc=dc(lvl, rank, power), every=recharge(how.get('repeat'), tag, hard, eff, opt.get('every')))
        for k in ('rider', 'type', 'doubt', 'weapon'):
            if opt.get(k):
                row[k] = opt[k]
        if opt.get('cond') or how.get('cond'):
            row['cond'] = opt.get('cond') or how.get('cond')
        if how.get('tcond'):
            row['tcond'] = how['tcond']
        if opt.get('dmg'):
            dmg = spell_damage(sp)
            if dmg > 0:
                row['wd'] = round(dmg)
                row['dice'], row['avg'] = dice(dmg, lvl, power, row['every'], rank, b)
        row['cap'] = dice(1e6, lvl, power, row['every'], rank, b)[0]
        if opt.get('weapon') and mob['type'] not in ARMED_TYPES:
            row['doubt'] = row.get('doubt') or 'a weapon action on a stand-in with no weapon: the hit falls back to its unarmed damage'
        rows.append(row)
    # At most so many abilities, the memorable first, one hard control; one per BG3 spell.
    order = {'star': 0, 'pick': 1, 'rule': 2, 'soft': 3}
    chosen, hard_used = [], False
    limit = power['abilities_low'] if eff <= 5 else power['abilities'][rank]
    for row in sorted((r for r in rows if r.get('b')), key=lambda r: order.get(r['src'], 9)):
        if row['b'] in seen:
            row['dropped'] = 'same BG3 ability as another of its spells'
            continue
        if row.get('hard') and hard_used and power['only_one_hard']:
            row['dropped'] = 'one hard control per mob'
            continue
        if len(chosen) >= limit:
            row['dropped'] = 'over the %d-ability limit for a %s mob' % (limit, rank)
            continue
        seen.add(row['b'])
        hard_used = hard_used or bool(row.get('hard'))
        chosen.append(row)
    return rows, chosen


# ---- the review page's data
def zone_of(mob, dbcs):
    """The zone a mob stands in most, from its spawns: a map rectangle, or the instance's name."""
    c = Counter()
    for m, x, y in mob['places']:
        hit = None
        for z in dbcs['zones']:
            # WorldMapArea: left, right, top, bottom in world coordinates. The rectangles overlap, so
            # the one whose centre is nearest for its size.
            if z[0] == m and z[3] <= y <= z[2] and z[5] <= x <= z[4]:
                d = ((y - (z[2] + z[3]) / 2) / (z[2] - z[3])) ** 2 + ((x - (z[4] + z[5]) / 2) / (z[4] - z[5])) ** 2
                if hit is None or d < hit[1]:
                    hit = (z[1], d)
        c[hit[0] if hit else dbcs['maps'].get(str(m), dbcs['maps'].get(m, 'map %s' % m))] += 1
    return c.most_common(1)[0][0] if c else '?'


def can_fight(mob, dbcs):
    """Not a friend to both player factions (a mob a player can meet in a fight)."""
    f = dbcs['faction'].get(str(mob['faction'])) or dbcs['faction'].get(mob['faction'])
    if not f:
        return True
    our, friend, hostile = f
    return bool(hostile & 7) or not (friend & 6)


def review(world, dbcs, rules, names, per_band=6):
    spells = {int(k): v for k, v in world['spells'].items()}
    zones = defaultdict(list)
    totals = Counter()
    every = []
    summary = {}
    for m in world['mobs'].values():
        if not m['spawns'] or m['type'] == 8 or not can_fight(m, dbcs):
            continue
        totals['mobs'] += 1
        rows, chosen = map_mob(m, spells, rules)
        if not [r for r in rows if r['src'] != 'skip']:
            totals['no_spell'] += 1
            continue
        totals['has_spell'] += 1
        totals['mapped'] += 1 if chosen else 0
        totals['script_only'] += 1 if not chosen and m.get('script_name') else 0
        totals['doubtful'] += 1 if any(r.get('doubt') for r in chosen) else 0
        for r in rows:
            totals['src_' + r['src']] += 1
        eff, lvl, rank = effective_level(m, rules['power'])
        z = zone_of(m, dbcs)
        for r in rows:
            if r['src'] == 'skip':
                continue
            b = summary.setdefault(r['wn'], {'wn': r['wn'], 'tag': r['tag'], 'n': 0, 'to': Counter(), 'src': r['src'], 'why': r.get('why') if isinstance(r.get('why'), str) else None, 'doubt': r.get('doubt'), 'floor': None})
            b['n'] += 1
            key = names.get(r['b'], [r['b']])[0] if r.get('b') else '—'
            b['to'][key] += 1
            if r.get('floor') and isinstance(r['floor'], int):
                b['floor'] = r['floor']
            if r['src'] in ('star', 'pick') and b['src'] not in ('star', 'pick'):
                b['src'], b['why'] = r['src'], r.get('why')
        rec = {'e': m['entry'], 'n': m['name'], 'sub': m['subname'], 'l': lvl, 'r': rank, 'ty': TYPES.get(m['type'], ''), 'fam': FAMILIES.get(m['pet_family'], ''),
               'sp': m['spawns'], 'z': z, 'script': m.get('script_name') or '',
               'a': [{k: v for k, v in r.items()} for r in rows if r['src'] != 'skip']}
        for x in rec['a']:
            if x.get('b'):
                x['bn'] = names.get(x['b'], [x['b']])[0]
        zones[z].append(rec)
        every.append(rec)
    sample = []
    for z, mobs in zones.items():
        bands = defaultdict(list)
        for m in mobs:
            bands[m['l'] // 5].append(m)
        for band, ms in sorted(bands.items()):
            ms.sort(key=lambda m: (-sum(1 for a in m['a'] if a['src'] == 'star'), -m['sp']))
            sample += ms[:per_band]
    return {'totals': totals, 'sample': sample, 'zones': sorted(zones, key=lambda z: sorted(m['l'] for m in zones[z])[len(zones[z]) // 2]), 'all_mobs': len(every),
            'spells': sorted(({**b, 'to': b['to'].most_common(4)} for b in summary.values()), key=lambda b: -b['n'])}


def export(out, db_cmd, data_dir):
    best = {}
    for v in db_query(db_cmd, CREATURE, 'creature_template'):
        if v['entry'] not in best or v['patch'] > best[v['entry']]['patch']:
            best[v['entry']] = v
    spawns = Counter()
    places = defaultdict(list)
    for v in db_query(db_cmd, ['id', 'map', 'position_x', 'position_y'], 'creature'):
        spawns[v['id']] += 1
        if len(places[v['id']]) < 30:
            places[v['id']].append([v['map'], round(v['position_x']), round(v['position_y'])])
    lists = {}
    for v in db_query(db_cmd, ['entry'] + ['%s_%d' % (c, i) for i in range(1, 9) for c in LIST_SLOT], 'creature_spells'):
        lists[v['entry']] = [{c: v['%s_%d' % (c, i)] for c in LIST_SLOT} for i in range(1, 9) if v['spellId_%d' % i]]
    casts = defaultdict(list)
    for v in db_query(db_cmd, ['id', 'datalong', 'target_type', 'command'], 'creature_ai_scripts'):
        if v['command'] == 15:
            casts[v['id']].append(v['datalong'])
    events = defaultdict(list)
    for v in db_query(db_cmd, EVENTS, 'creature_ai_events'):
        for a in ('action1_script', 'action2_script', 'action3_script'):
            for s in casts.get(v[a], ()):
                events[v['creature_id']].append({'spell': s, 'event': v['event_type'], 'chance': v['event_chance'],
                                                 'p': [v['event_param%d' % i] for i in range(1, 5)]})
    spells = {}
    for v in db_query(db_cmd, SPELL, 'spell_template'):
        if v['entry'] not in spells or v['build'] > spells[v['entry']]['build']:
            spells[v['entry']] = v
    mobs = {}
    for e, t in best.items():
        n = dict(t, spawns=spawns[e], places=places.get(e, []), list=lists.get(t['spell_list_id'], []), events=events.get(e, []))
        n['rank'] = n.pop('`rank`', n.get('rank', 0))
        mobs[e] = n
    tmp = str(out) + '.tmp'
    with open(tmp, 'w') as f:
        json.dump({'mobs': mobs, 'spells': spells, 'dbc': load_dbcs(data_dir)}, f, separators=(',', ':'))
    os.replace(tmp, out)
    print('%d creatures, %d spawned, %d spells -> %s' % (len(mobs), sum(1 for m in mobs.values() if m['spawns']), len(spells), out))


# ---- BG3's side: a copy of its spell, as stats text (bg3le compiles functor text only through SetRawAttribute,
# and reads it back compiled, so the text is made here from BG3's own stats files)
BG3_PAKS = (('Shared.pak', ('Shared', 'SharedDev')), ('Gustav.pak', ('Gustav', 'GustavDev', 'Honour')),
            ('GustavX.pak', ('GustavX', 'HonourX')))
MAIN_COSTS = ('ActionPoint', 'BonusActionPoint', 'ReactionActionPoint')
WEAPON_DAMAGE = re.compile(r'(Main|Offhand)\w*Weapon')
DC_TOKEN = '@DC@'  # the fixed DC, or SourceSpellDC() in the stats file for `scaling`, whose stand-ins carry the boost
PREFIX = 'WOWBG3_AB_'
# The game's file the copies travel in: small, in the last module loaded (Abilities.lua overrides it).
CARRIER_PAK, CARRIER = 'GustavX.pak', 'Public/GustavX/Stats/Generated/Data/Spell_Wall.txt'
SCHEMA = 3  # wowbg3-coord.py makes the table again below this


def load_bg3(path):
    """Every spell's fields with `using` merged: from BG3's Data folder, or a JSON made earlier."""
    if str(path).endswith('.json'):
        return json.load(open(path))
    sys.path.insert(0, str(HERE))
    from bg3pak import Pak
    entries = {}
    for pak, mods in BG3_PAKS:
        f = Path(path) / pak
        if not f.is_file():
            continue
        p = Pak(str(f))
        for mod in mods:
            prefix = 'Public/%s/Stats/Generated/Data/Spell_' % mod
            for name in sorted(n for n in p.files if n.startswith(prefix) and n.endswith('.txt')):
                cur = None
                for line in p.read(name).decode(errors='replace').split('\n'):
                    m = re.match(r'new entry "([^"]+)"', line)
                    if m:
                        cur = entries.setdefault(m.group(1), {'using': None, 'data': {}})
                        continue
                    if cur is None:
                        continue
                    m = re.match(r'using "([^"]+)"', line)
                    if m:
                        cur['using'] = m.group(1)
                        continue
                    m = re.match(r'data "([^"]+)" "(.*)"', line.rstrip('\r'))
                    if m:
                        cur['data'][m.group(1)] = m.group(2)

    def fields(n, depth=0):
        e = entries.get(n)
        if not e or depth > 20:
            return {}
        out = fields(e['using'], depth + 1) if e['using'] else {}
        out.update(e['data'])
        return out
    return {n: fields(n) for n in entries}


def split_top(text, sep):
    """Split on sep outside brackets."""
    out, cur, depth = [], [], 0
    for ch in text:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == sep and depth == 0:
            out.append(''.join(cur))
            cur = []
        else:
            cur.append(ch)
    out.append(''.join(cur))
    return out


def map_calls(text, name, fn):
    """Rewrite the arguments of every call of name; fn gets and returns the argument list."""
    out, pos, key = [], 0, name + '('
    while True:
        a = text.find(key, pos)
        if a < 0:
            return ''.join(out) + text[pos:]
        i, depth = a + len(key), 1
        while i < len(text) and depth:
            depth += {'(': 1, ')': -1}.get(text[i], 0)
            i += 1
        out += [text[pos:a], key, ','.join(fn(split_top(text[a + len(key):i - 1], ','))), ')']
        pos = i


def set_damage(text, amount, dtype):
    """The first DealDamage of a field does amount (half stays half), the others of the field go: a stand-in's
    damage is one number. Weapon damage stays weapon damage unless the row has dice of its own."""
    kept, first = [], True
    for seg in split_top(text, ';'):
        if 'DealDamage(' not in seg:
            kept.append(seg)
            continue
        if not first:
            continue
        first = False

        def one(args):
            old = args[0].strip()
            if amount:
                args[0] = amount + '/2' if old.endswith('/2') else amount
            if dtype and len(args) > 1:
                args[1] = dtype
            return args
        kept.append(map_calls(seg, 'DealDamage', one))
    return ';'.join(kept)


def set_saves(text):
    def one(args):
        if len(args) > 1:
            args[1] = DC_TOKEN
        return args
    return map_calls(text, 'SavingThrow', one)


def copy_fields(base, row):
    """The stats text a copy of BG3 spell base needs for one table row. Only what differs."""
    f = {}
    costs = [c for c in (base.get('UseCosts') or '').split(';') if c.split(':')[0] in MAIN_COSTS + ('Movement',)]
    if not any(c.split(':')[0] in MAIN_COSTS for c in costs):
        costs.insert(0, 'ActionPoint:1')
    f['UseCosts'] = ';'.join(costs)
    for k in ('Requirements', 'RequirementConditions', 'ContainerSpells', 'SpellContainerID'):
        if base.get(k):
            f[k] = ''
    if row.get('cond'):
        f['RequirementConditions'] = row['cond']
    if base.get('Level') not in (None, '0'):
        f['Level'] = '0'
    if base.get('MemoryCost') not in (None, '0'):
        f['MemoryCost'] = '0'
    if 'IsLinkedSpellContainer' in (base.get('SpellFlags') or ''):
        f['SpellFlags'] = ';'.join(x for x in base['SpellFlags'].split(';') if x != 'IsLinkedSpellContainer')
    f['Cooldown'] = 'OncePerCombat' if row['every'] == 'fight' else 'None'
    if row['every'] == 'recharge':
        f['RechargeValues'] = '5-6'
    elif base.get('RechargeValues'):
        f['RechargeValues'] = ''
    # Damage: the row's dice, or the level's cap for a BG3 spell's own fixed or level-mapped numbers.
    text = {k: base.get(k) or '' for k in ('SpellRoll', 'SpellSuccess', 'SpellFail', 'SpellProperties', 'TooltipDamageList', 'TargetConditions')}
    for k in ('SpellSuccess', 'SpellFail', 'SpellProperties', 'TooltipDamageList'):
        if 'DealDamage(' in text[k]:
            weapon = WEAPON_DAMAGE.search(text[k])
            amount = row.get('dice') or (None if weapon else row['cap'])
            text[k] = set_damage(text[k], amount, row.get('type'))
    if row.get('dice') and int(base.get('ProjectileCount') or 1) > 1:
        f['ProjectileCount'] = '1'
    if row.get('type'):
        f['DamageType'] = row['type']
    for k in ('SpellRoll', 'SpellSuccess', 'SpellFail', 'SpellProperties'):
        text[k] = set_saves(text[k])
    if row.get('rider'):
        status, turns, save = row['rider']
        apply = 'ApplyStatus(%s,100,%d)' % (status, turns)
        if save:
            apply = 'IF(not SavingThrow(Ability.%s,%s)):%s' % (save, DC_TOKEN, apply)
        k = 'SpellSuccess' if text['SpellRoll'].strip() else 'SpellProperties'
        text[k] = (text[k].rstrip(';') + ';' if text[k].strip() else '') + apply
    if row.get('tcond'):
        tc = text['TargetConditions'].strip()
        text['TargetConditions'] = '(%s) and %s' % (tc, row['tcond']) if tc else row['tcond']
    for k, v in text.items():
        if v != (base.get(k) or ''):
            f[k] = v
    return f


def table(world, rules, bg3):
    """The mod's table: Spells, the copies by a short stable name, with their stats text; Mobs, per creature entry,
    the copies it gets. Only what the mod needs; no zones, no texts but the WoW spell's name for the log."""
    spells = {int(k): v for k, v in world['spells'].items()}
    copies, mobs, names, missing = {}, {}, {}, Counter()
    for e, m in world['mobs'].items():
        if not m['spawns']:
            continue
        _, chosen = map_mob(m, spells, rules)
        got = []
        for r in chosen:
            base = bg3.get(r['b'])
            if base is None:
                missing[r['b']] += 1
                continue
            fields = copy_fields(base, r)
            # BG3's text under WoW's name: a string of ours per name.
            name = re.sub(r'\s+Proc$', '', r['wn'])
            handle = 'WowBg3_A' + hashlib.sha1(name.encode()).hexdigest()[:12]
            names[handle] = name
            fields['DisplayName'] = handle + ';1'
            # The DC names a copy only if its text uses it.
            dc_used = r['dc'] if DC_TOKEN in json.dumps(fields) else None
            key = hashlib.sha1(json.dumps([r['b'], dc_used, fields], sort_keys=True).encode()).hexdigest()[:12]
            copies.setdefault(key, {'B': r['b'], 'Dc': r['dc'], 'Wn': r['wn'], 'St': base.get('SpellType') or '', 'F': fields})
            got.append(key)
        if got:
            mobs[e] = got
    for b, n in missing.most_common():
        print('not in BG3 data: %s (%d mobs)' % (b, n), file=sys.stderr)
    return {'Schema': SCHEMA, 'Spells': copies, 'Mobs': mobs, 'Names': names}


def stats_text(copies, carrier, dc=None):
    """The carrier file's own text, then every copy as BG3's loader reads it. dc None: each copy's fixed DC."""
    out = [carrier.rstrip('\r\n') + '\r\n\r\n'] if carrier else []
    for key, c in sorted(copies.items()):
        out.append('new entry "%s%s"\r\ntype "SpellData"\r\ndata "SpellType" "%s"\r\nusing "%s"\r\n' % (PREFIX, key, c['St'], c['B']))
        for k, v in sorted(c['F'].items()):
            out.append('data "%s" "%s"\r\n' % (k, v.replace(DC_TOKEN, dc or str(c['Dc']))))
        out.append('\r\n')
    return ''.join(out)


def carrier_text(bg3_path):
    if str(bg3_path).endswith('.json'):
        return ''  # a test, on an extract
    sys.path.insert(0, str(HERE))
    from bg3pak import Pak
    return Pak(str(Path(bg3_path) / CARRIER_PAK)).read(CARRIER).decode(errors='replace')


def write_table(world, rules, bg3, out, carrier=''):
    """abilities.json last: it says the stats files beside it are there."""
    t = table(world, rules, bg3)
    d = Path(out).parent
    for name, dc in (('abilities-stats.txt', None), ('abilities-stats-scaled.txt', 'SourceSpellDC()')):
        (d / (name + '.tmp')).write_bytes(stats_text(t['Spells'], carrier, dc).encode())
        os.replace(d / (name + '.tmp'), d / name)
    t['Spells'] = {k: {'B': c['B'], 'Wn': c['Wn']} for k, c in t['Spells'].items()}
    tmp = str(out) + '.tmp'
    with open(tmp, 'w') as f:
        json.dump(t, f, separators=(',', ':'))
    os.replace(tmp, out)
    print('%d mobs with abilities, %d spell copies, %d names -> %s' % (len(t['Mobs']), len(t['Spells']), len(t['Names']), out))


def seed(world, out):
    """What table() reads of a world file: fightable mobs with something to cast and the spells they can reach."""
    sp = world['spells']
    mobs = {e: {k: v for k, v in m.items() if k not in ('places', 'subname')} for e, m in world['mobs'].items()
            if m['spawns'] and (m['list'] or m['events'] or m['auras'] or m['spawn_spell_id'])}
    todo = []
    for m in mobs.values():
        todo += [x['spellId'] for x in m['list']] + [x['spell'] for x in m['events']] + [m['spawn_spell_id']]
        todo += re.findall(r'\d+', str(m['auras'] or ''))
    seen = set()
    while todo:
        s = str(todo.pop())
        if s in seen or s not in sp:
            continue
        seen.add(s)
        todo += [sp[s].get('effectTriggerSpell%d' % i) or 0 for i in (1, 2, 3)]
    body = {'mobs': mobs, 'spells': {s: {k: v for k, v in sp[s].items() if k not in ('description', 'nameSubtext')} for s in seen}}
    with gzip.open(out, 'wt') as f:
        json.dump(body, f, separators=(',', ':'))
    print('%d mobs, %d spells -> %s' % (len(mobs), len(seen), out))


def main():
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('cmd', choices=('make', 'export', 'seed', 'build', 'show', 'stats'))
    a.add_argument('world')
    a.add_argument('mob', nargs='?')
    a.add_argument('--db', help='database command reading SQL on stdin (default: the server wowserver.py finds)')
    a.add_argument('--data', help="the client's Data folder (export)")
    a.add_argument('--out', help='where build writes')
    a.add_argument('--rules', default=str(HERE / 'wow_abilities_rules.json'))
    a.add_argument('--bg3', help="BG3's Data folder, or a JSON of its spells (build, make)")
    a.add_argument('--seed', help='a seed from `seed` (make: no database needed)')
    args = a.parse_args()
    db = ['sh', '-c', args.db] if args.db else None
    if args.cmd == 'make':
        # First run: from the release's seed, or from the database with the world file next to the table.
        if not (args.bg3 and (args.seed or args.data)):
            sys.exit('make needs --bg3 and --seed or --data')
        if args.seed:
            world = json.load(gzip.open(args.seed, 'rt'))
        else:
            world_file = str(args.world) + '.world'
            export(world_file, db, args.data)
            world = json.load(open(world_file))
        return write_table(world, json.load(open(args.rules)), load_bg3(args.bg3), args.world, carrier_text(args.bg3))
    if args.cmd == 'export':
        if not args.data:
            sys.exit('export needs --data')
        return export(args.world, db, args.data)
    world = json.load(open(args.world))
    if args.cmd == 'seed':
        return seed(world, args.mob)
    rules = json.load(open(args.rules))
    spells = {int(k): v for k, v in world['spells'].items()}
    if args.cmd == 'build':
        if not (args.out and args.bg3):
            sys.exit('build needs --out and --bg3')
        write_table(world, rules, load_bg3(args.bg3), args.out, carrier_text(args.bg3))
    elif args.cmd == 'show':
        for e, m in world['mobs'].items():
            if args.mob in (e, m['name']):
                rows, chosen = map_mob(m, spells, rules)
                print('%s (%s) level %d-%d' % (m['name'], e, m['level_min'], m['level_max']))
                for r in rows:
                    print('  ', 'USED' if r in chosen else '    ', r['wn'], '->', r.get('b'), {k: v for k, v in r.items() if k not in ('wn', 'b', 'w', 'why')})
    else:
        c = Counter()
        unmapped = Counter()
        for m in world['mobs'].values():
            if m['spawns']:
                rows, chosen = map_mob(m, spells, rules)
                c['mobs'] += 1
                c['with_spells'] += 1 if rows else 0
                c['mapped'] += 1 if chosen else 0
                for r in rows:
                    c[r['src']] += 1
                    if r['src'] == 'unmapped':
                        unmapped[r['wn']] += 1
        print(dict(c))
        print(unmapped.most_common(40))


if __name__ == '__main__':
    main()
