#!/usr/bin/env python3
"""The line under BG3's main menu says which WOW-BG3 this is: "WOW-BG3 0.1.0 (build 1a2b3c4d)".

The release builder writes the version and a build id into the pak (Version.lua); MenuLine.lua puts them after the
Script Extender's line, in a BG3 that loaded the mod. A release number stays the same over several builds, so the
id is what tells two of them apart on screen: these tests fail when it stops following the release's files, when
the pak loses the file, or when the line could land before the extender's or twice.

  python3 tools/test_menu_line.py     (the Lua part needs the `lupa` module; without it that part is skipped)
"""
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LUA = ROOT / 'mod/ScriptExtender/Lua'
sys.path.insert(0, str(ROOT / 'release'))
spec = importlib.util.spec_from_file_location('build_release', str(ROOT / 'release/build-release.py'))
br = importlib.util.module_from_spec(spec)
spec.loader.exec_module(br)

PAK = [('Mods/WOWBG3/meta.lsx', b'meta'), ('Mods/WOWBG3/ScriptExtender/Lua/A.lua', b'return 1\n')]
REST = [('WOW-BG3/wowbg3.so', 'aa' * 32), ('WOW-BG3/bg3le/libbg3le.so', 'bb' * 32)]


def test_the_build_id_follows_every_file_of_the_pak_and_of_the_release():
    base = br.build_id(PAK, REST)
    assert re.fullmatch(r'[0-9a-f]{8}', base)
    assert br.build_id(list(reversed(PAK)), list(reversed(REST))) == base, 'the order of the files must not matter'
    assert br.build_id(PAK[:1] + [(PAK[1][0], b'return 2\n')], REST) != base, 'a changed Lua file'
    assert br.build_id(PAK + [('Mods/WOWBG3/ScriptExtender/Lua/B.lua', b'')], REST) != base, 'a new file in the pak'
    assert br.build_id(PAK, REST[:1] + [(REST[1][0], 'cc' * 32)]) != base, 'a rebuilt extender'
    assert br.build_id(PAK, REST[:1]) != base, 'a file gone from the release'
    assert br.build_id(PAK) != base, 'a pak made alone is not the release'


def test_version_lua_says_the_version_and_the_build():
    text = br.version_lua('0.1.0', '1a2b3c4d').decode()
    assert 'return {Version = "0.1.0", Build = "1a2b3c4d"}' in text


def test_the_source_tree_has_no_version_file_and_the_builder_writes_it_last():
    assert not (LUA / br.VERSION_LUA).exists(), 'Version.lua is written by the release builder'
    src = (ROOT / 'release/build-release.py').read_text()
    assert 'files.append((f"Mods/{FOLDER}/ScriptExtender/Lua/{VERSION_LUA}", version_lua(version, build_id(files, release))))' in src
    # the release's pak is made after every other file is in place, from their hashes
    main = src[src.index('def main():'):]
    assert main.index('rest = [(str(p.relative_to(dist)), sha256(p))') < main.index('lspk.write(str(dist / "WOW-BG3.pak"), pak)') \
        < main.index('(dist / "MANIFEST.txt").write_text')
    assert main.count('lspk.write(') == 1


def test_the_client_starts_the_line_inside_the_module_check():
    c = (LUA / 'BootstrapClient.lua').read_text()
    assert c.index('Ext.Require("Module.lua").Start("client"') < c.index('Ext.Require("MenuLine.lua").Start()')
    assert 'MenuLine' not in (LUA / 'BootstrapServer.lua').read_text()


def test_the_line_goes_after_the_extenders_and_only_once():
    try:
        import lupa
    except ImportError:
        print('test_menu_line.py: the Lua part needs the lupa module; skipped')
        return
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.execute("""
        strings, logged = {}, {}
        Ext = {Log = {Print = function(s) logged[#logged + 1] = s end},
               Loca = {GetTranslatedString = function(h) return strings[h] end,
                       UpdateTranslatedString = function(h, t) strings[h] = t end},
               Events = {GameStateChanged = {Subscribe = function(self, f) changed = f end}},
               Require = function(name) return {Version = "0.1.0", Build = "1a2b3c4d"} end}
    """)
    M = L.execute((LUA / 'MenuLine.lua').read_text())
    g = L.globals()
    assert M.Line() == 'WOW-BG3 0.1.0 (build 1a2b3c4d)'
    copyright_line = '(c) Larian Studios. All rights reserved.'
    g.strings[M.HANDLE] = copyright_line
    M.Start()
    assert g.strings[M.HANDLE] == copyright_line, "not before the extender's line is there"
    ext = copyright_line + '\r\nScript Extender v32 loaded (bg3le, WOW-BG3 build), built on Oct 10 2026 18:54:29.'
    g.strings[M.HANDLE] = ext
    g.changed()
    assert g.strings[M.HANDLE] == ext + '\r\nWOW-BG3 0.1.0 (build 1a2b3c4d)'
    g.changed()
    g.changed()
    assert g.strings[M.HANDLE].count('WOW-BG3 0.1.0') == 1, 'once, however often the game state changes'
    g.strings[M.HANDLE] = ext   # BG3 loaded its texts anew and the extender wrote its line again
    g.changed()
    assert g.strings[M.HANDLE].endswith('\r\nWOW-BG3 0.1.0 (build 1a2b3c4d)')
    # the extender's present wording on Linux counts as its line too
    g.strings[M.HANDLE] = copyright_line + '\r\nbg3le loaded, Script Extender v32 API, built on Oct 10 2026 18:54:29.'
    g.changed()
    assert g.strings[M.HANDLE].endswith('\r\nWOW-BG3 0.1.0 (build 1a2b3c4d)')


if __name__ == '__main__':
    for k, f in sorted(globals().items()):
        if k.startswith('test_') and callable(f):
            f()
            print('ok', k)
