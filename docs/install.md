# Installing WOW-BG3 Passthrough

The release zip has its own README with the same steps; this page adds what is behind them.

## What you need

- Linux, x86_64, with the native Linux build of Baldur's Gate 3 from Steam (not the Windows build
  under Proton). Tested on AMD graphics (Mesa RADV 25.0 and 26.2) and on Nvidia (RTX 4070 Ti, driver 595); Intel is untested.
- Your own World of Warcraft 1.12.1 client, build 5875, English. WOW-BG3 never runs `WoW.exe`; it
  reads the client's `Data/` folder to build the local server's data and the textures it needs.
- About 15 GB of free disk space: your WoW client copy (about 5 GB) and up to about 10 GB the mod builds from it.
- An internet connection for the first start only (a 48 MB download, see below).
- Nothing else: the release brings the Linux Script Extender (bg3le with WOW-BG3's patches), the
  local WoW server (vmangos and MariaDB), the WoW engine (benilla), a Python for its tools and the
  licence texts of all of them.

## Steps

1. Download `WOW-BG3-0.1.0.zip` from the [Releases page](https://github.com/sebastianlofi/wow-bg3-passthrough/releases) and unpack it anywhere.
2. Copy `WOW-BG3.pak` and the `WOW-BG3/` folder into BG3's Mods folder,
   `~/.local/share/Larian Studios/Baldur's Gate 3/Mods/`, side by side.
3. Copy your WoW 1.12.1 client into `Mods/WOW-BG3/WoW/`, so that `Mods/WOW-BG3/WoW/Data/` exists.
4. With Steam closed, install the Script Extender once:

   ```
   python3 ~/.local/share/"Larian Studios/Baldur's Gate 3/Mods/WOW-BG3/bg3le/install.py"
   ```

   It copies bg3le to `~/.local/share/bg3le` and adds its launch wrapper to BG3's Steam launch
   options, as bg3le's own installer does. If you already use bg3le, its library is replaced by
   WOW-BG3's build; other Script Extender mods keep working. `--uninstall` undoes it, `--dry-run`
   shows what it would do. The same holds in reverse: installing or updating stock bg3le later
   (its own installer, or a mod manager that brings it) puts the stock library back over
   WOW-BG3's patched build, and WOW-BG3 stops working until you run this command again.
5. Start BG3 from Steam, enable WoW-BG3 Passthrough in the mod manager, start a New Game and make or pick a
   character. The loading screen stays up until the WoW side is ready; then you stand in the WoW start
   zone of your character's race.

The very first start builds the server's data from your WoW files and the first movement maps, and
downloads the server's world database once (about 48 MB from vmangos' public sources, checked against the
hashes in `server/sql.lock`; kept in `Mods/WOW-BG3/data/server/downloads`): about three minutes behind
the loading screen. It needs the internet that once; offline it stops with a message, and the next start
tries again. The mobs' abilities are made from that database and your WoW files before BG3 reads its stats
(bg3le patch 0008), so the first time WOW-BG3 runs BG3 stays on its loading screen for about two minutes
before the main menu. Later starts and loads take under a minute and need no internet.

Why the download: the WoW server needs a world database (every NPC, quest, item, vendor and spawn).
It is Blizzard-derived, so this mod can't ship it, and your WoW client doesn't contain it, so it can't
be built from your client either. The first start fetches it from vmangos' public sources (the WoW
server this mod uses), at a pinned version, and checks every file against the checksums that ship with
the mod (`server/sql.lock`) before using it. Nothing else is downloaded, and nothing is sent anywhere.

## What happens on your machine

- When WOW-BG3 is enabled, its launcher plugin starts two programs next to BG3, inside Steam's
  runtime container: the local WoW server (`server/wowbg3-server.sh run`) and the coordinator
  (`tools/wowbg3-coord.py`). Both stop when BG3 closes. Nothing listens outside your own machine: the WoW server
  answers on 127.0.0.1 only, its database has no port, and the Script Extender's Lua debugger (127.0.0.1:9998 in
  stock bg3le) stays closed in WOW-BG3's build unless you turn it on (`BG3LE_DEBUG=1`).
- The coordinator makes a local WoW account and, per BG3 playthrough, a hidden WoW character that
  matches your BG3 character's race and class. You never log in to anything.
- Data lives in the mod's own folder, `Mods/WOW-BG3/data/` (nothing outside BG3's Mods folder): the server's data and database, the WoW side of your
  saves (snapshots), logs (`logs/`) and status files (`run/launcher.json`, `run/coord.json`: during the first
  start its `Detail` says the server's step, such as the download's progress, and `Error` why it failed).
- Settings: `Mods/WOW-BG3/settings.conf` (WoW's sound, view distance, test chat commands). Read when
  BG3 starts. This beta ships `debug_commands = 1`, so every `/wowbg3` command typed in WoW's chat
  works, the test ones included (`refill`, `tp`, `where`, `find`, `faction`, `level`, `xprate`, `learn`, `unlearn`); `0`
  leaves only `help`, the party commands (`list`, `add`, `remove`, `pool`, `respec`) and `chat`.

## Saves

World mode only happens in games started with WOW-BG3 enabled; older saves stay plain BG3 (loading
one says so in a message box). Saving and loading work as in BG3: each BG3 save keeps a snapshot of
the WoW characters, and loading an older save takes them back to that point too. A new server
database (a reinstall that dropped `Mods/WOW-BG3/data/server/` but kept the snapshots) keeps
them as well: when the server starts, it puts each saved WoW character back from its snapshot.

## Removing it

In this order:

1. Disable WoW-BG3 Passthrough in the mod manager and close BG3.
2. Take the Script Extender out, with Steam closed:

   ```
   python3 ~/.local/share/"Larian Studios/Baldur's Gate 3/Mods/WOW-BG3/bg3le/install.py" --uninstall
   ```

   It removes bg3le from BG3's Steam launch options and deletes `~/.local/share/bg3le`. Do this before step 3: the
   installer is inside `Mods/WOW-BG3/`. Skip it only if other mods of yours need bg3le; then install stock bg3le
   again, since the library in `~/.local/share/bg3le` is WOW-BG3's build.
3. Delete `Mods/WOW-BG3.pak` and `Mods/WOW-BG3/` (its `data/` folder holds the server data and snapshots; keep a
   copy of it if you may come back).

Left behind: `localconfig.vdf.bg3le-backup` beside Steam's `localconfig.vdf` (the copy made before the launch
option was set; delete it if you like) and `/dev/shm/wow-bg3-<your user id>`, gone at the next reboot. World-mode
saves need the mod to load properly.

If you deleted the folder first: BG3 still starts through bg3le. Remove the `bg3le-launch` part from BG3's launch
options in Steam (Properties, General) and delete `~/.local/share/bg3le`.

## When something goes wrong

- The loading screen waits two minutes for the WoW side, and up to fifteen more while the first start is still
  building its data. If it gives up, look at
  `Mods/WOW-BG3/data/logs/` (`coord.log`, `server.log`, `benilla.log`) and bg3le's newest log
  in `~/.local/share/bg3le/logs/`.
- When reporting a bug, attach those logs and `Mods/WOW-BG3/data/run/launcher.json`.
- Characters are black with a pale ring round them, or the picture has a grey haze: the render plugin has no
  colour table. Look for `NO COLOUR TABLE` in `Mods/WOW-BG3/data/capture/layer.log` and in
  `Mods/WOW-BG3/data/logs/coord.log`. `Mods/WOW-BG3/fxbg-table.bin` must be there (it is part of the zip); the
  mod copies it into `Mods/WOW-BG3/data/capture/` at the next start.
- Nothing works any more after a stock bg3le install or update: it replaced WOW-BG3's patched
  library in `~/.local/share/bg3le`. Run WOW-BG3's `bg3le/install.py` again with Steam closed.
- Known issues of this version: [known-issues.md](known-issues.md).
