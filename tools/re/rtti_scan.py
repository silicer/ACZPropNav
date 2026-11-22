#!/usr/bin/env python3
"""Static MSVC RTTI scan of a PE64 image: recover type names -> Complete Object
Locator -> vftable RVAs, using only the *plaintext* sections (.data/_RDATA),
so it works on the Steam-DRM-wrapped acz.exe without unpacking.

Chain (x64 MSVC):
  vftable[-8] -> _RTTICompleteObjectLocator (COL)
  COL+0x00 signature = 1, COL+0x0C pTypeDescriptor (RVA)
  TypeDescriptor + 0x00 pVFTable, +0x10 mangled name (".?AV...")

Usage: rtti_scan.py <pe> [--grep substring] [--out file]
"""
import struct, sys

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
        chars = struct.unpack_from('<I', buf, o + 36)[0]
        out.append(dict(name=name, vsize=vsize, rva=vaddr, rawsize=rawsize, raw=rawptr, chars=chars))
    return ib, out

def rva2fo(secs, rva):
    for s in secs:
        if s['rva'] <= rva < s['rva'] + max(s['vsize'], s['rawsize']):
            d = rva - s['rva']
            if d < s['rawsize']:
                return s['raw'] + d
    return None

def main():
    path = sys.argv[1]
    grep = None
    if '--grep' in sys.argv:
        grep = sys.argv[sys.argv.index('--grep') + 1]
    outpath = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    buf = open(path, 'rb').read()
    ib, secs = sections(buf)
    print(f'imagebase 0x{ib:X}')
    for s in secs:
        print(f"  {s['name']:10s} rva=0x{s['rva']:08X} vsize=0x{s['vsize']:08X} raw=0x{s['rawsize']:08X} chars=0x{s['chars']:08X}")

    # 1. type descriptors: find ".?AV" / ".?AU" names, struct start is name-0x10
    tds = {}          # name_rva -> type_desc_rva
    names = {}        # type_desc_rva -> mangled name
    i = 0
    while True:
        j = buf.find(b'.?A', i)
        if j < 0:
            break
        i = j + 1
        end = buf.find(b'\0', j)
        if end < 0 or end - j > 300:
            continue
        nm = buf[j:end]
        if not all(0x20 <= c < 0x7f for c in nm):
            continue
        # map file offset -> rva
        for s in secs:
            if s['raw'] <= j < s['raw'] + s['rawsize']:
                name_rva = s['rva'] + (j - s['raw'])
                td_rva = name_rva - 0x10
                names[td_rva] = nm.decode('latin1')
                break

    print(f'\ntype descriptors found: {len(names)}')

    # 2. COLs: signature 1 at COL, pTypeDescriptor (rva) at COL+0x0C
    cols = {}         # col_rva -> td_rva
    for s in secs:
        if s['name'] not in ('.data', '_RDATA', '.rdata'):
            continue
        base, raw, rs = s['rva'], s['raw'], s['rawsize']
        for off in range(0, rs - 24, 4):
            sig, offv, cd = struct.unpack_from('<iii', buf, raw + off)
            if sig != 1 or (offv & 1):
                continue
            td = struct.unpack_from('<i', buf, raw + off + 0x0C)[0]
            if td in names:
                cols[base + off] = td
    print(f'complete object locators found: {len(cols)}')

    # 3. vftables: 8-byte pointer to COL, vftable begins right after it
    col_ptr = {ib + c: c for c in cols}
    found = []
    for s in secs:
        if s['name'] not in ('.data', '_RDATA', '.rdata'):
            continue
        base, raw, rs = s['rva'], s['raw'], s['rawsize']
        for off in range(0, rs - 8, 8):
            v = struct.unpack_from('<Q', buf, raw + off)[0]
            c = col_ptr.get(v)
            if c is not None:
                vt_rva = base + off + 8
                found.append((names[cols[c]], vt_rva, c))
    found.sort()
    print(f'vftables resolved: {len(found)}\n')

    lines = [f'name\tvftable_rva\tcol_rva']
    for nm, vt, c in found:
        lines.append(f'{nm}\t0x{vt:08X}\t0x{c:08X}')
    if grep:
        print(f'--- entries matching {grep!r} ---')
        for nm, vt, c in found:
            if grep.lower() in nm.lower():
                print(f'  {nm:60s} vftable=0x{vt:08X} col=0x{c:08X}')
    if outpath:
        open(outpath, 'w', encoding='utf-8').write('\n'.join(lines))
        print(f'wrote {outpath} ({len(found)} rows)')

if __name__ == '__main__':
    main()
