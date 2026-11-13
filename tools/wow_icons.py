#!/usr/bin/env python3
"""wow_icons.py: WoW's item icons as an icon atlas of our own for BG3.

  wow_icons.py build [--data WOW_DATA] [--out DIR] [--items items.json]

BG3 shows an item's icon by name from an icon atlas (GUI/Icons_*.lsx: names and UVs, plus one
DDS texture). This writes an atlas of our own, Icons_WowBg3: every WoW icon the item table uses
(link/items.json "i") in a 64 x 64 cell, named "WowBg3_<wow icon name in lower case>". Written to
DIR (default the harness IO dir's gui/): Icons_WowBg3.lsx, Icons_WowBg3.dds (DXT5 with mips) and
wow-icons.json (the names). BG3 reads its atlases once, at boot, and these files are made while
the game runs, so InventoryClient.lua hands the atlas to BG3's own atlas load when the files exist
(Ext.StaticData.LoadIconAtlas): the icons show without a restart. It is a new atlas, not a larger
copy of one of BG3's: BG3 already holds its own atlases' textures by then. Everything comes from
the player's own WoW files; nothing of either game is shipped.
"""
import argparse, io, json, os, re, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IO = ROOT / "bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3"
ATLAS = 'Icons_WowBg3'
ATLAS_UUID = '5d0c7e1a-9b34-4f6e-a2c8-71e3b6d4f905'  # the atlas texture's id: ours alone
CELL = 64
PREFIX = 'WowBg3_'


def dds_dxt5_mips(img, levels):
    """A DXT5 DDS with `levels` mip levels, made with Pillow alone (the release has no
    ImageMagick): Pillow's BC3 encoder per level, one header for the chain."""
    from PIL import Image
    chain, w, h = [], img.width, img.height
    for i in range(levels):
        lv = img if i == 0 else img.resize((max(1, w >> i), max(1, h >> i)), Image.LANCZOS)
        b = io.BytesIO()
        lv.save(b, 'DDS', pixel_format='DXT5')
        chain.append(b.getvalue()[128:])
    head = struct.pack('<4s7I', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w, len(chain[0]), 0, levels)
    head += b'\0' * 44 + struct.pack('<2I4s5I', 32, 0x4, b'DXT5', 0, 0, 0, 0, 0)
    head += struct.pack('<5I', 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
    return head + b''.join(chain)


def icon_key(name):
    return PREFIX + re.sub(r'[^a-z0-9_]', '_', name.lower())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cmd', choices=['build'])
    ap.add_argument('--data', default=os.environ.get('WOW_DATA') or str(ROOT / 'client/Data'))
    ap.add_argument('--bg3', default='', help='not used any more (the atlas holds no BG3 icons); accepted for old callers')
    ap.add_argument('--out', default=str(IO / 'gui'))
    ap.add_argument('--items', default=str(IO / 'link/items.json'))
    a = ap.parse_args()
    sys.path.insert(0, str(HERE))
    from PIL import Image
    from wow_mpq import WowData

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        magick = shutil.which('magick')
        names = sorted({r['i'] for r in json.loads(Path(a.items).read_text())['Items'] if r.get('i')}, key=str.lower)
        wow = WowData(a.data)
        SIZE = 512  # the smallest power of two that holds them all
        while (SIZE // CELL) ** 2 < len(names) and SIZE < 8192:
            SIZE *= 2
        atlas = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
        cells = [(x, y) for y in range(SIZE // CELL) for x in range(SIZE // CELL)]
        placed, missing = {}, []
        for name in names:
            raw = wow.read('Interface\\Icons\\%s.blp' % name)
            if raw is None:
                missing.append(name)
                continue
            if not cells:
                missing.append(name)
                continue
            p = Path(tmp) / 'i.blp'
            p.write_bytes(raw)
            try:
                im = Image.open(p).convert('RGBA')
            except Exception as e:  # noqa: BLE001
                print('icon %s: %s' % (name, e), file=sys.stderr)
                missing.append(name)
                continue
            if im.size != (CELL, CELL):
                im = im.resize((CELL, CELL), Image.LANCZOS)
            x, y = cells.pop(0)
            atlas.paste(im, (x * CELL, y * CELL))
            placed[icon_key(name)] = (x, y)
        big = Path(tmp) / 'atlas.png'
        atlas.save(big)
        levels = SIZE.bit_length()
        tmp_dds = out / (ATLAS + '.dds.tmp')
        if magick:
            subprocess.run([magick, str(big), '-define', 'dds:compression=dxt5', '-define', 'dds:mipmaps=%d' % levels,
                            'DDS:' + str(tmp_dds)], check=True)
        else:
            tmp_dds.write_bytes(dds_dxt5_mips(atlas, levels))
        tmp_dds.replace(out / (ATLAS + '.dds'))

    # The atlas list, in the layout of BG3's own GUI/Icons_*.lsx.
    rows = []
    for key, (x, y) in placed.items():
        u1, u2 = (x * CELL + 0.5) / SIZE, (x * CELL + CELL - 0.5) / SIZE
        v1, v2 = (y * CELL + 0.5) / SIZE, (y * CELL + CELL - 0.5) / SIZE
        rows.append('                <node id="IconUV">\r\n'
                    '                    <attribute id="MapKey" type="FixedString" value="%s"/>\r\n'
                    '                    <attribute id="U1" type="float" value="%.9g"/>\r\n'
                    '                    <attribute id="U2" type="float" value="%.9g"/>\r\n'
                    '                    <attribute id="V1" type="float" value="%.9g"/>\r\n'
                    '                    <attribute id="V2" type="float" value="%.9g"/>\r\n'
                    '                </node>\r\n' % (key, u1, u2, v1, v2))
    lsx = ('<?xml version="1.0" encoding="UTF-8"?>\r\n<save>\r\n    <version major="4" minor="5" revision="0" build="600"/>\r\n'
           '    <region id="IconUVList">\r\n        <node id="root">\r\n            <children>\r\n' + ''.join(rows) +
           '            </children>\r\n        </node>\r\n    </region>\r\n'
           '    <region id="TextureAtlasInfo">\r\n        <node id="root">\r\n            <children>\r\n'
           '                <node id="TextureAtlasIconSize">\r\n'
           '                    <attribute id="Height" type="int32" value="%d"/>\r\n'
           '                    <attribute id="Width" type="int32" value="%d"/>\r\n                </node>\r\n'
           '                <node id="TextureAtlasPath">\r\n'
           '                    <attribute id="Path" type="string" value="Assets/Textures/Icons/%s.dds"/>\r\n'
           '                    <attribute id="UUID" type="FixedString" value="%s"/>\r\n                </node>\r\n'
           '                <node id="TextureAtlasTextureSize">\r\n'
           '                    <attribute id="Height" type="int32" value="%d"/>\r\n'
           '                    <attribute id="Width" type="int32" value="%d"/>\r\n                </node>\r\n'
           '            </children>\r\n        </node>\r\n    </region>\r\n</save>\r\n' % (CELL, CELL, ATLAS, ATLAS_UUID, SIZE, SIZE))
    tmp_lsx = out / (ATLAS + '.lsx.tmp')
    tmp_lsx.write_text(lsx, encoding='utf-8', newline='')
    tmp_lsx.replace(out / (ATLAS + '.lsx'))
    # Last, and in one step: the mod's Lua takes this file as "the atlas is whole".
    tmp_json = out / 'wow-icons.json.tmp'
    tmp_json.write_text(json.dumps({'Schema': 2, 'Prefix': PREFIX, 'Atlas': ATLAS, 'Icons': sorted(placed), 'Missing': missing}))
    tmp_json.replace(out / 'wow-icons.json')
    print('%d WoW icons placed (%d cells left), %d missing -> %s' % (len(placed), len(cells), len(missing), out))


if __name__ == '__main__':
    main()
