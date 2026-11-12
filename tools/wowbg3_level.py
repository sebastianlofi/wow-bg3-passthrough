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

A platform (BG3's own moving ground: a lift, a raft) is a level of its own, placed in another level as a level
template and moved by Osiris PlatformMoveTo. "Platform": true makes a spec such a level (meta IsPlatformLevel, no
start points or zones); "Root" is the id of its root template (root_templates_lsx: Type LevelTemplate, IsMoving,
SubLevelName). A subgrid's "Owner" is the index of the spec's Scenery object it belongs to, as in BG3's own
platform levels (the grid is the floor object's). A level places platforms with
"LevelTemplates": [{"Root", "MapKey", "Name", "Pos"}] (LevelTemplates/_merged.lsx); "LiftPlatforms": true places
the family BG3 keeps for WoW's lifts (tools/wow_lift_platforms.py: release/build-release.py adds its levels).

Also in SPEC.json: "LightingList" / "AtmosphereList" (resource ids listed on the two zones, the first one the
level's own), "LightingZone" / "AtmosphereZone" (fixed ids for the zone triggers, so the mod's Lua can name them),
"BlankGrid" {"X0", "Z0", "N" or "NX" and "NZ", "Y"} (blank squares in rows from that corner: the pool world mode
moves under the party).

SPEC.json: {"Name": "WOWBG3_Skel_A", "Bounds": [minx, miny, minz, maxx, maxy, maxz],
            "Start": [x, y, z],
            "Subgrids": [{"X": minx, "Y": y, "Z": minz, "W": 128, "H": 128, "Kind": "walk"|"blank"}, ...]}
  walk  : every tile open (state 0), flat at y: something to stand on
  disc  : open tiles within "Radius" metres of the subgrid's middle, blank round them (a lift car's floor)
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


SEAM_FLAGS = (0x200 | 0x800) << 46   # AiGridTile flags SubgridEdge and SubgridIntersection (the plugin's kEdgeBit, kIntersectionBit)


def tiles(w, h, kind, radius=None):
    walk, blank = struct.pack("<Qhhhh", 0, 0, 0, -1, -1), struct.pack("<Qhhhh", 0x8005, 0, 0, -1, -1)
    if kind == "walk":
        return walk * (w * h)
    if kind == "blank":
        return blank * (w * h)
    if kind == "disc":
        # A round floor in the subgrid's middle (a lift car's), blank round it. Its outer metre is seams: BG3's path
        # finder steps from one subgrid to another only on tiles flagged SubgridEdge and SubgridIntersection, and a
        # platform at a stop lies inside a ground square's own subgrid (the plugin's Place sets the same two bits
        # where a storey meets the ground).
        seam = struct.pack("<Qhhhh", SEAM_FLAGS, 0, 0, -1, -1)
        cx, cz, r2, in2 = w * 0.25, h * 0.25, radius * radius, max(radius - 1.0, 0.0) ** 2
        out = []
        for r in range(h):
            for c in range(w):
                d2 = ((c + 0.5) * 0.5 - cx) ** 2 + ((r + 0.5) * 0.5 - cz) ** 2
                out.append(blank if d2 > r2 else seam if d2 > in2 else walk)
        return b"".join(out)
    raise ValueError(kind)


def cstr37(s):
    b = s.encode()
    assert len(b) == 36
    return b + b"\0"


def write_aigrid(path, name, subgrids):
    out = bytearray(struct.pack("<I", 21)) + cstr37(uid(name, "aigrid"))
    out += struct.pack("<I", len(subgrids))
    seen = set()
    for i, s in enumerate(subgrids):
        obj = uid(name, "scenery", s["Owner"]) if "Owner" in s else uid(name, "subgrid", i)
        sid = subgrid_id(obj)
        assert sid not in seen
        seen.add(sid)
        x, z = round(s["X"] * 2) / 2, round(s["Z"] * 2) / 2
        w, h = s.get("W", 128), s.get("H", 128)
        lz = lz4_literals(tiles(w, h, s["Kind"], s.get("Radius")))
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
             attr("IsGameMasterLevel", "bool", "False"), attr("IsPlatformLevel", "bool", str(bool(spec.get("Platform")))),
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


def write_level_templates(path, spec, layer):
    """LevelTemplates: the platforms placed in this level, each an instance of a root template (by id)."""
    name = spec["Name"]
    objs = []
    for t in spec["LevelTemplates"]:
        objs.append('<node id="GameObjects">' + attr("Flag", "int32", "1") + attr("LevelName", "FixedString", name)
                    + attr("MapKey", "FixedString", t["MapKey"]) + attr("Name", "LSString", t["Name"])
                    + attr("TemplateName", "FixedString", t["Root"]) + attr("Type", "FixedString", "LevelTemplate")
                    + "<children>" + layer_list(name, layer) + transform(t["Pos"]) + "</children></node>")
    x = (HDR + '\t<region id="Templates">\n\t\t<node id="Templates">\n\t\t\t<children>\n'
         + "".join("\t\t\t\t" + o + "\n" for o in objs) + "\t\t\t</children>\n\t\t</node>\n\t</region>\n</save>\n")
    open(path, "w").write(x)


def root_templates_lsx(specs):
    """The root templates of the platform levels among `specs` (the mod's Public/<mod>/RootTemplates): what a
    level's LevelTemplates name. The attributes are those of BG3's own moving platforms, without their sounds."""
    objs = []
    for spec in specs:
        if not spec.get("Platform"):
            continue
        objs.append('<node id="GameObjects">' + "".join([
            attr("CameraOffset", "fvec3", "0 0 0"), attr("Flag", "int32", "0"), attr("GroupID", "uint32", "0"),
            attr("HasGameplayValue", "bool", "False"), attr("IsDynamicLayer", "bool", "False"), attr("IsMoving", "bool", "True"),
            attr("IsScrollingObject", "bool", "False"), attr("LevelName", "FixedString", ""), attr("LevelTemplateType", "uint8", "0"),
            attr("MapKey", "FixedString", spec["Root"]), attr("MovablePlatformStartSound", "FixedString", ""),
            attr("MovablePlatformStopSound", "FixedString", ""), attr("Name", "LSString", "LT_" + spec["Name"]),
            attr("ParentTemplateId", "FixedString", ""), attr("PhysicsOpenTemplate", "FixedString", ""),
            attr("PhysicsTemplate", "FixedString", ""), attr("ScrollingDirection", "fvec3", "1 0 0"),
            attr("ScrollingDistance", "float", "0"), attr("ScrollingOffset", "float", "0"), attr("ScrollingOrigin", "fvec3", "0 0 0"),
            attr("ScrollingSpeed", "float", "0"), attr("StartingActive", "bool", "False"), attr("StartingLoaded", "bool", "True"),
            attr("SubLevelName", "FixedString", spec["Name"]), attr("TemplateAfterDestruction", "FixedString", ""),
            attr("Type", "FixedString", "LevelTemplate"), attr("VisualTemplate", "FixedString", "")]) + "</node>")
    if not objs:
        return None
    return (HDR + '\t<region id="Templates">\n\t\t<node id="Templates">\n\t\t\t<children>\n'
            + "".join("\t\t\t\t" + o + "\n" for o in objs) + "\t\t\t</children>\n\t\t</node>\n\t</region>\n</save>\n")


def write_minimap(path, spec):
    """The level's map frame (WorldMap/MiniMap): where BG3's minimap and map page lay the level's map picture, here the
    whole level with no picture. Without it the map's view model has no world size and nothing can be laid on the
    minimap (WowUi.lua puts WoW's own minimap art there)."""
    b = spec["Bounds"]
    # The picture's size in pixels sets the map's scale: BG3's own levels have about 0.75 pixel a metre (BGO_Main_A:
    # 4096 over 5441 m), and the minimap's zoom range is made for that.
    px = lambda span: str(int(round(span * 0.7527)))
    x = (HDR.replace('revision="9" build="0"', 'revision="9" build="310"')
         + '\t<region id="WorldMapMetaData">\n\t\t<node id="WorldMapMetaData">\n'
         + "".join("\t\t\t" + a + "\n" for a in [
             attr("BuildingUUID", "FixedString", ""), attr("Floor", "int32", "0"), attr("Height", "uint32", px(b[5] - b[2])),
             attr("RenderPatches", "bool", "False"), attr("TextureFilePath", "LSString", ""),
             attr("TriggerId", "guid", "00000000-0000-0000-0000-000000000000"), attr("Width", "uint32", px(b[3] - b[0])),
             attr("WorldHeight", "float", repr(float(b[5] - b[2]))), attr("WorldWidth", "float", repr(float(b[3] - b[0]))),
             attr("WorldX", "float", repr((b[0] + b[3]) / 2.0)), attr("WorldZ", "float", repr((b[2] + b[5]) / 2.0))])
         + "\t\t</node>\n\t</region>\n</save>\n")
    open(path, "w").write(x)


def write_layer(path, layer):
    open(path, "w").write('<?xml version="1.0" encoding="UTF-8"?>\n<save>\n    <version major="4" minor="0" revision="0" build="77"/>\n'
                          '    <region id="MetaData">\n        <node id="root">\n'
                          f'            {attr("ID", "guid", layer)}\n            {attr("LayerColor", "int32", "-16740097")}\n'
                          f'            {attr("Name", "LSString", "Triggers")}\n        </node>\n    </region>\n</save>\n')


def build(spec, out):
    """The level folder of `spec` (a dict as SPEC.json) in `out`; returns the level's name."""
    spec.setdefault("Lighting", "375af627-6f3c-19e8-1046-29265ae9e8f7")    # stock resource ids (Basic_Level_A's defaults)
    spec.setdefault("Atmosphere", "73e03af9-7ab1-47a7-906b-a4e0362045ef")
    if spec.get("LightingList"): spec["Lighting"] = spec["LightingList"][0]      # the zone's first is the level's own
    if spec.get("AtmosphereList"): spec["Atmosphere"] = spec["AtmosphereList"][0]
    if spec.get("BlankGrid"):  # {"X0","Z0","N","Y"}: N x N blank 128-tile squares from that corner, the treadmill's pool
        g = spec["BlankGrid"]
        nx, nz = g.get("NX", g.get("N")), g.get("NZ", g.get("N"))
        spec["Subgrids"] = spec.get("Subgrids", []) + [{"X": g["X0"] + 64 * i, "Y": g["Y"], "Z": g["Z0"] + 64 * j, "Kind": "blank"}
                                                        for j in range(nz) for i in range(nx)]
    platform = bool(spec.get("Platform"))
    for d in ("Ai", "Layers") + (() if platform else ("Triggers", "WorldMap")):
        os.makedirs(os.path.join(out, d), exist_ok=True)
    layer = uid(spec["Name"], "layer", "Triggers")
    write_meta(os.path.join(out, "meta.lsx"), spec)
    write_layer(os.path.join(out, "Layers", layer + ".lsx"), layer)
    if not platform:   # a platform is a piece of another level: that level's start points, zones and map
        write_minimap(os.path.join(out, "WorldMap", "MiniMap.lsx"), spec)
        write_triggers(os.path.join(out, "Triggers", "_merged.lsx"), spec, layer)
    if spec.get("Scenery"):
        os.makedirs(os.path.join(out, "Scenery"), exist_ok=True)
        write_scenery(os.path.join(out, "Scenery", "_merged.lsx"), spec, layer)
    if spec.get("LevelTemplates"):
        os.makedirs(os.path.join(out, "LevelTemplates"), exist_ok=True)
        write_level_templates(os.path.join(out, "LevelTemplates", "_merged.lsx"), spec, layer)
    n = write_aigrid(os.path.join(out, "Ai", "aigrid.data"), spec["Name"], spec["Subgrids"])
    json.dump(spec, open(os.path.join(out, "spec-out.json"), "w"), indent=1)
    print(f"{spec['Name']}: {len(spec['Subgrids'])} subgrids, aigrid {n} bytes ->", out)
    return spec["Name"]


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    build(json.load(open(sys.argv[1])), sys.argv[2])
    return 0


if __name__ == "__main__":
    sys.exit(main())
