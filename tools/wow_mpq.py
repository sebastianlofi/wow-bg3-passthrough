#!/usr/bin/env python3
"""wow_mpq.py: read files out of the player's WoW 1.12 MPQ archives (inventory).

  wow_mpq.py DATA_DIR list PATTERN        names from the archives' (listfile) matching a glob
  wow_mpq.py DATA_DIR cat NAME > out      one file, the newest archive that has it

DATA_DIR is the client's Data folder (base.MPQ, dbc.MPQ, interface.MPQ, ..., patch.MPQ,
patch-2.MPQ). Patch archives win over base ones, as in the client. Only reads; nothing is written
next to the client. MPQ format 0 (vanilla): hash and block tables, encrypted tables and files,
sectors compressed with zlib, bzip2 or PKWARE implode.
"""
import bz2, fnmatch, os, struct, sys, zlib

_CRYPT = []


def _crypt_table():
    seed = 0x00100001
    table = [0] * 0x500
    for i in range(0x100):
        j = i
        for _ in range(5):
            seed = (seed * 125 + 3) % 0x2AAAAB
            hi = (seed & 0xFFFF) << 16
            seed = (seed * 125 + 3) % 0x2AAAAB
            table[j] = hi | (seed & 0xFFFF)
            j += 0x100
    return table


def hash_string(s, kind):
    global _CRYPT
    if not _CRYPT:
        _CRYPT = _crypt_table()
    seed1, seed2 = 0x7FED7FED, 0xEEEEEEEE
    for ch in s.upper().replace('/', '\\'):
        c = ord(ch)
        seed1 = _CRYPT[(kind << 8) + c] ^ ((seed1 + seed2) & 0xFFFFFFFF)
        seed2 = (c + seed1 + seed2 + (seed2 << 5) + 3) & 0xFFFFFFFF
    return seed1


def decrypt(data, key):
    if not _CRYPT:
        hash_string('', 0)
    n = len(data) // 4
    words = list(struct.unpack('<%dI' % n, data[:n * 4]))
    seed = 0xEEEEEEEE
    for i in range(n):
        seed = (seed + _CRYPT[0x400 + (key & 0xFF)]) & 0xFFFFFFFF
        w = words[i] ^ ((key + seed) & 0xFFFFFFFF)
        key = ((((~key) << 0x15) + 0x11111111) | (key >> 0x0B)) & 0xFFFFFFFF
        seed = (w + seed + (seed << 5) + 3) & 0xFFFFFFFF
        words[i] = w
    return struct.pack('<%dI' % n, *words) + data[n * 4:]


# ---- PKWARE Data Compression Library "explode" (MPQ flag 0x100 and compression mask 0x08) ----
_LEN_BITS = [3, 2, 3, 3, 4, 4, 4, 5, 5, 5, 5, 6, 6, 6, 7, 7]
_LEN_CODE = [0x05, 0x03, 0x01, 0x06, 0x0A, 0x02, 0x0C, 0x14, 0x04, 0x18, 0x08, 0x30, 0x10, 0x20, 0x40, 0x00]
_EX_LEN_BITS = [0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8]
_LEN_BASE = [0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 14, 22, 38, 70, 134, 262]
_DIST_BITS = [2, 4, 4, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
              6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7]
_DIST_CODE = [0x03, 0x0D, 0x05, 0x19, 0x09, 0x11, 0x01, 0x3E, 0x1E, 0x2E, 0x0E, 0x36, 0x16, 0x26, 0x06, 0x3A,
              0x1A, 0x2A, 0x0A, 0x32, 0x12, 0x22, 0x42, 0x02, 0x7C, 0x3C, 0x5C, 0x1C, 0x6C, 0x2C, 0x4C, 0x0C,
              0x74, 0x34, 0x54, 0x14, 0x64, 0x24, 0x44, 0x04, 0x78, 0x38, 0x58, 0x18, 0x68, 0x28, 0x48, 0x08,
              0xF0, 0x70, 0xB0, 0x30, 0xD0, 0x50, 0x90, 0x10, 0xE0, 0x60, 0xA0, 0x20, 0xC0, 0x40, 0x80, 0x00]


class _Bits:
    def __init__(self, data):
        self.data, self.pos, self.buf, self.n = data, 0, 0, 0

    def need(self, k):
        while self.n < k:
            if self.pos >= len(self.data):
                raise EOFError
            self.buf |= self.data[self.pos] << self.n
            self.pos += 1
            self.n += 8

    def peek(self, k):
        self.need(k)
        return self.buf & ((1 << k) - 1)

    def drop(self, k):
        self.buf >>= k
        self.n -= k

    def get(self, k):
        v = self.peek(k)
        self.drop(k)
        return v


def _decode(bits, codes, lens):
    # codes are stored bit-reversed (LSB first), as the stream is read.
    for width in range(1, 9):
        try:
            v = bits.peek(width)
        except EOFError:
            v = bits.buf & ((1 << width) - 1)
        for i, (c, l) in enumerate(zip(codes, lens)):
            if l == width and c == v:
                bits.drop(width)
                return i
    raise ValueError('bad code')


def explode(data):
    if len(data) < 3:
        raise ValueError('implode stream too short')
    lit_mode, dict_bits = data[0], data[1]
    if lit_mode != 0:
        raise ValueError('ASCII-mode implode not supported')
    bits = _Bits(data[2:])
    out = bytearray()
    while True:
        if bits.get(1) == 0:
            out.append(bits.get(8))
            continue
        code = _decode(bits, _LEN_CODE, _LEN_BITS)
        length = _LEN_BASE[code] + (bits.get(_EX_LEN_BITS[code]) if _EX_LEN_BITS[code] else 0)
        if length == 519:
            break
        length += 2
        d = _decode(bits, _DIST_CODE, _DIST_BITS)
        low = bits.get(2 if length == 2 else dict_bits)
        dist = ((d << (2 if length == 2 else dict_bits)) | low) + 1
        for _ in range(length):
            out.append(out[-dist])
    return bytes(out)


def _decompress(data, full):
    if len(data) >= full:
        return data[:full]
    mask, body = data[0], data[1:]
    if mask & 0x10:
        body = bz2.decompress(body)
    if mask & 0x08:
        body = explode(body)
    if mask & 0x02:
        body = zlib.decompress(body)
    if mask & ~0x1A:
        raise ValueError('compression 0x%02x not supported' % mask)
    return body


class Mpq:
    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        off = 0
        while True:
            self.f.seek(off)
            head = self.f.read(32)
            if len(head) < 32:
                raise ValueError('%s: no MPQ header' % path)
            if head[:4] == b'MPQ\x1a':
                break
            off += 512
        self.base = off
        (_, hsize, _, ver, shift, hpos, bpos, hcount, bcount) = struct.unpack('<4sIIHHIIII', head)
        self.sector = 512 << shift
        self.f.seek(off + hpos)
        ht = decrypt(self.f.read(hcount * 16), hash_string('(hash table)', 3))
        self.hashes = [struct.unpack_from('<IIHHI', ht, i * 16) for i in range(hcount)]
        self.f.seek(off + bpos)
        bt = decrypt(self.f.read(bcount * 16), hash_string('(block table)', 3))
        self.blocks = [struct.unpack_from('<IIII', bt, i * 16) for i in range(bcount)]

    def _find(self, name):
        n = len(self.hashes)
        i = hash_string(name, 0) % n
        a, b = hash_string(name, 1), hash_string(name, 2)
        for _ in range(n):
            n1, n2, _loc, _plat, blk = self.hashes[i]
            if blk == 0xFFFFFFFF:
                return None
            if n1 == a and n2 == b and blk != 0xFFFFFFFE:
                return self.blocks[blk]
            i = (i + 1) % n
        return None

    def has(self, name):
        b = self._find(name)
        return b is not None and b[3] & 0x80000000

    def read(self, name):
        blk = self._find(name)
        if blk is None or not blk[3] & 0x80000000:
            return None
        pos, csize, fsize, flags = blk
        self.f.seek(self.base + pos)
        raw = self.f.read(csize)
        key = 0
        if flags & 0x10000:
            key = hash_string(name.replace('/', '\\').split('\\')[-1], 3)
            if flags & 0x20000:
                key = ((key + pos) ^ fsize) & 0xFFFFFFFF
        if flags & 0x01000000:  # single unit
            if flags & 0x10000:
                raw = decrypt(raw, key)
            if flags & 0x100:
                return explode(raw) if csize < fsize else raw
            return _decompress(raw, fsize) if flags & 0x200 else raw
        if not flags & 0x300:
            if flags & 0x10000:
                out = bytearray()
                for k, s in enumerate(range(0, fsize, self.sector)):
                    out += decrypt(raw[s:s + self.sector], key + k)
                return bytes(out)
            return raw
        nsec = (fsize + self.sector - 1) // self.sector
        table = raw[:(nsec + 1) * 4]
        if flags & 0x10000:
            table = decrypt(table, (key - 1) & 0xFFFFFFFF)
        offs = struct.unpack('<%dI' % (nsec + 1), table)
        out = bytearray()
        for k in range(nsec):
            chunk = raw[offs[k]:offs[k + 1]]
            if flags & 0x10000:
                chunk = decrypt(chunk, (key + k) & 0xFFFFFFFF)
            want = min(self.sector, fsize - k * self.sector)
            if flags & 0x100:
                out += explode(chunk) if len(chunk) < want else chunk
            else:
                out += _decompress(chunk, want)
        return bytes(out)

    def listfile(self):
        raw = self.read('(listfile)')
        if not raw:
            return []
        return [l.strip() for l in raw.decode('latin-1').replace(';', '\n').splitlines() if l.strip()]


class WowData:
    """Every MPQ in the Data folder, patches first (the client's own order)."""

    def __init__(self, data_dir):
        names = [n for n in os.listdir(data_dir) if n.lower().endswith('.mpq')]

        def rank(n):
            low = n.lower()
            if low.startswith('patch'):
                return (0, -len(low), [-ord(c) for c in low])
            return (1, low)
        self.archives = []
        for n in sorted(names, key=rank):
            try:
                self.archives.append(Mpq(os.path.join(data_dir, n)))
            except (ValueError, OSError) as e:
                print('skip %s: %s' % (n, e), file=sys.stderr)

    def read(self, name):
        for a in self.archives:
            if a.has(name):
                return a.read(name)
        return None

    def names(self):
        seen = set()
        for a in self.archives:
            for n in a.listfile():
                if n.lower() not in seen:
                    seen.add(n.lower())
                    yield n


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    w = WowData(sys.argv[1])
    if sys.argv[2] == 'list':
        for n in w.names():
            if fnmatch.fnmatch(n.lower(), sys.argv[3].lower()):
                print(n)
    elif sys.argv[2] == 'cat':
        data = w.read(sys.argv[3])
        if data is None:
            sys.exit('not found: %s' % sys.argv[3])
        sys.stdout.buffer.write(data)


if __name__ == '__main__':
    main()
