#!/usr/bin/env python3
"""wow_icons.py: WoW's item icons in BG3's own icon atlas.

  wow_icons.py build [--data WOW_DATA] [--bg3 BG3_DATA] [--out DIR] [--items items.json]

BG3 shows an item's icon by name from an icon atlas (GUI/Icons_*.lsx: names and UVs, plus one
DDS texture). This makes a larger copy of one of BG3's item atlases, Icons_Items_6 (1024 x 1024,
163 icons): BG3's own picture stays in the top-left 1024 x 1024, every WoW icon the item table
uses (link/items.json "i") goes into the free 64 x 64 cells of a 4096 x 4096 texture, named
"WowBg3_<wow icon name in lower case>". Written to DIR (default the harness IO dir's gui/):
Icons_Items_6.lsx, Icons_Items_6.dds (DXT5 with mips, ImageMagick) and wow-icons.json (the names).
InventoryClient.lua puts both files in place of the game's own with Ext.IO.AddPathOverride at
bootstrap, before BG3 loads its atlases. Everything comes from the player's own files (the WoW
MPQs and BG3's paks); nothing of either game is shipped.
"""
import argparse, io, json, os, re, shutil, struct, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
IO = ROOT / "bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3"
ATLAS_LSX = 'Public/Shared/GUI/Icons_Items_6.lsx'
ATLAS_DDS = 'Public/Shared/Assets/Textures/Icons/Icons_Items_6.dds'
SIZE, CELL, OLD = 4096, 64, 1024
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
    ap.add_argument('--data', default=str(ROOT / 'client/SoloCraft 1.12.1/Data'))
    ap.add_argument('--bg3', default=str(ROOT / 'bg3/private/game/Data'))
    ap.add_argument('--out', default=str(IO / 'gui'))
    ap.add_argument('--items', default=str(IO / 'link/items.json'))
    a = ap.parse_args()
    sys.path.insert(0, str(HERE))
    from PIL import Image
    from bg3pak import Pak
    from wow_mpq import WowData

    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    lsx = Pak(os.path.join(a.bg3, 'Shared.pak')).read(ATLAS_LSX).decode('utf-8')
    dds = Pak(os.path.join(a.bg3, 'Icons.pak')).read(ATLAS_DDS)
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / 'orig.dds'
        src.write_bytes(dds)
        png = Path(tmp) / 'orig.png'
        magick = shutil.which('magick')
        if magick:
            subprocess.run([magick, str(src) + '[0]', str(png)], check=True)
            orig = Image.open(png).convert('RGBA')
        else:  # Pillow reads BG3's DXT5 atlas itself (the release)
            orig = Image.open(src).convert('RGBA')
        assert orig.size == (OLD, OLD), orig.size
        atlas = Image.new('RGBA', (SIZE, SIZE), (0, 0, 0, 0))
        atlas.paste(orig, (0, 0))

        names = sorted({r['i'] for r in json.loads(Path(a.items).read_text())['Items'] if r.get('i')}, key=str.lower)
        wow = WowData(a.data)
        cells = [(x, y) for y in range(SIZE // CELL) for x in range(SIZE // CELL) if not (x < OLD // CELL and y < OLD // CELL)]
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
        tmp_dds = out / 'Icons_Items_6.dds.tmp'
        if magick:
            subprocess.run([magick, str(big), '-define', 'dds:compression=dxt5', '-define', 'dds:mipmaps=12',
                            'DDS:' + str(tmp_dds)], check=True)
        else:
            tmp_dds.write_bytes(dds_dxt5_mips(atlas, 12))
        tmp_dds.replace(out / 'Icons_Items_6.dds')

    # The atlas list: BG3's own UVs scaled into the top-left quarter, then ours.
    def scale(m):
        return '%s value="%.9g"' % (m.group(1), float(m.group(2)) * OLD / SIZE)
    lsx = re.sub(r'(id="[UV][12]" type="float") value="([0-9.eE+-]+)"', scale, lsx)
    lsx = re.sub(r'(<node id="TextureAtlasTextureSize">\s*<attribute id="Height" type="int32" value=")\d+',
                 r'\g<1>%d' % SIZE, lsx)
    lsx = re.sub(r'(<node id="TextureAtlasTextureSize">\s*<attribute id="Height" type="int32" value="\d+"/>\s*'
                 r'<attribute id="Width" type="int32" value=")\d+', r'\g<1>%d' % SIZE, lsx)
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
    marker = '            </children>\r\n        </node>\r\n    </region>\r\n    <region id="TextureAtlasInfo">'
    assert marker in lsx, 'atlas list layout changed'
    lsx = lsx.replace(marker, ''.join(rows) + marker, 1)
    (out / 'Icons_Items_6.lsx').write_text(lsx, encoding='utf-8', newline='')
    (out / 'wow-icons.json').write_text(json.dumps({'Schema': 1, 'Prefix': PREFIX, 'Atlas': 'Icons_Items_6',
                                                    'Icons': sorted(placed), 'Missing': missing}))
    print('%d WoW icons placed (%d cells left), %d missing -> %s' % (len(placed), len(cells), len(missing), out))


if __name__ == '__main__':
    main()
