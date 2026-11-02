#!/usr/bin/env python3
"""Licence texts and corresponding sources for a WOW-BG3 release.

Runs where the release is built, after the build steps (docs/build.md; the build trees are found
through release/build-env.sh's settings, from the environment). Three steps, in order:

  release/licences/collect.py fetch    the pinned upstream sources into WOWBG3_BUILD/licences/dl (sha256
                                       checked) and vmangos core at the pinned commit
  release/licences/collect.py texts    writes LICENSES/ from the build trees (commit it; the release
                                       build copies it to WOW-BG3/LICENSES/ in the zip)
  release/licences/collect.py sources  fills WOWBG3_BUILD/licences/sources/, the folder
                                       build-release.py --sources packs into WOW-BG3-VERSION-sources.tar.gz

Nothing is deleted: an older LICENSES or sources folder moves to WOWBG3_BUILD/old/.
Only the Python standard library is used (3.11 or newer).
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
import tomllib
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(os.environ.get("WOWBG3_BUILD") or ROOT / "build")   # release/build-env.sh's settings
TMP = BUILD / "licences"
DL = TMP / "dl"
CORE = TMP / "work/core"
OUT_TEXTS = ROOT / "LICENSES"
OUT_SOURCES = TMP / "sources"
SNIPER = Path(os.environ.get("WOWBG3_SNIPER") or BUILD / "bg3le-sniper")
SYSROOT = SNIPER / "sysroot"          # Steam Runtime 3 (sniper) SDK, patches/bg3le/sdk-image.lock
LLVM = SNIPER / "llvm-project"         # libc++ in libbg3le.so and wowbg3.so
BG3LE = Path(os.environ.get("BG3LE_SRC") or BUILD / "src/bg3le")
BG3LE_EXT = BG3LE / "external/third_party"
BENILLA = Path(os.environ.get("BENILLA_SRC") or BUILD / "src/benilla")
CARGO_HOME = Path(os.environ.get("CARGO_HOME") or BUILD / "cargo-home")
RUSTUP_HOME = Path(os.environ.get("RUSTUP_HOME") or BUILD / "rustup-home")
PYTHON = Path(os.environ.get("WOWBG3_PYTHON") or BUILD / "release/python/py")   # pack.sh's python: the copy the zip ships
MARIADB_BINTAR = Path(os.environ.get("MARIADB_TAR") or BUILD / "dl/mariadb-11.4.8-linux-systemd-x86_64.tar.gz")
SERVER_BUILD = Path(os.environ.get("WOWBG3_SERVER_BUILD") or BUILD / "portable-build")   # the shipped server pack's build

VMANGOS_COMMIT = "53d24d5c748bef5e0cde7b1155bcb3d979af614c"
RUSTC = "1.98.1"

# (file, url, sha256): every upstream source the sources archive carries.
PINS = [
    ("mariadb-11.4.8.tar.gz",
     "https://archive.mariadb.org/mariadb-11.4.8/source/mariadb-11.4.8.tar.gz",
     "52fa4dca2c5f80afc1667d523a27c06176d98532298a6b0c31ed73505f49e15c"),
    ("libxcrypt_4.4.18-4.dsc",
     "https://deb.debian.org/debian/pool/main/libx/libxcrypt/libxcrypt_4.4.18-4.dsc",
     "5c0ca54ddad5343596f6c0916caf30fbb9b8a144252b49dc74f97502f33cdb7a"),
    ("libxcrypt_4.4.18-4.debian.tar.xz",
     "https://deb.debian.org/debian/pool/main/libx/libxcrypt/libxcrypt_4.4.18-4.debian.tar.xz",
     "6c99b888c57e1411d870fa81d057e30444aa801ed430aa3126d31996e187dd84"),
    ("libxcrypt_4.4.18.orig.tar.xz",
     "https://deb.debian.org/debian/pool/main/libx/libxcrypt/libxcrypt_4.4.18.orig.tar.xz",
     "4cd2a06e98519d57a5572ee8885b6cc23c70a559d234c161d3f22c487edaa3fa"),
    ("gcc-10_10.3.0-3+steamrt3.1.dsc",
     "https://repo.steampowered.com/steamrt3/apt/pool/main/g/gcc-10/gcc-10_10.3.0-3%2Bsteamrt3.1.dsc",
     "1bc54e15345e4b2500ae43017c2f646c5122b812cbb061c1e4699b1339ed5d33"),
    ("gcc-10_10.3.0-3+steamrt3.1.debian.tar.xz",
     "https://repo.steampowered.com/steamrt3/apt/pool/main/g/gcc-10/gcc-10_10.3.0-3%2Bsteamrt3.1.debian.tar.xz",
     "a50fba674000a2d688a619a2a8909b542afaeea0dbe08c3926cf4026109cc3b7"),
    ("gcc-10_10.3.0.orig.tar.gz",
     "https://repo.steampowered.com/steamrt3/apt/pool/main/g/gcc-10/gcc-10_10.3.0.orig.tar.gz",
     "9ace579357cc2e976e4d2576fc1d519b6856495d98ccf11d1a67c5a9b4f79b8c"),
    ("gcc-8.5.0-28.el8_10.alma.1.src.rpm",
     "https://vault.almalinux.org/8.10/BaseOS/Source/Packages/gcc-8.5.0-28.el8_10.alma.1.src.rpm",
     "c94dbbd2cd6d5d01a5a6822ea18f8ce34942c25a80e17b4682d547d5e652dbd0"),
    ("cpython-3.12.15+20261003-x86_64-unknown-linux-gnu-pgo+lto-full.tar.zst",  # its licenses/ folder only
     "https://github.com/astral-sh/python-build-standalone/releases/download/20261003/"
     "cpython-3.12.15%2B20261003-x86_64-unknown-linux-gnu-pgo%2Blto-full.tar.zst",
     "aaeeeeea4f98a41c7acd7379229a374c201aee45c1954be5788c18af83733d2b"),
]
# Which pinned files go into the sources archive, and under which folder.
SOURCE_SETS = {
    "mariadb": ["mariadb-11.4.8.tar.gz"],
    "libxcrypt": ["libxcrypt_4.4.18-4.dsc", "libxcrypt_4.4.18-4.debian.tar.xz", "libxcrypt_4.4.18.orig.tar.xz"],
    "gcc-10-steamrt": ["gcc-10_10.3.0-3+steamrt3.1.dsc", "gcc-10_10.3.0-3+steamrt3.1.debian.tar.xz",
                       "gcc-10_10.3.0.orig.tar.gz"],
    "gcc-8-almalinux": ["gcc-8.5.0-28.el8_10.alma.1.src.rpm"],
}
# Crates under MPL-2.0 in benilla's player build: their source goes into the sources archive.
MPL_CRATES = ["symphonia-0.6.1", "symphonia-bundle-mp3-0.6.1", "symphonia-codec-adpcm-0.6.1",
              "symphonia-codec-pcm-0.6.1", "symphonia-core-0.6.1", "symphonia-format-riff-0.6.1",
              "symphonia-metadata-0.6.1", "triple_buffer-9.0.0"]

LICENCE_NAME = re.compile(r"(?i)^(licen[cs]e|copying|unlicense|notice|copyright|ofl|ufl|patents)"
                          r"|[-_.](licen[cs]e|copying)([-_.]|$)")
SKIP_SUFFIX = (".rs", ".toml", ".ttf", ".otf", ".c", ".h", ".py", ".json", ".yml", ".sh", ".lock", ".orig")

MIT_TEMPLATE = """MIT License

Copyright (c) {holders}

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""

MIT0_TEMPLATE = """MIT No Attribution

Copyright {holders}

Permission is hereby granted, free of charge, to any person obtaining a copy of this
software and associated documentation files (the "Software"), to deal in the Software
without restriction, including without limitation the rights to use, copy, modify,
merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"""


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def aside(path: Path):
    """Moves an existing output aside to WOWBG3_BUILD/old (never deleted)."""
    if path.exists():
        rb = BUILD / "old" / f"licences-{time.strftime('%Y%m%d')}"
        rb.mkdir(parents=True, exist_ok=True)
        shutil.move(str(path), str(rb / f"{path.name}-{time.strftime('%H%M%S')}"))


def need(path: Path) -> Path:
    if not path.exists():
        raise SystemExit(f"missing: {path}")
    return path


def head_comment(path: Path, max_lines=80) -> str:
    """The leading comment block of a source file (where its licence is)."""
    lines = need(path).read_text(encoding="utf-8", errors="replace").splitlines()
    out = []
    for line in lines[:max_lines]:
        out.append(line)
        if "*/" in line and len(out) > 1:
            break
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------------------------- fetch

def fetch():
    DL.mkdir(parents=True, exist_ok=True)
    for name, url, digest in PINS:
        dest = DL / name
        if not dest.exists():
            print("fetch", url)
            tmp = dest.with_name(dest.name + ".part")
            with urllib.request.urlopen(url) as r, tmp.open("wb") as f:
                shutil.copyfileobj(r, f)
            tmp.rename(dest)
        got = sha256(dest)
        if got != digest:
            raise SystemExit(f"sha256 mismatch {name}: {got} (pinned {digest})")
        print("ok", name)
    if not (CORE / ".git").is_dir():
        CORE.mkdir(parents=True)
        git = ["git", "-C", str(CORE)]
        subprocess.run(git + ["init", "-q"], check=True)
        subprocess.run(git + ["remote", "add", "origin", "https://github.com/vmangos/core.git"], check=True)
        subprocess.run(git + ["fetch", "-q", "--depth", "1", "origin", VMANGOS_COMMIT], check=True)
        subprocess.run(git + ["checkout", "-q", "FETCH_HEAD"], check=True)
    print("ok vmangos core", core_head())


def core_head() -> str:
    head = subprocess.run(["git", "-C", str(CORE), "rev-parse", "HEAD"], check=True, capture_output=True,
                          text=True).stdout.strip()
    if head != VMANGOS_COMMIT:
        raise SystemExit(f"vmangos core at {head}, not {VMANGOS_COMMIT}: run fetch")
    return head


# ---------------------------------------------------------------------------------------------- rust

def cargo_tree(manifest_dir: Path, extra) -> list:
    env = dict(os.environ, CARGO_HOME=str(CARGO_HOME), RUSTUP_HOME=str(RUSTUP_HOME), RUSTUP_AUTO_INSTALL="0",
               PATH=f"{CARGO_HOME}/bin:{os.environ['PATH']}")
    cmd = ["cargo", f"+{RUSTC}", "tree", "--offline", "--locked", "-e", "normal",
           "--target", "x86_64-unknown-linux-gnu", "--prefix", "none", "-f", "{p}|{l}", *extra]
    r = subprocess.run(cmd, cwd=manifest_dir, env=env, check=True, capture_output=True, text=True)
    rows = {}
    for line in r.stdout.splitlines():
        line = line.replace(" (*)", "").replace(" (proc-macro)", "").strip()
        if not line or "|" not in line:
            continue
        pkg, lic = line.split("|", 1)
        m = re.match(r"(\S+) v(\S+)(?: \((/[^)]*)\))?$", pkg)
        if not m:
            raise SystemExit(f"cargo tree line not understood: {line}")
        rows[(m[1], m[2])] = (lic, m[3])
    return [(n, v, lic, path) for (n, v), (lic, path) in sorted(rows.items())]


def crate_dir(name, version, path) -> Path:
    if path:
        return Path(path)
    regs = list((CARGO_HOME / "registry/src").glob(f"*/{name}-{version}"))
    if not regs:
        raise SystemExit(f"crate source not in {CARGO_HOME}: {name}-{version}")
    return regs[0]


def licence_files(d: Path):
    files = []
    for p in sorted(d.rglob("*")):
        rel = p.relative_to(d)
        if any(part in ("target", ".git", "tests", "benches", "examples") for part in rel.parts[:-1]):
            continue
        if p.is_file() and LICENCE_NAME.search(p.name) and p.suffix.lower() not in SKIP_SUFFIX:
            files.append(p)
    return files


def rust_notice(title: str, crates, own_root: Path, note: str) -> str:
    """One text: every crate with its declared licence and its licence files, each distinct text once."""
    texts = {}       # id -> (text, file name, [crates])
    rows = []
    common = SYSROOT / "usr/share/common-licenses"
    standard = {k: need(common / k).read_text() for k in ("Apache-2.0", "MPL-2.0", "CC0-1.0")}
    for name, version, lic, path in crates:
        d = crate_dir(name, version, path)
        cid = f"{name} {version}"
        files = licence_files(d)
        if not files and path:  # a workspace member: the workspace's own licence files
            files = [p for p in sorted(own_root.iterdir()) if p.is_file() and LICENCE_NAME.search(p.name)]
        refs = []
        for p in files:
            t = p.read_text(encoding="utf-8", errors="replace")
            k = hashlib.sha256(t.encode()).hexdigest()[:12]
            texts.setdefault(k, (t, p.name, []))[2].append(cid)
            refs.append(f"{p.relative_to(d) if p.is_relative_to(d) else p.name} [{k}]")
        if not files:
            # The published crate carries no licence file: the standard text of what it declares,
            # with the holders named in its Cargo.toml.
            meta = tomllib.loads((d / "Cargo.toml").read_text(encoding="utf-8")).get("package", {})
            holders = ", ".join(meta.get("authors") or []) or f"the {name} authors"
            ids = set(re.findall(r"[A-Za-z0-9.\-]+", lic.replace("/", " OR "))) - {"OR", "AND", "WITH"}
            for spdx in sorted(ids):
                if spdx == "MIT":
                    t = MIT_TEMPLATE.format(holders=holders)
                elif spdx == "MIT-0":
                    t = MIT0_TEMPLATE.format(holders=holders)
                elif spdx in standard:
                    t = standard[spdx]
                else:
                    raise SystemExit(f"{cid}: no licence file and no standard text for {spdx}")
                k = hashlib.sha256(t.encode()).hexdigest()[:12]
                texts.setdefault(k, (t, f"standard {spdx} text", []))[2].append(cid)
                refs.append(f"standard {spdx} text, holders from Cargo.toml [{k}]")
        rows.append(f"{cid} | {lic or 'see below'} | {'; '.join(refs)}")
    out = [title, "=" * len(title), "", note, "",
           "Crate | declared licence (SPDX) | its licence texts below, by id", ""]
    out += rows
    out += ["", "", "Licence texts", "-------------", ""]
    for k, (t, fname, users) in texts.items():
        out += [f"[{k}] {fname}, used by: {', '.join(users)}", "", t.rstrip(), "", "-" * 78, ""]
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------------------------- texts

def put(dest: Path, text_or_path):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(text_or_path, Path):
        shutil.copyfile(need(text_or_path), dest)
    else:
        dest.write_text(text_or_path, encoding="utf-8")


def tar_member(tar: Path, member_suffix: str) -> str:
    with tarfile.open(need(tar)) as t:
        for m in t:
            if m.name.endswith(member_suffix) and m.isfile():
                return t.extractfile(m).read().decode("utf-8", errors="replace")
    raise SystemExit(f"{member_suffix} not in {tar}")


def texts():
    out = OUT_TEXTS.parent / "LICENSES.new"
    aside(out)
    out.mkdir()
    common = SYSROOT / "usr/share/common-licenses"
    core_head()

    # vmangos (mangosd, realmd, the extractors): GPL-2.0 with vMaNGOS' OpenSSL exception.
    c = need(CORE / "dep")
    put(out / "vmangos/LICENSE", CORE / "LICENSE")
    bundled = ["vmangos core bundles these libraries (dep/ in its source tree, which is in the sources archive).",
               "Their licence notices, as the files in that tree carry them:", ""]
    for f, whole in (("libmpq/COPYING", True), ("recastnavigation/License.txt", True),
                     ("include/utf8cpp/LICENSE", True), ("g3dlite/source/license.cpp", True),
                     ("gsoap/stdsoap2.h", False), ("include/nlohmann/json.hpp", False),
                     ("include/mersennetwister/MersenneTwister.h", False), ("include/bzip2/bzlib.h", False),
                     ("cpptrace/LICENSE", True)):
        body = (c / f).read_text(encoding="utf-8", errors="replace") if whole else head_comment(c / f, 120)
        bundled += [f"==== dep/{f}", "", body.rstrip(), ""]
    put(out / "vmangos/BUNDLED-LIBRARIES.txt", "\n".join(bundled) + "\n")

    # MariaDB 11.4.8 (mariadbd, mariadb, mariadb-dump, ..., libmariadb.so.3, wolfSSL inside mariadbd).
    put(out / "mariadb/COPYING", tar_member(MARIADB_BINTAR, "-x86_64/COPYING"))
    put(out / "mariadb/CREDITS", tar_member(MARIADB_BINTAR, "-x86_64/CREDITS"))
    put(out / "mariadb/libmariadb-COPYING.LIB", tar_member(DL / "mariadb-11.4.8.tar.gz", "/libmariadb/COPYING.LIB"))
    put(out / "mariadb/THIRDPARTY", tar_member(DL / "mariadb-11.4.8.tar.gz", "mariadb-11.4.8/THIRDPARTY"))

    # Libraries from the Steam Runtime 3 SDK shipped in server/lib/.
    put(out / "server-lib/openssl-1.1-copyright", SYSROOT / "usr/share/doc/libssl1.1/copyright")
    put(out / "server-lib/gcc-10-copyright", SYSROOT / "usr/share/doc/gcc-10-base/copyright")
    put(out / "server-lib/libxcrypt-copyright", SYSROOT / "usr/share/doc/libcrypt1/copyright")
    put(out / "server-lib/GPL-3", common / "GPL-3")
    put(out / "server-lib/LGPL-2.1", common / "LGPL-2.1")

    # benilla: its own licences, its forks of lua-src and kira, the Rust crates of the player build.
    ben = BENILLA
    put(out / "benilla/LICENSE-MIT", ben / "LICENSE-MIT")
    put(out / "benilla/LICENSE-APACHE", ben / "LICENSE-APACHE")
    crates = cargo_tree(ben, ["-p", "benilla", "--no-default-features"])
    put(out / "benilla/RUST-CRATES.txt", rust_notice(
        "Rust crates in benilla's player build", crates, ben,
        f"cargo tree -p benilla --no-default-features -e normal, rustc {RUSTC}, benilla d3ac9f75 with WOW-BG3's\n"
        f"patches (patches/benilla/). The Rust standard library (rustc {RUSTC}) is MIT OR Apache-2.0. Lua 5.1.5\n"
        "is compiled in through benilla's lua-src fork (third_party/lua-src, MIT). Where a crate offers a\n"
        "choice of licences, WOW-BG3 distributes it under the permissive one; self_cell (Apache-2.0 OR\n"
        "GPL-2.0-only) under Apache-2.0. The MPL-2.0 crates' source is in the sources archive\n"
        "(rust-crates-mpl/)."))
    lua51 = list((ben / "third_party/lua-src").rglob("lua.h"))
    if lua51:
        txt = lua51[0].read_text(encoding="utf-8", errors="replace")
        m = re.search(r"\* Copyright \(C\) .* Lua\.org, PUC-Rio\..*?(?=\*{4})", txt, re.S)
        put(out / "benilla/lua-5.1.5-COPYRIGHT.txt", re.sub(r"(?m)^\* ?", "", m[0]) if m else head_comment(lua51[0]))

    # wow-bg3-link (tools/bin/wow-bg3-link): our crate and its dependencies.
    put(out / "wow-bg3-link/RUST-CRATES.txt", rust_notice(
        "Rust crates in wow-bg3-link", [r for r in cargo_tree(ROOT / "bridge", []) if r[0] != "wow-bg3-link"],
        ROOT / "bridge", f"cargo tree -e normal of bridge/, rustc {RUSTC}. The Rust standard library is MIT OR "
                       "Apache-2.0."))

    # bg3le (libbg3le.so with WOW-BG3's patches, patches/bg3le): what it compiles in (its package.sh).
    b = out / "bg3le"
    put(b / "LICENSE", BG3LE / "LICENSE")
    put(b / "bg3se-LICENSE", BG3LE / "vendor/bg3se/LICENSE")
    put(b / "bg3se-NOTICE.md", BG3LE / "vendor/NOTICE.md")
    for name, src in (("libcxx-LICENSE.TXT", LLVM / "libcxx/LICENSE.TXT"),
                      ("libcxxabi-LICENSE.TXT", LLVM / "libcxxabi/LICENSE.TXT"),
                      ("abseil-LICENSE", BG3LE_EXT / "abseil/LICENSE"),
                      ("protobuf-LICENSE", BG3LE_EXT / "protobuf/LICENSE"),
                      ("imgui-LICENSE.txt", BG3LE_EXT / "imgui/LICENSE.txt"),
                      ("optick-LICENSE", BG3LE_EXT / "optick/LICENSE"),
                      ("glm-copying.txt", BG3LE_EXT / "glm/copying.txt"),
                      ("rapidjson-license.txt", BG3LE_EXT / "rapidjson/license.txt"),
                      ("tinycrypt-LICENSE", BG3LE_EXT / "tinycrypt/LICENSE"),
                      ("vulkan-headers-LICENSE.md", BG3LE_EXT / "Vulkan/LICENSE.md"),
                      ("lz4-LICENSE", BG3LE / "external/lz4/LICENSE"),
                      ("zstd-LICENSE", BG3LE / "external/zstd/LICENSE")):
        put(b / name, src)
    lua = need(BG3LE / "external/lua/lua.h").read_text(encoding="utf-8", errors="replace")
    m = re.search(r"\* Copyright \(C\) .* Lua\.org, PUC-Rio\..*?(?=\*{4})", lua, re.S)
    if not m:
        raise SystemExit("Lua licence not found in bg3le's lua.h")
    put(b / "lua-LICENSE.txt", re.sub(r"(?m)^\* ?", "", m[0]))

    # LLVM libc++/libc++abi linked into wowbg3.so (same build as bg3le's).
    put(out / "llvm-libcxx/libcxx-LICENSE.TXT", LLVM / "libcxx/LICENSE.TXT")
    put(out / "llvm-libcxx/libcxxabi-LICENSE.TXT", LLVM / "libcxxabi/LICENSE.TXT")

    # The bundled Python: python-build-standalone's licence files, then each wheel's.
    pdir = TMP / "pbs"
    if not (pdir / "python/licenses").is_dir():
        pdir.mkdir(parents=True, exist_ok=True)
        subprocess.run(f"zstd -dc '{need(DL / PINS[-1][0])}' | tar -x -C '{pdir}' python/licenses python/PYTHON.json",
                       shell=True, check=True)
    for f in sorted((pdir / "python/licenses").iterdir()):
        put(out / "python" / f.name, f)
    site = need(PYTHON / "lib/python3.12/site-packages")
    for dist in sorted(site.glob("*.dist-info")):
        lic = dist / "licenses"
        for f in (sorted(lic.rglob("*")) if lic.is_dir() else []):
            if f.is_file():
                put(out / "python" / dist.name.replace(".dist-info", "") / f.relative_to(lic), f)

    # FastLZ (tools/link/tools/libfastlz.so, built from tools/fastlz/).
    put(out / "fastlz/LICENSE", head_comment(ROOT / "tools/fastlz/fastlz.c"))

    put(out / "README.txt", INDEX)
    aside(OUT_TEXTS)
    out.rename(OUT_TEXTS)
    n = sum(1 for p in OUT_TEXTS.rglob("*") if p.is_file())
    print(f"{OUT_TEXTS}: {n} files")


INDEX = """WOW-BG3: licence texts of everything in this download that WOW-BG3 did not write.
Which program each folder belongs to, and where its source is: THIRD-PARTY-NOTICES.md next to the README.
Made by release/licences/collect.py from the build trees of the release.

vmangos/        mangosd, realmd, the map extractors (server/bin/): GPL-2.0 with vMaNGOS' OpenSSL exception
mariadb/        MariaDB 11.4.8 (server/mariadb/, server/lib/libmariadb.so.3): GPL-2.0, LGPL-2.1
server-lib/     server/lib/: libssl, libcrypto (OpenSSL 1.1.1w), libstdc++, libgcc_s (GCC 10), libcrypt (libxcrypt)
benilla/        benilla/benilla: MIT OR Apache-2.0, and the Rust crates it is built from (RUST-CRATES.txt)
wow-bg3-link/   tools/bin/wow-bg3-link: the Rust crates it is built from
bg3le/          libbg3le.so: MIT, bg3se's MIT with the Commons Clause, and the libraries compiled into it
llvm-libcxx/    LLVM libc++ and libc++abi, linked into wowbg3.so
python/         python/: CPython 3.12 (python-build-standalone) and numpy, Pillow, lz4, zstandard
fastlz/         tools/link/tools/libfastlz.so: MIT
"""


# ---------------------------------------------------------------------------------------------- sources

def sources():
    out = OUT_SOURCES.parent / "sources.new"
    aside(out)
    out.mkdir(parents=True)
    index = ["Upstream sources of the GPL, LGPL and MPL parts of WOW-BG3's binary download (sha256).",
             "WOW-BG3's own patches, build scripts and pins are next to this folder in the archive.", ""]
    # vmangos core at the pinned commit: the tree the server pack was built from (plus patches/vmangos/).
    core_head()
    (out / "vmangos").mkdir()
    core_tgz = out / f"vmangos/vmangos-core-{VMANGOS_COMMIT[:10]}.tar.gz"
    subprocess.run(["git", "-C", str(CORE), "archive", "--format=tar.gz",
                    f"--prefix=vmangos-core-{VMANGOS_COMMIT[:10]}/", "-o", str(core_tgz), "HEAD"], check=True)
    # The exact build of the shipped server pack: its build script and log (patch list, compiler).
    for f in ("build.sh", "build.log"):
        shutil.copy2(need(SERVER_BUILD / f), out / f"vmangos/portable-build-{f}")
    # The binaries that build made: build-release.py --sources checks the zip's server/bin against these.
    pack = need(SERVER_BUILD / "wowbg3-server")
    built = [f"{sha256(p)}  {p.relative_to(pack)}" for p in sorted((pack / "bin").rglob("*"))
             if p.is_file() and os.access(p, os.X_OK) and p.suffix not in (".json", ".txt")]
    (out / "vmangos/BUILT.sha256").write_text("\n".join(built) + "\n")
    for d, names in SOURCE_SETS.items():
        (out / d).mkdir()
        for n in names:
            pin = next(p for p in PINS if p[0] == n)
            src = need(DL / n)
            if sha256(src) != pin[2]:
                raise SystemExit(f"sha256 mismatch {n}: run fetch")
            shutil.copy2(src, out / d / n)
    (out / "rust-crates-mpl").mkdir()
    for c in MPL_CRATES:
        hits = list((CARGO_HOME / "registry/cache").glob(f"*/{c}.crate"))
        if not hits:
            raise SystemExit(f"{c}.crate not in {CARGO_HOME}/registry/cache")
        shutil.copy2(hits[0], out / "rust-crates-mpl" / f"{c}.crate")
    for p in sorted(out.rglob("*")):
        if p.is_file():
            index.append(f"{sha256(p)}  {p.relative_to(out)}")
    (out / "SOURCES.sha256").write_text("\n".join(index) + "\n")
    aside(OUT_SOURCES)
    out.rename(OUT_SOURCES)
    total = sum(p.stat().st_size for p in OUT_SOURCES.rglob("*") if p.is_file())
    print(f"{OUT_SOURCES}: {total // (1 << 20)} MB")


if __name__ == "__main__":
    steps = {"fetch": fetch, "texts": texts, "sources": sources}
    if len(sys.argv) < 2 or any(a not in steps for a in sys.argv[1:]):
        raise SystemExit(__doc__)
    for a in sys.argv[1:]:
        steps[a]()
