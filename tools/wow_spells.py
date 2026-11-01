#!/usr/bin/env python3
"""wow_spells.py: the names of our spells, from the player's own WoW client (first run).

  wow_spells.py --data WOW_DATA --out link/spells.json

WowSpells.lua falls back to plain words without this file. Nothing of WoW's text is in the repository: only
the spell ids below.
"""
import argparse, json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from wow_abilities import dbc_rows

SPELLS = {'FrostArmor': 168, 'Nova': 122, 'IceBarrier': 11426, 'Conjure': 587}
NAME_COLUMN = 120  # Spell.dbc 1.12, English name


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--data', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()
    rows, text = dbc_rows(a.data, 'Spell')
    by_id = {r[0]: r for r in rows}
    out = {k: text(by_id[v][NAME_COLUMN]) for k, v in SPELLS.items() if v in by_id}
    Path(a.out).write_text(json.dumps(out, indent=0))
    print(out)


if __name__ == '__main__':
    main()
