# WOW-BG3 Passthrough

WOW-BG3 0.1.0: https://github.com/sebastianlofi/wow-bg3-passthrough

Play vanilla World of Warcraft (1.12.1) as the world of a new Baldur's Gate 3 game.
Your BG3 characters walk WoW's world; fights against WoW's mobs are BG3 fights;
BG3 saves keep the WoW side too.

Linux only, native BG3 from Steam (not the Windows version under Proton).
Tested on AMD graphics (RADV) only so far.

**Photosensitivity and epilepsy warning.** This is a work-in-progress mod. When the passthrough
between WoW and BG3 glitches, the screen can flash suddenly, including bright white flashes. If
you or anyone watching has photosensitive epilepsy or is sensitive to flashing lights, be careful
playing it. Stop right away if you feel unwell.

## What you need

- Baldur's Gate 3, native Linux build, from Steam.
- Your own World of Warcraft 1.12.1 client (build 5875, English).
  WOW-BG3 contains no Blizzard files. It reads your client's `Data/` folder
  to build what the local WoW server needs, on your machine.
- About 10 GB free disk space for the generated server data.

Everything else is in this download: the Linux Script Extender (bg3le, with
WOW-BG3's patches), the local WoW server, the WoW engine (benilla) and the tools.

Strongly recommended, not included: a level mod, because BG3 stops at level 12 without one. From Nexus
Mods: UnlockLevelCurve or Expansion Level 13-20, then 5e Spells with its spell list patch, and Advanced
Tabletop Spells for more level 7 to 9 spells. Only UnlockLevelCurve has been tried with WOW-BG3, to level 14. 5e Spells needs Script
Extender, and whether it works on the Linux Script Extender in this download is unknown.

## Install

1. Open BG3's Mods folder:
   `~/.local/share/Larian Studios/Baldur's Gate 3/Mods`
2. Copy `WOW-BG3.pak` and the `WOW-BG3` folder from this download into it.
   They sit side by side: `Mods/WOW-BG3.pak` and `Mods/WOW-BG3/`.
3. Copy your WoW 1.12.1 client files into `Mods/WOW-BG3/WoW/`,
   so that `Mods/WOW-BG3/WoW/Data/` exists.
4. Install the Script Extender, once (Steam closed; it offers to close Steam):

   ```
   python3 ~/.local/share/"Larian Studios/Baldur's Gate 3/Mods/WOW-BG3/bg3le/install.py"
   ```

   It copies bg3le to `~/.local/share/bg3le` and adds it to BG3's Steam launch
   options, the same way bg3le's own installer does. If you already use bg3le,
   this replaces its library with WOW-BG3's build (bg3le plus our patches: it
   still runs your other Script Extender mods). Undo: the same command with
   `--uninstall`.

   It works the other way too: installing or updating stock bg3le after this
   puts its own library back in `~/.local/share/bg3le` and WOW-BG3 stops
   working. Run the command above again to put WOW-BG3's build back.

## Play

1. Start BG3 from Steam.
2. Mod Manager: enable WOW-BG3.
   The local WoW server starts in the background now; nothing is online.
   The very first time it builds its data from your WoW files (a few minutes);
   a New Game started meanwhile keeps its loading screen up until it is done.
3. New Game. Make or pick a character and start.
   The loading screen stays up until the WoW side is ready, then you are in
   the WoW start zone of your character's race:

| BG3 race | Origin characters | WoW race | Faction | Start zone |
|---|---|---|---|---|
| Human | Gale, Wyll | Human | Alliance | Northshire (Elwynn Forest) |
| Half-Elf | Shadowheart | Human | Alliance | Northshire (Elwynn Forest) |
| Halfling | | Human | Alliance | Northshire (Elwynn Forest) |
| Elf | Astarion | Night Elf | Alliance | Shadowglen (Teldrassil) |
| Dwarf | | Dwarf | Alliance | Coldridge Valley (Dun Morogh) |
| Gnome | | Gnome | Alliance | Coldridge Valley (Dun Morogh) |
| Half-Orc | | Orc | Horde | Valley of Trials (Durotar) |
| Githyanki | Lae'zel | Troll | Horde | Valley of Trials (Durotar) |
| Tiefling | Karlach | Undead | Horde | Deathknell (Tirisfal Glades) |
| Drow | | Undead | Horde | Deathknell (Tirisfal Glades) |
| Dragonborn | | Tauren | Horde | Camp Narache (Mulgore) |

The Dark Urge goes by the race you pick for them.

The first start takes a few minutes and needs the internet once: the WoW
server's data and the first movement maps are built from your WoW files, and
its world database is downloaded (about 48 MB, from vmangos' public sources:
WOW-BG3 ships no game data), while BG3's loading screen waits. Offline, a
message says so; start BG3 again once connected. Mobs use their WoW abilities
from the second start on. Later starts and loads take under a minute.

To change which abilities a mob gets, see "Changing mob abilities" in the
project's README: a file of yours in ~/.local/share/wow-bg3/abilities-rules.json.

World mode only happens in games started with WOW-BG3 enabled.
Saves you made before installing it stay normal BG3; loading one with WOW-BG3
enabled says so in a message box (OK to close, the game goes on as plain BG3).

Saving and loading work as in BG3. Loading an older save also takes the WoW
character back to where it was in that save.

## Levels

WoW levels run to 60, the end goal. The level BG3 shows beside a character is that character's own BG3
level, not your WoW level. WoW levels 2, 3 and 4 each open a party place; from WoW level 5 each level earns
one BG3 level-up for one party member, and every 5th earns two. BG3's XP bar shows how far you are into your
WoW level, and a notice at each new WoW level says the rest.

Your characters do not all level together: one WoW level is one level-up for one character of your choice,
not a level for the whole party. Going from BG3 level 3 to 4 across a party of four takes four level-ups.

| WoW level | Level-ups earned | A party of four at BG3 level |
|---|---|---|
| 5 | 2 | 1 to 2 |
| 10 | 8 | 3 |
| 20 | 20 | 6 |
| 30 | 32 | 9 |
| 40 | 44 | 12 |
| 50 | 56 | 15 |
| 57 | 64 | 17 |
| 60 | 68 | 18 |

BG3 stops at level 12 without a level mod (see "What you need"), which a party of four reaches around WoW 40;
later level-ups wait in the pool. The goal is to unlock the levels past 12 for all four, to match WoW up to 60,
and to add new spells for those new levels. That is still to come.

## Where things are

- Server data, database, save snapshots, logs: `~/.local/share/wow-bg3/`
- Logs of the WoW side: `~/.local/share/wow-bg3/logs/`
- bg3le's own logs: `~/.local/share/bg3le/logs/`
- What the plugin is doing: `~/.local/share/wow-bg3/run/launcher.json`
- What the WoW side is doing: `~/.local/share/wow-bg3/logs/coord.log`
- Settings: `Mods/WOW-BG3/settings.conf` (WoW's sound, view distance, the test
  commands of `/wowbg3` in WoW's chat). Read when BG3 starts.
  This beta ships `debug_commands = 1`, so every `/wowbg3` command typed in
  WoW's chat works, the test ones included: `refill`, `tp`, `where`, `find`,
  `faction`, `time`, `level`, `xprate`, `learn` and `unlearn`. Set it to 0 to leave only `help`, the party
  commands (`list`, `add`, `remove`, `pool`, `respec`), `waypoint` and `chat`.

## Remove

Disable WOW-BG3 in the Mod Manager, then delete `Mods/WOW-BG3.pak` and
`Mods/WOW-BG3/`. To drop the WoW server data and snapshots too, delete
`~/.local/share/wow-bg3/`. World-mode saves need the mod to load properly.

## When something goes wrong

- The loading screen waits at most two minutes for the WoW side. If it gives
  up, look at `~/.local/share/wow-bg3/logs/` and bg3le's newest log.
- Report a bug with those logs and `launcher.json` from the `run` folder.
- The WoW side runs where Steam runs BG3, with the Python that comes in
  `WOW-BG3/python/`. Nothing else to install.
- Nothing happens any more after you installed or updated stock bg3le (its own
  installer, or a mod manager that ships it): it replaced WOW-BG3's patched
  build. Run WOW-BG3's `bg3le/install.py` again, with Steam closed.

## Licences

WOW-BG3's own code: see `LICENSE` in the WOW-BG3 repository. The programs WOW-BG3
bundles keep their own licences: the WoW server (vmangos, GPL-2.0), its database
(MariaDB, GPL-2.0), the WoW client engine (benilla, MIT or Apache-2.0), bg3le (MIT;
its Script Extender part is MIT with the Commons Clause, so it may not be sold),
Python with its packages, and the libraries they use. `THIRD-PARTY-NOTICES.md` says
which is which; the full texts are in `WOW-BG3/LICENSES/`. The source of every GPL,
LGPL and MPL part is in `WOW-BG3-0.1.0-sources.tar.gz` next to this download.
World of Warcraft and Baldur's Gate 3 belong to their owners; this mod is not made
or endorsed by Blizzard or Larian.
