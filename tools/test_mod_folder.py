#!/usr/bin/env python3
"""Everything the mod makes lives in its own folder beside the pak, Mods/WOW-BG3/data (its IO folder data/io).

A play install kept the server, snapshots and logs in ~/.local/share/wow-bg3 and the Lua's files in BG3's
Script Extender/wow-bg3: outside the mod's folder. The harness links those old names to the new folders in its own
test profiles, so a shipped file that still named one would work there and fail a player. These tests read the
shipped sources instead."""
import glob, re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OLD_DATA = 'local/share/wow-bg3'
OLD_IO = 'Script Extender/wow-bg3'


def shipped():
    src = (ROOT / 'release/build-release.py').read_text()
    block = src[src.index('TOOLS = ['):src.index(']', src.index('TOOLS_LATER = [')) + 1]
    files = ['tools/' + t for t in re.findall(r'"([\w\-]+\.py)"', block)]
    files += ['tools/wowbg3-coord.py', 'tools/relenv.py', 'tools/world-console.py', 'release/launcher.conf.in']
    for pat in ('server/*.sh', 'mod/ScriptExtender/Lua/*.lua', 'plugins/**/*.c', 'plugins/**/*.cpp', 'plugins/**/*.h'):
        files += [str(Path(f).relative_to(ROOT)) for f in glob.glob(str(ROOT / pat), recursive=True)]
    return [f for f in sorted(set(files)) if (ROOT / f).is_file()]


def lines(name):
    for n, line in enumerate((ROOT / name).read_text(errors='replace').splitlines(), 1):
        yield n, line


def test_nothing_shipped_names_the_old_data_folder():
    bad = ['%s:%d' % (f, n) for f in shipped() for n, l in lines(f) if OLD_DATA in l]
    assert not bad, bad


# The stock-bg3le fallbacks (a bg3le without WOW-BG3's patch 0012 keeps Ext.IO's wow-bg3/ in its Script Extender
# folder): each beside the code that asks for the mod's own folder first.
FALLBACKS = {
    'plugins/launcher/wowbg3_launcher.c': 'bg3le_io_root_add',
    'mod/ScriptExtender/Lua/WowUi.lua': 'Getenv("WOWBG3_IO")',
    'mod/ScriptExtender/Lua/InventoryClient.lua': 'Getenv("WOWBG3_IO")',
}


def test_the_old_io_folder_is_only_a_harness_default_or_a_stock_bg3le_fallback():
    bad = []
    for f in shipped():
        text = (ROOT / f).read_text(errors='replace')
        for n, l in lines(f):
            old = OLD_IO in l or re.search(r'SE_DIR \.\. "/wow-bg3"', l)
            if not old or 'bg3/private' in l or 'harness A' in l or l.lstrip().startswith(('#', '//', '--', '*')):
                continue  # harness A's development profile (relenv points the tool elsewhere in the release), a comment
            if f in FALLBACKS and FALLBACKS[f] in text:
                continue
            if f == 'tools/wowbg3cmd.py' and '"Script Extender/wow-bg3")' in l:
                continue  # A_IO, harness A's profile: used only when WOWBG3_IO is unset (the launcher always sets it)
            if f == 'tools/wowchar.py' and 'hero.json' in l:
                continue  # HERO, harness A's profile: relenv points it at the IO folder
            bad.append('%s:%d %s' % (f, n, l.strip()))
    assert not bad, bad


def test_relenv_points_every_tool_whose_default_is_harness_a():
    rel = (ROOT / 'tools/relenv.py').read_text()
    modules = re.search(r"MODULES = \[(.*?)\]", rel, re.S).group(1)
    for f in shipped():
        if f.startswith('tools/') and any(OLD_IO in l and 'bg3/private' in l for _, l in lines(f)):
            name = Path(f).stem
            assert name in ('wowbg3cmd',) or "'%s'" % name in modules, f


def test_the_launcher_and_plugins_name_the_mods_own_folder():
    launcher = (ROOT / 'plugins/launcher/wowbg3_launcher.c').read_text()
    assert '"%s/data", g_home' in launcher and '"%s/io", g_data' in launcher and 'add("wow-bg3", g_io)' in launcher
    capture = (ROOT / 'plugins/capture/wowbg3_capture.c').read_text()
    assert 'Mods/WOW-BG3/data' in capture and '"%s/io", data' in capture
    assert '/data/io' in (ROOT / 'plugins/early/wowbg3_early.c').read_text()
    assert '/data/io/' in (ROOT / 'plugins/wowbg3_plugin.cpp').read_text()
    patch = (ROOT / 'patches/bg3le/0012-mod-io-folder.patch').read_text()
    assert 'bg3le_io_root_add' in patch and 'resolve_user' in patch


def test_the_server_runs_from_a_folder_with_spaces_and_a_deep_path():
    """The data folder is in BG3's "Larian Studios/Baldur's Gate 3/Mods": MariaDB's install script splits paths at
    spaces, a socket path holds 107 bytes at most, and a config written before the move named the old folder."""
    srv = (ROOT / 'server/wowbg3-server.sh').read_text()
    assert '(cd "$V" && "$mdb_rel/scripts/mariadb-install-db"' in srv and '--datadir=./db' in srv
    assert 'case "$ROOT$V" in' in srv
    assert '[ "${#SOCK}" -gt 100 ]' in srv and 'echo "$SOCK" > "$RUN/socket"' in srv
    start = srv[srv.index('\nstart() {'):]
    assert start.index('"DataDir=') < start.index('bin/mangosd'), 'the folders are set at every start'
    import importlib.util, sys, tempfile
    sys.path.insert(0, str(ROOT / 'tools'))
    spec = importlib.util.spec_from_file_location('wowserver', str(ROOT / 'tools/wowserver.py'))
    ws = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(ws)
    with tempfile.TemporaryDirectory() as t:
        var = Path(t) / "Larian Studios/Baldur's Gate 3/Mods/WOW-BG3/data/server"
        (var / 'run').mkdir(parents=True)
        assert ws.db_socket(var) == var / 'run/mariadb.sock'
        (var / 'run/socket').write_text('/dev/shm/wow-bg3-1000/mariadb-1a2b3c4d.sock\n')
        assert str(ws.db_socket(var)) == '/dev/shm/wow-bg3-1000/mariadb-1a2b3c4d.sock'
    for tool in ('tools/wowbg3-coord.py', 'tools/wowbg3cmd.py', 'tools/wowserver.py'):
        assert "'run/mariadb.sock'" not in (ROOT / tool).read_text().replace("Path(var) / 'run/mariadb.sock'", ''), tool


def test_a_server_failure_of_an_earlier_bg3_does_not_count_for_this_one():
    """A first start that failed, then BG3 closed: the next BG3's coordinator read that "failed" before its own
    server had started, and the load never tried again (seen on a test PC)."""
    coord = (ROOT / 'tools/wowbg3-coord.py').read_text()
    fn = coord[coord.index('def server_state():'):coord.index('def server_note(')]
    assert "if st != 'running' and f.stat().st_mtime < STARTED:" in fn and "return 'none'" in fn
    assert coord.index('STARTED = time.time()') < coord.index('def server_state():')


def test_nothing_shipped_matches_paths_by_the_old_io_folders_name():
    """The minimap's picture is found by its path: matching "/wow-bg3/link/" (the old folder's name) found nothing once
    the IO folder moved to Mods/WOW-BG3/data/io, and the minimap was empty."""
    bad = []
    for f in shipped():
        for n, l in lines(f):
            s = l.strip()
            if s.startswith(('//', '#', '--', '*')):
                continue
            if re.search(r'["\']/wow-bg3/(link|gui|cursors)/', l):
                bad.append('%s:%d %s' % (f, n, s))
    assert not bad, bad
    plugin = (ROOT / 'plugins/wowbg3_plugin.cpp').read_text()
    # BG3's GUI hands the minimap's URI without the root's "/" ("home/<user>/..."): the match must drop it too.
    assert 'io_dir() + "link/minimap/"' in plugin and "t.find_first_not_of('/')" in plugin


if __name__ == '__main__':
    for name, fn in sorted(globals().items()):
        if name.startswith('test_'):
            fn()
            print('ok', name)
