#!/usr/bin/env python3
"""Find functions that write SSE/scalar stores into a small structure at the
same displacement set — used to locate the *plain missile* kinematics/guidance
writer (pos @ +0x34, dir/vel @ +0x40..+0x48, life @ +0x4C).

Heuristic only: decodes modrm of
    C5 xx 11 modrm [disp]        (VEX vmovss/vmovups/vmovaps/vmovsd store)
    F3 0F 11 modrm [disp]        (legacy movss store)
    0F 11  modrm [disp]          (movups store)
and records the displacement of every store whose base register is not rsp/rbp.

Usage:
  find_node_writers.py --want 0x34 --also 0x30,0x38,0x3C,0x40,0x44,0x48,0x4C
  find_node_writers.py --list            # histogram of all store displacements
  find_node_writers.py --func 0x9F230    # detail one function
"""
import os, struct, sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PE = os.path.join(ROOT, 'work', 'acz_copy.exe.unpacked.exe')


def sections(buf):
    e = struct.unpack_from('<I', buf, 0x3C)[0]
    coff = e + 4
    nsec = struct.unpack_from('<H', buf, coff + 2)[0]
    optsize = struct.unpack_from('<H', buf, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from('<H', buf, opt)[0]
    base = struct.unpack_from('<Q', buf, opt + 24)[0] if magic == 0x20b else struct.unpack_from('<I', buf, opt + 28)[0]
    sec = opt + optsize
    out = []
    for i in range(nsec):
        o = sec + i * 40
        name = buf[o:o + 8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, o + 8)
        out.append(dict(name=name, vsize=vsize, rva=vaddr, rawsize=rawsize, raw=rawptr, base=base))
    return base, out


def build_funcs(buf, secs):
    pd = next((s for s in secs if s['name'] == '.pdata'), None)
    funcs = []
    if pd:
        for i in range(0, pd['vsize'], 12):
            b, e, u = struct.unpack_from('<III', buf, pd['raw'] + i)
            if 0 < b < e < 0x10000000:
                funcs.append((b, e))
    funcs.sort()
    return funcs


def containing(funcs, rva):
    lo, hi = 0, len(funcs) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        b, e = funcs[mid]
        if rva < b:
            hi = mid - 1
        elif rva >= e:
            lo = mid + 1
        else:
            return b, e
    return None, None


def parse_modrm(buf, i, end):
    """i points at modrm. Returns (base_reg, disp, length) or None."""
    if i >= end:
        return None
    m = buf[i]
    mod, reg, rm = (m >> 6) & 3, (m >> 3) & 7, m & 7
    if mod == 3:
        return None
    ln = 1
    disp = 0
    if rm == 4:  # SIB
        if i + ln >= end:
            return None
        sib = buf[i + ln]
        ln += 1
        base = sib & 7
        if base == 5 and mod == 0:
            base = -1
    elif rm == 5 and mod == 0:
        base = -1
    else:
        base = rm
    if mod == 1:
        if i + ln >= end:
            return None
        disp = struct.unpack_from('<b', buf, i + ln)[0]
        ln += 2
    elif mod == 2:
        if i + ln + 4 > end:
            return None
        disp = struct.unpack_from('<i', buf, i + ln)[0]
        ln += 4
    return base, disp, ln


def collect(buf, secs, funcs):
    """Return list of (func_start, insn_rva, disp, modrm_reg)."""
    text = next(s for s in secs if s['name'] == '.text')
    tb, trva = text['raw'], text['rva']
    end = tb + min(text['rawsize'], text['vsize'])
    hits = []
    i = tb
    while i < end - 8:
        b = buf[i]
        disp_pos = None
        if b == 0xC5 and i + 2 < end and buf[i + 2] == 0x11:
            disp_pos = i + 3
        elif b == 0xF3 and buf[i + 1] == 0x0F and buf[i + 2] == 0x11:
            disp_pos = i + 3
        elif b == 0x0F and buf[i + 1] == 0x11:
            disp_pos = i + 2
        if disp_pos is not None:
            r = parse_modrm(buf, disp_pos, end)
            if r:
                base, disp, ln = r
                if base not in (4, 5, -1) and disp != 0:
                    f = containing(funcs, trva + (i - tb))
                    hits.append((f[0], trva + (i - tb), disp, (buf[disp_pos] >> 3) & 7))
            i += 1
            continue
        i += 1
    return hits


def main():
    args = sys.argv[1:]
    want = None
    also = set()
    if '--want' in args:
        want = int(args[args.index('--want') + 1], 16)
    if '--also' in args:
        for x in args[args.index('--also') + 1].split(','):
            also.add(int(x, 16))
    buf = open(PE, 'rb').read()
    base, secs = sections(buf)
    funcs = build_funcs(buf, secs)
    hits = collect(buf, secs, funcs)
    print(f'# {len(hits)} scalar/vector stores with displacement found')

    if '--list' in args:
        c = Counter(h[2] for h in hits if h[2] > 0)
        for d, n in sorted(c.items(), key=lambda kv: -kv[1])[:40]:
            print(f'   disp +0x{d:02X}: {n}')
        return

    if '--func' in args:
        f0 = int(args[args.index('--func') + 1], 16)
        f = containing(funcs, f0)
        print(f'func 0x{f0:X} range 0x{f[0]:X}..0x{f[1]:X} size 0x{f[1] - f[0]:X}')
        for fstart, rva, disp, reg in hits:
            if fstart == f[0]:
                print(f'   0x{rva:06X} store disp +0x{disp:02X}')
        return

    if want is None:
        print(__doc__)
        return

    by_func = defaultdict(list)
    for fstart, rva, disp, reg in hits:
        by_func[fstart].append((rva, disp))

    scored = []
    for fstart, lst in by_func.items():
        disps = {d for _, d in lst}
        if want not in disps:
            continue
        extra = len(disps & also) if also else 0
        scored.append((extra, fstart, sorted(disps)))
    scored.sort(key=lambda t: (-t[0], t[1]))
    print(f'# {len(scored)} functions store a float at +0x{want:02X}')
    for extra, fstart, disps in scored[:40]:
        e = dict(funcs).get(fstart)
        print(f'  func 0x{fstart:06X} (size 0x{(e - fstart) if e else 0:X}) extra={extra} disps={[hex(d) for d in disps]}')


if __name__ == '__main__':
    main()
