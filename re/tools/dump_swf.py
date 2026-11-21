#!/usr/bin/env python3
"""Dump Scaleform/SWF blobs out of a running process (read-only).

No injection, no hooks: OpenProcess(PROCESS_VM_READ), walk committed regions,
look for FWS/CWS/GFX signatures, write candidates to a folder, and print a
summary. The menu movie can then be analysed offline.

Usage:
    python dump_swf.py --process RDR --out _tools/swf
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import hashlib
import os
import struct
import sys

PROCESS_VM_READ = 0x0010
PROCESS_QUERY_INFORMATION = 0x0400
MEM_COMMIT = 0x1000
READABLE = {0x02, 0x04, 0x20, 0x40, 0x80}  # RO, RW, EXEC_RW, EXEC_RO, EXEC_WRITECOPY
PAGE_GUARD = 0x100

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, wt.LPCVOID, wt.LPVOID, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.argtypes = [wt.HANDLE, wt.LPCVOID, ctypes.c_void_p, ctypes.c_size_t]


class MBI(ctypes.Structure):
    _fields_ = [
        ("BaseAddress", ctypes.c_void_p),
        ("AllocationBase", ctypes.c_void_p),
        ("AllocationProtect", wt.DWORD),
        ("RegionSize", ctypes.c_size_t),
        ("State", wt.DWORD),
        ("Protect", wt.DWORD),
        ("Type", wt.DWORD),
    ]


def read_chunk(h, addr, size):
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t(0)
    if k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, size, ctypes.byref(got)):
        return buf.raw[: got.value]
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--process", default="RDR")
    ap.add_argument("--out", default="_tools/swf")
    ap.add_argument("--min-size", type=int, default=4096)
    args = ap.parse_args()

    # find pid
    pid = None
    import subprocess
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {args.process}.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if len(parts) >= 2 and parts[0].lower() == f"{args.process}.exe".lower():
            pid = int(parts[1])
            break
    if not pid:
        print(f"process {args.process} not running")
        return 1
    print(f"pid {pid}")

    h = k32.OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        print("OpenProcess failed", ctypes.get_last_error())
        return 1

    os.makedirs(args.out, exist_ok=True)
    addr = 0
    dumped = 0
    seen_hashes = set()
    scanned = 0
    mbi = MBI()
    while k32.VirtualQueryEx(h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) == ctypes.sizeof(mbi):
        base = mbi.BaseAddress or 0
        size = mbi.RegionSize
        if mbi.State == MEM_COMMIT and (mbi.Protect & 0xFF) in READABLE and not (mbi.Protect & PAGE_GUARD):
            off = 0
            while off < size:
                want = min(1 << 20, size - off)
                data = read_chunk(h, base + off, want)
                if data:
                    scanned += len(data)
                    for sig, kind in ((b"FWS", "FWS"), (b"CWS", "CWS"), (b"GFX", "GFX"), (b"GFx", "GFx"), (b"CFX", "CFX"), (b"CFx", "CFx")):
                        i = data.find(sig)
                        while i != -1:
                            blob = data[i:]
                            length = None
                            if kind in ("FWS", "CWS") and len(blob) >= 8:
                                length = struct.unpack_from("<I", blob, 4)[0]
                            if length and args.min_size <= length <= 64 * 1024 * 1024 and len(blob) >= min(length, 64):
                                blob = blob[:length]
                            if len(blob) >= args.min_size:
                                digest = hashlib.sha256(blob[:4096]).hexdigest()[:16]
                                if digest not in seen_hashes:
                                    seen_hashes.add(digest)
                                    path = os.path.join(args.out, f"{kind}_{base + off + i:016X}_{len(blob)}.bin")
                                    with open(path, "wb") as fh:
                                        fh.write(blob)
                                    print(f"  {path}  ({len(blob)} bytes)")
                                    dumped += 1
                            i = data.find(sig, i + 1)
                off += want
        nxt = base + size
        if nxt <= addr:
            break
        addr = nxt

    k32.CloseHandle(h)
    print(f"scanned {scanned/1024/1024:.0f} MB, dumped {dumped} blob(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

