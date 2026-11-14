#!/usr/bin/env python3
"""camp_grid.py OUR_PAK BG3_DATA OUT: our level's AI grid with BG3's camp in it, made on the player's machine.

The camp inside our own level (Camp.lua, release/levels/azeroth_a.json) is BG3's INTMAIN camp, placed by reference: the
level template LT_SCL_Intermezzo_C_ART (root c0865ea8) brings the sub-level SCL_Intermezzo_C_ART with its ground,
scenery, lights, items and triggers. Its AI grid does not come with it: BG3 bakes a placed sub-level's grid into the
parent level's own aigrid.data (INT_Main_A's holds the camp's 43 subgrids, each moved by the template's position), and
at run time loads only the level's file. Without it the camp had no walkable tiles: BG3's cursor found no ground there
and a click moved no one (a story move slid the hero across untouched).

So this joins the camp's grid (Mods/GustavDev/Levels/SCL_Intermezzo_C_ART/Ai/aigrid.data, from the player's own BG3
paks: nothing of Larian's ships with the mod) to the level's (Mods/WOWBG3/Levels/<level>/Ai/aigrid.data in OUR_PAK):
each camp subgrid kept byte for byte, its min corner moved by the template's position in our level (read from OUR_PAK's
LevelTemplates). The coordinator writes OUT at a start when its inputs changed and names it in bg3le's boot path
overrides, so BG3 reads it as the level's grid (wowbg3-coord.py boot_overrides).

AI grid version 21 (layout in tools/wowbg3_level.py): u32 version | char[37] file uuid | u32 subgrid count | subgrids
(u32 id, w, h | f32 x, y, z | char[37] object uuid | char[37] template uuid | u32 lz4 size, u32 deflate size | data) |
layers (u32 count, ...). The camp's layers are left out (our level writes none).

  camp_grid.py OUR_PAK BG3_DATA OUT     prints what it did
"""
import hashlib, os, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import bg3pak  # noqa: E402

LEVEL = "WOWBG3_Azeroth_A"
OUR_GRID = f"Mods/WOWBG3/Levels/{LEVEL}/Ai/aigrid.data"
OUR_LTS = f"Mods/WOWBG3/Levels/{LEVEL}/LevelTemplates/_merged.lsf"
CAMP_ROOT = "c0865ea8-8e34-4a9d-803f-9b3658ddbc1e"   # LT_SCL_Intermezzo_C_ART: the root template the level places
CAMP_GRID = "Mods/GustavDev/Levels/SCL_Intermezzo_C_ART/Ai/aigrid.data"
GAME_PATH = OUR_GRID   # what BG3 asks for: bg3le's override maps it to OUT


def parse(b):
    """(version, file uuid bytes, [(head bytes, x, y, z, rest bytes)], layers bytes) of an AI grid v21."""
    ver, = struct.unpack_from("<I", b, 0)
    if ver != 21:
        raise ValueError(f"AI grid version {ver}, not 21")
    fid = b[4:41]
    n, = struct.unpack_from("<I", b, 41)
    o, subs = 45, []
    for _ in range(n):
        head = b[o:o + 12]
        x, y, z = struct.unpack_from("<fff", b, o + 12)
        lz, df = struct.unpack_from("<II", b, o + 24 + 74)
        end = o + 24 + 74 + 8 + df
        subs.append((head, x, y, z, b[o + 24:end]))
        o = end
    return ver, fid, subs, b[o:]


def merge(ours, camp, offset):
    """Our grid with every camp subgrid appended, moved by offset (x, y, z); our layers kept."""
    ver, fid, subs, layers = parse(ours)
    _, _, csubs, _ = parse(camp)
    out = [struct.pack("<I", ver), fid, struct.pack("<I", len(subs) + len(csubs))]
    for head, x, y, z, rest in subs:
        out += [head, struct.pack("<fff", x, y, z), rest]
    for head, x, y, z, rest in csubs:
        out += [head, struct.pack("<fff", x + offset[0], y + offset[1], z + offset[2]), rest]
    out.append(layers)
    return b"".join(out), len(csubs)


def camp_position(pak):
    """Where our level places the camp's level template (its Transform Position), from OUR_PAK's LevelTemplates."""
    for root in bg3pak.read_lsf(pak.read(OUR_LTS)):
        for node in root.walk():
            if node.attrs.get("TemplateName") == CAMP_ROOT:
                for t in node.walk():
                    if "Position" in t.attrs:
                        return [float(v) for v in t.attrs["Position"]]
    raise LookupError("the level places no camp template")


def make(our_pak, bg3_data, out):
    """Writes OUT (and OUT.key) unless the inputs are the ones it was made from. Returns a line for the log."""
    pak = bg3pak.Pak(str(our_pak))
    ours = pak.read(OUR_GRID)
    src = bg3pak.find_in_paks(str(bg3_data), CAMP_GRID)
    if src is None:
        raise LookupError(f"no {CAMP_GRID} in {bg3_data}")
    camp = src.read(CAMP_GRID)
    pos = camp_position(pak)
    key = hashlib.sha256(ours + camp + repr(pos).encode()).hexdigest()
    out = Path(out)
    keyf = out.with_name(out.name + ".key")
    if out.is_file() and keyf.is_file() and keyf.read_text().strip() == key:
        return f"camp grid: current ({out})"
    merged, n = merge(ours, camp, pos)
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_name(out.name + ".tmp")
    tmp.write_bytes(merged)
    os.replace(tmp, out)
    keyf.write_text(key + "\n")
    return f"camp grid: {n} camp subgrids at {pos} joined to {LEVEL}'s grid -> {out} ({len(merged)} bytes)"


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    print(make(*sys.argv[1:]))
