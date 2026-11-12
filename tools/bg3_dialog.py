#!/usr/bin/env python3
"""bg3_dialog.py: a BG3 dialog of our own, written as BG3 loads one.

BG3 shows a dialog from three things in a mod's package (their layout read from BG3's own files with
tools/bg3pak.py; nothing of a BG3 dialog is in here):

  Mods/<mod>/Story/DialogsBinary/<Name>.lsf        region "dialog": the speakers and the nodes. A node is a line
        (constructor TagGreeting: the first line of the one addressed; TagQuestion: a line the player picks), with
        its text as a TranslatedString handle, its children by UUID, and the flags picking it sets.
  Public/<mod>/Content/Assets/Dialogs/[PAK]_<mod>/_merged.lsf   region "DialogBank": one Resource a dialog, whose ID
        is the dialog's name for the story ("<Name>_<ID>") and whose SourceFile is the dialog's path with
        "Dialogs" for "DialogsBinary" and ".lsj" for ".lsf".
  Public/<mod>/Flags/<uuid>.lsf                    region "Flags": a flag a node sets (Usage 4: on an object, here
        the player who picked the line; the story hears it as FlagSet(flag, speaker, dialog instance)).

A dialog here has no timeline (BG3's staged cameras and gestures): the speakers stand as they stood.

The dock guardian's dialog (mod/ScriptExtender/Lua/DockGuide.lua): one line of the guardian's, "Travel to <place>?",
and the player's two answers, "Yes, take me to <place>." (sets GO on the player) and "Not now.". The three texts are
handles of our own; the mod writes the place into them when the guardian takes its stand at a dock.

  files(folder)    [(path in the package, bytes)] for the release's .pak
  texts()          {handle: English text} for the package's .loca
"""
import uuid
from xml.sax.saxutils import quoteattr

import lsf_write

NS = uuid.UUID("6b1e3c52-7f0a-4c55-9d8e-0a17b3a2c9f1")   # this project's namespace (tools/loca_write.py)
VERSION = '<version major="4" minor="0" revision="9" build="330"/>'

# BG3's own speaker groups, as its dialogs name them (a speaker's "list"): the players, and the dream guardian of
# whoever it is in this game (the group BG3's own dialogs with the guardian use, e.g. CRE_AstralPrison_Daisy).
PLAYERS = "e0d1ff71-04a8-4340-ae64-9684d846eb83"
GUARDIANS = "6fe77279-1bba-5cc0-a5e0-34beabd2501d"


def uid(name):
    return str(uuid.uuid5(NS, name))


def handle(name):
    """A TranslatedString handle of our own: 'h' and a UUID with 'g' for its dashes, as BG3 writes them."""
    return "h" + uid("text:" + name).replace("-", "g")


NAME = "WOWBG3_DockGuardian"
BANK = uid("dock-guardian:bank")          # the story's name for the dialog is NAME_BANK
DIALOG = uid("dock-guardian:dialog")
GO = uid("dock-guardian:go")              # the flag "Yes, take me to ..." sets on the player
GO_NAME = "WOWBG3_DockGuardian_Go"
ASK, YES, NO = handle("dock-guardian:ask"), handle("dock-guardian:yes"), handle("dock-guardian:no")
# What the handles say before the mod has written a place into them.
TEXTS = {ASK: "Travel across the sea?", YES: "Yes, take me there.", NO: "Not now."}


def attr(name, kind, value):
    return "<attribute id=%s type=%s value=%s/>" % (quoteattr(name), quoteattr(kind), quoteattr(str(value)))


def node(name, attrs=(), kids=()):
    kids = "".join(kids)
    return "<node id=%s>%s%s</node>" % (quoteattr(name), "".join(attrs), "<children>%s</children>" % kids if kids else "")


def save(region, body):
    return '<?xml version="1.0" encoding="utf-8"?><save>%s<region id=%s>%s</region></save>' % (VERSION, quoteattr(region), body)


def line(key, constructor, speaker, text, children=(), root=False, sets=()):
    """One node of a dialog. sets: (flag UUID, speaker index it is set on) pairs, object flags."""
    attrs = [attr("constructor", "FixedString", constructor), attr("UUID", "FixedString", key)]
    if root:
        attrs.append(attr("Root", "bool", "True"))
    if not children:
        attrs.append(attr("endnode", "bool", "True"))
    attrs.append(attr("speaker", "int32", speaker))
    flags = [node("flaggroup", [attr("type", "FixedString", "Object")],
                  [node("flag", [attr("UUID", "FixedString", f), attr("value", "bool", "True"), attr("paramval", "int32", on)])])
             for f, on in sets]
    tag_text = ('<node id="TagText"><attribute id="TagText" type="TranslatedString" handle=%s version="1"/>%s%s</node>'
                % (quoteattr(text), attr("LineId", "guid", uid("line:" + key)), attr("stub", "bool", "True")))
    return node("node", attrs, [
        node("children", kids=[node("child", [attr("UUID", "FixedString", c)]) for c in children]),
        node("GameData", kids=[node("AiPersonalities"), node("MusicInstrumentSounds"), node("OriginSound")]),
        node("Tags"),
        node("setflags", kids=flags),
        node("checkflags"),
        node("TaggedTexts", kids=[node("TaggedText", [attr("HasTagRule", "bool", "True")], [
            node("TagTexts", kids=[tag_text]),
            node("RuleGroup", [attr("TagCombineOp", "uint8", 0)], [node("Rules")])])]),
    ])


def dialog_lsx(key, speakers, lines, root):
    """speakers: each slot's speaker group. lines: line() nodes. root: the first line's UUID."""
    return save("dialog", node("dialog", [attr("category", "LSString", "Generic NPC Dialog"), attr("UUID", "FixedString", key)], [
        node("DefaultAddressedSpeakers", kids=[node("Object", [attr("MapKey", "int32", i), attr("MapValue", "int32", -1)])
                                               for i in range(len(speakers))]),
        node("speakerlist", kids=[node("speaker", [attr("index", "FixedString", i), attr("list", "LSString", group),
                                                   attr("SpeakerMappingId", "guid", uid("speaker:%s:%d" % (key, i)))])
                                  for i, group in enumerate(speakers)]),
        node("nodes", kids=list(lines) + [node("RootNodes", [attr("RootNodes", "FixedString", root)])]),
    ]))


def bank_lsx(resources, folder):
    """resources: (bank ID, name, speaker slots) of each dialog in the package."""
    return save("DialogBank", node("DialogBank", kids=[
        node("Resource", [
            attr("AllowDeadSpeakers", "bool", "False"), attr("BlockFTB", "bool", "False"), attr("EnableTimeline", "bool", "False"),
            attr("ID", "FixedString", key), attr("Localized", "bool", "False"), attr("Name", "LSString", name),
            attr("SourceFile", "LSString", "Mods/%s/Story/Dialogs/%s.lsj" % (folder, name)),
            attr("_OriginalFileVersion_", "32", 144115200960757960), attr("automated", "bool", "False"),
            attr("isWorld", "bool", "False")],
            [node("SpeakerSlotsWithLines", [attr("HasLines", "bool", "True")]) for _ in range(slots)])
        for key, name, slots in resources]))


def flag_lsx(key, name, about):
    return save("Flags", node("Flags", [attr("Description", "LSString", about), attr("Name", "FixedString", name),
                                        attr("UUID", "guid", key), attr("Usage", "uint8", 4)]))


def dock_guardian_lsx():
    ask, yes, no = uid("dock-guardian:node-ask"), uid("dock-guardian:node-yes"), uid("dock-guardian:node-no")
    return dialog_lsx(DIALOG, [GUARDIANS, PLAYERS], [
        line(yes, "TagQuestion", 1, YES, sets=[(GO, 1)]),
        line(no, "TagQuestion", 1, NO),
        line(ask, "TagGreeting", 0, ASK, children=[yes, no], root=True),
    ], ask)


def files(folder):
    lsf = lambda lsx: lsf_write.lsx_to_lsf(lsx.encode("utf-8"))
    return [
        ("Mods/%s/Story/DialogsBinary/%s.lsf" % (folder, NAME), lsf(dock_guardian_lsx())),
        ("Public/%s/Content/Assets/Dialogs/[PAK]_%s/_merged.lsf" % (folder, folder), lsf(bank_lsx([(BANK, NAME, 2)], folder))),
        ("Public/%s/Flags/%s.lsf" % (folder, GO), lsf(flag_lsx(GO, GO_NAME, "The player asked the dock guardian for the crossing."))),
    ]


def texts():
    return dict(TEXTS)
