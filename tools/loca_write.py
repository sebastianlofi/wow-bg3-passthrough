#!/usr/bin/env python3
"""loca_write.py: BG3's binary text table (.loca) from handles and their texts.

The layout is LSLib's (github.com/Norbyte/lslib, MIT):

  "LOCA", u32 entry count, u32 offset of the texts
  an entry: char[64] handle (NUL padded), u16 version, u32 length of its text with the NUL
  the texts, one after another in the entries' order, UTF-8, each NUL terminated

`level_names(specs)` gives what BG3 needs to show a level of our own by a name instead of its folder's: BG3 looks a
level's name up as a TranslatedStringKey whose UUID is the level's name (its own are in
Mods/GustavDev/Localization/Levels.lsf), and shows the folder's name where there is none.
"""
import struct
import uuid

NS = uuid.UUID("6b1e3c52-7f0a-4c55-9d8e-0a17b3a2c9f1")   # the level builder's namespace (tools/wowbg3_level.py)


def loca(texts):
    """texts: {handle: text}. The .loca bytes, entries in the handles' order."""
    items = sorted(texts.items())
    head = b"LOCA" + struct.pack("<II", len(items), 12 + 70 * len(items))
    entries, body = bytearray(), bytearray()
    for handle, text in items:
        key = handle.encode("ascii")
        assert 0 < len(key) < 64, handle
        t = text.encode("utf-8") + b"\0"
        entries += key.ljust(64, b"\0") + struct.pack("<HI", 1, len(t))
        body += t
    return bytes(head + entries + body)


def handle(level):
    """The handle of a level's shown name: fixed by the level's name, so a rebuilt pak keeps it."""
    return "h" + str(uuid.uuid5(NS, "level name " + level)).replace("-", "g")


def level_names(specs):
    """specs: the level specifications (each with "Name" and "DisplayName").
    Returns (the TranslatedStringKeys table as .lsx text, {handle: shown name})."""
    nodes, texts = [], {}
    for s in sorted(specs, key=lambda s: s["Name"]):
        h = handle(s["Name"])
        texts[h] = s["DisplayName"]
        nodes.append(
            '                <node id="TranslatedStringKey">\n'
            f'                    <attribute id="Content" type="TranslatedString" handle="{h}" version="1"/>\n'
            '                    <attribute id="ExtraData" type="LSString" value="Level name. Shown in the UI."/>\n'
            '                    <attribute id="Speaker" type="FixedString" value=""/>\n'
            '                    <attribute id="Stub" type="bool" value="True"/>\n'
            f'                    <attribute id="UUID" type="FixedString" value="{s["Name"]}"/>\n'
            '                </node>\n')
    lsx = ('<?xml version="1.0" encoding="utf-8"?>\n<save>\n    <version major="4" minor="0" revision="9" build="330"/>\n'
           '    <region id="TranslatedStringKeys">\n        <node id="TranslatedStringKeys">\n            <children>\n'
           + "".join(nodes) +
           '            </children>\n        </node>\n    </region>\n</save>\n')
    return lsx, texts
