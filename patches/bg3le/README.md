# bg3le patches

WOW-BG3's patches to [bg3le](https://github.com/lenonk/bg3le), the Linux Script Extender for
Baldur's Gate 3, at commit `5c729b72296defd4a09556ee723b12b76a0d445d`.

| Patch | What it does |
|---|---|
| 0001-box-command-parameters | Noesis command parameters boxed from Lua scalars |
| 0002-harness-mouse-input | pointer warp and mouse button events through the SDL injection queue (test tooling) |
| 0003-plugin-state-gates-and-mod-plugins | native plugins from an enabled mod's folder (`NativePlugins` in the mod's Config.json), plugin state gates |
| 0004-prepare-running-hold-and-static-data | holds BG3's client in PrepareRunning (the loading screen stays up) until the WoW side is ready; static data fix |
| 0005-early-plugins | `EarlyNativePlugins`: plugins loaded before BG3 makes its Vulkan objects |
| 0006-boot-path-overrides | path overrides from a file, set before BG3 loads its stats: no mod's Lua runs that early (the mob abilities' spell copies) |
| 0007-loca-written-strings-stay-put | strings written through Ext.Loca stay where the index's views point: a vector growing left short handles' views dangling, and a later load crashed in `bg3le_loca_set` |
| 0008-first-start-stats-wait | on a new install's first start, BG3's stats load waits (at most 20 minutes) while the coordinator refreshes `boot-path-overrides.wait`: it is making the mobs' abilities that load reads, from the server's new database |
| 0009-sync-files-new-spells-for-the-ai | a spell made after the stats load (Create + Sync) also goes into `SpellPrototypeManager`'s `CombatAIOverrideSpells`, as the engine's load files every spell (its `CombatAIOverrideSpell` or itself): missing there, BG3's AI read a null and crashed on the holder's first turn |

`build.lock.json` pins every patch by sha256 with the file hashes before and after it, the compiler,
the SDK image and the sha256 of the resulting `libbg3le.so`. `externals.lock.json` pins bg3le's
third-party sources, `sdk-image.lock` the Steam Runtime 3 SDK image.

Build: `build.sh` (clone, `fetch-sources.py`, `apply-patches.py`, bg3le's checks,
`check-view-contract.py`, bg3le's own sniper build). Every object of that build (libc++, abseil,
protobuf, bg3le) is compiled through `repro-cc.sh`, which adds `-ffile-prefix-map` for the
source and build folders (no folder of the building machine ends up in `libbg3le.so` or in the
libc++ the plugins link) and sets `SOURCE_DATE_EPOCH` to the bg3le commit's time (bg3le's "built
on" line), so `libbg3le.so` is byte for byte the same wherever and whenever it is built. See [docs/build.md](../../docs/build.md).

## Licence

These patches are under bg3le's licence (MIT; the parts in bg3le's `vendor/bg3se` tree are MIT with
the Commons Clause). The scripts in this folder are WOW-BG3's (MIT, see the repository's LICENSE).
