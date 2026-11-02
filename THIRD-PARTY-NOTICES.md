# Third-party notices

WOW-BG3 contains programs and libraries made by other people. This file says which,
under which licence, and where their source is. The full licence texts are in
`WOW-BG3/LICENSES/` (one folder per program; `LICENSES/README.txt` is the index).

WOW-BG3 contains no files from World of Warcraft or Baldur's Gate 3. It reads the
player's own WoW 1.12.1 client and BG3 installation on the player's machine.
World of Warcraft is a trademark of Blizzard Entertainment, Inc. Baldur's Gate 3 is a
trademark of Larian Studios. WOW-BG3 is not made or endorsed by either.

**Sources archive.** `WOW-BG3-<version>-sources.tar.gz`, published next to each
release download, holds the source of every GPL, LGPL and MPL part listed below,
with WOW-BG3's patches and build scripts (see the `README.md` inside it).

## WOW-BG3 itself

- `WOW-BG3.pak` (Lua scripts), `wowbg3-launcher.so`, `wowbg3.so`, `wowbg3_render.so`,
  `tools/` (Python tools, `wowbg3-coord.py`), `benilla/AddOns/WowBg3Layout`,
  `server/wowbg3-server.sh`, `bg3le/install.py`, and WOW-BG3's patches to the programs below.
- Licence: the one in the WOW-BG3 repository's `LICENSE` file. WOW-BG3's patches to
  vmangos are distributed under vmangos' licence (GPL-2.0).
- `wowbg3.so` has LLVM's libc++ and libc++abi linked in (Apache-2.0 WITH LLVM-exception,
  `LICENSES/llvm-libcxx/`) and is built against bg3le's plugin header (MIT).

## vmangos: the WoW server

- Files: `server/bin/mangosd`, `server/bin/realmd`, `server/bin/Extractors/*`,
  `server/etc/*.conf.dist`.
- Licence: GNU General Public License version 2, with vMaNGOS' OpenSSL exception
  (`LICENSES/vmangos/LICENSE`). vmangos bundles libmpq (GPL-2.0), Recast/Detour (zlib),
  G3D (BSD), gSOAP (GPL-2.0 option), nlohmann/json (MIT), utf8cpp (BSL-1.0) and others:
  `LICENSES/vmangos/BUNDLED-LIBRARIES.txt`.
- Source: https://github.com/vmangos/core at commit 53d24d5c748bef5e0cde7b1155bcb3d979af614c
  with WOW-BG3's patches 0001 to 0023 (`patches/vmangos/` in the WOW-BG3 repository), built in the Steam Runtime 3
  SDK. The exact tree, patches and build script are in the sources archive.
- vmangos' databases are not in the release: `server/fetch-sql.py` downloads them on your PC at the
  first start, from the sources pinned in `server/sql.lock` (vmangos' world dump of 14 June 2021 from
  https://github.com/brotalnia/database, and vmangos core's `sql/` at the commit above), and checks each
  file's sha256. They describe WoW's world (creatures, items, quests) as the vmangos project publishes it.

## MariaDB: the server's database

- Files: `server/mariadb/` and `server/lib/libmariadb.so.3`, from MariaDB's official binary
  build `mariadb-11.4.8-linux-systemd-x86_64` (unmodified; debug symbols removed, unused
  programs and plugins left out).
- Licence: GPL-2.0 (the server and its programs; wolfSSL is built into `mariadbd`) and
  LGPL-2.1 (libmariadb, MariaDB Connector/C): `LICENSES/mariadb/`.
- Source: MariaDB 11.4.8, https://archive.mariadb.org/mariadb-11.4.8/source/, also in the
  sources archive.

## Libraries in `server/lib/`

From the Steam Runtime 3 (sniper) SDK the server was built in (Debian 11 packages):

- `libssl.so.1.1`, `libcrypto.so.1.1`: OpenSSL 1.1.1w (Debian 1.1.1w-0+deb11u8), under the
  OpenSSL and SSLeay licences (`LICENSES/server-lib/openssl-1.1-copyright`).
- `libstdc++.so.6`, `libgcc_s.so.1`: GCC 10.3.0 (Steam Runtime 10.3.0-3+steamrt3.1), GPL-3.0
  with the GCC Runtime Library Exception 3.1 (`LICENSES/server-lib/gcc-10-copyright`,
  `GPL-3`). Source package in the sources archive.
- `libcrypt.so.1`: libxcrypt 4.4.18 (Debian 4.4.18-4), LGPL-2.1 or later and others
  (`LICENSES/server-lib/libxcrypt-copyright`, `LGPL-2.1`). Source package in the sources archive.

## benilla: the WoW 1.12.1 client engine, run offscreen

- Files: `benilla/benilla`.
- Licence: MIT OR Apache-2.0 (`LICENSES/benilla/`).
- Source: https://github.com/samwhosung/benilla at commit
  d3ac9f75bf946bf9896a01deceb1b6dd539ace64 with WOW-BG3's patches (`patches/benilla/` in the
  WOW-BG3 repository, also in the sources archive).
- Built from 443 Rust crates, each under its own licence; every crate, its declared licence
  and its licence texts are in `LICENSES/benilla/RUST-CRATES.txt`. Most are MIT and/or
  Apache-2.0. Others:
  - MPL-2.0: symphonia 0.6.1 (symphonia, -bundle-mp3, -codec-adpcm, -codec-pcm, -core,
    -format-riff, -metadata) and triple_buffer 9.0.0, unmodified. Their source is on
    crates.io and in the sources archive (`upstream/rust-crates-mpl/`).
  - self_cell 1.2.2 is offered under Apache-2.0 OR GPL-2.0-only; WOW-BG3 uses it under Apache-2.0.
  - Lua 5.1.5 (MIT) is compiled in through benilla's fork of lua-src.
  - Others are under Zlib, ISC, BSD, 0BSD, MIT-0, CC0-1.0, Unlicense or Unicode-3.0 terms.

## wow-bg3-link: the fight bridge

- Files: `tools/bin/wow-bg3-link` (WOW-BG3's own Rust code).
- Built with serde, serde_json, itoa, memchr and zmij (MIT and/or Apache-2.0, Unlicense)
  and the Rust standard library (MIT OR Apache-2.0): `LICENSES/wow-bg3-link/RUST-CRATES.txt`.

## bg3le: the Linux Script Extender for BG3

- Files: `bg3le/libbg3le.so`, `bg3le/bg3le_install.py` (bg3le's installer, unchanged),
  `bg3le/bg3le-launch` (bg3le's launch wrapper, unchanged).
- Licence: bg3le is MIT (`LICENSES/bg3le/LICENSE`). It contains code from the BG3 Script
  Extender by Norbyte, under the MIT licence with the Commons Clause
  (`LICENSES/bg3le/bg3se-LICENSE`), so `libbg3le.so` as a whole carries the Commons Clause:
  it may not be sold. Also compiled in: LLVM libc++ and libc++abi, Abseil (Apache-2.0),
  Protocol Buffers (BSD-3-Clause), Dear ImGui, Optick, GLM, RapidJSON (MIT), TinyCrypt
  (BSD-3-Clause), Vulkan-Headers (Apache-2.0 OR MIT), LZ4 (BSD-2-Clause), Zstandard
  (BSD-3-Clause) and Lua 5.4 (MIT): `LICENSES/bg3le/`.
- Source: https://github.com/lenonk/bg3le at commit 5c729b72296defd4a09556ee723b12b76a0d445d
  with WOW-BG3's patches (`patches/bg3le/`; the patch list and the library's sha256 are in
  `patches/bg3le/build.lock.json`), both also in the sources archive.

## Python, for WOW-BG3's tools

- Files: `python/`.
- CPython 3.12.15 (PSF License) from python-build-standalone release 20261003, with the
  libraries it links in: OpenSSL 3 (Apache-2.0), libffi, expat, mpdecimal, ncurses, libedit,
  SQLite, zlib, bzip2, xz, libuuid (`LICENSES/python/`). Removed from the shipped copy: pip,
  tkinter, Tcl/Tk, the test suite, and the `_dbm` module (Berkeley DB).
- numpy 2.5.4 (BSD-3-Clause and others). Its `numpy.libs/` holds OpenBLAS (BSD-3-Clause),
  libgfortran (GPL-3.0 or later with the GCC Runtime Library Exception) and libquadmath
  (LGPL-2.1 or later), as numpy's own wheel ships them. Source of libgfortran and
  libquadmath: GCC (https://gcc.gnu.org), and the matching AlmaLinux GCC 8.5.0 source
  package in the sources archive.
- Pillow 12.3.0 (MIT-CMU). Its `pillow.libs/` holds libjpeg-turbo, libpng, libtiff, libwebp,
  libavif, FreeType (used under the FreeType License), HarfBuzz, Little CMS, OpenJPEG, xz,
  Zstandard, Brotli, libxcb and libXau, under their own permissive licences.
- lz4 4.4.5 and zstandard 0.25.0 (BSD-3-Clause, with the LZ4 and Zstandard libraries).
- Licence texts: `LICENSES/python/` and each package's `*.dist-info/licenses/` folder.

## FastLZ

- Files: `tools/link/tools/libfastlz.so`, built from `tools/fastlz/` in the WOW-BG3
  repository. Licence: MIT, by Ariya Hidayat (`LICENSES/fastlz/LICENSE`).
