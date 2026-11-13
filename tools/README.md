# Tools

The Python tools the release runs on the player's machine, started by the coordinator inside
Steam's runtime container. They need Python 3.9 or newer; Tav's textures, the item icons and the
quest cursors also need numpy, Pillow, lz4 and zstandard (the release ships a Python with them).

`release/build-release.py` places them in the zip as:

| Here | In the release |
|---|---|
| `wowbg3-coord.py` | `WOW-BG3/tools/wowbg3-coord.py` (the coordinator, run by the launcher plugin) |
| `world-console.py` | `WOW-BG3/tools/server/tools/world-console.py` (the server console words) |
| `relenv.py` and every other `.py`, `wow_items_rules.json` | `WOW-BG3/tools/link/tools/` |
| `fastlz/` | built into `WOW-BG3/tools/link/tools/libfastlz.so` (`fastlz/build.sh`) |

`relenv.py` points the tools at the player's folders from the environment the launcher plugin
gives every child (`WOWBG3_HOME`, `WOWBG3_DATA`, `WOWBG3_IO`, `WOWBG3_SHM`, `WOWBG3_WOW`); see its
header. The tools' own folder constants are development defaults that `relenv.py` replaces.

| Tool | What |
|---|---|
| `lifecycle.py` | follows BG3's sessions: new game, load, save, death; WoW characters and snapshots |
| `wowsave.py`, `wowchar.py`, `wowaccount.py`, `wowserver.py` | WoW character snapshots, BG3 to WoW character mapping, playthrough accounts, the server's console and database |
| `devserver.py` | the developers' Docker test server behind `wowserver.py` when `WOW_BG3_SERVER` is unset; source tree only, no release carries it |
| `test_shm_dir.py` | the folder WoW's frames go through is one of this user's own (`plugins/wowbg3_shm.h`, the one rule of the launcher and the render code; needs a C compiler) |
| `test_release_contents.py` | what a release carries and leaves out: no developer-only code in the shipped tools, the python and server packs without what no install uses, no links, bg3le's debugger off, the crash report's masking, the removal texts |
| `world_bridge.py`, `wow_arena.py`, `wow_stand.py`, `wow_vmap.py`, `wow_mpq.py` | world mode's ground squares from the server's map data |
| `wow_places.py` | where each WoW map lies in the one BG3 level: three floors (Eastern Kingdoms, Kalimdor, every other map) |
| `wow_docks.py` | WoW's ships and zeppelins as crossings between docks, from the server's game objects and the client's taxi paths; where to stand at a dock (`/wowbg3 cross`); `test_wow_docks.py` |
| `wow_lifts.py` | WoW's elevators and tram cars: the client's own timetable (position, stops, journeys), each car's floor from its collision model, benilla's live `wowlifts.bin`, the static ground a resting car takes away |
| `wow_lift_platforms.py` | the platforms the BG3 level keeps for WoW's lifts: round floors of several sizes, where they are parked, their ids (the table in the mod's `Lifts.lua`) |
| `inventory_bridge.py`, `wow_items.py`, `wow_icons.py` | the merged inventory, item data and the icon atlas |
| `wow_abilities.py`, `wow_abilities_rules.json` | the BG3 abilities each WoW mob's stand-in gets (link/abilities.json, and the stats files BG3's loader reads); `localseed` makes this PC's seed from the server's database once it is imported, from which every later start builds them (when the rules or a player's overlay change) with no database; nothing of it is shipped |
| `test_camp.py` | camp trips and spent supplies under a fake game (needs the `lupa` module) |
| `wowlevel.py` | the hero's WoW level for BG3's progression |
| `wowbg3-chatcmd.py`, `wowbg3cmd.py` | `/wowbg3` commands typed in WoW's chat |
| `test_world_move.py` | a move on the same continent (`World.reorigin`, stale squares, the cuts made ahead) without a game |
| `test_standin_aim.py` | the stand-in under WoW's model (`StandInAim.lua`, `plugins/wowbg3_standin_aim.h`) without a game (needs `lupa` and a C++ compiler) |
| `wow_waypoints.py` | fast travel between WoW's flight points: the rules (`waypoints` in `settings.conf`); `test_wow_waypoints.py` checks them without a game; `prewarm` also cuts the squares a move lands on, into the player's own data folder (`WOW_CUT_CACHE`: Blizzard-derived, never shipped) |
| `tavtex.py`, `bg3tex.py`, `bg3pak.py`, `bg3shade.py`, `bg3index.py` | textures for BG3's characters drawn inside WoW |
| `wowcursor.py` | WoW's quest cursors for BG3 |
