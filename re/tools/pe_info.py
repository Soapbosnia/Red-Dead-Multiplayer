#!/usr/bin/env python3
"""Static PE reconnaissance for an RDR1 PC build.

Read-only: parses headers, sections, imports and string metadata. It never
loads, executes or patches the target.

Usage:
    python pe_info.py "D:\\Games\\Red Dead Redemption\\RDR.exe" [--json out.json]
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys

DIR_IMPORT = 1
DIR_EXCEPTION = 3

RTTI_NEEDLE = b".?AV"

# Symbols an RDR1 multiplayer mod eventually needs to locate. Presence in the
# binary only tells us names leaked into strings - it is not proof of an
# exported API, but it is a strong starting point for x64dbg work.
PROBES = [
    b"CPed", b"CPlayer", b"CPlayerInfo", b"CVehicle", b"CWorld", b"CStreaming",
    b"CTheScripts", b"scrThread", b"CScenario", b"CTask", b"CModelInfo",
    b"rage::", b"fwEntity", b"fwArchetype", b"CNetwork", b"CNetGame",
    b"CNetworkPlayerMgr", b"CTimer", b"CGame", b"CPad", b"CControl",
    b"CAMERA", b"CCamera", b"CPhysics", b"CCloth", b"CHorse", b"CNavMesh",
    b"CWeapon", b"CInventory", b"CPedVariation", b"CDrawable",
]


def u16(b: bytes, o: int) -> int:
    return struct.unpack_from("<H", b, o)[0]


def u32(b: bytes, o: int) -> int:
    return struct.unpack_from("<I", b, o)[0]


def u64(b: bytes, o: int) -> int:
    return struct.unpack_from("<Q", b, o)[0]


class PE:
    def __init__(self, data: bytes) -> None:
        self.data = data
        if data[:2] != b"MZ":
            raise ValueError("not an MZ image")
        self.pe_off = u32(data, 0x3C)
        if data[self.pe_off:self.pe_off + 4] != b"PE\0\0":
            raise ValueError("not a PE image")
        fh = self.pe_off + 4
        self.machine = u16(data, fh)
        self.num_sections = u16(data, fh + 2)
        self.timestamp = u32(data, fh + 4)
        self.opt_size = u16(data, fh + 16)
        self.opt = fh + 20
        self.magic = u16(data, self.opt)
        if self.magic == 0x20B:
            self.bits = 64
            self.image_base = u64(data, self.opt + 24)
            self.dir_off = self.opt + 112
        elif self.magic == 0x10B:
            self.bits = 32
            self.image_base = u32(data, self.opt + 28)
            self.dir_off = self.opt + 96
        else:
            raise ValueError("unknown optional header magic")
        self.entry_rva = u32(data, self.opt + 16)
        self.size_of_image = u32(data, self.opt + 56)
        self.checksum = u32(data, self.opt + 64)
        self.subsystem = u16(data, self.opt + 68)
        self.num_dirs = u32(data, self.opt + 108) if self.bits == 64 else u32(data, self.opt + 92)

        self.sections = []
        sec = self.opt + self.opt_size
        for i in range(self.num_sections):
            o = sec + i * 40
            name = data[o:o + 8].rstrip(b"\0").decode("latin-1")
            self.sections.append({
                "name": name,
                "vsize": u32(data, o + 8),
                "vaddr": u32(data, o + 12),
                "rawsize": u32(data, o + 16),
                "rawptr": u32(data, o + 20),
                "characteristics": u32(data, o + 36),
            })

    def rva_to_off(self, rva: int) -> int | None:
        for s in self.sections:
            start = s["vaddr"]
            span = max(s["vsize"], s["rawsize"])
            if start <= rva < start + span:
                delta = rva - start
                if delta < s["rawsize"]:
                    return s["rawptr"] + delta
                return None
        return None

    def dir(self, idx: int) -> tuple[int, int]:
        if idx >= self.num_dirs:
            return (0, 0)
        return (u32(self.data, self.dir_off + idx * 8),
                u32(self.data, self.dir_off + idx * 8 + 4))

    def imports(self) -> list[str]:
        rva, _ = self.dir(DIR_IMPORT)
        out: list[str] = []
        if not rva:
            return out
        off = self.rva_to_off(rva)
        if off is None:
            return out
        while True:
            name_rva = u32(self.data, off + 12)
            if name_rva == 0:
                break
            noff = self.rva_to_off(name_rva)
            if noff is None:
                break
            end = self.data.find(b"\0", noff)
            out.append(self.data[noff:end].decode("latin-1"))
            off += 20
            if len(out) > 512:
                break
        return out


def ascii_strings(data: bytes, min_len: int = 5):
    """Yield (offset, text) for printable ASCII runs."""
    start = None
    for i, c in enumerate(data):
        printable = 32 <= c <= 126 or c == 9
        if printable:
            if start is None:
                start = i
        else:
            if start is not None and i - start >= min_len:
                yield start, data[start:i].decode("latin-1")
            start = None
    if start is not None and len(data) - start >= min_len:
        yield start, data[start:].decode("latin-1")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("exe")
    ap.add_argument("--json", dest="json_out")
    ap.add_argument("--grep", dest="grep",
                    help="comma-separated substrings; prints matching ASCII strings")
    args = ap.parse_args()

    data = open(args.exe, "rb").read()
    pe = PE(data)
    sha = hashlib.sha256(data).hexdigest().upper()

    print(f"file        : {os.path.abspath(args.exe)}")
    print(f"size        : {len(data):,} bytes")
    print(f"sha256      : {sha}")
    print(f"machine     : 0x{pe.machine:04X} ({'x64' if pe.machine == 0x8664 else 'x86' if pe.machine == 0x14C else '?'})")
    print(f"image base  : 0x{pe.image_base:X}")
    print(f"entry RVA   : 0x{pe.entry_rva:X}")
    print(f"size of img : 0x{pe.size_of_image:X} ({pe.size_of_image / (1024 ** 3):.2f} GiB)")
    print(f"timestamp   : 0x{pe.timestamp:08X}")
    print()

    print("sections:")
    prev_end = None
    for s in pe.sections:
        vstart = pe.image_base + s["vaddr"]
        vend = vstart + max(s["vsize"], 1)
        gap = ""
        if prev_end is not None and vstart > prev_end:
            gap = f"  <-- page-align slack {(vstart - prev_end):,} bytes"
        print(f"  {s['name']:<10} rva=0x{s['vaddr']:08X} vsize=0x{s['vsize']:08X} "
              f"raw=0x{s['rawptr']:08X}+0x{s['rawsize']:08X}{gap}")
        prev_end = vend
    print()

    imports = pe.imports()
    print(f"imports ({len(imports)}):")
    for dll in imports:
        print(f"  {dll}")
    print()

    # RTTI type descriptors, if the build kept /GR.
    rtti = []
    idx = data.find(RTTI_NEEDLE)
    while idx != -1 and len(rtti) < 400:
        end = data.find(b"@@", idx + 4)
        if end != -1 and end - idx < 160:
            name = data[idx:end + 2].decode("latin-1")
            if all(32 <= ord(ch) < 127 for ch in name):
                rtti.append(name)
        idx = data.find(RTTI_NEEDLE, idx + 1)
    print(f"MSVC RTTI type descriptors: {len(rtti)}")
    for name in rtti[:40]:
        print(f"  {name}")
    if len(rtti) > 40:
        print(f"  ... {len(rtti) - 40} more")
    print()

    text = data.decode("latin-1", errors="ignore")
    print("symbol probes (byte occurrences):")
    probe_counts = {}
    for needle in PROBES:
        n = text.count(needle.decode("latin-1"))
        probe_counts[needle.decode("latin-1")] = n
        if n:
            print(f"  {needle.decode('latin-1'):<22} {n}")
    print()

    total_strings = 0
    long_strings = []
    for _, s in ascii_strings(data, 6):
        total_strings += 1
        if len(s) >= 40 and len(long_strings) < 25:
            long_strings.append(s)
    if args.grep:
        needles = [n.strip().lower() for n in args.grep.split(",") if n.strip()]
        print(f"\nstring matches for {needles}:")
        hits = {n: [] for n in needles}
        for off, s in ascii_strings(data, 5):
            low = s.lower()
            for n in needles:
                if n in low and len(hits[n]) < 15:
                    hits[n].append((off, s))
        for n in needles:
            print(f"  [{n}] {len(hits[n])} shown")
            for off, s in hits[n]:
                print(f"    0x{off:08X}  {s[:120]}")
        print()

    print(f"printable strings (>=6 chars): {total_strings}")
    print("sample long strings:")
    for s in long_strings:
        print(f"  {s[:110]}")

    if args.json_out:
        payload = {
            "file": os.path.abspath(args.exe),
            "size": len(data),
            "sha256": sha,
            "machine": pe.machine,
            "bits": pe.bits,
            "image_base": pe.image_base,
            "entry_rva": pe.entry_rva,
            "size_of_image": pe.size_of_image,
            "sections": pe.sections,
            "imports": imports,
            "rtti": rtti,
            "probes": probe_counts,
            "string_count": total_strings,
        }
        with open(args.json_out, "w", encoding="utf-8") as fh:
            json.dump(payload, fh, indent=2)
        print(f"\njson written: {args.json_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
