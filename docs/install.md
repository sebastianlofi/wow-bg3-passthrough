# Installing WOW-BG3 Passthrough

The release zip has its own README with the same steps; this page adds what is behind them.

## What you need

- Linux, x86_64, with the native Linux build of Baldur's Gate 3 from Steam (not the Windows build
  under Proton). Tested on AMD graphics (Mesa RADV) so far.
- Your own World of Warcraft 1.12.1 client, build 5875, English. WOW-BG3 never runs `WoW.exe`; it
  reads the client's `Data/` folder to build the local server's data and the textures it needs.
- About 10 GB of free disk space for the generated server data.
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
5. Start BG3 from Steam, enable WOW-BG3 in the mod manager, start a New Game and make or pick a
   character. The loading screen stays up until the WoW side is ready; then you stand in the WoW start
   zone of your character's race.

The very first start builds the server's data from your WoW files and the first movement maps, and
downloads the server's world database once (about 48 MB from vmangos' public sources, checked against the
hashes in `server/sql.lock`; kept in `~/.local/share/wow-bg3/server/downloads`): about three minutes behind
the loading screen. It needs the internet that once; offline it stops with a message, and the next start
tries again. The mobs' abilities are made from that database and your WoW files on the same start, and BG3
uses them from its next start. Later starts and loads take under a minute and need no internet.

## What happens on your machine

- When WOW-BG3 is enabled, its launcher plugin starts two programs next to BG3, inside Steam's
  runtime container: the local WoW server (`server/wowbg3-server.sh run`) and the coordinator
  (`tools/wowbg3-coord.py`). Both stop when BG3 closes. Nothing listens outside your own machine.
- The coordinator makes a local WoW account and, per BG3 playthrough, a hidden WoW character that
  matches your BG3 character's race and class. You never log in to anything.
- Data lives in `~/.local/share/wow-bg3/`: the server's data and database, the WoW side of your
  saves (snapshots), logs (`logs/`) and status files (`run/launcher.json`, `run/coord.json`: during the first
  start its `Detail` says the server's step, such as the download's progress, and `Error` why it failed).
- Settings: `Mods/WOW-BG3/settings.conf` (WoW's sound, view distance, test chat commands). Read when
  BG3 starts. This beta ships `debug_commands = 1`, so every `/wowbg3` command typed in WoW's chat
  works, the test ones included (`refill`, `tp`, `where`, `find`, `faction`, `level`, `xprate`, `learn`, `unlearn`); `0`
  leaves only `help`, the party commands (`list`, `add`, `remove`, `pool`, `respec`) and `chat`.

## Saves

World mode only happens in games started with WOW-BG3 enabled; older saves stay plain BG3 (loading
one says so in a message box). Saving and loading work as in BG3: each BG3 save keeps a snapshot of
the WoW characters, and loading an older save takes them back to that point too.

## Removing it

Disable WOW-BG3 in the mod manager, then delete `Mods/WOW-BG3.pak` and `Mods/WOW-BG3/`. Delete
`~/.local/share/wow-bg3/` to drop the server data and snapshots as well. Run the Script Extender
installer with `--uninstall` if you no longer want bg3le. World-mode saves need the mod to load
properly.

## When something goes wrong

- The loading screen waits at most a few minutes for the WoW side. If it gives up, look at
  `~/.local/share/wow-bg3/logs/` (`coord.log`, `server.log`, `benilla.log`) and bg3le's newest log
  in `~/.local/share/bg3le/logs/`.
- When reporting a bug, attach those logs and `~/.local/share/wow-bg3/run/launcher.json`.
- Nothing works any more after a stock bg3le install or update: it replaced WOW-BG3's patched
  library in `~/.local/share/bg3le`. Run WOW-BG3's `bg3le/install.py` again with Steam closed.
- Known issues of this version: [known-issues.md](known-issues.md).
