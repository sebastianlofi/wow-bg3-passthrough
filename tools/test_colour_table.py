#!/usr/bin/env python3
"""The render plugin's colour table is in the repository, in the release, and reaches the player's data folder.

A day of play reports (a black character, a pale ring round every figure, a grey haze) came from this one file missing
in an install: it had never been part of the release. These tests fail when any link of the chain is taken out."""
import importlib.util, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load(name, rel):
    spec = importlib.util.spec_from_file_location(name, str(ROOT / rel))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


ct = load('colour_table', 'tools/colour_table.py')
SOURCE = ROOT / 'plugins/capture' / ct.NAME


def test_the_table_is_in_the_repository_and_is_a_table():
    assert ct.problem(SOURCE) is None, ct.problem(SOURCE)


def test_the_table_has_the_size_the_plugin_reads():
    c = (ROOT / 'plugins/capture/wowbg3_fxbg.inc.c').read_text()
    assert '#define FB_TAB (17 * 17 * 17 * 3)' in c
    assert ct.VALUES == 17 * 17 * 17 * 3 and SOURCE.stat().st_size == ct.SIZE


def test_the_release_carries_the_table_and_the_tool_that_places_it():
    br = load('build_release', 'release/build-release.py')
    rel = (ROOT / 'release/build-release.py').read_text()
    assert 'plugins/capture/fxbg-table.bin' in rel and 'folder / "fxbg-table.bin"' in rel
    # a build without the file must fail, not ship
    assert 'missing.append("plugins/capture/fxbg-table.bin")' in rel
    assert 'colour_table.py' in br.TOOLS


def test_the_coordinator_places_it_at_world_start():
    coord = (ROOT / 'tools/wowbg3-coord.py').read_text()
    assert 'import colour_table' in coord
    assert "colour_table.place(HOME / colour_table.NAME, CAPTURE)" in coord


def test_the_plugin_says_so_when_it_has_none():
    c = (ROOT / 'plugins/capture/wowbg3_fxbg.inc.c').read_text()
    assert 'NO COLOUR TABLE' in c and 'g_fbg.thave' in c


def test_place_fills_an_empty_data_folder():
    with tempfile.TemporaryDirectory() as d:
        cap = Path(d) / 'capture'
        ok, text = ct.place(SOURCE, cap)
        assert ok and 'placed' in text
        assert (cap / ct.NAME).read_bytes() == SOURCE.read_bytes()
        assert ct.place(SOURCE, cap) == (True, '')


def test_place_replaces_a_broken_or_old_table():
    with tempfile.TemporaryDirectory() as d:
        cap = Path(d) / 'capture'
        cap.mkdir()
        (cap / ct.NAME).write_bytes(b'\0' * ct.SIZE)
        assert ct.problem(cap / ct.NAME) == 'every colour reads as the same light (an empty table)'
        ok, text = ct.place(SOURCE, cap)
        assert ok and (cap / ct.NAME).read_bytes() == SOURCE.read_bytes()


def test_place_says_no_colour_table_when_there_is_none_anywhere():
    with tempfile.TemporaryDirectory() as d:
        ok, text = ct.place(Path(d) / 'nothing.bin', Path(d) / 'capture')
        assert not ok and 'NO COLOUR TABLE' in text


def test_place_keeps_a_good_table_when_the_release_has_none():
    with tempfile.TemporaryDirectory() as d:
        cap = Path(d) / 'capture'
        ct.place(SOURCE, cap)
        ok, text = ct.place(Path(d) / 'nothing.bin', cap)
        assert ok and 'keeping' in text and ct.problem(cap / ct.NAME) is None
