#!/usr/bin/env python3
"""Resolve a PE's IAT: map IAT slot RVA -> (dll, symbol).

Usage: iat.py <pe> [--dll NAME] [--map out.txt]
The output lets you annotate `callq *0x...(%rip)` indirections in llvm-objdump
output (the slot address objdump prints is exactly the IAT slot RVA + imagebase).
"""
import struct, sys

def parse(buf):
    e = struct.unpack_from('<I', buf, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from('<H', buf, coff + 2)[0]
    optsize = struct.unpack_from('<H', buf, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', buf, opt)[0]
    ib = struct.unpack_from('<Q', buf, opt + 24)[0] if magic == 0x20b else struct.unpack_from('<I', buf, opt + 28)[0]
    ddoff = opt + (112 if magic == 0x20b else 96)
    imp_rva, imp_size = struct.unpack_from('<II', buf, ddoff + 8)   # dir 1 = import
    sec = opt + optsize
    secs = []
    for i in range(nsec):
        o = sec + i * 40
        name = buf[o:o+8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, o + 8)
        secs.append((name, vaddr, vsize, rawptr, rawsize))
    return ib, secs, imp_rva

def rva2fo(secs, rva):
    for name, va, vs, rp, rs in secs:
        if va <= rva < va + max(vs, rs):
            return rp + (rva - va)
    raise ValueError(f'rva 0x{rva:X} not in any section')

def cstr(buf, fo):
    end = buf.find(b'\0', fo)
    return buf[fo:end].decode('latin1')

def main():
    buf = open(sys.argv[1], 'rb').read()
    ib, secs, imp_rva = parse(buf)
    outp = None
    if '--map' in sys.argv:
        outp = open(sys.argv[sys.argv.index('--map') + 1], 'w', encoding='utf-8')
    fo = rva2fo(secs, imp_rva)
    rows = []
    while True:
        oft, ts, fc, name_rva, ft = struct.unpack_from('<IIIII', buf, fo)
        if name_rva == 0:
            break
        dll = cstr(buf, rva2fo(secs, name_rva))
        ilt = oft or ft
        i = 0
        while True:
            ent = struct.unpack_from('<Q', buf, rva2fo(secs, ilt + i * 8))[0]
            if ent == 0:
                break
            slot = ft + i * 8
            if ent & (1 << 63):
                sym = f'#{ent & 0xFFFF}'
            else:
                sym = cstr(buf, rva2fo(secs, (ent & 0x7FFFFFFF) + 2))
            rows.append((slot, dll, sym))
            i += 1
        fo += 20
    for slot, dll, sym in sorted(rows):
        line = f'0x{slot:08X}  {dll:20s} {sym}'
        print(line)
        if outp:
            outp.write(line + '\n')

if __name__ == '__main__':
    main()
