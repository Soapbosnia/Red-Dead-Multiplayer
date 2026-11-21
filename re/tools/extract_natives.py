#!/usr/bin/env python3
"""Dump every script native RDR.exe registers: hash -> handler address (+ name/signature from a native DB).

The game registers each native once at boot with
    lea  r8, [rip+handler]     ; 4C 8D 05 rel32
    mov  edx, <hash>           ; BA imm32          (joaat of the lowercase native name)
    call RegisterNative        ; E8 rel32          (0x1401E7140 on build 0x673783F3)
so a byte scan of .text recovers the whole table; the register call is the most common target of that pattern.

Usage:
    python extract_natives.py <RDR.exe> [--db Natives.h] [--enums Enums.h] [--csv out.csv] [--icons blip_icons.h]

--db / --enums: K3rhos' RDR-PC-Natives-DB (MIT, https://github.com/K3rhos/RDR-PC-Natives-DB) for names,
signatures and the BlipIcon enum. Without --db only hashes and addresses are written.
No third-party Python modules needed.
"""
import argparse
import collections
import csv
import re
import struct
import sys


def joaat(s: str) -> int:
    h = 0
    for c in s.lower().encode():
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    h = (h + (h << 15)) & 0xFFFFFFFF
    return h


def sections(data: bytes):
    e = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, e + 6)[0]
    osz = struct.unpack_from('<H', data, e + 20)[0]
    base = struct.unpack_from('<Q', data, e + 24 + 24)[0]
    out = {}
    o = e + 24 + osz
    for _ in range(nsec):
        name = data[o:o + 8].rstrip(b'\0').decode(errors='replace')
        vsize, va, rsize, raw = struct.unpack_from('<IIII', data, o + 8)
        out[name] = (base + va, vsize, raw, rsize)
        o += 40
    return base, out


def scan(exe: str):
    data = open(exe, 'rb').read()
    timestamp = struct.unpack_from('<I', data, struct.unpack_from('<I', data, 0x3C)[0] + 8)[0]
    _, secs = sections(data)
    tva, _, traw, trsize = secs['.text']
    text = data[traw:traw + trsize]
    hits = []
    for m in re.finditer(rb'\x4C\x8D\x05(.{4})\xBA(.{4})\xE8(.{4})', text, re.S):
        o = m.start()
        handler = tva + o + 7 + struct.unpack('<i', m.group(1))[0]
        h = struct.unpack('<I', m.group(2))[0]
        target = tva + o + 17 + struct.unpack('<i', m.group(3))[0]
        hits.append((h, handler, target, tva + o))
    register, count = collections.Counter(t for _, _, t, _ in hits).most_common(1)[0]
    table = {h: (fn, site) for h, fn, t, site in hits if t == register}
    # A few registrations have other code between the lea and the mov: every `mov edx, imm32 ; call register`
    # with the nearest preceding `lea r8, [rip+x]`.
    for m in re.finditer(rb'\xBA(.{4})\xE8(.{4})', text, re.S):
        o = m.start()
        if tva + o + 10 + struct.unpack('<i', m.group(2))[0] != register:
            continue
        h = struct.unpack('<I', m.group(1))[0]
        if h in table:
            continue
        for back in range(7, 48):
            q = o - back
            if text[q:q + 3] == b'\x4C\x8D\x05':
                table[h] = (tva + q + 7 + struct.unpack('<i', text[q + 3:q + 7])[0], tva + q)
                break
    return timestamp, register, table


def load_db(path):
    src = open(path, encoding='latin-1').read()
    db = {}
    for m in re.finditer(r'static ([^\n]*?) (\w+)\(([^\n]*?)\)\s*\{[^\n]*?Invoke<0x([0-9A-Fa-f]+)', src):
        db[int(m.group(4), 16)] = (m.group(2), f'{m.group(1)} {m.group(2)}({m.group(3)})')
    return db


def load_icons(path):
    src = open(path, encoding='latin-1').read()
    m = re.search(r'enum BlipIcon\s*:\s*int\s*\{(.*?)\};', src, re.S)
    body = re.sub(r'//[^\n]*', '', m.group(1))
    val, out = -1, []
    for tok in (t.strip() for t in body.split(',')):
        if not tok:
            continue
        if '=' in tok:
            name, v = (x.strip() for x in tok.split('='))
            val = int(v, 0)
        else:
            name, val = tok, val + 1
        out.append((name, val))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('exe')
    ap.add_argument('--db')
    ap.add_argument('--enums')
    ap.add_argument('--csv')
    ap.add_argument('--icons')
    a = ap.parse_args()

    timestamp, register, table = scan(a.exe)
    db = load_db(a.db) if a.db else {}
    named = sum(1 for h in table if h in db)
    print(f'RDR.exe timestamp 0x{timestamp:08X}: {len(table)} natives registered through 0x{register:X}, '
          f'{named} named', file=sys.stderr)

    if a.csv:
        with open(a.csv, 'w', newline='') as f:
            w = csv.writer(f)
            w.writerow(['hash', 'name', 'handler', 'register_site', 'signature'])
            for h in sorted(table, key=lambda x: (db.get(x, ('~',))[0], x)):
                fn, site = table[h]
                name, sig = db.get(h, ('', ''))
                w.writerow([f'0x{h:08X}', name, f'0x{fn:X}', f'0x{site:X}', sig])

    if a.icons and a.enums:
        icons = load_icons(a.enums)
        lines = ['// Generated by rdrmp_re/tools/extract_natives.py from RDR-PC-Natives-DB Enums.h (MIT).']
        lines += [f'{{"{n}", {v}}},' for n, v in icons]
        open(a.icons, 'w').write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
