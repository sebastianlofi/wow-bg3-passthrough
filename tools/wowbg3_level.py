#!/usr/bin/env python3
"""wowbg3_level.py SPEC.json OUTDIR: write a BG3 level folder of our own from nothing but this code.

No terrain, no characters, no story, nothing derived from WoW or copied from a Larian level.
What it writes: an empty level the size of a WoW continent, whose ground world mode fills in while the game runs.

  meta.lsx               LevelData: LevelBounds, LevelLighting, LevelAtmosphere (ids of stock BG3
                         lighting/atmosphere *resources*, referenced by id, not copied)
  Layers/<guid>.lsx      one editor layer ("Triggers") the triggers belong to
  Triggers/_merged.lsx   start points (Player 0..3), one lighting trigger and one atmosphere trigger
                         covering the bounds; same structure as Basic_Level_A's triggers
  Scenery/_merged.lsx    optional: static objects from stock BG3 templates (by id), e.g. the WoW rays' proxy shape
  Ai/aigrid.data         AI grid, version 21 (layout from libbg3, github.com/eiz/libbg3, MIT):
                           u32 version | char[37] file uuid (36 + NUL) | u32 subgrid count |
                           per subgrid: u32 id, u32 width, u32 height, f32 x, y, z (min corner, metres,
                           x and z on 0.5 m), char[37] object uuid, char[37] template uuid,
                           u32 lz4 size, u32 deflate size, deflate(lz4(width*height tiles)) |
                           u32 layer count (0 here)
                         tile (16 bytes): u64 state, i16 height, i16 bottom (both metres*50 above y),
                           i16 metadata = -1, i16 surface = -1 (runtime only)
  The .lsx files must be turned into .lsf (LSLib's converter) before BG3 reads them.

Also in SPEC.json: "LightingList" / "AtmosphereList" (resource ids listed on the two zones, the first one the
level's own), "LightingZone" / "AtmosphereZone" (fixed ids for the zone triggers, so the mod's Lua can name them),
"BlankGrid" {"X0", "Z0", "N" or "NX" and "NZ", "Y"} (blank squares in rows from that corner: the pool world mode
moves under the party).

SPEC.json: {"Name": "WOWBG3_Skel_A", "Bounds": [minx, miny, minz, maxx, maxy, maxz],
            "Start": [x, y, z],
            "Subgrids": [{"X": minx, "Y": y, "Z": minz, "W": 128, "H": 128, "Kind": "walk"|"blank"}, ...]}
  walk  : every tile open (state 0), flat at y: something to stand on
  blank : every tile 0x8005 (EMPTY | MOVEMENT_BLOCKED | BLOCKED_UNREACHABLE = "GenericBlock"), the
          state of BG3's own out-of-bounds layer tiles that the plugin's Pool takes for the treadmill
"""
import json, os, struct, sys, uuid, zlib

LZ4_MAX_LITERAL_BLOCK = None
NS = uuid.UUID("6b1e3c52-7f0a-4c55-9d8e-0a17b3a2c9f1")   # our own namespace: every id below is derived from it


def uid(*parts):
    """A stable uuid for a named thing in this level (same spec -> same ids)."""
    return str(uuid.uuid5(NS, "/".join(str(p) for p in parts)))


def lz4_literals(data):
    """One LZ4 block holding data as a single literal run (valid for any LZ4 decoder; no matches).
    Deflate afterwards removes the redundancy, so the file stays small."""
    n = len(data)
    out = bytearray()
    if n < 15:
        out.append(n << 4)
    else:
        out.append(0xF0)
        r = n - 15
        while r >= 255:
            out.append(255); r -= 255
        out.append(r)
    out += data
    return bytes(out)


def subgrid_id(obj_uuid):
    """libbg3 aigrid_uuid_hash: xor of the uuid's two 64-bit halves (as stored in memory), folded to 32 bits."""
    b = uuid.UUID(obj_uuid).bytes_le  # Windows GUID memory layout
    lo, hi = struct.unpack("<QQ", b)
    x = lo ^ hi
    return ((x >> 32) ^ x) & 0xFFFFFFFF


def tiles(w, h, kind):
    if kind == "walk":
        state, height = 0, 0
    elif kind == "blank":
        state, height = 0x8005, 0
    else:
        raise ValueError(kind)
    one = struct.pack("<Qhhhh", state, height, height, -1, -1)
    return one * (w * h)


def cstr37(s):
    b = s.encode()
    assert len(b) == 36
    return b + b"\0"


def write_aigrid(path, name, subgrids):
    out = bytearray(struct.pack("<I", 21)) + cstr37(uid(name, "aigrid"))
    out += struct.pack("<I", len(subgrids))
    seen = set()
    for i, s in enumerate(subgrids):
        obj = uid(name, "subgrid", i)
        sid = subgrid_id(obj)
        assert sid not in seen
        seen.add(sid)
        x, z = round(s["X"] * 2) / 2, round(s["Z"] * 2) / 2
        w, h = s.get("W", 128), s.get("H", 128)
        lz = lz4_literals(tiles(w, h, s["Kind"]))
        dz = zlib.compress(lz, 9)
        out += struct.pack("<IIIfff", sid, w, h, x, s["Y"], z)
        out += cstr37(obj) + cstr37("00000000-0000-0000-0000-000000000000")
        out += struct.pack("<II", len(lz), len(dz)) + dz
        s["Id"] = sid
    out += struct.pack("<I", 0)  # no AiGridLayer deltas (no level templates)
    open(path, "wb").write(out)
    return len(out)


HDR = '<?xml version="1.0" encoding="utf-8"?>\n<save>\n\t<version major="4" minor="0" revision="9" build="0" lslib_meta="v1,bswap_guids,lsf_adjacency" />\n'


def attr(i, t, v):
    return f'<attribute id="{i}" type="{t}" value="{v}" />'


def f3(v):
    return " ".join(repr(float(a)) for a in v)


def write_meta(path, spec):
    b = spec["Bounds"]
    x = (HDR.replace('revision="9" build="0"', 'revision="0" build="77"')
         + '\t<region id="LevelData">\n\t\t<node id="LevelData">\n'
         + "".join("\t\t\t" + a + "\n" for a in [
             attr("IsGameMasterLevel", "bool", "False"), attr("IsPlatformLevel", "bool", "False"),
             attr("LevelAtmosphere", "FixedString", spec["Atmosphere"]), attr("LevelLighting", "FixedString", spec["Lighting"]),
             attr("LevelTemplateBoundTrigger", "FixedString", ""), attr("VisualStreamingEnabled", "bool", "True")])
         + '\t\t\t<children>\n\t\t\t\t<node id="LevelBounds">\n'
         + "\t\t\t\t\t" + attr("Max", "fvec3", f3(b[3:6])) + "\n"
         + "\t\t\t\t\t" + attr("Min", "fvec3", f3(b[0:3])) + "\n"
         + "\t\t\t\t</node>\n\t\t\t</children>\n\t\t</node>\n\t</region>\n</save>\n")
    open(path, "w").write(x)


def layer_list(name, layer):
    return ('<node id="LayerList"><children><node id="Layers"><children><node id="Object">'
            + attr("MapKey", "FixedString", name)
            + '<children><node id="Layer">' + attr("Object", "guid", layer)
            + '</node></children></node></children></node></children></node>')


def transform(pos):
    return ('<node id="Transform">' + attr("Position", "fvec3", f3(pos)) + attr("RotationQuat", "fvec4", "0 0 0 1")
            + attr("Scale", "float", "1") + "</node>")


def write_triggers(path, spec, layer):
    name = spec["Name"]
    b = spec["Bounds"]
    objs = []
    sx, sy, sz = spec["Start"]
    for p in range(4):  # start points, template e1dd1fce (BG3's StartPoint trigger template)
        objs.append('<node id="GameObjects">' + attr("Angle", "float", "-1") + attr("Flag", "int32", "1")
                    + attr("LevelName", "FixedString", name) + attr("MapKey", "FixedString", uid(name, "start", p))
                    + attr("Name", "LSString", f"StartPoint_{p:03d}__000") + attr("Player", "uint8", str(p))
                    + attr("TemplateName", "FixedString", "e1dd1fce-eb20-4824-b232-5a78344351b1")
                    + attr("Type", "FixedString", "trigger") + "<children><node id=\"FadeChildren\" />"
                    + layer_list(name, layer) + transform((sx + 2 * p, sy, sz)) + "</children></node>")
    # Lighting (template 1a4cfe92) and atmosphere (template 14ec1f7b) zones: polygon over the bounds, a height.
    poly = [(b[0], b[2]), (b[3], b[2]), (b[3], b[5]), (b[0], b[5])]
    for kind, tpl, res in (("Lighting", "1a4cfe92-0a55-4253-b856-44cd0b0e8f8f", spec["Lighting"]),
                           ("Atmosphere", "14ec1f7b-dfb9-4001-af8f-469b06c1c70f", spec["Atmosphere"])):
        objs.append('<node id="GameObjects">' + attr("FadeTime", "float", "4") + attr("Flag", "int32", "1")
                    + attr("Height", "float", str(float(b[4] - b[1]))) + attr("LevelName", "FixedString", name)
                    + attr("MapKey", "FixedString", spec.get(kind + "Zone") or uid(name, kind)) + attr("Name", "LSString", f"WOWBG3_{kind}Trigger_All")
                    + attr("TemplateName", "FixedString", tpl) + attr("Type", "FixedString", "trigger")
                    + "<children><node id=\"FadeChildren\" />" + layer_list(name, layer)
                    + "".join('<node id="Points">' + attr("Object", "fvec2", f"{float(px)!r} {float(pz)!r}") + "</node>" for px, pz in poly)
                    + '<node id="ResourceIDs"><children>'
                    + "".join('<node id="ResourceID">' + attr("Object", "FixedString", r) + "</node>" for r in (spec.get(kind + "List") or [res]))
                    + "</children></node>" + transform((0.0, float(b[1]), 0.0)) + "</children></node>")
    x = (HDR + '\t<region id="Templates">\n\t\t<node id="Templates">\n\t\t\t<children>\n'
         + "".join("\t\t\t\t" + o + "\n" for o in objs) + "\t\t\t</children>\n\t\t</node>\n\t</region>\n</save>\n")
    open(path, "w").write(x)


def write_scenery(path, spec, layer):
    """Scenery: static objects placed from stock BG3 templates (by id). The ray proxy: the plugin's WoW
    rays need one static BG3 shape in the level's physics scene to name in their hits."""
    name = spec["Name"]
    objs = []
    for i, s in enumerate(spec.get("Scenery", [])):
        objs.append('<node id="GameObjects">' + attr("Flag", "int32", "1") + attr("LevelName", "FixedString", name)
                    + attr("MapKey", "FixedString", uid(name, "scenery", i)) + attr("Name", "LSString", s.get("Name", f"WOWBG3_Scenery_{i:03d}"))
                    + attr("TemplateName", "FixedString", s["Template"]) + attr("Type", "FixedString", "scenery")
                    + "<children>" + layer_list(name, layer) + transform(s["Pos"]) + "</children></node>")
    x = (HDR + '\t<region id="Templates">\n\t\t<node id="Templates">\n\t\t\t<children>\n'
         + "".join("\t\t\t\t" + o + "\n" for o in objs) + "\t\t\t</children>\n\t\t</node>\n\t</region>\n</save>\n")
    open(path, "w").write(x)


def write_layer(path, layer):
    open(path, "w").write('<?xml version="1.0" encoding="UTF-8"?>\n<save>\n    <version major="4" minor="0" revision="0" build="77"/>\n'
                          '    <region id="MetaData">\n        <node id="root">\n'
                          f'            {attr("ID", "guid", layer)}\n            {attr("LayerColor", "int32", "-16740097")}\n'
                          f'            {attr("Name", "LSString", "Triggers")}\n        </node>\n    </region>\n</save>\n')


def main():
    spec = json.load(open(sys.argv[1]))
    out = sys.argv[2]
    spec.setdefault("Lighting", "375af627-6f3c-19e8-1046-29265ae9e8f7")    # stock resource ids (Basic_Level_A's defaults)
    spec.setdefault("Atmosphere", "73e03af9-7ab1-47a7-906b-a4e0362045ef")
    if spec.get("LightingList"): spec["Lighting"] = spec["LightingList"][0]      # the zone's first is the level's own
    if spec.get("AtmosphereList"): spec["Atmosphere"] = spec["AtmosphereList"][0]
    if spec.get("BlankGrid"):  # {"X0","Z0","N","Y"}: N x N blank 128-tile squares from that corner, the treadmill's pool
        g = spec["BlankGrid"]
        nx, nz = g.get("NX", g.get("N")), g.get("NZ", g.get("N"))
        spec["Subgrids"] = spec.get("Subgrids", []) + [{"X": g["X0"] + 64 * i, "Y": g["Y"], "Z": g["Z0"] + 64 * j, "Kind": "blank"}
                                                        for j in range(nz) for i in range(nx)]
    for d in ("Ai", "Triggers", "Layers"):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    layer = uid(spec["Name"], "layer", "Triggers")
    write_meta(os.path.join(out, "meta.lsx"), spec)
    write_layer(os.path.join(out, "Layers", layer + ".lsx"), layer)
    write_triggers(os.path.join(out, "Triggers", "_merged.lsx"), spec, layer)
    if spec.get("Scenery"):
        os.makedirs(os.path.join(out, "Scenery"), exist_ok=True)
        write_scenery(os.path.join(out, "Scenery", "_merged.lsx"), spec, layer)
    n = write_aigrid(os.path.join(out, "Ai", "aigrid.data"), spec["Name"], spec["Subgrids"])
    json.dump(spec, open(os.path.join(out, "spec-out.json"), "w"), indent=1)
    print(f"{spec['Name']}: {len(spec['Subgrids'])} subgrids, aigrid {n} bytes ->", out)


if __name__ == "__main__":
    main()
