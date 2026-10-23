#!/usr/bin/env python3
"""LSPK v18 archives (BG3 .pak), written the way bg3le's own pak_write does
(bg3/bg3le/src/pak.cpp), which the native engine is proven to load:

  header (40 bytes): "LSPK", u32 version 18, u64 list offset, u32 list size
  (compressed list + the two counts), u8 flags, u8 priority, 16 bytes md5,
  u16 parts; then the stored (uncompressed) file contents; then the list:
  u32 count, u32 compressed size, LZ4 block of count x 272-byte entries
  (name[256], u32 offset low, u16 offset high, u8 part, u8 flags,
  u32 size on disk, u32 uncompressed size = 0 for a stored entry).

No third-party modules: the list is LZ4-coded as one literal run (a valid LZ4
block any decoder reads), and the reader decodes any LZ4 block.

  lspk.py write OUT.pak DIR        pack DIR's files (paths relative to DIR)
  lspk.py list PAK                 names and sizes
  lspk.py cat PAK NAME             one file to stdout
"""
import os
import struct
import sys

HEADER = 40
ENTRY = struct.Struct("<256sIHBBII")  # 272 bytes
METHOD_NONE, METHOD_ZLIB, METHOD_LZ4 = 0, 1, 2


def lz4_literals(data: bytes) -> bytes:
    """One LZ4 block holding data as a single literal run (no matches)."""
    n = len(data)
    out = bytearray()
    if n < 15:
        out.append(n << 4)
    else:
        out.append(0xF0)
        rest = n - 15
        while rest >= 255:
            out.append(255)
            rest -= 255
        out.append(rest)
    out += data
    return bytes(out)


def lz4_decode(src: bytes, size: int) -> bytes:
    out = bytearray()
    i = 0
    while i < len(src):
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        if off == 0 or start < 0:
            raise ValueError("bad LZ4 match offset")
        for k in range(ml):
            out.append(out[start + k])
    if len(out) != size:
        raise ValueError(f"LZ4 gave {len(out)} bytes, expected {size}")
    return bytes(out)


def write(out_path: str, files, priority: int = 0) -> None:
    """files: list of (archive name with forward slashes, bytes)."""
    entries = []
    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(b"\0" * HEADER)
        at = HEADER
        for name, data in files:
            raw = name.encode("utf-8")
            if len(raw) >= 256 or "\\" in name or name.startswith("/"):
                raise ValueError(f"bad archive name {name!r}")
            f.write(data)
            entries.append(ENTRY.pack(raw, at & 0xFFFFFFFF, at >> 32, 0, METHOD_NONE, len(data), 0))
            at += len(data)
        listing = lz4_literals(b"".join(entries))
        f.write(struct.pack("<II", len(entries), len(listing)))
        f.write(listing)
        header = b"LSPK" + struct.pack("<IQIBB16sH", 18, at, len(listing) + 8, 0, priority, b"\0" * 16, 1)
        assert len(header) == HEADER
        f.seek(0)
        f.write(header)
    os.replace(tmp, out_path)


def read_list(path: str):
    with open(path, "rb") as f:
        head = f.read(HEADER)
        if head[:4] != b"LSPK":
            raise ValueError("not an LSPK archive")
        version, list_at, list_size, flags, priority, _md5, parts = struct.unpack("<IQIBB16sH", head[4:])
        if version != 18:
            raise ValueError(f"LSPK version {version}; only 18 is read here")
        f.seek(list_at)
        count, packed = struct.unpack("<II", f.read(8))
        raw = lz4_decode(f.read(packed), count * ENTRY.size)
    out = []
    for k in range(count):
        name, lo, hi, part, eflags, on_disk, size = ENTRY.unpack_from(raw, k * ENTRY.size)
        out.append({"Name": name.split(b"\0", 1)[0].decode("utf-8"), "Offset": lo | (hi << 32),
                    "Part": part, "Flags": eflags, "SizeOnDisk": on_disk, "Size": size})
    return out, {"Priority": priority, "Parts": parts, "Flags": flags}


def read_file(path: str, entry) -> bytes:
    with open(path, "rb") as f:
        f.seek(entry["Offset"])
        data = f.read(entry["SizeOnDisk"])
    method = entry["Flags"] & 0x0F
    if method == METHOD_NONE:
        return data
    if method == METHOD_LZ4:
        return lz4_decode(data, entry["Size"])
    if method == METHOD_ZLIB:
        import zlib
        return zlib.decompress(data)
    raise ValueError(f"compression {method}")


def collect(root: str):
    files = []
    for base, dirs, names in os.walk(root):
        dirs.sort()
        for n in sorted(names):
            full = os.path.join(base, n)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            with open(full, "rb") as f:
                files.append((rel, f.read()))
    return files


def main(argv):
    if len(argv) == 4 and argv[1] == "write":
        files = collect(argv[3])
        write(argv[2], files)
        print(f"{len(files)} files written to {argv[2]}")
        return 0
    if len(argv) == 3 and argv[1] == "list":
        entries, info = read_list(argv[2])
        for e in entries:
            print(f"{e['SizeOnDisk']:9d}  {e['Name']}")
        print(f"{len(entries)} files, priority {info['Priority']}, parts {info['Parts']}")
        return 0
    if len(argv) == 4 and argv[1] == "cat":
        entries, _ = read_list(argv[2])
        for e in entries:
            if e["Name"] == argv[3]:
                sys.stdout.buffer.write(read_file(argv[2], e))
                return 0
        print(f"{argv[3]} not in {argv[2]}", file=sys.stderr)
        return 1
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
