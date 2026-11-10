#!/usr/bin/env python3
"""lsf_write.py IN.lsx OUT.lsf: BG3's binary resource format (LSF version 7) from its XML form (LSX).

The release build turns the level and lighting files this project writes as .lsx into the .lsf files BG3 loads,
with no other tool. The layout is LSLib's (github.com/Norbyte/lslib, MIT) "v2" one, uncompressed, as BG3's own
files have it:

  "LSOF", u32 version 7, u64 engine version (major << 55 | minor << 47 | revision << 31 | build)
  u32 x 8: sizes of the strings, keys, nodes and attributes sections, each as (uncompressed, on disk; 0 = not packed)
  u32 x 2: the values section the same way; u8 compression 0, u8 0, u16 0, u32 metadata format 2
  strings: u32 bucket count (512), each bucket u16 count then (u16 length, bytes) entries; a name is written
           elsewhere as bucket << 16 | index in the bucket, the bucket chosen by LSLib's hash of the name
  nodes:   12 bytes each: name, index of its first attribute (-1: none), index of its parent (-1: a region's root)
  attrs:   12 bytes each: name, type | length << 6, its node; values follow one another in this order
  values:  the attributes' bytes

A region is a root node; a node's children are written grouped by name, names in the order they first appear
(LSLib keeps them in a dictionary by name), so a file from here is byte for byte the one LSLib's converter writes.
"""
import struct
import sys
import uuid
import xml.etree.ElementTree as ET

TYPES = {'uint8': 1, 'int16': 2, 'uint16': 3, 'int32': 4, 'uint32': 5, 'float': 6, 'double': 7, 'ivec2': 8, 'ivec3': 9,
         'ivec4': 10, 'fvec2': 11, 'fvec3': 12, 'fvec4': 13, 'mat2x2': 14, 'mat3x3': 15, 'mat3x4': 16, 'mat4x3': 17,
         'mat4x4': 18, 'bool': 19, 'string': 20, 'path': 21, 'FixedString': 22, 'LSString': 23, 'uint64': 24,
         'int64': 26, 'int8': 27, 'guid': 31}
NUMBERS = {1: '<B', 2: '<h', 3: '<H', 4: '<i', 5: '<I', 6: '<f', 7: '<d', 24: '<Q', 26: '<q', 27: '<b'}
VECTORS = {8: (2, 'i'), 9: (3, 'i'), 10: (4, 'i'), 11: (2, 'f'), 12: (3, 'f'), 13: (4, 'f'), 14: (4, 'f'), 15: (9, 'f'),
           16: (12, 'f'), 17: (12, 'f'), 18: (16, 'f')}
BUCKETS = 0x200


def name_hash(s):
    h = 0
    for c in s:
        h = ((h << 5) - h + ord(c)) & 0xffffffff
    return h


def bucket_of(s):
    h = name_hash(s)
    return (h & 0x1ff) ^ ((h >> 9) & 0x1ff) ^ ((h >> 18) & 0x1ff) ^ ((h >> 27) & 0x1ff)


def value_bytes(type_id, text, swap_guids):
    if type_id in NUMBERS:
        return struct.pack(NUMBERS[type_id], float(text) if type_id in (6, 7) else int(text))
    if type_id in VECTORS:
        n, kind = VECTORS[type_id]
        parts = text.split()
        assert len(parts) == n, (type_id, text)
        return struct.pack('<%d%s' % (n, kind), *[(float if kind == 'f' else int)(p) for p in parts])
    if type_id == 19:
        return struct.pack('<B', 1 if text.strip().lower() in ('true', '1') else 0)
    if type_id in (20, 21, 22, 23):
        return text.encode('utf-8') + b'\0'
    if type_id == 31:
        b = bytearray(uuid.UUID(text).bytes_le)
        if swap_guids:
            for i in range(8, 16, 2):
                b[i], b[i + 1] = b[i + 1], b[i]
        return bytes(b)
    raise ValueError('attribute type %d is not written here' % type_id)


def lsx_to_lsf(xml):
    root = ET.fromstring(xml)
    ver = root.find('version')
    major, minor, revision, build = (int(ver.get(k)) for k in ('major', 'minor', 'revision', 'build'))
    swap_guids = 'bswap_guids' in (ver.get('lslib_meta') or 'bswap_guids')
    buckets = [[] for _ in range(BUCKETS)]

    def name(s):
        b = buckets[bucket_of(s)]
        if s not in b:
            b.append(s)
        return bucket_of(s) << 16 | b.index(s)

    nodes, attrs, values = bytearray(), bytearray(), bytearray()
    counts = [0, 0]  # nodes, attributes

    def write(node, parent):
        me = counts[0]
        counts[0] += 1
        own = node.findall('attribute')
        nodes.extend(struct.pack('<Iii', name(node.get('id')), counts[1] if own else -1, parent))
        for a in own:
            t = TYPES[a.get('type')] if not a.get('type').isdigit() else int(a.get('type'))
            v = value_bytes(t, a.get('value') or '', swap_guids)
            attrs.extend(struct.pack('<IIi', name(a.get('id')), t | len(v) << 6, me))
            values.extend(v)
            counts[1] += 1
        kids = node.find('children')
        if kids is not None:
            order = []
            for k in kids.findall('node'):
                if k.get('id') not in order:
                    order.append(k.get('id'))
            for kid_name in order:
                for k in kids.findall('node'):
                    if k.get('id') == kid_name:
                        write(k, me)

    for region in root.findall('region'):
        top = region.find('node')
        assert top is not None and top.get('id') == region.get('id'), 'a region whose node has another name: ' + str(region.get('id'))
        write(top, -1)
    strings = bytearray(struct.pack('<I', BUCKETS))
    for b in buckets:
        strings.extend(struct.pack('<H', len(b)))
        for s in b:
            e = s.encode('utf-8')
            strings.extend(struct.pack('<H', len(e)) + e)
    engine = major << 55 | minor << 47 | revision << 31 | build
    head = b'LSOF' + struct.pack('<IQ', 7, engine)
    head += struct.pack('<8I', len(strings), 0, 0, 0, len(nodes), 0, len(attrs), 0)
    head += struct.pack('<2I', len(values), 0) + struct.pack('<BBHI', 0, 0, 0, 2)
    return bytes(head + strings + nodes + attrs + values)


def main(argv):
    if len(argv) != 2:
        print(__doc__)
        return 2
    open(argv[1], 'wb').write(lsx_to_lsf(open(argv[0], 'rb').read()))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
