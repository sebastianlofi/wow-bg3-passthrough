#!/usr/bin/env python3
"""wowchar.py: the hidden WoW character for a BG3 hero (plug-and-play).

Race decides start zone and side, class follows the BG3 class where vanilla allows the pair.
Mapping (the plug-and-play-plan notes, "More decisions"):
  Human, Half-Elf -> Human     Elf -> Night Elf     Dwarf -> Dwarf     Gnome, Halfling -> Gnome (the maintainer, 2026-11-14)
  Half-Orc -> Orc      Githyanki -> Troll      Tiefling, Drow -> Undead      Dragonborn -> Tauren
  Barbarian, Fighter, Monk -> Warrior; Ranger -> Hunter; Rogue, Bard -> Rogue; Cleric -> Priest;
  Wizard, Sorcerer -> Mage; Warlock -> Warlock; Druid -> Druid; Paladin -> Paladin;
  an illegal pair falls back down the class's list (ending in Warrior, legal for every race).

  wowchar.py [HERO_JSON]        print the mapping as JSON (default: the private BG3's hero.json)
  wowchar.py --spec [HERO_JSON] print benilla's WOW_BG3_CREATE spec (patch 0048): name,race,class,gender
  wowchar.py --test             check every BG3 race x class gives a legal vanilla pair
Reads only. link/tools/lifecycle.py uses mapping(), free_name() and bot_name() for every party member's WoW
character (the hero at a new game's first world-mode start, companions as they join).
"""
import json, re, sys
from pathlib import Path

HERO = Path(__file__).resolve().parents[2] / ("bg3/private/home/.local/share/Larian Studios/"
            "Baldur's Gate 3/Script Extender/wow-bg3/hero.json")

# WoW 1.12 ids (ChrRaces.dbc, ChrClasses.dbc)
RACE = {"Human": 1, "Orc": 2, "Dwarf": 3, "NightElf": 4, "Undead": 5, "Tauren": 6, "Gnome": 7, "Troll": 8}
CLASS = {"Warrior": 1, "Paladin": 2, "Hunter": 3, "Rogue": 4, "Priest": 5, "Shaman": 7, "Mage": 8,
         "Warlock": 9, "Druid": 11}
# vanilla's legal race/class pairs (CharStartOutfit / playercreateinfo)
LEGAL = {
    "Human": {"Warrior", "Paladin", "Rogue", "Priest", "Mage", "Warlock"},
    "Dwarf": {"Warrior", "Paladin", "Hunter", "Rogue", "Priest"},
    "NightElf": {"Warrior", "Hunter", "Rogue", "Priest", "Druid"},
    "Gnome": {"Warrior", "Rogue", "Mage", "Warlock"},
    "Orc": {"Warrior", "Hunter", "Rogue", "Shaman", "Warlock"},
    "Undead": {"Warrior", "Rogue", "Priest", "Mage", "Warlock"},
    "Tauren": {"Warrior", "Hunter", "Shaman", "Druid"},
    "Troll": {"Warrior", "Hunter", "Rogue", "Priest", "Shaman", "Mage"},
}
START = {"Human": "Northshire", "Dwarf": "Coldridge Valley", "Gnome": "Coldridge Valley",
         "NightElf": "Shadowglen (Teldrassil)", "Orc": "Valley of Trials", "Troll": "Valley of Trials",
         "Undead": "Deathknell", "Tauren": "Camp Narache (Mulgore)"}

# BG3 race (parent race name, letters only, lower case) -> WoW race
BG3_RACE = {"human": "Human", "halfelf": "Human", "halfling": "Gnome", "elf": "NightElf",
            "drow": "Undead", "dwarf": "Dwarf", "gnome": "Gnome", "halforc": "Orc",
            "githyanki": "Troll", "tiefling": "Undead", "dragonborn": "Tauren"}
# template prefixes BG3 uses for player bodies (fallback when the race component can't be read)
TEMPLATE_RACE = {"humans": "human", "elves": "elf", "halfelves": "halfelf", "drow": "drow",
                 "dwarves": "dwarf", "halflings": "halfling", "gnomes": "gnome", "tieflings": "tiefling",
                 "githyanki": "githyanki", "halforcs": "halforc", "dragonborn": "dragonborn"}
# BG3 class -> WoW classes in order of preference
BG3_CLASS = {
    "barbarian": ["Warrior"], "fighter": ["Warrior"], "monk": ["Warrior"],
    "ranger": ["Hunter", "Warrior"], "rogue": ["Rogue", "Warrior"], "bard": ["Rogue", "Warrior"],
    "cleric": ["Priest", "Shaman", "Warrior"], "wizard": ["Mage", "Warlock", "Warrior"],
    "sorcerer": ["Mage", "Warlock", "Warrior"], "warlock": ["Warlock", "Warrior"],
    "druid": ["Druid", "Shaman", "Priest", "Warrior"], "paladin": ["Paladin", "Shaman", "Warrior"],
}


BOT_ACCOUNT_PREFIX = "WB3B"  # server patch 0010, BOT_ACCOUNT_PREFIX


def key(s):
    return re.sub(r"[^a-z]", "", str(s or "").lower())


def wow_name(name):
    """A legal WoW name: letters only, first capital, 2..12 long."""
    letters = re.sub(r"[^A-Za-z]", "", str(name or ""))[:12]
    if len(letters) < 2:
        letters = "Tav"
    return letters[0].upper() + letters[1:].lower()


def names_from(base):
    """base as a legal WoW name, then with letters added (a, b, ... then aa, ab, ...)."""
    base = wow_name(base)
    yield base
    letters = "abcdefghijklmnopqrstuvwxyz"
    for n in (1, 2):
        stem = base[:12 - n]
        for i in range(len(letters) ** n):
            tail, k = "", i
            for _ in range(n):
                tail, k = letters[k % 26] + tail, k // 26
            yield stem + tail


def free_name(base, taken):
    """The first of names_from(base) that no name in taken (case-insensitive) has: a new game's hero gets
    a character of its own even when an older playthrough's hero had the same name."""
    low = {t.lower() for t in taken}
    for name in names_from(base):
        if name.lower() not in low:
            return name
    raise ValueError("no free WoW name for %s" % base)


def is_bot_account(account):
    """Whether a WoW account is a bot's (server patch 0010: WB3B + character name). The server's `bot add`
    logs in an existing character only from such an account and answers `refused taken` for any other."""
    return str(account or "").upper().startswith(BOT_ACCOUNT_PREFIX)


def bot_name(base, taken, owners):
    """The WoW character for a companion called base: the first of names_from(base) that this party does
    not use (taken) and that is either a bot's character already or free. owners: {every character's name:
    its account}. A name on another account stays with its owner: a hero of an older playthrough called
    Gale keeps Gale, for that playthrough's saves, and companion Gale is Galea, again at every later load."""
    low = {t.lower() for t in taken}
    who = {str(n).lower(): a for n, a in owners.items()}
    for name in names_from(base):
        k = name.lower()
        if k not in low and (k not in who or is_bot_account(who[k])):
            return name
    raise ValueError("no free WoW name for %s" % base)


def mapping(hero):
    tmpl = str(hero.get("Template") or "")
    race_key = key(hero.get("RaceParent") or hero.get("Race"))
    if race_key not in BG3_RACE:  # a base race's parent is "Humanoid" (Gale): the race itself
        race_key = key(hero.get("Race"))
    if race_key not in BG3_RACE:
        race_key = TEMPLATE_RACE.get(key(tmpl.split("_")[0]), race_key)
    race = BG3_RACE.get(race_key, "Human")
    cls_pref = BG3_CLASS.get(key(hero.get("Class")), ["Warrior"])
    cls = next(c for c in cls_pref + ["Warrior"] if c in LEGAL[race])
    body = str(hero.get("BodyType") or "")
    female = "_Female_" in tmpl or body.lower() in {"female", "1"}
    return {
        "Name": wow_name(hero.get("Name")), "Race": race, "RaceId": RACE[race], "Class": cls,
        "ClassId": CLASS[cls], "Gender": 1 if female else 0, "Start": START[race],
        "Side": "Horde" if race in {"Orc", "Undead", "Tauren", "Troll"} else "Alliance",
        "From": {"Race": hero.get("Race"), "RaceParent": hero.get("RaceParent"),
                 "Class": hero.get("Class"), "Template": tmpl or None},
    }


def main(argv):
    if argv[:1] == ["--test"]:
        bad = []
        for r in BG3_RACE:
            for c in BG3_CLASS:
                m = mapping({"RaceParent": r, "Class": c, "Name": "Test"})
                if m["Class"] not in LEGAL[m["Race"]]:
                    bad.append((r, c))
        names = [free_name("Tav", []), free_name("Tav", ["tav"]), free_name("Shadowheart", ["Shadowheart"]),
                 free_name("Laezel", ["Laezel", "Laezela"]), free_name("Lae'zel", [])]
        if names != ["Tav", "Tava", "Shadowhearta", "Laezelb", "Laezel"]:
            bad.append(("free_name", names))
        bots = [bot_name("Gale", [], {}), bot_name("Gale", [], {"Gale": "WB3PAAAAAAAA"}),
                bot_name("Gale", [], {"Gale": "WB3PAAAAAAAA", "Galea": "WB3BGALEA"}),
                bot_name("Gale", [], {"Gale": "WB3PAAAAAAAA", "Galea": "WB3PBBBBBBBB"}),
                bot_name("Gale", ["Gale"], {"Gale": "WB3BGALE"})]
        if bots != ["Gale", "Galea", "Galea", "Galeb", "Galea"]:
            bad.append(("bot_name", bots))
        print("ok" if not bad else "illegal: %s" % bad)
        return 0 if not bad else 1
    spec = argv[:1] == ["--spec"]
    path = Path(argv[1 if spec else 0]) if len(argv) > (1 if spec else 0) else HERO
    m = mapping(json.loads(path.read_text()))
    if spec:
        print("%s,%d,%d,%d" % (m["Name"], m["RaceId"], m["ClassId"], m["Gender"]))
    else:
        print(json.dumps(m, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
