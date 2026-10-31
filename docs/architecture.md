# How WOW-BG3 Passthrough works

Two real games run at once and talk through files and shared memory: Baldur's Gate 3, the game
you see and control, and a WoW 1.12.1 client that nobody sees. Neither is rewritten; each gets a
mod. The idea is the one SkyCraft used for Skyrim and Minecraft.

## The pieces

| Piece | Runs where | Code |
|---|---|---|
| BG3 with bg3le (Linux Script Extender) and WOW-BG3's patches | Steam, native Linux | `patches/bg3le` |
| The BG3 mod: world mode, stand-ins, fights, party, saves, inventory, UI | inside BG3 (Lua) | `mod/` |
| Native plugins loaded by bg3le from the mod's folder | inside BG3 | `plugins/` |
| benilla, a WoW 1.12.1 client in Rust, with WOW-BG3's patches, offscreen | child of BG3 | `patches/benilla` |
| vmangos (WoW server) and MariaDB, local only | child of BG3 | `server/`, `patches/vmangos` |
| The coordinator and its tools (Python) | child of BG3 | `tools/` |
| The fight bridge (Rust) | started by the coordinator | `bridge/` |

The **launcher plugin** (`plugins/launcher`) starts when bg3le loads it. Once WOW-BG3 is enabled in
the mod manager it starts the server and the coordinator (`release/launcher.conf.in`) and stops them
when BG3 closes. It also holds BG3's loading screen (with bg3le patch 0004) until the WoW side
reports the world is ready, so a new game or a load never shows BG3's own level.

The **coordinator** (`tools/wowbg3-coord.py`) follows BG3's sessions: on a new game it makes a WoW
character to match the BG3 character (race and class mapped to a legal vanilla pair), on every load
it restores the WoW characters from that save's snapshot, on every save it takes a new snapshot
(`tools/lifecycle.py`, `tools/wowsave.py`). It runs benilla offscreen and the bridges below.

## World mode

- BG3's character lives in an empty part of a BG3 level, on a **treadmill**: square pieces of BG3's
  walkable grid are moved under the character as it walks and given WoW's ground (heights, walls,
  triangles for rays) from the server's map data (`tools/world_bridge.py`, `tools/wow_arena.py`,
  `tools/wow_vmap.py`; the main plugin moves the squares and answers BG3's rays).
- **BG3 leads.** BG3's camera and the character's position go to benilla every frame; benilla
  moves the WoW player and its camera to match and renders WoW's world offscreen.
- **The picture.** The render plugin (`plugins/render`, sharing `plugins/capture`) hooks BG3's
  Vulkan calls through BG3's own function table and composites WoW's frame behind BG3's characters,
  with depth, so BG3's party stands in WoW's world. The BG3 character is also drawn inside WoW with
  WoW's light, and BG3's spell effects and ground surfaces are replayed on WoW's ground.
- **Mobs and NPCs** stay WoW's. Near the party they are mirrored into BG3 as invisible stand-ins
  (`mod/ScriptExtender/Lua/Mirrors.lua`, `bridge/src/mirror.rs`) so BG3 can see, target and fight
  them. Clicks on WoW's people go to WoW (talk, vendor, quest, loot).
- **Party members** are WoW characters too (bots on the server), each with its own treadmill.

## Fights

1. A WoW mob turns hostile near the party. The server (patch series in `patches/vmangos`) holds the
   mob and its pack before the first swing.
2. BG3 starts a fight against the stand-ins: turn order, movement, rolls, spells and AI are BG3's.
   WoW plays the matching animations.
3. Damage, deaths and fleeing go back through the fight bridge (`bridge/`) to the server as real
   WoW events, so XP, loot and quest credit are WoW's own, once.
4. When BG3's fight ends, the server lets the mobs go.

## The contract between the two sides

The BG3 side (Lua mod, plugins, the BG3 half of the bridges) does not depend on the WoW version. It
reads and writes only a fixed set of channels: ground squares and collision, world origin and moves,
camera and pose, WoW's frame, figures and effects to draw, nearby mobs, fight handoff and credit,
character snapshots, inventory and WoW level. Each file on those channels carries a layout version
(binary files start with a 4 byte magic whose last character is the version, for example `WBC1`).
Everything version specific (client patches, map readers, server patches) stays behind that line, so
another WoW backend could implement the same channels later.

## Folders at runtime

- `Mods/WOW-BG3/` (the install): plugins, `launcher.conf`, `settings.conf`, `server/`, `benilla/`,
  `tools/`, `python/`, `bg3le/`, `WoW/` (the player's client), `LICENSES/`.
- `~/.local/share/wow-bg3/`: server data and database, snapshots, logs, status files.
- BG3's `Script Extender/wow-bg3/`: the link files between the mod and the WoW side.
