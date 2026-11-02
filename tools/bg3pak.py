#!/usr/bin/env python3
"""Read-only access to BG3's game data for the Tav texture copy: LSPK v18 packages and LSF
(binary LSX) resources. Formats as LSLib (Norbyte) reads them; only what the texture work needs.

  bg3pak.py list PAK [SUBSTRING]           file names in a package (all parts)
  bg3pak.py cat PAK NAME OUT               extract one file
  bg3pak.py lsf FILE.lsf                   dump an LSF file as an indented tree

Needs lz4 and zstandard (the release ships them in python/)."""
import os, struct, sys, uuid

import lz4.block
import lz4.frame
import zstandard


def _decompress(data, method, size, chunked=False):
    if method == 0:
        return data
    if method == 1:
        import zlib
        return zlib.decompress(data)
    if method == 2:
        if chunked:
            return lz4.frame.decompress(data)
        return lz4.block.decompress(data, uncompressed_size=size)
    if method == 3:
        return zstandard.ZstdDecompressor().decompress(data, max_output_size=size)
    raise ValueError(f'compression method {method}')


class Pak:
    """An LSPK v18 package (and its _1, _2 ... parts)."""

    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            magic, version = struct.unpack('<4sI', f.read(8))
            if magic != b'LSPK' or version != 18:
                raise ValueError(f'{path}: {magic} v{version}, want LSPK v18')
            list_off, list_size, flags, prio = struct.unpack('<QIBB', f.read(14))
            f.seek(list_off)
            n, csize = struct.unpack('<II', f.read(8))
            raw = _decompress(f.read(csize), 2, n * 272)
        self.files = {}
        for i in range(n):
            e = raw[i * 272:(i + 1) * 272]
            name = e[:256].split(b'\0', 1)[0].decode('utf-8', 'replace')
            off1, off2, part, fl, disk, size = struct.unpack_from('<IHBBII', e, 256)
            self.files[name] = (off1 | (off2 << 32), part, fl, disk, size)

    def part_path(self, part):
        if part == 0:
            return self.path
        base, ext = os.path.splitext(self.path)
        return f'{base}_{part}{ext}'

    def read(self, name):
        off, part, fl, disk, size = self.files[name]
        with open(self.part_path(part), 'rb') as f:
            f.seek(off)
            data = f.read(disk)
        if fl & 0x0f == 0:
            return data
        return _decompress(data, fl & 0x0f, size)


# ---------------------------------------------------------------- LSF
TYPES_FIXED = {1: '<B', 2: '<h', 3: '<H', 4: '<i', 5: '<I', 6: '<f', 7: '<d', 8: '<2i', 9: '<3i', 10: '<4i',
               11: '<2f', 12: '<3f', 13: '<4f', 14: '<4f', 15: '<9f', 16: '<12f', 17: '<12f', 18: '<16f',
               19: '<B', 24: '<Q', 26: '<q', 27: '<b', 32: '<q'}
STRING_TYPES = {20, 21, 22, 23, 29, 30}


def guid_str(b):
    # BG3 writes GUIDs with bytes 8..15 swapped in pairs (LSLib "ByteSwapGuids").
    b = bytearray(b)
    for i in range(8, 16, 2):
        b[i], b[i + 1] = b[i + 1], b[i]
    return str(uuid.UUID(bytes_le=bytes(b)))


class Node:
    __slots__ = ('name', 'attrs', 'children', 'parent')

    def __init__(self, name):
        self.name, self.attrs, self.children, self.parent = name, {}, [], None

    def find(self, name):
        return [c for c in self.children if c.name == name]

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


def read_lsf(data):
    if data[:4] != b'LSOF':
        raise ValueError('not LSF')
    version, engine = struct.unpack_from('<IQ', data, 4)
    o = 16
    if version >= 6:
        fields = struct.unpack_from('<' + 'I' * 8, data, o)
        o += 32
        if version >= 7:
            # Keys section sizes (LSF v7) come after strings.
            sizes = list(fields)
            extra = struct.unpack_from('<II', data, o)
            o += 8
            (su, sd, ku, kd, nu, nd, au, ad), (vu, vd) = sizes, extra
        else:
            su, sd, nu, nd, au, ad, vu, vd = fields
            ku = kd = 0
        cflags, _, _, meta_format = struct.unpack_from('<BBHI', data, o)
        o += 8
    else:
        raise ValueError(f'LSF v{version} not handled')
    method = cflags & 0x0f

    def section(disk, size, chunked):
        nonlocal o
        if disk == 0 and size == 0:
            return b''
        raw = data[o:o + (disk if disk else size)]
        o += disk if disk else size
        if disk == 0 or method == 0:
            return raw
        return _decompress(raw, method, size, chunked)

    strings_raw = section(sd, su, False)
    keys_raw = section(kd, ku, True) if version >= 7 else b''
    nodes_raw = section(nd, nu, True)
    attrs_raw = section(ad, au, True)
    values = section(vd, vu, True)
    # Strings: hash buckets of (u16 length, bytes).
    names = []
    p = 0
    (nb,) = struct.unpack_from('<I', strings_raw, p)
    p += 4
    for _ in range(nb):
        (ns,) = struct.unpack_from('<H', strings_raw, p)
        p += 2
        bucket = []
        for _ in range(ns):
            (ln,) = struct.unpack_from('<H', strings_raw, p)
            p += 2
            bucket.append(strings_raw[p:p + ln].decode('utf-8', 'replace'))
            p += ln
        names.append(bucket)

    def name_of(h):
        return names[h >> 16][h & 0xffff]

    extended = meta_format == 1  # KeysAndAdjacency; 0 and 2 are the v2 entries
    nodes = []
    if extended:
        for i in range(len(nodes_raw) // 16):
            h, parent, nxt, first = struct.unpack_from('<IiiI', nodes_raw, i * 16)
            nodes.append((name_of(h), parent, first if first != 0xffffffff else -1))
    else:
        for i in range(len(nodes_raw) // 12):
            h, first, parent = struct.unpack_from('<Iii', nodes_raw, i * 12)
            nodes.append((name_of(h), parent, first))
    attrs = []
    if extended:
        for i in range(len(attrs_raw) // 16):
            h, tl, nxt, off = struct.unpack_from('<IIiI', attrs_raw, i * 16)
            attrs.append((name_of(h), tl & 0x3f, tl >> 6, nxt, off))
    else:
        off = 0
        for i in range(len(attrs_raw) // 12):
            h, tl, node = struct.unpack_from('<IIi', attrs_raw, i * 12)
            attrs.append((name_of(h), tl & 0x3f, tl >> 6, node, off))
            off += tl >> 6

    def value(t, ln, off):
        b = values[off:off + ln]
        if t in STRING_TYPES:
            return b.rstrip(b'\0').decode('utf-8', 'replace')
        if t == 31:
            return guid_str(b)
        if t in TYPES_FIXED:
            v = struct.unpack(TYPES_FIXED[t], b[:struct.calcsize(TYPES_FIXED[t])])
            if t == 19:
                return bool(v[0])
            return v[0] if len(v) == 1 else list(v)
        if t in (28, 33):
            # TranslatedString: version u16 (BG3), then handle (u32 length + bytes).
            try:
                (hl,) = struct.unpack_from('<I', b, 2)
                return b[6:6 + hl].rstrip(b'\0').decode('utf-8', 'replace')
            except struct.error:
                return None
        return b.hex()

    out = [Node(n[0]) for n in nodes]
    for i, (name, parent, first) in enumerate(nodes):
        if parent >= 0:
            out[i].parent = out[parent]
            out[parent].children.append(out[i])
    if extended:
        for i, (name, parent, first) in enumerate(nodes):
            a = first
            while a >= 0 and a < len(attrs):
                an, t, ln, nxt, off = attrs[a]
                out[i].attrs[an] = value(t, ln, off)
                a = nxt
    else:
        for an, t, ln, node, off in attrs:
            if 0 <= node < len(out):
                out[node].attrs[an] = value(t, ln, off)
    roots = [n for n in out if n.parent is None]
    return roots


def dump(roots, f=sys.stdout):
    def rec(n, d):
        f.write('  ' * d + n.name + ' ' + ' '.join(f'{k}={v!r}' for k, v in n.attrs.items()) + '\n')
        for c in n.children:
            rec(c, d + 1)
    for r in roots:
        rec(r, 0)


def find_in_paks(data_dir, name, paks=None):
    """The newest package holding NAME (patches first)."""
    paks = paks or (sorted((f for f in os.listdir(data_dir) if f.startswith('Patch') and f.endswith('.pak')), reverse=True)
                    + ['GustavX.pak', 'Gustav.pak', 'Shared.pak', 'Textures.pak', 'Gustav_Textures.pak', 'LowTex.pak',
                       'Models.pak', 'Materials.pak', 'Assets.pak', 'Game.pak'])
    for pk in paks:
        path = os.path.join(data_dir, pk)
        if not os.path.exists(path):
            continue
        p = _pak_cache.get(path) or _pak_cache.setdefault(path, Pak(path))
        if name in p.files:
            return p
    return None


_pak_cache = {}


def dds_info(b):
    """(width, height, mips, format name, header size) of a DDS file."""
    h, w, pitch, depth, mips = struct.unpack_from('<IIIII', b, 12)
    fourcc = b[84:88]
    head = 128
    fmt = fourcc.decode('latin1')
    if fourcc == b'DX10':
        (dxgi,) = struct.unpack_from('<I', b, 128)
        head = 148
        fmt = {71: 'BC1', 72: 'BC1s', 74: 'BC2', 75: 'BC2s', 77: 'BC3', 78: 'BC3s', 80: 'BC4', 81: 'BC4s', 83: 'BC5',
               84: 'BC5s', 95: 'BC6H', 96: 'BC6Hs', 98: 'BC7', 99: 'BC7s', 28: 'RGBA8', 29: 'RGBA8s', 87: 'BGRA8'}.get(dxgi, f'dxgi{dxgi}')
    return w, h, max(mips, 1), fmt, head


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'list':
        p = Pak(sys.argv[2])
        sub = sys.argv[3] if len(sys.argv) > 3 else ''
        for n, (off, part, fl, disk, size) in sorted(p.files.items()):
            if sub in n:
                print(f'{size:>12} {part} {fl:02x} {n}')
    elif cmd == 'cat':
        open(sys.argv[4], 'wb').write(Pak(sys.argv[2]).read(sys.argv[3]))
    elif cmd == 'lsf':
        dump(read_lsf(open(sys.argv[2], 'rb').read()))
    elif cmd == 'dds':
        p = find_in_paks(sys.argv[2], sys.argv[3])
        print(dds_info(p.read(sys.argv[3])) if p else 'not found')
