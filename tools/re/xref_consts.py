#!/usr/bin/env python3
"""Find code references to float constants in a PE64 image, and attribute them
to functions using .pdata.

Technique: a RIP-relative instruction ends with a 32-bit displacement at some
offset p; next_ip = p+4. So for every byte position p in an executable section,
target_rva = (p_in_rva + 4) + int32(buf[p:p+4]).  We check that against a table
of interesting float RVA locations.  This needs no instruction decoder and
false positives are statistically nil.

Usage:
  xref_consts.py <pe> [--consts 0.0174532925,57.2957795,...] [--out file]
Prints, per constant, the referencing instruction RVAs grouped by function.
"""
import struct, sys, math

CONST_LIB = [0.0174532925, 57.2957795, 6.2831855, 3.14159265, 180.0, 360.0,
             2.0, 0.5, 1.0, 9.0, 40.0, 45.0, 55.0, 60.0, 65.0, 75.0, 90.0,
             120.0, 150.0, 2000.0, 4000.0, 10000.0, 0.1, 0.01]

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

def pdata_funcs(buf, secs):
    pd = next((s for s in secs if s['name'] == '.pdata'), None)
    if not pd:
        return []
    funcs = []
    for i in range(0, pd['vsize'], 12):
        b, e, u = struct.unpack_from('<III', buf, pd['raw'] + i)
        if 0 < b < e < 0x10000000:
            funcs.append((b, e))
    funcs.sort()
    return funcs

def find_const_rvas(buf, secs, vals):
    """float value -> [rva] where that f32 appears in a read-only/data section"""
    want = {}
    for v in vals:
        want[struct.pack('<f', v)] = v
    found = {}
    for s in secs:
        if s['name'] not in ('.rdata', '.data', '_RDATA'):
            continue
        d = buf[s['raw']:s['raw'] + s['rawsize']]
        for pat, v in want.items():
            start = 0
            while True:
                j = d.find(pat, start)
                if j < 0:
                    break
                found.setdefault(v, []).append(s['rva'] + j)
                start = j + 1
    return found

def main():
    path = sys.argv[1]
    vals = CONST_LIB
    if '--consts' in sys.argv:
        vals = [float(x) for x in sys.argv[sys.argv.index('--consts') + 1].split(',')]
    outpath = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    buf = open(path, 'rb').read()
    ib, secs = sections(buf)
    text = next(s for s in secs if s['name'] == '.text')
    funcs = pdata_funcs(buf, secs)
    consts = find_const_rvas(buf, secs, vals)
    print(f'imagebase 0x{ib:X}  .text rva=0x{text["rva"]:X} size=0x{text["vsize"]:X}  functions(.pdata)={len(funcs)}')
    print(f'constants located: ' + ', '.join(f'{v:g}@0x{consts[v][0]:X}' for v in sorted(consts)))

    # invert: rva -> value
    rva2val = {}
    for v, rvas in consts.items():
        for r in rvas:
            rva2val[r] = v

    tbase = text['rva']
    d = buf[text['raw']:text['raw'] + text['rawsize']]
    hits = {}   # value -> [(insn_rva, func_begin)]
    import bisect
    begins = [f[0] for f in funcs]
    for p in range(0, len(d) - 4):
        disp = struct.unpack_from('<i', d, p)[0]
        insn_rva = tbase + p
        tgt = insn_rva + 4 + disp
        if tgt in rva2val:
            v = rva2val[tgt]
            i = bisect.bisect_right(begins, insn_rva) - 1
            fb = funcs[i][0] if i >= 0 and funcs[i][0] <= insn_rva < funcs[i][1] else None
            hits.setdefault(v, []).append((insn_rva, fb))

    lines = []
    for v in sorted(hits, key=lambda x: -len(hits[x])):
        lines.append(f'=== {v:g}  ({len(hits[v])} refs)  const_rva=0x{consts[v][0]:X}')
        byfunc = {}
        for insn, fb in hits[v]:
            byfunc.setdefault(fb, []).append(insn)
        for fb, insns in sorted(byfunc.items(), key=lambda kv: -len(kv[1])):
            fs = f'func+0x{fb:X}' if fb is not None else 'no-pdata'
            lines.append(f'   {fs:14s} {len(insns):3d} refs: ' + ', '.join(f'0x{i:X}' for i in sorted(insns)[:8]))
    txt = '\n'.join(lines)
    print(txt)
    if outpath:
        open(outpath, 'w', encoding='utf-8').write(txt + '\n')

if __name__ == '__main__':
    main()
