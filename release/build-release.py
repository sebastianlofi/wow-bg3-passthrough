#!/usr/bin/env python3
"""Builds the WOW-BG3 release a player installs (docs/build.md).

Output in OUT/WOW-BG3-VERSION/ and OUT/WOW-BG3-VERSION.zip:
  WOW-BG3.pak     the mod BG3's mod manager lists (meta.lsx, Script Extender config and Lua)
  WOW-BG3/        goes next to the pak in BG3's Mods folder:
                  wowbg3-launcher.so, wowbg3.so, wowbg3_render.so (bg3le plugins),
                  launcher.conf, settings.conf, server/ (portable server), benilla/ (offscreen client:
                  benilla, AddOns/WowBg3Layout), tools/ (the coordinator wowbg3-coord.py and the tools it
                  runs: link/tools/*.py, server/tools/world-console.py, bin/wow-bg3-link),
                  WoW/ (empty: the player's own WoW 1.12.1 files), NOTICES/ (licence texts)
  README.md       the repository's README.md (one file for GitHub and the zip, the maintainer 2026-11-14), with the
                  docs/ pages and LICENSE it links to, so its links work offline
  THIRD-PARTY-NOTICES.md, MANIFEST.txt (sha256 of every file)

  build-release.py [--version 0.1.0] --out DIR [--launcher SO] [--plugin SO] [--render SO ...]
                   [--server DIR] [--benilla BIN] [--fight-bridge BIN] [--allow-missing]
  The coordinator and its tools come from this tree's tools/ (TOOLS); --benilla is the player build's
  binary, the addon comes from wow-addon/; the server's wowbg3-server.sh is this tree's, over the
  packed copy. Defaults for the built parts are under WOWBG3_BUILD (release/build-env.sh).
  --version defaults to WOWBG3_VERSION, else RELEASE_VERSION (0.1.0, the public release); a test build
  can pass its own N.N.N.N build number.
  --source NAME=DIR (repeatable): DIR goes into OUT/WOW-BG3-VERSION-sources.tar.gz as NAME/, the
      corresponding source of the GPL parts; patches/ and the build scripts are always added.

No game files: nothing from WoW, BG3 or server data goes in, and nothing made from them (check_no_game_data
refuses a build that has any). The server's database SQL and the mobs' abilities are made on the player's PC at
the first start: server/fetch-sql.py downloads the SQL from vmangos' public sources (server/sql.lock), and the
coordinator builds the abilities from that database and the player's client. --server and --benilla are the
server pack (server/pack-portable.sh) and benilla's player build (patches/benilla/build.sh) for the
Steam runtime (sniper); their licence texts are in LICENSES/. Without --allow-missing a
missing part is an error; with it the part is listed as missing in MANIFEST.txt (test builds only).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import stat
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RELEASE_VERSION = "0.1.0"  # the public release; README.md and docs/ name it
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import lspk  # noqa: E402

BUILD = Path(os.environ.get("WOWBG3_BUILD") or ROOT / "build")          # release/build-env.sh
SNIPER = Path(os.environ.get("WOWBG3_SNIPER") or BUILD / "bg3le-sniper")
BG3LE_SRC = Path(os.environ.get("BG3LE_SRC") or BUILD / "src/bg3le")
BENILLA_SRC = Path(os.environ.get("BENILLA_SRC") or BUILD / "src/benilla")
MOD_SRC = ROOT / "mod"
UUID = "a97c1258-37c3-434c-9309-c982f6cea252"
FOLDER = "WOWBG3"          # the release module's folder (and ModTable)
LAUNCHER = "wowbg3-launcher.so"
PLUGIN = "wowbg3.so"
# The tools the coordinator runs (tools/relenv.py points them at the player's folders).
TOOLS = ["lifecycle.py", "colour_table.py", "crashreport.py", "wowsave.py", "wowchar.py", "wowserver.py", "world_bridge.py", "wow_arena.py", "wow_stand.py", "wow_waypoints.py",
         "wow_places.py", "wow_docks.py", "wow_lifts.py", "wow_lift_platforms.py", "wowbg3_level.py", "wow_vmap.py", "wow_mpq.py", "inventory_bridge.py", "wowlevel.py", "wow_items.py", "wow_items_rules.json"]
TOOLS_LATER = ["wowaccount.py", "wowbg3-chatcmd.py", "wowbg3cmd.py",  # playthrough accounts, chat commands
               "tavtex.py", "bg3tex.py", "bg3pak.py", "bg3shade.py", "bg3index.py",  # Tav's textures (with --python)
               "wow_icons.py", "wowcursor.py",  # the icon atlas and the quest cursors (first run, with --python)
               "wow_abilities.py", "wow_abilities_rules.json",  # the mobs' abilities (first run, with --python)
               "wow_spells.py",  # our spells' names from the player's client (first run, with --python)
               "camp_grid.py",  # the camp's AI grid joined to our level's, from the player's BG3 paks (each start)
               "seenoise.py"]  # BG3's see-through noise for benilla (first run, with --python)


def version64(v: str) -> int:
    parts = [int(x) for x in v.split(".")] + [0, 0, 0, 0]
    major, minor, rev, build = parts[:4]
    return (major << 55) | (minor << 47) | (rev << 31) | build


def meta_lsx(folder: str, version: str) -> bytes:
    text = (MOD_SRC / "meta.lsx").read_text(encoding="utf-8")
    assert f'value="{UUID}"' in text, "meta.lsx lost the WOW-BG3 UUID"
    text = re.sub(r'(id="Folder" type="LSString" value=")[^"]*(")', rf"\g<1>{folder}\g<2>", text)
    v64 = str(version64(version))
    text = re.sub(r'(id="Version64" type="int64" value=")[^"]*(")', rf"\g<1>{v64}\g<2>", text)
    return text.encode("utf-8")


def config_json(native_plugins) -> bytes:
    src = json.loads((MOD_SRC / "ScriptExtender/Config.json").read_text(encoding="utf-8"))
    src["ModTable"] = FOLDER
    # bg3le patch 0003: plugins the mod ships beside its pak, in Mods/WOW-BG3/.
    src["NativePlugins"] = list(native_plugins)
    # bg3le patch 0005: the render plugin starts before BG3 makes its Vulkan objects (it must
    # see BG3's first pipelines); listed in both, bg3le loads it once.
    src["EarlyNativePlugins"] = [n for n in native_plugins if n == "wowbg3_render.so"]
    return (json.dumps(src, indent=2) + "\n").encode("utf-8")


VERSION_LUA = "Version.lua"  # written here, never a file of the source tree


def build_id(files, release=()) -> str:
    """Eight hex digits that name one build: from every file of the pak (name and content) and, for a whole release,
    every other file it holds (`release`: [(path in the release, sha256)]). The same sources give the same id."""
    h = hashlib.sha256()
    for name, data in sorted(files):
        h.update(b"pak\0" + name.encode() + b"\0" + hashlib.sha256(data).hexdigest().encode() + b"\n")
    for name, digest in sorted(release):
        h.update(b"file\0" + str(name).encode() + b"\0" + digest.encode() + b"\n")
    return h.hexdigest()[:8]


def built_at() -> str:
    """When this release is built, in the extender's own form ("Jan  2 2026 03:04:05", C's __DATE__ __TIME__): the
    builder's local time, or SOURCE_DATE_EPOCH's when it is set (a reproducible build). bg3le's line says its own date,
    which its reproducible build fixes at the bg3le commit's; the menu shows this one."""
    import time
    t = time.localtime(int(os.environ['SOURCE_DATE_EPOCH'])) if os.environ.get('SOURCE_DATE_EPOCH') else time.localtime()
    months = 'Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec'.split()
    return '%s %2d %d %02d:%02d:%02d' % (months[t.tm_mon - 1], t.tm_mday, t.tm_year, t.tm_hour, t.tm_min, t.tm_sec)


def version_lua(version: str, build: str, built: str = None) -> bytes:
    """What MenuLine.lua writes into the extender's line under BG3's main menu: "(bg3le, WoW-BG3 <version> build
    <build>), built on <built>"."""
    return ('-- Written by release/build-release.py: this release, this build of it and when it was built.\n'
            'return {Version = "%s", Build = "%s", Built = "%s"}\n' % (version, build, built or built_at())).encode()


def pak_files(version: str, native_plugins, release=()):
    """`release`: the other files of the release this pak belongs to ([(path, sha256)]), for the build id; a pak
    made alone (a test pak) gets the id of its own files."""
    files = [(f"Mods/{FOLDER}/meta.lsx", meta_lsx(FOLDER, version)),
             (f"Mods/{FOLDER}/ScriptExtender/Config.json", config_json(native_plugins))]
    lua = sorted((MOD_SRC / "ScriptExtender/Lua").glob("*.lua"))
    if not lua:
        raise SystemExit("no Lua in " + str(MOD_SRC))
    if any(p.name == VERSION_LUA for p in lua):
        raise SystemExit(f"{VERSION_LUA} is written by the release builder: take it out of {MOD_SRC}")
    for p in lua:
        files.append((f"Mods/{FOLDER}/ScriptExtender/Lua/{p.name}", p.read_bytes()))
    # The stats of our own spells: never an empty file (an empty stats .txt hangs BG3's load).
    for p in sorted((MOD_SRC / f"Public/{FOLDER}/Stats/Generated/Data").glob("*.txt")):
        files.append((f"Public/{FOLDER}/Stats/Generated/Data/{p.name}", p.read_bytes()))
    files += level_files()
    files.append((f"Mods/{FOLDER}/ScriptExtender/Lua/{VERSION_LUA}", version_lua(version, build_id(files, release))))
    return files


def level_files():
    """The levels of our own (release/levels/*.json, tools/wowbg3_level.py) and the lighting and atmosphere they name
    (tools/wowbg3_lighting.py), as BG3 loads them: .lsf written by tools/lsf_write.py. Nothing of a BG3 level, of a BG3
    lighting or of WoW is in them."""
    import tempfile
    sys.path.insert(0, str(ROOT / "tools"))
    import lsf_write
    import wowbg3_level
    import wowbg3_lighting
    files = [(f"Public/{FOLDER}/Content/Assets/Atmospheres/[PAK]_Atmospheres/_merged.lsf",
              lsf_write.lsx_to_lsf(wowbg3_lighting.bank_lsx().encode("utf-8")))]
    import wow_lift_platforms
    specs = [json.loads(q.read_text()) for q in sorted((HERE / "levels").glob("*.json"))]
    # BG3's own moving ground for WoW's lifts: a level that asks for it places the family of platforms, each a
    # small level of its own with a root template the placement names (tools/wow_lift_platforms.py).
    if any(s.get("LiftPlatforms") for s in specs):
        for s in specs:
            if s.get("LiftPlatforms"):
                s["LevelTemplates"] = s.get("LevelTemplates", []) + wow_lift_platforms.instances()
        specs += wow_lift_platforms.specs()
    roots = wowbg3_level.root_templates_lsx(specs)
    if roots:
        files.append((f"Public/{FOLDER}/RootTemplates/WOWBG3_Platforms.lsf", lsf_write.lsx_to_lsf(roots.encode("utf-8"))))
    for spec in specs:
        with tempfile.TemporaryDirectory() as tmp:
            name = wowbg3_level.build(spec, tmp)
            for p in sorted(Path(tmp).rglob("*")):
                if not p.is_file() or p.name == "spec-out.json":
                    continue
                rel = p.relative_to(tmp).as_posix()
                data = p.read_bytes()
                # BG3 reads a level's meta, triggers and scenery as .lsf; its layer files stay .lsx, as in BG3's own levels.
                if rel.endswith(".lsx") and not rel.startswith("Layers/"):
                    rel, data = rel[:-4] + ".lsf", lsf_write.lsx_to_lsf(data)
                if rel.startswith("_Globals/"):  # the level's global objects (waypoint triggers): BG3's Globals folder
                    files.append((f"Mods/{FOLDER}/Globals/{name}/{rel[len('_Globals/'):]}", data))
                    continue
                files.append((f"Mods/{FOLDER}/Levels/{name}/{rel}", data))
    # The names BG3 shows for these levels (the load list, the loading screen): a key a level and its English text.
    # Without them BG3 shows the level's folder name.
    import loca_write
    lsx, texts = loca_write.level_names([json.loads(p.read_text()) for p in sorted((HERE / "levels").glob("*.json"))])
    files.append((f"Mods/{FOLDER}/Localization/Levels.lsf", lsf_write.lsx_to_lsf(lsx.encode("utf-8"))))
    # The dock guardian's dialog (tools/bg3_dialog.py, DockGuide.lua): the dialog, its bank entry, the flag its
    # "yes" sets, and its three lines in the same text table.
    import bg3_dialog
    files += bg3_dialog.files(FOLDER)
    texts.update(bg3_dialog.texts())
    files.append((f"Localization/English/{FOLDER}.loca", loca_write.loca(texts)))
    return files


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def copy_tree(src: Path, dst: Path):
    shutil.copytree(src, dst, symlinks=False)


# What of the packs a player's install never uses stays out of the release (a zip that carries a PAM module, Java
# archives and three copies of python reads as bloat, or worse).
# The server pack: its var/ and an older pack's sql/; of MariaDB, the Java archives of its CONNECT engine, its PAM
# helper, the mroonga and groonga files, a build macro and test data. What mariadb-install-db reads stays (the
# system, help, sys schema and test-db SQL, the error messages, the character sets).
SERVER_OUT = ("var", "var.*", "sql", "*.jar", "pam_user_map.so", "user_map.conf", "mroonga", "groonga",
              "groonga-normalizer-mysql", "aclocal", "mariadb_test_data_timezone.sql")
# The python pack: _dbm links Berkeley DB (Sleepycat licence) and tcl9 is tkinter's, removed already; build helpers
# (2to3, python3-config and its config folder), the turtle, numpy's tests and its Fortran tool; and libpython, which
# nothing loads (bin/python3 has the interpreter linked in, and no extension module names the library).
PYTHON_OUT = ("_dbm.cpython-*", "tcl9", "__pycache__", "2to3*", "python3*-config", "config-3.*-*", "turtle.py",
              "tests", "f2py", "libpython3*.so*")


def server_ignore(src):
    """copytree's ignore for the server pack: SERVER_OUT by name, and mariadb/lib (libmariadb twice: its own client
    programs have it linked in, and the copy mangosd and realmd load is the pack's lib/)."""
    by_name = shutil.ignore_patterns(*SERVER_OUT)

    def ignore(folder, names):
        out = set(by_name(folder, names))
        if Path(folder).name == "mariadb" and "lib" in names:
            out.add("lib")
        return out
    return ignore


def one_python(py: Path):
    """No links in the release's python, and one interpreter, bin/python3 (the name launcher.conf and
    wowbg3-server.sh call): a zip stores a link as another copy of its target, which made three pythons of 68 MB."""
    exe = py / "bin/python3"
    real = exe.resolve()
    for p in sorted(py.rglob("*")):
        if p.is_symlink():
            p.unlink()
    if real != exe and real.is_file():
        real.rename(exe)


# What a release must never carry: the server's database SQL (WoW's world, its texts), a table or seed made from
# it, and the player's own files' products. Checked on the finished folder.
GAME_DATA = (re.compile(r'\.sql(\.gz|\.7z|\.xz)?$', re.I), re.compile(r'(^|/)abilities[-_](seed|world)', re.I),
             re.compile(r'(^|/)(items|abilities|spells)\.json$'), re.compile(r'\.(dbc|mpq|m2|blp|wmo|adt|wdt|mmtile|vmtile|vmtree)$', re.I))
# MariaDB's own SQL (its system tables, help) is the database program's, not game data.
GAME_DATA_OK = re.compile(r'^server/mariadb/share/')


def check_no_game_data(folder: Path):
    bad = []
    for p in sorted(folder.rglob('*')):
        rel = p.relative_to(folder).as_posix()
        if p.is_file() and not GAME_DATA_OK.match(rel) and any(r.search(rel) for r in GAME_DATA):
            bad.append(rel)
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", default=os.environ.get("WOWBG3_VERSION") or RELEASE_VERSION)
    ap.add_argument("--out")
    ap.add_argument("--launcher", default=str(BUILD / "wowbg3-launcher" / LAUNCHER))
    ap.add_argument("--plugin", default=str(BUILD / "wowbg3-plugin" / PLUGIN))
    ap.add_argument("--render", action="append", default=None,
                    help="render plugin .so (repeatable; default WOWBG3_BUILD/wowbg3-render/wowbg3_render.so)")
    ap.add_argument("--server", help="the packed portable server folder (server/pack-portable.sh; has wowbg3-server.sh)")
    ap.add_argument("--benilla", help="the player build of benilla (WOWBG3_BUILD/benilla-target-sniper-player/release/benilla)")
    ap.add_argument("--cursor-plugin", default=str(BUILD / "wowbg3-early/wowbg3-early.so"),
                    help="wowbg3-early.so (WoW's quest cursors in BG3; WowCursor.lua needs it)")
    ap.add_argument("--fight-bridge", help="the fight bridge's sniper build (WOWBG3_BUILD/link-target-sniper/release/wow-bg3-link)")
    ap.add_argument("--bg3le", default=str(SNIPER / "bg3le/libbg3le.so"),
                    help="the bg3le build with WOW-BG3's patches (its sha256 should be patches/bg3le/build.lock.json's)")
    ap.add_argument("--python", help="a portable python with numpy, Pillow, lz4, zstandard (release/python/pack.sh): WOW-BG3/python")
    ap.add_argument("--fastlz", help="libfastlz.so built for sniper (Tav's textures)")
    ap.add_argument("--allow-missing", action="store_true")
    ap.add_argument("--source", action="append", default=[], help="NAME=DIR for the sources archive")
    ap.add_argument("--sources", help="upstream sources of the GPL/LGPL/MPL parts, made by release/licences/collect.py "
                    "(WOWBG3_BUILD/licences/sources): the sources archive's upstream/; the zip's server/bin must be their build")
    a = ap.parse_args()
    if not a.render:  # given ones replace the default (append would keep it)
        a.render = [str(BUILD / "wowbg3-render/wowbg3_render.so")]

    if not a.version or not a.out:
        ap.error("--out is required")
    if not re.fullmatch(r"\d+\.\d+\.\d+(\.\d+)?", a.version):
        ap.error("version is N.N.N or N.N.N.N")

    missing = []
    out = Path(a.out).resolve()
    name = f"WOW-BG3-{a.version}"
    dist = out / name
    if dist.exists():
        raise SystemExit(f"{dist} exists; pick a new version or move it aside")
    folder = dist / "WOW-BG3"
    folder.mkdir(parents=True)

    plugins = []
    for so in [a.launcher, a.plugin, *a.render, a.cursor_plugin]:
        p = Path(so)
        if p.is_file():
            shutil.copy2(p, folder / p.name)
            os.chmod(folder / p.name, 0o755)
            plugins.append(p.name)
        else:
            missing.append(f"plugin {so}")
    # The launcher loads first (name order), then wowbg3.so and the render plugin.
    plugins.sort()

    shutil.copy2(HERE / "launcher.conf.in", folder / "launcher.conf")
    shutil.copy2(HERE / "settings.conf", folder / "settings.conf")
    # The render plugin's colour table (a displayed colour to the light BG3 shows it as, measured once against BG3's
    # display transform). The plugin reads it from the player's data folder, where wowbg3-coord.py puts this copy.
    # Without it WoW's picture is fed into BG3 as one grey and nothing of WoW's light reaches BG3's characters.
    table = HERE.parent / "plugins/capture/fxbg-table.bin"
    if table.is_file():
        shutil.copy2(table, folder / "fxbg-table.bin")
    else:
        missing.append("plugins/capture/fxbg-table.bin")

    # The portable server pack, with this tree's wowbg3-server.sh (run, WOWBG3_SERVER_VAR) and SQL download
    # (fetch-sql.py, sql.lock). An older pack's sql/ (vmangos' database) stays out.
    if a.server and (Path(a.server) / "wowbg3-server.sh").is_file():
        shutil.copytree(a.server, folder / "server", symlinks=False, ignore=server_ignore(a.server))
        for f in ("wowbg3-server.sh", "fetch-sql.py", "sql.lock"):
            shutil.copy2(ROOT / "server" / f, folder / "server" / f)
        os.chmod(folder / "server/wowbg3-server.sh", 0o755)
        os.chmod(folder / "server/fetch-sql.py", 0o755)
    else:
        missing.append("server (wowbg3-server.sh)")
    # benilla: the binary, its world-mode HUD addon, its licences.
    ben = folder / "benilla"
    ben.mkdir()
    if a.benilla and Path(a.benilla).is_file():
        shutil.copy2(a.benilla, ben / "benilla")
        os.chmod(ben / "benilla", 0o755)
    else:
        missing.append("benilla (player build)")
    copy_tree(ROOT / "wow-addon/WowBg3Layout", ben / "AddOns/WowBg3Layout")
    (ben / "LICENSES").mkdir()
    for lic in ("LICENSE-MIT", "LICENSE-APACHE"):
        if (BENILLA_SRC / lic).is_file():
            shutil.copy2(BENILLA_SRC / lic, ben / "LICENSES" / lic)
    # tools: the coordinator, the PC tools it runs, the release's console, the fight bridge.
    tools = folder / "tools"
    (tools / "link/tools").mkdir(parents=True)
    (tools / "server/tools").mkdir(parents=True)
    (tools / "bin").mkdir()
    shutil.copy2(ROOT / "tools/wowbg3-coord.py", tools / "wowbg3-coord.py")
    shutil.copy2(ROOT / "tools/relenv.py", tools / "link/tools/relenv.py")
    shutil.copy2(ROOT / "tools/world-console.py", tools / "server/tools/world-console.py")
    for f in TOOLS + TOOLS_LATER:
        src = ROOT / "tools" / f
        if src.is_file():
            shutil.copy2(src, tools / "link/tools" / f)
        elif f in TOOLS:
            missing.append(f"tool tools/{f}")
        else:
            print(f"note: tools/{f} not there yet (left out)")
    # bg3le, WOW-BG3's build (the release ships everything a player needs):
    # our installer (release/bg3le-install.py) with bg3le's own install.py and launch wrapper, unchanged.
    b3 = folder / "bg3le"
    b3.mkdir()
    bg3le_note = ""
    if Path(a.bg3le).is_file():
        shutil.copy2(a.bg3le, b3 / "libbg3le.so")
        os.chmod(b3 / "libbg3le.so", 0o755)
        shutil.copy2(HERE / "bg3le-install.py", b3 / "install.py")
        shutil.copy2(BG3LE_SRC / "install.py", b3 / "bg3le_install.py")
        shutil.copy2(BG3LE_SRC / "installer/bg3le-launch", b3 / "bg3le-launch")
        os.chmod(b3 / "bg3le-launch", 0o755)
        os.chmod(b3 / "install.py", 0o755)
        lib_sha = sha256(b3 / "libbg3le.so")
        try:
            locked = json.loads((ROOT / "patches/bg3le/build.lock.json").read_text()).get("library_sha256")
        except (OSError, ValueError):
            locked = None
        bg3le_note = "bg3le library %s (patches/bg3le/build.lock.json library_sha256: %s)" % (
            lib_sha, "the same" if locked == lib_sha else "DIFFERS, %s" % locked)
        print(bg3le_note)
    else:
        missing.append("bg3le (libbg3le.so)")
    if a.python and (Path(a.python) / "bin/python3").exists():
        shutil.copytree(a.python, folder / "python", symlinks=True, ignore=shutil.ignore_patterns(*PYTHON_OUT))
        one_python(folder / "python")
    else:
        print("note: no --python: the runtime's python3 runs the tools; Tav stays untextured")
    if a.fastlz and Path(a.fastlz).is_file():
        shutil.copy2(a.fastlz, tools / "link/tools/libfastlz.so")
    if a.fight_bridge and Path(a.fight_bridge).is_file():
        shutil.copy2(a.fight_bridge, tools / "bin/wow-bg3-link")
        os.chmod(tools / "bin/wow-bg3-link", 0o755)
    else:
        missing.append("fight bridge (wow-bg3-link, sniper build)")
    wow = folder / "WoW"
    wow.mkdir()
    (wow / "PUT-YOUR-WOW-FILES-HERE.txt").write_text(
        "Copy your own World of Warcraft 1.12.1 (build 5875) client files into this folder,\n"
        "so that this folder holds Data/ (and WoW.exe). WOW-BG3 never runs WoW.exe; it reads Data/\n"
        "to build the server data and the textures on your machine.\n")

    # Licence texts of every third-party part: LICENSES/, made by release/licences/collect.py texts
    # from the build trees; THIRD-PARTY-NOTICES.md says which is which.
    if (ROOT / "LICENSES/README.txt").is_file():
        copy_tree(ROOT / "LICENSES", folder / "LICENSES")
    else:
        missing.append("LICENSES (release/licences/collect.py texts)")

    # The README is the repository's own, and it names the release version; a test build's zip says its own. Its
    # links go to docs/ and LICENSE: they come along, so they work offline (the developer pages too: Building from
    # source and Looking to contribute? link them).
    def versioned(text):
        return (text.replace(f"WOW-BG3-{RELEASE_VERSION}", f"WOW-BG3-{a.version}")
                .replace(f"WOW-BG3 {RELEASE_VERSION}", f"WOW-BG3 {a.version}"))
    (dist / "README.md").write_text(versioned((ROOT / "README.md").read_text(encoding="utf-8")), encoding="utf-8")
    shutil.copytree(ROOT / "docs", dist / "docs", ignore=shutil.ignore_patterns("__pycache__"))
    for page in (dist / "docs").glob("*.md"):
        page.write_text(versioned(page.read_text(encoding="utf-8")), encoding="utf-8")
    shutil.copy2(ROOT / "LICENSE", dist / "LICENSE")
    shutil.copy2(ROOT / "THIRD-PARTY-NOTICES.md", dist / "THIRD-PARTY-NOTICES.md")

    import tarfile
    spath = out / f"{name}-sources.tar.gz"
    if a.sources:  # the upstream sources must be complete and the shipped server their build
        up = Path(a.sources)
        sums = [l.split("  ", 1) for l in (up / "SOURCES.sha256").read_text().splitlines()
                if re.fullmatch(r"[0-9a-f]{64}  \S.*", l)]
        bad = [f for h, f in sums if not (up / f).is_file() or sha256(up / f) != h]
        built = [l.split("  ", 1) for l in (up / "vmangos/BUILT.sha256").read_text().splitlines() if l]
        bad += [f"server/{f} (not the build in {up}/vmangos)" for h, f in built
                if not (folder / "server" / f).is_file() or sha256(folder / "server" / f) != h]
        if not sums or not built or bad:
            raise SystemExit(f"--sources {up}: " + ("; ".join(bad) or "empty SOURCES.sha256 or BUILT.sha256"))
    no_cache = lambda ti: None if "__pycache__" in ti.name else ti
    with tarfile.open(spath, "w:gz") as t:
        for arc, d in [("README.md", HERE / "licences/SOURCES-README.md"),
                       ("wow-bg3-server-patches", ROOT / "patches/vmangos"),
                       ("wow-bg3-benilla-patches", ROOT / "patches/benilla"),
                       ("wow-bg3-bg3le-patches", ROOT / "patches/bg3le"),
                       ("wow-bg3-build/server-portable", ROOT / "server"),
                       ("wow-bg3-build/build-env.sh", HERE / "build-env.sh"),
                       ("wow-bg3-build/licences", HERE / "licences")]:
            t.add(str(d), arcname=f"{name}-sources/{arc}", filter=no_cache)
        if a.sources:
            t.add(str(Path(a.sources)), arcname=f"{name}-sources/upstream")
        for item in a.source:
            arc, _, d = item.partition("=")
            if not arc or not Path(d).is_dir():
                raise SystemExit(f"--source {item}: NAME=DIR with an existing DIR")
            t.add(d, arcname=f"{name}-sources/{arc}")
    if not a.sources:
        missing.append("upstream sources of the GPL/LGPL/MPL parts (--sources WOWBG3_BUILD/licences/sources)")

    links = [str(p.relative_to(dist)) for p in sorted(dist.rglob("*")) if p.is_symlink()]
    if links:  # the zip would hold each as a second copy of its target
        shutil.move(str(dist), str(out / f"{name}.incomplete"))
        raise SystemExit("links in the release: " + ", ".join(links[:20]) + f" (moved to {name}.incomplete)")

    bad = check_no_game_data(folder)
    if bad:
        shutil.move(str(dist), str(out / f"{name}.incomplete"))
        raise SystemExit("game data in the release: " + ", ".join(bad[:20]) + f" (moved to {name}.incomplete)")

    if missing and not a.allow_missing:
        shutil.move(str(dist), str(out / f"{name}.incomplete"))
        raise SystemExit("missing: " + "; ".join(missing) + f" (moved to {name}.incomplete)")

    # The pak last: its Version.lua carries the build id, made from every other file of the release.
    rest = [(str(p.relative_to(dist)), sha256(p)) for p in sorted(dist.rglob("*")) if p.is_file()]
    pak = pak_files(a.version, plugins, rest)
    lspk.write(str(dist / "WOW-BG3.pak"), pak)
    build = re.search(rb'Build = "([0-9a-f]{8})"', dict(pak)[f"Mods/{FOLDER}/ScriptExtender/Lua/{VERSION_LUA}"]).group(1).decode()
    print(f"WOW-BG3 {a.version} (build {build})")

    lines = [f"WOW-BG3 {a.version}  mod UUID {UUID}", f"BUILD {build}"]
    lines += [f"MISSING {m}" for m in missing]
    if bg3le_note:
        lines.append(bg3le_note)
    for p in sorted(dist.rglob("*")):
        if p.is_file():
            lines.append(f"{sha256(p)}  {p.relative_to(dist)}")
    (dist / "MANIFEST.txt").write_text("\n".join(lines) + "\n")

    zpath = out / f"{name}.zip"
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for p in sorted(dist.rglob("*")):
            rel = Path(name) / p.relative_to(dist)
            if p.is_dir():
                continue
            info = zipfile.ZipInfo.from_file(p, str(rel))
            info.external_attr = (stat.S_IMODE(p.stat().st_mode) | stat.S_IFREG) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with p.open("rb") as f:
                z.writestr(info, f.read())
    print(f"{dist}\n{zpath} ({zpath.stat().st_size} bytes)")
    if missing:
        print("MISSING (test build):", "; ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
