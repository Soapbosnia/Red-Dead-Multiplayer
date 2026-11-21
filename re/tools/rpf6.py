#!/usr/bin/env python3
"""Minimal RDR1 RPF6 reader (PC port, encrypted TOC).

Layout (from MagicRDR's RPF6.cs):
  header  : "RPF6" | entryCount u32 | debugOffset u32 | encFlag u32   (big endian)
  tocSize : ((entryCount*5) << 2 + 15) & ~15
  toc     : at 0x10, AES-256-ECB, no padding, key below
  entry   : 20 bytes, big endian
    dir   : nameHash u32 | flags u32 | contentIndex u32(&0x7fffffff) | contentCount u32(&0x0fffffff) | unk u32
    file  : nameHash u32 | size u32(&0x0fffffff) | offset u32 | flag1 u32 | flag2 u32
Names are hashes, not strings, so entries are addressed by index/hash.

Usage:
  python rpf6.py list <archive.rpf>
  python rpf6.py extract <archive.rpf> <outdir> [max_size]
"""

from __future__ import annotations

import os
import struct
import sys

from Crypto.Cipher import AES

KEY = bytes([
    0xB7, 0x62, 0xDF, 0xB6, 0xE2, 0xB2, 0xC6, 0xDE, 0xAF, 0x72, 0x2A, 0x32,
    0xD2, 0xFB, 0x6F, 0x0C, 0x98, 0xA3, 0x21, 0x74, 0x62, 0xC9, 0xC4, 0xED,
    0xAD, 0xAA, 0x2E, 0xD0, 0xDD, 0xF9, 0x2F, 0x10,
])


class Entry:
    __slots__ = ("index", "name_hash", "is_dir", "size", "size_raw", "offset", "flag1", "flag2",
                 "content_index", "content_count", "children", "parent", "flags", "unk")

    def __init__(self):
        self.children = []
        self.parent = None

    def path(self) -> str:
        parts = []
        e = self
        while e is not None:
            parts.append("root" if e.index == 0 else f"0x{e.name_hash:08X}")
            e = e.parent
        return "/".join(reversed(parts))


class RPF6:
    def __init__(self, path: str):
        self.path = path
        self.f = open(path, "rb")
        head = self.f.read(16)
        ident, self.entry_count, self.debug_offset, self.enc_flag = struct.unpack(">IIII", head)
        if ident != 0x52504636:
            raise ValueError(f"not RPF6: 0x{ident:08X}")
        self.encrypted = self.enc_flag != 0
        toc_size = ((self.entry_count * 5) << 2) + 15 & ~15
        self.f.seek(0x10)
        toc = self.f.read(toc_size)
        if self.encrypted:
            # PC port applies the AES-256 block cipher 16 times per block
            # (the game's TOC routine loops the block function 16x). Plain
            # AES-256-ECB once does not produce a valid table.
            if len(toc) % 16:
                toc += b"\0" * (16 - len(toc) % 16)
            cipher = AES.new(KEY, AES.MODE_ECB)
            for _ in range(16):
                toc = cipher.decrypt(toc)
        self.entries: list[Entry] = []
        for i in range(self.entry_count):
            e = Entry()
            e.index = i
            e.name_hash, = struct.unpack_from(">I", toc, i * 20)
            if toc[i * 20 + 8] == 0x80:
                e.is_dir = True
                _, e.flags, e.content_index, e.content_count, e.unk = struct.unpack_from(">IIIII", toc, i * 20)
                e.size = 0
                e.offset = 0
                e.flag1 = e.flag2 = 0
            else:
                e.is_dir = False
                _, e.size_raw, e.offset, e.flag1, e.flag2 = struct.unpack_from(">IIIII", toc, i * 20)
                e.size = e.size_raw & 0x0FFFFFFF
                e.content_index = e.content_count = 0
            self.entries.append(e)
        self._link()

    def _link(self):
        root = self.entries[0]
        stack = [root]
        seen = {0}
        while stack:
            d = stack.pop()
            for j in range(d.content_index, d.content_index + d.content_count):
                if j >= len(self.entries) or j in seen:
                    continue
                seen.add(j)
                e = self.entries[j]
                e.parent = d
                d.children.append(e)
                if e.is_dir:
                    stack.append(e)

    def data_offset(self, e: Entry) -> int:
        # Resource entries carry the resource type in the low byte and mask the
        # offset with 0x7FFFFF00; regular files use the full 31 bits.
        if e.flag1 & 0x80000000:
            return (e.offset & 0x7FFFFF00) * 8
        return (e.offset & 0x7FFFFFFF) * 8

    def read(self, e: Entry) -> bytes:
        if e.is_dir or e.size == 0:
            return b""
        self.f.seek(self.data_offset(e))
        return self.f.read(e.size)

    def payload(self, e: Entry) -> bytes:
        """Return the usable bytes of an entry (decompressed when possible)."""
        raw = self.read(e)
        if not raw:
            return raw
        import zstandard as zstd

        # RSC5 resource: 'RSC\\x05' + 8 flag bytes, then a zstd frame.
        if raw[:4] == b"RSC\x05" and raw[12:16] == b"\x28\xb5\x2f\xfd":
            return zstd.ZstdDecompressor().decompress(raw[12:], max_output_size=256 * 1024 * 1024)
        if raw[:4] == b"\x28\xb5\x2f\xfd":
            return zstd.ZstdDecompressor().decompress(raw, max_output_size=256 * 1024 * 1024)
        return raw

    def decompress(self, data: bytes) -> bytes:
        if not data:
            return data
        try:
            import zlib
            return zlib.decompress(data, -15)
        except Exception:
            return data


def cmd_list(path: str) -> None:
    r = RPF6(path)
    print(f"{path}: entries={r.entry_count} encrypted={r.encrypted}")
    for e in r.entries[:40]:
        kind = "D" if e.is_dir else "F"
        extra = f"n={e.content_count}" if e.is_dir else f"size={e.size} off=0x{e.offset*8:X}"
        print(f"  [{e.index:4}] {kind} hash=0x{e.name_hash:08X} {extra}")


def cmd_scan(path: str, outdir: str, max_size: int) -> None:
    r = RPF6(path)
    os.makedirs(outdir, exist_ok=True)
    magics = {b"GFX": "GFX", b"FWS": "FWS", b"CWS": "CWS", b"RSC7": "RSC7", b"\x89PNG": "PNG", b"DDS ": "DDS"}
    found = 0
    for e in r.entries:
        if e.is_dir or e.size == 0 or e.size > max_size:
            continue
        data = r.read(e)
        dec = r.decompress(data)
        head = dec[:4]
        for m, name in magics.items():
            if head.startswith(m):
                out = os.path.join(outdir, f"{name}_{e.index:04d}_{e.name_hash:08X}.bin")
                with open(out, "wb") as fh:
                    fh.write(dec)
                print(f"  {out}  ({len(dec)} bytes)  entry={e.index} hash=0x{e.name_hash:08X}")
                found += 1
                break
    print(f"found {found} candidate(s)")


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd, path = sys.argv[1], sys.argv[2]
    if cmd == "list":
        cmd_list(path)
    elif cmd == "scan":
        cmd_scan(path, sys.argv[3] if len(sys.argv) > 3 else "rpf_out",
                 int(sys.argv[4]) if len(sys.argv) > 4 else 8 * 1024 * 1024)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
