#!/usr/bin/env python3
"""seenoise.py --bg3 BG3_DATA --out FILE: BG3's see-through noise for benilla (patch 0090), made from the
player's own BG3 data.

BG3's wall materials cut what stands between the camera and the character with two capsules whose edge is
broken up by one texture they all bind, REF_Cloud_B (512 x 512, BC1, in Textures.pak). benilla's port of
that law reads its red channel from the link folder as 512 x 512 bytes, row-major (see-noise.r8); nothing
of BG3's ships with the mod, so the coordinator's datagen makes it once here. Needs bg3pak (lz4, zstandard).
"""
import argparse, struct, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
NAME = 'Generated/Public/Shared/Assets/_Reference/Resources/REF_Cloud_B.DDS'


def bc1_red(dds):
    """The top mip's red channel of a BC1 DDS, row-major bytes."""
    h, w = struct.unpack_from('<II', dds, 12)
    data, out, o = dds[128:], bytearray(w * h), 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            c0, c1, bits = struct.unpack_from('<HHI', data, o)
            o += 8
            r0, r1 = ((c0 >> 11) & 31) * 255 // 31, ((c1 >> 11) & 31) * 255 // 31
            pal = (r0, r1, (2 * r0 + r1) // 3, (r0 + 2 * r1) // 3) if c0 > c1 else (r0, r1, (r0 + r1) // 2, 0)
            for k in range(16):
                out[(by + k // 4) * w + bx + k % 4] = pal[(bits >> (2 * k)) & 3]
    return w, h, bytes(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--bg3', required=True, help="BG3's Data folder")
    ap.add_argument('--out', required=True)
    a = ap.parse_args(argv)
    sys.path.insert(0, str(HERE))
    from bg3pak import find_in_paks
    pak = find_in_paks(a.bg3, NAME)
    if pak is None:
        print('seenoise: %s is in none of the paks under %s' % (NAME, a.bg3), file=sys.stderr)
        return 1
    dds = pak.read(NAME)
    if dds[:4] != b'DDS ' or dds[84:88] != b'DXT1':
        print('seenoise: %s is not a BC1 DDS' % NAME, file=sys.stderr)
        return 1
    w, h, red = bc1_red(dds)
    if (w, h) != (512, 512):
        print('seenoise: %s is %dx%d, benilla reads 512x512' % (NAME, w, h), file=sys.stderr)
        return 1
    out = Path(a.out)
    tmp = out.with_suffix('.tmp')
    tmp.write_bytes(red)
    tmp.replace(out)
    print('seenoise: %s (%d bytes) from %s' % (out, len(red), pak.path))
    return 0


if __name__ == '__main__':
    sys.exit(main())
