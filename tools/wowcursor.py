#!/usr/bin/env python3
"""WoW's quest cursors for BG3. Harness tool; the release asks
benilla to write the same files (the interaction notes "Cursors").

BG3 draws its cursor with SDL color cursors made from `Cursors/<name>_1.cur` and `_2.cur` in
Game.pak (32 x 32, 32-bit, hotspot 0,0; `_2` is the pressed look). WowCursor.lua points two cursor
types world mode never uses (Listen and ListenWarning) at the files written here, so BG3 shows
WoW's gold `!` (a quest to pick up) and gold `?` (a quest to hand in) over a WoW quest giver.
The art is WoW's own (the player's files): Interface\\GossipFrame\\AvailableQuestIcon.blp and
ActiveQuestIcon.blp (16 x 16), scaled to the cursor size.

  wowcursor.py [--data WOW_DATA_DIR] [--out DIR]     write quest-available.cur, quest-turnin.cur,
                                                      merchant.cur, merchant-gray.cur (and their
                                                      -pressed variants) into DIR
Default DIR: the private profile's Script Extender/wow-bg3/cursors. Needs Pillow (tmp/pyenv).
Output is derived from Blizzard data: never commit it.
"""
import argparse
import io
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
SE = ROOT / "bg3/private/home/.local/share/Larian Studios/Baldur's Gate 3/Script Extender/wow-bg3"
MPQX = ROOT / "tmp/benilla-target/debug/mpqx"
ICONS = {"quest-available": "Interface\\GossipFrame\\AvailableQuestIcon.blp",
         "quest-turnin": "Interface\\GossipFrame\\ActiveQuestIcon.blp",
         # WoW's merchant cursor over a vendor (benilla patch 0064 writes the same; BG3's OpenDoor).
         "merchant": "Interface\\Cursor\\Buy.blp",
         "merchant-gray": "Interface\\Cursor\\UnableBuy.blp"}
SIZE = 32


def cur_bytes(img, hot=(0, 0)):
    """A one-image Windows .cur as BG3's own files are: 32 x 32 BGRA, bottom-up, AND mask after."""
    img = img.convert("RGBA")
    w, h = img.size
    assert (w, h) == (SIZE, SIZE)
    px = img.transpose(Image.FLIP_TOP_BOTTOM).tobytes("raw", "BGRA")
    mask = bytes(w * h // 8)  # all 0: the alpha channel decides
    bmp = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(px) + len(mask), 0, 0, 0, 0) + px + mask
    head = struct.pack("<HHH", 0, 2, 1) + struct.pack("<BBBBHHII", w, h, 0, 0, hot[0], hot[1], len(bmp), 22)
    return head + bmp


def cursor_image(icon, pressed=False):
    """The 16 x 16 icon scaled into the cursor, its top left near the hotspot (as BG3's art sits)."""
    icon = icon.convert("RGBA")
    box = icon.getbbox() or (0, 0, icon.width, icon.height)
    glyph = icon.crop(box)
    scale = (26 if pressed else 30) / max(glyph.size)
    glyph = glyph.resize((max(1, round(glyph.width * scale)), max(1, round(glyph.height * scale))), Image.LANCZOS)
    out = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    out.alpha_composite(glyph, (1 if not pressed else 3, 1 if not pressed else 3))
    return out


def read_blp(data_dir, vpath):
    if not MPQX.is_file():  # the release: link/tools/wow_mpq.py reads the MPQs
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from wow_mpq import WowData
        raw = WowData(str(data_dir)).read(vpath)
        assert raw is not None, "no %s in %s" % (vpath, data_dir)
        return Image.open(io.BytesIO(raw))
    with tempfile.TemporaryDirectory() as td:
        out = Path(td) / "icon.blp"
        subprocess.run([str(MPQX), str(data_dir), vpath, str(out)], check=True, stdout=subprocess.DEVNULL)
        return Image.open(io.BytesIO(out.read_bytes()))


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--data", default=str(ROOT / "client/SoloCraft 1.12.1/Data"))
    p.add_argument("--out", default=str(SE / "cursors"))
    a = p.parse_args()
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    for stem, vpath in ICONS.items():
        icon = read_blp(a.data, vpath)
        for pressed in (False, True):
            name = stem + ("-pressed" if pressed else "") + ".cur"
            (out / name).write_bytes(cur_bytes(cursor_image(icon, pressed)))
            print(out / name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
