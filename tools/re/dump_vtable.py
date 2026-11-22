#!/usr/bin/env python3
"""Dump virtual-function tables of an *unpacked* PE64 image, using the RTTI map
produced by rtti_scan.py.

For each vftable: read qwords starting at vftable_rva; stop when a pointer no
longer lands in the executable section. For each slot print the function RVA and
its size from .pdata (so you can tell real methods from thunks).

Usage:
  dump_vtable.py <unpacked-pe> <rtti.tsv> [--grep REGEX] [--max 24]
"""
import re, struct, sys

def sections(buf):
    e = struct.unpack_from('<I', buf, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from('<H', buf, coff + 2)[0]
    optsize = struct.unpack_from('<H', buf, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', buf, opt)[0]
    ib = struct.unpack_from('<Q', buf, opt + 24)[0] if magic == 0x20b else struct.unpack_from('<I', buf, opt + 28)[0]
    sec = opt + optsize
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = buf[o:o+8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, o + 8)
        out.append(dict(name=name, vsize=vsize, rva=vaddr, rawsize=rawsize, raw=rawptr))
    return ib, out

def main():
    pe, tsv = sys.argv[1], sys.argv[2]
    grep = sys.argv[sys.argv.index('--grep') + 1] if '--grep' in sys.argv else None
    maxslot = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 20
    buf = open(pe, 'rb').read()
    ib, secs = sections(buf)
    text = next(s for s in secs if s['name'] == '.text')
    pd = next((s for s in secs if s['name'] == '.pdata'), None)
    funcs = {}
    if pd:
        for i in range(0, pd['vsize'], 12):
            b, e, u = struct.unpack_from('<III', buf, pd['raw'] + i)
            if 0 < b < e < 0x10000000:
                funcs[b] = e - b

    def rva2fo(rva):
        for s in secs:
            if s['rva'] <= rva < s['rva'] + s['rawsize']:
                return s['raw'] + (rva - s['rva'])
        return None

    rows = []
    for line in open(tsv, encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) < 3 or p[0] == 'name':
            continue
        rows.append((p[0], int(p[1], 16), int(p[2], 16)))

    if grep:
        rows = [r for r in rows if re.search(grep, r[0], re.I)]
    print(f'{len(rows)} vftables selected\n')
    for name, vt, col in rows:
        fo = rva2fo(vt)
        if fo is None:
            print(f'{name}: vftable 0x{vt:X} outside file sections')
            continue
        print(f'== {name}   vftable=0x{vt:08X}')
        for slot in range(maxslot):
            q = struct.unpack_from('<Q', buf, fo + slot * 8)[0]
            if q < ib:
                print(f'   [{slot:2d}] 0x{q:016X}  (not a pointer, end)')
                break
            frva = q - ib
            if not (text['rva'] <= frva < text['rva'] + text['vsize']):
                print(f'   [{slot:2d}] rva=0x{frva:06X}  (outside .text, end)')
                break
            sz = funcs.get(frva)
            szs = f'size=0x{sz:X}' if sz else ('not a function start' if funcs else '')
            print(f'   [{slot:2d}] rva=0x{frva:06X}  {szs}')

if __name__ == '__main__':
    main()
