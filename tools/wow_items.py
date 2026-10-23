#!/usr/bin/env python3
"""wow_items.py: every WoW item as a BG3 item.

  wow_items.py build [--out FILE] [--rules FILE] [--data WOW_DATA] [--db CMD]
      writes the item table (default: the harness IO dir's link/items.json)
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
        'holy_res', 'fire_res', 'nature_res', 'frost_res', 'shadow_res', 'arcane_res', 'spellcategory_1'] + \
       ['stat_type%d' % i for i in range(1, 11)] + ['stat_value%d' % i for i in range(1, 11)]
STAT_NAMES = {0: 'Mana', 1: 'Health', 3: 'Agility', 4: 'Strength', 5: 'Intellect', 6: 'Spirit', 7: 'Stamina'}
DIES = [(4, 2.5), (6, 3.5), (8, 4.5), (10, 5.5), (12, 6.5)]


def load_items(db_cmd):
    q = 'select %s from item_template;' % ', '.join(COLS)
    r = subprocess.run(db_cmd, input=q, capture_output=True, text=True)
    if r.returncode:
        sys.exit('database: %s' % r.stderr.strip())
    lines = r.stdout.splitlines()
    head = lines[0].split('\t')
    best = {}
    for line in lines[1:]:
        v = dict(zip(head, line.split('\t')))
        for k in v:
            if k != 'name':
                v[k] = float(v[k]) if '.' in v[k] else int(v[k])
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
        if 'heal' in o and any(w in low for w in rules.get('heal_words', [])):
            base = o['heal']
        elif 'drink' in o and (it['spellcategory_1'] == rules.get('drink_category', 59)
                               or any(re.search(r'\b%s' % w, low) for w in rules['drink_words'])):
            base = o['drink']
        elif 'food' in o and (it['spellcategory_1'] == rules.get('food_category', 11)
                              or any(re.search(r'\b%s' % w, low) for w in rules['food_words'])):
            base = o['food']
        row.update(k='object', b=base, s=o['name'])
    if row['k'] in ('weapon', 'armor', 'worn'):
        boosts = ability_boosts(rules, it) + boosts
    if boosts:
        row['bo'] = ';'.join(boosts)
    wd = wow_text(it)
    if wd:
        row['wd'] = wd
    return row


def build(args):
    rules = json.loads(Path(args.rules).read_text())
    items = load_items(args.db)
    icons = load_icons(args.data)
    rows = [map_item(rules, it, icons) for _, it in sorted(items.items())]
    out = {'Schema': 1, 'Rules': os.path.basename(args.rules), 'CopperPerGold': rules['copper_per_bg3_gold'],
           'Count': len(rows), 'Items': rows}
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    tmp = Path(str(args.out) + '.tmp')
    tmp.write_text(json.dumps(out, separators=(',', ':'), ensure_ascii=False))
    tmp.replace(args.out)
    print('%d items -> %s (%d bytes); %d with an icon' % (len(rows), args.out, Path(args.out).stat().st_size,
                                                        sum(1 for r in rows if r['i'])))
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['build', 'show', 'stats'])
    ap.add_argument('entries', nargs='*', type=int)
    ap.add_argument('--out', default=str(IO / 'link/items.json'))
    ap.add_argument('--rules', default=str(HERE / 'wow_items_rules.json'))
    ap.add_argument('--data', default=str(DATA))
    ap.add_argument('--db', default=None, help='shell command that runs SQL from stdin on the mangos database')
    args = ap.parse_args()
    args.db = ['sh', '-c', args.db] if args.db else DB_CMD
    if args.cmd == 'build':
        build(args)
        return
    rules = json.loads(Path(args.rules).read_text())
    items = load_items(args.db)
    icons = load_icons(args.data)
    if args.cmd == 'show':
        for e in args.entries:
            print(json.dumps(map_item(rules, items[e], icons), indent=1, ensure_ascii=False))
    else:
        count = {}
        for it in items.values():
            r = map_item(rules, it, icons)
            count[(r['k'], r['b'])] = count.get((r['k'], r['b']), 0) + 1
        for (k, b), n in sorted(count.items(), key=lambda x: -x[1]):
            print('%6d  %-7s %s' % (n, k, b))


if __name__ == '__main__':
    main()
