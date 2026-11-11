"""A level of our own is shown by a name, not by its folder's (WOWBG3_Azeroth_A in the load list: a playtest report).
The release pak carries the key BG3 looks the name up by and its English text."""
import importlib.util
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import loca_write  # noqa: E402
import lsf_write  # noqa: E402


def specs():
    return [json.loads(p.read_text()) for p in sorted((ROOT / 'release/levels').glob('*.json'))]


def test_every_level_has_a_shown_name():
    for s in specs():
        assert s.get('DisplayName'), s['Name']
        assert s['DisplayName'] != s['Name'] and '_' not in s['DisplayName'], s['DisplayName']


def test_the_first_level_is_azeroth():
    assert {s['Name']: s['DisplayName'] for s in specs()}['WOWBG3_Azeroth_A'] == 'Azeroth'


def test_loca_layout():
    data = loca_write.loca({'hb': 'Two', 'ha': 'One'})
    assert data[:4] == b'LOCA' and struct.unpack_from('<II', data, 4) == (2, 12 + 70 * 2)
    key, version, length = struct.unpack_from('<64sHI', data, 12)
    assert key.rstrip(b'\0') == b'ha' and version == 1 and length == 4
    assert data[12 + 140:] == b'One\0Two\0'


def test_the_key_table_as_bg3_reads_its_own():
    """BG3's own Levels.lsf: a node a level, Content a translated string (u16 version, i32 length, handle), UUID the
    level's name."""
    lsx, texts = loca_write.level_names([{'Name': 'WOWBG3_Test_A', 'DisplayName': 'Test'}])
    h = loca_write.handle('WOWBG3_Test_A')
    assert texts == {h: 'Test'} and len(h) == 37 and h[0] == 'h' and '-' not in h
    lsf = lsf_write.lsx_to_lsf(lsx.encode('utf-8'))
    assert lsf[:4] == b'LSOF'
    assert struct.pack('<Hi', 1, 38) + h.encode() + b'\0' in lsf
    assert b'WOWBG3_Test_A\0' in lsf
    for name in (b'TranslatedStringKeys', b'TranslatedStringKey', b'Content', b'UUID', b'Stub', b'Speaker', b'ExtraData'):
        assert name in lsf, name


def test_a_handle_stays_the_same():
    assert loca_write.handle('WOWBG3_Azeroth_A') == loca_write.handle('WOWBG3_Azeroth_A')
    assert loca_write.handle('WOWBG3_Azeroth_A') != loca_write.handle('WOWBG3_Azeroth_B')


def test_the_release_pak_carries_both_files():
    spec = importlib.util.spec_from_file_location('br', ROOT / 'release/build-release.py')
    br = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(br)
    files = dict(br.level_files())
    table = files[f'Mods/{br.FOLDER}/Localization/Levels.lsf']
    text = files[f'Localization/English/{br.FOLDER}.loca']
    for s in specs():
        h = loca_write.handle(s['Name']).encode()
        assert s['Name'].encode() + b'\0' in table and h + b'\0' in table, s['Name']
        assert h in text and s['DisplayName'].encode('utf-8') + b'\0' in text, s['Name']
