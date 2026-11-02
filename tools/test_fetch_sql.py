#!/usr/bin/env python3
"""Tests of server/fetch-sql.py (the first start's database download): no network, no game data. A tiny 7z and a
tiny core archive are made here; the real ones were checked on a PC against sql.lock's hashes."""
import hashlib, importlib.util, io, lzma, struct, sys, tarfile, tempfile, urllib.error
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('fetch_sql', str(ROOT / 'server/fetch-sql.py'))
f = importlib.util.module_from_spec(spec)
spec.loader.exec_module(f)


def num(v):
    """7z's variable-length number."""
    if v < 0x80:
        return bytes([v])
    if v < 0x4000:
        return bytes([0x80 | (v >> 8), v & 0xFF])
    return bytes([0xFF]) + struct.pack('<Q', v)


def make_7z(data, lzma2=False):
    """One file, one coder, plain header: as 7-Zip writes a single file with -m0=LZMA (or LZMA2)."""
    if lzma2:
        packed = lzma.compress(data, lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA2, 'dict_size': 1 << 16}])
        coder = bytes([0x21 | 0x20, 0x21]) + num(1) + bytes([8])  # flag: id size 1, has props; dict 2^16 (bits 8)
    else:
        alone = lzma.compress(data, lzma.FORMAT_ALONE, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': 1 << 16}])
        props, packed = alone[:5], alone[13:]
        coder = bytes([0x03 | 0x20]) + b'\x03\x01\x01' + num(5) + props
    head = (b'\x01\x04'                                          # Header, MainStreamsInfo
            + b'\x06' + num(0) + num(1) + b'\x09' + num(len(packed)) + b'\x00'
            + b'\x07\x0b' + num(1) + b'\x00' + num(1) + coder
            + b'\x0c' + num(len(data)) + b'\x00'
            + b'\x00\x00')                                     # (no files info needed by the reader), end
    start = struct.pack('<QQI', len(packed), len(head), 0)
    return b"7z\xbc\xaf\x27\x1c\x00\x04" + struct.pack('<I', 0) + start + packed + head


def test_7z_lzma_and_lzma2():
    data = b''.join(b'INSERT INTO t VALUES (%d);\n' % i for i in range(20000))
    with tempfile.TemporaryDirectory() as d:
        for two in (False, True):
            p = Path(d) / 'a.7z'
            p.write_bytes(make_7z(data, two))
            out = io.BytesIO()
            assert f.unpack_7z(p, out.write) == len(data)
            assert out.getvalue() == data, two
        p.write_bytes(b'PK\x03\x04 not a 7z')
        try:
            f.unpack_7z(p, io.BytesIO().write)
            assert False
        except ValueError:
            pass


def core_tar(path, commit, files):
    with tarfile.open(path, 'w:gz', format=tarfile.PAX_FORMAT, pax_headers={'comment': commit}) as t:
        for name, body in files.items():
            info = tarfile.TarInfo('core-%s/%s' % (commit, name))
            info.size = len(body)
            t.addfile(info, io.BytesIO(body))


CORE = {'sql/characters.sql': b'-- chars\n', 'sql/logon.sql': b'-- logon\n', 'sql/logs.sql': b'-- logs\n',
        # merge.sh: every *_NAME.sql, in sort order of the path, whatever order the archive has them in
        'sql/migrations/20210620000000_world.sql': b'B\n', 'sql/migrations/20210515000000_world.sql': b'A\n',
        'sql/migrations/20220101000000_characters.sql': b'C\n', 'sql/migrations/README': b'x',
        'src/game/World.cpp': b'code'}


def lock_for(d, world, commit='abc123'):
    """A sql.lock for these files, as the real one pins the real ones."""
    h = lambda b: hashlib.sha256(b).hexdigest()
    seven = make_7z(world)
    p = {'WORLD_DB_URL': 'https://example.invalid/db/world_full.7z', 'WORLD_DB_SHA256': h(seven), 'WORLD_DB_SIZE': str(len(seven)),
         'CORE_SQL_URL': 'https://example.invalid/core.tar.gz', 'CORE_SQL_COMMIT': commit, 'CORE_SQL_SIZE': '0',
         'SQL_SHA256_world': h(world), 'SQL_SHA256_characters': h(b'-- chars\n'), 'SQL_SHA256_logon': h(b'-- logon\n'),
         'SQL_SHA256_logs': h(b'-- logs\n'), 'SQL_SHA256_world_updates': h(b'A\nB\n'), 'SQL_SHA256_characters_updates': h(b'C\n'),
         'SQL_SHA256_logon_updates': h(b''), 'SQL_SHA256_logs_updates': h(b'')}
    tar = Path(d) / 'served-core.tar.gz'
    core_tar(tar, commit, CORE)
    return p, {p['WORLD_DB_URL']: seven, p['CORE_SQL_URL']: tar.read_bytes()}


class Resp(io.BytesIO):
    def __init__(self, body):
        super().__init__(body)
        self.headers = {'Content-Length': str(len(body))}


def opener_for(served, calls):
    def opener(req):
        calls.append(req.full_url)
        if req.full_url not in served:
            raise urllib.error.URLError('Name or service not known')
        return Resp(served[req.full_url])
    return opener


def test_fetch_makes_checked_sql_and_caches_downloads():
    world = b'CREATE TABLE creature_template (entry int);\n' * 500
    with tempfile.TemporaryDirectory() as d:
        p, served = lock_for(d, world)
        calls, out, cache = [], Path(d) / 'sql', Path(d) / 'dl'
        prog = f.Progress(str(Path(d) / 'progress'))
        f.fetch(out, cache, prog, p, opener_for(served, calls))
        assert (out / 'world.sql').read_bytes() == world
        assert (out / 'migrations/world_db_updates.sql').read_bytes() == b'A\nB\n'
        assert (out / 'migrations/characters_db_updates.sql').read_bytes() == b'C\n'
        assert (out / 'migrations/logs_db_updates.sql').read_bytes() == b''
        assert (out / '.complete').read_text().strip() == f.version(p)
        assert len(calls) == 2 and (Path(d) / 'progress').is_file()
        # Again (a first start that stopped after the downloads): no network needed.
        f.fetch(Path(d) / 'sql2', cache, prog, p, opener_for({}, calls))
        assert len(calls) == 2 and (Path(d) / 'sql2/world.sql').read_bytes() == world


def test_offline_is_a_clear_error():
    with tempfile.TemporaryDirectory() as d:
        p, _ = lock_for(d, b'x')
        try:
            f.fetch(Path(d) / 'sql', Path(d) / 'dl', f.Progress(None), p, opener_for({}, []))
            assert False
        except f.Failed as e:
            assert e.code == 3 and 'internet' in str(e) and 'example.invalid' in str(e), e
        assert not (Path(d) / 'sql/.complete').exists()


def test_a_different_file_never_reaches_the_database():
    with tempfile.TemporaryDirectory() as d:
        p, served = lock_for(d, b'right\n')
        bad = dict(p, SQL_SHA256_world_updates='0' * 64)
        try:
            f.fetch(Path(d) / 'sql', Path(d) / 'dl', f.Progress(None), bad, opener_for(served, []))
            assert False
        except f.Failed as e:
            assert e.code == 4 and 'world_db_updates' in str(e), e
        assert not (Path(d) / 'sql/.complete').exists()
        # The core archive is set aside, so the next start downloads it again.
        assert not list((Path(d) / 'dl').glob('vmangos-core-*.tar.gz'))
        # A world dump that is not the pinned file.
        served[p['WORLD_DB_URL']] = make_7z(b'other\n')
        try:
            f.fetch(Path(d) / 'sql3', Path(d) / 'dl3', f.Progress(None), p, opener_for(served, []))
            assert False
        except f.Failed as e:
            assert e.code == 4 and 'not the pinned file' in str(e), e
        # Another commit's archive.
        p2, served2 = lock_for(d, b'right\n', commit='other')
        served2[p2['CORE_SQL_URL']] = served[p['CORE_SQL_URL']]
        try:
            f.fetch(Path(d) / 'sql4', Path(d) / 'dl4', f.Progress(None), p2, opener_for(served2, []))
            assert False
        except f.Failed as e:
            assert e.code == 4 and 'commit' in str(e), e


def test_the_real_lock():
    p = f.pins()
    for k in ('WORLD_DB_URL', 'WORLD_DB_SHA256', 'CORE_SQL_URL', 'CORE_SQL_COMMIT'):
        assert p.get(k), k
    assert all(len(p['SQL_SHA256_' + k]) == 64 for k in f.OUTPUTS)
    # Pinned to commits, never to a branch.
    assert p['CORE_SQL_COMMIT'] in p['CORE_SQL_URL'] and '/master/' not in p['WORLD_DB_URL']
    env = (ROOT / 'release/build-env.sh').read_text()
    assert 'VMANGOS_COMMIT=%s' % p['CORE_SQL_COMMIT'] in env, 'the SQL is of the core the server is built from'
    assert len(f.version(p)) == 16


def test_feed_reports_progress():
    with tempfile.TemporaryDirectory() as d:
        src = Path(d) / 'a.sql'
        src.write_bytes(b'x' * (3 << 20))
        out = io.BytesIO()
        old = sys.stdout
        sys.stdout = type('S', (), {'buffer': out})()
        try:
            f.feed(src, 'Importing', f.Progress(str(Path(d) / 'p')))
        finally:
            sys.stdout = old
        assert out.getvalue() == src.read_bytes()
        assert (Path(d) / 'p').read_text().startswith('Importing: ')


if __name__ == '__main__':
    for name, fn in list(globals().items()):
        if name.startswith('test_'):
            fn()
            print('ok', name)
