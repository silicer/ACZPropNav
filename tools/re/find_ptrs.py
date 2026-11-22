#!/usr/bin/env python3
"""Find 8-byte little-endian pointer occurrences of a VA in a PE image, report RVA.

Usage: find_ptrs.py <pe> <va-hex> [va-hex ...]
Also supports --scan-rva <start> <len> to hexdump a range.
Section map is parsed from the PE headers, so no external deps.
"""
import struct, sys

def sections(buf):
    e_lfanew = struct.unpack_from('<I', buf, 0x3C)[0]
    assert buf[e_lfanew:e_lfanew+4] == b'PE\0\0'
    coff = e_lfanew + 4
    nsec = struct.unpack_from('<H', buf, coff + 2)[0]
    optsize = struct.unpack_from('<H', buf, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', buf, opt)[0]
    imagebase = struct.unpack_from('<Q', buf, opt + 24)[0] if magic == 0x20b else struct.unpack_from('<I', buf, opt + 28)[0]
    sec = opt + optsize
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = buf[o:o+8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, o + 8)
        out.append((name, vaddr, vsize, rawptr, rawsize))
    return imagebase, out

def main():
    buf = open(sys.argv[1], 'rb').read()
    imagebase, secs = sections(buf)
    print(f'imagebase 0x{imagebase:X}')
    for name, va, vs, rp, rs in secs:
        print(f'  {name:10s} rva=0x{va:08X} vsize=0x{vs:X} raw=0x{rp:X} rawsize=0x{rs:X}')
    for arg in sys.argv[2:]:
        if arg.startswith('0x'):
            va = int(arg, 16)
            pat = struct.pack('<Q', va)
            print(f'\n=== pointers to VA 0x{va:X} ===')
            start = 0
            found = 0
            while True:
                i = buf.find(pat, start)
                if i < 0:
                    break
                found += 1
                # map file offset to RVA
                rv = None
                for name, sva, vs, rp, rs in secs:
                    if rp <= i < rp + rs:
                        rv = sva + (i - rp)
                        secname = name
                        break
                print(f'  file 0x{i:08X}  rva 0x{rv:08X}  rva_of_ptr_slot=0x{imagebase+rv:X}  [{secname}]')
                start = i + 1
            if not found:
                print('  (none)')

if __name__ == '__main__':
    main()
