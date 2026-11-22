#!/usr/bin/env python3
"""PE section report: per-section Shannon entropy, zero-fill test, and a
byte-level diff of two PE files mapped by section name.

Usage:
  pe_report.py <pe> [<pe2>]
For each section: name, RVA, vsize, rawsize, entropy, nonzero byte count.
With a second PE: per-section differing-byte count / first differing offset.
"""
import math, struct, sys

def parse(buf):
    e = struct.unpack_from('<I', buf, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from('<H', buf, coff + 2)[0]
    optsize = struct.unpack_from('<H', buf, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', buf, opt)[0]
    ib = struct.unpack_from('<Q', buf, opt + 24)[0] if magic == 0x20b else struct.unpack_from('<I', buf, opt + 28)[0]
    ep = struct.unpack_from('<I', buf, opt + 16)[0]
    sec = opt + optsize
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = buf[o:o+8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, o + 8)
        chars = struct.unpack_from('<I', buf, o + 36)[0]
        out.append(dict(name=name, vsize=vsize, rva=vaddr, rawsize=rawsize, raw=rawptr, chars=chars))
    return ib, ep, out

def entropy(b):
    if not b:
        return 0.0
    cnt = [0] * 256
    for x in b:
        cnt[x] += 1
    n = len(b)
    return -sum((c / n) * math.log2(c / n) for c in cnt if c)

def main():
    f1 = sys.argv[1]
    b1 = open(f1, 'rb').read()
    ib1, ep1, s1 = parse(b1)
    print(f'== {f1}  size={len(b1)}  imagebase=0x{ib1:X}  entry=0x{ep1:X}')
    for s in s1:
        d = b1[s['raw']:s['raw'] + s['rawsize']]
        nz = sum(1 for x in d if x)
        print(f"  {s['name']:10s} rva=0x{s['rva']:08X} vsize=0x{s['vsize']:08X} raw=0x{s['rawsize']:08X} "
              f"entropy={entropy(d):5.3f} nonzero={nz}/{len(d)}")
    if len(sys.argv) > 2:
        f2 = sys.argv[2]
        b2 = open(f2, 'rb').read()
        ib2, ep2, s2 = parse(b2)
        print(f'\n== {f2}  size={len(b2)}  entry=0x{ep2:X}  (delta {len(b2)-len(b1):+d} bytes)')
        m2 = {s['name']: s for s in s2}
        for s in s1:
            o = m2.get(s['name'])
            n = min(s['rawsize'], o['rawsize']) if o else 0
            if not o:
                print(f"  {s['name']:10s} only in file1")
                continue
            a = b1[s['raw']:s['raw'] + n]
            b = b2[o['raw']:o['raw'] + n]
            diffs = [i for i in range(n) if a[i] != b[i]]
            first = f'first@+0x{diffs[0]:X}' if diffs else '-'
            same_rva = 'same-rva' if s['rva'] == o['rva'] else f"rva {s['rva']:#x}->{o['rva']:#x}"
            print(f"  {s['name']:10s} raw {s['rawsize']:#x}->{o['rawsize']:#x} {same_rva} "
                  f"diffbytes={len(diffs)} {first}")

if __name__ == '__main__':
    main()
