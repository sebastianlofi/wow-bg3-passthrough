#!/usr/bin/env python3
"""fetch-sql.py: the server's database SQL, made on the player's PC at the first start (wowbg3-server.sh init).

The release ships no game data: vmangos' world database (WoW's creatures, items, quests and their texts) and the
rest of vmangos' SQL are downloaded here from their public sources, pinned in sql.lock next to this file, and every
file made is checked against its sha256 there.

  fetch-sql.py fetch OUT_DIR --cache DIR [--progress FILE] [--error FILE]
        OUT_DIR/world.sql, characters.sql, logon.sql, logs.sql and migrations/*_db_updates.sql (OUT_DIR/.complete
        when all are right). The downloads stay in DIR (checked again on every use), so a first start that stopped
        after them needs no second download.
  fetch-sql.py feed FILE [--label TEXT] [--progress FILE]
        FILE to stdout, how far it is in the progress file (the database import: `feed world.sql | mariadb`).
  fetch-sql.py version
        the database's version: a short hash of the SQL's pins (wowbg3-server.sh keeps it in var/sql.version).

Progress: one line in --progress, replaced as it goes (wowbg3-coord.py puts it in run/coord.json Detail).
Errors: one line in --error and on stderr, exit 3 when a download failed (no internet), 4 when a file is not the
pinned one. Standard library only, Python 3.9 or later (Steam's runtime). The world dump is a 7z of one LZMA stream:
read here with the lzma module, no 7z program needed.
"""
import argparse, fnmatch, hashlib, io, lzma, os, re, ssl, struct, sys, tarfile, time, urllib.error, urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
LOCK = HERE / 'sql.lock'
CHUNK = 1 << 20
# Where else a container may keep the system's CA certificates (Steam's runtime has the host's under /run/host).
CA_FILES = ('/etc/ssl/certs/ca-certificates.crt', '/etc/pki/tls/certs/ca-bundle.crt', '/etc/ssl/cert.pem',
            '/run/host/etc/ssl/certs/ca-certificates.crt', '/run/host/etc/pki/tls/certs/ca-bundle.crt')
OFFLINE = ("WOW-BG3's first start downloads the WoW server's world database once (about 48 MB) and could not: %s. "
           "Connect to the internet and start BG3 again.")
CORE_FILES = ('characters', 'logon', 'logs')
UPDATES = ('characters', 'logon', 'logs', 'world')


class Failed(Exception):
    def __init__(self, code, text):
        super().__init__(text)
        self.code = code


def pins(path=LOCK):
    out = {}
    for line in Path(path).read_text().splitlines():
        m = re.match(r'([A-Za-z_][A-Za-z0-9_]*)=(\S+)$', line.strip())
        if m:
            out[m.group(1)] = m.group(2)
    return out


def version(p):
    """The database's identity: what the SQL holds, not where it came from."""
    keys = sorted(k for k in p if k.startswith('SQL_SHA256_'))
    return hashlib.sha256(''.join('%s=%s\n' % (k, p[k]) for k in keys).encode()).hexdigest()[:16]


class Progress:
    def __init__(self, path):
        self.path, self.last, self.text = path, 0.0, None

    def say(self, text, force=False):
        now = time.time()
        if not self.path or (not force and now - self.last < 0.5) or text == self.text:
            return
        self.last, self.text = now, text
        tmp = '%s.tmp' % self.path
        with open(tmp, 'w') as f:
            f.write(text + '\n')
        os.replace(tmp, self.path)


def mb(n):
    return '%.1f MB' % (n / 1e6)


# ---- downloads ----
def _contexts():
    yield ssl.create_default_context()
    for ca in CA_FILES:
        if os.path.isfile(ca):
            yield ssl.create_default_context(cafile=ca)


def download(url, dest, size, label, prog, opener=None):
    """url to dest (via dest.part), how far in the progress line. A failed connection is Failed(3)."""
    host = re.sub(r'^\w+://([^/]+).*', r'\1', url)
    last = None
    for ctx in ([None] if opener else _contexts()):
        try:
            req = urllib.request.Request(url, headers={'User-Agent': 'wow-bg3-server'})
            resp = opener(req) if opener else urllib.request.urlopen(req, timeout=60, context=ctx)
            total = int(resp.headers.get('Content-Length') or 0) or size
            got = 0
            part = Path(str(dest) + '.part')
            with resp, open(part, 'wb') as f:
                while True:
                    b = resp.read(CHUNK)
                    if not b:
                        break
                    f.write(b)
                    got += len(b)
                    prog.say('%s: %s of %s' % (label, mb(got), mb(total)) if total else '%s: %s' % (label, mb(got)))
            os.replace(part, dest)
            return
        except ssl.SSLCertVerificationError as e:   # this container's certificates: try the next file
            last = e
        except urllib.error.URLError as e:
            if isinstance(getattr(e, 'reason', None), ssl.SSLCertVerificationError):
                last = e
                continue
            raise Failed(3, OFFLINE % ('%s: %s' % (host, getattr(e, 'reason', e))))
        except OSError as e:
            raise Failed(3, OFFLINE % ('%s: %s' % (host, e)))
    raise Failed(3, OFFLINE % ('%s: no certificate here could check its TLS certificate (%s)' % (host, last)))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(CHUNK), b''):
            h.update(b)
    return h.hexdigest()


# ---- 7z: one folder, one LZMA or LZMA2 coder (what the world dump is) ----
class _R:
    def __init__(self, b):
        self.b, self.i = b, 0

    def byte(self):
        v = self.b[self.i]
        self.i += 1
        return v

    def take(self, n):
        v = self.b[self.i:self.i + n]
        self.i += n
        return v

    def num(self):
        first, mask, v = self.byte(), 0x80, 0
        for i in range(8):
            if not first & mask:
                return v | ((first & (mask - 1)) << (8 * i))
            v |= self.byte() << (8 * i)
            mask >>= 1
        return v


def _digests(r, n):
    if r.byte():
        defined = [True] * n
    else:
        defined, b, mask = [], 0, 0
        for _ in range(n):
            if not mask:
                b, mask = r.byte(), 0x80
            defined.append(bool(b & mask))
            mask >>= 1
    return [struct.unpack('<I', r.take(4))[0] if d else None for d in defined]


def _streams(r):
    info = {'pack_pos': 0, 'pack_sizes': [], 'folders': []}
    t = r.byte()
    if t == 0x06:                                   # PackInfo
        info['pack_pos'], n = r.num(), r.num()
        t = r.byte()
        while t:
            if t == 0x09:
                info['pack_sizes'] = [r.num() for _ in range(n)]
            elif t == 0x0A:
                _digests(r, n)
            else:
                raise ValueError('7z: pack info %#x' % t)
            t = r.byte()
        t = r.byte()
    if t == 0x07:                                   # UnPackInfo
        if r.byte() != 0x0B:
            raise ValueError('7z: no folders')
        nf = r.num()
        if r.byte():
            raise ValueError('7z: external folders')
        for _ in range(nf):
            coders, tin, tout = [], 0, 0
            for _ in range(r.num()):
                flag = r.byte()
                cid = bytes(r.take(flag & 0x0F))
                nin, nout = (r.num(), r.num()) if flag & 0x10 else (1, 1)
                props = bytes(r.take(r.num())) if flag & 0x20 else b''
                if flag & 0x80:
                    raise ValueError('7z: alternative coders')
                coders.append((cid, props))
                tin, tout = tin + nin, tout + nout
            for _ in range(tout - 1):
                r.num(), r.num()
            if tin - (tout - 1) > 1:
                for _ in range(tin - (tout - 1)):
                    r.num()
            info['folders'].append({'coders': coders, 'nout': tout})
        if r.byte() != 0x0C:
            raise ValueError('7z: no unpack sizes')
        for f in info['folders']:
            f['sizes'] = [r.num() for _ in range(f['nout'])]
    return info


def _decoder(cid, props, size):
    if cid == b'\x03\x01\x01':                      # LZMA: 7z's has no end mark, its size is the folder's
        d, = struct.unpack('<I', props[1:5])
        lc, lp, pb = props[0] % 9, props[0] // 9 % 5, props[0] // 45
        return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'dict_size': d, 'lc': lc, 'lp': lp, 'pb': pb}]), b''
    if cid == b'\x21':                              # LZMA2
        bits = props[0]
        dict_size = 0xFFFFFFFF if bits == 40 else (2 | (bits & 1)) << (bits // 2 + 11)
        return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA2, 'dict_size': dict_size}]), b''
    raise ValueError('7z: coder %s is not LZMA' % cid.hex())


def _unpack(f, info, out, prog=None, label=''):
    """The archive's one stream to out.write; returns its size."""
    if len(info['folders']) != 1 or len(info['folders'][0]['coders']) != 1:
        raise ValueError('7z: more than one stream or coder')
    folder = info['folders'][0]
    size = folder['sizes'][-1]
    dec, head = _decoder(*folder['coders'][0], size)
    f.seek(32 + info['pack_pos'])
    left, done = info['pack_sizes'][0], 0
    if head:
        piece = dec.decompress(head)
        out(piece)
        done += len(piece)
    while left > 0 and done < size:
        b = f.read(min(CHUNK, left))
        if not b:
            raise ValueError('7z: archive cut short')
        left -= len(b)
        piece = dec.decompress(b, size - done)
        out(piece)
        done += len(piece)
    while done < size and not dec.eof and not dec.needs_input:   # what the last block left in the decoder
        piece = dec.decompress(b'', size - done)
        if not piece:
            break
        out(piece)
        done += len(piece)
        if prog:
            prog.say('%s: %d%%' % (label, 100 * done // size))
    if done != size:
        raise ValueError('7z: %d of %d bytes' % (done, size))
    return size


def unpack_7z(path, out, prog=None, label=''):
    """The one file of a 7z archive (plain or encoded header) to out.write."""
    with open(path, 'rb') as f:
        h = f.read(32)
        if h[:6] != b"7z\xbc\xaf\x27\x1c":
            raise ValueError('not a 7z archive')
        off, size = struct.unpack('<QQ', h[12:28])
        f.seek(32 + off)
        r = _R(f.read(size))
        t = r.byte()
        if t == 0x17:                               # the header is itself packed
            buf = io.BytesIO()
            _unpack(f, _streams(r), buf.write)
            r = _R(buf.getvalue())
            t = r.byte()
        if t != 0x01 or r.byte() != 0x04:
            raise ValueError('7z: header without main streams')
        return _unpack(f, _streams(r), out, prog, label)


# ---- vmangos core's sql/ ----
def core_sql(tar_path, out_dir, commit, prog):
    """characters, logon, logs.sql and the merged migrations (sql/migrations/merge.sh: every *_NAME.sql, in
    `sort` order of their paths, one after another) from GitHub's archive of the core."""
    found, migrations = {}, {n: {} for n in UPDATES}
    with tarfile.open(tar_path, 'r:gz') as t:
        if commit and t.pax_headers.get('comment') not in (None, commit):
            raise Failed(4, 'the vmangos core archive is of commit %s, not %s' % (t.pax_headers.get('comment'), commit))
        for m in t:
            if not m.isfile():
                continue
            rel = m.name.split('/', 1)[1] if '/' in m.name else m.name
            if rel in ['sql/%s.sql' % n for n in CORE_FILES]:
                found[rel[4:-4]] = t.extractfile(m).read()
            elif rel.startswith('sql/migrations/'):
                base = rel.rsplit('/', 1)[1]
                for n in UPDATES:
                    if fnmatch.fnmatch(base, '*_%s.sql' % n):
                        migrations[n]['./' + rel[len('sql/migrations/'):]] = t.extractfile(m).read()
            prog.say('Reading vmangos SQL: %s' % rel[:60])
    missing = [n for n in CORE_FILES if n not in found]
    if missing:
        raise Failed(4, 'the vmangos core archive has no sql/%s.sql' % ', '.join(missing))
    for n, body in found.items():
        (out_dir / ('%s.sql' % n)).write_bytes(body)
    (out_dir / 'migrations').mkdir(exist_ok=True)
    for n, files in migrations.items():
        with open(out_dir / 'migrations' / ('%s_db_updates.sql' % n), 'wb') as f:
            for k in sorted(files, key=lambda s: s.encode()):
                f.write(files[k])


OUTPUTS = {'world': 'world.sql', 'characters': 'characters.sql', 'logon': 'logon.sql', 'logs': 'logs.sql',
           'world_updates': 'migrations/world_db_updates.sql', 'characters_updates': 'migrations/characters_db_updates.sql',
           'logon_updates': 'migrations/logon_db_updates.sql', 'logs_updates': 'migrations/logs_db_updates.sql'}


def fetch(out_dir, cache, prog, p, opener=None):
    out_dir, cache = Path(out_dir), Path(cache)
    out_dir.mkdir(parents=True, exist_ok=True)
    cache.mkdir(parents=True, exist_ok=True)
    if (out_dir / '.complete').exists():
        (out_dir / '.complete').unlink()
    t0 = time.time()
    # 1. The world dump, kept as downloaded (its bytes are pinned).
    world7z = cache / Path(p['WORLD_DB_URL']).name
    if not world7z.is_file() or sha256_file(world7z) != p['WORLD_DB_SHA256']:
        download(p['WORLD_DB_URL'], world7z, int(p.get('WORLD_DB_SIZE', 0)), 'Downloading the world database', prog, opener)
        got = sha256_file(world7z)
        if got != p['WORLD_DB_SHA256']:
            os.replace(world7z, str(world7z) + '.bad')
            raise Failed(4, 'the world database download is not the pinned file (sha256 %s, want %s)' % (got, p['WORLD_DB_SHA256']))
    # 2. vmangos core's archive: GitHub makes it on request, so its files are checked, not its bytes.
    core = cache / ('vmangos-core-%s.tar.gz' % p['CORE_SQL_COMMIT'][:12])
    if not core.is_file():
        download(p['CORE_SQL_URL'], core, int(p.get('CORE_SQL_SIZE', 0)), 'Downloading vmangos SQL', prog, opener)
    with open(out_dir / 'world.sql.tmp', 'wb') as f:
        unpack_7z(world7z, f.write, prog, 'Unpacking the world database')
    os.replace(out_dir / 'world.sql.tmp', out_dir / 'world.sql')
    try:
        core_sql(core, out_dir, p.get('CORE_SQL_COMMIT'), prog)
    except (tarfile.TarError, EOFError, OSError) as e:
        os.replace(core, str(core) + '.bad')        # next start downloads it again
        raise Failed(4, 'the vmangos core archive could not be read (%s); it is downloaded again next start' % e)
    prog.say('Checking the SQL', force=True)
    bad = []
    for key, rel in OUTPUTS.items():
        got = sha256_file(out_dir / rel)
        if got != p['SQL_SHA256_' + key]:
            bad.append('%s (sha256 %s)' % (rel, got))
    if bad:
        if any(not b.startswith('world.sql ') for b in bad):   # from the core's archive: fetched again next start
            os.replace(core, str(core) + '.bad')
        raise Failed(4, 'not the pinned SQL: %s' % '; '.join(bad))
    (out_dir / '.complete').write_text(version(p) + '\n')
    print('SQL ready in %s (%.0f s): version %s' % (out_dir, time.time() - t0, version(p)))


def feed(path, label, prog):
    size, done = os.path.getsize(path), 0
    out = sys.stdout.buffer
    with open(path, 'rb') as f:
        for b in iter(lambda: f.read(CHUNK), b''):
            out.write(b)
            done += len(b)
            prog.say('%s: %d%%' % (label, 100 * done // max(size, 1)))
    out.flush()


def main(argv=None):
    a = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    a.add_argument('cmd', choices=('fetch', 'feed', 'version'))
    a.add_argument('path', nargs='?')
    a.add_argument('--cache')
    a.add_argument('--progress')
    a.add_argument('--error')
    a.add_argument('--label', default='Importing')
    a.add_argument('--lock', default=str(LOCK))
    args = a.parse_args(argv)
    p = pins(args.lock)
    prog = Progress(args.progress)
    if args.cmd == 'version':
        print(version(p))
        return 0
    if args.cmd == 'feed':
        feed(args.path, args.label, prog)
        return 0
    if not (args.path and args.cache):
        a.error('fetch needs OUT_DIR and --cache')
    try:
        fetch(args.path, args.cache, prog, p)
        return 0
    except (Failed, ValueError, lzma.LZMAError) as e:
        code = getattr(e, 'code', 4)
        text = str(e) if isinstance(e, Failed) else 'the world database could not be unpacked: %s' % e
        if args.error:
            Path(args.error).write_text(text + '\n')
        print('fetch-sql: %s' % text, file=sys.stderr)
        return code


if __name__ == '__main__':
    sys.exit(main())
