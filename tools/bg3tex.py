#!/usr/bin/env python3
"""BG3 textures for the Tav texture copy: DDS (BC1/BC3/BC4/BC5/BC7) and virtual textures
(Granite GTS/GTP tile sets, as LSLib's VirtualTextures reads them), decoded to RGBA arrays.

  bg3tex.py dds GAME_DATA NAME OUT.png             a DDS from the packages, top mip
  bg3tex.py vt GAME_DATA GTEX LAYER LEVEL OUT.png  one virtual texture's layer (0 albedo, 1 normal,
                                                   2 physical) at a mip level (0 = full size)

Needs numpy, pillow, lz4 (the release ships them in python/) and libfastlz.so next to this file (FastLZ, built from
LSLib's LSLibNative/fastlz.c: cc -O2 -shared -fPIC fastlz.c -o libfastlz.so)."""
import ctypes, os, struct, sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bg3pak  # noqa: E402

_fastlz = None


def fastlz_decompress(data, size):
    global _fastlz
    if _fastlz is None:
        _fastlz = ctypes.CDLL(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'libfastlz.so'))
        _fastlz.fastlz_decompress.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
    out = ctypes.create_string_buffer(size)
    n = _fastlz.fastlz_decompress(data, len(data), out, size)
    if n <= 0:
        raise ValueError('fastlz failed')
    return out.raw[:n]


# ---------------------------------------------------------------- BC decoding (numpy)
def _rgb565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def _bc1_colors(blocks, alpha_mode=True):
    c0 = blocks[:, 0].astype(np.uint32) | (blocks[:, 1].astype(np.uint32) << 8)
    c1 = blocks[:, 2].astype(np.uint32) | (blocks[:, 3].astype(np.uint32) << 8)
    p0, p1 = _rgb565(c0), _rgb565(c1)
    n = len(blocks)
    pal = np.zeros((n, 4, 4), np.int32)
    pal[:, 0, :3], pal[:, 1, :3] = p0, p1
    pal[:, :, 3] = 255
    four = (c0 > c1) | (not alpha_mode)
    pal[:, 2, :3] = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    pal[:, 3, :3] = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    if alpha_mode:
        pal[:, 3, 3] = np.where(four, 255, 0)
    idx = blocks[:, 4:8].astype(np.uint32)
    bits = idx[:, 0] | (idx[:, 1] << 8) | (idx[:, 2] << 16) | (idx[:, 3] << 24)
    sel = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))[None, :]) & 3
    return np.take_along_axis(pal, sel[:, :, None].astype(np.int64).repeat(4, 2), 1)  # n,16,4


def _bc4_values(blocks):
    a0, a1 = blocks[:, 0].astype(np.int32), blocks[:, 1].astype(np.int32)
    pal = np.zeros((len(blocks), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    big = a0 > a1
    for i in range(1, 7):
        pal[:, i + 1] = np.where(big, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, i + 1] = np.where(big, pal[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(big, pal[:, 6], 0)
    pal[:, 7] = np.where(big, pal[:, 7], 255)
    bits = np.zeros(len(blocks), np.uint64)
    for k in range(6):
        bits |= blocks[:, 2 + k].astype(np.uint64) << np.uint64(8 * k)
    sel = (bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))[None, :]) & np.uint64(7)
    return np.take_along_axis(pal, sel.astype(np.int64), 1)  # n,16


def decode_bc(data, w, h, fmt):
    bw, bh = (w + 3) // 4, (h + 3) // 4
    bs = 8 if fmt in ('BC1', 'DXT1', 'BC4') else 16
    blocks = np.frombuffer(data[:bw * bh * bs], np.uint8).reshape(bw * bh, bs)
    if fmt in ('BC1', 'DXT1'):
        px = _bc1_colors(blocks)
    elif fmt in ('BC3', 'DXT5'):
        px = _bc1_colors(blocks[:, 8:], alpha_mode=False)
        px[:, :, 3] = _bc4_values(blocks[:, :8])
    elif fmt == 'BC4':
        v = _bc4_values(blocks)
        px = np.stack([v, v, v, np.full_like(v, 255)], -1)
    elif fmt == 'BC5':
        r, g = _bc4_values(blocks[:, :8]), _bc4_values(blocks[:, 8:])
        px = np.stack([r, g, np.zeros_like(r), np.full_like(r, 255)], -1)
    elif fmt == 'BC7':
        # Pillow's BCn decoder (BG3's skin masks, e.g. SKIN_Shared_Shading_MSKA, are BC7).
        from PIL import Image
        return np.array(Image.frombytes('RGBA', (w, h), bytes(data[:bw * bh * bs]), 'bcn', 7))
    else:
        raise ValueError(f'format {fmt}')
    img = px.reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return img[:h, :w].astype(np.uint8)


DXGI_FMT = {71: 'BC1', 72: 'BC1', 77: 'BC3', 78: 'BC3', 80: 'BC4', 83: 'BC5'}


def dds_rgba(b, mip=0):
    w, h, mips, fmt, head = bg3pak.dds_info(b)
    fmt = {'BC1s': 'BC1', 'BC3s': 'BC3', 'DXT1': 'BC1', 'DXT5': 'BC3', 'ATI2': 'BC5', 'ATI1': 'BC4', 'BC4U': 'BC4',
           'BC5U': 'BC5', 'BC7s': 'BC7'}.get(fmt, fmt)
    bs = 8 if fmt in ('BC1', 'BC4') else 16
    off = head
    for _ in range(min(mip, mips - 1)):
        off += max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * bs
        w, h = max(1, w // 2), max(1, h // 2)
    return decode_bc(b[off:], w, h, fmt)


# ---------------------------------------------------------------- virtual textures
GTS_HEADER = struct.Struct('<III16sIQIQiiiIIQIIIQ7IIIQIQIQQ4I')


class TileSet:
    def __init__(self, data_dir, gts_name):
        self.data_dir = data_dir
        p = bg3pak.find_in_paks(data_dir, gts_name, ['VirtualTextures.pak'])
        if not p:
            raise FileNotFoundError(gts_name)
        self.pak = p
        b = self.gts = p.read(gts_name)
        f = GTS_HEADER.unpack_from(b, 0)
        (magic, ver, _, guid, self.num_layers, layers_off, num_levels, levels_off, self.tw, self.th, self.border,
         _i2, num_flat, flat_off, _i6, _i7, num_packed, packed_off) = f[:18]
        (self.page_size, num_pf, pf_off, cc_size, cc_off, npb, pb_off, thumbs) = f[25:33]
        assert magic == 0x47505247, hex(magic)
        self.levels = [struct.unpack_from('<IIQ', b, levels_off + 16 * i) for i in range(num_levels)]
        self.flat_idx = [np.frombuffer(b, '<u4', w * h * self.num_layers, off) for (w, h, off) in self.levels]
        self.flat = np.frombuffer(b, np.dtype([('pf', '<u2'), ('page', '<u2'), ('chunk', '<u2'), ('d', '<u2'),
                                                ('packed', '<u4')]), num_flat, flat_off)
        self.blocks = {}
        for i in range(npb):
            bid, codec, size, off = struct.unpack_from('<IIIQ', b, pb_off + 20 * i)
            if codec == 9:
                c1 = b[off + 2:off + 18].split(b'\0')[0].decode()
                c2 = b[off + 18:off + 34].split(b'\0')[0].decode()
                self.blocks[bid] = ('bc', c1, c2)
            else:
                self.blocks[bid] = ('uniform',)
        self.page_files = []
        for i in range(num_pf):
            o = pf_off + i * (512 + 4 + 16 + 4)
            name = b[o:o + 512].decode('utf-16-le').split('\0')[0]
            self.page_files.append(name)
        self.textures = self._fourcc(b, cc_off, cc_size)
        self._gtp = {}

    def _fourcc(self, b, off, size):
        def read(o, end):
            out = []
            while o < end:
                cc, fmt, ext, ln = struct.unpack_from('<4sBBH', b, o)
                o += 8
                if ext == 1:
                    (hi,) = struct.unpack_from('<I', b, o)
                    o += 4
                    ln |= hi << 16
                if fmt == 1:
                    val = read(o, o + ln)
                elif fmt == 2:
                    val = b[o:o + ln - 2].decode('utf-16-le')
                elif fmt == 3:
                    (val,) = struct.unpack_from('<I', b, o)
                else:
                    val = b[o:o + ln]
                o += ln
                o += (-o) % 4
                out.append((cc.decode('latin1'), val))
            return out
        root = read(off, off + size)
        tex = {}

        def child(node, name):
            for k, v in node:
                if k == name:
                    return v
            return None
        atls = child(root[0][1], 'ATLS') if root else None
        for k, t in (child(atls, 'TXTS') or []):
            tex[child(t, 'NAME')] = (child(t, 'XXXX'), child(t, 'YYYY'), child(t, 'WDTH'), child(t, 'HGHT'))
        return tex

    def gtp(self, index):
        # The value itself is returned, not looked up again: a caller sharing the tile set may clear
        # _gtp in between (tavtex drops the page files after each texture).
        got = self._gtp.get(index)
        if got is None:
            name = 'Generated/Public/VirtualTextures/' + self.page_files[index]
            p = bg3pak.find_in_paks(self.data_dir, name, ['VirtualTextures.pak'])
            if not p:
                raise FileNotFoundError(name)
            data = p.read(name)
            pages = []
            for page in range(len(data) // self.page_size):
                o = page * self.page_size + (24 if page == 0 else 0)
                (n,) = struct.unpack_from('<I', data, o)
                pages.append(struct.unpack_from(f'<{n}I', data, o + 4))
            got = (data, pages)
            self._gtp[index] = got
        return got

    def tile_bc3(self, level, layer, x, y):
        w, h, _ = self.levels[level]
        ti = int(self.flat_idx[level][layer + self.num_layers * (x + y * w)])
        if ti & 0x80000000:
            return None
        info = self.flat[ti]
        data, pages = self.gtp(int(info['pf']))
        o = int(info['page']) * self.page_size + pages[int(info['page'])][int(info['chunk'])]
        codec, block, size = struct.unpack_from('<III', data, o)
        payload = data[o + 12:o + 12 + size]
        tsize = 16 * ((self.tw + 3) // 4) * ((self.th + 3) // 4) + 16 * ((self.tw // 2 + 3) // 4) * ((self.th // 2 + 3) // 4)
        if codec == 0:
            r, g, bb, a = payload[:4]
            img = np.zeros((self.th, self.tw, 4), np.uint8)
            img[:] = (r, g, bb, a)
            return img
        kind = self.blocks[block]
        if kind[1] == 'lz4':
            import lz4.block
            raw = lz4.block.decompress(payload, uncompressed_size=tsize)
        elif kind[1] == 'lz77':
            raw = fastlz_decompress(payload, tsize)
        else:
            raw = payload
        return decode_bc(raw, self.tw, self.th, 'BC3')

    def _tile_range(self, gtex, level):
        """(min_x, min_y, max_x, max_y, cw, ch): the texture's tiles at a level, and a tile's content size."""
        x0, y0, tw, th = self.textures[gtex]
        cw, ch = self.tw - 2 * self.border, self.th - 2 * self.border
        tX, tY, tW, tH = x0 // cw, y0 // ch, tw // cw, th // ch
        lv = 1 << level
        min_x = tX // lv + (1 if tX % lv else 0)
        min_y = tY // lv + (1 if tY % lv else 0)
        max_x = (tX + tW) // lv + (1 if (tX + tW) % lv else 0) - 1
        max_y = (tY + tH) // lv + (1 if (tY + tH) % lv else 0) - 1
        return min_x, min_y, max_x, max_y, cw, ch

    def missing(self, gtex, layer, level):
        """How many of the texture's tiles of a layer the tile set leaves out at a level (flat index flag
        0x80000000). Granite stores a layer only down to the level its source reached (
        Gale's robe bracers' albedo and physical layers stop at 512 px of 1024, Astarion's pants'
        physical layer and his shirt lace's normal and physical too); BG3 samples the next coarser
        level there, and texture() returns those tiles as zeros."""
        min_x, min_y, max_x, max_y, _, _ = self._tile_range(gtex, level)
        w = self.levels[level][0]
        idx = self.flat_idx[level]
        return sum(1 for y in range(min_y, max_y + 1) for x in range(min_x, max_x + 1)
                   if int(idx[layer + self.num_layers * (x + y * w)]) & 0x80000000)

    def texture(self, gtex, layer, level):
        min_x, min_y, max_x, max_y, cw, ch = self._tile_range(gtex, level)
        out = np.zeros(((max_y - min_y + 1) * ch, (max_x - min_x + 1) * cw, 4), np.uint8)
        for y in range(min_y, max_y + 1):
            for x in range(min_x, max_x + 1):
                t = self.tile_bc3(level, layer, x, y)
                if t is not None:
                    b = self.border
                    out[(y - min_y) * ch:(y - min_y + 1) * ch, (x - min_x) * cw:(x - min_x + 1) * cw] = t[b:b + ch, b:b + cw]
        return out


def find_tileset(data_dir, gtex):
    """The GTS whose page files include GTEX (the page file names carry the hash)."""
    p = bg3pak.find_in_paks(data_dir, 'Generated/Public/VirtualTextures/Albedo_Normal_Physical_0.gts', ['VirtualTextures.pak'])
    for name in p.files:
        if name.endswith(f'_{gtex}.gtp'):
            base = name.rsplit('_', 1)[0]
            return base + '.gts'
    return None


def save_png(img, path):
    from PIL import Image
    Image.fromarray(img, 'RGBA').save(path)


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'dds':
        p = bg3pak.find_in_paks(sys.argv[2], sys.argv[3])
        save_png(dds_rgba(p.read(sys.argv[3])), sys.argv[4])
    elif cmd == 'vt':
        data, gtex, layer, level, out = sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), sys.argv[6]
        gts = find_tileset(data, gtex)
        ts = TileSet(data, gts)
        print(gts, 'tile', ts.tw, ts.th, ts.border, 'layers', ts.num_layers, 'levels', len(ts.levels), 'tex', ts.textures.get(gtex))
        save_png(ts.texture(gtex, layer, level), out)
