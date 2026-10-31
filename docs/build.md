# Building WOW-BG3 Passthrough from source

Everything in the release zip is built from this repository plus pinned upstream sources. Every
binary is built against Steam Runtime 3 ("sniper", glibc 2.31), the runtime Steam starts native BG3
in, so the release runs on any distribution Steam supports.

## What you need

- Linux x86_64 with about 60 GB free for sources, toolchains and build trees.
- clang 22.1.8 (the build scripts check the version), `bsdtar`, `lua`, `zstd`, `rsync`, `git`, `curl`.
- Docker: the sniper SDK image (`patches/bg3le/sdk-image.lock`) provides the sysroot and builds the
  server.
- rustup with toolchain 1.98.1 (`CARGO_HOME` and `RUSTUP_HOME` default to the build folder).
- Python 3.11 or newer.

## Settings

`release/build-env.sh` holds the settings every build script shares. Set any of them in the
environment to change it:

| Setting | Default | What |
|---|---|---|
| `WOWBG3_BUILD` | `build/` in the repository | builds, downloads, toolchains |
| `WOWBG3_SNIPER` | `$WOWBG3_BUILD/bg3le-sniper` | sniper sysroot, libc++, abseil and `libbg3le.so` |
| `BG3LE_SRC` | `$WOWBG3_BUILD/src/bg3le` | bg3le at the pinned commit with `patches/bg3le` applied |
| `BENILLA_SRC` | `$WOWBG3_BUILD/src/benilla` | benilla at the pinned commit with `patches/benilla` applied |
| `CARGO_HOME`, `RUSTUP_HOME` | `$WOWBG3_BUILD/cargo-home`, `rustup-home` | Rust toolchain |
| `MARIADB_TAR` | `$WOWBG3_BUILD/dl/mariadb-11.4.8-...tar.gz` | the pinned MariaDB bintar |
| `WOWBG3_SERVER_BUILD` | `$WOWBG3_BUILD/portable-build` | the server build and pack (`server/pins.sh`) |
| `CARGO_BUILD_JOBS` | 2 | parallel rustc jobs (one can use 7 GB) |

Missing upstream checkouts are cloned at the pinned commits; existing ones are checked against them.

## Everything at once

```
release/build-all.sh 0.1.0
```

It runs the steps below in order, skipping heavy parts that already exist (`REBUILD=1` redoes them),
and writes `$WOWBG3_BUILD/dist/WOW-BG3-0.1.0.zip` and `WOW-BG3-0.1.0-sources.tar.gz`. Publish both:
the sources archive carries the source of every GPL, LGPL and MPL part in the zip.

## The steps

| Step | Script | Output (under `$WOWBG3_BUILD`) | Notes |
|---|---|---|---|
| bg3le | `patches/bg3le/build.sh` | `bg3le-sniper/bg3le/libbg3le.so` | also makes the sysroot and libc++ the plugins use, every object through `repro-cc.sh`; its sha256 should equal `library_sha256` in `patches/bg3le/build.lock.json` |
| launcher plugin | `plugins/launcher/build.sh` | `wowbg3-launcher/wowbg3-launcher.so` | tests without BG3: `plugins/launcher/test.sh` |
| main plugin | `plugins/build.sh` | `wowbg3-plugin/wowbg3.so` | |
| render plugin | `plugins/render/build.sh` | `wowbg3-render/wowbg3_render.so` | |
| quest cursor plugin | `plugins/early/build.sh` | `wowbg3-early/wowbg3-early.so` | |
| fight bridge | `bridge/build.sh` | `link-target-sniper/release/wow-bg3-link` | tests: `cargo +1.98.1 test --locked` in `bridge/` |
| benilla | `patches/benilla/build.sh` | `benilla-target-sniper-player/release/benilla` | the longest step (a large Rust and Bevy build) |
| server | `server/build-portable.sh`, then `server/pack-portable.sh` | `portable-build/wowbg3-server/` | vmangos core with `patches/vmangos`, MariaDB 11.4.8, vmangos' database SQL |
| Python | `release/python/fetch.sh`, then `pack.sh` | `release/python/py/` | python-build-standalone 3.12 with numpy, Pillow, lz4, zstandard |
| FastLZ | `tools/fastlz/build.sh` | `release/libfastlz.so` | |
| licences | `release/licences/collect.py fetch texts sources` | `LICENSES/` (in the repository), `licences/sources/` | run `texts` again whenever a bundled part changes |
| zip | `release/build-release.py --version V --out DIR ...` | `DIR/WOW-BG3-V.zip`, `-sources.tar.gz` | see `build-release.py --help`; without `--sources` it is a test build |

`build-release.py` takes the built parts as arguments and adds the mod (packed from `mod/`), the
launcher configuration and settings (`release/`), the tools (`tools/`), the WoW addon
(`wow-addon/`), the server start script (`server/wowbg3-server.sh`), bg3le's installer
(`release/bg3le-install.py`) and the licence texts. `MANIFEST.txt` in the zip lists the sha256 of
every file.

## Reproducibility

Rebuilt from this repository with the same toolchain and dependency folders, the launcher, main and
quest cursor plugins, FastLZ and the fight bridge came out byte for byte the same as in the 0.1.0
release. No folder of the building machine ends up in a release file: `patches/bg3le/build.sh`
compiles libc++, abseil, protobuf and bg3le through `repro-cc.sh` (`-ffile-prefix-map`), so
their `__FILE__` strings name `/bg3le/...` and `/bg3le-sniper/...`, and with `SOURCE_DATE_EPOCH`
at the bg3le commit's time `libbg3le.so` comes out byte for byte the same in any build folder at any
time (the main plugin links that libc++); the render plugin's debug information
names its folders relatively (`-ffile-prefix-map`); the Rust builds remap `CARGO_HOME`, `RUSTUP_HOME`
and their source folder (`--remap-path-prefix`). The mod pack, the tools and the configuration files
are copied as they are.
