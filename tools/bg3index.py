#!/usr/bin/env python3
"""Index BG3's resource banks (materials, textures, virtual textures, material presets) from the
packages' Content/_merged.lsf files into one JSON, for the Tav texture copy.

  bg3index.py GAME_DATA_DIR OUT.json

Later packages override earlier ones (Shared, Gustav, GustavX, then the patch packages)."""
import json, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bg3pak import Pak, read_lsf  # noqa: E402

BANKS = {'MaterialBank': 'materials', 'TextureBank': 'textures', 'VirtualTextureBank': 'vtextures',
         'MaterialPresetBank': 'presets', 'VisualBank': None}


def resource(node):
    r = dict(node.attrs)
    for c in node.children:
        r.setdefault(c.name, []).append(resource(c))
    return r


def main():
    data, out = sys.argv[1], sys.argv[2]
    order = ['Shared.pak', 'Gustav.pak', 'GustavX.pak'] + sorted(
        f for f in os.listdir(data) if f.startswith('Patch') and f.endswith('.pak'))
    index = {v: {} for v in BANKS.values() if v}
    for pk in order:
        path = os.path.join(data, pk)
        if not os.path.exists(path):
            continue
        p = Pak(path)
        names = [n for n in p.files if '/Content/' in n and n.endswith('_merged.lsf')]
        got = 0
        for n in names:
            try:
                roots = read_lsf(p.read(n))
            except Exception as e:  # noqa: BLE001
                print(f'{pk}: {n}: {e}', file=sys.stderr)
                continue
            for root in roots:
                for bank in root.walk():
                    kind = BANKS.get(bank.name)
                    if not kind:
                        continue
                    for res in bank.find('Resource'):
                        r = resource(res)
                        r['_file'] = n
                        index[kind][r.get('ID')] = r
                        got += 1
        print(f'{pk}: {len(names)} files, {got} resources', file=sys.stderr)
    json.dump(index, open(out, 'w'))
    print({k: len(v) for k, v in index.items()}, file=sys.stderr)


main()
