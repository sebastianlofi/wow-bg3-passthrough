#!/usr/bin/env python3
"""wow_items.py: every WoW item as a BG3 item.

  wow_items.py build [--out FILE] [--rules FILE] [--data WOW_DATA] [--db CMD] [--effects]
      writes the item table (default: the harness IO dir's link/items.json)
  wow_items.py effects                 the rule table with its counts, as markdown (docs/item-effects.md)
  wow_items.py show ENTRY [ENTRY...]   one item's row, for tuning the rules
  wow_items.py stats                   how many items map to each BG3 base

Sources: the server's item_template (WoW names, stats, prices; the highest patch row per entry)
through CMD (default: the private database container, read-only select), and the player's
ItemDisplayInfo.dbc (icon names) read from the WoW Data folder's MPQs (wow_mpq.py). The rules
(which BG3 base item, how stats become BG3 numbers) are in wow_items_rules.json next to this file;
the inventory notes explains them. The table holds no Blizzard art, only names and numbers the
player's own server and client already have.

Row (JSON, keys short because the BG3 side loads all ~17,700 at once):
  {"e": entry, "n": name, "q": quality, "k": kind (weapon|armor|worn|object), "b": BG3 stats
   entry used as base, "t": WoW inventory type, "s": WoW slot name, "i": icon name, "st": stack size,
   "bp": buy price (copper), "sp": sell price, "il": item level, "rl": required level,
   "r": BG3 rarity, "mg": armour magic bonus (+AC), "d": damage die, "dt": damage type, "en": weapon +N, "x": extra damage,
   "ac": armour class, "bo": BG3 boosts (Ability, Resistance, AC), "w": weight kg,
   "wd": WoW text for the description (damage, armour, stats as WoW shows them)}

The effects switch (--effects, WOW_BG3_ITEM_EFFECTS=1 or "enabled" in the rules' "effects", on by default;
off, the table is what it was without it): rows gain "ef" (on-equip points by kind) and "rp" (random
property pool), "wd" gains WoW's Equip/Use lines, an instant healing consumable picks its BG3 potion by
WoW's amount, and the table gains Effects (how points become boosts), Props (the random suffixes) and
Pools. Inventory.lua sums the points of what a member wears and adds the boosts to the member.
"""
import argparse, json, math, os, re, struct, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IO = ROOT / "bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3"
DATA = ROOT / 'client/SoloCraft 1.12.1/Data'
DB_CMD = ['sudo', '-n', 'docker', 'exec', '-i', 'wow-bg3-database-1', 'sh', '-c',
          'mariadb -uroot -p"$MARIADB_ROOT_PASSWORD" --batch mangos']
COLS = ['entry', 'patch', 'class', 'subclass', 'name', 'display_id', 'quality', 'buy_price', 'sell_price',
        'inventory_type', 'item_level', 'required_level', 'stackable', 'container_slots', 'delay',
        'dmg_min1', 'dmg_max1', 'dmg_type1', 'dmg_min2', 'dmg_max2', 'dmg_type2', 'armor', 'block',
        'holy_res', 'fire_res', 'nature_res', 'frost_res', 'shadow_res', 'arcane_res', 'spellcategory_1', 'spellid_1'] + \
       ['stat_type%d' % i for i in range(1, 11)] + ['stat_value%d' % i for i in range(1, 11)]
STAT_NAMES = {0: 'Mana', 1: 'Health', 3: 'Agility', 4: 'Strength', 5: 'Intellect', 6: 'Spirit', 7: 'Stamina'}
DIES = [(4, 2.5), (6, 3.5), (8, 4.5), (10, 5.5), (12, 6.5)]
SPELL_COLS = ['random_property'] + ['%s_%d' % (c, i) for i in range(1, 6) for c in ('spellid', 'spelltrigger')]
SPELL_TEMPLATE = ['entry', 'build', 'name', 'description'] + \
    ['%s%d' % (c, i) for i in (1, 2, 3) for c in ('effect', 'effectApplyAuraName', 'effectBasePoints',
                                                  'effectDieSides', 'effectMiscValue')]
TRIGGER_USE, TRIGGER_EQUIP, TRIGGER_HIT = 0, 1, 2
TRIGGER_TEXT = {TRIGGER_USE: 'Use', TRIGGER_EQUIP: 'Equip', TRIGGER_HIT: 'Chance on hit'}
EFFECT_APPLY_AURA, EFFECT_HEAL = 6, 10
AURA_MOD_RESISTANCE, AURA_MOD_SKILL, AURA_MOD_STAT = 22, 30, 29
ENCH_RESISTANCE, ENCH_EQUIP_SPELL, ENCH_STAT = 4, 3, 5


def query(db_cmd, cols, table, text=('name',)):
    q = 'select %s from %s;' % (', '.join(cols), table)
    r = subprocess.run(db_cmd, input=q.encode(), capture_output=True)  # bytes: spell texts hold bare carriage returns
    if r.returncode:
        sys.exit('database: %s' % r.stderr.decode(errors='replace').strip())
    lines = r.stdout.decode(errors='replace').rstrip('\n').split('\n')
    head = lines[0].split('\t')
    for line in lines[1:]:
        v = dict(zip(head, line.split('\t')))
        for k in v:
            if k not in text:
                v[k] = float(v[k]) if '.' in v[k] else int(v[k])
        yield v


def load_items(db_cmd, cols=COLS):
    best = {}
    for v in query(db_cmd, cols, 'item_template'):
        e = v['entry']
        if e not in best or v['patch'] > best[e]['patch']:
            best[e] = v
    return best


def load_icons(data_dir):
    sys.path.insert(0, str(HERE))
    from wow_mpq import WowData
    raw = WowData(str(data_dir)).read('DBFilesClient\\ItemDisplayInfo.dbc')
    if raw is None:
        sys.exit('no ItemDisplayInfo.dbc in %s' % data_dir)
    _, n, f, rs, ss = struct.unpack('<4s4I', raw[:20])
    strs = raw[20 + n * rs:]
    icons = {}
    for r in range(n):
        v = struct.unpack_from('<%dI' % f, raw, 20 + r * rs)
        o = v[5]
        if 0 < o < ss:
            icons[v[0]] = strs[o:strs.index(b'\0', o)].decode('latin-1')
    return icons


def die_for(avg):
    for size, mean in DIES:
        if mean >= avg:
            return '1d%d' % size
    return '1d12'


def magic_bonus(rules, quality, ilvl):
    """+AC like BG3's own +1..+3 armours, from WoW quality and item level (rules 'magic')."""
    best = 0
    for m in rules['magic']:
        if quality >= m['quality'] and ilvl >= m['item_level']:
            best = max(best, m['bonus'])
    return min(best, rules.get('magic_cap', 3))


def body_base(b, ilvl):
    base = b['tiers'][0][1]
    for lvl, name in b['tiers']:
        if ilvl >= lvl:
            base = name
    return base


def ability_boosts(rules, it):
    sums = {}
    for i in range(1, 11):
        t, v = it['stat_type%d' % i], it['stat_value%d' % i]
        name = rules['abilities'].get(str(t))
        if name and v:
            sums[name] = sums.get(name, 0) + v
    out = []
    for name, v in sorted(sums.items()):
        n = int(v / rules['ability_per'])
        n = max(-rules['ability_cap'], min(rules['ability_cap'], n))
        if n:
            out.append('Ability(%s,%d)' % (name, n))
    for col, dtype in rules['resist'].items():
        if it[col] >= rules['resist_at']:
            out.append('Resistance(%s,Resistant)' % dtype)
    return out


def wow_text(it):
    parts = []
    if it['dmg_max1']:
        parts.append('%d - %d Damage, Speed %.2f' % (it['dmg_min1'], it['dmg_max1'], it['delay'] / 1000.0))
        if it['delay']:
            parts.append('(%.1f damage per second)' % ((it['dmg_min1'] + it['dmg_max1']) / 2 / (it['delay'] / 1000.0)))
    if it['dmg_max2']:
        names = {1: 'Holy', 2: 'Fire', 3: 'Nature', 4: 'Frost', 5: 'Shadow', 6: 'Arcane'}
        parts.append('+%d - %d %s Damage' % (it['dmg_min2'], it['dmg_max2'], names.get(it['dmg_type2'], '')))
    if it['armor']:
        parts.append('%d Armor' % it['armor'])
    if it['block']:
        parts.append('%d Block' % it['block'])
    for i in range(1, 11):
        t, v = it['stat_type%d' % i], it['stat_value%d' % i]
        if v and t in STAT_NAMES:
            parts.append('%+d %s' % (v, STAT_NAMES[t]))
    for col, nm in [('holy_res', 'Holy'), ('fire_res', 'Fire'), ('nature_res', 'Nature'), ('frost_res', 'Frost'),
                    ('shadow_res', 'Shadow'), ('arcane_res', 'Arcane')]:
        if it[col]:
            parts.append('%+d %s Resistance' % (it[col], nm))
    if it['required_level'] > 1:
        parts.append('Requires Level %d' % it['required_level'])
    return '\n'.join(parts)


def map_item(rules, it, icons):
    cls, sub, inv = it['class'], it['subclass'], it['inventory_type']
    ilvl, q = it['item_level'], it['quality']
    row = {'e': it['entry'], 'n': it['name'], 'q': q, 't': inv, 'i': icons.get(it['display_id'], ''),
           'st': max(1, it['stackable']), 'bp': it['buy_price'], 'sp': it['sell_price'], 'il': ilvl,
           'rl': it['required_level'], 'r': rules['rarity_by_quality'].get(str(q), 'Common')}
    boosts = []
    if cls == 2 and str(sub) in rules['weapons']:
        w = rules['weapons'][str(sub)]
        base = w['base']
        if 'fast' in w and it['delay'] and it['delay'] <= w['fast_delay']:
            base = w['fast']
        row.update(k='weapon', b=base, s=w['name'])
        avg = (it['dmg_min1'] + it['dmg_max1']) / 2
        dps = avg / (it['delay'] / 1000.0) if it['delay'] else 0
        hand = 'two_hand' if inv == 17 else 'ranged' if inv in (15, 25, 26) else 'one_hand'
        en = sum(1 for th in rules['enchant_dps'][hand] if dps >= th)
        if en:
            row['en'] = en
            boosts.append('WeaponEnchantment(%d)' % en)
        dt = rules['damage_types'].get(str(it['dmg_type1']), '')
        if dt:
            row['dt'] = dt
        if it['dmg_max2'] and rules['damage_types'].get(str(it['dmg_type2'])):
            avg2 = (it['dmg_min2'] + it['dmg_max2']) / 2 * rules['extra_damage_scale']
            row['x'] = '%s,%s' % (die_for(avg2), rules['damage_types'][str(it['dmg_type2'])])
            boosts.append('WeaponDamage(%s,%s)' % (die_for(avg2), rules['damage_types'][str(it['dmg_type2'])]))
    elif str(inv) in rules['slots'] and (cls == 4 or inv in (2, 11, 16)):
        sl = rules['slots'][str(inv)]
        row.update(k='armor', s=sl['name'])
        if sl.get('body'):
            b = rules['body'].get(str(sub), rules['body']['1'])
            row['b'] = body_base(b, ilvl)
            mg = magic_bonus(rules, q, ilvl)
            row['ac'] = rules['base_ac'][row['b']] + mg
            if mg:
                row['mg'] = mg
        else:
            row['b'] = sl['base'].get(str(sub), sl['base']['any'])
            if sl.get('magic'):
                mg = magic_bonus(rules, q, ilvl)
                row['ac'] = rules['base_ac'][row['b']] + mg
                if mg:
                    row['mg'] = mg
            elif inv in (1, 8, 10, 16) and ilvl >= rules['small_ac']['item_level'] and q >= rules['small_ac']['quality']:
                boosts.append('AC(1)')
    elif cls in (2, 4) or str(inv) in rules['worn']['types']:
        row.update(k='worn', b=rules['worn']['base'], s=rules['worn']['types'].get(str(inv), 'Worn'))
    else:
        o = rules['objects'].get('%d:%d' % (cls, sub)) or rules['objects'].get(str(cls)) or rules['objects']['default']
        base = o['base']
        low = it['name'].lower()
        by_level = lambda tiers: next(v for top, v in tiers if ilvl <= top or top == 999)
        alcohol = 'drink' in o and it['spellid_1'] in rules['alcohol_spells']
        if 'heal' in o and any(w in low for w in rules.get('heal_words', [])):
            base = o['heal']
        elif alcohol:
            base = rules['alcohol']
        elif 'drink' in o and (it['spellcategory_1'] == rules.get('drink_category', 59)
                               or any(re.search(r'\b%s' % w, low) for w in rules['drink_words'])):
            base = by_level(rules['drink_heal']) if it['spellcategory_1'] == rules.get('drink_category', 59) else o['drink']
        elif 'food' in o and (it['spellcategory_1'] == rules.get('food_category', 11)
                              or any(re.search(r'\b%s' % w, low) for w in rules['food_words'])):
            base = by_level(rules['food_heal']) if it['spellcategory_1'] == rules.get('food_category', 11) else o['food']
        row.update(k='object', b=base, s=o['name'])
        # What WoW itself eats and drinks (not an elixir named for water). BG3 spends these at camp.
        if alcohol:
            row['sv'] = rules['alcohol_supply']
        elif it['spellcategory_1'] in (rules.get('food_category', 11), rules.get('drink_category', 59)):
            row['sv'] = rules['conjured_supply'] if 'conjured' in low else by_level(rules['camp_supply'])
    if row['k'] in ('weapon', 'armor', 'worn'):
        boosts = ability_boosts(rules, it) + boosts
    if boosts:
        row['bo'] = ';'.join(boosts)
    wd = wow_text(it)
    if wd:
        row['wd'] = wd
    return row


# ---- the effects switch: on-equip and on-use spells, random suffixes (rules "effects") ----

def load_spells(db_cmd):
    best = {}
    for v in query(db_cmd, SPELL_TEMPLATE, 'spell_template', text=('name', 'description')):
        e = v['entry']
        if e not in best or v['build'] > best[e]['build']:
            best[e] = v
    return best


def load_dbc(data_dir, name):
    """A DBC's rows as tuples of uint32, and its string reader."""
    sys.path.insert(0, str(HERE))
    from wow_mpq import WowData
    raw = WowData(str(data_dir)).read('DBFilesClient\\%s.dbc' % name)
    if raw is None:
        sys.exit('no %s.dbc in %s' % (name, data_dir))
    _, n, f, rs, ss = struct.unpack('<4s4I', raw[:20])
    strs = raw[20 + n * rs:]
    text = lambda o: strs[o:strs.index(b'\0', o)].decode('latin-1') if 0 < o < ss else ''
    return [struct.unpack_from('<%dI' % f, raw, 20 + r * rs) for r in range(n)], text


def spell_points(sp, n):
    """What effect n of a spell is worth: WoW's base points plus its die."""
    base, die = sp['effectBasePoints%d' % n], sp['effectDieSides%d' % n]
    return base + 1 if die <= 1 else base + (die + 1) / 2


def add_points(pts, kind, v):
    pts[kind] = round(pts.get(kind, 0) + v, 3)


def spell_kinds(fx, sp):
    """A spell's on-equip auras as {kind: points}; auras without a rule give nothing."""
    pts = {}
    for n in (1, 2, 3):
        if sp['effect%d' % n] != EFFECT_APPLY_AURA:
            continue
        aura, misc = sp['effectApplyAuraName%d' % n], sp['effectMiscValue%d' % n]
        v = spell_points(sp, n)
        if aura == AURA_MOD_STAT:
            for stat, name in fx['stats'].items():
                if misc in (-1, int(stat)):
                    add_points(pts, name, v)
        elif aura == AURA_MOD_RESISTANCE:
            for bit, r in fx['resist_bits'].items():
                if misc & int(bit):
                    add_points(pts, r['kind'], v * r.get('scale', 1))
        elif aura == AURA_MOD_SKILL:
            r = fx['skills'].get(str(misc))
            if r:
                add_points(pts, r['kind'], v * r.get('scale', 1))
        else:
            r = fx['auras'].get(str(aura))
            if r and (not r.get('school_mask') or misc & r['school_mask']):
                add_points(pts, r['kind'], v * r.get('scale', 1))
    return pts


def spell_text(sp):
    """WoW's own text for a spell; its name when the text has variables other than the amounts."""
    text = sp.get('description', '').replace('\\n', ' ').replace('\r', ' ').strip()
    for n in (1, 2, 3):
        if sp['effect%d' % n]:
            text = re.sub(r'\$[sS]%d\b' % n, '%d' % abs(spell_points(sp, n)), text)
    return sp['name'] if not text or '$' in text else text


def load_props(rules, db_cmd, data_dir, spells):
    """Random suffixes: ItemRandomProperties -> SpellItemEnchantment -> points (the DBCs of 1.12.1:
    property id, internal name, 5 enchantments, name; enchantment id, 3 types, 3 min, 3 max, 3 arguments,
    name). Returns (props, pools), pools being what the server's item_enchantment_template allows."""
    fx = rules['effects']
    irp, irp_text = load_dbc(data_dir, 'ItemRandomProperties')
    sie, sie_text = load_dbc(data_dir, 'SpellItemEnchantment')
    sie = {r[0]: r for r in sie}
    props = []
    for r in irp:
        pts, lines = {}, []
        for en in r[2:7]:
            s = sie.get(en)
            if not s:
                continue
            lines.append(sie_text(s[13]))
            for k in range(3):
                t, lo, arg = s[1 + k], s[4 + k], s[10 + k]
                if t == ENCH_EQUIP_SPELL and arg in spells:
                    for kind, v in spell_kinds(fx, spells[arg]).items():
                        add_points(pts, kind, v)
                elif t == ENCH_RESISTANCE:
                    bit = fx['resist_bits'].get(str(1 << arg))
                    if bit:
                        add_points(pts, bit['kind'], lo * bit.get('scale', 1))
                elif t == ENCH_STAT and str(arg) in rules['abilities']:
                    add_points(pts, rules['abilities'][str(arg)], lo)
        name = irp_text(r[7])
        if name:
            props.append({'id': r[0], 'n': name, 'ef': pts, 't': '\n'.join(l for l in lines if l)})
    pools = {}
    for v in query(db_cmd, ['entry', 'ench'], 'item_enchantment_template'):
        pools.setdefault(v['entry'], set()).add(v['ench'])
    return props, [{'id': e, 'p': sorted(p)} for e, p in sorted(pools.items())]


def effect_rules(rules):
    """How points become boosts (the table's Effects): the abilities by the rules for WoW's fixed stats."""
    fx = rules['effects']
    kinds = dict(fx['kinds'])
    for name in fx['stats'].values():
        kinds[name] = {'per': rules['ability_per'], 'cap': rules['ability_cap'], 'b': 'Ability(%s,{n})' % name}
    return kinds


def add_effects(rules, row, it, spells):
    """The switch's additions to a row made by map_item."""
    fx = rules['effects']
    pts, lines = {}, []
    for i in range(1, 6):
        sp, trigger = spells.get(it['spellid_%d' % i]), it['spelltrigger_%d' % i]
        if not sp or trigger not in TRIGGER_TEXT:
            continue
        lines.append('%s: %s' % (TRIGGER_TEXT[trigger], spell_text(sp)))
        if trigger == TRIGGER_EQUIP and row['k'] in ('weapon', 'armor', 'worn'):
            for kind, v in spell_kinds(fx, sp).items():
                add_points(pts, kind, v)
        if trigger == TRIGGER_USE and it['class'] == 0 and row['b'] in (rules['objects']['0']['base'], rules['objects']['0']['heal']):
            for n in (1, 2, 3):
                if sp['effect%d' % n] == EFFECT_HEAL:
                    for at, base in fx['heal_tiers']:
                        if spell_points(sp, n) >= at:
                            row['b'] = base
    if pts:
        row['ef'] = pts
    if it['random_property']:
        row['rp'] = it['random_property']
    if lines:
        row['wd'] = '\n'.join(([row['wd']] if row.get('wd') else []) + lines)


def effects_table(rules, items, spells, props, pools):
    """The rule table, counted from the data: markdown for docs/item-effects.md."""
    fx = rules['effects']
    notes = fx['table']
    out = []
    def section(title, keyed, label, rule):
        out.append('\n### %s\n\n| WoW effect | Count | Items | BG3 |\n|---|---:|---:|---|' % title)
        rest = [0, set()]
        for k, (n, ents, ex) in sorted(keyed.items(), key=lambda x: -x[1][0]):
            name = label(k)
            if name is None and n < fx['table_rest_below']:
                rest[0] += n
                rest[1] |= ents
                continue
            out.append('| %s | %d | %d | %s |' % (name or 'other: %s' % ex, n, len(ents), rule(k)))
        if rest[0]:
            out.append('| everything else | %d | %d | not mapped |' % (rest[0], len(rest[1])))
    def boost(kind):
        return 'pool `%s`: %s per %s, cap %s' % (kind, fx['kinds'][kind]['b'], fx['kinds'][kind]['per'], fx['kinds'][kind]['cap'])
    equip, use, hit = {}, {}, {}
    for it in items.values():
        for i in range(1, 6):
            sp, trigger = spells.get(it['spellid_%d' % i]), it['spelltrigger_%d' % i]
            if not sp:
                continue
            if trigger == TRIGGER_HIT:
                c = hit.setdefault('proc', [0, set(), it['name']])
                c[0] += 1
                c[1].add(it['entry'])
                continue
            into = equip if trigger == TRIGGER_EQUIP else use if trigger == TRIGGER_USE else None
            if into is None:
                continue
            for n in (1, 2, 3):
                e = sp['effect%d' % n]
                if not e:
                    continue
                k = '%d:%d' % (e, sp['effectApplyAuraName%d' % n])
                c = into.setdefault(k, [0, set(), '%s (%s)' % (sp['name'], it['name'])])
                c[0] += 1
                c[1].add(it['entry'])
    aura_of = lambda k: k.split(':')[1]
    def equip_rule(k):
        a = aura_of(k)
        if k.split(':')[0] != str(EFFECT_APPLY_AURA):
            return 'not mapped'
        if a in fx['auras']:
            return boost(fx['auras'][a]['kind']) + (', ' + notes['equip'][a] if a in notes['equip'] else '')
        return notes['equip'].get(a, 'not mapped')
    section('On-equip effects (spell trigger 1)', {k: tuple(v) for k, v in equip.items()},
            lambda k: notes['equip_names'].get(aura_of(k)) if k.split(':')[0] == str(EFFECT_APPLY_AURA) else None, equip_rule)
    section('Chance on hit (spell trigger 2)', {k: tuple(v) for k, v in hit.items()},
            lambda k: 'any proc', lambda k: notes['hit'])
    section('On-use effects (spell trigger 0)', {k: tuple(v) for k, v in use.items()},
            lambda k: notes['use_names'].get(k), lambda k: notes['use'].get(k, 'not mapped'))
    used = set()
    pool_of = {p['id']: p['p'] for p in pools}
    with_rp = [it for it in items.values() if it['random_property']]
    for it in with_rp:
        used |= set(pool_of.get(it['random_property'], ()))
    by_name = {}
    for p in props:
        if p['id'] in used:
            c = by_name.setdefault(p['n'], [0, set(), p['n']])
            c[0] += 1
            c[1].add(p['id'])
    out.append('\n### Random suffixes\n\n%d items can roll one, from %d pools, %d properties under %d names.\n' %
               (len(with_rp), len(pool_of), len(used), len(by_name)))
    out.append('| Suffix | Properties | BG3 pool kinds |\n|---|---:|---|')
    kinds = {}
    for p in props:
        if p['id'] in used:
            kinds.setdefault(p['n'], set()).update(p['ef'])
    for name, c in sorted(by_name.items(), key=lambda x: (-x[1][0], x[0])):
        out.append('| %s | %d | %s |' % (name, c[0], ', '.join(sorted(kinds[name])) or 'name and text only'))
    return '\n'.join(out)


def effects_on(args, rules):
    return bool(args.effects or os.environ.get('WOW_BG3_ITEM_EFFECTS') == '1' or rules['effects'].get('enabled'))


def build(args):
    rules = json.loads(Path(args.rules).read_text())
    effects = effects_on(args, rules)
    items = load_items(args.db, COLS + SPELL_COLS if effects else COLS)
    icons = load_icons(args.data)
    rows = [map_item(rules, it, icons) for _, it in sorted(items.items())]
    out = {'Schema': 1, 'Rules': os.path.basename(args.rules), 'CopperPerGold': rules['copper_per_bg3_gold'],
           'Count': len(rows), 'Items': rows}
    if effects:
        spells = load_spells(args.db)
        for row in rows:
            add_effects(rules, row, items[row['e']], spells)
        out['Props'], out['Pools'] = load_props(rules, args.db, args.data, spells)
        out['Effects'] = effect_rules(rules)
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(str(args.out) + '.tmp')
    tmp.write_text(json.dumps(out, separators=(',', ':'), ensure_ascii=False))
    tmp.replace(args.out)
    print('%d items -> %s (%d bytes); %d with an icon' % (len(rows), args.out, Path(args.out).stat().st_size,
                                                        sum(1 for r in rows if r['i'])))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['build', 'show', 'stats', 'effects'])
    ap.add_argument('entries', nargs='*', type=int)
    ap.add_argument('--out', default=str(IO / 'link/items.json'))
    ap.add_argument('--rules', default=str(HERE / 'wow_items_rules.json'))
    ap.add_argument('--data', default=str(DATA))
    ap.add_argument('--db', default=None, help='shell command that runs SQL from stdin on the mangos database')
    ap.add_argument('--effects', action='store_true', help='map on-equip and on-use effects and random suffixes')
    args = ap.parse_args()
    args.db = ['sh', '-c', args.db] if args.db else DB_CMD
    if args.cmd == 'build':
        build(args)
        return
    rules = json.loads(Path(args.rules).read_text())
    if args.cmd == 'effects':
        spells = load_spells(args.db)
        props, pools = load_props(rules, args.db, args.data, spells)
        print(effects_table(rules, load_items(args.db, COLS + SPELL_COLS), spells, props, pools))
        return
    effects = effects_on(args, rules)
    items = load_items(args.db, COLS + SPELL_COLS if effects else COLS)
    icons = load_icons(args.data)
    spells = load_spells(args.db) if effects else None
    if args.cmd == 'show':
        for e in args.entries:
            row = map_item(rules, items[e], icons)
            if effects:
                add_effects(rules, row, items[e], spells)
            print(json.dumps(row, indent=1, ensure_ascii=False))
    else:
        count = {}
        for it in items.values():
            r = map_item(rules, it, icons)
            count[(r['k'], r['b'])] = count.get((r['k'], r['b']), 0) + 1
        for (k, b), n in sorted(count.items(), key=lambda x: -x[1]):
            print('%6d  %-7s %s' % (n, k, b))


if __name__ == '__main__':
    main()
