#!/usr/bin/env python3
"""The release ships no Blizzard-derived data: release/build-release.py's check and the server pack's scripts."""
import importlib.util, re, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_release', str(ROOT / 'release/build-release.py'))
br = importlib.util.module_from_spec(spec)
spec.loader.exec_module(br)


def test_check_finds_game_data():
    with tempfile.TemporaryDirectory() as d:
        f = Path(d)
        for rel in ('server/sql/world.sql', 'server/sql/migrations/world_db_updates.sql', 'tools/link/tools/abilities-seed.json.gz',
                    'server/mariadb/share/mariadb_system_tables.sql', 'server/wowbg3-server.sh', 'server/fetch-sql.py',
                    'server/sql.lock', 'tools/link/tools/wow_abilities_rules.json', 'x/Interface.MPQ', 'x/link/items.json'):
            (f / rel).parent.mkdir(parents=True, exist_ok=True)
            (f / rel).write_text('x')
        assert br.check_no_game_data(f) == ['server/sql/migrations/world_db_updates.sql', 'server/sql/world.sql',
                                            'tools/link/tools/abilities-seed.json.gz', 'x/Interface.MPQ', 'x/link/items.json']


def test_nothing_ships_the_sql_or_a_seed():
    pack = (ROOT / 'server/pack-portable.sh').read_text()
    assert 'docker cp' not in pack and 'VMANGOS_DB_IMAGE' not in pack and 'fetch-sql.py' in pack and 'sql.lock' in pack
    rel = (ROOT / 'release/build-release.py').read_text()
    assert 'abilities_seed' not in rel and 'abilities-seed.json.gz"' not in rel
    assert not any('seed' in t for t in br.TOOLS + br.TOOLS_LATER)
    srv = (ROOT / 'server/wowbg3-server.sh').read_text()
    assert '$ROOT/sql/' not in srv and 'fetch-sql.py" fetch' in srv
    # The server's pins are in one place, sourced by the build too.
    assert 'sql.lock' in (ROOT / 'server/pins.sh').read_text()


def test_first_start_stats_wait():
    """bg3le patch 0008 follows 0006 on its file, and the coordinator writes the file it waits on."""
    import hashlib, json
    lock = json.loads((ROOT / 'patches/bg3le/build.lock.json').read_text())
    after = {}
    for p in lock['runtime_patches']:
        f = ROOT / 'patches/bg3le' / Path(p['file']).name
        assert hashlib.sha256(f.read_bytes()).hexdigest() == p['sha256'], p['file']
        for t in p['targets']:
            if t['path'] in after:
                assert t['before_sha256'] == after[t['path']], (p['file'], t['path'])
            after[t['path']] = t['after_sha256']
    assert any('0008-first-start-stats-wait' in p['file'] for p in lock['runtime_patches'])
    patch = (ROOT / 'patches/bg3le/0008-first-start-stats-wait.patch').read_text()
    coord = (ROOT / 'tools/wowbg3-coord.py').read_text()
    assert '".wait"' in patch and "'boot-path-overrides.wait'" in coord and 'ws.st_mtime < 10' in patch.replace('std::time(nullptr) - ', '')


def test_usage_shows_the_whole_header():
    srv = (ROOT / 'server/wowbg3-server.sh').read_text().split('\n')
    end = next(i for i, l in enumerate(srv) if l.startswith('set -euo'))
    m = re.search(r"sed -n '2,(\d+)p' \"\$0\"", '\n'.join(srv))
    assert m and int(m.group(1)) == end, (m and m.group(1), end)


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_'):
            fn()
            print('ok', name)
