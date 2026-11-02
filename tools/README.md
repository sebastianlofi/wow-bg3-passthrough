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
| `world_bridge.py`, `wow_arena.py`, `wow_stand.py`, `wow_vmap.py`, `wow_mpq.py` | world mode's ground squares from the server's map data |
| `inventory_bridge.py`, `wow_items.py`, `wow_icons.py` | the merged inventory, item data and the icon atlas |
| `wow_abilities.py`, `wow_abilities_rules.json` | the BG3 abilities each WoW mob's stand-in gets (link/abilities.json, and the stats files BG3's loader reads); `seed` makes the release's abilities-seed.json.gz, from which a first start builds them with no database |
| `test_camp.py` | camp trips and spent supplies under a fake game (needs the `lupa` module) |
| `wowlevel.py` | the hero's WoW level for BG3's progression |
| `wowbg3-chatcmd.py`, `wowbg3cmd.py` | `/wowbg3` commands typed in WoW's chat |
| `test_world_move.py` | a move on the same continent (`World.reorigin`, stale squares, the cuts made ahead) without a game |
| `wow_waypoints.py` | fast travel between WoW's flight points: the rules (`waypoints` in `settings.conf`); `test_wow_waypoints.py` checks them without a game; `prewarm` also cuts the squares a move lands on, into the player's own data folder (`WOW_CUT_CACHE`: Blizzard-derived, never shipped) |
| `tavtex.py`, `bg3tex.py`, `bg3pak.py`, `bg3shade.py`, `bg3index.py` | textures for BG3's characters drawn inside WoW |
| `wowcursor.py` | WoW's quest cursors for BG3 |
