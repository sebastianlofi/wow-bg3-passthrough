"""The dock guardian's dialog (tools/bg3_dialog.py, DockGuide.lua) without a game: the three files BG3 loads a dialog
from, the lines, the names the mod's Lua uses for them, and that the release carries them."""
import importlib.util
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import bg3_dialog as D  # noqa: E402

LUA = ROOT / 'mod/ScriptExtender/Lua/DockGuide.lua'
try:
    import lupa
except ImportError:
    lupa = None


def nodes(lsx, name):
    return [n for n in ET.fromstring(lsx).iter('node') if n.get('id') == name]


def attrs(n):
    return {a.get('id'): a.get('value') if a.get('handle') is None else a.get('handle') for a in n.findall('attribute')}


def test_the_dialog_is_one_line_and_two_answers():
    lsx = D.dock_guardian_lsx()
    lines = {attrs(n)['UUID']: n for n in nodes(lsx, 'node')}
    root = attrs(nodes(lsx, 'RootNodes')[0])['RootNodes']
    ask = lines[root]
    assert attrs(ask)['constructor'] == 'TagGreeting' and attrs(ask)['speaker'] == '0' and attrs(ask)['Root'] == 'True'
    answers = [attrs(c)['UUID'] for c in ask.iter('node') if c.get('id') == 'child']
    assert len(answers) == 2
    texts = []
    for key in [root] + answers:
        n = lines[key]
        texts.append(attrs([t for t in n.iter('node') if t.get('id') == 'TagText'][0])['TagText'])
        if key != root:
            assert attrs(n)['constructor'] == 'TagQuestion' and attrs(n)['speaker'] == '1' and attrs(n)['endnode'] == 'True'
    assert texts == [D.ASK, D.YES, D.NO]
    # Only "yes" sets the flag, on the player who said it (speaker 1).
    flags = [(attrs(f)['UUID'], attrs(f)['paramval']) for f in lines[answers[0]].iter('node') if f.get('id') == 'flag']
    assert flags == [(D.GO, '1')]
    assert not [f for f in lines[answers[1]].iter('node') if f.get('id') == 'flag']


def test_the_speakers_are_bg3s_own_groups():
    speakers = [attrs(s) for s in nodes(D.dock_guardian_lsx(), 'speaker')]
    assert [(s['index'], s['list']) for s in speakers] == [('0', D.GUARDIANS), ('1', D.PLAYERS)]


def test_the_bank_names_the_dialog_for_the_story():
    r = attrs(nodes(D.bank_lsx([(D.BANK, D.NAME, 2)], 'WOWBG3'), 'Resource')[0])
    assert r['ID'] == D.BANK and r['Name'] == D.NAME and r['EnableTimeline'] == 'False'
    assert r['SourceFile'] == 'Mods/WOWBG3/Story/Dialogs/WOWBG3_DockGuardian.lsj'


def test_the_files_are_lsf_at_the_paths_bg3_reads():
    files = dict(D.files('WOWBG3'))
    assert sorted(files) == ['Mods/WOWBG3/Story/DialogsBinary/WOWBG3_DockGuardian.lsf',
                             'Public/WOWBG3/Content/Assets/Dialogs/[PAK]_WOWBG3/_merged.lsf',
                             'Public/WOWBG3/Flags/%s.lsf' % D.GO]
    for data in files.values():
        assert data[:4] == b'LSOF' and struct.unpack_from('<I', data, 4)[0] == 7
    # The flag is an object flag, as BG3's own dialog flags on a speaker are.
    flag = attrs(nodes(D.flag_lsx(D.GO, D.GO_NAME, ''), 'Flags')[0])
    assert flag['Usage'] == '4' and flag['UUID'] == D.GO


def test_every_line_has_a_text():
    assert set(D.texts()) == {D.ASK, D.YES, D.NO}
    assert all(re.fullmatch(r'h[0-9a-f]{8}(g[0-9a-f]{4}){3}g[0-9a-f]{12}', h) for h in D.texts())


def test_the_mods_lua_uses_the_same_names():
    lua = LUA.read_text()
    assert 'M.DIALOG = "%s_%s"' % (D.NAME, D.BANK) in lua
    assert 'M.GO = "%s"' % D.GO in lua
    for h in (D.ASK, D.YES, D.NO):
        assert '"%s"' % h in lua


def test_the_release_carries_the_dialog_and_starts_the_module():
    build = (ROOT / 'release/build-release.py').read_text()
    assert 'bg3_dialog.files(FOLDER)' in build and 'texts.update(bg3_dialog.texts())' in build
    assert '"DockGuide.lua"' in (ROOT / 'mod/ScriptExtender/Lua/WorldProbe.lua').read_text()
    assert 'Ext.Require("DockGuide.lua").Client(runId)' in (ROOT / 'mod/ScriptExtender/Lua/WorldMode.lua').read_text()


def module():
    L = lupa.LuaRuntime(unpack_returned_tuples=True)
    L.execute('Ext = {Require = function() return {} end}; Osi = {}')
    return L, L.execute(LUA.read_text())


def test_the_lines_name_the_place():
    if lupa is None:
        return
    L, M = module()
    t = M.Lines('Ratchet, The Barrens')
    assert (t['Ask'], t['Yes'], t['No']) == ('Travel to Ratchet, The Barrens?', 'Yes, take me to Ratchet, The Barrens.', 'Not now.')


def test_the_guardian_stands_at_the_nearest_pier_within_reach():
    if lupa is None:
        return
    L, M = module()
    near = L.eval('''function(M)
        local docks = {{Key = "295:2", Name = "Auberdine", Wow = {-3709.5, -575.1, 6.0}},
                       {Key = "302:1", Name = "Theramore", Wow = {-3690.0, -600.0, 6.0}}}
        local centre = {1000.0, 50.0, 2000.0}
        local function at(wow) return M.ToBg3(centre, wow) end
        local a, pa, ma = M.Nearest(docks, centre, at({-3708.0, -576.0, 6.0}), 55.0)
        local b = M.Nearest(docks, centre, at({-3691.0, -599.0, 6.0}), 55.0)
        local c = M.Nearest(docks, centre, at({-3500.0, -575.0, 6.0}), 55.0)
        local p = M.ToBg3(centre, {10.0, 20.0, 30.0})
        return a.Key, ma, b.Key, c == nil, p[1], p[2], p[3], pa[2]
    end''')(M)
    a, ma, b, none, x, y, z, pier_y = near
    assert a == '295:2' and ma < 2.0 and b == '302:1' and none is True
    # BG3 = Centre + (WoW y, WoW z, -WoW x) * 0.9144
    assert abs(x - (1000.0 + 20 * 0.9144)) < 1e-6 and abs(y - (50.0 + 30 * 0.9144)) < 1e-6 and abs(z - (2000.0 - 10 * 0.9144)) < 1e-6
    assert abs(pier_y - (50.0 + 6 * 0.9144)) < 1e-6
