# WOW-BG3 sources archive

This archive is published next to each WOW-BG3 release download. It holds the
source code of the programs in the download that are under the GPL, the LGPL
or the MPL, so that the download's binaries come with their source.
Which program is under which licence: `THIRD-PARTY-NOTICES.md` in the download.

## What is here

| Folder | What | Binaries in the download it builds |
|---|---|---|
| `wow-bg3-server-patches/` | WOW-BG3's patches to vmangos core (0001 to 0015, applied in order) | `server/bin/*` |
| `wow-bg3-build/server-portable/` | the scripts that build and pack the server (`build-portable.sh`, `pack-portable.sh`, `pins.sh`) | `server/` |
| `wow-bg3-build/sdk-image.lock` | the Steam Runtime 3 (sniper) SDK image every binary was built in | all |
| `upstream/vmangos/` | vmangos core at commit 53d24d5c74 (`git archive`), and `portable-build-build.sh` / `portable-build-build.log`: the exact build of the shipped server (the patch list, the CMake options, the compiler) | `server/bin/*` |
| `upstream/mariadb/` | MariaDB 11.4.8 source, unmodified (archive.mariadb.org) | `server/mariadb/*`, `server/lib/libmariadb.so.3` (the shipped files are MariaDB's own official binary build `mariadb-11.4.8-linux-systemd-x86_64`, sha256 c29f9b8e...) |
| `upstream/gcc-10-steamrt/` | the Steam Runtime 3 source package of GCC 10 (10.3.0-3+steamrt3.1) | `server/lib/libstdc++.so.6`, `server/lib/libgcc_s.so.1` |
| `upstream/libxcrypt/` | the Debian 11 source package of libxcrypt (4.4.18-4) | `server/lib/libcrypt.so.1` |
| `upstream/gcc-8-almalinux/` | the AlmaLinux 8 source package of GCC 8.5.0-28 (holds libgfortran and libquadmath) | `python/lib/python3.12/site-packages/numpy.libs/libgfortran*`, `libquadmath*` (bundled by numpy's wheel; see below) |
| `upstream/rust-crates-mpl/` | the MPL-2.0 Rust crates benilla is built with, as published on crates.io, unmodified | `benilla/benilla` |
| `wow-bg3-benilla-patches/` | WOW-BG3's patches to benilla and its build script | `benilla/benilla` |
| `wow-bg3-bg3le-patches/` | WOW-BG3's patches to bg3le, and `build.lock.json` (bg3le commit, compiler, patch hashes) | `bg3le/libbg3le.so` |
| `upstream/SOURCES.sha256` | sha256 of every file under `upstream/` | |

## Building the server from this archive

1. Unpack `upstream/vmangos/vmangos-core-53d24d5c74.tar.gz`.
2. Apply `wow-bg3-server-patches/0001-*.patch` to `0015-*.patch` in order (`git apply`).
3. Build as `upstream/vmangos/portable-build-build.sh` does, in the SDK image named in
   `wow-bg3-build/sdk-image.lock` (GCC 14, `-DSUPPORTED_CLIENT_BUILD=5875 -DBUILD_EXTRACTORS=1`,
   MariaDB's headers and libmariadb from the official 11.4.8 binary build).
4. `wow-bg3-build/server-portable/pack-portable.sh` makes the `server/` folder of the download.

## Notes

- benilla (MIT OR Apache-2.0) and bg3le (MIT; its bg3se part MIT with the Commons Clause) do not
  require their source to be published with a binary. Their upstream source is public at the
  commits named in `THIRD-PARTY-NOTICES.md`, and WOW-BG3's patches to them are in this archive.
- The numpy wheel's `libgfortran` and `libquadmath` were built by the numpy project on a
  manylinux_2_28 (AlmaLinux 8) system with GCC 8.5.0. WOW-BG3 did not build them; the source
  package here is the AlmaLinux release that matches the GCC version the wheel reports.
- OpenSSL 1.1.1w (`server/lib/libssl.so.1.1`, `libcrypto.so.1.1`) and the Python packages are under
  licences that do not require source with binaries; their sources are public (Debian's
  `openssl 1.1.1w-0+deb11u8`, python-build-standalone 20261003, PyPI).
